#!/bin/sh
# Regenerate the host-side signatures (sigs-tables.inc) on the Mac with llvm-mingw's x86-64 clang:
#   TL_WIN_TOOLCHAIN=... runtime/win/x64/gen-all.sh
# The GLES/EGL ones (sigs-gl.inc) need the Khronos headers, which the VM's MSYS2 has:
#   python gen-sigs.py sigs-gl.inc clang -- gl-protos.c:%gl+egl     (in runtime/win/x64, CLANG64 shell)
set -e
H="$(cd "$(dirname "$0")/.." && pwd)"
TC="${TL_WIN_TOOLCHAIN:?set TL_WIN_TOOLCHAIN}"
cd "$H"
python3 x64/gen-sigs.py x64/sigs-tables.inc "$TC/llvm-mingw-20261020-ucrt-macos-universal/bin/x86_64-w64-mingw32-clang" \
    -std=gnu11 -D_GNU_SOURCE -DTL_WINDOWS=1 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -include "$H/compat/win-compat.h" \
    -I"$H/compat" -I"$H/../src/translation-layer" -I"$H/../src/translation-layer-next" -I"$H/../tools" -I"$TC/win-arm64/include" -- \
    husk-tl-bionic-win.c:tl_tab_win_core husk-tl-bionic-win-io.c:tl_tab_win_io husk-tl-bionic-win-thread.c:tl_tab_win_thread \
    husk-tl-bionic-win-str.c:tl_tab_win_str,tl_tab_win_math \
    "../src/translation-layer-next/husk-tl-opensles.c:tl_tab_opensles,#k_vt_object=slobject,#k_vt_play=slplay,#k_vt_bq=slbq,#k_vt_volume=slvolume,#k_vt_config=slconfig,#k_vt_engine=slengine" \
    "../src/translation-layer-next/husk-tl-jni.c:@g_env_fns=jni@build_tables+tl_jni_init,#g_vm_fns=jvm"
