/*
 * egl.c -- EGL bridge for a Unity build that contains GLES2 and GLES3 paths.
 *
 * Hitman GO advertises GLES2 and carries m_GraphicsAPIs=[8,11].  The Mali-450
 * stops at GLES2, so any optional GLES3 request is negotiated here rather than
 * by patching the original player or data:
 *
 *   - eglChooseConfig strips EGL_OPENGL_ES3_BIT_KHR from the renderable-type
 *     mask so the driver actually returns configs,
 *   - eglCreateContext rewrites EGL_CONTEXT_CLIENT_VERSION 3 to 2 and drops the
 *     ES3-only context attributes,
 *   - eglQueryString(EGL_VERSION) is left alone: Unity reads the *GL* version
 *     string through glGetString, which the shader layer answers.
 *
 * Everything else forwards to the system EGL, which on this device is the
 * Mali fbdev driver.  We never set SDL_VIDEODRIVER and never pick a display
 * ourselves; EGL_DEFAULT_DISPLAY is what the driver wants on fbdev.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "bc.h"
#include "egl_sdl.h"
#include "etc2_decode.h"

/* Attribute names that only exist for ES3 contexts. */
#define EGL_CONTEXT_MINOR_VERSION_KHR      0x30FB
#define EGL_OPENGL_ES3_BIT_KHR             0x00000040

static void *libegl;

static void *sys(const char *n)
{
    if (!libegl) {
        libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            nx_die("cannot open the system libEGL: %s", dlerror());
    }
    void *f = dlsym(libegl, n);
    if (!f)
        nx_log("system EGL has no %s", n);
    return f;
}

/* GL entry points come from the driver blob, which on this image is what every
 * libGLES* name links to.  Unity does not import them through the PLT: it builds
 * its own function table at runtime, and an entry it cannot resolve stays NULL
 * and is called anyway -- glGetString(GL_EXTENSIONS) jumping to 0 is what a
 * missing lookup looks like from the crash.  Note the driver's
 * eglGetProcAddress answers for extensions only, so the core names have to come
 * from here. */
static void *libgl;

static void *gl_raw(const char *name)
{
    if (!name || name[0] != 'g' || name[1] != 'l')
        return NULL;
    if (bc_sdl_video_active())
        return bc_sdl_gl_proc(name);
    if (!libgl) {
        static const char *const cands[] = {
            "libGLESv2.so.2", "libGLESv2.so", "libGLESv3.so", "libmali.so",
        };
        for (size_t i = 0; i < sizeof cands / sizeof *cands && !libgl; i++)
            libgl = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (!libgl) {
            nx_log("cannot open a system GLES library: %s", dlerror());
            return NULL;
        }
    }
    return dlsym(libgl, name);
}

static GLenum shader_types[256];
static unsigned long shader_sources;
static unsigned long shader_compiles;
static unsigned long program_links;
static unsigned long draw_calls;
static unsigned long texture_images;
static unsigned long texture_sub_images;
static unsigned long texture_binds;
static unsigned long texture_parameters;
static unsigned long buffer_uploads;
static unsigned long framebuffer_calls;
static GLenum active_texture_unit = GL_TEXTURE0;
static GLuint bound_texture_2d;
static GLuint bound_array_buffer;
static GLuint bound_element_array_buffer;
static GLuint current_program;

typedef struct {
    GLuint program;
    GLint location;
    char name[64];
} bc_uniform_name;

static bc_uniform_name uniform_names[128];
static size_t uniform_name_count;

static const char *uniform_label(GLint location)
{
    for (size_t i = uniform_name_count; i > 0; i--)
        if (uniform_names[i - 1].location == location)
            return uniform_names[i - 1].name;
    return "?";
}

static void remember_uniform(GLuint program, GLint location, const char *name)
{
    if (location < 0 || !name || uniform_name_count >=
                                  sizeof uniform_names / sizeof *uniform_names)
        return;
    bc_uniform_name *entry = &uniform_names[uniform_name_count++];
    entry->program = program;
    entry->location = location;
    snprintf(entry->name, sizeof entry->name, "%s", name);
}

static int trace_texture_call(unsigned long call, GLsizei width,
                              GLsizei height)
{
    return bc_trace_gl && (call <= 160 || width >= 512 || height >= 512);
}

unsigned long bc_swap_count;


/* Sondas de ambiente do caminho quente lidas UMA vez: getenv e' varredura
 * linear do environ e estas ficam DENTRO do quadro (glBindFramebuffer sozinho
 * roda dezenas de vezes por frame). */
static int bc_env_flag(const char *name)
{
    const char *v = getenv(name);
    return v != NULL;
}

/* Print sob demanda: o input pede um shot (L3 com BC_SHOT ligado) e o proximo
   frame grava.  Existe porque ler /dev/fb0 de fora devolve preto enquanto o
   Mali renderiza — a unica testemunha honesta e o glReadPixels de dentro. */
int bc_shot_request;
static char bc_shot_path[256];

static void capture_current_fbo_if_requested(void)
{
    static int finished;
    static const char *path;
    static int path_known;
    static unsigned shot_seq;
    if (!path_known) {
        path = getenv("BC_FBO_CAPTURE");
        path_known = 1;
    }
    int on_demand = 0;
    if (bc_shot_request) {
        bc_shot_request = 0;
        /* Sem caminho cravado no binario de release: o padrao e' a pasta do
           proprio jogo, seja qual for onde ele foi instalado. */
        const char *dir = getenv("BC_SHOT_DIR");
        if (!dir || !*dir) dir = bc_gamedir;
        snprintf(bc_shot_path, sizeof bc_shot_path, "%.200s/shot_%03u.ppm",
                 dir, ++shot_seq);
        on_demand = 1;
    }
    if (on_demand) {
        path = bc_shot_path;
        finished = 0;
    }
    if (finished || !path || !*path || draw_calls < 2)
        return;
    if (!on_demand) {
        const char *want = getenv("BC_FBO_CAPTURE_FRAME");
        if (want && *want && bc_swap_count < strtoul(want, NULL, 10))
            return;
    }
    finished = 1;
    if (on_demand)
        fprintf(stderr, "[bc/shot] gravando %s\n", path);

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels)
        return;
    GLint viewport[4] = { 0, 0, 0, 0 };
    get_int(GL_VIEWPORT, viewport);
    int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return;
    size_t pixels = (size_t)width * (size_t)height;
    unsigned char *rgba = malloc(pixels * 4);
    unsigned char *rgb = malloc(pixels * 3);
    if (!rgba || !rgb) {
        free(rgb);
        free(rgba);
        return;
    }
    GLint old_pack = 4;
    get_int(GL_PACK_ALIGNMENT, &old_pack);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);
    for (int y = 0; y < height; y++) {
        const unsigned char *source =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        unsigned char *dest = rgb + (size_t)y * (size_t)width * 3;
        for (int x = 0; x < width; x++) {
            dest[x * 3 + 0] = source[x * 4 + 0];
            dest[x * 3 + 1] = source[x * 4 + 1];
            dest[x * 3 + 2] = source[x * 4 + 2];
        }
    }
    FILE *output = fopen(path, "wb");
    if (output) {
        fprintf(output, "P6\n%d %d\n255\n", width, height);
        size_t written = fwrite(rgb, 3, pixels, output);
        if (fclose(output) == 0 && written == pixels)
            nx_log("FBO capture: %dx%d -> %s", width, height, path);
    }
    free(rgb);
    free(rgba);
}

static void trace_buffer_payload(const void *data, GLsizeiptr size)
{
    uint32_t words[4] = { 0, 0, 0, 0 };
    if (!data || size <= 0) {
        fprintf(stderr, " data=null");
        return;
    }
    size_t take = (size_t)size < sizeof words ? (size_t)size : sizeof words;
    memcpy(words, data, take);
    fprintf(stderr, " data=%08x,%08x,%08x,%08x",
            words[0], words[1], words[2], words[3]);
}

static void dump_buffer_upload(GLenum target, GLuint buffer, GLintptr offset,
                               const void *data, GLsizeiptr size,
                               unsigned long upload)
{
    const char *prefix = getenv("BC_GL_DUMP_PREFIX");
    if (!prefix || !*prefix || !data || size <= 0)
        return;
    char path[1024];
    int length = snprintf(path, sizeof path,
                          "%s-buffer-%u-target-%x-offset-%ld-upload-%lu.bin",
                          prefix, buffer, target, (long)offset, upload);
    if (length < 0 || (size_t)length >= sizeof path)
        return;
    FILE *output = fopen(path, "wb");
    if (!output) {
        nx_log("GL dump: cannot open %s", path);
        return;
    }
    size_t written = fwrite(data, 1, (size_t)size, output);
    if (fclose(output) != 0 || written != (size_t)size)
        nx_log("GL dump: short write for %s", path);
    else
        nx_log("GL dump: %ld bytes -> %s", (long)size, path);
}

