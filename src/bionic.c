/*
 * bionic.c -- the libc/liblog/libdl surface that the Android objects import.
 *
 * Only the differences between bionic and glibc need real work; everything
 * whose ABI matches on arm64 (stat, dirent, most of stdio) is forwarded.
 * The differences that bite here:
 *   - FORTIFY (__*_chk) is bionic-only,
 *   - struct sigaction has a different field order,
 *   - __sF is an array of FILE, not three pointers,
 *   - the pthread objects are smaller than glibc's (see pthread_bridge.c),
 *   - __system_property_get / __android_log_* do not exist at all.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <time.h>
#include <math.h>
#include <locale.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <fnmatch.h>
#include <libgen.h>
#include <syslog.h>
#include <utime.h>
#include <zlib.h>
#include <malloc.h>
#include <pthread.h>

#include "nx_elf.h"
#include "bc.h"

/* ------------------------------------------------------------------ logging */

int bc_log_level = 0;   /* BC_LOGCAT=1 mirrors the game's own log */

static const char lvl[] = "?????VDIWEFS";

int my___android_log_write(int prio, const char *tag, const char *msg)
{
    if (bc_log_level)
        fprintf(stderr, "[%c/%s] %s\n", lvl[prio & 15], tag ? tag : "?",
                msg ? msg : "");
    return 0;
}

int my___android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    if (!bc_log_level)
        return 0;
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return my___android_log_write(prio, tag, buf);
}

int my___android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = my___android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void my___android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char buf[1024] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    }
    nx_die("assert from %s: %s %s", tag ? tag : "?", cond ? cond : "", buf);
}

void my_android_set_abort_message(const char *m)
{
    fprintf(stderr, "[bc] abort message: %s\n", m ? m : "(null)");
}

/* -------------------------------------------------------- system properties */

/* Unity reads these to pick quality tiers and to decide whether it is running
 * on a known device; answering with a plausible modern phone keeps it out of
 * its own low-end fallbacks.  Nothing here is device specific. */
static const struct { const char *k, *v; } props[] = {
    { "ro.product.model",            "Pixel 6" },
    { "ro.product.manufacturer",     "Google" },
    { "ro.product.brand",            "google" },
    { "ro.product.device",           "oriole" },
    { "ro.product.name",             "oriole" },
    { "ro.product.board",            "oriole" },
    { "ro.product.cpu.abi",          "arm64-v8a" },
    { "ro.product.cpu.abilist",      "arm64-v8a" },
    { "ro.build.version.sdk",        "31" },
    { "ro.build.version.release",    "12" },
    { "ro.build.id",                 "SQ1D.220205.004" },
    { "ro.build.fingerprint",        "google/oriole/oriole:12/SQ1D.220205.004/1/user/release-keys" },
    { "ro.build.type",               "user" },
    { "ro.build.tags",               "release-keys" },
    { "ro.debuggable",               "0" },
    { "ro.secure",                   "1" },
    { "ro.hardware",                 "oriole" },
    { "ro.arch",                     "arm64" },
    { "debug.egl.hw",                "1" },
    { "ro.opengles.version",         "196608" },   /* 3.0 -- matches the APK */
    { "persist.sys.locale",          "en-US" },
    { "ro.product.locale",           "en-US" },
};

/* Verbose-only diagnostics for Android/libc compatibility calls. */
#define BIONIC_TRACE(...) nx_log(__VA_ARGS__)

int my___system_property_get(const char *key, char *value)
{
    BIONIC_TRACE("__system_property_get(\"%s\")", key ? key : "(null)");
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        if (strcmp(props[i].k, key) == 0)
            return (int)strlen(strcpy(value, props[i].v));
    value[0] = 0;
    return 0;
}

const void *my___system_property_find(const char *key)
{
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        if (strcmp(props[i].k, key) == 0)
            return &props[i];
    return NULL;
}

int my___system_property_read(const void *pi, char *name, char *value)
{
    const struct { const char *k, *v; } *p = pi;
    if (!p)
        return 0;
    if (name)
        strcpy(name, p->k);
    if (value)
        strcpy(value, p->v);
    return (int)strlen(p->v);
}

void my___system_property_read_callback(const void *pi,
                                        void (*cb)(void *, const char *,
                                                   const char *, uint32_t),
                                        void *ck)
{
    const struct { const char *k, *v; } *p = pi;
    if (p && cb)
        cb(ck, p->k, p->v, 1);
}

int my___system_property_foreach(void (*cb)(const void *, void *), void *ck)
{
    for (size_t i = 0; i < sizeof props / sizeof *props; i++)
        cb(&props[i], ck);
    return 0;
}

/* ----------------------------------------------------------------- __errno */

int *my___errno(void) { return &errno; }

/* Os wrappers de alocador abaixo existem para que TODA alocacao das libs
 * Android caia no alocador do host (resolvido uma vez por dlsym), em vez de
 * cada .so tentar o seu.  Sao repasses puros. */
/* O sysroot atual do NextOS ja e glibc 2.44 e versiona cosh/sinh nela, mas o
 * firmware do device ainda esta em 2.43 e nao tem essas versoes. Pinar a versao
 * 2.17 (mesma funcao, presente em toda glibc suportada) mantem o binario
 * carregavel no device sem baixar o toolchain. */
