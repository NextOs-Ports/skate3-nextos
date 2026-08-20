/*
 * main.c -- native Skateboard Party 3 bootstrap for NextOS.
 *
 * There is no Android application or emulator in this path.  We load the
 * original arm64 Unity objects, run their real init arrays/JNI_OnLoad, then
 * drive Unity's native surface and render lifecycle directly.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "nx_elf.h"
#include "bc.h"

char bc_gamedir[1024];
char bc_datadir[1024];
char bc_apk[1024];
char bc_home[1024];
long bc_max_frames = 0;
int bc_trace_gl = 0;
int bc_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Bomb Chicken v44 is a normal, unprotected Unity 2022.3.39f1 IL2CPP build
 * (arm64 only, no PairIP/packer).  Keep the exact NativeLoader order and do
 * not introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *bc_pthread_table(size_t *n);
extern const nx_import *bc_android_table(size_t *n);
extern const nx_import *bc_egl_table(size_t *n);

/* One combined, sorted import table: bionic + pthread bridge + libandroid +
 * EGL.  nx_resolve_import binary-searches it. */
static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne;
    const nx_import *p = bc_pthread_table(&np);
    const nx_import *an = bc_android_table(&na);
    const nx_import *eg = bc_egl_table(&ne);

    size_t bn;
    extern nx_import *bc_bionic_entries(size_t *n);
    nx_import *be = bc_bionic_entries(&bn);
    all = calloc(bn + np + na + ne + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)
        all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)
        all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)
        all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)
        all[all_n++] = eg[i];
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, egl %zu)",
           all_n, bn, np, na, ne);
}

/* Report the modules mapped by the native loader. */
/* ------------------------------------------------------------- frame watchdog */

static volatile unsigned long watchdog_frame;
/* Remendos de diagnostico da sessao 09/08 (alloc-recovery + skip de null-deref).
 * Ficam DESLIGADOS por padrao: pulavam falha cegamente dentro da libil2cpp e da
 * libunity, fabricando ponteiros lixo e mascarando a falha real. SK3_RECOVERY=1
 * religa quando for para investigar. */
static int sk3_recovery;
static void *sk3_dummy_class;  /* System.Object class for alloc-retry recovery */

/* Hook de il2cpp_class_from_name pra logar tipos null */
void *(*real_class_from_name)(void *, const char *, const char *);
void *my_class_from_name(void *image, const char *ns, const char *name)
{
    void *r = real_class_from_name ? real_class_from_name(image, ns, name) : NULL;
    if (!r && sk3_recovery) {
        if (!sk3_dummy_class) {
            void *(*dg)(void) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_domain_get");
            void *(*dga)(void*,size_t*) = (void*(*)(void*,size_t*))nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_domain_get_assemblies");
            void *(*agm)(void*) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_assembly_get_image");
            void *(*cfn2)(void*,const char*,const char*) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_class_from_name");
            if (dg&&dga&&agm&&cfn2) {
                void *dom=dg(); size_t nn=0; void **as=dga(dom,&nn);
                if (as&&nn>0){void *img2=agm(as[0]); if(img2) sk3_dummy_class=cfn2(img2,"System","Object");}
            }
        }
        fprintf(stderr, "[bc/class-null] %s.%s -> fallback %p\n", ns?ns:"?", name?name:"?", sk3_dummy_class);
        return sk3_dummy_class;
    }
    return r;
}
void sk3_set_class_from_name(void *real) { real_class_from_name = real; }

/* Hook de il2cpp_class_from_il2cpp_type (path do deserializador) */
void *(*real_cfit)(void *);
void *my_cfit(void *type)
{
    void *r = real_cfit ? real_cfit(type) : NULL;
    if (!r && type && sk3_recovery) {
        if (!sk3_dummy_class) {
            void *(*dg)(void) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_domain_get");
            void *(*dga)(void*,size_t*) = (void*(*)(void*,size_t*))nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_domain_get_assemblies");
            void *(*agm)(void*) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_assembly_get_image");
            void *(*cfn2)(void*,const char*,const char*) = nx_lookup_in(nx_find_mod("libil2cpp.so"), "il2cpp_class_from_name");
            if (dg&&dga&&agm&&cfn2) {
                void *dom=dg(); size_t nn=0; void **as=dga(dom,&nn);
                if (as&&nn>0){void *img2=agm(as[0]); if(img2) sk3_dummy_class=cfn2(img2,"System","Object");}
            }
        }
        fprintf(stderr, "[bc/cfit-null] type=%p -> fallback %p\n", type, sk3_dummy_class);
        return sk3_dummy_class;
    }
    return r;
}
void sk3_set_cfit(void *real) { real_cfit = real; }
static pid_t watchdog_tid;
static int watchdog_seconds;