/*
 * Amlogic's fbdev OSD compositor uses the alpha channel of fb0.  Unity can
 * leave valid RGB with alpha zero, which makes a correctly rendered frame scan
 * out as black.  Mirror the small piece of GL state needed by the proven
 * Horizon Chase A-only clear so the regular swap path does not introduce
 * glGet* stalls on every frame.
 */
typedef struct {
    int initialized;
    GLuint framebuffer;
    GLboolean color_mask[4];
    GLfloat clear_color[4];
    GLboolean scissor;
} bc_gl_state;

static bc_gl_state gl_state = {
    .color_mask = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
};

static void my_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    static void (*real)(GLenum, GLuint);
    static void (*finish)(void);
    if (!real) {
        real = gl_raw("glBindFramebuffer");
        if (!real)
            real = gl_raw("glBindFramebufferOES");
    }
    GLuint previous = gl_state.framebuffer;
    if (previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == 0x8CA9))
        capture_current_fbo_if_requested();
    static int fbo_finish = -1;
    if (fbo_finish < 0)
        fbo_finish = bc_env_flag("BC_FBO_FINISH");
    if (fbo_finish && previous != 0 && framebuffer == 0 &&
        (target == GL_FRAMEBUFFER || target == 0x8CA9)) {
        if (!finish)
            finish = gl_raw("glFinish");
        if (finish) {
            finish();
            if (bc_trace_gl)
                fprintf(stderr,
                        "[bc/gl] forced finish before fbo %u -> 0\n",
                        previous);
        }
    }
    if (real)
        real(target, framebuffer);
    if (target == GL_FRAMEBUFFER || target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */)
        gl_state.framebuffer = framebuffer;
    framebuffer_calls++;
    if (bc_trace_gl && framebuffer_calls <= 160)
        fprintf(stderr, "[bc/gl] fbo-bind #%lu target=%#x framebuffer=%u\n",
                framebuffer_calls, target, framebuffer);
}

static void my_glDeleteFramebuffers(GLsizei count, const GLuint *framebuffers)
{
    static void (*real)(GLsizei, const GLuint *);
    GLuint current = gl_state.framebuffer;
    if (!real) {
        real = gl_raw("glDeleteFramebuffers");
        if (!real)
            real = gl_raw("glDeleteFramebuffersOES");
    }
    if (real)
        real(count, framebuffers);
    if (current && count > 0 && framebuffers) {
        for (GLsizei i = 0; i < count; i++) {
            if (framebuffers[i] == current) {
                gl_state.framebuffer = 0;
                break;
            }
        }
    }
}

static void my_glColorMask(GLboolean red, GLboolean green, GLboolean blue,
                           GLboolean alpha)
{
    static void (*real)(GLboolean, GLboolean, GLboolean, GLboolean);
    gl_state.color_mask[0] = !!red;
    gl_state.color_mask[1] = !!green;
    gl_state.color_mask[2] = !!blue;
    gl_state.color_mask[3] = !!alpha;
    if (!real)
        real = gl_raw("glColorMask");
    if (real)
        real(red, green, blue, alpha);
}

static void my_glClearColor(GLfloat red, GLfloat green, GLfloat blue,
                            GLfloat alpha)
{
    static void (*real)(GLfloat, GLfloat, GLfloat, GLfloat);
    gl_state.clear_color[0] = red;
    gl_state.clear_color[1] = green;
    gl_state.clear_color[2] = blue;
    gl_state.clear_color[3] = alpha;
    if (!real)
        real = gl_raw("glClearColor");
    if (real)
        real(red, green, blue, alpha);
}

static void my_glEnable(GLenum capability)
{
    static void (*real)(GLenum);
    if (capability == GL_SCISSOR_TEST)
        gl_state.scissor = GL_TRUE;
    if (!real)
        real = gl_raw("glEnable");
    if (real)
        real(capability);
}

static void my_glDisable(GLenum capability)
{
    static void (*real)(GLenum);
    if (capability == GL_SCISSOR_TEST)
        gl_state.scissor = GL_FALSE;
    if (!real)
        real = gl_raw("glDisable");
    if (real)
        real(capability);
}

static void force_opaque_backbuffer(void)
{
    static int no_opaque = -1;
    if (no_opaque < 0)
        no_opaque = bc_env_flag("BC_NO_OPAQUE_BACKBUFFER");
    if (no_opaque)
        return;

    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*clear)(GLbitfield);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);

    if (!gl_state.initialized) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        if (!get_int || !get_float || !get_bool || !is_enabled)
            return;
        get_int(GL_FRAMEBUFFER_BINDING, (GLint *)&gl_state.framebuffer);
        get_float(GL_COLOR_CLEAR_VALUE, gl_state.clear_color);
        get_bool(GL_COLOR_WRITEMASK, gl_state.color_mask);
        gl_state.scissor = !!is_enabled(GL_SCISSOR_TEST);
        gl_state.initialized = 1;
        static int glstate_trace = -1;
        if (glstate_trace < 0)
            glstate_trace = bc_env_flag("BC_GLSTATE_TRACE");
        if (glstate_trace) {
            fprintf(stderr,
                    "[bc/gl] state fbo=%u mask=%u%u%u%u "
                    "clear=%.2f,%.2f,%.2f,%.2f scissor=%u\n",
                    gl_state.framebuffer, gl_state.color_mask[0],
                    gl_state.color_mask[1], gl_state.color_mask[2],
                    gl_state.color_mask[3], gl_state.clear_color[0],
                    gl_state.clear_color[1], gl_state.clear_color[2],
                    gl_state.clear_color[3], gl_state.scissor);
        }
    }

    if (gl_state.framebuffer != 0)
        return;
    if (!color_mask) {
        color_mask = gl_raw("glColorMask");
        clear_color = gl_raw("glClearColor");
        clear = gl_raw("glClear");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
    }
    if (!color_mask || !clear_color || !clear || !enable || !disable)
        return;

    GLboolean old_mask[4];
    GLfloat old_clear[4];
    memcpy(old_mask, gl_state.color_mask, sizeof old_mask);
    memcpy(old_clear, gl_state.clear_color, sizeof old_clear);
    GLboolean had_scissor = gl_state.scissor;

    if (had_scissor)
        disable(GL_SCISSOR_TEST);
    color_mask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    if (had_scissor)
        enable(GL_SCISSOR_TEST);
}

static void clear_cursor_runs(const uint16_t *mask, int rows, int x, int y,
                              int scale, int width, int height,
                              void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                              void (*clear)(GLbitfield))
{
    for (int row = 0; row < rows; row++) {
        uint16_t bits = mask[row];
        for (int col = 0; col < 16;) {
            while (col < 16 && !(bits & (UINT16_C(1) << col)))
                col++;
            int start = col;
            while (col < 16 && (bits & (UINT16_C(1) << col)))
                col++;
            if (start == col)
                continue;
            int sx = x + start * scale;
            int top = y + row * scale;
            int sw = (col - start) * scale;
            int sh = scale;
            if (sx < 0) {
                sw += sx;
                sx = 0;
            }
            if (top < 0) {
                sh += top;
                top = 0;
            }
            if (sx + sw > width)
                sw = width - sx;
            if (top + sh > height)
                sh = height - top;
            if (sw <= 0 || sh <= 0)
                continue;
            scissor(sx, height - top - sh, sw, sh);
            clear(GL_COLOR_BUFFER_BIT);
        }
    }
}

typedef struct {
    char character;
    uint8_t row[7];
} pixel_glyph;

