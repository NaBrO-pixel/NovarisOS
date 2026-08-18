/* uspace64.c - a Linux process's heap, its mappings, and its first stack. */

#include "uspace64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"
#include "proc64.h"

#define KERNEL_VMA  0xFFFFFFFF80000000ULL

/* The auxiliary vector types glibc's startup actually reads. */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25

static uint64_t pages_allocated;

uint64_t uspace64_pages_allocated(void) { return pages_allocated; }

/* The heap and the mmap bump pointer live in the process now rather
 * than here. A forked child gets its own copy of both; a static shared
 * between them would give two processes one break.
 *
 * Reached through a function rather than a macro named after the field,
 * which expands inside `p->brk_base` and produces a syntax error a long
 * way from where it was written. */
static proc64_t* cur(void) { return proc64_current(); }

void uspace64_reset(vmspace64_t* space, uint64_t brk_start) {
    proc64_t* p = cur();
    (void)space;
    if (!p) return;
    p->brk_base    = brk_start;
    p->brk_current = brk_start;
    p->mmap_next   = USPACE64_MMAP_BASE;
    pages_allocated = 0;
}

/* Maps [start, end) as anonymous zeroed user memory. Returns 0 on
 * failure, having mapped whatever it managed - the callers below undo
 * their own partial work by simply not advancing their pointer. */
/* `zero_existing` says what to do with a page that is already mapped in
 * the range. A fresh mmap must hand back zeros, so it says yes; brk
 * says no, because it grows the heap from an unaligned break and the
 * page holding the break is full of live heap. Zeroing there wipes the
 * allocator's own data, which presents as "stack smashing detected"
 * from a program that never touched its stack. */
static int map_anon(uint64_t start, uint64_t end, uint64_t flags,
                    int zero_existing) {
    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame, existing;

        if (paging64_translate(va, &existing) == PAGING64_OK) {
            /* Already mapped - keep the page, but adopt the NEW
             * permissions. A loader reserves a library's whole range
             * read-only and then maps each segment over it with
             * MAP_FIXED and the permissions that segment needs; leaving
             * the reservation's flags in place makes the data segment
             * read-only, and what that looks like is ld.so faulting on
             * its own relocations.
             *
             * And zero it. An anonymous mapping reads as zeros - that is
             * the whole of what "anonymous" promises - and a page being
             * recycled from an earlier mapping is exactly the case where
             * that is easy to get wrong. A loader maps a library's whole
             * span from the file, then maps the .bss over the tail of it
             * with MAP_ANONYMOUS|MAP_FIXED; skip the zeroing and the
             * .bss contains whatever the file had at that offset. What
             * that looks like is a NULL list terminator that is not
             * NULL, in a library whose relocations all applied fine. */
            existing &= ~(PAGE64_SIZE - 1);
            if (zero_existing)
                kmemset(phys64_to_virt(existing), 0, PAGE64_SIZE);
            paging64_map(va, existing, flags);
            continue;
        }

        frame = pmm64_alloc_high();
        if (!frame) return 0;
        /* Zeroed through the direct map, which reaches all of RAM since
         * Milestone 66 - so a frame's address no longer decides whether
         * a process is allowed to have it. */
        kmemset(phys64_to_virt(frame), 0, PAGE64_SIZE);

        if (paging64_map(va, frame, flags) != PAGING64_OK) {
            pmm64_free_frame(frame);
            return 0;
        }
        pages_allocated++;
    }
    return 1;
}

uint64_t uspace64_brk(uint64_t addr) {
    uint64_t want;

    if (addr == 0 || addr < cur()->brk_base) return cur()->brk_current;

    want = (addr + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);
    if (want > cur()->brk_current) {
        if (!map_anon(cur()->brk_current, want,
                      PAGE64_PRESENT | PAGE64_WRITE | PAGE64_USER, 0))
            return cur()->brk_current;   /* Linux reports failure this way */
    }
    cur()->brk_current = addr;
    return cur()->brk_current;
}

#define MAP_FIXED 0x10