void bc_watchdog_frame(void) { watchdog_frame++; }

static void *watchdog_thread(void *arg)
{
    (void)arg;
    unsigned long last = watchdog_frame;
    for (;;) {
        struct timespec t = { watchdog_seconds, 0 };
        nanosleep(&t, NULL);
        if (watchdog_frame != last) {
            last = watchdog_frame;
            continue;
        }
        fprintf(stderr,
                "[bc] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void bc_arm_frame_watchdog(void)
{
    const char *v = getenv("BC_WATCHDOG");
    if (!v || !*v)
        return;
    watchdog_seconds = atoi(v);
    if (watchdog_seconds <= 0)
        return;
    watchdog_tid = (pid_t)syscall(SYS_gettid);
    pthread_t th;
    if (pthread_create(&th, NULL, watchdog_thread, NULL) != 0) {
        nx_log("watchdog: cannot start thread");
        return;
    }
    pthread_detach(th);
    nx_log("watchdog armed: %ds without a frame faults tid %d",
           watchdog_seconds, (int)watchdog_tid);
}

int bc_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
{
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))m->base;
        info.dlpi_name = m->name;
        info.dlpi_phdr = (const ElfW(Phdr) *)m->phdr;
        info.dlpi_phnum = (ElfW(Half))m->phnum;
        int r = cb(&info, sizeof info, data);
        if (r)
            return r;
    }
    return 0;
}

/* Which mapped module contains an address, for dladdr. */
const char *bc_mod_at(const void *addr, void **base_out)
{
    const uint8_t *p = addr;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        if (p >= m->base && p < m->base + m->span) {
            if (base_out)
                *base_out = m->base;
            return m->name;
        }
    }
    return NULL;
}

static void read_env(void)
{
    const char *v;
    sk3_recovery = (v = getenv("SK3_RECOVERY")) && *v != '0';
    nx_verbose   = (v = getenv("BC_VERBOSE")) && *v != '0';
    bc_log_level = (v = getenv("BC_LOGCAT")) && *v != '0';
    bc_trace_jni = (v = getenv("BC_JNILOG")) && *v != '0';
    bc_trace_gl  = (v = getenv("BC_GLLOG")) && *v != '0';
    if ((v = getenv("BC_FRAMES")))
        bc_max_frames = strtol(v, NULL, 10);
}

static void copy_path(char *out, size_t capacity, const char *value,
                      const char *description)
{
    size_t length = strlen(value);
    if (length >= capacity)
        nx_die("%s path is too long", description);
    memcpy(out, value, length + 1);
}

static void join_path(char *out, size_t capacity, const char *base,
                      const char *first, const char *second)
{
    int written;
    if (second)
        written = snprintf(out, capacity, "%s/%s/%s", base, first, second);
    else
        written = snprintf(out, capacity, "%s/%s", base, first);
    if (written < 0 || (size_t)written >= capacity)
        nx_die("game path is too long");
}

static void setup_paths(const char *arg)
{
    if (arg && *arg)
        copy_path(bc_gamedir, sizeof bc_gamedir, arg, "game directory");
    else if (!getcwd(bc_gamedir, sizeof bc_gamedir))
        copy_path(bc_gamedir, sizeof bc_gamedir, ".", "game directory");
    join_path(bc_datadir, sizeof bc_datadir, bc_gamedir, "assets", NULL);
    join_path(bc_apk, sizeof bc_apk, bc_gamedir, "assets", NULL);
    join_path(bc_home, sizeof bc_home, bc_gamedir, "home", NULL);
    mkdir(bc_home, 0755);
}

int bc_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !bc_capture_mode)
            continue;
        join_path(path, sizeof path, bc_gamedir, "lib", LIBS[i].file);
        nx_mod *m = nx_load(path, LIBS[i].soname);
        if (!m) {
            if (LIBS[i].required)
                nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
            nx_log("optional %s missing", LIBS[i].file);
        }
    }
    /* Relocate in the same order; by the time libunity is relocated the other
     * modules can satisfy its cross-module imports. */
    int missing = 0;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !bc_capture_mode)
            continue;
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m)
            missing += nx_relocate(m);
    }
    return missing;
}