static const pixel_glyph keyboard_font[] = {
    { 'A', {14,17,17,31,17,17,17} },
    { 'B', {30,17,17,30,17,17,30} },
    { 'C', {14,17,16,16,16,17,14} },
    { 'D', {30,17,17,17,17,17,30} },
    { 'E', {31,16,16,30,16,16,31} },
    { 'F', {31,16,16,30,16,16,16} },
    { 'G', {14,17,16,23,17,17,14} },
    { 'H', {17,17,17,31,17,17,17} },
    { 'I', {31,4,4,4,4,4,31} },
    { 'J', {7,2,2,2,18,18,12} },
    { 'K', {17,18,20,24,20,18,17} },
    { 'L', {16,16,16,16,16,16,31} },
    { 'M', {17,27,21,21,17,17,17} },
    { 'N', {17,25,21,19,17,17,17} },
    { 'O', {14,17,17,17,17,17,14} },
    { 'P', {30,17,17,30,16,16,16} },
    { 'Q', {14,17,17,17,21,18,13} },
    { 'R', {30,17,17,30,20,18,17} },
    { 'S', {15,16,16,14,1,1,30} },
    { 'T', {31,4,4,4,4,4,4} },
    { 'U', {17,17,17,17,17,17,14} },
    { 'V', {17,17,17,17,17,10,4} },
    { 'W', {17,17,17,21,21,21,10} },
    { 'X', {17,17,10,4,10,17,17} },
    { 'Y', {17,17,10,4,4,4,4} },
    { 'Z', {31,1,2,4,8,16,31} },
    { 'a', {0,0,14,1,15,17,15} },
    { 'b', {16,16,30,17,17,17,30} },
    { 'c', {0,0,14,17,16,17,14} },
    { 'd', {1,1,15,17,17,17,15} },
    { 'e', {0,0,14,17,31,16,14} },
    { 'f', {6,9,8,28,8,8,8} },
    { 'g', {0,0,15,17,15,1,14} },
    { 'h', {16,16,30,17,17,17,17} },
    { 'i', {4,0,12,4,4,4,14} },
    { 'j', {2,0,6,2,2,18,12} },
    { 'k', {16,16,18,20,24,20,18} },
    { 'l', {12,4,4,4,4,4,14} },
    { 'm', {0,0,26,21,21,17,17} },
    { 'n', {0,0,30,17,17,17,17} },
    { 'o', {0,0,14,17,17,17,14} },
    { 'p', {0,0,30,17,30,16,16} },
    { 'q', {0,0,15,17,15,1,1} },
    { 'r', {0,0,22,25,16,16,16} },
    { 's', {0,0,15,16,14,1,30} },
    { 't', {8,8,28,8,8,9,6} },
    { 'u', {0,0,17,17,17,19,13} },
    { 'v', {0,0,17,17,17,10,4} },
    { 'w', {0,0,17,17,21,21,10} },
    { 'x', {0,0,17,10,4,10,17} },
    { 'y', {0,0,17,17,15,1,14} },
    { 'z', {0,0,31,2,4,8,31} },
    { '0', {14,17,19,21,25,17,14} },
    { '1', {4,12,4,4,4,4,14} },
    { '2', {14,17,1,2,4,8,31} },
    { '3', {30,1,1,14,1,1,30} },
    { '4', {2,6,10,18,31,2,2} },
    { '5', {31,16,16,30,1,1,30} },
    { '6', {14,16,16,30,17,17,14} },
    { '7', {31,1,2,4,8,8,8} },
    { '8', {14,17,17,14,17,17,14} },
    { '9', {14,17,17,15,1,1,14} },
    { '-', {0,0,0,31,0,0,0} },
    { '/', {1,2,2,4,8,8,16} },
    { '_', {0,0,0,0,0,0,31} },
    { '\'',{4,4,2,0,0,0,0} },
};

static const uint8_t *keyboard_glyph(char character)
{
    for (size_t i = 0; i < sizeof keyboard_font / sizeof *keyboard_font; i++)
        if (keyboard_font[i].character == character)
            return keyboard_font[i].row;
    return NULL;
}

static void keyboard_rect(int x, int y, int w, int h,
                          float r, float g, float b, float a,
                          const GLint *viewport,
                          void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                          void (*clear_color)(GLfloat, GLfloat, GLfloat,
                                              GLfloat),
                          void (*clear)(GLbitfield))
{
    int left = viewport[0] + x * viewport[2] / 1280;
    int right = viewport[0] + (x + w) * viewport[2] / 1280;
    int top = y * viewport[3] / 720;
    int bottom = (y + h) * viewport[3] / 720;
    if (left < viewport[0])
        left = viewport[0];
    if (right > viewport[0] + viewport[2])
        right = viewport[0] + viewport[2];
    if (top < 0)
        top = 0;
    if (bottom > viewport[3])
        bottom = viewport[3];
    if (right <= left || bottom <= top)
        return;
    scissor(left, viewport[1] + viewport[3] - bottom,
            right - left, bottom - top);
    clear_color(r, g, b, a);
    clear(GL_COLOR_BUFFER_BIT);
}

static int keyboard_text_width(const char *text, int scale)
{
    return text ? (int)strlen(text) * 6 * scale : 0;
}

static void keyboard_text(int x, int y, int scale, const char *text,
                          float r, float g, float b, float a,
                          const GLint *viewport,
                          void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                          void (*clear_color)(GLfloat, GLfloat, GLfloat,
                                              GLfloat),
                          void (*clear)(GLbitfield))
{
    if (!text)
        return;
    for (; *text; text++, x += 6 * scale) {
        if (*text == ' ')
            continue;
        const uint8_t *rows = keyboard_glyph(*text);
        if (!rows)
            continue;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < 5;) {
                while (col < 5 && !(bits & (1u << (4 - col))))
                    col++;
                int start = col;
                while (col < 5 && (bits & (1u << (4 - col))))
                    col++;
                if (start < col)
                    keyboard_rect(x + start * scale, y + row * scale,
                                  (col - start) * scale, scale,
                                  r, g, b, a, viewport,
                                  scissor, clear_color, clear);
            }
        }
    }
}

/* Controller keyboard modelled after the proven FF4 naming keyboard.  HGO is
 * GLES2, so this version uses only scissored clears: no shader, texture or
 * vertex state from Unity is replaced. */
static void draw_gamepad_keyboard(void)
{
    char typed[64];
    int uppercase = 0;
    int selected = 0;
    const bc_keyboard_key *keys = NULL;
    size_t key_count = 0;
    if (!bc_input_keyboard_snapshot(typed, sizeof typed, &uppercase,
                                     &selected, &keys, &key_count))
        return;

    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);
    static void (*scissor)(GLint, GLint, GLsizei, GLsizei);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear)(GLbitfield);
    if (!get_int) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
        scissor = gl_raw("glScissor");
        clear_color = gl_raw("glClearColor");
        color_mask = gl_raw("glColorMask");
        clear = gl_raw("glClear");
    }
    if (!get_int || !get_float || !get_bool || !is_enabled || !enable ||
        !disable || !scissor || !clear_color || !color_mask || !clear)
        return;

    GLint framebuffer = 0;
    GLint viewport[4] = { 0, 0, 1280, 720 };
    GLint old_scissor[4];
    GLfloat old_clear[4];
    GLboolean old_mask[4];
    GLboolean had_scissor = is_enabled(GL_SCISSOR_TEST);
    get_int(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0)
        return;
    get_int(GL_VIEWPORT, viewport);
    get_int(GL_SCISSOR_BOX, old_scissor);
    get_float(GL_COLOR_CLEAR_VALUE, old_clear);
    get_bool(GL_COLOR_WRITEMASK, old_mask);
    enable(GL_SCISSOR_TEST);
    color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

#define KRECT(x,y,w,h,r,g,b,a) \
    keyboard_rect((x),(y),(w),(h),(r),(g),(b),(a), viewport, \
                  scissor, clear_color, clear)
