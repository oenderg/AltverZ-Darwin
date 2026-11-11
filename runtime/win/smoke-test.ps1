# SPDX-License-Identifier: GPL-2.0-or-later
# Start a portable Windows build twice and check that it draws what it should, in CI or by hand:
#
#   pwsh runtime/win/smoke-test.ps1 -Game <unpacked zip folder> -Obb <main...obb> -Out <folder> [-Baseline <png>]
#   pwsh runtime/win/smoke-test.ps1 -Picture <png> -Baseline <png>      (how alike two pictures are)
#
# The first start is a new player's: no data but the OBB (linked in, so it is not downloaded), so the x64
# build translates the game first. The second start must find the translation it saved (x64). Each start
# passes once the game draws frames and, with -Baseline, once a picture of its window matches the baseline
# (at least -Match percent alike, within 30 s). Each start's log and picture go to -Out.
# Exits 1 when a start shows an error, ends on its own, draws nothing in time or draws the wrong picture.
param(
    [string]$Game,
    [string]$Obb,
    [string]$Out,
    [string]$Baseline,
    [string]$Picture,
    [int]$Match = 90,
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

# How alike two pictures are, in percent: both scaled to 64x48, the share of pixels whose red, green and
# blue are each within 32 of the other's. Runs of the same build on the runners come out at 100, a black
# window at about 5, the same screen in a window of another shape at about 40.
function Get-Alike([string]$a, [string]$b) {
    $w = 64; $h = 48
    $small = foreach ($path in $a, $b) {
        $src = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $path).Path)
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($src, 0, 0, $w, $h)
        $g.Dispose(); $src.Dispose()
        , $bmp
    }
    $same = 0
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $p = $small[0].GetPixel($x, $y); $q = $small[1].GetPixel($x, $y)
            if ([Math]::Abs($p.R - $q.R) -le 32 -and [Math]::Abs($p.G - $q.G) -le 32 -and [Math]::Abs($p.B - $q.B) -le 32) { $same++ }
        }
    }
    $small[0].Dispose(); $small[1].Dispose()
    return [Math]::Round($same * 100.0 / ($w * $h), 1)
}

if ($Picture) {
    if (-not $Baseline) { throw '-Picture needs -Baseline' }
    Write-Host "smoke: $Picture is $(Get-Alike $Picture $Baseline)% like $Baseline"
    exit 0
}
foreach ($need in 'Game', 'Obb', 'Out') { if (-not (Get-Variable $need -ValueOnly)) { throw "-$need is needed" } }
# Without its baseline the starts still run and keep their pictures (the new baseline), then fail
$noBaseline = $Baseline -and -not (Test-Path -LiteralPath $Baseline)
if ($noBaseline) { Write-Host "smoke: no baseline picture at ${Baseline}: the pictures are kept, and the test fails"; $Baseline = $null }

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

# Once frames are drawn: the window's picture, taken until it matches the baseline (30 s at most). Keeps the
# closest one as <name>.png; true when it matches, or when there is no baseline to match.
function Test-Picture([string]$name, $p) {
    $path = Join-Path $Out "$name.png"
    # Without a baseline, the picture that will become one: past the loading screen
    if (-not $Baseline) { Start-Sleep -Seconds 10; Save-Screen $path $p; return $true }
    $try = Join-Path $Out "$name-try.png"
    $best = -1
    $until = (Get-Date).AddSeconds(30)
    do {
        if ($p.HasExited) { break }
        Save-Screen $try $p
        if (Test-Path -LiteralPath $try) {
            $alike = Get-Alike $try $Baseline
            if ($alike -gt $best) { $best = $alike; Move-Item -Force -LiteralPath $try -Destination $path }
            if ($best -ge $Match) { break }
        }
        Start-Sleep -Seconds 3
    } while ((Get-Date) -lt $until)
    if (Test-Path -LiteralPath $try) { Remove-Item -LiteralPath $try -Force }
    Write-Host "smoke: $name start: the picture is $best% like the baseline (at least $Match% needed)"
    if ($best -ge $Match) { return $true }
    Copy-Item -LiteralPath $Baseline -Destination (Join-Path $Out 'baseline.png') -Force
    return $false
}

# One start: true once a 5 s frame line shows frames drawn and the picture matches; false on an error, an
# exit, the time limit or another picture.
function Start-Game([string]$name, [int]$minutes, [string[]]$must) {
    Write-Host "smoke: $name start of $($exe.Name)"
    # The previous start's log is kept in -Out: without it, its frame lines cannot pass this start
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    $p = Start-Process -FilePath $exe.FullName -WorkingDirectory $Game -PassThru
    $deadline = (Get-Date).AddMinutes($minutes)
    $shown = 0
    $drew = $false
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
        if ($drawn) { $drew = $true; break }
        if ($p.HasExited) { Write-Host "smoke: $name start: the game ended with exit code $($p.ExitCode)"; break }
    }
    if (-not $drew -and (Get-Date) -ge $deadline) { Write-Host "smoke: $name start: no frames in $minutes minutes" }
    $ok = $drew
    if ($drew) { $ok = Test-Picture $name $p }
    elseif (-not $p.HasExited) { Save-Screen (Join-Path $Out "$name.png") $p }
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
if ($first -and $second -and $noBaseline) { Write-Host 'smoke: FAILED: both starts drew, but there is no baseline to compare with'; exit 1 }
if ($first -and $second) { Write-Host 'smoke: both starts drew the picture they should'; exit 0 }
Write-Host 'smoke: FAILED'
exit 1