__asm__(".symver cosh,cosh@GLIBC_2.17");
__asm__(".symver sinh,sinh@GLIBC_2.17");
static void *(*sk3_real_malloc)(size_t);
static void *(*sk3_real_calloc)(size_t,size_t);
static void *(*sk3_real_realloc)(void*,size_t);
static int  (*sk3_real_posix_memalign)(void**,size_t,size_t);
static void *(*sk3_real_aligned_alloc)(size_t,size_t);
static void sk3_init_alloc(void)
{
    sk3_real_malloc = dlsym(RTLD_NEXT,"malloc");
    sk3_real_calloc = dlsym(RTLD_NEXT,"calloc");
    sk3_real_realloc = dlsym(RTLD_NEXT,"realloc");
    sk3_real_posix_memalign = dlsym(RTLD_NEXT,"posix_memalign");
    sk3_real_aligned_alloc = dlsym(RTLD_NEXT,"aligned_alloc");
}
/* Resolve antes de qualquer alocacao do jogo: dlsym pode chamar calloc, o que
 * reentraria em my_calloc antes de sk3_real_calloc estar pronto. */
__attribute__((constructor))
static void sk3_alloc_ctor(void) { sk3_init_alloc(); }
void *my_malloc(size_t sz)
{
    return sk3_real_malloc ? sk3_real_malloc(sz) : malloc(sz);
}
void *my_calloc(size_t n, size_t sz)
{
    return sk3_real_calloc ? sk3_real_calloc(n,sz) : calloc(n,sz);
}
void *my_realloc(void *p, size_t sz)
{
    if (!sk3_real_realloc) sk3_init_alloc();
    return sk3_real_realloc ? sk3_real_realloc(p,sz) : realloc(p,sz);
}
int my_posix_memalign(void **m, size_t align, size_t sz)
{
    if (!sk3_real_posix_memalign) sk3_init_alloc();
    return sk3_real_posix_memalign ? sk3_real_posix_memalign(m,align,sz)
                                  : posix_memalign(m,align,sz);
}
void *my_aligned_alloc(size_t align, size_t sz)
{
    if (!sk3_real_aligned_alloc) sk3_init_alloc();
    return sk3_real_aligned_alloc ? sk3_real_aligned_alloc(align,sz)
                                  : aligned_alloc(align,sz);
}
/* Unity/il2cpp alocam blocos grandes direto via mmap, sem passar pelo malloc:
 * o wrapper existe para manter esse caminho sob o alocador do host. */
void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return mmap(addr, len, prot, flags, fd, off);
}

/* Skateboard Party 3 runs Mirror/SmartFox networking that, early in boot,
 * formats the device's local address with inet_ntop.  The address pointer it
 * hands in comes from a stubbed/peripheral path and can be a tiny invalid
 * value (observed 0x4); libc's inet_ntop then dereferences it and faults.
 * The game is offline here, so a defensive inet_ntop that returns a harmless
 * address for any non-mapped src lets the network code continue (and simply
 * never connect) instead of aborting the whole Awake. */
const char *my_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (dst && size > 0) {
        if (src && (uintptr_t)src > 0x10000)
            return inet_ntop(af, src, dst, size);
        if (af == AF_INET6) {
            if (size < 2) return NULL;
            strcpy(dst, "::");
        } else {
            if (size < 8) return NULL;
            strcpy(dst, "0.0.0.0");
        }
        return dst;
    }
    return NULL;
}

/* ------------------------------------------------------------------- stdio */

/* bionic exports __sF as `FILE __sF[3]`, so the game computes &__sF[1] for
 * stdout.  We publish a same-shaped array of placeholders and translate them
 * back on every call that takes a FILE*. */
typedef struct { char pad[152]; } bionic_FILE;
bionic_FILE bc_sF[3];

static FILE *xf(void *f)
{
    if (f == (void *)&bc_sF[0])
        return stdin;
    if (f == (void *)&bc_sF[1])
        return stdout;
    if (f == (void *)&bc_sF[2])
        return stderr;
    return (FILE *)f;
}

static int my_fprintf(void *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(xf(f), fmt, ap);
    va_end(ap);
    return r;
}
static int my_vfprintf(void *f, const char *fmt, va_list ap) { return vfprintf(xf(f), fmt, ap); }
static int my_fputc(int c, void *f)         { return fputc(c, xf(f)); }
static int my_fputs(const char *s, void *f) { return fputs(s, xf(f)); }
static int my_fgetc(void *f)                { return fgetc(xf(f)); }
static char *my_fgets(char *s, int n, void *f) { return fgets(s, n, xf(f)); }
static size_t my_fwrite(const void *p, size_t a, size_t b, void *f) { return fwrite(p, a, b, xf(f)); }
static size_t my_fread(void *p, size_t a, size_t b, void *f) { return fread(p, a, b, xf(f)); }
static int my_fflush(void *f)               { return fflush(f ? xf(f) : NULL); }
static int my_fclose(void *f)               { return f == (void *)bc_sF ? 0 : fclose(xf(f)); }
static int my_feof(void *f)                 { return feof(xf(f)); }
static int my_ferror(void *f)               { return ferror(xf(f)); }
static void my_clearerr(void *f)            { clearerr(xf(f)); }
static int my_fileno(void *f)               { return fileno(xf(f)); }
static void my_setbuf(void *f, char *b)     { setbuf(xf(f), b); }
static int my_setvbuf(void *f, char *b, int m, size_t s) { return setvbuf(xf(f), b, m, s); }
static int my_fseek(void *f, long o, int w)  { return fseek(xf(f), o, w); }
static long my_ftell(void *f)                { return ftell(xf(f)); }
static void my_rewind(void *f)               { rewind(xf(f)); }
static int my_ungetc(int c, void *f)         { return ungetc(c, xf(f)); }
static int my_vprintf(const char *f, va_list ap) { return vfprintf(stdout, f, ap); }