#define KTEXT(x,y,s,t,r,g,b,a) \
    keyboard_text((x),(y),(s),(t),(r),(g),(b),(a), viewport, \
                  scissor, clear_color, clear)

    /* Near-black frame, burgundy cabinet and cream highlights match HGO. */
    KRECT(132, 268, 1016, 452, 0.02f, 0.01f, 0.02f, 1.0f);
    KRECT(140, 276, 1000, 444, 0.50f, 0.08f, 0.14f, 1.0f);
    KRECT(148, 284, 984, 436, 0.035f, 0.025f, 0.035f, 1.0f);
    KTEXT(171, 290, 2, "ENTER NAME", 0.98f, 0.92f, 0.80f, 1.0f);

    KRECT(167, 306, 947, 62, 0.50f, 0.08f, 0.14f, 1.0f);
    KRECT(173, 312, 935, 50, 0.01f, 0.01f, 0.015f, 1.0f);
    char shown[66];
    snprintf(shown, sizeof shown, "%s_", typed);
    KTEXT(190, 323, 4, shown, 0.98f, 0.92f, 0.80f, 1.0f);

    for (size_t i = 0; i < key_count; i++) {
        const bc_keyboard_key *key = &keys[i];
        char dynamic_label[8];
        const char *label = key->label;
        if (key->action == BC_KEY_CHARACTER) {
            dynamic_label[0] = uppercase ? key->upper : key->lower;
            dynamic_label[1] = '\0';
            label = dynamic_label;
        } else if (key->action == BC_KEY_SHIFT) {
            snprintf(dynamic_label, sizeof dynamic_label, "%s",
                     uppercase ? "UPPER" : "LOWER");
            label = dynamic_label;
        }
        int highlighted = (int)i == selected ||
            (key->action == BC_KEY_SHIFT && uppercase);
        KRECT(key->x - 3, key->y - 3, key->w + 6, key->h + 6,
              highlighted ? 0.98f : 0.02f,
              highlighted ? 0.82f : 0.01f,
              highlighted ? 0.48f : 0.02f, 1.0f);
        KRECT(key->x, key->y, key->w, key->h,
              highlighted ? 0.65f : 0.30f,
              highlighted ? 0.10f : 0.055f,
              highlighted ? 0.16f : 0.095f, 1.0f);
        int scale = strlen(label) > 3 ? 2 : 3;
        int text_x =
            key->x + (key->w - keyboard_text_width(label, scale)) / 2;
        int text_y = key->y + (key->h - 7 * scale) / 2;
        KTEXT(text_x, text_y, scale, label,
              0.98f, 0.92f, 0.80f, 1.0f);
    }
    KTEXT(171, 650, 2, "R3/A SELECT  B DELETE  X SHIFT  START DONE",
          0.82f, 0.78f, 0.70f, 1.0f);

#undef KTEXT
#undef KRECT
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor[0], old_scissor[1],
            old_scissor[2], old_scissor[3]);
    if (!had_scissor)
        disable(GL_SCISSOR_TEST);
}

/* A small Hitman-style pixel cursor: cream face, near-black outline and
 * wine-red drop shadow.  It is drawn with scissored color clears immediately
 * before swap, avoiding shaders/textures and preserving Unity's GL program. */
static void draw_gamepad_cursor(void)
{
    float cursor_x, cursor_y;
    if (!bc_input_cursor(&cursor_x, &cursor_y))
        return;

    static const uint16_t outline[] = {
        0b0000000000000001, 0b0000000000000011,
        0b0000000000000111, 0b0000000000001111,
        0b0000000000011111, 0b0000000000111111,
        0b0000000001111111, 0b0000000011111111,
        0b0000000111111111, 0b0000001111111111,
        0b0000011111111111, 0b0000111111111111,
        0b0001111111111111, 0b0000000011111111,
        0b0000000111101111, 0b0000001111000111,
        0b0000001111000011, 0b0000011110000001,
        0b0000011110000000, 0b0000001100000000,
    };
    static const uint16_t fill[] = {
        0, 0, 0b0000000000000010, 0b0000000000000110,
        0b0000000000001110, 0b0000000000011110,
        0b0000000000111110, 0b0000000001111110,
        0b0000000011111110, 0b0000000111111110,
        0b0000001111111110, 0b0000011111111110,
        0b0000000111111110, 0b0000000000111110,
        0b0000000011000110, 0b0000000110000010,
        0b0000000110000000, 0b0000001100000000,
        0b0000001100000000, 0,
    };
    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);
    static void (*scissor)(GLint, GLint, GLsizei, GLsizei);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear)(GLbitfield);
    if (!get_int) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
        scissor = gl_raw("glScissor");
        clear_color = gl_raw("glClearColor");
        color_mask = gl_raw("glColorMask");
        clear = gl_raw("glClear");
    }
    if (!get_int || !get_float || !get_bool || !is_enabled || !enable ||
        !disable || !scissor || !clear_color || !color_mask || !clear)
        return;

    GLint framebuffer = 0;
    GLint viewport[4] = { 0, 0, 1280, 720 };
    GLint old_scissor[4];
    GLfloat old_clear[4];
    GLboolean old_mask[4];
    GLboolean had_scissor = is_enabled(GL_SCISSOR_TEST);
    get_int(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0)
        return;
    get_int(GL_VIEWPORT, viewport);
    get_int(GL_SCISSOR_BOX, old_scissor);
    get_float(GL_COLOR_CLEAR_VALUE, old_clear);
    get_bool(GL_COLOR_WRITEMASK, old_mask);

    int width = viewport[2];
    int height = viewport[3];
    bc_input_set_screen_size(width, height);
    int x = viewport[0] + (int)(cursor_x * width / 1280.0f);
    int y = (int)(cursor_y * height / 720.0f);
    int scale = width >= 1000 ? 2 : 1;
    enable(GL_SCISSOR_TEST);
    color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    clear_color(0.48f, 0.10f, 0.18f, 1.0f);
    clear_cursor_runs(outline, 20, x + 3, y + 3, scale, width, height,
                      scissor, clear);
    clear_color(0.035f, 0.020f, 0.035f, 1.0f);
    clear_cursor_runs(outline, 20, x, y, scale, width, height,
                      scissor, clear);
    clear_color(0.98f, 0.92f, 0.80f, 1.0f);
    clear_cursor_runs(fill, 20, x, y, scale, width, height,
                      scissor, clear);

    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor[0], old_scissor[1],
            old_scissor[2], old_scissor[3]);
    if (!had_scissor)
        disable(GL_SCISSOR_TEST);
}

static void remember_shader(GLuint shader, GLenum type)
{
    if (shader < sizeof shader_types / sizeof *shader_types)
        shader_types[shader] = type;
}

static const char *shader_stage(GLuint shader)
{
    GLenum type = shader < sizeof shader_types / sizeof *shader_types
                    ? shader_types[shader] : 0;
    return type == GL_VERTEX_SHADER ? "vertex"
         : type == GL_FRAGMENT_SHADER ? "fragment" : "unknown";
}

static GLuint my_glCreateShader(GLenum type)
{
    static GLuint (*real)(GLenum);
    if (!real)
        real = gl_raw("glCreateShader");
    GLuint shader = real(type);
    remember_shader(shader, type);
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] create %s shader=%u\n",
                type == GL_VERTEX_SHADER ? "vertex"
                : type == GL_FRAGMENT_SHADER ? "fragment" : "unknown",
                shader);
    return shader;
}

static void my_glShaderSource(GLuint shader, GLsizei count,
                              const GLchar *const *strings,
                              const GLint *lengths)
{
    static void (*real)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    if (!real)
        real = gl_raw("glShaderSource");
    shader_sources++;
    if (bc_trace_gl) {
        size_t total = 0;
        char preview[161];
        size_t used = 0;
        for (GLsizei i = 0; i < count; i++) {
            size_t n = lengths && lengths[i] >= 0
                         ? (size_t)lengths[i] : strlen(strings[i]);
            total += n;
            if (used < sizeof preview - 1) {
                size_t take = n;
                if (take > sizeof preview - 1 - used)
                    take = sizeof preview - 1 - used;
                memcpy(preview + used, strings[i], take);
                used += take;
            }
        }
        preview[used] = '\0';
        for (size_t i = 0; i < used; i++)
            if (preview[i] == '\n' || preview[i] == '\r')
                preview[i] = ' ';
        fprintf(stderr,
                "[bc/gl] source #%lu shader=%u stage=%s bytes=%zu: %.160s\n",
                shader_sources, shader, shader_stage(shader), total, preview);
        if (getenv("BC_GLSOURCE")) {
            fprintf(stderr, "[bc/gl/source-begin] shader=%u stage=%s\n",
                    shader, shader_stage(shader));
            for (GLsizei i = 0; i < count; i++) {
                size_t n = lengths && lengths[i] >= 0
                             ? (size_t)lengths[i] : strlen(strings[i]);
                fwrite(strings[i], 1, n, stderr);
            }
            fprintf(stderr, "\n[bc/gl/source-end] shader=%u\n", shader);
        }
    }
    real(shader, count, strings, lengths);
}