uint64_t uspace64_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                       uint64_t flags) {
    uint64_t start, end, pflags;

    if (length == 0) return (uint64_t)-22;         /* -EINVAL */
    length = (length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);

    /* MAP_FIXED means exactly here, and it is not optional: a dynamic
     * loader reserves one range for a library and then maps each
     * segment into its place with MAP_FIXED. Give it a different
     * address and the segments land wherever, while the relocations
     * still point at where they were asked to go.
     *
     * Pages already mapped in the range are kept rather than replaced -
     * the caller is overwriting a reservation it made itself, and the
     * contents are about to be written anyway. */
    if (flags & MAP_FIXED) {
        start = addr & ~(PAGE64_SIZE - 1);
    } else {
        /* A hint is honoured only when it is free; anything else comes
         * out of the bump region. */
        start = addr ? (addr & ~(PAGE64_SIZE - 1)) : cur()->mmap_next;
        if (addr) {
            uint64_t probe;
            if (paging64_translate(start, &probe) == PAGING64_OK)
                start = cur()->mmap_next;
        }
    }
    end = start + length;

    pflags = PAGE64_PRESENT | PAGE64_USER;
    if (prot & 0x2) pflags |= PAGE64_WRITE;        /* PROT_WRITE */

    if (!map_anon(start, end, pflags, 1)) return (uint64_t)-12; /* -ENOMEM */

    /* A fixed mapping does not move the bump pointer: it was placed by
     * the caller, in a range the caller is keeping track of. */
    if (!(flags & MAP_FIXED) && start == cur()->mmap_next) cur()->mmap_next = end;
    return start;
}

/* Maps a fixed range of physical memory into the calling process.
 *
 * Every other mapping in this file hands out frames from the allocator,
 * and the process may have them. This one hands out a device that
 * already exists at an address nobody chose - so it takes the physical
 * address as an argument instead of asking for one, and it must not ever
 * be freed back to the allocator, which is why munmap of it only tears
 * down the page tables.
 *
 * Uncacheable for the same reason the kernel's own mapping is: a process
 * writing to VRAM through a write-back mapping is writing to the cache,
 * and the screen shows whatever gets evicted. */
uint64_t uspace64_map_phys(uint64_t length, uint64_t phys, uint64_t prot) {
    uint64_t start, off, pflags;

    if (length == 0) return (uint64_t)-22;            /* -EINVAL */
    length = (length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);
    if (phys & (PAGE64_SIZE - 1)) return (uint64_t)-22;

    start = cur()->mmap_next;

    pflags = PAGE64_PRESENT | PAGE64_USER | PAGE64_PCD | PAGE64_PWT;
    if (prot & 0x2) pflags |= PAGE64_WRITE;           /* PROT_WRITE */

    for (off = 0; off < length; off += PAGE64_SIZE) {
        if (paging64_map(start + off, phys + off, pflags) != PAGING64_OK) {
            /* Undo the part that was mapped, so a failure leaves the
             * address space as it was rather than half a framebuffer. */
            for (uint64_t done = 0; done < off; done += PAGE64_SIZE)
                paging64_unmap(start + done);
            return (uint64_t)-12;                     /* -ENOMEM */
        }
    }

    cur()->mmap_next = start + length;
    return start;
}

void uspace64_protect(uint64_t addr, uint64_t length, uint64_t prot) {
    uint64_t start = addr & ~(PAGE64_SIZE - 1);
    uint64_t end = start + ((length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1));
    uint64_t flags = PAGE64_PRESENT | PAGE64_USER;

    if (prot & 0x2) flags |= PAGE64_WRITE;             /* PROT_WRITE */

    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame;
        if (paging64_translate(va, &frame) != PAGING64_OK) continue;
        paging64_map(va, frame & ~(PAGE64_SIZE - 1), flags);
    }
}

uint64_t uspace64_munmap(uint64_t addr, uint64_t length) {
    uint64_t start = addr & ~(PAGE64_SIZE - 1);
    uint64_t end;

    if (length == 0) return (uint64_t)-22;
    end = start + ((length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1));

    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame;
        if (paging64_translate(va, &frame) != PAGING64_OK) continue;
        paging64_unmap(va);
        pmm64_free_frame(frame & ~(PAGE64_SIZE - 1));
    }
    return 0;
}

