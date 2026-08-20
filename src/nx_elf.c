/* nx_elf -- see nx_elf.h */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "nx_elf.h"

int nx_verbose = 0;
int nx_unresolved = 0;

static nx_mod *mods;
static const nx_import *imports;
static size_t imports_n;

void nx_log(const char *fmt, ...)
{
    if (!nx_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[sk3] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

void nx_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[sk3] FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    _exit(1);
}

void nx_set_imports(const nx_import *tab, size_t n)
{
    imports = tab;
    imports_n = n;
}

void *nx_resolve_import(const char *sym)
{
    /* The table is kept sorted by name so this is a binary search; the loader
     * does ~30k lookups over five modules and a linear scan showed up in the
     * startup time on the device. */
    size_t lo = 0, hi = imports_n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(imports[mid].name, sym);
        if (c == 0)
            return imports[mid].addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* ------------------------------------------------------------------ mapping */

static size_t page_up(size_t x)
{
    size_t p = (size_t)getpagesize();
    return (x + p - 1) & ~(p - 1);
}

static size_t page_down(size_t x)
{
    size_t p = (size_t)getpagesize();
    return x & ~(p - 1);
}

static int prot_of(Elf64_Word f)
{
    int p = 0;
    if (f & PF_R)
        p |= PROT_READ;
    if (f & PF_W)
        p |= PROT_WRITE;
    if (f & PF_X)
        p |= PROT_EXEC;
    return p;
}

static size_t gnu_hash_nsym(nx_mod *m);

nx_mod *nx_load(const char *path, const char *soname)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        nx_log("open %s: %s", path, strerror(errno));
        return NULL;
    }

    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64) {
        nx_log("%s is not an arm64 ELF", path);
        close(fd);
        return NULL;
    }

    size_t phsz = (size_t)eh.e_phnum * eh.e_phentsize;
    Elf64_Phdr *ph = malloc(phsz);
    if (!ph || pread(fd, ph, phsz, eh.e_phoff) != (ssize_t)phsz) {
        free(ph);
        close(fd);
        return NULL;
    }

    /* Span of the whole image, so every segment lands at base + p_vaddr. */
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (size_t i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr;
        uintptr_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end > hi)
            hi = end;
    }
    if (lo == (uintptr_t)-1)
        nx_die("%s has no PT_LOAD", path);

    size_t span = page_up(hi) - page_down(lo);
    uint8_t *base = mmap(NULL, span, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
        nx_die("reserve %zu bytes for %s: %s", span, soname, strerror(errno));
    base -= page_down(lo);

    for (size_t i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD)
            continue;
        uintptr_t vstart = page_down(p->p_vaddr);
        size_t fend = p->p_vaddr + p->p_filesz;
        /* Relocations touch .data.rel.ro/.got and, in this build, a couple of
         * ABS64 slots inside the read-only segment; map everything writable and
         * tighten it after nx_relocate(). */
        int prot = prot_of(p->p_flags) | PROT_WRITE;
        if (p->p_filesz) {
            size_t flen = page_up(fend) - vstart;
            void *got = mmap(base + vstart, flen, prot,
                             MAP_PRIVATE | MAP_FIXED, fd,
                             (off_t)page_down(p->p_offset));
            if (got == MAP_FAILED)
                nx_die("map %s seg %zu: %s", soname, i, strerror(errno));
        }
        if (p->p_memsz > p->p_filesz) {
            /* .bss: anonymous zero pages after the file-backed part, and zero
             * the tail of the last file page by hand. */
            size_t bss_start = page_up(fend);
            size_t bss_end = page_up(p->p_vaddr + p->p_memsz);
            if (fend < bss_start)
                memset(base + fend, 0, bss_start - fend);
            if (bss_end > bss_start) {
                void *got = mmap(base + bss_start, bss_end - bss_start, prot,
                                 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (got == MAP_FAILED)
                    nx_die("map %s bss: %s", soname, strerror(errno));
            }
        }
    }
    close(fd);

    nx_mod *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", soname);
    snprintf(m->path, sizeof m->path, "%s", path);
    m->base = base;
    m->span = span;
    m->phdr = ph;
    m->phnum = eh.e_phnum;

    for (size_t i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            m->dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
    if (!m->dyn)
        nx_die("%s has no PT_DYNAMIC", soname);

    size_t relasz = 0, relaent = sizeof(Elf64_Rela), jmprelsz = 0;
    for (Elf64_Dyn *d = m->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   m->dynsym = (Elf64_Sym *)(base + d->d_un.d_ptr); break;
        case DT_STRTAB:   m->dynstr = (const char *)(base + d->d_un.d_ptr); break;
        case DT_STRSZ:    m->dynstr_sz = d->d_un.d_val; break;
        case DT_RELA:     m->rela = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_RELASZ:   relasz = d->d_un.d_val; break;
        case DT_RELAENT:  relaent = d->d_un.d_val; break;
        case DT_JMPREL:   m->jmprel = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
        case DT_PLTGOT:   m->pltgot = (uintptr_t *)(base + d->d_un.d_ptr); break;
        case DT_INIT:     m->init_func = (void (*)(void))(base + d->d_un.d_ptr); break;
        case DT_INIT_ARRAY: m->init_array = (void (**)(void))(base + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: m->init_n = d->d_un.d_val / sizeof(void *); break;
        case DT_HASH:     m->hash = (const uint32_t *)(base + d->d_un.d_ptr); break;
        case DT_GNU_HASH: m->gnu_hash = (const uint32_t *)(base + d->d_un.d_ptr); break;
        default: break;
        }
    }
    if (relaent == 0)
        relaent = sizeof(Elf64_Rela);
    m->rela_n = relasz / relaent;
    m->jmprel_n = jmprelsz / sizeof(Elf64_Rela);
    m->nsym = m->hash ? m->hash[1] : gnu_hash_nsym(m);
    if (!m->nsym && m->dynstr && m->dynsym) {
        /* Last resort: .dynstr follows .dynsym in every object here. */
        size_t bytes = (size_t)((const char *)m->dynstr - (const char *)m->dynsym);
        m->nsym = bytes / sizeof(Elf64_Sym);
    }

    m->next = mods;
    mods = m;
    nx_log("loaded %-22s base=%p span=%zu rela=%zu jmprel=%zu init_array=%zu",
           soname, (void *)base, span, m->rela_n, m->jmprel_n, m->init_n);
    return m;
}

/* DT_GNU_HASH does not store the symbol count, but the highest index reachable
 * through its bucket chains is the last symbol, which is what we need to walk
 * .dynsym linearly. */
static size_t gnu_hash_nsym(nx_mod *m)
{
    const uint32_t *h = m->gnu_hash;
    if (!h)
        return 0;
    uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
    const uint32_t *buckets = h + 4 + bloom_size * (sizeof(uint64_t) / 4);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last)
            last = buckets[i];
    if (last < symoffset)
        return symoffset;
    while (!(chain[last - symoffset] & 1))
        last++;
    return last + 1;
}

