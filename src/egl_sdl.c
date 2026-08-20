/*
 * egl_sdl.c -- SDL-owned EGL facade for KMSDRM/Wayland devices.
 *
 * Unity still follows its Android EGL lifecycle and sees stable fake EGL
 * handles.  Each fake context owns a real SDL GL context, and SDL performs the
 * final page flip.  The legacy Mali-450/fbdev profile never enters this file's
 * facade; egl.c keeps forwarding that backend to its native EGL driver.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "egl_sdl.h"
#include "nx_elf.h"

enum { VIDEO_UNDECIDED = -1, VIDEO_RAW_EGL = 0, VIDEO_SDL = 1 };

typedef struct {
    uint32_t magic;
    int is_window;
    int width;
    int height;
} bc_sdl_surface;

typedef struct {
    uint32_t magic;
    SDL_GLContext context;
    EGLSurface draw;
    EGLSurface read;
    int is_pbuffer;
    unsigned id;
} bc_sdl_context;

#define SURFACE_MAGIC 0x50465355u
#define CONTEXT_MAGIC 0x50464354u

static int video_mode = VIDEO_UNDECIDED;
static SDL_Window *video_window;
static SDL_GLContext share_root;
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned next_context_id = 1;
static int screen_width = 1280;
static int screen_height = 720;
static int depth_size = 24;
static int stencil_size = 8;
static unsigned frame_count;
static unsigned char display_tag;
static unsigned char config_tag;
static _Thread_local bc_sdl_context *current_context;

static int positive_env(const char *name);

static void capture_frame_if_requested(void)
{
    static int finished;
    if (finished)
        return;
    const char *path = getenv("BC_SCREENSHOT");
    if (!path || !*path)
        return;
    int wanted = positive_env("BC_SCREENSHOT_FRAME");
    if (!wanted)
        wanted = 300;
    if ((int)frame_count < wanted)
        return;
    finished = 1;

    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) =
        (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
            SDL_GL_GetProcAddress("glReadPixels");
    void (*get_int)(GLenum, GLint *) =
        (void (*)(GLenum, GLint *))SDL_GL_GetProcAddress("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) =
        (void (*)(GLenum, GLint))SDL_GL_GetProcAddress("glPixelStorei");
    if (!read_pixels || !get_int || !pixel_store) {
        nx_log("screenshot: required GLES functions are missing");
        return;
    }

    size_t pixels = (size_t)screen_width * (size_t)screen_height;
    unsigned char *rgba = malloc(pixels * 4);
    unsigned char *rgb = malloc(pixels * 3);
    if (!rgba || !rgb) {
        nx_log("screenshot: allocation failed");
        free(rgb);
        free(rgba);
        return;
    }

    GLint old_pack = 4;
    get_int(GL_PACK_ALIGNMENT, &old_pack);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, screen_width, screen_height,
                GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);
    for (int y = 0; y < screen_height; y++) {
        const unsigned char *source =
            rgba + (size_t)(screen_height - 1 - y) * screen_width * 4;
        unsigned char *dest = rgb + (size_t)y * screen_width * 3;
        for (int x = 0; x < screen_width; x++) {
            dest[x * 3 + 0] = source[x * 4 + 0];
            dest[x * 3 + 1] = source[x * 4 + 1];
            dest[x * 3 + 2] = source[x * 4 + 2];
        }
    }

    FILE *output = fopen(path, "wb");
    if (!output) {
        nx_log("screenshot: cannot open %s", path);
    } else {
        fprintf(output, "P6\n%d %d\n255\n", screen_width, screen_height);
        size_t written = fwrite(rgb, 3, pixels, output);
        if (fclose(output) == 0 && written == pixels)
            nx_log("screenshot: frame %u -> %s", frame_count, path);
        else
            nx_log("screenshot: incomplete write to %s", path);
    }
    free(rgb);
    free(rgba);
}

static int env_on(const char *name)
{
    const char *value = getenv(name);
    return value && *value && strcmp(value, "0") != 0 &&
           strcasecmp(value, "false") != 0 &&
           strcasecmp(value, "no") != 0 &&
           strcasecmp(value, "off") != 0;
}

static int positive_env(const char *name)
{
    const char *value = getenv(name);
    if (!value || !*value)
        return 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    return end && *end == '\0' && parsed > 0 && parsed <= 16384
               ? (int)parsed : 0;
}

static void detect_size(void)
{
    int width = positive_env("BC_SCREEN_W");
    int height = positive_env("BC_SCREEN_H");
    const char *source = NULL;
    if (width && height) {
        source = "launcher";
    } else {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 &&
            mode.w > 0 && mode.h > 0) {
            width = mode.w;
            height = mode.h;
            source = "SDL display mode";
        }
    }
    if (width && height) {
        screen_width = width;
        screen_height = height;
    }
    fprintf(stderr, "[bc/video] resolution %dx%d (%s)\n",
            screen_width, screen_height, source ? source : "fallback");
}

static int alpha_size;

static void set_context_attributes(int depth, int stencil, int alpha)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

static int context_is_gles(void)
{
    const GLubyte *(*get_string)(GLenum) =
        (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
    const char *version = get_string
                              ? (const char *)get_string(GL_VERSION) : NULL;
    return !version || strstr(version, "OpenGL ES") != NULL;
}

static int create_window(void)
{
    /* Alpha 8 vem primeiro: Unity 2022.3 contrata RGBA8888 e o Mesa/Panfrost
     * devolve RGBX8888 no primeiro match quando alpha 0 é aceito (tela preta
     * no RG-DS/ROCKNIX — fix aprovado no Horizon Chase v1.0.3).  Alpha 0
     * permanece como fallback para drivers sem config RGBA janela. */
    static const struct { int depth, stencil, alpha; } formats[] = {
        { 24, 8, 8 }, { 24, 8, 0 }, { 16, 0, 8 }, { 16, 0, 0 }, { 0, 0, 0 },
    };
    detect_size();

    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        set_context_attributes(formats[i].depth, formats[i].stencil,
                               formats[i].alpha);
        video_window = SDL_CreateWindow(
            "Bomb Chicken", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, screen_width, screen_height,
            SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (!video_window) {
            fprintf(stderr,
                    "[bc/video] SDL window depth%d/stencil%d failed: %s\n",
                    formats[i].depth, formats[i].stencil, SDL_GetError());
            continue;
        }

        share_root = SDL_GL_CreateContext(video_window);
        if (share_root && !context_is_gles()) {
            fprintf(stderr,
                    "[bc/video] SDL returned desktop GL; rejecting it\n");
            SDL_GL_DeleteContext(share_root);
            share_root = NULL;
        }
        if (share_root) {
            depth_size = formats[i].depth;
            stencil_size = formats[i].stencil;
            int r = 0, g = 0, b = 0, a = 0;
            SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
            SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
            SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
            SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
            alpha_size = a;
            fprintf(stderr,
                    "[bc/video] SDL config: R%dG%dB%dA%d depth%d stencil%d\n",
                    r, g, b, a, depth_size, stencil_size);
            break;
        }

        fprintf(stderr,
                "[bc/video] GLES2 depth%d/stencil%d failed: %s\n",
                formats[i].depth, formats[i].stencil, SDL_GetError());
        SDL_DestroyWindow(video_window);
        video_window = NULL;
    }

    if (!video_window || !share_root)
        return 0;

    int drawable_width = 0, drawable_height = 0;
    SDL_GL_GetDrawableSize(video_window, &drawable_width, &drawable_height);
    if (drawable_width > 0 && drawable_height > 0) {
        screen_width = drawable_width;
        screen_height = drawable_height;
    }
    SDL_GL_SetSwapInterval(1);

    const GLubyte *(*get_string)(GLenum) =
        (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
    fprintf(stderr,
            "[bc/video] SDL GLES2 depth%d/stencil%d drawable=%dx%d\n",
            depth_size, stencil_size, screen_width, screen_height);
    if (get_string) {
        fprintf(stderr, "[bc/video] GL_VENDOR=%s\n",
                get_string(GL_VENDOR) ?
                    (const char *)get_string(GL_VENDOR) : "?");
        fprintf(stderr, "[bc/video] GL_RENDERER=%s\n",
                get_string(GL_RENDERER) ?
                    (const char *)get_string(GL_RENDERER) : "?");
        fprintf(stderr, "[bc/video] GL_VERSION=%s\n",
                get_string(GL_VERSION) ?
                    (const char *)get_string(GL_VERSION) : "?");
    }
    SDL_GL_MakeCurrent(video_window, NULL);
    return 1;
}

