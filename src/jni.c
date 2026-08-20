/*
 * jni.c -- the JNIEnv the Android objects talk to.
 *
 * The vtable is built by index rather than by naming the fields of
 * JNINativeInterface: the port has to match the real slot numbers exactly
 * because Unity and its Android plugins call through them by offset.  Keeping
 * an index table makes every slot checkable against the disassembly.
 *
 * Objects are small tagged structs, not opaque cookies, so GetObjectClass and
 * IsInstanceOf can tell a KeyEvent from a MotionEvent from a byte[] -- Unity
 * needs that distinction for its native Android input and asset paths.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "nx_elf.h"
#include "bc.h"
#include "jni_slots.h"

int bc_trace_jni = 0;

#define JT(...) do { if (bc_trace_jni) nx_log("jni: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------- object model */

typedef enum {
    O_CLASS, O_STRING, O_BYTEARRAY, O_OBJARRAY, O_OBJECT, O_THROWABLE,
    O_DIRECTBUF,
} otype;

typedef struct jobj {
    otype type;
    const char *cls;         /* class name for O_OBJECT / O_CLASS */
    char *str;               /* O_STRING payload (UTF-8, owned) */
    void *data;              /* array payload */
    int len;
    struct jobj **elems;     /* O_OBJARRAY */
    int64_t prim;            /* payload of a boxed primitive */
    int boxed;
} jobj;

static jobj *new_obj(otype t, const char *cls)
{
    jobj *o = calloc(1, sizeof *o);
    o->type = t;
    o->cls = cls;
    return o;
}

static jobj *mk_class(const char *name)
{
    /* Classes are interned so IsSameObject and the method tables work. */
    static jobj *cache[256];
    static const char *names[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return cache[i];
    if (n == 256)
        nx_die("class cache full (%s)", name);
    names[n] = strdup(name);
    cache[n] = new_obj(O_CLASS, names[n]);
    return cache[n++];
}

static jobj *mk_string(const char *s)
{
    jobj *o = new_obj(O_STRING, "java/lang/String");
    o->str = strdup(s ? s : "");
    o->len = (int)strlen(o->str);
    return o;
}

static jobj *mk_object(const char *cls)
{
    return new_obj(O_OBJECT, cls);
}

static jobj *mk_file(const char *path)
{
    jobj *o = mk_object("java/io/File");
    o->str = strdup(path ? path : "");
    o->len = (int)strlen(o->str);
    return o;
}

/* Objects published to Unity's Android input backend. */
static jobj *input_device;
static jobj *touch_device;
static jobj *motion_range_list;
static jobj *touch_motion_range_list;
static jobj *motion_range_iterator;
static jobj *motion_ranges[8];
static jobj *key_event_object;
static jobj *motion_event_object;
static jobj *fmod_device_object;
static jobj *fmod_bytebuffer;
static jobj *preferences_object;
static jobj *preferences_editor;
static jobj *armory_activity_object;
static jobj *permission_plugin_object;
static unsigned char fmod_pcm[65536];
static int fmod_buffer_size = sizeof fmod_pcm;
static int fmod_should_run;
static char input_device_name[128] = "NextOS Gamepad";
static char input_device_descriptor[160] = "nextos-native-gamepad";
static int input_device_vendor;
static int input_device_product;
static struct {
    int action;
    int keycode;
    int source;
    int device_id;
    int meta_state;
    int repeat;
    int scancode;
    int flags;
    int unicode;
    int64_t event_time;
    int64_t down_time[256];
} key_event;
typedef struct {
    int action;
    int source;
    int device_id;
    int meta_state;
    int button_state;
    int flags;
    int64_t event_time;
    int64_t down_time;
    float axis[48];
} motion_payload;
static motion_payload motion_event;
#define MOTION_CLONE_COUNT 64
static jobj *motion_clones[MOTION_CLONE_COUNT];
static motion_payload motion_clone_data[MOTION_CLONE_COUNT];
static unsigned motion_clone_next;

static const int motion_axis_ids[8] = {
    0, 1, 11, 14, 17, 18, 15, 16,
};

static motion_payload *motion_from_object(jobj *object)
{
    return object && object->data
        ? (motion_payload *)object->data : &motion_event;
}

static int64_t monotonic_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void bc_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor)
{
    snprintf(input_device_name, sizeof input_device_name, "%s",
             name && *name ? name : "NextOS Gamepad");
    snprintf(input_device_descriptor, sizeof input_device_descriptor,
             "nextos-%04x-%04x-%s", vendor & 0xffff, product & 0xffff,
             descriptor && *descriptor ? descriptor : "gamepad");
    input_device_vendor = vendor;
    input_device_product = product;
}

void *bc_jni_key_event(int action, int keycode, int scancode)
{
    int64_t now = monotonic_millis();
    if (keycode < 0 || keycode >= (int)(sizeof key_event.down_time /
                                        sizeof *key_event.down_time))
        keycode = 0;
    if (action == 0 || key_event.down_time[keycode] == 0)
        key_event.down_time[keycode] = now;
    key_event.action = action;
    key_event.keycode = keycode;
    /* Android reports these devices as GAMEPAD | DPAD | JOYSTICK. */
    key_event.source = 0x01000611;
    key_event.device_id = 1;
    key_event.meta_state = 0;
    key_event.repeat = 0;
    key_event.scancode = scancode;
    key_event.flags = 0;
    key_event.unicode = 0;
    key_event.event_time = now;
    if (action == 1) {
        /* nativeInjectEvent consumes the object synchronously, so the slot can
         * be released as soon as this call returns to the input bridge. */
    }
    return key_event_object;
}

void *bc_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y)
{
    int64_t now = monotonic_millis();
    motion_event.action = 2; /* MotionEvent.ACTION_MOVE */
    motion_event.source = 0x01000010; /* InputDevice.SOURCE_JOYSTICK */
    motion_event.device_id = 1;
    motion_event.meta_state = 0;
    motion_event.button_state = 0;
    motion_event.flags = 0;
    motion_event.event_time = now;
    motion_event.down_time = now;
    memset(motion_event.axis, 0, sizeof motion_event.axis);
    motion_event.axis[0] = lx;   /* AXIS_X */
    motion_event.axis[1] = ly;   /* AXIS_Y */
    motion_event.axis[11] = rx;  /* AXIS_Z */
    motion_event.axis[14] = ry;  /* AXIS_RZ */
    motion_event.axis[17] = lt;  /* AXIS_LTRIGGER */
    motion_event.axis[18] = rt;  /* AXIS_RTRIGGER */
    motion_event.axis[15] = hat_x;
    motion_event.axis[16] = hat_y;
    return motion_event_object;
}

void *bc_jni_touch_event(int action, float x, float y)
{
    int64_t now = monotonic_millis();
    motion_event.action = action;
    motion_event.source = 0x00001002; /* InputDevice.SOURCE_TOUCHSCREEN */
    motion_event.device_id = 0;
    motion_event.meta_state = 0;
    motion_event.button_state = 0;
    motion_event.flags = 0;
    motion_event.event_time = now;
    if (action == 0 || motion_event.down_time == 0)
        motion_event.down_time = now;
    memset(motion_event.axis, 0, sizeof motion_event.axis);
    motion_event.axis[0] = x;
    motion_event.axis[1] = y;
    return motion_event_object;
}

void *bc_jni_fmod_device(void)
{
    return fmod_device_object;
}

void *bc_jni_fmod_bytebuffer(void)
{
    return fmod_bytebuffer;
}

void *bc_jni_fmod_pcm(void)
{
    return fmod_pcm;
}

int bc_jni_fmod_pcm_capacity(void)
{
    return (int)sizeof fmod_pcm;
}

void bc_jni_fmod_set_buffer_size(int bytes)
{
    if (bytes <= 0 || bytes > (int)sizeof fmod_pcm)
        return;
    fmod_buffer_size = bytes;
    if (fmod_bytebuffer)
        fmod_bytebuffer->len = bytes;
}

int bc_jni_fmod_should_run(void)
{
    return __atomic_load_n(&fmod_should_run, __ATOMIC_ACQUIRE);
}

/* ----------------------------------------------------------- method registry */

/* Method IDs are indices into a flat table so a handler can be looked up in
 * one step and an unknown method still returns a usable ID (returning NULL
 * from GetMethodID makes Unity abort long before we learn what it wanted). */
typedef struct {
    const char *cls, *name, *sig;
    void *handler;          /* bc_jni_handler, or NULL for "safe default" */
} jmethod;

#define MAX_METHODS 1024
static jmethod methods[MAX_METHODS];
static int method_n;

typedef struct {
    void *env;
    jobj *self;
    va_list *ap;
    const uint64_t *args;
    unsigned arg_index;
    const jmethod *m;
} jctx;
typedef int64_t (*bc_jni_handler)(jctx *);

/* JNI's A calls carry a jvalue[] instead of a va_list.  Both representations
 * use one eight-byte slot per argument on this aarch64 target; keeping the
 * readers here lets the same platform handler serve Call*Method, *V and *A. */
static jobj *jarg_obj(jctx *c)
{
    if (c->args)
        return (jobj *)(uintptr_t)c->args[c->arg_index++];
    return va_arg(*c->ap, jobj *);
}

static int32_t jarg_int(jctx *c)
{
    if (c->args)
        return (int32_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int32_t);
}

static int64_t jarg_long(jctx *c)
{
    if (c->args)
        return (int64_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int64_t);
}

static float jarg_float(jctx *c)
{
    float value;
    if (c->args) {
        uint32_t bits = (uint32_t)c->args[c->arg_index++];
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    return (float)va_arg(*c->ap, double);
}

static void *method_id(const char *cls, const char *name, const char *sig)
{
    for (int i = 0; i < method_n; i++)
        if (strcmp(methods[i].name, name) == 0 &&
            strcmp(methods[i].sig, sig) == 0 &&
            (!cls || !methods[i].cls || strcmp(methods[i].cls, cls) == 0))
            return (void *)(uintptr_t)(i + 1);
    if (method_n == MAX_METHODS)
        nx_die("method table full");
    methods[method_n].cls = cls ? strdup(cls) : NULL;
    methods[method_n].name = strdup(name);
    methods[method_n].sig = strdup(sig);
    methods[method_n].handler = NULL;
    JT("new method id %d %s.%s%s", method_n + 1, cls ? cls : "?", name, sig);
    return (void *)(uintptr_t)(++method_n);
}

static const jmethod *by_id(void *mid)
{
    uintptr_t i = (uintptr_t)mid;
    return (i >= 1 && i <= (uintptr_t)method_n) ? &methods[i - 1] : NULL;
}

void bc_jni_bind(const char *cls, const char *name, const char *sig, void *fn)
{
    void *id = method_id(cls, name, sig);
    methods[(uintptr_t)id - 1].handler = fn;
}

/* --------------------------------------------------------------- registry of
 * natives that the loaded objects register with us */

typedef struct { char cls[128]; char name[128]; char sig[192]; void *fn; } jnative;
#define MAX_NATIVES 512
static jnative natives[MAX_NATIVES];
static int natives_n;
static jobj *unity_player_object;

void *bc_jni_native(const char *cls, const char *name)
{
    for (int i = 0; i < natives_n; i++)
        if (strcmp(natives[i].name, name) == 0 &&
            (!cls || strcmp(natives[i].cls, cls) == 0))
            return natives[i].fn;
    return NULL;
}

void bc_jni_set_unity_player(void *player)
{
    unity_player_object = player;
}

static jobj *soft_input_player(void)
{
    return unity_player_object
        ? unity_player_object
        : mk_class("com/unity3d/player/UnityPlayer");
}

void bc_jni_soft_input_text(const char *text)
{
    void *native =
        bc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetInputString");
    if (!native)
        return;
    jobj *value = mk_string(text ? text : "");
    ((void (*)(void *, void *, void *))native)(
        bc_jni_env(), soft_input_player(), value);
}

void bc_jni_soft_input_selection(int start, int length)
{
    void *native =
        bc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetInputSelection");
    if (native)
        ((void (*)(void *, void *, int, int))native)(
            bc_jni_env(), soft_input_player(), start, length);
}

void bc_jni_soft_input_visible(int visible)
{
    void *native =
        bc_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetKeyboardIsVisible");
    if (native)
        ((void (*)(void *, void *, int))native)(
            bc_jni_env(), soft_input_player(), visible != 0);
}

void bc_jni_soft_input_closed(int canceled)
{
    const char *name =
        canceled ? "nativeSoftInputCanceled" : "nativeSoftInputClosed";
    void *native =
        bc_jni_native("com/unity3d/player/UnityPlayer", name);
    if (native)
        ((void (*)(void *, void *))native)(
            bc_jni_env(), soft_input_player());
}

/* ------------------------------------------------------------------ vtable */

static void *vt[JNI_SLOT_COUNT];
static void *env_ptr = vt;          /* JNIEnv* is a pointer to the vtable ptr */
static void *jvm_vt[8];
static void *jvm_ptr = jvm_vt;

void *bc_jni_env(void) { return &env_ptr; }
void *bc_jni_vm(void) { return &jvm_ptr; }

/* --------------------------------------------------- Java callback/Looper flow
 *
 * Unity 2022 implements UnityChoreographer through bitter/jnibridge:
 *
 *   newInterfaceProxy(Handler.Callback + FrameCallback)
 *   -> HandlerThread.start()
 *   -> Handler(obtained looper, proxy)
 *   -> obtainMessage().sendToTarget()
 *   -> proxy.handleMessage()
 *   -> proxy.doFrame(frameTimeNanos), once per display tick
 *
 * The Android classes do not exist on NextOS, but the native proxy and its
 * callbacks do.  These objects preserve that exact ordering and invoke the
 * registered JNIBridge native just as the Java Looper would.
 */

enum {
    PROXY_HANDLER_CALLBACK = 1u << 0,
    PROXY_FRAME_CALLBACK   = 1u << 1,
    PROXY_RUNNABLE         = 1u << 2,
};

static jobj *choreo_proxy;
static jobj *looper_object;
static jobj *choreographer_object;
static jobj *message_object;
static jobj *doframe_method;
static jobj *handlemsg_method;
static jobj *run_method;
static jobj *doframe_args;
static jobj *handlemsg_args;
static jobj *empty_args;
static jobj *frame_time_box;
static void *doframe_mid;
static void *handlemsg_mid;
static void *run_mid;
static int message_pending;
static int choreo_thread_started;
static int message_what;

static int64_t monotonic_nanos(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static unsigned proxy_interfaces(jobj *interfaces)
{
    unsigned flags = 0;
    if (!interfaces)
        return flags;
    int n = interfaces->type == O_OBJARRAY ? interfaces->len : 1;
    for (int i = 0; i < n; i++) {
        jobj *c = interfaces->type == O_OBJARRAY
                    ? interfaces->elems[i] : interfaces;
        const char *name = c ? c->cls : NULL;
        if (!name)
            continue;
        if (strcmp(name, "android/os/Handler$Callback") == 0)
            flags |= PROXY_HANDLER_CALLBACK;
        else if (strcmp(name, "android/view/Choreographer$FrameCallback") == 0)
            flags |= PROXY_FRAME_CALLBACK;
        else if (strcmp(name, "java/lang/Runnable") == 0)
            flags |= PROXY_RUNNABLE;
    }
    return flags;
}

static void *invoke_proxy(jobj *proxy, jobj *iface, jobj *method, jobj *args)
{
    if (!proxy || !iface || !method || !args)
        return NULL;
    void *invoke = bc_jni_native("bitter/jnibridge/JNIBridge", "invoke");
    if (!invoke || !proxy->prim)
        return NULL;
    return ((void *(*)(void *, void *, int64_t, void *, void *, void *))invoke)(
        bc_jni_env(), iface, proxy->prim, iface, method, args);
}

static int deliver_handle_message(void)
{
    jobj *proxy = __atomic_load_n(&choreo_proxy, __ATOMIC_ACQUIRE);
    if (!proxy)
        return 0;
    jobj *iface = mk_class("android/os/Handler$Callback");
    nx_log("jni: UnityChoreographer handleMessage(what=%d)", message_what);
    (void)invoke_proxy(proxy, iface, handlemsg_method, handlemsg_args);
    return 1;
}

static int deliver_frame(int64_t nanos)
{
    jobj *proxy = __atomic_load_n(&choreo_proxy, __ATOMIC_ACQUIRE);
    if (!proxy)
        return 0;
    __atomic_store_n(&frame_time_box->prim, nanos, __ATOMIC_RELEASE);
    jobj *iface = mk_class("android/view/Choreographer$FrameCallback");
    (void)invoke_proxy(proxy, iface, doframe_method, doframe_args);
    return 1;
}

static void *choreographer_driver(void *arg)
{
    (void)arg;
    void *domain = NULL;
    void *thread = NULL;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (il2cpp) {
        void *(*domain_get)(void) = nx_lookup_in(il2cpp, "il2cpp_domain_get");
        void *(*thread_attach)(void *) =
            nx_lookup_in(il2cpp, "il2cpp_thread_attach");
        if (domain_get && thread_attach) {
            domain = domain_get();
            thread = thread_attach(domain);
        }
    }
    nx_log("jni: UnityChoreographer HandlerThread active"
           " (il2cpp domain=%p thread=%p)", domain, thread);

    while (!__atomic_exchange_n(&message_pending, 0, __ATOMIC_ACQ_REL))
        usleep(1000);
    if (!deliver_handle_message())
        return NULL;

    const int64_t period = 16666667LL;
    int64_t next = monotonic_nanos() + period;
    unsigned long frames = 0;
    for (;;) {
        while (__atomic_exchange_n(&message_pending, 0, __ATOMIC_ACQ_REL))
            (void)deliver_handle_message();

        struct timespec until = {
            .tv_sec = next / 1000000000LL,
            .tv_nsec = next % 1000000000LL,
        };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) ==
               EINTR)
            ;
        if (deliver_frame(next)) {
            frames++;
            if (frames == 1)
                nx_log("jni: UnityChoreographer first doFrame delivered");
        }
        next += period;
        int64_t now = monotonic_nanos();
        if (next < now)
            next = now + period;
    }
}