/* --------------------------------------------------------------- FORTIFY */

static void *my___memcpy_chk(void *d, const void *s, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memcpy_chk overflow (%zu > %zu)", n, dl);
    return memcpy(d, s, n);
}
static void *my___memmove_chk(void *d, const void *s, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memmove_chk overflow");
    return memmove(d, s, n);
}
static void *my___memset_chk(void *d, int c, size_t n, size_t dl)
{
    if (n > dl)
        nx_die("__memset_chk overflow");
    return memset(d, c, n);
}
static size_t my___strlen_chk(const char *s, size_t dl) { (void)dl; return strlen(s); }
static char *my___strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
static char *my___strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
static char *my___strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
static char *my___strncat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }
static char *my___strchr_chk(const char *s, int c, size_t sl)
{
    (void)sl;
    return strchr(s, c);
}
static mode_t my___umask_chk(mode_t mask) { return umask(mask); }

/* bionic's __vsnprintf_chk truncates like vsnprintf; the __*_chk family takes
 * two extra leading arguments (dest length and fortify flags). */
static int my___vsnprintf_chk(char *d, size_t n, int flag, size_t dl,
                              const char *fmt, va_list ap)
{
    (void)flag; (void)dl;
    return vsnprintf(d, n, fmt, ap);
}
static int my___snprintf_chk(char *d, size_t n, int flag, size_t dl,
                             const char *fmt, ...)
{
    (void)flag; (void)dl;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(d, n, fmt, ap);
    va_end(ap);
    return r;
}
static int my___vsprintf_chk(char *d, int flag, size_t dl, const char *fmt, va_list ap)
{
    (void)flag;
    return vsnprintf(d, dl, fmt, ap);
}
static int my___sprintf_chk(char *d, int flag, size_t dl, const char *fmt, ...)
{
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(d, dl, fmt, ap);
    va_end(ap);
    return r;
}
static void my___FD_SET_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_SET(fd, s); }
static void my___FD_CLR_chk(int fd, fd_set *s, size_t sz) { (void)sz; FD_CLR(fd, s); }
static int my___FD_ISSET_chk(int fd, fd_set *s, size_t sz) { (void)sz; return FD_ISSET(fd, s); }
static ssize_t my___read_chk(int fd, void *b, size_t n, size_t dl) { (void)dl; return read(fd, b, n); }

static void my___stack_chk_fail(void)
{
    /* This should never be reached.  Keep the Android caller visible in the
     * fatal message, since glibc's unwinder does not know about nx_elf's
     * manually mapped modules. */
    void *caller = __builtin_return_address(0);
    void *base = NULL;
    extern const char *bc_mod_at(const void *addr, void **base_out);
    const char *module = bc_mod_at(caller, &base);
    if (module && base)
        nx_die("__stack_chk_fail caller=%p %s+%#zx", caller, module,
               (size_t)((const unsigned char *)caller -
                        (const unsigned char *)base));
    nx_die("__stack_chk_fail caller=%p", caller);
}

/* --------------------------------------------------------------- atexit */

/* Handing the game's destructors to glibc's __cxa_atexit would register them
 * against our DSO handle and run them from exit(); the port leaves through
 * _exit() precisely because the engine's threads never stop, so keep the list
 * ourselves and never run it. */
static struct { void (*fn)(void *); void *arg; } dtors[256];
static int dtors_n;

static int my___cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (dtors_n < (int)(sizeof dtors / sizeof *dtors)) {
        dtors[dtors_n].fn = fn;
        dtors[dtors_n].arg = arg;
        dtors_n++;
    }
    return 0;
}
static void my___cxa_finalize(void *dso) { (void)dso; }
static void my___cxa_pure_virtual(void) { nx_die("pure virtual call"); }

/* bionic's fork bookkeeping entry point; glibc keeps it private, and nothing
 * here forks, so recording nothing is correct rather than merely harmless. */
static int my___register_atfork(void (*prepare)(void), void (*parent)(void),
                                void (*child)(void), void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0;
}

/* --------------------------------------------------------------- sigaction */

/* bionic/arm64: { int sa_flags; handler; sigset64_t mask(8); restorer; }
 * glibc/arm64:  { handler; sigset_t mask(128); int sa_flags; restorer; }     */