int bc_sdl_video_init(void)
{
    if (video_mode != VIDEO_UNDECIDED)
        return video_mode == VIDEO_SDL;

    if (env_on("BC_RAW_EGL_CONTEXTS")) {
        video_mode = VIDEO_RAW_EGL;
        fprintf(stderr, "[bc/video] raw EGL selected by override\n");
        return 0;
    }

    int was_initialized = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        video_mode = VIDEO_RAW_EGL;
        fprintf(stderr,
                "[bc/video] SDL video unavailable (%s); using raw EGL\n",
                SDL_GetError());
        return 0;
    }

    const char *driver = SDL_GetCurrentVideoDriver();
    int force_sdl = env_on("BC_PURE_SDL_CONTEXTS");
    if (!force_sdl && driver && strcasecmp(driver, "mali") == 0) {
        video_mode = VIDEO_RAW_EGL;
        fprintf(stderr,
                "[bc/video] backend '%s' -> raw EGL/fbdev\n", driver);
        if (!was_initialized)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    video_mode = VIDEO_SDL;
    fprintf(stderr, "[bc/video] backend '%s' -> SDL-owned contexts%s\n",
            driver ? driver : "?", force_sdl ? " (override)" : "");
    if (!create_window())
        nx_die("SDL owns backend '%s' but could not create a GLES2 window: %s",
               driver ? driver : "?", SDL_GetError());
    return 1;
}

