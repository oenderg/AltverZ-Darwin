/*
 * The four things the shared runtime still asks of the engine files PvZ2 does not
 * build (tools/regress/pvz2-sources.sh). Husk runs many engines; PvZ2 is a native
 * PopCap game and needs none of them, so its hosts leave those files out -- they stay
 * in the tree, so the fork keeps merging cleanly with upstream -- and this answers for
 * them the way they answer for PvZ2:
 *
 *   tl_cocos_audio_hook  the shared speaker output (husk-tl-jni-fmod.c defines it);
 *                        the audio code installs the real writer into it at start-up.
 *   tl_manifest_gles     the manifest's OpenGL ES version (husk-tl-sdl.c); only the
 *                        generic Java-app start-up asks, and PvZ2 does not use it.
 *   tl_vk_available,     Vulkan through MoltenVK (husk-tl-vulkan.m); PvZ2 draws with
 *   tl_vk_resolve        OpenGL ES, and no MoltenVK is configured.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void (*tl_cocos_audio_hook)(const int16_t *samples, int frames, int channels, int rate);

int tl_manifest_gles(const char *apk) { (void)apk; return 0; }

bool tl_vk_available(void) { return false; }

void *tl_vk_resolve(const char *name) { (void)name; return NULL; }