struct bionic_sigaction {
    int sa_flags;
    union {
        void (*handler)(int);
        void (*action)(int, void *, void *);
    } u;
    unsigned long sa_mask;
    void (*sa_restorer)(void);
};

static int my_sigaction(int sig, const struct bionic_sigaction *na,
                        struct bionic_sigaction *oa)
{
    struct sigaction g, og;
    if (na) {
        memset(&g, 0, sizeof g);
        g.sa_flags = na->sa_flags;
        if (na->sa_flags & SA_SIGINFO)
            g.sa_sigaction = (void (*)(int, siginfo_t *, void *))na->u.action;
        else
            g.sa_handler = na->u.handler;
        sigemptyset(&g.sa_mask);
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (na->sa_mask & (1UL << (i - 1)))
                sigaddset(&g.sa_mask, i);
    }
    int r = sigaction(sig, na ? &g : NULL, oa ? &og : NULL);
    if (oa && r == 0) {
        memset(oa, 0, sizeof *oa);
        oa->sa_flags = og.sa_flags;
        oa->u.handler = og.sa_handler;
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (sigismember(&og.sa_mask, i))
                oa->sa_mask |= 1UL << (i - 1);
    }
    return r;
}

/* bionic's sigset_t for the plain (non-64) calls is also a single word. */
static int my_sigemptyset(unsigned long *s) { if (s) *s = 0; return 0; }
static int my_sigfillset(unsigned long *s) { if (s) *s = ~0UL; return 0; }
static int my_sigaddset(unsigned long *s, int n) { if (s && n > 0) *s |= 1UL << (n - 1); return 0; }
static int my_sigdelset(unsigned long *s, int n) { if (s && n > 0) *s &= ~(1UL << (n - 1)); return 0; }
static int my_sigismember(const unsigned long *s, int n) { return s && n > 0 && (*s >> (n - 1)) & 1; }

static int my_sigsuspend(const unsigned long *m)
{
    sigset_t g;
    sigemptyset(&g);
    if (m)
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (*m & (1UL << (i - 1)))
                sigaddset(&g, i);
    return sigsuspend(&g);
}

static int my_pthread_sigmask(int how, const unsigned long *set,
                              unsigned long *old)
{
    sigset_t g, go;
    sigset_t *gp = NULL;
    if (set) {
        sigemptyset(&g);
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (*set & (1UL << (i - 1)))
                sigaddset(&g, i);
        gp = &g;
    }
    int r = pthread_sigmask(how, gp, old ? &go : NULL);
    if (old && r == 0) {
        *old = 0;
        for (int i = 1; i < 64 && i < _NSIG; i++)
            if (sigismember(&go, i))
                *old |= 1UL << (i - 1);
    }
    return r;
}

static int my_pthread_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void))
{
    (void)prepare;
    (void)parent;
    (void)child;
    return 0;
}

/* ------------------------------------------------------------------ ctype */

/* bionic's _ctype_ is a 1+256 byte table indexed as _ctype_[c+1]. */
static const unsigned char bc_ctype_tab[1 + 256] = { 0 };
static const unsigned char *bc_ctype_ptr = bc_ctype_tab + 1;

static int my___ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }

/* -------------------------------------------------------------------- misc */

static int my_ptrace(int req, ...)
{
    BIONIC_TRACE("ptrace(%d) -> EPERM", req);
    errno = EPERM;
    return -1;
}

static unsigned long my_getauxval(unsigned long t)
{
    unsigned long v = getauxval(t);
    BIONIC_TRACE("getauxval(%#lx) -> %#lx", t, v);
    return v;
}

static int my_stat(const char *path, struct stat *st)
{
    int r = stat(path, st);
    BIONIC_TRACE("stat(\"%s\") -> %d", path ? path : "(null)", r);
    return r;
}

static DIR *my_opendir(const char *path)
{
    DIR *d = opendir(path);
    BIONIC_TRACE("opendir(\"%s\") -> %p", path ? path : "(null)", (void *)d);
    return d;
}



static long my_sysconf(int name)
{
    /* bionic and glibc disagree on the _SC_* numbers, and Unity asks for the
     * page size, the core count and the cache line size while sizing its job
     * pool.  Translate the handful that matter and forward the rest. */
    switch (name) {
    case 0x27: return getpagesize();                    /* _SC_PAGESIZE */
    case 0x61: return sysconf(_SC_NPROCESSORS_CONF);    /* _SC_NPROCESSORS_CONF */
    case 0x62: return sysconf(_SC_NPROCESSORS_ONLN);    /* _SC_NPROCESSORS_ONLN */
    case 0x0a: return sysconf(_SC_OPEN_MAX);
    default:   return sysconf(name);
    }
}



static void *my_dlopen(const char *name, int flags);
static void *my_dlsym(void *h, const char *sym);
static int my_dlclose(void *h);
static const char *my_dlerror(void);
static int my_dladdr(const void *addr, void *info);
static int my_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data);

/* Declared in the other translation units of the port. */
extern void *bc_android_sym(const char *name);
extern void *bc_egl_sym(const char *name);
extern void *bc_jni_sym(const char *name);