/* -------------------------------------------------------------- resolution */

void *nx_lookup_in(nx_mod *m, const char *sym)
{
    if (!m->dynsym || !m->dynstr || !m->nsym)
        return NULL;
    for (size_t i = 1; i < m->nsym; i++) {
        Elf64_Sym *s = &m->dynsym[i];
        if (s->st_shndx == SHN_UNDEF || !s->st_value)
            continue;
        if (strcmp(m->dynstr + s->st_name, sym) == 0)
            return m->base + s->st_value;
    }
    return NULL;
}

nx_mod *nx_find_mod(const char *soname)
{
    for (nx_mod *m = mods; m; m = m->next)
        if (strcmp(m->name, soname) == 0)
            return m;
    return NULL;
}

void *nx_lookup(const char *sym)
{
    for (nx_mod *m = mods; m; m = m->next) {
        void *a = nx_lookup_in(m, sym);
        if (a)
            return a;
    }
    return NULL;
}

static void *resolve(nx_mod *self, const char *name, int *missing)
{
    void *a = nx_resolve_import(name);
    if (a)
        return a;
    /* Prefer another module over the loading one: libunity pulls Burst and
     * il2cpp entry points, and libmain pulls nothing. */
    for (nx_mod *m = mods; m; m = m->next) {
        if (m == self)
            continue;
        a = nx_lookup_in(m, name);
        if (a)
            return a;
    }
    a = nx_lookup_in(self, name);
    if (a)
        return a;
    if (missing)
        (*missing)++;
    return NULL;
}