static void my_glCompileShader(GLuint shader)
{
    static void (*compile)(GLuint);
    static void (*getiv)(GLuint, GLenum, GLint *);
    static void (*getlog)(GLuint, GLsizei, GLsizei *, GLchar *);
    if (!compile)
        compile = gl_raw("glCompileShader");
    if (!getiv)
        getiv = gl_raw("glGetShaderiv");
    if (!getlog)
        getlog = gl_raw("glGetShaderInfoLog");
    compile(shader);
    shader_compiles++;
    GLint ok = GL_FALSE, loglen = 0;
    getiv(shader, GL_COMPILE_STATUS, &ok);
    getiv(shader, GL_INFO_LOG_LENGTH, &loglen);
    if (bc_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(shader, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr,
                "[bc/gl] compile #%lu shader=%u stage=%s ok=%d log=%s\n",
                shader_compiles, shader, shader_stage(shader), ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static void my_glLinkProgram(GLuint program)
{
    static void (*link)(GLuint);
    static void (*getiv)(GLuint, GLenum, GLint *);
    static void (*getlog)(GLuint, GLsizei, GLsizei *, GLchar *);
    if (!link)
        link = gl_raw("glLinkProgram");
    if (!getiv)
        getiv = gl_raw("glGetProgramiv");
    if (!getlog)
        getlog = gl_raw("glGetProgramInfoLog");
    link(program);
    program_links++;
    GLint ok = GL_FALSE, loglen = 0;
    getiv(program, GL_LINK_STATUS, &ok);
    getiv(program, GL_INFO_LOG_LENGTH, &loglen);
    if (bc_trace_gl || ok != GL_TRUE) {
        char log[2048] = "";
        if (loglen > 1) {
            GLsizei got = 0;
            getlog(program, sizeof log - 1, &got, log);
            if (got >= 0 && got < (GLsizei)sizeof log)
                log[got] = '\0';
        }
        fprintf(stderr, "[bc/gl] link #%lu program=%u ok=%d log=%s\n",
                program_links, program, ok == GL_TRUE,
                log[0] ? log : "(empty)");
    }
}

static void my_glUseProgram(GLuint program)
{
    static void (*real)(GLuint);
    current_program = program;
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] use-program %u\n", program);
    if (!real)
        real = gl_raw("glUseProgram");
    if (real)
        real(program);
}

static GLint my_glGetAttribLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetAttribLocation");
    GLint location = real ? real(program, name) : -1;
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] attrib program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glBindBuffer(GLenum target, GLuint buffer)
{
    static void (*real)(GLenum, GLuint);
    if (target == GL_ARRAY_BUFFER)
        bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        bound_element_array_buffer = buffer;
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] buffer-bind target=%#x buffer=%u\n",
                target, buffer);
    if (!real)
        real = gl_raw("glBindBuffer");
    if (real)
        real(target, buffer);
}

static void my_glBufferData(GLenum target, GLsizeiptr size, const void *data,
                            GLenum usage)
{
    static void (*real)(GLenum, GLsizeiptr, const void *, GLenum);
    static void (*sub)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (bc_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[bc/gl] buffer-data #%lu target=%#x buffer=%u bytes=%ld "
                "usage=%#x",
                buffer_uploads, target, buffer, (long)size, usage);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                    : target == GL_ELEMENT_ARRAY_BUFFER
                    ? bound_element_array_buffer : 0;
    dump_buffer_upload(target, buffer, 0, data, size, buffer_uploads);
    if (!real)
        real = gl_raw("glBufferData");
    if (!real)
        return;
    static int split_upload = -1;
    if (split_upload < 0)
        split_upload = bc_env_flag("BC_BUFFER_SPLIT_UPLOAD");
    if (split_upload && data && size > 0 &&
        usage == GL_DYNAMIC_DRAW) {
        if (!sub)
            sub = gl_raw("glBufferSubData");
        if (sub) {
            real(target, size, NULL, usage);
            sub(target, 0, size, data);
            if (bc_trace_gl)
                fprintf(stderr,
                        "[bc/gl] split buffer upload target=%#x "
                        "buffer=%u bytes=%ld\n",
                        target, buffer, (long)size);
            return;
        }
    }
    real(target, size, data, usage);
}

static void my_glBufferSubData(GLenum target, GLintptr offset,
                               GLsizeiptr size, const void *data)
{
    static void (*real)(GLenum, GLintptr, GLsizeiptr, const void *);
    buffer_uploads++;
    if (bc_trace_gl && buffer_uploads <= 160) {
        GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                        : target == GL_ELEMENT_ARRAY_BUFFER
                        ? bound_element_array_buffer : 0;
        fprintf(stderr,
                "[bc/gl] buffer-sub #%lu target=%#x buffer=%u offset=%ld "
                "bytes=%ld",
                buffer_uploads, target, buffer, (long)offset, (long)size);
        trace_buffer_payload(data, size);
        fputc('\n', stderr);
    }
    GLuint buffer = target == GL_ARRAY_BUFFER ? bound_array_buffer
                    : target == GL_ELEMENT_ARRAY_BUFFER
                    ? bound_element_array_buffer : 0;
    dump_buffer_upload(target, buffer, offset, data, size, buffer_uploads);
    if (!real)
        real = gl_raw("glBufferSubData");
    if (real)
        real(target, offset, size, data);
}

static void my_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                     GLboolean normalized, GLsizei stride,
                                     const void *pointer)
{
    static void (*real)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                        const void *);
    if (bc_trace_gl)
        fprintf(stderr,
                "[bc/gl] attrib-pointer index=%u size=%d type=%#x norm=%u "
                "stride=%d pointer=%p array-buffer=%u\n",
                index, size, type, !!normalized, stride, pointer,
                bound_array_buffer);
    if (!real)
        real = gl_raw("glVertexAttribPointer");
    if (real)
        real(index, size, type, normalized, stride, pointer);
}

static void my_glEnableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] attrib-enable index=%u\n", index);
    if (!real)
        real = gl_raw("glEnableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glDisableVertexAttribArray(GLuint index)
{
    static void (*real)(GLuint);
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] attrib-disable index=%u\n", index);
    if (!real)
        real = gl_raw("glDisableVertexAttribArray");
    if (real)
        real(index);
}

static void my_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                      GLenum textarget, GLuint texture,
                                      GLint level)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint, GLint);
    if (bc_trace_gl)
        fprintf(stderr,
                "[bc/gl] fbo-texture fbo=%u target=%#x attachment=%#x "
                "textarget=%#x texture=%u level=%d\n",
                gl_state.framebuffer, target, attachment, textarget, texture,
                level);
    if (!real)
        real = gl_raw("glFramebufferTexture2D");
    if (real)
        real(target, attachment, textarget, texture, level);
}

static void my_glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                         GLenum renderbuffer_target,
                                         GLuint renderbuffer)
{
    static void (*real)(GLenum, GLenum, GLenum, GLuint);
    if (bc_trace_gl)
        fprintf(stderr,
                "[bc/gl] fbo-renderbuffer fbo=%u target=%#x attachment=%#x "
                "renderbuffer-target=%#x renderbuffer=%u\n",
                gl_state.framebuffer, target, attachment,
                renderbuffer_target, renderbuffer);
    if (!real)
        real = gl_raw("glFramebufferRenderbuffer");
    if (real)
        real(target, attachment, renderbuffer_target, renderbuffer);
}

static void my_glRenderbufferStorage(GLenum target, GLenum internal_format,
                                     GLsizei width, GLsizei height)
{
    static void (*real)(GLenum, GLenum, GLsizei, GLsizei);
    if (bc_trace_gl)
        fprintf(stderr,
                "[bc/gl] renderbuffer-storage target=%#x internal=%#x "
                "size=%dx%d\n",
                target, internal_format, width, height);
    if (!real)
        real = gl_raw("glRenderbufferStorage");
    if (real)
        real(target, internal_format, width, height);
}

static GLenum my_glCheckFramebufferStatus(GLenum target)
{
    static GLenum (*real)(GLenum);
    if (!real)
        real = gl_raw("glCheckFramebufferStatus");
    GLenum status = real ? real(target) : 0;
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] fbo-status fbo=%u target=%#x status=%#x\n",
                gl_state.framebuffer, target, status);
    return status;
}

static void trace_current_fbo_status(void)
{
    if (!bc_trace_gl || gl_state.framebuffer == 0 || draw_calls > 20)
        return;
    static GLenum (*check)(GLenum);
    if (!check)
        check = gl_raw("glCheckFramebufferStatus");
    if (check)
        fprintf(stderr, "[bc/gl] pre-draw fbo=%u status=%#x\n",
                gl_state.framebuffer, check(GL_FRAMEBUFFER));
}