static void *my_memalign(size_t a, size_t n)
{
    void *p = NULL;
    if (a < sizeof(void *))
        a = sizeof(void *);
    if (posix_memalign(&p, a, n) != 0)
        return NULL;
    return p;
}

static size_t my_malloc_usable_size(void *p) { return p ? malloc_usable_size(p) : 0; }

/* strlcpy only entered glibc in 2.38, while ArkOS/R36S ships glibc 2.30.
 * Android exposes it on every supported API level, so keep the bionic-facing
 * implementation inside the loader instead of inheriting the host libc
 * version.  The return value is the full source length, including when the
 * destination is truncated. */
static size_t my_strlcpy(char *dst, const char *src, size_t size)
{
    size_t source_length = strlen(src);
    if (size != 0) {
        size_t copy_length = source_length < size - 1 ? source_length : size - 1;
        memcpy(dst, src, copy_length);
        dst[copy_length] = '\0';
    }
    return source_length;
}

/* gettid was exported by glibc only from 2.30 onward.  The kernel ABI is the
 * same syscall on every AArch64 firmware we target, and this avoids making the
 * universal build depend on a libc wrapper merely because it exists. */
static pid_t my_gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/* ---------------------------------------------------------- import table */

#define E(n)      { #n, (void *)(uintptr_t)n }
#define M(n)      { #n, (void *)(uintptr_t)my_##n }
#define A(n, f)   { n,  (void *)(uintptr_t)(f) }