/* --- basics ------------------------------------------------------------- */

static int32_t j_GetVersion(void *e) { (void)e; return 0x00010006; }

static jobj *j_FindClass(void *e, const char *name)
{
    (void)e;
    JT("FindClass %s", name);
    return mk_class(name);
}

static jobj *j_GetObjectClass(void *e, jobj *o)
{
    (void)e;
    return mk_class(o && o->cls ? o->cls : "java/lang/Object");
}

static uint8_t j_IsInstanceOf(void *e, jobj *o, jobj *c)
{
    (void)e;
    if (!o || !c || !c->cls)
        return 0;
    if (strcmp(c->cls, "java/lang/Object") == 0)
        return 1;
    if (strcmp(c->cls, "android/view/InputEvent") == 0 && o->cls &&
        (strcmp(o->cls, "android/view/KeyEvent") == 0 ||
         strcmp(o->cls, "android/view/MotionEvent") == 0))
        return 1;
    return o->cls && strcmp(o->cls, c->cls) == 0;
}

static uint8_t j_IsAssignableFrom(void *e, jobj *a, jobj *b)
{
    (void)e;
    return a && b && a->cls && b->cls && strcmp(a->cls, b->cls) == 0;
}

static jobj *j_GetSuperclass(void *e, jobj *c) { (void)e; (void)c; return mk_class("java/lang/Object"); }

static void *j_NewGlobalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewLocalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewWeakGlobalRef(void *e, void *o) { (void)e; return o; }
static void j_DeleteRef(void *e, void *o) { (void)e; (void)o; }
static uint8_t j_IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static int32_t j_PushLocalFrame(void *e, int32_t n) { (void)e; (void)n; return 0; }
static void *j_PopLocalFrame(void *e, void *r) { (void)e; return r; }
static int32_t j_EnsureLocalCapacity(void *e, int32_t n) { (void)e; (void)n; return 0; }

static jobj *pending_exception;
static void *j_ExceptionOccurred(void *e) { (void)e; return pending_exception; }
static void j_ExceptionDescribe(void *e) { (void)e; }
static void j_ExceptionClear(void *e) { (void)e; pending_exception = NULL; }
static uint8_t j_ExceptionCheck(void *e) { (void)e; return pending_exception != NULL; }
static int32_t j_Throw(void *e, jobj *t) { (void)e; pending_exception = t; return 0; }
static int32_t j_ThrowNew(void *e, jobj *c, const char *m)
{
    (void)e;
    nx_log("jni: ThrowNew %s: %s", c && c->cls ? c->cls : "?", m ? m : "");
    pending_exception = new_obj(O_THROWABLE, c ? c->cls : "java/lang/Exception");
    return 0;
}
static void j_FatalError(void *e, const char *m) { (void)e; nx_die("JNI FatalError: %s", m); }

static int32_t j_GetJavaVM(void *e, void **vm) { (void)e; *vm = bc_jni_vm(); return 0; }
static int32_t j_MonitorEnter(void *e, void *o) { (void)e; (void)o; return 0; }
static int32_t j_MonitorExit(void *e, void *o) { (void)e; (void)o; return 0; }

/* --- strings ----------------------------------------------------------- */

static jobj *j_NewStringUTF(void *e, const char *s) { (void)e; return mk_string(s); }
static const char *j_GetStringUTFChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    return s && s->str ? s->str : "";
}
static void j_ReleaseStringUTFChars(void *e, jobj *s, const char *c) { (void)e; (void)s; (void)c; }
static int32_t j_GetStringUTFLength(void *e, jobj *s) { (void)e; return s ? s->len : 0; }
static int32_t j_GetStringLength(void *e, jobj *s) { (void)e; return s ? s->len : 0; }
static jobj *j_NewString(void *e, const uint16_t *u, int32_t n)
{
    (void)e;
    char *b = malloc((size_t)n + 1);
    for (int32_t i = 0; i < n; i++)
        b[i] = (char)(u[i] < 128 ? u[i] : '?');
    b[n] = 0;
    jobj *o = mk_string(b);
    free(b);
    return o;
}
static const uint16_t *j_GetStringChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 1;
    int n = s ? s->len : 0;
    uint16_t *u = calloc((size_t)n + 1, 2);
    for (int i = 0; i < n; i++)
        u[i] = (uint8_t)s->str[i];
    return u;
}
static void j_ReleaseStringChars(void *e, jobj *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
static void j_GetStringUTFRegion(void *e, jobj *s, int32_t off, int32_t len, char *out)
{
    (void)e;
    if (s && s->str)
        memcpy(out, s->str + off, (size_t)len);
}

/* --- arrays ------------------------------------------------------------ */

static int32_t j_GetArrayLength(void *e, jobj *a) { (void)e; return a ? a->len : 0; }

static jobj *j_NewByteArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[B");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), 1);
    return o;
}

static jobj *j_NewIntArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[I");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), sizeof(int32_t));
    return o;
}

static jobj *mk_int_array(const int32_t *values, int32_t n)
{
    jobj *o = j_NewIntArray(NULL, n);
    if (values && n > 0)
        memcpy(o->data, values, (size_t)n * sizeof *values);
    return o;
}
static void *j_GetByteArrayElements(void *e, jobj *a, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    return a ? a->data : NULL;
}
static void j_ReleaseArrayElements(void *e, jobj *a, void *p, int32_t mode)
{
    (void)e; (void)a; (void)p; (void)mode;
}
static void j_GetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (char *)a->data + s, (size_t)n);
}
static void j_SetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((char *)a->data + s, buf, (size_t)n);
}

static void j_GetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (int32_t *)a->data + s, (size_t)n * sizeof(int32_t));
}

static void j_SetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n,
                                const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((int32_t *)a->data + s, buf, (size_t)n * sizeof(int32_t));
}
static jobj *j_NewObjectArray(void *e, int32_t n, jobj *cls, jobj *init)
{
    (void)e; (void)cls;
    jobj *o = new_obj(O_OBJARRAY, "[Ljava/lang/Object;");
    o->len = n;
    o->elems = calloc((size_t)(n > 0 ? n : 1), sizeof *o->elems);
    for (int32_t i = 0; i < n; i++)
        o->elems[i] = init;
    return o;
}
static jobj *j_GetObjectArrayElement(void *e, jobj *a, int32_t i)
{
    (void)e;
    return (a && a->elems && i >= 0 && i < a->len) ? a->elems[i] : NULL;
}
static void j_SetObjectArrayElement(void *e, jobj *a, int32_t i, jobj *v)
{
    (void)e;
    if (a && a->elems && i >= 0 && i < a->len)
        a->elems[i] = v;
}
static void *j_GetPrimitiveArrayCritical(void *e, jobj *a, uint8_t *copy)
{
    return j_GetByteArrayElements(e, a, copy);
}
static void j_ReleasePrimitiveArrayCritical(void *e, jobj *a, void *p, int32_t m)
{
    (void)e; (void)a; (void)p; (void)m;
}
static jobj *j_NewDirectByteBuffer(void *e, void *addr, int64_t cap)
{
    (void)e;
    jobj *o = new_obj(O_DIRECTBUF, "java/nio/ByteBuffer");
    o->data = addr;
    o->len = (int)cap;
    return o;
}
static void *j_GetDirectBufferAddress(void *e, jobj *b) { (void)e; return b ? b->data : NULL; }
static int64_t j_GetDirectBufferCapacity(void *e, jobj *b) { (void)e; return b ? b->len : -1; }
static int32_t j_GetObjectRefType(void *e, void *o) { (void)e; return o ? 2 : 0; }

/* --- calls ------------------------------------------------------------- */

static int is_unbox(const char *name)
{
    static const char *const n[] = { "longValue", "intValue", "shortValue",
                                     "byteValue", "charValue", "booleanValue",
                                     "floatValue", "doubleValue" };
    for (size_t i = 0; i < sizeof n / sizeof *n; i++)
        if (strcmp(name, n[i]) == 0)
            return 1;
    return 0;
}

/* --- Android input devices, KeyEvent and MotionEvent ------------------ */

static int64_t jfloat_result(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int64_t j_InputDevice_getDeviceIds(jctx *c)
{
    (void)c;
    /* Real Android enumerates its built-in touchscreen as an InputDevice too.
     * Unity must see that device during its startup scan before later
     * touchscreen MotionEvents can become Touches. */
    const int32_t ids[] = { 0, 1 };
    static int logged;
    if (!logged++)
        nx_log("jni: publishing Android touchscreen id=0 and gamepad id=1 "
               "(%s %04x:%04x)",
               input_device_name, input_device_vendor & 0xffff,
               input_device_product & 0xffff);
    return (int64_t)(uintptr_t)mk_int_array(ids, 2);
}

static int64_t j_InputDevice_getDevice(jctx *c)
{
    int id = jarg_int(c);
    return (int64_t)(uintptr_t)(id == 0 ? touch_device :
                                id == 1 ? input_device : NULL);
}

static int64_t j_InputEvent_getDevice(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/MotionEvent") == 0 &&
        motion_from_object(c->self)->device_id == 0) {
        JT("touch getDevice -> %p (id=0 sources=%#x)",
           (void *)touch_device, 0x00001002);
        return (int64_t)(uintptr_t)touch_device;
    }
    return (int64_t)(uintptr_t)input_device;
}

