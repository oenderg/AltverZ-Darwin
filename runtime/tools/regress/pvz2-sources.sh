# The runtime sources the PvZ2 hosts compile (sourced by build-macos-app.sh and
# build-ios-app.sh; needs R, N and T set). Husk supports many engines and PvZ2 needs
# none of them: their files are left out here, not deleted, so the fork keeps merging
# cleanly with upstream. tools/pvz2-standins.c answers the four calls the shared code
# still makes into them.
PVZ2_UNUSED="husk-tl-unity.c husk-tl-unity-app.c husk-tl-jni-ue4.c
    husk-tl-flutter.c husk-tl-flutter-codec.c husk-tl-flutter-plugins.c husk-tl-flutter-text.c
    husk-tl-godot.c husk-tl-jni-godot.c husk-tl-gamemaker.c
    husk-tl-cocos.c husk-tl-cocos-text.c husk-tl-jni-cocos.c husk-tl-jni-fmod.c
    husk-tl-gtasa.c husk-tl-geode.c husk-tl-jni-minecraft.c husk-tl-python.c
    husk-tl-sdl.c husk-tl-sdl-apps.c husk-tl-vulkan.m
    husk-tl-gameactivity.c husk-tl-nativeactivity.c"

pvz2_runtime_sources() {
    local f
    for f in "$N"/*.c "$N"/*.m; do
        case " $(echo $PVZ2_UNUSED) " in *" $(basename "$f") "*) ;; *) echo "$f" ;; esac
    done
    echo "$R/tools/pvz2-standins.c"
    echo "$T/husk-tl-elf.c" "$T/husk-tl-zip.c" "$T/husk-tl-sound.c"
}