/* A fault inside a module we mapped ourselves has no symbols and no link map,
 * so the only way to place it is to print the PC against the module bases.
 * Always on: it costs nothing until something goes wrong. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    /* SIGSEGV no alocador de arrays do IL2CPP (null class -> array alloc crash):
     * Retornar null causa SEGV no caller (libunity usa o null). Em vez disso,
     * fazer RETRY com uma classe dummy (System.Object) → array valido (tipo
     * errado mas não-null) → deserializer continua. */
    if (sig == SIGSEGV) {
        nx_mod *il2 = nx_find_mod("libil2cpp.so");
        nx_mod *unity = nx_find_mod("libunity.so");
        int in_il2 = il2 && pc >= (unsigned long)il2->base && pc < (unsigned long)il2->base + il2->span;
        int in_unity = unity && pc >= (unsigned long)unity->base && pc < (unsigned long)unity->base + unity->span;
        unsigned long off = 0;
        if (in_il2) off = pc - (unsigned long)il2->base;
        else if (in_unity) off = pc - (unsigned long)unity->base;

        /* Allocator: null-class -> return null (sp+48, return to caller) */
        if (sk3_recovery && in_il2 && off >= 0xd0d150 && off <= 0xd0d400) {
            static int alloc_recoveries;
            if (alloc_recoveries < 50) {
                alloc_recoveries++;
                unsigned long sp = u->uc_mcontext.sp;
                unsigned long saved_lr = *(volatile unsigned long *)sp;
                u->uc_mcontext.sp = sp + 48;
                u->uc_mcontext.regs[0] = 0;
                u->uc_mcontext.pc = saved_lr;
                if (alloc_recoveries <= 5)
                    fprintf(stderr, "[bc] alloc-recovery #%d (off=0x%lx) -> null\n",
                            alloc_recoveries, off);
                return;
            }
        }
        /* Null-deref em libil2cpp OU libunity: skip pc+=4 com loop-break */
        if (sk3_recovery && (in_il2 || in_unity)) {
            static unsigned long last_pc;
            static int same_count;
            static int total_skips;
            if (total_skips < 10000) {
                total_skips++;
                if (pc == last_pc) {
                    same_count++;
                    if (same_count >= 3) {
                        unsigned long lr = u->uc_mcontext.regs[30];
                        if (lr > 0x10000) {
                            u->uc_mcontext.pc = lr;
                            u->uc_mcontext.regs[0] = 0;
                            same_count = 0;
                            if (total_skips <= 20)
                                fprintf(stderr, "[bc] loop-break @0x%lx -> lr=0x%lx\n", off, lr);
                            return;
                        }
                    }
                } else {
                    last_pc = pc;
                    same_count = 1;
                }
                u->uc_mcontext.pc += 4;
                if (total_skips <= 20 || total_skips % 200 == 0)
                    fprintf(stderr, "[bc] skip #%d (off=0x%lx %s)\n", total_skips, off,
                            in_unity ? "unity" : "il2cpp");
                return;
            }
        }
    }
    fprintf(stderr, "\n[bc] signal %d at pc=%#lx addr=%p\n", sig, pc,
            si ? si->si_addr : NULL);
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[bc]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[bc]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[bc]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[bc]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[bc]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    bc_input_request_exit();
}

static void install_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    /* SIGTERM/SIGINT seguem o caminho do SELECT+START (pause/save/saída),
     * nunca morte seca: frontends e supervisores mandam TERM primeiro. */
    struct sigaction quit;
    memset(&quit, 0, sizeof quit);
    quit.sa_handler = on_exit_signal;
    sigemptyset(&quit.sa_mask);
    sigaction(SIGTERM, &quit, NULL);
    sigaction(SIGINT, &quit, NULL);
}

/* ===== sk3 vtrap: capturar o metodo virtual (br x4) que aloca os 2 GB =====
 * O wrapper de dispatch virtual em libil2cpp VA 0x1c24e10 faz `br x4` em
 * 0x1c24e80 com x4 = endereco do metodo (so' resolvido em runtime).  Trocamos
 * o `br x4` por `brk #1`; o handler SIGTRAP loga x4 (offset do libil2cpp) e
 * redireciona o PC pra x4, deixando o jogo rodar.  Cap de SK3_VTRAP_MAX
 * capturas pra nao estourar se o dispatcher for quente. */