static nx_import tab[] = {
    /* logging / properties / bionic-only */
    M(__android_log_print), M(__android_log_write), M(__android_log_vprint),
    M(__android_log_assert), M(android_set_abort_message),
    M(__system_property_get), M(__system_property_find),
    M(__system_property_read), M(__system_property_read_callback),
    M(__system_property_foreach),
    M(__errno), M(__stack_chk_fail),
    M(__cxa_atexit), M(__cxa_finalize), M(__cxa_pure_virtual),
    A("__register_atfork", my___register_atfork),
    A("__sF", bc_sF), A("_ctype_", &bc_ctype_ptr),
    M(__ctype_get_mb_cur_max),

    /* FORTIFY */
    M(__memcpy_chk), M(__memmove_chk), M(__memset_chk), M(__strlen_chk),
    M(__strcpy_chk), M(__strcat_chk), M(__strncpy_chk), M(__strncat_chk),
    M(__strchr_chk), M(__umask_chk),
    M(__vsnprintf_chk), M(__snprintf_chk), M(__vsprintf_chk), M(__sprintf_chk),
    M(__FD_SET_chk), M(__FD_CLR_chk), M(__FD_ISSET_chk), M(__read_chk),

    /* stdio through the __sF translation */
    M(fprintf), M(vfprintf), M(fputc), M(fputs), M(fgetc), M(fgets),
    M(fwrite), M(fread), M(fflush), M(fclose), M(feof), M(ferror),
    M(clearerr), M(fileno), M(setbuf), M(setvbuf), M(fseek), M(ftell),
    M(rewind), M(ungetc), M(vprintf),
    E(fopen), E(fdopen), E(freopen), E(printf), E(snprintf), E(sprintf),
    E(vsnprintf), E(vsprintf), E(asprintf), E(vasprintf), E(sscanf),
    E(vsscanf), E(fscanf), E(puts), E(putchar), E(remove), E(rename), E(tmpfile),
    E(fseeko), E(ftello), E(getline), E(getdelim), E(swprintf),

    /* memory */
    M(malloc), E(free), M(calloc), M(realloc), M(posix_memalign),
    A("memalign", my_memalign), A("malloc_usable_size", my_malloc_usable_size),
    M(aligned_alloc), E(valloc), E(reallocarray),

    /* strings */
    E(memcpy), E(memmove), E(memset), E(memcmp), E(memchr), E(memrchr),
    E(mempcpy), E(strlen), E(strnlen), E(strcpy), E(strncpy), E(strcat),
    E(strncat), E(strcmp), E(strncmp), E(strcasecmp), E(strncasecmp),
    E(strchr), E(strrchr), E(strstr), E(strcasestr), E(strdup), E(strndup),
    M(strlcpy),
    E(strtok), E(strtok_r), E(strspn), E(strcspn), E(strpbrk), E(strerror),
    E(strerror_r), E(strsignal), E(strsep), E(strcoll), E(strxfrm),
    E(basename), E(dirname),
    E(wmemcpy), E(wmemmove), E(wmemset), E(wmemcmp), E(wmemchr),
    E(wcslen), E(wcscpy), E(wcscmp), E(wcsncmp), E(wcschr), E(wcsrchr),
    E(wcsstr), E(wcsdup), E(wcrtomb), E(wcsnrtombs), E(wcsrtombs),
    E(mbrlen), E(mbrtowc), E(mbsrtowcs), E(mbsnrtowcs), E(mbtowc), E(mblen),
    E(wctomb), E(btowc), E(wctob),

    /* conversion / locale */
    E(atoi), E(atol), E(atoll), E(strtol), E(strtoll), E(strtoul),
    E(strtoull), E(strtof), E(strtod), E(strtold), E(strtoll_l),
    E(strtoull_l), E(strtold_l), E(abs), E(labs), E(llabs), E(div), E(ldiv),
    E(lldiv), E(qsort), E(bsearch), E(newlocale), E(freelocale),
    E(duplocale), E(uselocale), E(setlocale), E(localeconv),
    E(strcoll_l), E(strxfrm_l), E(wcscoll_l), E(wcsxfrm_l),
    E(isdigit_l), E(islower_l), E(isupper_l), E(isxdigit_l), E(iswlower_l),
    E(iswupper_l), E(iswprint_l), E(iswspace_l), E(iswalpha_l),
    E(iswdigit_l), E(iswxdigit_l), E(iswblank_l), E(iswcntrl_l),
    E(iswpunct_l), E(toupper_l), E(tolower_l), E(towupper_l), E(towlower_l),
    E(towupper), E(towlower), E(iswalpha), E(iswcntrl), E(iswdigit),
    E(iswlower), E(iswprint), E(iswpunct), E(iswspace), E(iswupper),
    E(iswxdigit),
    E(isspace), E(isdigit), E(isalpha),
    E(isalnum), E(isupper), E(islower), E(isprint), E(ispunct),
    E(isxdigit), E(toupper), E(tolower), E(strftime), E(strftime_l),
    E(wcstol), E(wcstoul), E(wcstoll), E(wcstoull), E(wcstof), E(wcstod),
    E(wcstold), E(wcscoll), E(wcsxfrm), E(iswblank),

    /* process / signals / time */
    E(abort), E(exit), E(_exit), E(atexit), E(getenv), E(setenv), E(putenv),
    E(unsetenv), E(system), E(raise), E(signal), E(setjmp), E(longjmp),
    E(siglongjmp), E(sigaltstack), E(getpid), M(gettid), E(getppid), E(getuid),
    E(geteuid), E(getgid), E(getegid), E(getpwuid_r), E(getpwuid),
    E(getpagesize), E(setpriority), E(getpriority),
    E(prctl), E(syscall), E(uname), E(getrusage), E(getrlimit), E(setrlimit),
    E(sched_yield), E(sched_getaffinity), E(sched_setaffinity),
    E(sched_get_priority_min), E(sched_get_priority_max),
    E(gettimeofday), E(clock_gettime), E(clock_getres), E(nanosleep),
    E(usleep), E(sleep), E(time), E(clock), E(localtime), E(localtime_r),
    E(gmtime), E(gmtime_r), E(mktime), E(timegm), E(difftime),
    E(srand48), E(lrand48), E(drand48), E(rand), E(srand), E(rand_r),
    E(random), E(srandom), E(utime), E(utimes), E(gethostname),
    M(sigaction), M(sigemptyset), M(sigfillset), M(sigaddset), M(sigdelset),
    M(sigismember), M(sigsuspend), M(ptrace), M(sysconf),
    M(getauxval), M(stat), M(opendir),

    /* files */
    E(open), E(open64), E(openat), E(close), E(read), E(write), E(pread),
    E(pwrite), E(pread64), E(pwrite64), E(readv), E(writev), E(lseek),
    E(lseek64), E(fstat), E(lstat), E(fstatat), E(statfs),
    E(fstatfs), E(statvfs), E(fsync), E(fdatasync), E(ftruncate),
    E(truncate), E(access), E(faccessat), E(mkdir), E(mkdirat), E(rmdir),
    E(unlink), E(unlinkat), E(link), E(readlink), E(symlink), E(chmod),
    E(fchmod), E(futimens), E(sendfile),
    E(umask), E(chown), E(readdir), E(closedir), E(rewinddir),
    E(dirfd), E(realpath), E(getcwd), E(chdir), E(dup), E(dup2), E(dup3),
    E(pipe), E(pipe2), E(fcntl), E(ioctl), E(isatty), E(poll), E(select), E(flock),
    E(inotify_init), E(inotify_add_watch),
    E(fnmatch), M(mmap), E(mmap64), E(munmap), E(mprotect), E(madvise),
    E(msync), E(mremap),

    /* sockets */
    E(socket), E(connect), E(bind), E(listen), E(accept), E(send), E(recv),
    E(sendto), E(recvfrom), E(sendmsg), E(recvmsg), E(setsockopt),
    E(getsockopt), E(shutdown),
    E(getsockname), E(getpeername), E(inet_addr), E(inet_pton), M(inet_ntop),
    E(gethostbyname), E(gethostbyaddr), E(getaddrinfo), E(freeaddrinfo),
    E(getnameinfo), E(gai_strerror), E(if_nametoindex), E(htons), E(htonl),
    E(ntohs), E(ntohl),
    /* interface enumeration -- Mirror/SmartFox networking asks for the local
     * IP via getifaddrs; without it the symbol is unresolved and inet_ntop is
     * handed a null pointer (crash in inet_ntop+172).  The host glibc impl is
     * fine: the device has a real address, and the offline server connection
     * simply never completes. */
    E(getifaddrs), E(freeifaddrs),

    /* syslog (libunity links it but never enables it) */
    E(openlog), E(closelog), E(syslog),

    /* libm */
    E(acos), E(acosf), E(asin), E(asinf), E(atan), E(atanf), E(atan2),
    E(atan2f), E(cos), E(cosf), E(sin), E(sinf), E(tan), E(tanf), E(cosh),
    E(sinh), E(tanh), E(acosh), E(asinh), E(atanh), E(exp), E(expf), E(exp2),
    E(exp2f), E(expm1), E(log), E(logf), E(log2), E(log2f), E(log10),
    E(log10f), E(log1p), E(logb), E(ilogb), E(pow), E(powf), E(sqrt),
    E(sqrtf), E(cbrt), E(cbrtf), E(hypot), E(hypotf), E(fmod), E(fmodf),
    E(modf), E(modff), E(frexp), E(frexpf), E(ldexp), E(ldexpf), E(scalbn),
    E(scalbnf), E(ceil), E(ceilf), E(floor), E(floorf), E(round), E(roundf),
    E(trunc), E(truncf), E(rint), E(nearbyint), E(fabs), E(fabsf), E(fmin),
    E(fmax), E(fdim), E(copysign), E(lgamma), E(tgamma), E(erf), E(erfc),
    E(remainder), E(remquo), E(sincos), E(sincosf), E(nan), E(finite),

    /* zlib -- libunity links libz for its bundle reader */
    E(inflate), E(inflateInit_), E(inflateInit2_), E(inflateEnd),
    E(inflateReset), E(inflateSetDictionary), E(inflateCopy), E(inflateSync),
    E(deflate), E(deflateInit_), E(deflateInit2_), E(deflateEnd),
    E(deflateReset), E(deflateBound), E(compress), E(compress2),
    E(uncompress), E(crc32), E(adler32), E(zlibVersion), E(zError),

    /* pthread calls whose objects are plain scalars on arm64: pthread_t is a
     * pointer and pthread_key_t an unsigned int in both libcs, so no bridge. */
    E(pthread_self), E(pthread_join), E(pthread_detach), E(pthread_equal),
    E(pthread_exit), E(pthread_kill), E(pthread_key_create),
    E(pthread_key_delete), E(pthread_getspecific), E(pthread_setspecific),
    M(pthread_sigmask), M(pthread_atfork), E(pthread_getcpuclockid),

    /* libdl -- our own, so the game only ever sees modules we loaded */
    M(dlopen), M(dlsym), M(dlclose), M(dlerror), M(dladdr), M(dl_iterate_phdr),
};