static void apply(nx_mod *m, Elf64_Rela *r, size_t n, int *missing)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t type = ELF64_R_TYPE(r[i].r_info);
        uint32_t sidx = ELF64_R_SYM(r[i].r_info);
        uintptr_t *slot = (uintptr_t *)(m->base + r[i].r_offset);
        const char *name = NULL;
        Elf64_Sym *sym = NULL;
        if (sidx && m->dynsym && m->dynstr) {
            sym = &m->dynsym[sidx];
            name = m->dynstr + sym->st_name;
        }

        switch (type) {
        case R_AARCH64_RELATIVE:
            *slot = (uintptr_t)m->base + r[i].r_addend;
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
        case R_AARCH64_ABS64: {
            void *a = NULL;
            if (sym && sym->st_shndx != SHN_UNDEF && sym->st_value)
                a = m->base + sym->st_value;      /* defined here */
            else if (name && *name)
                a = resolve(m, name, missing);
            if (!a) {
                if (name && *name && ELF64_ST_BIND(sym->st_info) != STB_WEAK)
                    nx_log("  unresolved %s <- %s", m->name, name);
                *slot = 0;
            } else {
                *slot = (uintptr_t)a + r[i].r_addend;
            }
            break;
        }
        case 1030:   /* R_AARCH64_TLS_TPREL64 */
        case 1031:   /* R_AARCH64_TLSDESC     */
            /* libunity's only TLS-relative relocation is the weak
             * _ZTH15gDeferredAction thread-init helper; leaving it zero keeps
             * the guarded path from firing. */
            *slot = 0;
            break;
        case R_AARCH64_NONE:
            break;
        default:
            nx_log("  %s: unhandled reloc type %u at %#lx", m->name, type,
                   (unsigned long)r[i].r_offset);
            break;
        }
    }
}

int nx_relocate(nx_mod *m)
{
    int missing = 0;
    if (m->rela)
        apply(m, m->rela, m->rela_n, &missing);
    if (m->jmprel)
        apply(m, m->jmprel, m->jmprel_n, &missing);
    m->relocated = 1;
    nx_unresolved = missing;
    nx_log("relocated %-22s unresolved=%d", m->name, missing);
    return missing;
}

void nx_run_init(nx_mod *m)
{
    if (m->inited)
        return;
    m->inited = 1;
    if (m->init_func) {
        nx_log("%s: DT_INIT %p", m->name, (void *)m->init_func);
        m->init_func();
    }
    for (size_t i = 0; i < m->init_n; i++) {
        void (*f)(void) = m->init_array[i];
        if (!f || f == (void (*)(void))-1)
            continue;
        m->init_array[i] = NULL;   /* an init that re-enters must not loop */
        f();
    }
    nx_log("%s: init done (%zu entries)", m->name, m->init_n);
}

/* ------------------------------------------------------------- got patching */

/* .got.plt slot n lives at pltgot[n]; slots 0..2 are the linker's. */
int nx_patch_pltgot(nx_mod *m, unsigned slot, void *fn)
{
    if (!m->pltgot)
        return -1;
    m->pltgot[slot] = (uintptr_t)fn;
    return 0;
}

uintptr_t nx_read_pltgot(nx_mod *m, unsigned slot)
{
    return m->pltgot ? m->pltgot[slot] : 0;
}

/* Which probe fired last.  x16/x17 are the procedure-call scratch registers, so
 * the stub can publish the slot number without touching an argument register;
 * a handler written in C cannot read a register itself once its prologue has
 * run.  Concurrent probes race here, which is fine for a bring-up aid. */
unsigned nx_probe_slot;

/* AArch64 stub, argument registers untouched:
 *
 *   ldr x16, =&nx_probe_slot
 *   ldr w17, =id
 *   str w17, [x16]
 *   ldr x16, =handler
 *   br  x16
 */
void *nx_make_probe(unsigned id, void *handler)
{
    static uint8_t *pool;
    static size_t used, cap;
    const size_t STUB = 64;

    if (!pool || used + STUB > cap) {
        cap = (size_t)getpagesize() * 8;
        pool = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pool == MAP_FAILED)
            nx_die("probe pool: %s", strerror(errno));
        used = 0;
    }
    uint8_t *s = pool + used;
    used += STUB;

    uint32_t *c = (uint32_t *)s;
    c[0] = 0x58000110;      /* ldr x16, .+0x20  -> &nx_probe_slot */
    c[1] = 0x18000131;      /* ldr w17, .+0x24  -> id             */
    c[2] = 0xB9000211;      /* str w17, [x16]                     */
    c[3] = 0x58000130;      /* ldr x16, .+0x24  -> handler        */
    c[4] = 0xD61F0200;      /* br  x16                            */
    c[5] = c[6] = c[7] = 0xD503201F;
    void *slotp = &nx_probe_slot;
    memcpy(s + 0x20, &slotp, sizeof slotp);
    memcpy(s + 0x28, &id, sizeof id);
    memcpy(s + 0x30, &handler, sizeof handler);

    __builtin___clear_cache((char *)s, (char *)s + STUB);
    return s;
}
