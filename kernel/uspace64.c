/* uspace64.c - a Linux process's heap, its mappings, and its first stack. */

#include "uspace64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"
#include "proc64.h"
#include "vmspace64.h"

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

/* Does [start, start+length) lie wholly inside the user half? Written
 * as a subtraction rather than start+length so that a length large
 * enough to wrap is refused instead of wrapping into a range that looks
 * fine - which is the shape a caller probing for the limit produces. */
static int fits_in_user_half(uint64_t start, uint64_t length) {
    if (start >= USPACE64_LIMIT) return 0;
    return length <= USPACE64_LIMIT - start;
}

/* Is every page in [start, end) unmapped? Asked before a mapping that
 * must not disturb anything already there - which means asking about
 * the whole range and not just a page of it. */
static int range_free(uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame;
        if (paging64_translate(va, &frame) == PAGING64_OK) return 0;
    }
    return 1;
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
#define MAP_FIXED_NOREPLACE 0x100000

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
    if (flags & MAP_FIXED_NOREPLACE) {
        /* Exactly here, and only if the whole range is free. Linux
         * fails this with EEXIST and changes nothing, and the caller is
         * entitled to treat the refusal as information.
         *
         * This is the flag Wine reserves its address space with, and
         * getting it wrong is not a lost reservation, it is memory
         * corruption at a distance. Wine asks to reserve
         * 0x7ffffe000000..0x7fffffff0000 PROT_NONE; that range contains
         * this kernel's user stack, so on Linux the call fails and
         * reserve_area halves the range and recurses around the stack.
         * Without the check the range was mapped anyway and map_anon
         * zeroed every page already in it - all 128 pages of Wine's own
         * stack, saved return addresses included. Nothing faulted at the
         * time: execution ran on until the first `ret` popped a zeroed
         * return address and jumped to 0. Milestone 71 recorded that as
         * "calls through a null pointer" and it was this.
         *
         * Note there is no MAP_FIXED here to test: the flag implies
         * fixed placement by itself, which is why the hint path below
         * used to take these calls and honour a 32MB request after
         * probing a single page of it. */
        start = addr & ~(PAGE64_SIZE - 1);
        if (!fits_in_user_half(start, length)) return (uint64_t)-12; /* -ENOMEM */
        end   = start + length;
        if (!range_free(start, end)) return (uint64_t)-17;   /* -EEXIST */
    } else if (flags & MAP_FIXED) {
        start = addr & ~(PAGE64_SIZE - 1);
        if (!fits_in_user_half(start, length)) return (uint64_t)-12; /* -ENOMEM */
        end   = start + length;
    } else {
        /* A hint is honoured only when it fits and is free; anything
         * else comes out of the bump region. The whole range has to be
         * free, not just its first page - a hint that fits everywhere
         * except its last page is not a hint that fits. */
        start = addr ? (addr & ~(PAGE64_SIZE - 1)) : cur()->mmap_next;
        if (addr && !(fits_in_user_half(start, length) &&
                      range_free(start, start + length)))
            start = cur()->mmap_next;
        if (!fits_in_user_half(start, length)) return (uint64_t)-12;
        end = start + length;
    }

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

/* mprotect(2), for the one distinction this kernel's page tables draw:
 * whether the process may write.
 *
 * PROT_NONE is mapped onto "readable but not writable" rather than onto
 * an absent page, and that is a deliberate limit rather than an
 * oversight. A PROT_NONE range is still a range somebody has taken, and
 * the only record this kernel keeps of who has taken what is the page
 * tables themselves - so unmapping it would make Wine's own PROT_NONE
 * address-space reservations read as free, and the very next
 * MAP_FIXED_NOREPLACE would be told it could have them. Reads that
 * ought to fault therefore succeed. PROT_EXEC is likewise not
 * distinguished, because nothing here sets NX.
 *
 * What this must not do is get PROT_WRITE wrong, which is what it was
 * doing by not existing: the mprotect case in syscall64.c returned 0
 * without touching anything, so Wine's mprotect of its own PE image from
 * read-only to read-write was answered "done" and the first write to it
 * faulted with nobody to blame. */
void uspace64_protect(uint64_t addr, uint64_t length, uint64_t prot) {
    uint64_t start = addr & ~(PAGE64_SIZE - 1);
    uint64_t end = start + ((length + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1));

    for (uint64_t va = start; va < end; va += PAGE64_SIZE)
        vmspace64_set_writable(va, (prot & 0x2) != 0);   /* PROT_WRITE */
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