static GLint my_glGetUniformLocation(GLuint program, const GLchar *name)
{
    static GLint (*real)(GLuint, const GLchar *);
    if (!real)
        real = gl_raw("glGetUniformLocation");
    GLint location = real ? real(program, name) : -1;
    remember_uniform(program, location, name);
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] uniform program=%u location=%d name=%s\n",
                program, location, name ? name : "(null)");
    return location;
}

static void my_glUniform1i(GLint location, GLint value)
{
    static void (*real)(GLint, GLint);
    if (!real)
        real = gl_raw("glUniform1i");
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] uniform1i location=%d name=%s value=%d\n",
                location, uniform_label(location), value);
    if (real)
        real(location, value);
}

static void my_glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z,
                           GLfloat w)
{
    static void (*real)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    if (!real)
        real = gl_raw("glUniform4f");
    if (bc_trace_gl)
        fprintf(stderr,
                "[bc/gl] uniform4f location=%d name=%s "
                "value=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), x, y, z, w);
    if (real)
        real(location, x, y, z, w);
}

static void my_glUniform4fv(GLint location, GLsizei count,
                            const GLfloat *value)
{
    static void (*real)(GLint, GLsizei, const GLfloat *);
    if (!real)
        real = gl_raw("glUniform4fv");
    if (bc_trace_gl && value && count > 0) {
        fprintf(stderr,
                "[bc/gl] uniform4fv location=%d name=%s count=%d "
                "first=%.9g,%.9g,%.9g,%.9g\n",
                location, uniform_label(location), count,
                value[0], value[1], value[2], value[3]);
        if (count == 4)
            fprintf(stderr,
                    "[bc/gl] uniform4fv-matrix location=%d name=%s "
                    "rows=[%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g] "
                    "[%.9g %.9g %.9g %.9g] [%.9g %.9g %.9g %.9g]\n",
                    location, uniform_label(location),
                    value[0], value[1], value[2], value[3],
                    value[4], value[5], value[6], value[7],
                    value[8], value[9], value[10], value[11],
                    value[12], value[13], value[14], value[15]);
    }
    if (real)
        real(location, count, value);
}

static void my_glActiveTexture(GLenum texture)
{
    static void (*real)(GLenum);
    active_texture_unit = texture;
    if (!real)
        real = gl_raw("glActiveTexture");
    if (real)
        real(texture);
}

static void my_glBindTexture(GLenum target, GLuint texture)
{
    static void (*real)(GLenum, GLuint);
    texture_binds++;
    if (target == GL_TEXTURE_2D)
        bound_texture_2d = texture;
    if (bc_trace_gl && texture_binds <= 160)
        fprintf(stderr,
                "[bc/gl] tex-bind #%lu unit=%#x target=%#x texture=%u\n",
                texture_binds, active_texture_unit, target, texture);
    if (!real)
        real = gl_raw("glBindTexture");
    if (real)
        real(target, texture);
}

static void my_glPixelStorei(GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLint);
    if (bc_trace_gl)
        fprintf(stderr, "[bc/gl] pixel-store pname=%#x value=%d\n",
                pname, value);
    if (!real)
        real = gl_raw("glPixelStorei");
    if (real)
        real(pname, value);
}

static void my_glTexParameteri(GLenum target, GLenum pname, GLint value)
{
    static void (*real)(GLenum, GLenum, GLint);
    texture_parameters++;
    if (bc_trace_gl && texture_parameters <= 240)
        fprintf(stderr,
                "[bc/gl] tex-param #%lu unit=%#x texture=%u target=%#x "
                "pname=%#x value=%#x\n",
                texture_parameters, active_texture_unit, bound_texture_2d,
                target, pname, value);
    if (!real)
        real = gl_raw("glTexParameteri");
    if (real)
        real(target, pname, value);
}

static void my_glTexImage2D(GLenum target, GLint level, GLint internal_format,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                        GLenum, const void *);
    texture_images++;
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[bc/gl] tex-image #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d border=%d format=%#x "
                "type=%#x data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, border, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexImage2D");
    if (real)
        real(target, level, internal_format, width, height, border, format,
             type, data);
}

static void my_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                               GLsizei width, GLsizei height, GLenum format,
                               GLenum type, const void *data)
{
    static void (*real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                        GLenum, const void *);
    texture_sub_images++;
    if (trace_texture_call(texture_sub_images, width, height))
        fprintf(stderr,
                "[bc/gl] tex-sub #%lu unit=%#x texture=%u target=%#x "
                "level=%d xy=%d,%d size=%dx%d format=%#x type=%#x data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, type,
                data ? "set" : "null");
    if (!real)
        real = gl_raw("glTexSubImage2D");
    if (real)
        real(target, level, x, y, width, height, format, type, data);
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real)
        real = gl_raw("glDrawArrays");
    draw_calls++;
    if (bc_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[bc/gl] draw #%lu arrays mode=%#x first=%d count=%d "
                "program=%u fbo=%u vbo=%u\n",
                draw_calls, mode, first, count, current_program,
                gl_state.framebuffer, bound_array_buffer);
    trace_current_fbo_status();
    real(mode, first, count);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real)
        real = gl_raw("glDrawElements");
    draw_calls++;
    if (bc_trace_gl && (draw_calls <= 20 || draw_calls % 1000 == 0))
        fprintf(stderr,
                "[bc/gl] draw #%lu elements mode=%#x count=%d type=%#x "
                "indices=%p program=%u fbo=%u vbo=%u ebo=%u\n",
                draw_calls, mode, count, type, indices, current_program,
                gl_state.framebuffer, bound_array_buffer,
                bound_element_array_buffer);
    trace_current_fbo_status();
    real(mode, count, type, indices);
}

static int native_etc2_support(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    const GLubyte *(*get_string)(GLenum) = gl_raw("glGetString");
    const char *version = get_string ? (const char *)get_string(GL_VERSION) : NULL;
    const char *extensions = get_string
                           ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    known = (version && strstr(version, "OpenGL ES 3")) ||
            (extensions && (strstr(extensions, "GL_OES_compressed_ETC2") ||
                            strstr(extensions, "GL_ARB_ES3_compatibility")));
    nx_log("ETC2 uploads: %s", known ? "native" : "software RGBA fallback");
    return known;
}

static int is_etc2(GLenum format)
{
    return format >= 0x9274 && format <= 0x9279;
}