static int64_t j_InputDevice_getString(jctx *c)
{
    if (c->self == touch_device)
        return (int64_t)(uintptr_t)mk_string(
            strcmp(c->m->name, "getDescriptor") == 0
                ? "nextos:touch:0" : "NextOS Touchscreen");
    const char *value = strcmp(c->m->name, "getDescriptor") == 0
        ? input_device_descriptor : input_device_name;
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_InputDevice_getInt(jctx *c)
{
    if (c->self == touch_device) {
        if (strcmp(c->m->name, "getSources") == 0) return 0x00001002;
        if (strcmp(c->m->name, "getId") == 0) return 0;
        return 0;
    }
    if (strcmp(c->m->name, "getVendorId") == 0) return input_device_vendor;
    if (strcmp(c->m->name, "getProductId") == 0) return input_device_product;
    if (strcmp(c->m->name, "getSources") == 0) return 0x01000611;
    if (strcmp(c->m->name, "getId") == 0) return 1;
    if (strcmp(c->m->name, "getControllerNumber") == 0) return 1;
    return 0; /* KEYBOARD_TYPE_NONE */
}

static int64_t j_InputDevice_supportsSource(jctx *c)
{
    int source = jarg_int(c);
    int available = c->self == touch_device ? 0x00001002 : 0x01000611;
    return (available & source) == source;
}

static int64_t j_InputDevice_getMotionRanges(jctx *c)
{
    return (int64_t)(uintptr_t)(c->self == touch_device
                                    ? touch_motion_range_list
                                    : motion_range_list);
}

static int64_t j_InputDevice_getMotionRange(jctx *c)
{
    int axis = jarg_int(c);
    if (c->self == touch_device)
        return 0;
    for (size_t i = 0; i < sizeof motion_axis_ids /
                            sizeof *motion_axis_ids; i++)
        if (motion_axis_ids[i] == axis)
            return (int64_t)(uintptr_t)motion_ranges[i];
    return 0;
}

static int64_t j_List_size(jctx *c)
{
    return c->self ? c->self->len : 0;
}

static int64_t j_List_get(jctx *c)
{
    int index = jarg_int(c);
    if (!c->self || !c->self->elems || index < 0 || index >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[index];
}

static int64_t j_List_iterator(jctx *c)
{
    motion_range_iterator->elems = c->self ? c->self->elems : NULL;
    motion_range_iterator->len = c->self ? c->self->len : 0;
    motion_range_iterator->prim = 0;
    return (int64_t)(uintptr_t)motion_range_iterator;
}

static int64_t j_Iterator_hasNext(jctx *c)
{
    return c->self && c->self->prim < c->self->len;
}

static int64_t j_Iterator_next(jctx *c)
{
    if (!c->self || !c->self->elems ||
        c->self->prim < 0 || c->self->prim >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[c->self->prim++];
}

static int64_t j_MotionRange_getInt(jctx *c)
{
    if (!c->self)
        return 0;
    if (strcmp(c->m->name, "getAxis") == 0)
        return c->self->prim;
    return 0x01000010; /* SOURCE_JOYSTICK */
}

static int64_t j_MotionRange_getFloat(jctx *c)
{
    int axis = c->self ? (int)c->self->prim : -1;
    float value = 0.0f;
    if (strcmp(c->m->name, "getMin") == 0)
        value = axis == 17 || axis == 18 ? 0.0f : -1.0f;
    else if (strcmp(c->m->name, "getMax") == 0)
        value = 1.0f;
    else if (strcmp(c->m->name, "getFlat") == 0)
        value = 0.05f;
    else if (strcmp(c->m->name, "getResolution") == 0)
        value = 1.0f / 32767.0f;
    return jfloat_result(value);
}

static int64_t j_KeyEvent_getInt(jctx *c)
{
    if (strcmp(c->m->name, "getAction") == 0) return key_event.action;
    if (strcmp(c->m->name, "getKeyCode") == 0) return key_event.keycode;
    if (strcmp(c->m->name, "getSource") == 0) return key_event.source;
    if (strcmp(c->m->name, "getDeviceId") == 0) return key_event.device_id;
    if (strcmp(c->m->name, "getMetaState") == 0) return key_event.meta_state;
    if (strcmp(c->m->name, "getRepeatCount") == 0) return key_event.repeat;
    if (strcmp(c->m->name, "getScanCode") == 0) return key_event.scancode;
    if (strcmp(c->m->name, "getFlags") == 0) return key_event.flags;
    if (strcmp(c->m->name, "getUnicodeChar") == 0) return key_event.unicode;
    return 0;
}

static int64_t j_KeyEvent_getLong(jctx *c)
{
    if (strcmp(c->m->name, "getDownTime") == 0)
        return key_event.down_time[key_event.keycode];
    return key_event.event_time;
}

static int64_t j_KeyEvent_isSystem(jctx *c)
{
    (void)c;
    return 0;
}

static int64_t j_MotionEvent_getInt(jctx *c)
{
    const char *name = c->m->name;
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getInt(c);
    motion_payload *event = motion_from_object(c->self);
    if (strcmp(name, "getPointerId") == 0 ||
        strcmp(name, "findPointerIndex") == 0 ||
        strcmp(name, "getToolType") == 0)
        (void)jarg_int(c);
    if (strcmp(name, "getAction") == 0 ||
        strcmp(name, "getActionMasked") == 0) {
        int value = strcmp(name, "getActionMasked") == 0
            ? event->action & 0xff : event->action;
        if (event->source == 0x00001002)
            JT("touch %s -> %d", name, value);
        return value;
    }
    if (strcmp(name, "getSource") == 0) {
        if (event->source == 0x00001002)
            JT("touch getSource -> %#x", event->source);
        return event->source;
    }
    if (strcmp(name, "getDeviceId") == 0) {
        if (event->source == 0x00001002)
            JT("touch getDeviceId -> %d", event->device_id);
        return event->device_id;
    }
    if (strcmp(name, "getMetaState") == 0) return event->meta_state;
    if (strcmp(name, "getButtonState") == 0) return event->button_state;
    if (strcmp(name, "getFlags") == 0) return event->flags;
    if (strcmp(name, "getPointerCount") == 0) return 1;
    if (strcmp(name, "getPointerId") == 0) return 0;
    if (strcmp(name, "findPointerIndex") == 0) return 0;
    if (strcmp(name, "getToolType") == 0)
        return event->source == 0x00001002 ? 1 : 0; /* TOOL_TYPE_FINGER */
    return 0; /* history/action index, edge flags, tool type, display id */
}

static int64_t j_MotionEvent_getLong(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getLong(c);
    motion_payload *event = motion_from_object(c->self);
    if (c->m->sig[1] == 'I')
        (void)jarg_int(c);
    int64_t value = strcmp(c->m->name, "getDownTime") == 0
        ? event->down_time : event->event_time;
    if (event->source == 0x00001002)
        JT("touch %s -> %lld", c->m->name, (long long)value);
    return value;
}

static int64_t j_MotionEvent_getFloat(jctx *c)
{
    const char *name = c->m->name;
    motion_payload *event = motion_from_object(c->self);
    int axis = -1;
    if (strcmp(name, "getAxisValue") == 0 ||
        strcmp(name, "getHistoricalAxisValue") == 0) {
        axis = jarg_int(c);
    } else if (strcmp(name, "getX") == 0 ||
               strcmp(name, "getHistoricalX") == 0 ||
               strcmp(name, "getRawX") == 0) {
        axis = 0;
    } else if (strcmp(name, "getY") == 0 ||
               strcmp(name, "getHistoricalY") == 0 ||
               strcmp(name, "getRawY") == 0) {
        axis = 1;
    }
    float value;
    if (strcmp(name, "getPressure") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getSize") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getTouchMajor") == 0 ||
             strcmp(name, "getTouchMinor") == 0 ||
             strcmp(name, "getToolMajor") == 0 ||
             strcmp(name, "getToolMinor") == 0)
        value = event->source == 0x00001002 ? 8.0f : 0.0f;
    else
        value = axis >= 0 && axis < (int)(sizeof event->axis /
                                          sizeof *event->axis)
                  ? event->axis[axis] : 0.0f;
    if (event->source == 0x00001002)
        JT("touch %s axis=%d -> %.3f", name, axis, value);
    return jfloat_result(value);
}

static int64_t j_MotionEvent_isFromSource(jctx *c)
{
    motion_payload *event = motion_from_object(c->self);
    int source = jarg_int(c);
    return (event->source & source) == source;
}

static int64_t j_MotionEvent_obtain(jctx *c)
{
    jobj *source = jarg_obj(c);
    unsigned slot = motion_clone_next++ % MOTION_CLONE_COUNT;
    if (!motion_clones[slot])
        motion_clones[slot] = mk_object("android/view/MotionEvent");
    motion_clone_data[slot] = *motion_from_object(source);
    motion_clones[slot]->data = &motion_clone_data[slot];
    if (motion_clone_data[slot].source == 0x00001002)
        JT("touch obtain clone=%u action=%d xy=%.1f,%.1f "
           "down=%lld event=%lld", slot, motion_clone_data[slot].action,
           motion_clone_data[slot].axis[0], motion_clone_data[slot].axis[1],
           (long long)motion_clone_data[slot].down_time,
           (long long)motion_clone_data[slot].event_time);
    return (int64_t)(uintptr_t)motion_clones[slot];
}

static int64_t j_MotionEvent_recycle(jctx *c)
{
    (void)c;
    return 0;
}

/* java.io.File.  Unity and Android plugins inspect their data files before
 * opening them.  A zero length is not a neutral answer: it means a truncated
 * or missing install and selects their normal report-and-bail path. */
static int64_t j_File_length(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        JT("File.length(\"%s\") -> 0 (no such file)", p ? p : "(null)");
        return 0;
    }
    JT("File.length(\"%s\") -> %lld", p, (long long)st.st_size);
    return (int64_t)st.st_size;
}

static int64_t j_File_exists(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    int64_t r = p && stat(p, &st) == 0;
    JT("File.exists(\"%s\") -> %lld", p ? p : "(null)", (long long)r);
    return r;
}

static int64_t j_File_getPath(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(c->self ? c->self->str : NULL);
}

static int64_t j_File_getParent(jctx *c)
{
    const char *path = c->self && c->self->str ? c->self->str : "";
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash)
        buf[0] = 0;
    else if (slash == buf)
        slash[1] = 0;
    else
        *slash = 0;
    return (int64_t)(uintptr_t)mk_string(buf);
}

static int64_t j_File_getParentFile(jctx *c)
{
    jobj *s = (jobj *)(uintptr_t)j_File_getParent(c);
    return (int64_t)(uintptr_t)mk_file(s ? s->str : "");
}

static int64_t j_File_isDirectory(jctx *c)
{
    struct stat st;
    const char *p = c->self && c->self->str ? c->self->str : "";
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int64_t j_File_mkdirs(jctx *c)
{
    const char *p = c->self && c->self->str ? c->self->str : "";
    if (!*p)
        return 0;
    if (mkdir(p, 0755) == 0 || errno == EEXIST)
        return 1;
    return 0;
}

static int64_t j_Context_getFilesDir(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_file(bc_home);
}

static int64_t j_Context_getPackageCodePath(jctx *c)
{
    (void)c;
    /* Unity treats this as the extracted package root and appends
     * assets/bin/Data itself.  Returning bc_datadir would produce
     * <gamedir>/assets/assets/bin/Data. */
    return (int64_t)(uintptr_t)mk_string(bc_gamedir);
}

#define SK3_PACKAGE "com.ratrodstudio.skateparty3lite"

static int64_t j_Context_getPackageName(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string(SK3_PACKAGE);
}

/* A libunity monta o PlayerPrefs a partir de getApplicationInfo().dataDir e do
 * sufixo ".v2.playerprefs"; sem este objeto ela desiste calada e o jogo NAO
 * salva nada.  dataDir aponta para o mesmo lugar que getFilesDir, que e onde o
 * jogo ja escreve Unity/, il2cpp/ e o shader cache. */
static int64_t j_Context_getApplicationInfo(jctx *c)
{
    (void)c;
    static jobj *info;
    if (!info)
        info = mk_object("android/content/pm/ApplicationInfo");
    return (int64_t)(uintptr_t)info;
}

static int64_t j_AppInfo_dataDir(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string(bc_home);
}

static int64_t j_AppInfo_nativeLibraryDir(jctx *c)
{
    (void)c;
    char path[1200];
    snprintf(path, sizeof path, "%s/lib", bc_gamedir);
    return (int64_t)(uintptr_t)mk_string(path);
}

static int64_t j_AppInfo_sourceDir(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string(bc_gamedir);
}

/* Unity Analytics/Services roda a cada frame: PackageManager.getPackageInfo ->
 * versionName, getInstallerPackageName e String.getBytes(charset).  O stub
 * generico devolvia versionName=null e getBytes=null; o C# estourava
 * (NullReference) e repetia no frame seguinte -- laco infinito de JNI que
 * enchia o log e roubava o frame.  Devolver os valores reais encerra o laco. */
#define SK3_VERSION_NAME "1.11.3"

void *bc_jret_bytes(const void *p, int n);

static int64_t j_PackageManager_getPackageInfo(jctx *c)
{
    (void)c;
    static jobj *info;
    if (!info)
        info = mk_object("android/content/pm/PackageInfo");
    return (int64_t)(uintptr_t)info;
}

static int64_t j_PackageInfo_versionName(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string(SK3_VERSION_NAME);
}

static int64_t j_PackageManager_getInstallerPackageName(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("com.android.vending");
}

static int64_t j_String_getBytes(jctx *c)
{
    const char *s = c->self && c->self->str ? c->self->str : "";
    return (int64_t)(uintptr_t)bc_jret_bytes(s, (int)strlen(s));
}

static int64_t j_AudioManager_getDevices(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)j_NewObjectArray(
        NULL, 0, mk_class("android/media/AudioDeviceInfo"), NULL);
}

static int64_t j_Context_getAssets(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("android/content/res/AssetManager");
}

static int64_t j_Context_getContentResolver(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("android/content/ContentResolver");
}

static int64_t j_SettingsSecure_getString(jctx *c)
{
    (void)jarg_obj(c); /* ContentResolver */
    jobj *key = jarg_obj(c);
    const char *name = key && key->str ? key->str : "";

    /* Android's ANDROID_ID is a stable, app-visible 64-bit hexadecimal
     * identifier.  Keep it deterministic so the game's offline profile and
     * obfuscation keys remain valid between launches. */
    const char *value = strcmp(name, "android_id") == 0
                            ? "6e6578746f736867"
                            : "";
    JT("Settings.Secure.getString(\"%s\") -> \"%s\"", name, value);
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_Locale_getDefault(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("java/util/Locale");
}

static int64_t j_Locale_getString(jctx *c)
{
    const char *value = "en-US";
    if (strcmp(c->m->name, "getLanguage") == 0)
        value = "en";
    else if (strcmp(c->m->name, "getCountry") == 0)
        value = "US";
    else if (strcmp(c->m->name, "toString") == 0)
        value = "en_US";
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_GoogleIAB_instance(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("com/prime31/GoogleIABPlugin");
}

static int64_t j_PlayGames_initialize(jctx *c)
{
    (void)jarg_obj(c);
    return 0;
}

static int64_t j_PlayGames_getSignInClient(jctx *c)
{
    (void)jarg_obj(c);
    return (int64_t)(uintptr_t)mk_object(
        "com/google/android/gms/games/GamesSignInClient");
}

/* ArmoryActivity's constructor only publishes itself and creates this
 * permission helper.  These objects reproduce that Java-side state for the
 * C# AndroidJavaObject calls while all permissions needed by the local port
 * are already satisfied by its own game directory. */
static int64_t j_Armory_getActivity(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)armory_activity_object;
}

static void normalize_reflection_signature(char *out, size_t capacity,
                                           const char *signature)
{
    if (!capacity)
        return;
    size_t i = 0;
    for (; signature && signature[i] && i + 1 < capacity; i++)
        out[i] = signature[i] == '.' ? '/' : signature[i];
    out[i] = 0;
}

/* Unity 2022 routes AndroidJavaObject/AndroidJavaClass lookups through its
 * Java ReflectionHelper before converting the reflected handle back into a
 * JNI method/field ID.  Preserve that native Android sequence: the reflected
 * object and the eventual ID share the stable registry entry, exactly as in
 * the proven Terraria/Horizon bridge. */
static int64_t j_Reflection_getConstructorID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getConstructorID(%s, %s)", cls ? cls : "?", sig);
    return (int64_t)(uintptr_t)method_id(cls, "<init>", normalized);
}

static int64_t j_Reflection_getMethodID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *name = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    int is_static = jarg_int(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *method = name && name->str ? name->str : "";
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getMethodID(%s.%s, %s, static=%d)",
       cls ? cls : "?", method, sig, is_static);
    return (int64_t)(uintptr_t)method_id(cls, method, normalized);
}

static int64_t j_Reflection_getFieldID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *name = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    int is_static = jarg_int(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *field = name && name->str ? name->str : "";
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getFieldID(%s.%s, %s, static=%d)",
       cls ? cls : "?", field, sig, is_static);
    return (int64_t)(uintptr_t)method_id(cls, field, normalized);
}

