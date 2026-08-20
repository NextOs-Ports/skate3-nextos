#ifndef BC_EGL_SDL_H
#define BC_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int bc_sdl_video_init(void);
int bc_sdl_video_active(void);
void *bc_sdl_gl_proc(const char *name);
void *bc_sdl_egl_proc(const char *name);
EGLBoolean bc_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