#define SK3_VTRAP_SITE   0x1c24e80u
#define SK3_VTRAP_MAX    600
static volatile unsigned long sk3_vtrap_addr;   /* runtime addr do br x4 */
static volatile uint32_t sk3_vtrap_orig;        /* instr original (br x4) */
static volatile int sk3_vtrap_count;
static volatile unsigned long sk3_il2_base;
static volatile unsigned long sk3_il2_span;
static void on_vtrap(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    if (pc == sk3_vtrap_addr) {
        unsigned long x4 = (unsigned long)u->uc_mcontext.regs[4];
        if (sk3_vtrap_count < SK3_VTRAP_MAX) {
            unsigned long off = (sk3_il2_base && x4 >= sk3_il2_base &&
                                 x4 < sk3_il2_base + sk3_il2_span)
                              ? x4 - sk3_il2_base : 0;
            fprintf(stderr, "[sk3/vtrap#%d] x4=0x%lx libil2cpp+0x%lx\n",
                    sk3_vtrap_count, x4, off);
        }
        sk3_vtrap_count++;
        u->uc_mcontext.pc = x4;   /* desvia pro metodo virtual (br x4) */
        return;
    }
    /* SIGTRAP alheio: deixa o default (nao deveria acontecer). */
    _exit(5);
}
static void sk3_install_vtrap(void)
{
    if (!getenv("SK3_VTRAP"))
        return;
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!il2) return;
    sk3_il2_base = (unsigned long)il2->base;
    sk3_il2_span = il2->span;
    sk3_vtrap_addr = sk3_il2_base + SK3_VTRAP_SITE;
    uint32_t *site = (uint32_t *)sk3_vtrap_addr;
    sk3_vtrap_orig = *site;
    if (sk3_vtrap_orig != 0xd61f0080u) {  /* br x4 */
        fprintf(stderr, "[sk3/vtrap] site nao eh br x4 (0x%x), abortando vtrap\n",
                sk3_vtrap_orig);
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_vtrap;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL);
    /* torna a pagina do site gravavel */
    uintptr_t page = sk3_vtrap_addr & ~0xfffull;
    mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    *site = 0xd4200020u;   /* brk #1 */
    __builtin___clear_cache((char *)site, (char *)site + 4);
    fprintf(stderr, "[sk3/vtrap] armado em %p (br x4 -> brk #1)\n", (void *)sk3_vtrap_addr);
}

static void run_unity(void)
{
    void *env = bc_jni_env();
    void *player = bc_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = bc_jni_activity();
    void *surface = bc_jret_obj("android/view/Surface");
    void *fn;

    bc_jni_set_unity_player(player);

    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[bc] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[bc] initJni OK\n");

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[bc] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[bc] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = bc_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        const char *focus = getenv("SK3_FOCUS");
        if (focus && focus[0] == '1') {
            ((void (*)(void *, void *, int))fn)(env, player, 1);
            fprintf(stderr, "[bc] nativeFocusChanged(true) OK\n");
        } else {
            /* Num launch fresco o app ja nasce "running"; forcar focus/resume
             * dispara OnApplicationPause prematuro que acorda o DB do skate3
             * antes do bootstrap (OOM em get_Instance). Omitimos por padrao. */
            fprintf(stderr, "[bc] nativeFocusChanged omitido (SK3_FOCUS=1 p/ forcar)\n");
        }
    }
    /* DELAY nativeResume: OnAndroid, RRPlugins.Awake() (IvorySDK init) runs
     * BEFORE OnApplicationPause (triggered by nativeResume). In our so-loader,
     * nativeResume fires immediately, before scene Awake. Delay it to frame 30
     * so the scene initializes (IvorySDK sets PluginDatabase fields) first. */
    void *native_resume_fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    int resume_delay = 30;
    const char *rd = getenv("SK3_RESUME_DELAY");
    if (rd && *rd) resume_delay = atoi(rd);
    fprintf(stderr, "[bc] nativeResume delayed to frame %d\n", resume_delay);

    bc_audio_start(env);

    void *render = bc_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[bc] nativeRender loop%s\n",
            bc_max_frames > 0 ? " (test frame limit active)" : "");

    bc_input_init();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless BC_WATCHDOG names a timeout in seconds. */
    bc_arm_frame_watchdog();

    unsigned long frame = 0;
    const char *frame_us_env = getenv("BC_FRAME_US");
    long frame_budget_us = frame_us_env && *frame_us_env
                         ? strtol(frame_us_env, NULL, 10) : 16667;
    struct timespec frame_start;
    int report_fps = getenv("BC_FPS") != NULL;
    struct timespec fps_mark;
    clock_gettime(CLOCK_MONOTONIC, &fps_mark);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        bc_watchdog_frame();
        bc_input_poll(env, player, frame);
        if (bc_input_exit_requested()) {
            fprintf(stderr, "[bc] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        /* Delayed nativeResume: chamar depois de N frames */
        if (native_resume_fn && frame == (unsigned long)resume_delay) {
            fprintf(stderr, "[bc] nativeResume at frame %lu\n", frame);
            ((void (*)(void *, void *))native_resume_fn)(env, player);
        }
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[bc] frame %lu keep=%u\n", frame, keep);
        if (report_fps && frame % 300 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - fps_mark.tv_sec) +
                        (now.tv_nsec - fps_mark.tv_nsec) / 1e9;
            if (dt > 0)
                fprintf(stderr, "[bc/fps] %.1f fps (300 frames in %.2fs)\n",
                        300.0 / dt, dt);
            fps_mark = now;
        }
        if (!keep) {
            fprintf(stderr, "[bc] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (bc_max_frames > 0 && frame >= (unsigned long)bc_max_frames) {
            fprintf(stderr, "[bc] test frame limit reached (%lu)\n", frame);
            break;
        }
        /* Pacing pelo TEMPO QUE SOBRA do orcamento do quadro, nunca um sleep
         * fixo somado ao trabalho: com swap bloqueando no vsync um sleep
         * cru de 16,67 ms derruba um jogo de acao para metade da taxa. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long spent_us = (now.tv_sec - frame_start.tv_sec) * 1000000L +
                            (now.tv_nsec - frame_start.tv_nsec) / 1000L;
            long budget_us = frame_budget_us;
            if (budget_us > 0 && spent_us < budget_us)
                usleep((useconds_t)(budget_us - spent_us));
        }
    }

    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[bc] nativeFocusChanged(false) OK\n");
    }
    fn = bc_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[bc] nativePause OK\n");
    }
    bc_input_close();
    bc_audio_stop();
}

/* UM JOGO SO: a trava vai no BINARIO, nunca so no script do launcher.  Um
 * script pode ser copiado, renomeado ou lancado por outro caminho; o executavel
 * e' o unico recurso que toda instancia tem em comum. */
static void claim_single_instance(void)
{
    static int lock_fd = -1;
    lock_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (lock_fd < 0)
        return;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr,
                "[bc] outra instancia do Skateboard Party 3 ja esta rodando; saindo\n");
        _exit(1);
    }
    /* Intencionalmente sem close(): a trava vale enquanto o processo viver. */
}