static void my_glCompressedTexImage2D(GLenum target, GLint level,
                                      GLenum internal_format, GLsizei width,
                                      GLsizei height, GLint border,
                                      GLsizei image_size, const void *data)
{
    static void (*compressed)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                              GLsizei, const void *);
    static void (*plain)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                         GLenum, const void *);
    texture_images++;
    if (trace_texture_call(texture_images, width, height))
        fprintf(stderr,
                "[bc/gl] tex-compressed #%lu unit=%#x texture=%u target=%#x "
                "level=%d internal=%#x size=%dx%d bytes=%d data=%s\n",
                texture_images, active_texture_unit, bound_texture_2d, target,
                level, internal_format, width, height, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexImage2D");
    if (!plain) plain = gl_raw("glTexImage2D");
    if (data && is_etc2(internal_format) && !native_etc2_support()) {
        unsigned char *rgba = etc2_decode_rgba(internal_format, width, height,
                                                data, image_size);
        if (rgba && plain) {
            plain(target, level, GL_RGBA, width, height, border, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
    if (compressed)
        compressed(target, level, internal_format, width, height, border,
                   image_size, data);
}

static void my_glCompressedTexSubImage2D(GLenum target, GLint level,
                                         GLint x, GLint y, GLsizei width,
                                         GLsizei height, GLenum format,
                                         GLsizei image_size, const void *data)
{
    static void (*compressed)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
                              GLenum, GLsizei, const void *);
    static void (*plain)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                         GLenum, const void *);
    texture_sub_images++;
    if (trace_texture_call(texture_sub_images, width, height))
        fprintf(stderr,
                "[bc/gl] tex-compressed-sub #%lu unit=%#x texture=%u "
                "target=%#x level=%d xy=%d,%d size=%dx%d format=%#x "
                "bytes=%d data=%s\n",
                texture_sub_images, active_texture_unit, bound_texture_2d,
                target, level, x, y, width, height, format, image_size,
                data ? "set" : "null");
    if (!compressed) compressed = gl_raw("glCompressedTexSubImage2D");
    if (!plain) plain = gl_raw("glTexSubImage2D");
    if (data && is_etc2(format) && !native_etc2_support()) {
        unsigned char *rgba = etc2_decode_rgba(format, width, height, data,
                                                image_size);
        if (rgba && plain) {
            plain(target, level, x, y, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            return;
        }
        free(rgba);
    }
    if (compressed)
        compressed(target, level, x, y, width, height, format, image_size,
                   data);
}

/* glGetString interceptado pra reportar GPU conhecida -> libunity computa
 * graphicsMemorySize > 0 -> PluginDatabase.OnEnable nao divide por zero. */
static const GLubyte *(*real_glGetString)(GLenum);
static GLubyte *fake_renderer = (GLubyte *)"Adreno (TM) 650";
static const GLubyte *my_glGetString(GLenum name)
{
    if (!real_glGetString)
        real_glGetString = gl_raw("glGetString");
    if (!real_glGetString)
        return NULL;
    if (name == 0x1F01 /* GL_RENDERER */) {
        nx_log("glGetString(GL_RENDERER) -> \"%s\" (era Mali-450)", fake_renderer);
        return fake_renderer;
    }
    return real_glGetString(name);
}

void *bc_gl_sym(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "glGetString") == 0)
        return my_glGetString;
    if (strcmp(name, "glBindFramebuffer") == 0 ||
        strcmp(name, "glBindFramebufferOES") == 0)
        return my_glBindFramebuffer;
    if (strcmp(name, "glDeleteFramebuffers") == 0 ||
        strcmp(name, "glDeleteFramebuffersOES") == 0)
        return my_glDeleteFramebuffers;
    if (strcmp(name, "glColorMask") == 0)
        return my_glColorMask;
    if (strcmp(name, "glClearColor") == 0)
        return my_glClearColor;
    if (strcmp(name, "glEnable") == 0)
        return my_glEnable;
    if (strcmp(name, "glDisable") == 0)
        return my_glDisable;
    if (strcmp(name, "glCreateShader") == 0)
        return my_glCreateShader;
    if (strcmp(name, "glShaderSource") == 0)
        return my_glShaderSource;
    if (strcmp(name, "glCompileShader") == 0)
        return my_glCompileShader;
    if (strcmp(name, "glLinkProgram") == 0)
        return my_glLinkProgram;
    if (strcmp(name, "glUseProgram") == 0)
        return my_glUseProgram;
    if (strcmp(name, "glGetAttribLocation") == 0)
        return my_glGetAttribLocation;
    if (strcmp(name, "glBindBuffer") == 0)
        return my_glBindBuffer;
    if (strcmp(name, "glBufferData") == 0)
        return my_glBufferData;
    if (strcmp(name, "glBufferSubData") == 0)
        return my_glBufferSubData;
    if (strcmp(name, "glVertexAttribPointer") == 0)
        return my_glVertexAttribPointer;
    if (strcmp(name, "glEnableVertexAttribArray") == 0)
        return my_glEnableVertexAttribArray;
    if (strcmp(name, "glDisableVertexAttribArray") == 0)
        return my_glDisableVertexAttribArray;
    if (strcmp(name, "glFramebufferTexture2D") == 0)
        return my_glFramebufferTexture2D;
    if (strcmp(name, "glFramebufferRenderbuffer") == 0)
        return my_glFramebufferRenderbuffer;
    if (strcmp(name, "glRenderbufferStorage") == 0)
        return my_glRenderbufferStorage;
    if (strcmp(name, "glCheckFramebufferStatus") == 0)
        return my_glCheckFramebufferStatus;
    if (strcmp(name, "glGetUniformLocation") == 0)
        return my_glGetUniformLocation;
    if (strcmp(name, "glUniform1i") == 0)
        return my_glUniform1i;
    if (strcmp(name, "glUniform4f") == 0)
        return my_glUniform4f;
    if (strcmp(name, "glUniform4fv") == 0)
        return my_glUniform4fv;
    if (strcmp(name, "glActiveTexture") == 0)
        return my_glActiveTexture;
    if (strcmp(name, "glBindTexture") == 0)
        return my_glBindTexture;
    if (strcmp(name, "glPixelStorei") == 0)
        return my_glPixelStorei;
    if (strcmp(name, "glTexParameteri") == 0)
        return my_glTexParameteri;
    if (strcmp(name, "glTexImage2D") == 0)
        return my_glTexImage2D;
    if (strcmp(name, "glTexSubImage2D") == 0)
        return my_glTexSubImage2D;
    if (strcmp(name, "glDrawArrays") == 0)
        return my_glDrawArrays;
    if (strcmp(name, "glDrawElements") == 0)
        return my_glDrawElements;
    if (strcmp(name, "glCompressedTexImage2D") == 0)
        return my_glCompressedTexImage2D;
    if (strcmp(name, "glCompressedTexSubImage2D") == 0)
        return my_glCompressedTexSubImage2D;
    return gl_raw(name);
}

#define SYS(name, ret, ...) \
    static ret (*p_##name)(__VA_ARGS__); \
    static ret r_##name(__VA_ARGS__)

/* --- the two calls we rewrite ------------------------------------------- */

static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                       EGLint, EGLint *);

static EGLBoolean my_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib,
                                     EGLConfig *cfgs, EGLint n, EGLint *num)
{
    EGLint copy[64];
    size_t k = 0;
    int patched = 0;
    int saw_stencil = 0;
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 58; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_RENDERABLE_TYPE && (val & EGL_OPENGL_ES3_BIT_KHR)) {
                val = (val & ~EGL_OPENGL_ES3_BIT_KHR) | EGL_OPENGL_ES2_BIT;
                patched = 1;
            }
            if (name == EGL_STENCIL_SIZE) {
                saw_stencil = 1;
                if (val < 8) { val = 8; patched = 1; }
            }
            nx_log("eglChooseConfig: pedido 0x%04x = %d", (unsigned)name, (int)val);
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    /* A UI da Unity desenha painel/mascara com STENCIL (o shader de texto tem
     * StencilComp/StencilID).  Sem bits de stencil no config, todo elemento
     * mascarado some — foi o que deixou o menu de pause invisivel neste port
     * (NextOS, 07/08/2026: "nao era pra ter menu no meio da tela?").
     * BC_NO_FORCE_STENCIL=1 desliga o forcamento. */
    if (!saw_stencil && k < 60 &&
        !(getenv("BC_NO_FORCE_STENCIL") &&
          strcmp(getenv("BC_NO_FORCE_STENCIL"), "0") != 0)) {
        copy[k++] = EGL_STENCIL_SIZE;
        copy[k++] = 8;
        patched = 1;
        nx_log("eglChooseConfig: STENCIL 8 forcado (a UI da Unity precisa)");
    }
    copy[k] = EGL_NONE;
    if (patched)
        nx_log("eglChooseConfig: atributos ajustados");
    if (!p_eglChooseConfig)
        p_eglChooseConfig = sys("eglChooseConfig");
    EGLBoolean ok = p_eglChooseConfig(dpy, attrib ? copy : NULL, cfgs, n, num);
    /* prova por medida: quantos bits de stencil o config escolhido tem */
    if (ok && num && *num > 0 && cfgs) {
        EGLint st = -1, dp = -1;
        EGLBoolean (*getattr_)(EGLDisplay, EGLConfig, EGLint, EGLint *) =
            sys("eglGetConfigAttrib");
        if (getattr_) {
            getattr_(dpy, cfgs[0], EGL_STENCIL_SIZE, &st);
            getattr_(dpy, cfgs[0], EGL_DEPTH_SIZE, &dp);
            nx_log("eglChooseConfig: config escolhido stencil=%d depth=%d",
                   (int)st, (int)dp);
        }
    }
    return ok;
}

static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);

static EGLContext my_eglCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const EGLint *attrib)
{
    EGLint copy[32];
    size_t k = 0;
    int downgraded = 0;
    if (attrib) {
        for (const EGLint *a = attrib; *a != EGL_NONE && k < 28; a += 2) {
            EGLint name = a[0], val = a[1];
            if (name == EGL_CONTEXT_CLIENT_VERSION && val > 2) {
                val = 2;
                downgraded = 1;
            }
            if (name == EGL_CONTEXT_MINOR_VERSION_KHR)
                continue;                       /* ES2 has no minor version */
            copy[k++] = name;
            copy[k++] = val;
        }
    }
    copy[k] = EGL_NONE;
    if (downgraded)
        nx_log("eglCreateContext: ES3 context requested, created ES2");
    if (!p_eglCreateContext)
        p_eglCreateContext = sys("eglCreateContext");
    return p_eglCreateContext(dpy, cfg, share, attrib ? copy : NULL);
}