static int64_t j_Reflection_getFieldSignature(jctx *c)
{
    void *field = jarg_obj(c);
    const jmethod *entry = by_id(field);
    return (int64_t)(uintptr_t)mk_string(entry && entry->sig
                                             ? entry->sig : "");
}

static int64_t j_Unity_currentActivity(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)armory_activity_object;
}

static int64_t j_Armory_getPermissionPlugin(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)permission_plugin_object;
}

static int64_t j_Armory_getOsVersion(jctx *c)
{
    (void)c;
    return 33;
}

static int64_t j_Armory_getTargetSdkVersion(jctx *c)
{
    (void)c;
    return 36;
}

static int64_t j_Armory_hasPermission(jctx *c)
{
    jobj *permission = jarg_obj(c);
    JT("Armory permission %s -> granted",
       permission && permission->str ? permission->str : "(unknown)");
    return 1;
}

static int64_t j_Permission_canRequest(jctx *c)
{
    (void)c;
    return 1;
}

static int64_t j_Permission_noRationale(jctx *c)
{
    (void)jarg_obj(c);
    return 0;
}

static int64_t j_Permission_noop(jctx *c)
{
    if (strstr(c->m->sig, "Ljava/lang/String;"))
        (void)jarg_obj(c);
    return 0;
}

static int64_t j_AssetManager_open(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *rel = name && name->str ? name->str : "";
    while (*rel == '/')
        rel++;
    if (strncmp(rel, "assets/", 7) == 0)
        rel += 7;

    char path[1280];
    snprintf(path, sizeof path, "%s/%s", bc_datadir, rel);
    struct stat st;
    if (stat(path, &st) != 0) {
        JT("AssetManager.open(\"%s\") -> NULL", rel);
        return 0;
    }
    jobj *stream = mk_object("java/io/InputStream");
    stream->str = strdup(path);
    stream->len = (int)st.st_size;
    JT("AssetManager.open(\"%s\") -> %s", rel, path);
    return (int64_t)(uintptr_t)stream;
}

static int64_t j_Scanner_next(jctx *c)
{
    const char *path = c->self ? c->self->str : NULL;
    if (!path)
        return (int64_t)(uintptr_t)mk_string("");
    FILE *f = fopen(path, "rb");
    if (!f)
        return (int64_t)(uintptr_t)mk_string("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0 || n > 1024 * 1024) {
        fclose(f);
        return (int64_t)(uintptr_t)mk_string("");
    }
    char *buf = calloc((size_t)n + 1, 1);
    if (buf && n)
        (void)fread(buf, 1, (size_t)n, f);
    fclose(f);
    jobj *s = mk_string(buf ? buf : "");
    free(buf);
    return (int64_t)(uintptr_t)s;
}

static int64_t j_Environment_mounted(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("mounted");
}

static int64_t j_String_equals(jctx *c)
{
    jobj *other = jarg_obj(c);
    const char *a = c->self && c->self->str ? c->self->str : "";
    const char *b = other && other->str ? other->str : "";
    return strcmp(a, b) == 0;
}

static int64_t j_StringBuilder_toString(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}

static int uri_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static int64_t j_Uri_encode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *encoded = malloc(length * 3 + 1);
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (uri_unreserved(byte)) {
            encoded[out++] = (char)byte;
        } else {
            encoded[out++] = '%';
            encoded[out++] = hex[byte >> 4];
            encoded[out++] = hex[byte & 15];
        }
    }
    encoded[out] = '\0';
    jobj *result = mk_string(encoded);
    free(encoded);
    return (int64_t)(uintptr_t)result;
}

static int uri_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int64_t j_Uri_decode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *decoded = malloc(length + 1);
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        int high, low;
        if (text[i] == '%' && i + 2 < length &&
            (high = uri_hex(text[i + 1])) >= 0 &&
            (low = uri_hex(text[i + 2])) >= 0) {
            decoded[out++] = (char)((high << 4) | low);
            i += 2;
        } else {
            decoded[out++] = text[i];
        }
    }
    decoded[out] = '\0';
    jobj *result = mk_string(decoded);
    free(decoded);
    return (int64_t)(uintptr_t)result;
}

/* Unity reads android.os.Build to pick device-specific workarounds, and the
 * first thing it does with MANUFACTURER is strcasecmp(field, "Amazon") -- with
 * the field absent that faults inside strcasecmp before anything is drawn.
 *
 * These values describe this device, and deliberately match no vendor Unity
 * has a workaround for: a name borrowed from a real manufacturer would switch
 * on a code path written for someone else's hardware.  The field name arrives
 * in the context, so one handler serves the whole class.
 */
static const struct { const char *name, *value; } build_fields[] = {
    { "MANUFACTURER", "NextOS" },
    { "BRAND",        "NextOS" },
    { "MODEL",        "NextOS-Retro-Elite" },
    { "PRODUCT",      "nextos" },
    { "DEVICE",       "amlogic" },
    { "BOARD",        "amlogic" },
    { "HARDWARE",     "amlogic" },
    { "ID",           "NextOS" },
    { "TYPE",         "user" },
    { "TAGS",         "release-keys" },
    { "FINGERPRINT",  "NextOS/nextos/amlogic:13/NextOS/4.8.2:user/release-keys" },
    { "RELEASE",      "13" },
    { "CODENAME",     "REL" },
    { "INCREMENTAL",  "4.8.2" },
    { "SERIAL",       "unknown" },
};

static int64_t j_Build_string(jctx *c)
{
    const char *name = c->m ? c->m->name : NULL;
    if (name)
        for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++)
            if (strcmp(build_fields[i].name, name) == 0)
                return (int64_t)(uintptr_t)mk_string(build_fields[i].value);
    /* Never NULL: the caller passes these straight to string functions. */
    return (int64_t)(uintptr_t)mk_string("");
}

/* API 33 is the platform this 2022.3 player was built against.  Zero would read
 * as older than every gate Unity checks and take paths this build never
 * compiled. */
static int64_t j_Build_SDK_INT(jctx *c)
{
    (void)c;
    return 33;
}

/* Unity's preferences, enough for the questions it asks before the first frame.
 *
 * The one that matters is the GLES level warning.  Before showing it, Unity asks
 * `getPreferences(0).getBoolean("gles-api-check", false)` -- "has the user
 * already ticked do-not-show-again?".  Answer false and it builds an
 * AlertDialog, hands it to Activity.runOnUiThread and blocks the render thread
 * on a condition variable waiting for a button click.  There is no Java UI
 * thread here to deliver one, so frame 1 never returns.
 *
 * Answering true is not a trick to dodge a check: this port runs GLES2 on
 * Mali-450 deliberately, so the warning is one we have already read.
 */
enum pref_type {
    PREF_INT = 1,
    PREF_FLOAT,
    PREF_STRING,
    PREF_BOOL,
};

typedef struct {
    char *key;
    enum pref_type type;
    int32_t integer;
    float real;
    char *string;
} pref_entry;

#define MAX_PREFS 1024
static pref_entry preferences[MAX_PREFS];
static int preferences_count;
static int preferences_loaded;
static pthread_mutex_t preferences_lock = PTHREAD_MUTEX_INITIALIZER;

static pref_entry *pref_find_locked(const char *key)
{
    for (int i = 0; i < preferences_count; i++)
        if (strcmp(preferences[i].key, key) == 0)
            return &preferences[i];
    return NULL;
}

static pref_entry *pref_set_locked(const char *key, enum pref_type type)
{
    pref_entry *entry = pref_find_locked(key);
    if (!entry) {
        if (preferences_count >= MAX_PREFS)
            return NULL;
        entry = &preferences[preferences_count++];
        memset(entry, 0, sizeof *entry);
        entry->key = strdup(key);
    }
    if (entry->type == PREF_STRING) {
        free(entry->string);
        entry->string = NULL;
    }
    entry->type = type;
    return entry;
}

static int pref_write(FILE *file, const void *data, size_t bytes)
{
    return fwrite(data, 1, bytes, file) == bytes;
}

static int pref_read(FILE *file, void *data, size_t bytes)
{
    return fread(data, 1, bytes, file) == bytes;
}

