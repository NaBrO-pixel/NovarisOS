/* uspace64.c - a Linux process's heap, its mappings, and its first stack. */

#include "uspace64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"

#define KERNEL_VMA  0xFFFFFFFF80000000ULL
#define PHYS_WINDOW 0x40000000ULL

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

static vmspace64_t* proc_space;
static uint64_t brk_base, brk_current;
static uint64_t mmap_next;
static uint64_t pages_allocated;

uint64_t uspace64_pages_allocated(void) { return pages_allocated; }

void uspace64_reset(vmspace64_t* space, uint64_t brk_start) {
    proc_space  = space;
    brk_base    = brk_start;
    brk_current = brk_start;
    mmap_next   = USPACE64_MMAP_BASE;
    pages_allocated = 0;
}

/* Maps [start, end) as anonymous zeroed user memory. Returns 0 on
 * failure, having mapped whatever it managed - the callers below undo
 * their own partial work by simply not advancing their pointer. */
static int map_anon(uint64_t start, uint64_t end, uint64_t flags) {
    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame, existing;

        if (paging64_translate(va, &existing) == PAGING64_OK) continue;

        frame = pmm64_alloc_frame();
        if (!frame) return 0;
        /* Zeroed through the physical window, which only reaches the
         * first gigabyte. Beyond that a temporary mapping would be
         * needed, and this kernel has not needed one yet. */
        if (frame >= PHYS_WINDOW) { pmm64_free_frame(frame); return 0; }
        kmemset((void*)(KERNEL_VMA + frame), 0, PAGE64_SIZE);

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

    if (addr == 0 || addr < brk_base) return brk_current;

    want = (addr + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);
    if (want > brk_current) {
        if (!map_anon(brk_current, want,
                      PAGE64_PRESENT | PAGE64_WRITE | PAGE64_USER))
            return brk_current;   /* Linux reports failure this way */
    }
    brk_current = addr;
    return brk_current;
}

uint64_t uspace64_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                       uint64_t flags) {
    uint64_t start, end, pflags;
    (void)flags;

    if (length == 0) return (uint64_t)-22;         /* -EINVAL */
    length = (length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);

    /* A hint is honoured only when it is free; anything else comes out
     * of the bump region. MAP_FIXED is not implemented, and a caller
     * that needed it would be silently given the wrong address - which
     * is worth knowing about before something relies on it. */
    start = addr ? (addr & ~(PAGE64_SIZE - 1)) : mmap_next;
    if (addr) {
        uint64_t probe;
        if (paging64_translate(start, &probe) == PAGING64_OK)
            start = mmap_next;
    }
    end = start + length;

    pflags = PAGE64_PRESENT | PAGE64_USER;
    if (prot & 0x2) pflags |= PAGE64_WRITE;        /* PROT_WRITE */

    if (!map_anon(start, end, pflags)) return (uint64_t)-12;  /* -ENOMEM */

    if (start == mmap_next) mmap_next = end;
    return start;
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
                              uint64_t stack_pages, const char* argv0,
                              const elf64_info_t* elf) {
    uint64_t saved_cr3, p, argv0_va, random_va, rsp;
    uint64_t* v;
    uint64_t n;
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

    n = kstrlen(argv0) + 1;
    p -= n;
    argv0_va = p;
    kmemcpy((void*)p, argv0, n);

    p &= ~15ULL;

    /* argc, argv[0], NULL, envp NULL, then sixteen auxv pairs. Counted
     * exactly so that rsp lands 16-byte aligned *pointing at argc*,
     * which is what the ABI specifies at process entry - glibc's
     * _start does an `and rsp, -16` of its own, but only after reading
     * argc off the stack it was given. */
    n = 4 + 2 * 16;
    p -= n * 8;
    p &= ~15ULL;
    rsp = p;

    v = (uint64_t*)p;
    *v++ = 1;                    /* argc */
    *v++ = argv0_va;             /* argv[0] */
    *v++ = 0;                    /* argv terminator */
    *v++ = 0;                    /* envp terminator (no environment) */

    *v++ = AT_PHDR;   *v++ = elf->phdr_va;
    *v++ = AT_PHENT;  *v++ = elf->phent;
    *v++ = AT_PHNUM;  *v++ = elf->phnum;
    *v++ = AT_PAGESZ; *v++ = PAGE64_SIZE;
    *v++ = AT_BASE;   *v++ = 0;      /* no interpreter: this is static */
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
