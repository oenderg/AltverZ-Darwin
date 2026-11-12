/* For gen-sigs.py: every GLES 2/3.x and EGL prototype, extensions included (the game's GL calls go straight to ANGLE). */
#define GL_GLEXT_PROTOTYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