/* Unity asks for the surface it should render into.  On fbdev the native
 * window is the framebuffer itself, so hand the driver what it expects rather
 * than our ANativeWindow shim. */
static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig,
                                              EGLNativeWindowType,
                                              const EGLint *);

static EGLSurface my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                            EGLNativeWindowType win,
                                            const EGLint *attrib)
{
    extern void *bc_native_window(void);
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)bc_native_window();
    nx_log("eglCreateWindowSurface: game win=%p -> native %p", (void *)win,
           (void *)real);
    return p_eglCreateWindowSurface(dpy, cfg, real, attrib);
}

static EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);

static EGLDisplay my_eglGetDisplay(EGLNativeDisplayType d)
{
    (void)d;
    if (!p_eglGetDisplay)
        p_eglGetDisplay = sys("eglGetDisplay");
    return p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);

/* One-shot diagnostic for the raw Mali/fbdev path.  The SDL-owned backend has
 * the equivalent hook in egl_sdl.c; keeping this immediately before the real
 * swap captures exactly what Unity is about to present. */
extern void *bc_native_window(void);

static void capture_raw_frame_if_requested(void)
{
    static int finished;
    unsigned long frame = ++bc_swap_count;

    static const char *path;
    static int path_known;
    if (!path_known) {
        path = getenv("BC_SCREENSHOT");
        path_known = 1;
    }
    if (!path || !*path)
        return;

    int live_mode = getenv("BC_SCREENSHOT_LIVE") &&
                    strcmp(getenv("BC_SCREENSHOT_LIVE"), "0") != 0;
    int live_request = live_mode && access("/tmp/bcshot", F_OK) == 0;
    if (live_mode) {
        if (!live_request)
            return;
        unlink("/tmp/bcshot");
    } else if (finished) {
        return;
    }

    if (!live_mode) {
        long wanted = 300;
        const char *value = getenv("BC_SCREENSHOT_FRAME");
        if (value && *value) {
            char *end = NULL;
            long parsed = strtol(value, &end, 10);
            if (end && *end == '\0' && parsed > 0)
                wanted = parsed;
        }
        if (frame < (unsigned long)wanted)
            return;
        finished = 1;
    }

    void (*get_int)(GLenum, GLint *) = gl_raw("glGetIntegerv");
    void (*pixel_store)(GLenum, GLint) = gl_raw("glPixelStorei");
    void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        void *) = gl_raw("glReadPixels");
    if (!get_int || !pixel_store || !read_pixels) {
        nx_log("screenshot: required GLES functions are missing");
        return;
    }

    /* Unity leaves whatever offscreen FBO it used last bound when the frame
     * ends; reading that instead of the presented buffer reports a false
     * black screen.  Bind the default framebuffer for the read and restore
     * the game's binding afterwards. */
    void (*bind_fb)(GLenum, GLuint) = gl_raw("glBindFramebuffer");
    GLint prev_fb = 0;
    get_int(GL_FRAMEBUFFER_BINDING, &prev_fb);
    if (bind_fb && prev_fb != 0)
        bind_fb(GL_FRAMEBUFFER, 0);

    GLint viewport[4] = { 0, 0, 0, 0 };
    get_int(GL_VIEWPORT, viewport);
    int width = viewport[2];
    int height = viewport[3];
    if (width <= 0 || height <= 0) {
        unsigned short *win = (unsigned short *)bc_native_window();
        if (win) { width = win[0]; height = win[1]; }
    }
    nx_log("screenshot: fbo at swap = %d (reading 0), viewport %dx%d",
           prev_fb, width, height);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        nx_log("screenshot: invalid viewport %dx%d", width, height);
        return;
    }

    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 4) {
        nx_log("screenshot: viewport size overflow");
        return;
    }
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
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    pixel_store(GL_PACK_ALIGNMENT, old_pack);
    if (bind_fb && prev_fb != 0)
        bind_fb(GL_FRAMEBUFFER, (GLuint)prev_fb);

    size_t nonblack = 0;
    size_t alpha_zero = 0;
    for (int y = 0; y < height; y++) {
        const unsigned char *source =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        unsigned char *dest = rgb + (size_t)y * (size_t)width * 3;
        for (int x = 0; x < width; x++) {
            unsigned char red = source[x * 4 + 0];
            unsigned char green = source[x * 4 + 1];
            unsigned char blue = source[x * 4 + 2];
            unsigned char alpha = source[x * 4 + 3];
            dest[x * 3 + 0] = red;
            dest[x * 3 + 1] = green;
            dest[x * 3 + 2] = blue;
            if ((unsigned)red + green + blue > 24)
                nonblack++;
            if (alpha == 0)
                alpha_zero++;
        }
    }

    FILE *output = fopen(path, "wb");
    if (!output) {
        nx_log("screenshot: cannot open %s", path);
    } else {
        fprintf(output, "P6\n%d %d\n255\n", width, height);
        size_t written = fwrite(rgb, 3, pixels, output);
        if (fclose(output) == 0 && written == pixels) {
            nx_log("screenshot: frame %lu -> %s (%zu/%zu nonblack, "
                   "%zu alpha-zero)",
                   frame, path, nonblack, pixels, alpha_zero);
        } else {
            nx_log("screenshot: incomplete write to %s", path);
        }
    }
    free(rgb);
    free(rgba);
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    draw_gamepad_keyboard();
    draw_gamepad_cursor();
    if (bc_sdl_video_active())
        return bc_sdl_swap_buffers(display, surface);
    capture_raw_frame_if_requested();
    force_opaque_backbuffer();
    if (!p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");
    return p_eglSwapBuffers(display, surface);
}

static void *my_eglGetProcAddress(const char *name)
{
    if (!name)
        return NULL;
    if (name[0] == 'g' && name[1] == 'l')
        return bc_gl_sym(name);
    if (bc_sdl_video_active())
        return bc_sdl_egl_proc(name);
    static void *(*real)(const char *);
    if (!real)
        real = sys("eglGetProcAddress");
    return real ? real(name) : NULL;
}

/* --- everything else is a straight forward ------------------------------ */

/* A trampoline table beats writing 20 wrappers: the calls we do not rewrite
 * are resolved to the system symbol directly at start-up. */
static const char *const passthrough[] = {
    "eglInitialize", "eglTerminate", "eglGetConfigs", "eglGetConfigAttrib",
    "eglCreatePbufferSurface", "eglDestroySurface", "eglQuerySurface",
    "eglBindAPI", "eglQueryAPI", "eglDestroyContext", "eglMakeCurrent",
    "eglGetCurrentContext", "eglGetCurrentSurface", "eglGetCurrentDisplay",
    "eglQueryContext", "eglGetError",
    "eglQueryString", "eglSurfaceAttrib", "eglSwapInterval",
    "eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative",
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
    "eglPresentationTimeANDROID", "eglDupNativeFenceFDANDROID",
};

#define MAXTAB (sizeof passthrough / sizeof *passthrough + 8)
static nx_import tab[MAXTAB];
static size_t tab_n;

void bc_egl_init(void)
{
    /* SDL owns KMS/Wayland contexts; Mali fbdev stays on the vendor EGL path,
     * matching the validated Horizon Chase backend split. */
    int sdl_owned = bc_capture_mode ? 0 : bc_sdl_video_init();
    tab_n = 0;
    tab[tab_n++] = (nx_import){ "eglChooseConfig",
        sdl_owned ? bc_sdl_egl_proc("eglChooseConfig")
                  : (void *)my_eglChooseConfig };
    tab[tab_n++] = (nx_import){ "eglCreateContext",
        sdl_owned ? bc_sdl_egl_proc("eglCreateContext")
                  : (void *)my_eglCreateContext };
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
        sdl_owned ? bc_sdl_egl_proc("eglCreateWindowSurface")
                  : (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay",
        sdl_owned ? bc_sdl_egl_proc("eglGetDisplay")
                  : (void *)my_eglGetDisplay };
    tab[tab_n++] = (nx_import){ "eglSwapBuffers",        (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",     (void *)my_eglGetProcAddress };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sdl_owned ? bc_sdl_egl_proc(passthrough[i])
                            : dlsym(RTLD_DEFAULT, passthrough[i]);
        if (!f && !sdl_owned)
            f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (%s ownership)", tab_n,
           sdl_owned ? "SDL" : "raw");
}

const nx_import *bc_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *bc_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return bc_gl_sym(name);
}