static int preferences_save_locked(void)
{
    char path[1200];
    char temporary[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", bc_home);
    snprintf(temporary, sizeof temporary,
             "%s/shared-preferences.bin.tmp", bc_home);
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return 0;

    static const unsigned char magic[8] =
        { 'H', 'G', 'O', 'P', 'R', 'E', 'F', '1' };
    uint32_t count = (uint32_t)preferences_count;
    int ok = pref_write(file, magic, sizeof magic) &&
             pref_write(file, &count, sizeof count);
    for (int i = 0; ok && i < preferences_count; i++) {
        pref_entry *entry = &preferences[i];
        uint8_t type = (uint8_t)entry->type;
        uint32_t key_length = (uint32_t)strlen(entry->key);
        uint32_t value_length =
            entry->type == PREF_STRING
                ? (uint32_t)strlen(entry->string ? entry->string : "")
                : (uint32_t)sizeof(uint32_t);
        ok = pref_write(file, &type, sizeof type) &&
             pref_write(file, &key_length, sizeof key_length) &&
             pref_write(file, &value_length, sizeof value_length) &&
             pref_write(file, entry->key, key_length);
        if (!ok)
            break;
        if (entry->type == PREF_STRING)
            ok = pref_write(file, entry->string ? entry->string : "",
                            value_length);
        else if (entry->type == PREF_FLOAT)
            ok = pref_write(file, &entry->real, sizeof entry->real);
        else
            ok = pref_write(file, &entry->integer, sizeof entry->integer);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = 0;
    if (fclose(file) != 0)
        ok = 0;
    if (ok && rename(temporary, path) != 0)
        ok = 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

static void preferences_load_locked(void)
{
    if (preferences_loaded)
        return;
    preferences_loaded = 1;

    char path[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", bc_home);
    FILE *file = fopen(path, "rb");
    if (!file)
        return;

    unsigned char magic[8];
    static const unsigned char expected[8] =
        { 'H', 'G', 'O', 'P', 'R', 'E', 'F', '1' };
    uint32_t count;
    if (!pref_read(file, magic, sizeof magic) ||
        memcmp(magic, expected, sizeof magic) != 0 ||
        !pref_read(file, &count, sizeof count) ||
        count > MAX_PREFS) {
        fclose(file);
        nx_log("prefs: ignored invalid %s", path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t type;
        uint32_t key_length;
        uint32_t value_length;
        if (!pref_read(file, &type, sizeof type) ||
            !pref_read(file, &key_length, sizeof key_length) ||
            !pref_read(file, &value_length, sizeof value_length) ||
            key_length == 0 || key_length > 4096 ||
            value_length > 1024 * 1024 ||
            type < PREF_INT || type > PREF_BOOL)
            break;
        char *key = calloc((size_t)key_length + 1, 1);
        char *value = calloc((size_t)value_length + 1, 1);
        if (!key || !value ||
            !pref_read(file, key, key_length) ||
            !pref_read(file, value, value_length)) {
            free(key);
            free(value);
            break;
        }
        pref_entry *entry = pref_set_locked(key, (enum pref_type)type);
        if (entry) {
            if (type == PREF_STRING) {
                entry->string = value;
                value = NULL;
            } else if (value_length == sizeof(uint32_t)) {
                if (type == PREF_FLOAT)
                    memcpy(&entry->real, value, sizeof entry->real);
                else
                    memcpy(&entry->integer, value, sizeof entry->integer);
            }
        }
        free(key);
        free(value);
    }
    fclose(file);
    nx_log("prefs: loaded %d persistent entries", preferences_count);
}

/* Acesso ao PlayerPrefs de dentro do port.  Usado pelo conserto de
 * LevelStart.UpdateLevelCompletion (input.c): o fim de fase escreve o progresso
 * aqui, e a chave "Progress" e' texto puro no nosso armazenamento — a Unity
 * urlencoda valor no Android, entao a virgula chega como %2C. */
int bc_prefs_get_string(const char *key, char *out, size_t size)
{
    int ok = 0;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(key);
    if (entry && entry->type == PREF_STRING && entry->string && size) {
        snprintf(out, size, "%s", entry->string);
        ok = 1;
    }
    pthread_mutex_unlock(&preferences_lock);
    return ok;
}

int bc_prefs_set_string(const char *key, const char *value)
{
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(key, PREF_STRING);
    if (entry)
        entry->string = strdup(value ? value : "");
    int ok = entry != NULL && preferences_save_locked();
    pthread_mutex_unlock(&preferences_lock);
    return ok;
}

static int64_t j_getPreferences(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_object;
}

static int64_t j_Prefs_getBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    if (strcmp(k, "gles-api-check") == 0)
        return 1;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int value = entry && entry->type == PREF_BOOL
        ? entry->integer != 0 : dflt != 0;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getBoolean(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int32_t value = entry && entry->type == PREF_INT
        ? entry->integer : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getInt(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float dflt = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    float value = entry && entry->type == PREF_FLOAT
        ? entry->real : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getFloat(\"%s\", %.3f) -> %.3f", k, dflt, value);
    return jfloat_result(value);
}

static int64_t j_Prefs_getString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *dflt = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    jobj *value = entry && entry->type == PREF_STRING
        ? mk_string(entry->string ? entry->string : "") : dflt;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)value;
}

static int64_t j_Prefs_contains(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int value = pref_find_locked(k) != NULL;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.contains(\"%s\") -> %d", k, value);
    return value;
}

static int64_t j_Prefs_edit(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_INT);
    if (entry)
        entry->integer = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putInt(\"%s\", %d)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float value = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_FLOAT);
    if (entry)
        entry->real = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putFloat(\"%s\", %.3f)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *value = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    const char *v = value && value->str ? value->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_STRING);
    if (entry)
        entry->string = strdup(v);
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putString(\"%s\", %d bytes)", k, (int)strlen(v));
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_BOOL);
    if (entry)
        entry->integer = value != 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_remove(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        if (strcmp(preferences[i].key, k) != 0)
            continue;
        free(preferences[i].key);
        free(preferences[i].string);
        memmove(&preferences[i], &preferences[i + 1],
                (size_t)(preferences_count - i - 1) *
                    sizeof preferences[0]);
        preferences_count--;
        break;
    }
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_clear(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        free(preferences[i].key);
        free(preferences[i].string);
    }
    preferences_count = 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_apply(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int ok = preferences_save_locked();
    pthread_mutex_unlock(&preferences_lock);
    if (!ok)
        nx_log("prefs: cannot persist settings under %s", bc_home);
    return 0;
}

static int64_t j_Prefs_commit(jctx *c)
{
    j_Prefs_apply(c);
    return 1;
}

static int64_t j_Object_getClass(jctx *c)
{
    return (int64_t)(uintptr_t)mk_class(
        c->self && c->self->cls ? c->self->cls : "java/lang/Object");
}

static int64_t j_getClassLoader(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("java/lang/ClassLoader");
}

static int64_t j_ClassLoader_findLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *lib = name && name->str ? name->str : "";
    const char *file = NULL;

    if (strcmp(lib, "il2cpp") == 0 || strcmp(lib, "libil2cpp.so") == 0)
        file = "libil2cpp.so";
    else if (strcmp(lib, "main") == 0 || strcmp(lib, "libmain.so") == 0)
        file = "libmain.so";
    else if (strcmp(lib, "unity") == 0 || strcmp(lib, "libunity.so") == 0)
        file = "libunity.so";

    if (!file) {
        JT("ClassLoader.findLibrary(\"%s\") -> empty", lib);
        return (int64_t)(uintptr_t)mk_string("");
    }

    char path[1280];
    snprintf(path, sizeof path, "%s/lib/%s", bc_gamedir, file);
    JT("ClassLoader.findLibrary(\"%s\") -> %s", lib, path);
    return (int64_t)(uintptr_t)mk_string(path);
}

static int64_t j_System_loadLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *requested = name && name->str ? name->str : "";
    const char *base = strrchr(requested, '/');
    base = base ? base + 1 : requested;

    char soname[128];
    if (strstr(base, ".so"))
        snprintf(soname, sizeof soname, "%s", base);
    else if (strncmp(base, "lib", 3) == 0)
        snprintf(soname, sizeof soname, "%s.so", base);
    else
        snprintf(soname, sizeof soname, "lib%s.so", base);

    nx_mod *module = nx_find_mod(soname);
    if (!module) {
        JT("System.%s(\"%s\") -> module unavailable",
           c->m && c->m->name ? c->m->name : "loadLibrary", requested);
        return 0;
    }

    nx_run_init(module);
    JT("System.%s(\"%s\") -> %s initialized",
       c->m && c->m->name ? c->m->name : "loadLibrary", requested,
       soname);
    return 0;
}

static int64_t j_forName(jctx *c)
{
    jobj *name = jarg_obj(c);
    (void)jarg_int(c);
    (void)jarg_obj(c);
    const char *n = name && name->str ? name->str : "java/lang/Object";
    char cls[256];
    snprintf(cls, sizeof cls, "%s", n);
    for (char *p = cls; *p; p++)
        if (*p == '.')
            *p = '/';
    return (int64_t)(uintptr_t)mk_class(cls);
}

static int64_t j_newInterfaceProxy(jctx *c)
{
    int64_t handle = jarg_long(c);
    jobj *interfaces = jarg_obj(c);
    jobj *proxy = mk_object("bitter/jnibridge/Proxy");
    proxy->prim = handle;
    proxy->len = (int)proxy_interfaces(interfaces);
    JT("newInterfaceProxy handle=%#llx flags=%#x -> %p",
       (unsigned long long)handle, proxy->len, (void *)proxy);
    if ((proxy->len & (PROXY_HANDLER_CALLBACK | PROXY_FRAME_CALLBACK)) ==
        (PROXY_HANDLER_CALLBACK | PROXY_FRAME_CALLBACK)) {
        __atomic_store_n(&choreo_proxy, proxy, __ATOMIC_RELEASE);
        nx_log("jni: UnityChoreographer proxy captured handle=%#llx",
               (unsigned long long)handle);
    }
    return (int64_t)(uintptr_t)proxy;
}

static int64_t j_Thread_start(jctx *c)
{
    if (!c->self || !c->self->cls ||
        strcmp(c->self->cls, "android/os/HandlerThread") != 0 ||
        !c->self->str || strcmp(c->self->str, "UnityChoreographer") != 0)
        return 0;

    int expected = 0;
    if (!__atomic_compare_exchange_n(&choreo_thread_started, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;

    pthread_t thread;
    int rc = pthread_create(&thread, NULL, choreographer_driver, NULL);
    if (rc != 0) {
        __atomic_store_n(&choreo_thread_started, 0, __ATOMIC_RELEASE);
        nx_log("jni: cannot start UnityChoreographer HandlerThread: %s",
               strerror(rc));
        return 0;
    }
    pthread_detach(thread);
    nx_log("jni: HandlerThread.start(\"UnityChoreographer\")");
    return 0;
}

static int64_t j_FMOD_start(jctx *c)
{
    if (c->self)
        fmod_device_object = c->self;
    __atomic_store_n(&fmod_should_run, 1, __ATOMIC_RELEASE);
    nx_log("audio: FMODAudioDevice.start -> native AudioTrack requested");
    return 0;
}

static int64_t j_FMOD_stop(jctx *c)
{
    (void)c;
    __atomic_store_n(&fmod_should_run, 0, __ATOMIC_RELEASE);
    nx_log("audio: FMODAudioDevice.%s -> native AudioTrack stopped",
           c->m && c->m->name ? c->m->name : "stop");
    return 0;
}

static int64_t j_FMOD_isRunning(jctx *c)
{
    (void)c;
    return bc_jni_fmod_should_run() ? 1 : 0;
}

static int64_t j_HandlerThread_getLooper(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)looper_object;
}

static int64_t j_Looper_get(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)looper_object;
}

static int64_t j_Handler_obtainMessage(jctx *c)
{
    (void)c;
    message_what = jarg_int(c);
    message_object->prim = message_what;
    JT("Handler.obtainMessage(%d) -> %p", message_what,
       (void *)message_object);
    return (int64_t)(uintptr_t)message_object;
}

static int64_t j_Message_sendToTarget(jctx *c)
{
    (void)c;
    __atomic_store_n(&message_pending, 1, __ATOMIC_RELEASE);
    nx_log("jni: Message.sendToTarget queued for UnityChoreographer");
    return 0;
}

static int64_t j_System_nanoTime(jctx *c)
{
    (void)c;
    return monotonic_nanos();
}

static int64_t j_Choreographer_getInstance(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)choreographer_object;
}

static int64_t j_Choreographer_postFrameCallback(jctx *c)
{
    /* doFrame is paced by choreographer_driver.  The callback remains the same
     * Java proxy; posting is one-shot on Android and the driver likewise calls
     * it once per tick. */
    jobj *callback = jarg_obj(c);
    if (callback && (callback->len & PROXY_FRAME_CALLBACK))
        __atomic_store_n(&choreo_proxy, callback, __ATOMIC_RELEASE);
    return 0;
}

static int64_t j_Method_getName(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}

static int64_t j_runOnUiThread(jctx *c)
{
    jobj *runnable = jarg_obj(c);
    if (!runnable || !(runnable->len & PROXY_RUNNABLE))
        return 0;
    jobj *iface = mk_class("java/lang/Runnable");
    JT("runOnUiThread: invoking proxy=%p handle=%#llx", (void *)runnable,
       (unsigned long long)runnable->prim);
    (void)invoke_proxy(runnable, iface, run_method, empty_args);
    return 0;
}

static int64_t j_Unity_showSoftInput(jctx *c)
{
    if (c->self)
        unity_player_object = c->self;
    jobj *initial = jarg_obj(c);
    int keyboard_type = jarg_int(c);
    int autocorrection = jarg_int(c);
    int multiline = jarg_int(c);
    int secure = jarg_int(c);
    int alert = jarg_int(c);
    jobj *placeholder = jarg_obj(c);
    int character_limit = jarg_int(c);
    int hide_input = jarg_int(c);
    int close_on_outside_tap = jarg_int(c);
    (void)autocorrection;
    (void)multiline;
    (void)secure;
    (void)alert;
    (void)placeholder;
    (void)hide_input;
    (void)close_on_outside_tap;

    nx_log("jni: showSoftInput(type=%d limit=%d)", keyboard_type,
           character_limit);
    bc_input_keyboard_open(
        initial && initial->str ? initial->str : "", character_limit);
    return 0;
}

static int64_t j_Unity_setSoftInputStr(jctx *c)
{
    jobj *text = jarg_obj(c);
    bc_input_keyboard_set(text && text->str ? text->str : "");
    return 0;
}

static int64_t j_Unity_hideSoftInput(jctx *c)
{
    (void)c;
    bc_input_keyboard_hide();
    return 0;
}

static int64_t dispatch(void *e, jobj *self, void *mid, va_list *ap,
                        const uint64_t *args)
{
    const jmethod *m = by_id(mid);
    if (!m) {
        JT("call on unknown method id %p", mid);
        return 0;
    }
    if (self && self->boxed && is_unbox(m->name)) {
        JT("unboxed %s.%s -> %lld", self->cls, m->name, (long long)self->prim);
        return self->prim;
    }
    if (m->handler) {
        jctx c = {
            .env = e,
            .self = self,
            .ap = ap,
            .args = args,
            .arg_index = 0,
            .m = m,
        };
        return ((bc_jni_handler)m->handler)(&c);
    }
    /* Skateboard Party 3 consulta o memory-class do Android via ReflectionHelper
     * (java/lang/Object.GetMemoryClass()I) e o usa como divisor no calculo do
     * tamanho do cache do banco de dados.  Sem handler, o dispatch devolve 0
     * para retorno int -> divisao por zero -> +Inf (0x7F800000) -> alocacao de
     * 2 GB em SkateboardGameDatabase.get_Instance -> OOM.  Devolver um valor
     * plausivel (MB) mantem o cache finito. */
    if (strcmp(m->name, "GetMemoryClass") == 0 ||
        strcmp(m->name, "getMemoryClass") == 0 ||
        strcmp(m->name, "getLargeMemoryClass") == 0)
        return 256;
    /* Display: o jogo pergunta largura/altura/refreshRate ao android.view.Display
     * e o shim devolvia 0.  Divisoes por essas dimensoes (ou por refreshRate em
     * calculo de buffer de frames) viravam +Inf -> OOM 2 GB.  Devolver os valores
     * reais do framebuffer. */
    if (strcmp(m->name, "getRefreshRate") == 0)
        return jfloat_result(60.0f);
    if (strcmp(m->name, "getWidth") == 0)
        return 1280;
    if (strcmp(m->name, "getHeight") == 0)
        return 720;
    if (strcmp(m->name, "getRealWidth") == 0)
        return 1280;
    if (strcmp(m->name, "getRealHeight") == 0)
        return 720;
    /* Display timing: o jogo pode dividir por esses valores (cast pra float).
     * getAppVsyncOffsetNanos=0 -> divisao por zero -> +Inf -> 0x7F800000. */
    if (strcmp(m->name, "getAppVsyncOffsetNanos") == 0)
        return 1000000;   /* 1ms em nanos */
    if (strcmp(m->name, "getPresentationDeadlineNanos") == 0)
        return 16000000;  /* ~16ms em nanos */
    /* PackageInfo.versionCode=0 do stub -> divisao por zero -> OOM. Build 66. */
    if (strcmp(m->name, "versionCode") == 0)
        return 66;
    /* MotionEvent.getAxisValue=0.0 (sem touch no init) -> divisor -> +Inf. */
    if (strcmp(m->name, "getAxisValue") == 0)
        return jfloat_result(0.5f);
    /* Java inheritance is implicit on Android, while this deliberately small
     * registry keys reflected methods by the concrete runtime class.  Unity's
     * AndroidJavaObject signature builder calls Object.getClass() for every
     * reference argument and Class.getName() on the result.  Reproduce those
     * two inherited contracts generically so a ContentResolver (or any later
     * Android object) reports its real runtime type instead of becoming NULL. */
    if (self && self->cls && strcmp(m->name, "getClass") == 0)
        return (int64_t)(uintptr_t)mk_class(self->cls);
    if (self && self->type == O_CLASS &&
        (strcmp(m->name, "getName") == 0 ||
         strcmp(m->name, "getCanonicalName") == 0 ||
         strcmp(m->name, "getTypeName") == 0)) {
        char name[512];
        snprintf(name, sizeof name, "%s", self->cls ? self->cls
                                                       : "java/lang/Object");
        for (char *p = name; *p; p++)
            if (*p == '/')
                *p = '.';
        return (int64_t)(uintptr_t)mk_string(name);
    }
    /* A method we do not implement whose return type is the receiver's own
     * class is a builder -- Intent.putExtra, setPackage, setFlags, setData all
     * return the Intent so calls can be chained.  Returning NULL breaks the
     * chain mid-expression and every later link then operates on nothing;
     * returning the receiver is what the platform does. */
    if (self && self->cls) {
        const char *ret = strchr(m->sig, ')');
        size_t n = strlen(self->cls);
        if (ret && ret[1] == 'L' && strncmp(ret + 2, self->cls, n) == 0 &&
            ret[2 + n] == ';') {
            JT("builder %s.%s -> self", self->cls, m->name);
            return (int64_t)(uintptr_t)self;
        }
    }
    /* Peripheral Java services -- Play Games, GMS Tasks, platform helpers --
     * are not part of the game's own flow, but returning NULL for a call whose
     * declared return type is an object makes the managed caller dereference
     * null and abort the entire Awake() that started it.  That is exactly what
     * kept Bomb Chicken on a black preload screen: GamesSignInClient
     * .isAuthenticated() returned null, GooglePlayGames threw inside
     * LevelStart.Awake() and the scene never advanced.  Hand back a live object
     * of the declared type instead so the flow continues; the listener simply
     * never fires, which is what an offline sign-in looks like. */
    const char *rt = strchr(m->sig, ')');
    if (rt && !getenv("BC_JNI_STRICT_NULL")) {
        if (rt[1] == 'L') {
            char cls[256];
            const char *end = strchr(rt + 2, ';');
            size_t n = end ? (size_t)(end - (rt + 2)) : 0;
            if (n && n < sizeof cls) {
                memcpy(cls, rt + 2, n);
                cls[n] = 0;
                if (strcmp(cls, "java/lang/String") == 0) {
                    JT("unhandled %s.%s%s -> \"\"",
                       m->cls ? m->cls : "?", m->name, m->sig);
                    return (int64_t)(uintptr_t)mk_string("");
                }
                JT("unhandled %s.%s%s -> stub %s",
                   m->cls ? m->cls : "?", m->name, m->sig, cls);
                return (int64_t)(uintptr_t)mk_object(cls);
            }
        }
    }
    JT("unhandled %s.%s%s -> 0", m->cls ? m->cls : "?", m->name, m->sig);
    return 0;
}

/* Argument readers for handlers. */
jobj *bc_jarg_obj(jctx *c) { return jarg_obj(c); }
int32_t bc_jarg_int(jctx *c) { return jarg_int(c); }
int64_t bc_jarg_long(jctx *c) { return jarg_long(c); }
const char *bc_jarg_str(jctx *c)
{
    jobj *o = jarg_obj(c);
    return o && o->str ? o->str : NULL;
}
void *bc_jret_str(const char *s) { return mk_string(s); }
void *bc_jret_class(const char *s) { return mk_class(s); }
void *bc_jret_obj(const char *cls) { return new_obj(O_OBJECT, cls); }
void *bc_jni_activity(void) { return armory_activity_object; }
void *bc_jret_bytes(const void *p, int n)
{
    jobj *o = j_NewByteArray(NULL, n);
    if (p && n > 0)
        memcpy(o->data, p, (size_t)n);
    return o;
}

#define CALLV(nm, rt, cast) \
    static rt nm(void *e, void *o, void *mid, va_list ap) \
    { return (rt)(cast)dispatch(e, o, mid, &ap, NULL); }
#define CALL(nm, vnm, rt) \
    static rt nm(void *e, void *o, void *mid, ...) \
    { va_list ap; va_start(ap, mid); rt r = vnm(e, o, mid, ap); va_end(ap); return r; }
#define CALLA(nm, rt) \
    static rt nm(void *e, void *o, void *mid, void *args) \
    { return (rt)dispatch(e, o, mid, NULL, (const uint64_t *)args); }

CALLV(v_obj,  void *,   uintptr_t)
CALLV(v_bool, uint8_t,  uint8_t)
CALLV(v_byte, int8_t,   int8_t)
CALLV(v_char, uint16_t, uint16_t)
CALLV(v_short,int16_t,  int16_t)
CALLV(v_int,  int32_t,  int32_t)
CALLV(v_long, int64_t,  int64_t)

static float v_float(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    float f;
    memcpy(&f, &r, sizeof f);
    if (bc_trace_jni && f == 0.0f) {
        const jmethod *m = by_id(mid);
        fprintf(stderr, "[bc/jni-float0] %s.%s%s -> 0.0\n",
                m && m->cls ? m->cls : "?", m ? m->name : "?", m ? m->sig : "?");
    }
    return f;
}
static double v_double(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void v_void(void *e, void *o, void *mid, va_list ap)
{
    dispatch(e, o, mid, &ap, NULL);
}

CALL(c_obj, v_obj, void *)
CALL(c_bool, v_bool, uint8_t)
CALL(c_byte, v_byte, int8_t)
CALL(c_char, v_char, uint16_t)
CALL(c_short, v_short, int16_t)
CALL(c_int, v_int, int32_t)
CALL(c_long, v_long, int64_t)
CALL(c_float, v_float, float)
CALL(c_double, v_double, double)
static void c_void(void *e, void *o, void *mid, ...)
{
    va_list ap;
    va_start(ap, mid);
    v_void(e, o, mid, ap);
    va_end(ap);
}

CALLA(a_obj, void *)
CALLA(a_bool, uint8_t)
CALLA(a_byte, int8_t)
CALLA(a_char, uint16_t)
CALLA(a_short, int16_t)
CALLA(a_int, int32_t)
CALLA(a_long, int64_t)
static float a_float(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    float f;
    memcpy(&f, &r, sizeof f);
    return f;
}
static double a_double(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void a_void(void *e, void *o, void *m, void *v)
{
    (void)dispatch(e, o, m, NULL, (const uint64_t *)v);
}

static void *j_GetMethodID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}
static void *j_GetFieldID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}

static jobj *j_AllocObject(void *e, jobj *c)
{
    (void)e;
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    return new_obj(O_OBJECT, c ? c->cls : NULL);
}

/* Boxed JNI primitives have to retain their value for later longValue(),
 * intValue() and related calls. */
static int box_kind(const char *cls)
{
    if (!cls)
        return 0;
    if (strcmp(cls, "java/lang/Long") == 0)      return 'J';
    if (strcmp(cls, "java/lang/Integer") == 0)   return 'I';
    if (strcmp(cls, "java/lang/Short") == 0)     return 'S';
    if (strcmp(cls, "java/lang/Byte") == 0)      return 'B';
    if (strcmp(cls, "java/lang/Character") == 0) return 'C';
    if (strcmp(cls, "java/lang/Boolean") == 0)   return 'Z';
    if (strcmp(cls, "java/lang/Float") == 0)     return 'F';
    if (strcmp(cls, "java/lang/Double") == 0)    return 'D';
    return 0;
}

static jobj *box_from(jobj *c, void *mid, va_list *ap)
{
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        /* Not a boxed primitive.  Keep a single String argument: java/io/File
         * and android/content/Intent are both built from one and then asked
         * about it, and an object that forgot its own path is indistinguishable
         * from a file that does not exist. */
        const jmethod *m = by_id(mid);
        if (m && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = va_arg(*ap, jobj *);
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            /* Unity marshals managed UTF-8 strings by constructing a Java
             * String from byte[] plus the "UTF-8" charset name.  Dropping
             * that byte array turns every PlayerPrefs key into "", causing
             * all settings -- including the four audio volumes -- to alias. */
            jobj *bytes = va_arg(*ap, jobj *);
            if (strcmp(m->sig, "([BLjava/lang/String;)V") == 0)
                (void)va_arg(*ap, jobj *);
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/util/Scanner") == 0 &&
                   strcmp(m->sig,
                          "(Ljava/io/InputStream;Ljava/lang/String;)V") == 0) {
            jobj *stream = va_arg(*ap, jobj *);
            (void)va_arg(*ap, jobj *);
            if (stream && stream->str) {
                o->str = strdup(stream->str);
                o->len = (int)strlen(o->str);
            }
        }
        JT("new %s(\"%s\")", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    o->boxed = k;
    switch (k) {
    case 'J': o->prim = va_arg(*ap, int64_t); break;
    case 'I': o->prim = va_arg(*ap, int32_t); break;
    /* short, byte, char and boolean are promoted to int in a varargs call. */
    case 'S': o->prim = (int16_t)va_arg(*ap, int32_t); break;
    case 'B': o->prim = (int8_t)va_arg(*ap, int32_t); break;
    case 'C': o->prim = (uint16_t)va_arg(*ap, int32_t); break;
    case 'Z': o->prim = va_arg(*ap, int32_t) ? 1 : 0; break;
    case 'F': { double f = va_arg(*ap, double); float g = (float)f;
                memcpy(&o->prim, &g, sizeof g); break; }
    case 'D': { double f = va_arg(*ap, double);
                memcpy(&o->prim, &f, sizeof f); break; }
    default: break;
    }
    JT("boxed %s = %lld", o->cls, (long long)o->prim);
    return o;
}

static jobj *v_NewObject(void *e, jobj *c, void *mid, va_list ap)
{
    (void)e;
    return box_from(c, mid, &ap);
}
static jobj *j_NewObject(void *e, jobj *c, void *mid, ...)
{
    (void)e;
    va_list ap;
    va_start(ap, mid);
    jobj *o = box_from(c, mid, &ap);
    va_end(ap);
    return o;
}
static jobj *a_NewObject(void *e, jobj *c, void *mid, void *v)
{
    /* The A form passes a jvalue array, one slot per argument.  The VM builds
     * its java/io/File through this form and its Intent through the varargs
     * one, so the String argument has to be kept here too -- covering only the
     * varargs path leaves the File with no path at all. */
    (void)e;
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        const jmethod *m = by_id(mid);
        if (m && v && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = *(jobj **)v;
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && v && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            jobj *bytes = *(jobj **)v;
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        }
        JT("new %s(\"%s\") [A]", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    if (k && v) {
        o->boxed = k;
        o->prim = *(const int64_t *)v;
        if (k == 'I') o->prim = (int32_t)o->prim;
        if (k == 'S') o->prim = (int16_t)o->prim;
        if (k == 'B') o->prim = (int8_t)o->prim;
        if (k == 'C') o->prim = (uint16_t)o->prim;
        if (k == 'Z') o->prim = o->prim ? 1 : 0;
    }
    return o;
}

/* Fields all go through the same dispatch, so a getter can be bound the same
 * way a method is; anything unbound reads as zero/NULL. */
static void *f_obj(void *e, jobj *o, void *fid)
{
    return (void *)(uintptr_t)dispatch(e, o, fid, NULL, NULL);
}
static uint8_t f_bool(void *e, jobj *o, void *f) { return (uint8_t)(uintptr_t)f_obj(e, o, f); }
static int8_t f_byte(void *e, jobj *o, void *f) { return (int8_t)(uintptr_t)f_obj(e, o, f); }
static uint16_t f_char(void *e, jobj *o, void *f) { return (uint16_t)(uintptr_t)f_obj(e, o, f); }
static int16_t f_short(void *e, jobj *o, void *f) { return (int16_t)(uintptr_t)f_obj(e, o, f); }
static int32_t f_int(void *e, jobj *o, void *f) { return (int32_t)(uintptr_t)f_obj(e, o, f); }
static int64_t f_long(void *e, jobj *o, void *f) { return (int64_t)(uintptr_t)f_obj(e, o, f); }
static float f_float(void *e, jobj *o, void *f) {
    if (bc_trace_jni) {
        const jmethod *m = by_id(f);
        fprintf(stderr, "[bc/jni-field-float] %s.%s -> 1.0 (was 0.0!)\n",
                m && m->cls ? m->cls : "?", m ? m->name : "?");
    }
    return 1.0f;
}
static double f_double(void *e, jobj *o, void *f) {
    if (bc_trace_jni) {
        const jmethod *m = by_id(f);
        fprintf(stderr, "[bc/jni-field-double] %s.%s -> 1.0 (was 0.0!)\n",
                m && m->cls ? m->cls : "?", m ? m->name : "?");
    }
    return 1.0;
}
static void f_set(void *e, jobj *o, void *f, ...) { (void)e; (void)o; (void)f; }

static int32_t j_RegisterNatives(void *e, jobj *c, const void *m, int32_t n)
{
    (void)e;
    const struct { const char *name; const char *sig; void *fn; } *r = m;
    for (int32_t i = 0; i < n; i++) {
        if (natives_n == MAX_NATIVES)
            nx_die("native table full");
        jnative *k = &natives[natives_n++];
        snprintf(k->cls, sizeof k->cls, "%s", c && c->cls ? c->cls : "?");
        snprintf(k->name, sizeof k->name, "%s", r[i].name);
        snprintf(k->sig, sizeof k->sig, "%s", r[i].sig);
        k->fn = r[i].fn;
        nx_log("RegisterNatives %s.%s%s -> %p", k->cls, k->name, k->sig, k->fn);
    }
    return 0;
}
static int32_t j_UnregisterNatives(void *e, jobj *c) { (void)e; (void)c; return 0; }

static void *j_FromReflected(void *e, void *o)
{
    (void)e;
    if (o == doframe_method)
        return doframe_mid;
    if (o == handlemsg_method)
        return handlemsg_mid;
    if (o == run_method)
        return run_mid;
    return o;
}
static void *j_ToReflected(void *e, jobj *c, void *m, uint8_t st)
{
    (void)e; (void)c; (void)st;
    return m;
}
static jobj *j_DefineClass(void *e, const char *n, void *l, const void *b, int32_t sz)
{
    (void)e; (void)l; (void)b; (void)sz;
    return mk_class(n ? n : "?");
}

/* --- JavaVM ----------------------------------------------------------- */

static int32_t vm_GetEnv(void *vm, void **out, int32_t ver)
{
    (void)vm; (void)ver;
    *out = bc_jni_env();
    return 0;
}
static int32_t vm_AttachCurrentThread(void *vm, void **out, void *args)
{
    (void)vm; (void)args;
    *out = bc_jni_env();
    return 0;
}
static int32_t vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }

/* ------------------------------------------------------------------- build */

void bc_jni_init(void)
{
    for (int i = 0; i < JNI_SLOT_COUNT; i++)
        vt[i] = NULL;

    vt[JNI_GetVersion] = j_GetVersion;
    vt[JNI_DefineClass] = j_DefineClass;
    vt[JNI_FindClass] = j_FindClass;
    vt[JNI_FromReflectedMethod] = j_FromReflected;
    vt[JNI_FromReflectedField] = j_FromReflected;
    vt[JNI_ToReflectedMethod] = j_ToReflected;
    vt[JNI_GetSuperclass] = j_GetSuperclass;
    vt[JNI_IsAssignableFrom] = j_IsAssignableFrom;
    vt[JNI_ToReflectedField] = j_ToReflected;
    vt[JNI_Throw] = j_Throw;
    vt[JNI_ThrowNew] = j_ThrowNew;
    vt[JNI_ExceptionOccurred] = j_ExceptionOccurred;
    vt[JNI_ExceptionDescribe] = j_ExceptionDescribe;
    vt[JNI_ExceptionClear] = j_ExceptionClear;
    vt[JNI_FatalError] = j_FatalError;
    vt[JNI_PushLocalFrame] = j_PushLocalFrame;
    vt[JNI_PopLocalFrame] = j_PopLocalFrame;
    vt[JNI_NewGlobalRef] = j_NewGlobalRef;
    vt[JNI_DeleteGlobalRef] = j_DeleteRef;
    vt[JNI_DeleteLocalRef] = j_DeleteRef;
    vt[JNI_IsSameObject] = j_IsSameObject;
    vt[JNI_NewLocalRef] = j_NewLocalRef;
    vt[JNI_EnsureLocalCapacity] = j_EnsureLocalCapacity;
    vt[JNI_AllocObject] = j_AllocObject;
    vt[JNI_NewObject] = j_NewObject;
    vt[JNI_NewObjectV] = v_NewObject;
    vt[JNI_NewObjectA] = a_NewObject;
    vt[JNI_GetObjectClass] = j_GetObjectClass;
    vt[JNI_IsInstanceOf] = j_IsInstanceOf;
    vt[JNI_GetMethodID] = j_GetMethodID;

    /* Call<Type>Method / ...V / ...A, then the Nonvirtual and Static blocks:
     * same handlers, the dispatcher does not care which flavour was used. */
    static void *const call3[10][3] = {
        { c_obj, v_obj, a_obj },       { c_bool, v_bool, a_bool },
        { c_byte, v_byte, a_byte },    { c_char, v_char, a_char },
        { c_short, v_short, a_short }, { c_int, v_int, a_int },
        { c_long, v_long, a_long },    { c_float, v_float, a_float },
        { c_double, v_double, a_double }, { c_void, v_void, a_void },
    };
    for (int t = 0; t < 10; t++)
        for (int k = 0; k < 3; k++) {
            vt[JNI_CallObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallNonvirtualObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallStaticObjectMethod + t * 3 + k] = call3[t][k];
        }

    vt[JNI_GetFieldID] = j_GetFieldID;
    void *const getf[9] = { f_obj, f_bool, f_byte, f_char, f_short, f_int,
                            f_long, f_float, f_double };
    for (int i = 0; i < 9; i++) {
        vt[JNI_GetObjectField + i] = getf[i];
        vt[JNI_GetStaticObjectField + i] = getf[i];
        vt[JNI_SetObjectField + i] = f_set;
        vt[JNI_SetStaticObjectField + i] = f_set;
    }
    vt[JNI_GetStaticMethodID] = j_GetMethodID;
    vt[JNI_GetStaticFieldID] = j_GetFieldID;

    vt[JNI_NewString] = j_NewString;
    vt[JNI_GetStringLength] = j_GetStringLength;
    vt[JNI_GetStringChars] = j_GetStringChars;
    vt[JNI_ReleaseStringChars] = j_ReleaseStringChars;
    vt[JNI_NewStringUTF] = j_NewStringUTF;
    vt[JNI_GetStringUTFLength] = j_GetStringUTFLength;
    vt[JNI_GetStringUTFChars] = j_GetStringUTFChars;
    vt[JNI_ReleaseStringUTFChars] = j_ReleaseStringUTFChars;
    vt[JNI_GetStringRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringUTFRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringCritical] = j_GetStringChars;
    vt[JNI_ReleaseStringCritical] = j_ReleaseStringChars;

    vt[JNI_GetArrayLength] = j_GetArrayLength;
    vt[JNI_NewObjectArray] = j_NewObjectArray;
    vt[JNI_GetObjectArrayElement] = j_GetObjectArrayElement;
    vt[JNI_SetObjectArrayElement] = j_SetObjectArrayElement;
    for (int i = 0; i < 8; i++) {
        vt[JNI_NewBooleanArray + i] = j_NewByteArray;
        vt[JNI_GetBooleanArrayElements + i] = j_GetByteArrayElements;
        vt[JNI_ReleaseBooleanArrayElements + i] = j_ReleaseArrayElements;
        vt[JNI_GetBooleanArrayRegion + i] = j_GetByteArrayRegion;
        vt[JNI_SetBooleanArrayRegion + i] = j_SetByteArrayRegion;
    }
    /* Primitive-array slots are adjacent, but element width is not.  The
     * gamepad enumerator returns int[], so give that type its real byte stride. */
    vt[JNI_NewBooleanArray + 4] = j_NewIntArray;
    vt[JNI_GetBooleanArrayRegion + 4] = j_GetIntArrayRegion;
    vt[JNI_SetBooleanArrayRegion + 4] = j_SetIntArrayRegion;
    vt[JNI_GetPrimitiveArrayCritical] = j_GetPrimitiveArrayCritical;
    vt[JNI_ReleasePrimitiveArrayCritical] = j_ReleasePrimitiveArrayCritical;

    vt[JNI_RegisterNatives] = j_RegisterNatives;
    vt[JNI_UnregisterNatives] = j_UnregisterNatives;
    vt[JNI_MonitorEnter] = j_MonitorEnter;
    vt[JNI_MonitorExit] = j_MonitorExit;
    vt[JNI_GetJavaVM] = j_GetJavaVM;
    vt[JNI_NewWeakGlobalRef] = j_NewWeakGlobalRef;
    vt[JNI_DeleteWeakGlobalRef] = j_DeleteRef;
    vt[JNI_ExceptionCheck] = j_ExceptionCheck;
    vt[JNI_NewDirectByteBuffer] = j_NewDirectByteBuffer;
    vt[JNI_GetDirectBufferAddress] = j_GetDirectBufferAddress;
    vt[JNI_GetDirectBufferCapacity] = j_GetDirectBufferCapacity;
    vt[JNI_GetObjectRefType] = j_GetObjectRefType;

    jvm_vt[3] = vm_DestroyJavaVM;
    jvm_vt[4] = vm_AttachCurrentThread;
    jvm_vt[5] = vm_DetachCurrentThread;
    jvm_vt[6] = vm_GetEnv;
    jvm_vt[7] = vm_AttachCurrentThread;   /* AttachCurrentThreadAsDaemon */

    looper_object = mk_object("android/os/Looper");
    choreographer_object = mk_object("android/view/Choreographer");
    message_object = mk_object("android/os/Message");

    doframe_method = mk_object("java/lang/reflect/Method");
    doframe_method->str = strdup("doFrame");
    handlemsg_method = mk_object("java/lang/reflect/Method");
    handlemsg_method->str = strdup("handleMessage");
    run_method = mk_object("java/lang/reflect/Method");
    run_method->str = strdup("run");

    frame_time_box = mk_object("java/lang/Long");
    frame_time_box->boxed = 'J';
    doframe_args = j_NewObjectArray(NULL, 1, mk_class("java/lang/Object"),
                                    frame_time_box);
    handlemsg_args = j_NewObjectArray(NULL, 1, mk_class("java/lang/Object"),
                                     message_object);
    empty_args = j_NewObjectArray(NULL, 0, mk_class("java/lang/Object"), NULL);

    /* The reflected methods handed to JNIBridge.invoke must map to the same
     * stable IDs that the native proxy obtained from these interfaces. */
    doframe_mid = method_id("android/view/Choreographer$FrameCallback",
                            "doFrame", "(J)V");
    handlemsg_mid = method_id("android/os/Handler$Callback", "handleMessage",
                              "(Landroid/os/Message;)Z");
    run_mid = method_id("java/lang/Runnable", "run", "()V");

    /* Bound up front rather than on demand so the VM's GetMethodID finds these
     * already carrying a handler; method_id matches on name and signature, so a
     * late bind would create a second, handler-less entry. */
    bc_jni_bind("bitter/jnibridge/JNIBridge", "newInterfaceProxy",
                 "(J[Ljava/lang/Class;)Ljava/lang/Object;",
                 (void *)j_newInterfaceProxy);
    bc_jni_bind("com/unity3d/player/ReflectionHelper", "getConstructorID",
                 "(Ljava/lang/Class;Ljava/lang/String;)"
                 "Ljava/lang/reflect/Constructor;",
                 (void *)j_Reflection_getConstructorID);
    bc_jni_bind("com/unity3d/player/ReflectionHelper", "getMethodID",
                 "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)"
                 "Ljava/lang/reflect/Method;",
                 (void *)j_Reflection_getMethodID);
    bc_jni_bind("com/unity3d/player/ReflectionHelper", "getFieldID",
                 "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)"
                 "Ljava/lang/reflect/Field;",
                 (void *)j_Reflection_getFieldID);
    bc_jni_bind("com/unity3d/player/ReflectionHelper", "getFieldSignature",
                 "(Ljava/lang/reflect/Field;)Ljava/lang/String;",
                 (void *)j_Reflection_getFieldSignature);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "currentActivity",
                 "Landroid/app/Activity;", (void *)j_Unity_currentActivity);
    /* AndroidJavaObject erases reference return/field types to Object in the
     * ReflectionHelper signature.  Keep the typed aliases above for direct
     * JNI users and publish the erased forms used by Hitman GO's managed
     * Armory layer. */
    bc_jni_bind("com/unity3d/player/UnityPlayer", "currentActivity",
                 "Ljava/lang/Object;", (void *)j_Unity_currentActivity);
    bc_jni_bind("java/lang/Thread", "start", "()V",
                 (void *)j_Thread_start);
    bc_jni_bind("org/fmod/FMODAudioDevice", "start", "()V",
                 (void *)j_FMOD_start);
    bc_jni_bind("org/fmod/FMODAudioDevice", "stop", "()V",
                 (void *)j_FMOD_stop);
    bc_jni_bind("org/fmod/FMODAudioDevice", "close", "()V",
                 (void *)j_FMOD_stop);
    bc_jni_bind("org/fmod/FMODAudioDevice", "isRunning", "()Z",
                 (void *)j_FMOD_isRunning);
    bc_jni_bind("android/os/HandlerThread", "getLooper",
                 "()Landroid/os/Looper;", (void *)j_HandlerThread_getLooper);
    bc_jni_bind("android/os/Looper", "getMainLooper",
                 "()Landroid/os/Looper;", (void *)j_Looper_get);
    bc_jni_bind("android/os/Looper", "myLooper",
                 "()Landroid/os/Looper;", (void *)j_Looper_get);
    bc_jni_bind("android/os/Handler", "obtainMessage",
                 "(I)Landroid/os/Message;", (void *)j_Handler_obtainMessage);
    bc_jni_bind("android/os/Message", "sendToTarget", "()V",
                 (void *)j_Message_sendToTarget);
    bc_jni_bind("java/lang/System", "nanoTime", "()J",
                 (void *)j_System_nanoTime);
    bc_jni_bind("android/view/Choreographer", "getInstance",
                 "()Landroid/view/Choreographer;",
                 (void *)j_Choreographer_getInstance);
    bc_jni_bind("android/view/Choreographer", "postFrameCallback",
                 "(Landroid/view/Choreographer$FrameCallback;)V",
                 (void *)j_Choreographer_postFrameCallback);
    bc_jni_bind("java/lang/reflect/Method", "getName",
                 "()Ljava/lang/String;", (void *)j_Method_getName);
    bc_jni_bind("android/app/Activity", "runOnUiThread",
                 "(Ljava/lang/Runnable;)V", (void *)j_runOnUiThread);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "showSoftInput",
                 "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V",
                 (void *)j_Unity_showSoftInput);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "setSoftInputStr",
                 "(Ljava/lang/String;)V",
                 (void *)j_Unity_setSoftInputStr);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "hideSoftInput", "()V",
                 (void *)j_Unity_hideSoftInput);

    input_device = mk_object("android/view/InputDevice");
    touch_device = mk_object("android/view/InputDevice");
    motion_range_list = mk_object("java/util/List");
    touch_motion_range_list = mk_object("java/util/List");
    motion_range_list->len = (int)(sizeof motion_ranges /
                                    sizeof *motion_ranges);
    motion_range_list->elems = calloc((size_t)motion_range_list->len,
                                      sizeof *motion_range_list->elems);
    for (int i = 0; i < motion_range_list->len; i++) {
        motion_ranges[i] = mk_object("android/view/InputDevice$MotionRange");
        motion_ranges[i]->prim = motion_axis_ids[i];
        motion_range_list->elems[i] = motion_ranges[i];
    }
    motion_range_iterator = mk_object("java/util/Iterator");
    key_event_object = mk_object("android/view/KeyEvent");
    motion_event_object = mk_object("android/view/MotionEvent");
    motion_event_object->data = &motion_event;
    fmod_device_object = mk_object("org/fmod/FMODAudioDevice");
    fmod_bytebuffer = j_NewDirectByteBuffer(NULL, fmod_pcm,
                                            fmod_buffer_size);
    preferences_object =
        mk_object("android/content/SharedPreferences");
    preferences_editor =
        mk_object("android/content/SharedPreferences$Editor");
    armory_activity_object =
        mk_object("com/unity3d/player/UnityPlayerActivity");
    permission_plugin_object =
        mk_object("com/squareenixmontreal/armory/PermissionPlugin");
    bc_jni_bind("android/view/InputDevice", "getDeviceIds", "()[I",
                 (void *)j_InputDevice_getDeviceIds);
    bc_jni_bind("android/view/InputDevice", "getDevice",
                 "(I)Landroid/view/InputDevice;",
                 (void *)j_InputDevice_getDevice);
    bc_jni_bind("android/view/InputEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    bc_jni_bind("android/view/InputDevice", "getName",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    bc_jni_bind("android/view/InputDevice", "getDescriptor",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    bc_jni_bind("android/view/InputDevice", "getVendorId", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "getProductId", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "getSources", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "getId", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "getControllerNumber", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "getKeyboardType", "()I",
                 (void *)j_InputDevice_getInt);
    bc_jni_bind("android/view/InputDevice", "supportsSource", "(I)Z",
                 (void *)j_InputDevice_supportsSource);
    bc_jni_bind("android/view/InputDevice", "getMotionRanges",
                 "()Ljava/util/List;", (void *)j_InputDevice_getMotionRanges);
    bc_jni_bind("android/view/InputDevice", "getMotionRange",
                 "(I)Landroid/view/InputDevice$MotionRange;",
                 (void *)j_InputDevice_getMotionRange);
    bc_jni_bind("java/util/List", "size", "()I", (void *)j_List_size);
    bc_jni_bind("java/util/List", "get", "(I)Ljava/lang/Object;",
                 (void *)j_List_get);
    bc_jni_bind("java/util/List", "iterator", "()Ljava/util/Iterator;",
                 (void *)j_List_iterator);
    bc_jni_bind("java/util/Iterator", "hasNext", "()Z",
                 (void *)j_Iterator_hasNext);
    bc_jni_bind("java/util/Iterator", "next", "()Ljava/lang/Object;",
                 (void *)j_Iterator_next);
    bc_jni_bind("android/view/InputDevice$MotionRange", "getAxis", "()I",
                 (void *)j_MotionRange_getInt);
    bc_jni_bind("android/view/InputDevice$MotionRange", "getSource", "()I",
                 (void *)j_MotionRange_getInt);
    static const char *const range_float_methods[] = {
        "getMin", "getMax", "getFlat", "getFuzz", "getResolution",
    };
    for (size_t i = 0; i < sizeof range_float_methods /
                            sizeof *range_float_methods; i++)
        bc_jni_bind("android/view/InputDevice$MotionRange",
                     range_float_methods[i], "()F",
                     (void *)j_MotionRange_getFloat);

    static const char *const key_int_methods[] = {
        "getAction", "getKeyCode", "getSource", "getDeviceId",
        "getMetaState", "getRepeatCount", "getScanCode", "getFlags",
        "getUnicodeChar",
    };
    for (size_t i = 0; i < sizeof key_int_methods / sizeof *key_int_methods; i++)
        bc_jni_bind("android/view/KeyEvent", key_int_methods[i], "()I",
                     (void *)j_KeyEvent_getInt);
    bc_jni_bind("android/view/KeyEvent", "getEventTime", "()J",
                 (void *)j_KeyEvent_getLong);
    bc_jni_bind("android/view/KeyEvent", "getDownTime", "()J",
                 (void *)j_KeyEvent_getLong);
    bc_jni_bind("android/view/KeyEvent", "isSystem", "()Z",
                 (void *)j_KeyEvent_isSystem);

    static const char *const motion_int_methods[] = {
        "getAction", "getActionMasked", "getActionIndex", "getSource",
        "getDeviceId", "getMetaState", "getButtonState", "getFlags",
        "getPointerCount", "getHistorySize", "getEdgeFlags",
        "getActionButton", "getDisplayId", "getClassification",
    };
    for (size_t i = 0; i < sizeof motion_int_methods /
                            sizeof *motion_int_methods; i++)
        bc_jni_bind("android/view/MotionEvent", motion_int_methods[i],
                     "()I", (void *)j_MotionEvent_getInt);
    static const char *const motion_int_arg_methods[] = {
        "getPointerId", "findPointerIndex", "getToolType",
    };
    for (size_t i = 0; i < sizeof motion_int_arg_methods /
                            sizeof *motion_int_arg_methods; i++)
        bc_jni_bind("android/view/MotionEvent", motion_int_arg_methods[i],
                     "(I)I", (void *)j_MotionEvent_getInt);
    bc_jni_bind("android/view/MotionEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);
    bc_jni_bind("android/view/MotionEvent", "getDownTime", "()J",
                 (void *)j_MotionEvent_getLong);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalEventTime", "(I)J",
                 (void *)j_MotionEvent_getLong);
    bc_jni_bind("android/view/MotionEvent", "getAxisValue", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getAxisValue", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(II)F", (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(III)F", (void *)j_MotionEvent_getFloat);
    static const char *const motion_xy_methods[] = {
        "getX", "getY", "getRawX", "getRawY", "getPressure", "getSize",
        "getTouchMajor", "getTouchMinor", "getToolMajor", "getToolMinor",
        "getOrientation",
    };
    for (size_t i = 0; i < sizeof motion_xy_methods /
                            sizeof *motion_xy_methods; i++) {
        bc_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "()F",
                     (void *)j_MotionEvent_getFloat);
        bc_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "(I)F",
                     (void *)j_MotionEvent_getFloat);
    }
    bc_jni_bind("android/view/MotionEvent", "getHistoricalX", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalX", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalY", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "getHistoricalY", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    bc_jni_bind("android/view/MotionEvent", "isFromSource", "(I)Z",
                 (void *)j_MotionEvent_isFromSource);
    bc_jni_bind("android/view/MotionEvent", "obtain",
                 "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;",
                 (void *)j_MotionEvent_obtain);
    bc_jni_bind("android/view/MotionEvent", "recycle", "()V",
                 (void *)j_MotionEvent_recycle);
    bc_jni_bind("android/view/MotionEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    bc_jni_bind("android/view/InputEvent", "getSource", "()I",
                 (void *)j_MotionEvent_getInt);
    bc_jni_bind("android/view/InputEvent", "getDeviceId", "()I",
                 (void *)j_MotionEvent_getInt);
    bc_jni_bind("android/view/InputEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);

    bc_jni_bind("java/io/File", "length", "()J", (void *)j_File_length);
    bc_jni_bind("java/io/File", "exists", "()Z", (void *)j_File_exists);
    bc_jni_bind("java/io/File", "getPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    bc_jni_bind("java/io/File", "getAbsolutePath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    bc_jni_bind("java/io/File", "getCanonicalPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    bc_jni_bind("java/io/File", "getParent", "()Ljava/lang/String;",
                 (void *)j_File_getParent);
    bc_jni_bind("java/io/File", "getParentFile", "()Ljava/io/File;",
                 (void *)j_File_getParentFile);
    bc_jni_bind("java/io/File", "isDirectory", "()Z",
                 (void *)j_File_isDirectory);
    bc_jni_bind("java/io/File", "mkdirs", "()Z", (void *)j_File_mkdirs);

    bc_jni_bind("android/content/Context", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    bc_jni_bind("android/content/Context", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    bc_jni_bind("android/content/Context", "getCacheDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    bc_jni_bind("android/content/Context", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    bc_jni_bind("android/content/Context", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    bc_jni_bind("android/content/Context", "getApplicationInfo",
                 "()Landroid/content/pm/ApplicationInfo;",
                 (void *)j_Context_getApplicationInfo);
    bc_jni_bind("android/app/Activity", "getApplicationInfo",
                 "()Landroid/content/pm/ApplicationInfo;",
                 (void *)j_Context_getApplicationInfo);
    bc_jni_bind("android/content/pm/ApplicationInfo", "dataDir",
                 "Ljava/lang/String;", (void *)j_AppInfo_dataDir);
    bc_jni_bind("android/content/pm/ApplicationInfo", "nativeLibraryDir",
                 "Ljava/lang/String;", (void *)j_AppInfo_nativeLibraryDir);
    bc_jni_bind("android/content/pm/ApplicationInfo", "sourceDir",
                 "Ljava/lang/String;", (void *)j_AppInfo_sourceDir);
    bc_jni_bind("android/content/pm/ApplicationInfo", "publicSourceDir",
                 "Ljava/lang/String;", (void *)j_AppInfo_sourceDir);
    bc_jni_bind("android/content/pm/ApplicationInfo", "packageName",
                 "Ljava/lang/String;", (void *)j_Context_getPackageName);
    bc_jni_bind("android/content/pm/PackageManager", "getPackageInfo",
                 "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
                 (void *)j_PackageManager_getPackageInfo);
    bc_jni_bind("android/content/pm/PackageManager", "getInstallerPackageName",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_PackageManager_getInstallerPackageName);
    bc_jni_bind("android/content/pm/PackageInfo", "versionName",
                 "Ljava/lang/String;", (void *)j_PackageInfo_versionName);
    bc_jni_bind("android/content/pm/PackageInfo", "packageName",
                 "Ljava/lang/String;", (void *)j_Context_getPackageName);
    bc_jni_bind("java/lang/String", "getBytes", "(Ljava/lang/String;)[B",
                 (void *)j_String_getBytes);
    bc_jni_bind("java/lang/String", "getBytes", "()[B",
                 (void *)j_String_getBytes);
    bc_jni_bind("android/media/AudioManager", "getDevices",
                 "(I)[Landroid/media/AudioDeviceInfo;",
                 (void *)j_AudioManager_getDevices);
    bc_jni_bind("android/content/Context", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    bc_jni_bind("android/content/Context", "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    bc_jni_bind("android/content/Context", "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    /* initJni receives an Activity; Android inheritance makes these Context
     * methods, but the flat registry needs the concrete class aliases too. */
    bc_jni_bind("android/app/Activity", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    bc_jni_bind("android/app/Activity", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    bc_jni_bind("android/app/Activity", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    bc_jni_bind("android/app/Activity", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    bc_jni_bind("android/app/Activity", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    bc_jni_bind("android/app/Activity", "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    bc_jni_bind("android/app/Activity", "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    /* ArmoryActivity extends UnityPlayerActivity/Activity.  The tiny object
     * model has no inheritance lookup, so publish the inherited Context calls
     * under the concrete launcher class as Android's GetMethodID would. */
    /* Bomb Chicken ships the stock com.unity3d.player.UnityPlayerActivity
     * (AndroidManifest), so the inherited Context calls are published under
     * that concrete class. */
    static const char armory_activity[] =
        "com/unity3d/player/UnityPlayerActivity";
    bc_jni_bind(armory_activity, "getFilesDir", "()Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    bc_jni_bind(armory_activity, "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    bc_jni_bind(armory_activity, "getCacheDir", "()Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    bc_jni_bind(armory_activity, "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    bc_jni_bind(armory_activity, "getPackageName", "()Ljava/lang/String;",
                 (void *)j_Context_getPackageName);
    bc_jni_bind(armory_activity, "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    bc_jni_bind(armory_activity, "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    bc_jni_bind(armory_activity, "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    bc_jni_bind(armory_activity, "getClass", "()Ljava/lang/Class;",
                 (void *)j_Object_getClass);
    bc_jni_bind(armory_activity, "getClass", "()Ljava/lang/Object;",
                 (void *)j_Object_getClass);
    bc_jni_bind(armory_activity, "runOnUiThread",
                 "(Ljava/lang/Runnable;)V", (void *)j_runOnUiThread);
    bc_jni_bind(armory_activity, "getActivity",
                 "()Lcom/unity3d/player/UnityPlayerActivity;",
                 (void *)j_Armory_getActivity);
    bc_jni_bind(armory_activity, "getActivity", "()Ljava/lang/Object;",
                 (void *)j_Armory_getActivity);
    bc_jni_bind(armory_activity, "getPermissionPlugin",
                 "()Lcom/squareenixmontreal/armory/PermissionPlugin;",
                 (void *)j_Armory_getPermissionPlugin);
    bc_jni_bind(armory_activity, "getPermissionPlugin",
                 "()Ljava/lang/Object;",
                 (void *)j_Armory_getPermissionPlugin);
    bc_jni_bind(armory_activity, "getOsVersion", "()I",
                 (void *)j_Armory_getOsVersion);
    bc_jni_bind(armory_activity, "getTargetSdkVersion", "()I",
                 (void *)j_Armory_getTargetSdkVersion);
    bc_jni_bind(armory_activity, "hasPermission",
                 "(Ljava/lang/String;)Z", (void *)j_Armory_hasPermission);

    static const char permission_plugin[] =
        "com/squareenixmontreal/armory/PermissionPlugin";
    bc_jni_bind(permission_plugin, "canRequestPermissions", "()Z",
                 (void *)j_Permission_canRequest);
    bc_jni_bind(permission_plugin, "hasPermission",
                 "(Ljava/lang/String;)Z", (void *)j_Armory_hasPermission);
    bc_jni_bind(permission_plugin, "shouldShowRequestPermissionRationale",
                 "(Ljava/lang/String;)Z", (void *)j_Permission_noRationale);
    bc_jni_bind(permission_plugin, "openApplicationSettings", "()V",
                 (void *)j_Permission_noop);
    bc_jni_bind(permission_plugin, "requestAccountPermission", "()V",
                 (void *)j_Permission_noop);
    bc_jni_bind(permission_plugin, "requestLocationPermission", "()V",
                 (void *)j_Permission_noop);
    bc_jni_bind(permission_plugin, "requestPermission",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    bc_jni_bind(permission_plugin, "requestPermissions",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    bc_jni_bind("android/provider/Settings$Secure", "getString",
                 "(Landroid/content/ContentResolver;Ljava/lang/String;)"
                 "Ljava/lang/String;", (void *)j_SettingsSecure_getString);
    bc_jni_bind("android/provider/Settings$Secure", "getString",
                 "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_SettingsSecure_getString);
    bc_jni_bind("java/util/Locale", "getDefault",
                 "()Ljava/util/Locale;", (void *)j_Locale_getDefault);
    bc_jni_bind("java/util/Locale", "getDefault", "()Ljava/lang/Object;",
                 (void *)j_Locale_getDefault);
    bc_jni_bind("java/util/Locale", "getLanguage",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    bc_jni_bind("java/util/Locale", "getCountry",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    bc_jni_bind("java/util/Locale", "toLanguageTag",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    bc_jni_bind("java/util/Locale", "toString", "()Ljava/lang/String;",
                 (void *)j_Locale_getString);
    bc_jni_bind("com/prime31/GoogleIABPlugin", "instance",
                 "()Ljava/lang/Object;", (void *)j_GoogleIAB_instance);
    bc_jni_bind("com/prime31/GoogleIABPlugin", "init",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    bc_jni_bind("com/google/android/gms/games/PlayGamesSdk", "initialize",
                 "(Lcom/unity3d/player/UnityPlayerActivity;)V",
                 (void *)j_PlayGames_initialize);
    bc_jni_bind("com/google/android/gms/games/PlayGames",
                 "getGamesSignInClient",
                 "(Lcom/unity3d/player/UnityPlayerActivity;)"
                 "Ljava/lang/Object;", (void *)j_PlayGames_getSignInClient);
    bc_jni_bind("com/squareenixmontreal/armory/SignatureReader",
                 "ShowSignature", "()V", (void *)j_Permission_noop);
    bc_jni_bind("android/content/res/AssetManager", "open",
                 "(Ljava/lang/String;)Ljava/io/InputStream;",
                 (void *)j_AssetManager_open);
    bc_jni_bind("java/util/Scanner", "next", "()Ljava/lang/String;",
                 (void *)j_Scanner_next);
    bc_jni_bind("android/os/Environment", "getExternalStorageState",
                 "()Ljava/lang/String;", (void *)j_Environment_mounted);
    bc_jni_bind("android/os/Environment", "MEDIA_MOUNTED",
                 "Ljava/lang/String;", (void *)j_Environment_mounted);
    bc_jni_bind("java/lang/String", "equals", "(Ljava/lang/Object;)Z",
                 (void *)j_String_equals);
    bc_jni_bind("java/lang/StringBuilder", "toString",
                 "()Ljava/lang/String;", (void *)j_StringBuilder_toString);
    bc_jni_bind("android/net/Uri", "encode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_encode);
    bc_jni_bind("android/net/Uri", "decode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_decode);

    for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++) {
        bc_jni_bind("android/os/Build", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
        bc_jni_bind("android/os/Build$VERSION", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
    }
    bc_jni_bind("android/os/Build$VERSION", "SDK_INT", "I",
                 (void *)j_Build_SDK_INT);

    bc_jni_bind("android/app/Activity", "getPreferences",
                 "(I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    bc_jni_bind("android/content/Context", "getSharedPreferences",
                 "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    bc_jni_bind("android/content/SharedPreferences", "getBoolean",
                 "(Ljava/lang/String;Z)Z", (void *)j_Prefs_getBoolean);
    bc_jni_bind("android/content/SharedPreferences", "getInt",
                 "(Ljava/lang/String;I)I", (void *)j_Prefs_getInt);
    bc_jni_bind("android/content/SharedPreferences", "getFloat",
                 "(Ljava/lang/String;F)F", (void *)j_Prefs_getFloat);
    bc_jni_bind("android/content/SharedPreferences", "getString",
                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Prefs_getString);
    bc_jni_bind("android/content/SharedPreferences", "contains",
                 "(Ljava/lang/String;)Z", (void *)j_Prefs_contains);
    bc_jni_bind("android/content/SharedPreferences", "edit",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_edit);
    bc_jni_bind("android/content/SharedPreferences$Editor", "putInt",
                 "(Ljava/lang/String;I)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putInt);
    bc_jni_bind("android/content/SharedPreferences$Editor", "putFloat",
                 "(Ljava/lang/String;F)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putFloat);
    bc_jni_bind("android/content/SharedPreferences$Editor", "putString",
                 "(Ljava/lang/String;Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putString);
    bc_jni_bind("android/content/SharedPreferences$Editor", "putBoolean",
                 "(Ljava/lang/String;Z)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putBoolean);
    bc_jni_bind("android/content/SharedPreferences$Editor", "remove",
                 "(Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_remove);
    bc_jni_bind("android/content/SharedPreferences$Editor", "clear",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_clear);
    bc_jni_bind("android/content/SharedPreferences$Editor", "apply",
                 "()V", (void *)j_Prefs_apply);
    bc_jni_bind("android/content/SharedPreferences$Editor", "commit",
                 "()Z", (void *)j_Prefs_commit);
    bc_jni_bind("java/lang/Object", "getClass", "()Ljava/lang/Class;",
                 (void *)j_Object_getClass);
    bc_jni_bind("java/lang/Class", "getClassLoader",
                 "()Ljava/lang/ClassLoader;", (void *)j_getClassLoader);
    bc_jni_bind("java/lang/ClassLoader", "findLibrary",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_ClassLoader_findLibrary);
    bc_jni_bind("java/lang/System", "loadLibrary",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);
    bc_jni_bind("java/lang/System", "load",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "getClassLoader",
                 "()Ljava/lang/ClassLoader;", (void *)j_getClassLoader);
    bc_jni_bind("com/unity3d/player/UnityPlayer", "forName",
                 "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
                 (void *)j_forName);

    /* Sanity: native Android objects call JNIEnv slots by their ABI offset. */
    if (JNI_NewStringUTF * 8 != 1336)
        nx_die("JNIEnv layout drift: NewStringUTF is at %d, not 1336",
               JNI_NewStringUTF * 8);
    nx_log("jni: env=%p vtable slots=%d", bc_jni_env(), JNI_SLOT_COUNT);
}

void *bc_jni_sym(const char *name)
{
    (void)name;
    return NULL;
}