/* Modules whose exports we hand out through the fake dlopen/dlsym. */
static const char *const fake_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
    "libandroid.so", "libEGL.so", "libGLESv2.so", "libGLESv3.so",
    "libmediandk.so", "libOpenSLES.so",
    "libunity.so", "libil2cpp.so", "libmain.so",
};
#define FAKE_HANDLE(i) ((void *)(uintptr_t)(0xD1000000u + (unsigned)(i)))

/* SDK libs carregados via dlopen real do host (pra P/Invokes resolverem). */
#define MAX_SDK_H 16
static void *sdk_handles[MAX_SDK_H];
static int sdk_count = 0;
#define SDK_HANDLE(i) ((void *)(uintptr_t)(0xD2000000u + (unsigned)(i)))
#define IS_SDK_HANDLE(h) ((uintptr_t)(h) >= 0xD2000000u && (uintptr_t)(h) < 0xD2000000u + MAX_SDK_H)

void sk3_preload_sdks(const char *const *names)
{
    for (const char *const *p = names; *p && sdk_count < MAX_SDK_H; p++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/lib/%s", bc_gamedir, *p);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            sdk_handles[sdk_count] = h;
            nx_log("preload SDK: %s OK (handle %d)", *p, sdk_count);
            sdk_count++;
        } else {
            nx_log("preload SDK: %s FAIL: %s", *p, dlerror());
        }
    }
}

static void *my_dlopen(const char *name, int flags)
{
    (void)flags;
    if (!name || !*name)
        return FAKE_HANDLE(0);            /* self / global scope */
    const char *slash = strrchr(name, '/');
    const char *b = slash ? slash + 1 : name;
    for (size_t i = 0; i < sizeof fake_libs / sizeof *fake_libs; i++)
        if (strcmp(fake_libs[i], b) == 0) {
            nx_mod *module = nx_find_mod(b);
            if (module)
                nx_run_init(module);
            BIONIC_TRACE("dlopen(\"%s\") -> handle %zu", name, i);
            return FAKE_HANDLE(i);
        }
    /* SDK libs: tentar dlopen real do host a partir de <gamedir>/lib/ */
    if (sdk_count < MAX_SDK_H) {
        char path[1200];
        snprintf(path, sizeof path, "%s/lib/%s", bc_gamedir, b);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            int idx = sdk_count++;
            sdk_handles[idx] = h;
            nx_log("dlopen(%s) -> SDK carregado via host: %s (handle %d)", b, path, idx);
            return SDK_HANDLE(idx);
        }
        nx_log("dlopen(%s) -> host falhou: %s", b, dlerror());
    }
    nx_log("dlopen(%s) -> global scope", name);
    return FAKE_HANDLE(0);
}

static int64_t sk3_ivory_stub0(void) { return 0; }