int bc_sdl_video_active(void)
{
    return video_mode == VIDEO_SDL;
}

void *bc_sdl_gl_proc(const char *name)
{
    return video_mode == VIDEO_SDL && name
               ? SDL_GL_GetProcAddress(name) : NULL;
}

static EGLDisplay sdl_eglGetDisplay(EGLNativeDisplayType native_display)
{
    (void)native_display;
    return (EGLDisplay)&display_tag;
}

static EGLBoolean sdl_eglInitialize(EGLDisplay display, EGLint *major,
                                    EGLint *minor)
{
    (void)display;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

static EGLBoolean sdl_eglTerminate(EGLDisplay display)
{
    (void)display;
    return EGL_TRUE;
}

static EGLBoolean sdl_eglGetConfigs(EGLDisplay display, EGLConfig *configs,
                                    EGLint config_size, EGLint *num_config)
{
    (void)display;
    if (configs && config_size > 0)
        configs[0] = (EGLConfig)&config_tag;
    if (num_config)
        *num_config = 1;
    return EGL_TRUE;
}

static EGLBoolean sdl_eglChooseConfig(EGLDisplay display,
                                      const EGLint *attributes,
                                      EGLConfig *configs,
                                      EGLint config_size,
                                      EGLint *num_config)
{
    (void)attributes;
    return sdl_eglGetConfigs(display, configs, config_size, num_config);
}

static EGLBoolean sdl_eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                                         EGLint attribute, EGLint *value)
{
    (void)display;
    (void)config;
    if (!value)
        return EGL_FALSE;
    switch (attribute) {
    case EGL_BUFFER_SIZE:       *value = 24 + alpha_size; break;
    case EGL_ALPHA_SIZE:        *value = alpha_size; break;
    case EGL_BLUE_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_RED_SIZE:          *value = 8; break;
    case EGL_DEPTH_SIZE:        *value = depth_size; break;
    case EGL_STENCIL_SIZE:      *value = stencil_size; break;
    case EGL_CONFIG_CAVEAT:     *value = EGL_NONE; break;
    case EGL_CONFIG_ID:         *value = 1; break;
    case EGL_SURFACE_TYPE:      *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT:        *value = EGL_OPENGL_ES2_BIT; break;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
    case EGL_NATIVE_RENDERABLE: *value = EGL_FALSE; break;
    default:                    *value = 0; break;
    }
    return EGL_TRUE;
}

static EGLSurface make_surface(int is_window, const EGLint *attributes)
{
    bc_sdl_surface *surface = calloc(1, sizeof *surface);
    if (!surface)
        return EGL_NO_SURFACE;
    surface->magic = SURFACE_MAGIC;
    surface->is_window = is_window;
    surface->width = is_window ? screen_width : 16;
    surface->height = is_window ? screen_height : 16;
    if (!is_window && attributes) {
        for (const EGLint *a = attributes; *a != EGL_NONE; a += 2) {
            if (a[0] == EGL_WIDTH && a[1] > 0)
                surface->width = a[1];
            else if (a[0] == EGL_HEIGHT && a[1] > 0)
                surface->height = a[1];
        }
    }
    return (EGLSurface)surface;
}