static void sk3_patch_class_from_name(void);
int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    claim_single_instance();

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("GC_DISABLE_INCREMENTAL", "1", 0);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);

    sk3_patch_class_from_name();
    fprintf(stderr, "[sk3] Skateboard Party 3 for NextOS -- gamedir %s\n", bc_gamedir);

    bc_jni_init();
    bc_egl_init();
    build_imports();

    int missing = bc_load_modules();
    sk3_install_vtrap();
    /* Pre-carregar SDK libs nativas (com deps resuvidas no gamedir/lib) */
    {
        const char *sdks[] = {
            "libivorysdk_core.so", NULL,
        };
        extern void sk3_preload_sdks(const char *const *);
        sk3_preload_sdks(sdks);
    }
    fprintf(stderr, "[bc] modules loaded, %d relocations unresolved\n", missing);

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so has no JNI_OnLoad");
    int main_version = main_onload(bc_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[bc] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        bc_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    join_path(libdir, sizeof libdir, bc_gamedir, "lib", NULL);
    void *loader_class =
        bc_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = bc_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        bc_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[bc] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();
    return 0;
}

/* Patch il2cpp_class_from_name para logar tipos null */
static void sk3_patch_class_from_name(void)
{
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!il2 || !real_class_from_name) return;
    uint32_t *site = (uint32_t *)((unsigned long)il2->base + 0xd56160);
    /* ldr x16, [pc, #8] = 0x58000050 */
    /* br x16 = 0xd61f0200 */
    /* .quad my_class_from_name */
    uintptr_t fn = (uintptr_t)my_class_from_name;
    uint32_t lo = (uint32_t)fn;
    uint32_t hi = (uint32_t)(fn >> 32);
    extern long mprotect_writable(uintptr_t, size_t);
    /* Already have mprotect available */
    uintptr_t page = ((uintptr_t)site) & ~0xfffull;
    mprotect((void*)page, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC);
    site[0] = 0x58000050u;  /* ldr x16, [pc, #8] */
    site[1] = 0xd61f0200u;  /* br x16 */
    site[2] = lo;            /* fn low 32 */
    site[3] = hi;            /* fn high 32 */
    __builtin___clear_cache((char*)site, (char*)(site+4));
    fprintf(stderr, "[bc] patched il2cpp_class_from_name -> %p\n", my_class_from_name);
}