static void *my_dlsym(void *h, const char *sym)
{
    if (!sym)
        return NULL;
    /* SDK libs: dlsym real do host. */
    if (IS_SDK_HANDLE(h)) {
        int idx = (int)((uintptr_t)h - 0xD2000000u);
        void *r = dlsym(sdk_handles[idx], sym);
        BIONIC_TRACE("dlsym(sdk[%d], \"%s\") -> %p", idx, sym, r);
        return r;
    }
    /* Data symbols requested through Android's global dlsym scope. */
    if (strcmp(sym, "environ") == 0)
        return &environ;

    /* Fake handles for original game objects retain normal dlsym(handle, ...)
     * scope.  This matters for JNI_OnLoad: all three libraries export that
     * name, and NativeLoader must receive the one belonging to the handle it
     * just opened. */
    void *a = NULL;
    uintptr_t handle = (uintptr_t)h;
    uintptr_t first = (uintptr_t)FAKE_HANDLE(0);
    size_t fake_count = sizeof fake_libs / sizeof *fake_libs;
    if (handle >= first && handle < first + fake_count) {
        size_t index = (size_t)(handle - first);
        nx_mod *module = nx_find_mod(fake_libs[index]);
        if (module)
            a = nx_lookup_in(module, sym);
    }
    if (!a) a = nx_resolve_import(sym);
    if (!a) a = bc_android_sym(sym);
    if (!a) a = bc_egl_sym(sym);
    if (!a) a = bc_jni_sym(sym);
    if (!a) a = nx_lookup(sym);
    if (!a) {
        /* SDK libs: buscar nos handles pre-carregados */
        for (int i = 0; i < sdk_count && !a; i++)
            a = dlsym(sdk_handles[i], sym);
    }
    /* Hook il2cpp_class_from_name: retornar wrapper que loga nulls */
    if (strcmp(sym, "il2cpp_class_from_name") == 0) {
        extern void *nx_lookup(const char *);
        void *real = nx_lookup("il2cpp_class_from_name");
        if (real) {
            extern void sk3_set_class_from_name(void *);
            extern void *my_class_from_name(void *, const char *, const char *);
            sk3_set_class_from_name(real);
            return my_class_from_name;
        }
    }
    /* Hook il2cpp_class_from_il2cpp_type: logar nulls (este é o path do deserializador) */
    if (strcmp(sym, "il2cpp_class_from_il2cpp_type") == 0) {
        extern void *nx_lookup(const char *);
        void *real = nx_lookup("il2cpp_class_from_il2cpp_type");
        if (real) {
            extern void sk3_set_cfit(void *);
            extern void *my_cfit(void *);
            sk3_set_cfit(real);
            return my_cfit;
        }
    }
    /* IvorySDK (lojas/notificacoes/telemetria da Ratrod) e' P/Invoke numa lib
     * Android que nao carrega no host.  Sem simbolo o IL2CPP lanca
     * EntryPointNotFoundException; o jogo engole, mas repete Ivory_*_IsReady
     * TODO frame (12k excecoes com stack trace numa partida) e o .cctor de
     * Ivory_CSharp morre na primeira chamada.  Um stub que devolve 0/false/NULL
     * para QUALQUER Ivory_* = SDK presente mas inerte ("nao esta pronto"). */
    if (!a && strncmp(sym, "Ivory_", 6) == 0) {
        nx_log("dlsym(%s) -> stub Ivory (0)", sym);
        return (void *)sk3_ivory_stub0;
    }
    if (!a)
        nx_log("dlsym(%s) -> NULL", sym);
    else
        /* Traced on success too, not just on failure: the VM resolves what it
         * needs through here, so a wrong answer is indistinguishable from a
         * missing one in the crash that follows, and only the trace separates
         * them. */
        BIONIC_TRACE("dlsym(\"%s\") -> %p", sym, a);
    return a;
}

static int my_dlclose(void *h) { (void)h; return 0; }
static const char *my_dlerror(void) { return NULL; }

static int my_dladdr(const void *addr, void *info)
{
    struct { const char *fname; void *fbase; const char *sname; void *saddr; } *i = info;
    extern const char *bc_mod_at(const void *addr, void **base_out);
    void *base = NULL;
    const char *name = bc_mod_at(addr, &base);

    BIONIC_TRACE("dladdr(%p) -> %s", addr, name ? name : "(none)");
    if (!i || !name)
        return 0;                 /* zero is failure, and then info is untouched */
    i->fname = name;
    i->fbase = base;
    i->sname = NULL;              /* bionic leaves these null for an address
                                   * that is not exactly a symbol */
    i->saddr = NULL;
    return 1;                     /* nonzero is success */
}

static int my_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data)
{
    /* Android code can enumerate the native link map, so the modules mapped by
     * this loader have to be visible here. */
    return bc_iterate_mods(cb, data);
}

/* --------------------------------------------------------------- publish */

static int cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

void bc_bionic_init(void)
{
    qsort(tab, sizeof tab / sizeof *tab, sizeof *tab, cmp);
    nx_log("bionic table: %zu symbols", sizeof tab / sizeof *tab);
}

size_t bc_bionic_count(void) { return sizeof tab / sizeof *tab; }

nx_import *bc_bionic_entries(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}