static EGLSurface sdl_eglCreateWindowSurface(EGLDisplay display,
                                              EGLConfig config,
                                              EGLNativeWindowType window,
                                              const EGLint *attributes)
{
    (void)display;
    (void)config;
    (void)window;
    EGLSurface surface = make_surface(1, attributes);
    nx_log("SDL EGL window surface -> %p", (void *)surface);
    return surface;
}

static EGLSurface sdl_eglCreatePbufferSurface(EGLDisplay display,
                                               EGLConfig config,
                                               const EGLint *attributes)
{
    (void)display;
    (void)config;
    return make_surface(0, attributes);
}

static EGLBoolean sdl_eglDestroySurface(EGLDisplay display,
                                        EGLSurface handle)
{
    (void)display;
    bc_sdl_surface *surface = (bc_sdl_surface *)handle;
    if (surface && surface->magic == SURFACE_MAGIC) {
        surface->magic = 0;
        free(surface);
    }
    return EGL_TRUE;
}

static EGLBoolean sdl_eglQuerySurface(EGLDisplay display, EGLSurface handle,
                                      EGLint attribute, EGLint *value)
{
    (void)display;
    bc_sdl_surface *surface = (bc_sdl_surface *)handle;
    if (!surface || surface->magic != SURFACE_MAGIC || !value)
        return EGL_FALSE;
    switch (attribute) {
    case EGL_WIDTH:         *value = surface->width; break;
    case EGL_HEIGHT:        *value = surface->height; break;
    case EGL_CONFIG_ID:     *value = 1; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    default:                *value = 0; break;
    }
    return EGL_TRUE;
}

static EGLContext sdl_eglCreateContext(EGLDisplay display, EGLConfig config,
                                       EGLContext share,
                                       const EGLint *attributes)
{
    (void)display;
    (void)config;
    (void)share;
    (void)attributes;
    bc_sdl_context *context = calloc(1, sizeof *context);
    if (!context)
        return EGL_NO_CONTEXT;

    pthread_mutex_lock(&context_lock);
    set_context_attributes(depth_size, stencil_size, alpha_size);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GL_MakeCurrent(video_window, share_root);
    context->context = SDL_GL_CreateContext(video_window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(video_window, NULL);
    pthread_mutex_unlock(&context_lock);

    if (!context->context) {
        nx_log("SDL EGL context creation failed: %s", SDL_GetError());
        free(context);
        return EGL_NO_CONTEXT;
    }
    context->magic = CONTEXT_MAGIC;
    context->id = next_context_id++;
    nx_log("SDL EGL context #%u -> %p", context->id, (void *)context);
    return (EGLContext)context;
}

static EGLBoolean sdl_eglDestroyContext(EGLDisplay display, EGLContext handle)
{
    (void)display;
    bc_sdl_context *context = (bc_sdl_context *)handle;
    if (context && context->magic == CONTEXT_MAGIC) {
        if (current_context == context) {
            SDL_GL_MakeCurrent(video_window, NULL);
            current_context = NULL;
        }
        if (context->context)
            SDL_GL_DeleteContext(context->context);
        context->magic = 0;
        free(context);
    }
    return EGL_TRUE;
}

static EGLBoolean sdl_eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                                     EGLSurface read, EGLContext handle)
{
    (void)display;
    if (handle == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE) {
        current_context = NULL;
        return SDL_GL_MakeCurrent(video_window, NULL) == 0
                   ? EGL_TRUE : EGL_FALSE;
    }

    bc_sdl_context *context = (bc_sdl_context *)handle;
    bc_sdl_surface *surface = (bc_sdl_surface *)draw;
    if (!context || context->magic != CONTEXT_MAGIC ||
        !surface || surface->magic != SURFACE_MAGIC)
        return EGL_FALSE;

    context->draw = draw;
    context->read = read;
    context->is_pbuffer = !surface->is_window;
    if (SDL_GL_MakeCurrent(video_window, context->context) != 0) {
        nx_log("SDL EGL MakeCurrent context #%u failed: %s",
               context->id, SDL_GetError());
        return EGL_FALSE;
    }
    current_context = context;
    return EGL_TRUE;
}

static EGLContext sdl_eglGetCurrentContext(void)
{
    return (EGLContext)current_context;
}

static EGLSurface sdl_eglGetCurrentSurface(EGLint which)
{
    if (!current_context)
        return EGL_NO_SURFACE;
    return which == EGL_READ ? current_context->read : current_context->draw;
}

