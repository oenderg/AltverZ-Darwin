# SPDX-License-Identifier: GPL-2.0-or-later
# Start a portable Windows build twice and check that it draws, in CI or by hand:
#
#   pwsh runtime/win/smoke-test.ps1 -Game <unpacked zip folder> -Obb <main...obb> -Out <folder>
#
# The first start is a new player's: no data but the OBB (linked in, so it is not downloaded), so the x64
# build translates the game first. It passes once the game draws frames. The second start must find the
# translation it saved (x64) and draw again. Each start's log and a screenshot of the screen go to -Out.
# Exits 1 when a start shows an error, ends on its own, or draws nothing in time.
param(
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$Obb,
    [Parameter(Mandatory)][string]$Out,
    [int]$FirstMinutes = 40,
    [int]$SecondMinutes = 5
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Smoke -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
[Smoke.Win]::SetProcessDPIAware() | Out-Null

$exe = Get-ChildItem -LiteralPath $Game -Filter 'PvZ2 Reflourished*.exe' | Select-Object -First 1
if (-not $exe) { throw "no PvZ2 Reflourished*.exe in $Game" }
if (-not (Test-Path -LiteralPath (Join-Path $Game 'portable'))) { throw "$Game is not a portable copy (no 'portable' file)" }
$x64 = $exe.Name -like '*x64*'
$data = Join-Path $Game 'data'
$log = Join-Path $data 'pvz2.log'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# The OBB where the game looks for it, as a hard link: the same volume, no 1.3 GB copy
$obbDir = Join-Path $data 'sdcard\Android\obb\com.ea.game.pvz2_rfl'
New-Item -ItemType Directory -Force -Path $obbDir | Out-Null
$obbTo = Join-Path $obbDir (Split-Path $Obb -Leaf)
if (-not (Test-Path -LiteralPath $obbTo)) {
    try { New-Item -ItemType HardLink -Path $obbTo -Target $Obb | Out-Null }
    catch { Copy-Item -LiteralPath $Obb -Destination $obbTo }
}

# The game's window as it draws it (PrintWindow's full content: even under other windows), else the screen
function Save-Screen([string]$path, $p) {
    try {
        $p.Refresh()
        $w = $p.MainWindowHandle
        $r = New-Object Smoke.Win+RECT
        if ($w -ne [IntPtr]::Zero -and [Smoke.Win]::GetWindowRect($w, [ref]$r) -and $r.Right -gt $r.Left) {
            $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $dc = $g.GetHdc()
            $ok = [Smoke.Win]::PrintWindow($w, $dc, 2)
            $g.ReleaseHdc($dc)
            if ($ok) { $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); return }
            $g.Dispose(); $bmp.Dispose()
        }
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
    } catch { Write-Host "smoke: no screenshot: $_" }
}

function Read-Log {
    if (-not (Test-Path -LiteralPath $log)) { return '' }
    # The game keeps the log open for writing: read it shared
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite, Delete')
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
}

# One start: true once a 5 s frame line shows frames drawn, false on an error, an exit or the time limit.
function Start-Game([string]$name, [int]$minutes, [string[]]$must) {
    Write-Host "smoke: $name start of $($exe.Name)"
    # The previous start's log is kept in -Out: without it, its frame lines cannot pass this start
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    $p = Start-Process -FilePath $exe.FullName -WorkingDirectory $Game -PassThru
    $deadline = (Get-Date).AddMinutes($minutes)
    $shown = 0
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        $text = Read-Log
        # The log so far, a line at a time, so a slow start shows where it is
        $lines = $text -split "`r?`n"
        for ($i = $shown; $i -lt $lines.Count - 1; $i++) {
            if ($lines[$i] -cmatch 'preparing|translated|translation|frames=|failed|cannot|could not') { Write-Host "  $($lines[$i])" }
        }
        $shown = [Math]::Max($shown, $lines.Count - 1)
        if ($text -match 'the translation failed|could not start') { Write-Host "smoke: $name start: the log shows an error"; break }
        $drawn = [regex]::Matches($text, 'windows: frames=(\d+), last 5s=([\d.]+) fps') |
            Where-Object { [int64]$_.Groups[1].Value -gt 0 -and [double]$_.Groups[2].Value -gt 0 }
        if ($drawn) { Start-Sleep -Seconds 10; $ok = $true; break }
        if ($p.HasExited) { Write-Host "smoke: $name start: the game ended with exit code $($p.ExitCode)"; break }
    }
    if (-not $ok -and (Get-Date) -ge $deadline) { Write-Host "smoke: $name start: no frames in $minutes minutes" }
    if (-not $p.HasExited) { Save-Screen (Join-Path $Out "$name.png") $p }
    $text = Read-Log
    Set-Content -LiteralPath (Join-Path $Out "$name.log") -Value $text
    foreach ($line in $must) {
        if ($text -notmatch [regex]::Escape($line)) { Write-Host "smoke: $name start: the log never says '$line'"; $ok = $false }
    }
    $last = [regex]::Matches($text, 'windows: frames=\d+, last 5s=[\d.]+ fps') | Select-Object -Last 1
    if ($last) { Write-Host "smoke: $name start: $($last.Value)" }
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(30000) | Out-Null
    }
    Start-Sleep -Seconds 2
    return $ok
}

$first = Start-Game 'first' $FirstMinutes $(if ($x64) { @('functions translated in') } else { @() })
$second = $false
if ($first) {
    $second = Start-Game 'second' $SecondMinutes $(if ($x64) { @('the x64 translation cache is there') } else { @() })
}
if ($first -and $second) { Write-Host 'smoke: both starts drew frames'; exit 0 }
Write-Host 'smoke: FAILED'
exit 1