uint64_t uspace64_build_stack(vmspace64_t* space, uint64_t stack_top,
                              uint64_t stack_pages, const char* const* argv,
                              const elf64_info_t* elf,
                              uint64_t interp_base,
                              const char* const* envp) {
    uint64_t saved_cr3, p, random_va, rsp;
    uint64_t argv_va[8], env_va[16];
    uint64_t* v;
    uint64_t n;
    int nenv = 0, nargv = 0;
    (void)stack_pages;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(space->pml4_phys) : "memory");

    p = stack_top;

    /* AT_RANDOM has to point at sixteen bytes the program can read:
     * glibc copies them into the stack guard and the pointer guard
     * before main runs, and dereferences it unconditionally. */
    p -= 16;
    random_va = p;
    for (int i = 0; i < 16; i++)
        ((uint8_t*)p)[i] = (uint8_t)(0x5A + i * 7);

    for (nargv = 0; argv && argv[nargv]; nargv++) { }
    if (nargv > (int)(sizeof(argv_va) / sizeof(argv_va[0])))
        nargv = (int)(sizeof(argv_va) / sizeof(argv_va[0]));
    for (int a = nargv - 1; a >= 0; a--) {
        uint64_t l = kstrlen(argv[a]) + 1;
        p -= l;
        argv_va[a] = p;
        kmemcpy((void*)p, argv[a], l);
    }

    /* The environment, laid down the same way. A program with an empty
     * environment is not a normal program: glibc looks up the user to
     * find HOME, and Wine derives its prefix from it, so "no variables
     * at all" turns into a NULL dereference deep inside a library. */
    for (nenv = 0; envp && envp[nenv]; nenv++) { }
    if (nenv > (int)(sizeof(env_va) / sizeof(env_va[0])))
        nenv = (int)(sizeof(env_va) / sizeof(env_va[0]));
    for (int e = nenv - 1; e >= 0; e--) {
        uint64_t l = kstrlen(envp[e]) + 1;
        p -= l;
        env_va[e] = p;
        kmemcpy((void*)p, envp[e], l);
    }

    p &= ~15ULL;

    /* argc, argv[0], NULL, envp NULL, then sixteen auxv pairs. Counted
     * exactly so that rsp lands 16-byte aligned *pointing at argc*,
     * which is what the ABI specifies at process entry - glibc's
     * _start does an `and rsp, -16` of its own, but only after reading
     * argc off the stack it was given. */
    n = 3 + (uint64_t)nargv + (uint64_t)nenv + 2 * 16;
    p -= n * 8;
    p &= ~15ULL;
    rsp = p;

    v = (uint64_t*)p;
    *v++ = (uint64_t)nargv;      /* argc */
    for (int a = 0; a < nargv; a++) *v++ = argv_va[a];
    *v++ = 0;                    /* argv terminator */
    for (int e = 0; e < nenv; e++) *v++ = env_va[e];
    *v++ = 0;                    /* envp terminator */

    *v++ = AT_PHDR;   *v++ = elf->phdr_va;
    *v++ = AT_PHENT;  *v++ = elf->phent;
    *v++ = AT_PHNUM;  *v++ = elf->phnum;
    *v++ = AT_PAGESZ; *v++ = PAGE64_SIZE;
    /* Where the dynamic loader was mapped. ld.so reads this to find
     * itself, because it has to relocate its own image before it can
     * relocate anything else. Zero for a static binary. */
    *v++ = AT_BASE;   *v++ = interp_base;
    *v++ = AT_FLAGS;  *v++ = 0;
    *v++ = AT_ENTRY;  *v++ = elf->entry;
    *v++ = AT_UID;    *v++ = 0;
    *v++ = AT_EUID;   *v++ = 0;
    *v++ = AT_GID;    *v++ = 0;
    *v++ = AT_EGID;   *v++ = 0;
    *v++ = AT_SECURE; *v++ = 0;
    *v++ = AT_CLKTCK; *v++ = 100;
    *v++ = AT_HWCAP;  *v++ = 0;
    *v++ = AT_RANDOM; *v++ = random_va;
    *v++ = AT_NULL;   *v++ = 0;

    __asm__ __volatile__("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return rsp;
}
