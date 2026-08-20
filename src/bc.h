/* bc.h -- shared declarations for the Bomb Chicken port. */

#ifndef BC_H
#define BC_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char bc_gamedir[1024];
extern char bc_datadir[1024];   /* <gamedir>/assets */
extern char bc_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char bc_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int bc_log_level;    /* BC_LOGCAT   : mirror the game's own log     */
extern int bc_trace_jni;    /* BC_JNILOG   : every JNI call                */
extern int bc_trace_gl;     /* BC_GLLOG    : GL calls and shader sources   */
extern long bc_max_frames;  /* BC_FRAMES=N : stop after N frames           */
extern int bc_capture_mode; /* always zero; retained by the EGL abstraction */

void bc_bionic_init(void);
size_t bc_bionic_count(void);
void bc_pthread_init(void);
void bc_android_init(void);
void bc_egl_init(void);
void bc_jni_init(void);

void *bc_android_sym(const char *name);
void *bc_egl_sym(const char *name);
void *bc_gl_sym(const char *name);
void *bc_jni_sym(const char *name);
void *bc_jni_env(void);
void *bc_jni_vm(void);
void *bc_jni_activity(void);
void *bc_jni_native(const char *cls, const char *name);
void *bc_jret_obj(const char *cls);
void *bc_jret_class(const char *cls);
void *bc_jret_str(const char *text);
void bc_jni_set_unity_player(void *player);
void bc_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *bc_jni_key_event(int action, int keycode, int scancode);
void *bc_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *bc_jni_touch_event(int action, float x, float y);
void *bc_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *bc_jni_fmod_device(void);
void *bc_jni_fmod_bytebuffer(void);
void *bc_jni_fmod_pcm(void);
int bc_jni_fmod_pcm_capacity(void);
void bc_jni_fmod_set_buffer_size(int bytes);
int bc_jni_fmod_should_run(void);
int bc_audio_start(void *env);
void bc_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int bc_input_init(void);
void bc_input_poll(void *env, void *player, unsigned long frame);
void bc_input_close(void);
int bc_input_exit_requested(void);
void bc_input_request_exit(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int bc_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void bc_input_set_screen_size(int width, int height);

enum {
    BC_KEY_CHARACTER,
    BC_KEY_BACKSPACE,
    BC_KEY_SHIFT,
    BC_KEY_SPACE,
    BC_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} bc_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void bc_input_keyboard_open(const char *initial, int character_limit);
void bc_input_keyboard_set(const char *text);
void bc_input_keyboard_hide(void);
int bc_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const bc_keyboard_key **keys,
                                size_t *key_count);
void bc_jni_soft_input_text(const char *text);
void bc_jni_soft_input_selection(int start, int length);
void bc_jni_soft_input_visible(int visible);
void bc_jni_soft_input_closed(int canceled);

/* PlayerPrefs do jogo, para o conserto do fim de fase (input.c). */
int bc_prefs_get_string(const char *key, char *out, size_t size);
int bc_prefs_set_string(const char *key, const char *value);

/* The three arm64 objects, in load order. */
int bc_load_modules(void);
void bc_arm_frame_watchdog(void);
void bc_watchdog_frame(void);

int bc_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* BC_H */