static EGLDisplay sdl_eglGetCurrentDisplay(void)
{
    return current_context ? (EGLDisplay)&display_tag : EGL_NO_DISPLAY;
}

static EGLBoolean sdl_eglQueryContext(EGLDisplay display, EGLContext handle,
                                      EGLint attribute, EGLint *value)
{
    (void)display;
    bc_sdl_context *context = (bc_sdl_context *)handle;
    if (!context || context->magic != CONTEXT_MAGIC || !value)
        return EGL_FALSE;
    if (attribute == EGL_CONTEXT_CLIENT_VERSION)
        *value = 2;
    else if (attribute == EGL_CONFIG_ID)
        *value = 1;
    else
        *value = 0;
    return EGL_TRUE;
}

EGLBoolean bc_sdl_swap_buffers(EGLDisplay display, EGLSurface handle)
{
    (void)display;
    if (!current_context || current_context->draw != handle ||
        current_context->is_pbuffer)
        return EGL_TRUE;
    capture_frame_if_requested();
    SDL_GL_SwapWindow(video_window);
    frame_count++;
    if (frame_count <= 3 || frame_count % 600 == 0)
        nx_log("SDL page flip #%u", frame_count);
    return EGL_TRUE;
}

static EGLBoolean sdl_eglBindAPI(EGLenum api)
{
    return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE;
}

static EGLenum sdl_eglQueryAPI(void) { return EGL_OPENGL_ES_API; }
static EGLint sdl_eglGetError(void) { return EGL_SUCCESS; }

static const char *sdl_eglQueryString(EGLDisplay display, EGLint name)
{
    (void)display;
    switch (name) {
    case EGL_VENDOR:      return "Bomb Chicken SDL EGL";
    case EGL_VERSION:     return "1.4";
    case EGL_EXTENSIONS:  return "";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    default:              return "";
    }
}

static EGLBoolean sdl_eglSurfaceAttrib(EGLDisplay display, EGLSurface surface,
                                       EGLint attribute, EGLint value)
{
    (void)display; (void)surface; (void)attribute; (void)value;
    return EGL_TRUE;
}

static EGLBoolean sdl_eglSwapInterval(EGLDisplay display, EGLint interval)
{
    (void)display;
    return SDL_GL_SetSwapInterval(interval) == 0 ? EGL_TRUE : EGL_FALSE;
}

static EGLBoolean sdl_eglReleaseThread(void)
{
    current_context = NULL;
    return SDL_GL_MakeCurrent(video_window, NULL) == 0
               ? EGL_TRUE : EGL_FALSE;
}

static EGLBoolean sdl_eglWaitClient(void) { return EGL_TRUE; }
static EGLBoolean sdl_eglWaitGL(void) { return EGL_TRUE; }
static EGLBoolean sdl_eglWaitNative(EGLint engine)
{
    (void)engine;
    return EGL_TRUE;
}

void *bc_sdl_egl_proc(const char *name)
{
#define EGL_PROC(symbol) \
    if (strcmp(name, #symbol) == 0) return (void *)sdl_##symbol
    if (!name || video_mode != VIDEO_SDL)
        return NULL;
    EGL_PROC(eglGetDisplay);
    EGL_PROC(eglInitialize);
    EGL_PROC(eglTerminate);
    EGL_PROC(eglGetConfigs);
    EGL_PROC(eglChooseConfig);
    EGL_PROC(eglGetConfigAttrib);
    EGL_PROC(eglCreateWindowSurface);
    EGL_PROC(eglCreatePbufferSurface);
    EGL_PROC(eglDestroySurface);
    EGL_PROC(eglQuerySurface);
    EGL_PROC(eglBindAPI);
    EGL_PROC(eglQueryAPI);
    EGL_PROC(eglCreateContext);
    EGL_PROC(eglDestroyContext);
    EGL_PROC(eglMakeCurrent);
    EGL_PROC(eglGetCurrentContext);
    EGL_PROC(eglGetCurrentSurface);
    EGL_PROC(eglGetCurrentDisplay);
    EGL_PROC(eglQueryContext);
    EGL_PROC(eglGetError);
    EGL_PROC(eglQueryString);
    EGL_PROC(eglSurfaceAttrib);
    EGL_PROC(eglSwapInterval);
    EGL_PROC(eglReleaseThread);
    EGL_PROC(eglWaitClient);
    EGL_PROC(eglWaitGL);
    EGL_PROC(eglWaitNative);
#undef EGL_PROC
    return NULL;
}
