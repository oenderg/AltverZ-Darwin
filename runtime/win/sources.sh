# The runtime files the Windows programs build (sourced by build-win.sh; needs H, N, T).
WIN_CORE="$H/tl-win-compat.c $H/husk-tl-aot-win.c
  $H/husk-tl-bionic-win.c $H/husk-tl-bionic-win-io.c $H/husk-tl-bionic-win-thread.c $H/husk-tl-bionic-win-str.c
  $N/husk-tl-ld.c $N/husk-tl-a64.c $N/husk-tl-xmem.c $N/husk-tl-jni.c $N/husk-tl-va.c $N/husk-tl-dexindex.c $N/husk-tl-dvm.c $N/husk-tl-dvm-natives.c $N/husk-tl-dvm-annotations.c $N/husk-tl-dvm-proxy.c $N/husk-tl-jni-hle.c $N/husk-tl-dvm-android.c $N/husk-tl-jni-gms.c $N/husk-tl-jni-input.c $N/husk-tl-jni-loop.c $N/husk-tl-jni-tls.c $N/husk-tl-dvm-gles10.c $N/husk-tl-dvm-gles10ext.c $N/husk-tl-dvm-gles11.c $N/husk-tl-dvm-gles11ext.c $N/husk-tl-dvm-gles20.c $N/husk-tl-dvm-gles30.c $N/husk-tl-dvm-gles31.c $N/husk-tl-dvm-gles32.c $H/../tools/pvz2-standins.c $H/husk-tl-win-standins.c $N/husk-tl-gamepad.c $N/husk-tl-egl.c $H/husk-tl-nwindow-win.c $H/husk-tl-crash-win.c
  $T/husk-tl-elf.c $T/husk-tl-zip.c"

# The game: the driver and the Windows host.
WIN_GAME="$N/husk-tl-pvz2.c $N/husk-tl-pvz2-picker.c $N/husk-tl-opensles.c
  $H/husk-tl-audio-win.c $H/husk-tl-cdn-win.c $H/husk-tl-pvz2-level-win.c $T/husk-tl-sound.c
  $H/../tools/pvz2-windows.c $H/../tools/pvz2-windows-settings.c"
