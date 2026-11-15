#!/bin/bash
# Copy the DLLs the x86-64 program needs from MSYS2's CLANG64 into a folder, for the setup and
# the zip (build-installer.sh and package-zip.sh take it as --x64-runtime). In the CLANG64 shell:
#
#   runtime/win/x64/collect-dlls.sh <PvZ2 Reflourished x64.exe> <folder>
#
# It follows the imports (llvm-objdump -p) from the program through every DLL found in
# /clang64/bin; the rest are Windows' own (and WinSparkle.dll, which the setup adds). The set
# must be the one runtime/win/licenses names: a new DLL needs its license there, so the
# script stops when the set changes.
set -euo pipefail
EXE="${1:?usage: collect-dlls.sh <exe> <folder>}"; OUT="${2:?usage: collect-dlls.sh <exe> <folder>}"
BIN="${CLANG64_BIN:-/clang64/bin}"
EXPECTED="libc++.dll libffi-8.dll libiconv-2.dll libllvm-23.dll libwinpthread-1.dll libxml2-16.dll libzstd.dll zlib1.dll"

imports() { llvm-objdump -p "$1" | sed -n 's/^ *DLL Name: *//p' | tr -d '\r'; }
mkdir -p "$OUT"
found=()
queue=("$EXE")
while [ ${#queue[@]} -gt 0 ]; do
    file="${queue[0]}"; queue=("${queue[@]:1}")
    while read -r dll; do
        [ -n "$dll" ] || continue
        if [ ! -f "$BIN/$dll" ]; then
            # Windows' own DLLs are not in $BIN; a lib* one that is missing would be a program that does not start
            case "$dll" in
                lib*|LIB*) echo "collect-dlls: $file imports $dll, which is not in $BIN" >&2; exit 1 ;;
            esac
            continue
        fi
        case " ${found[*]-} " in *" $dll "*) continue ;; esac
        found+=("$dll")
        cp "$BIN/$dll" "$OUT/"
        queue+=("$BIN/$dll")
    done < <(imports "$file")
done

got="$(printf '%s\n' "${found[@]}" | tr 'A-Z' 'a-z' | sort | tr '\n' ' ' | sed 's/ $//')"
if [ "$got" != "$EXPECTED" ]; then
    echo "collect-dlls: the program needs: $got" >&2
    echo "collect-dlls: runtime/win/licenses covers: $EXPECTED" >&2
    exit 1
fi
echo "collect-dlls: ${#found[@]} DLLs in $OUT: $got"
