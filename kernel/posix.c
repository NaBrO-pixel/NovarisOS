/* posix.c - the Linux/i386 system call ABI. See include/posix.h for why
 * this exists and what it is for. */

#include "posix.h"
#include "posix_proc.h"
#include "console.h"
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "vfs.h"
#include "kstring.h"
#include "rtc.h"
#include "pit.h"
#include "socket.h"
#include "kheap.h"
#include "elf.h"

#define PAGE_SIZE 4096u
/* Must match ELF_STACK_SIZE in kernel/process.c - reported through
 * ugetrlimit(RLIMIT_STACK), which glibc uses to size its guard pages. */
#define ELF_STACK_BYTES (16u * PAGE_SIZE)

/* --- file descriptors ---------------------------------------------------
 *
 * A real table, per process, and since Milestone 29 that is true rather
 * than aspirational: the table lives in the process structure keyed by
 * address space (include/posix_proc.h), so a forked child gets a copy and
 * a thread gets the same one. Descriptors 0/1/2 are the console; the rest
 * are open files, sockets and pipe ends. */

#define MAX_FDS POSIX_MAX_FDS

static int fd_alloc_from(posix_proc_t* p, int lowest) {
    if (lowest < 0) lowest = 0;
    for (int i = lowest; i < MAX_FDS; i++) {
        if (p->fds[i].kind == FD_FREE) return i;
    }
    return -1;
}

static int fd_alloc(void) {
    return fd_alloc_from(posix_current(), 0);
}

static fd_entry_t* fd_get(int fd) {
    posix_proc_t* p = posix_current();
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (p->fds[fd].kind == FD_FREE) return 0;
    return &p->fds[fd];
}

/* --- the seam with kernel/socket.c (Milestone 28) ------------------------
 *
 * Sockets are objects the socket layer owns; descriptors are a table this
 * file owns. These four functions are the whole of the boundary.
 *
 * `export`/`import` carry a descriptor over a socket for SCM_RIGHTS. The
 * token is the index of a saved copy of the entry, because the *number*
 * is the sender's and means nothing at the other end - what travels is
 * the object, not the name for it. */

#define MAX_IN_FLIGHT 32
static fd_entry_t in_flight[MAX_IN_FLIGHT];

/* The trap frame of the call being served. read() on a socket may have to
 * park the task, and parking means saving the frame - but read()'s
 * signature is Linux's and has nowhere to put one. */
static registers_t* cur_regs = 0;

/* An implementation asking to be run again later rather than returning
 * now. `addr` is what it wants to be woken on, `deadline` an absolute PIT
 * tick after which it wants to be woken anyway. See posix.h. */
static uint32_t block_addr = 0;
static uint32_t block_deadline = 0;

void posix_request_block(uint32_t addr, registers_t* regs) {
    block_addr = addr;
    block_deadline = 0;
    posix_request_retry();
    (void)regs;
}

void posix_request_block_until(uint32_t addr, uint32_t deadline,
                               registers_t* regs) {
    block_addr = addr;
    block_deadline = deadline;
    posix_request_retry();
    (void)regs;
}

int posix_fd_install_socket(struct socket* s) {
    posix_proc_t* p = posix_current();
    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;
    p->fds[fd].kind = FD_SOCKET;
    p->fds[fd].sock = s;
    p->fds[fd].node = 0;
    p->fds[fd].offset = 0;
    p->fds[fd].writable = 1;
    p->fds[fd].append = 0;
    p->fds[fd].dir_index = 0;
    p->fds[fd].cloexec = 0;
    return fd;
}

struct socket* posix_fd_socket(int fd) {
    fd_entry_t* e = fd_get(fd);
    if (!e || e->kind != FD_SOCKET) return 0;
    return e->sock;
}

uint32_t posix_fd_export(int fd) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return 0;
    for (int i = 0; i < MAX_IN_FLIGHT; i++) {
        if (in_flight[i].kind == FD_FREE) {
            in_flight[i] = *e;
            /* The sender keeps its descriptor, so the object is now named
             * twice: once by the sender and once by the message. The
             * reference travels with the message and is handed to the
             * receiver by posix_fd_import(). */
            if (e->kind == FD_SOCKET) socket_ref(e->sock);
            if (e->kind == FD_FILE) vfs_node_ref(e->node);
            return (uint32_t)(i + 1);   /* 0 means "nothing" */
        }
    }
    return 0;
}

int posix_fd_import(uint32_t token) {
    posix_proc_t* p = posix_current();
    if (!token || token > MAX_IN_FLIGHT) return -EBADF;
    fd_entry_t* src = &in_flight[token - 1];
    if (src->kind == FD_FREE) return -EBADF;
    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;
    p->fds[fd] = *src;
    src->kind = FD_FREE;
    return fd;
}

/* --- files kept alive by a mapping (Milestone 30) ------------------------
 *
 * A shared mapping outlives the descriptor it was made from, and on Unix
 * it outlives the file's *name* too: the ordinary way to make anonymous
 * shared memory is to create a file, open it, unlink it and map it, which
 * is exactly what wineserver does. So a mapping holds a reference.
 *
 * Released at the end of the program run rather than at munmap, because
 * nothing here records which file a given address range came from. That
 * is a real limit and a small one - the reference costs a node, the run
 * ends, and the table is fixed at four because Wine uses one. */
#define MAX_PINNED 4
static vfs_node_t* pinned[MAX_PINNED];

static void posix_pin_mapped(vfs_node_t* node) {
    for (int i = 0; i < MAX_PINNED; i++) {
        if (pinned[i] == node) return;      /* already held */
        if (!pinned[i]) {
            pinned[i] = node;
            vfs_node_ref(node);
            return;
        }
    }
}

static void posix_unpin_all(void) {
    for (int i = 0; i < MAX_PINNED; i++) {
        if (!pinned[i]) continue;
        vfs_node_unref(pinned[i]);
        pinned[i] = 0;
    }
}

/* --- the process's address space ----------------------------------------
 *
 * mmap and brk both hand out address space, so they need somewhere to
 * hand it out from. Both arenas sit clear of everything the loaders place
 * (paging_reserve_region() knows about them), and both are per-process:
 * the mappings live in the process's own page directory since Milestone
 * 15, so they vanish when it is destroyed. */

#define MMAP_ARENA_START 0x42000000u
#define MMAP_ARENA_END   0x48000000u
#define BRK_ARENA_START  0x48000000u
#define BRK_ARENA_END    0x49000000u

/* The path the running program was started from. Wine's loader finds its
 * own installation by reading /proc/self/exe, and without an answer it
 * cannot locate ntdll.so - which is exactly where it stopped the first
 * time it ran here. See ROADMAP.md Milestone 27. */
void posix_set_exe_path(const char* path) {
    posix_proc_t* p = posix_current();
    if (!path || !*path) {
        kstrlcpy(p->exe_path, "/program", sizeof(p->exe_path));
        return;
    }
    if (path[0] == '/') {
        kstrlcpy(p->exe_path, path, sizeof(p->exe_path));
    } else {
        p->exe_path[0] = '/';
        kstrlcpy(p->exe_path + 1, path, sizeof(p->exe_path) - 1);
    }
}

/* Linux's PROT_* to the x86 page-table bits. There is no way to say
 * "readable but not writable *and* not executable" on 32-bit x86 without
 * PAE's NX bit, so PROT_EXEC is accepted and ignored - noted here because
 * a silently-wrong protection is worth being explicit about. */
static uint32_t prot_to_pte(int prot) {
    /* PROT_NONE on a page that already has memory behind it is "present,
     * but not the program's" - the frame stays, PAGE_USER goes, and a
     * ring-3 access faults while the bytes survive. That distinction is
     * not academic: mprotect(PROT_NONE) followed by mprotect(READ|WRITE)
     * has to give the data back, which is how Wine's heap marks a block
     * inaccessible and then reuses it. An implementation that freed the
     * frame passed every test here and made Wine read its own heap
     * headers out of a page of zeroes. */
    if (prot == PROT_NONE) return PAGE_PRESENT;
    uint32_t flags = PAGE_USER | PAGE_PRESENT;
    if (prot & PROT_WRITE) flags |= PAGE_RW;
    return flags;
}

/* Wakes a parent suspended in vfork - defined with the fork machinery
 * below, declared here because the exit path is one of the two things it
 * waits for. */
static void vfork_release(posix_proc_t* child);

/* Ends the *process*, not just the task: releases its user memory, records
 * its exit status where a waiting parent can find it, and wakes that
 * parent. Called when the last task of an address space exits, and by the
 * fault path when a program dies of a signal.
 *
 * Releasing the memory here rather than leaving it to the end of the batch
 * is what makes a fork/exec loop possible at all: a forked child is a full
 * eager copy of its parent, so a shell-shaped program that forked a dozen
 * times would otherwise hold a dozen copies until every one of them had
 * finished. */
static void process_teardown(int status) {
    posix_proc_t* p = posix_current();

    /* Only the last thread of the process ends it - the others are still
     * running in this address space, and its pages are still theirs. */
    uint32_t pd = scheduler_current_address_space();
    if (pd && scheduler_as_sibling_count(pd) > 0) return;

    /* Every descriptor this process still holds. A socket's peer has to
     * find out, or a reader on the far side waits for a writer that has
     * gone - which is precisely how a pipe reports end of file. */
    for (int i = 3; i < MAX_FDS; i++) {
        if (p->fds[i].kind == FD_SOCKET) socket_close(p->fds[i].sock);
        if (p->fds[i].kind == FD_FILE) vfs_node_unref(p->fds[i].node);
        p->fds[i].kind = FD_FREE;
    }

    /* Only if these pages are this process's to release - see
     * posix_proc_t::owns_memory. A task the POSIX layer merely met is
     * running in somebody else's address space. */
    if (pd && p->owns_memory) paging_release_user_pages();

    /* A parent suspended in vfork is waiting for exactly this. */
    vfork_release(p);

    int ppid = p->ppid;
    posix_proc_exit(p, status);

    /* A parent parked in wait4 is waiting on its own structure's address.
     * Waking it costs nothing when there is no such parent. */
    posix_proc_t* parent = posix_proc_by_pid(ppid);
    if (parent) scheduler_wake_on((uint32_t)parent, 1, 0);
}

void posix_exit_status(int status) {
    process_teardown(status);
    if (scheduler_is_active()) {
        scheduler_exit_current();
        /* Returns normally; isr.s's epilogue performs the switch as this
         * call chain unwinds - see w32_thread_exit() for the same shape
         * and why halting here instead would hang. */
        return;
    }
    process_exit_to_kernel(); /* does not return */
}

void posix_exit_process(void) {
    posix_exit_status(0);
}

void posix_process_begin(void) {
    socket_process_begin();
    posix_proc_reset_all();
    for (int i = 0; i < MAX_IN_FLIGHT; i++) in_flight[i].kind = FD_FREE;

    posix_proc_t* p = posix_proc_create(0);
    if (!p) return;
    for (int i = 0; i < 3; i++) {
        p->fds[i].kind = FD_CONSOLE;
        p->fds[i].node = 0;
        p->fds[i].offset = 0;
        p->fds[i].writable = (i != 0);
    }
}

void posix_process_end(void) {
    socket_process_end();
    posix_unpin_all();
    /* The mappings themselves need no teardown: they were made in the
     * process's own page directory, which is about to be destroyed. Only
     * the bookkeeping goes, so the next program starts from a clean table
     * rather than inheriting a high-water mark - or a child nobody
     * collected. */
    posix_proc_reset_all();
}

uint32_t posix_unimplemented_count(void) {
    return posix_current()->unimplemented;
}

/* --- memory ------------------------------------------------------------- */

static int map_range(uint32_t base, uint32_t bytes, uint32_t flags) {
    for (uint32_t va = base; va < base + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        if (pte & PAGE_PRESENT) {
            /* Already mapped - which is the normal case for MAP_FIXED, and
             * exactly what a dynamic linker does: reserve the whole span
             * of a library with one PROT_NONE anonymous mapping, then map
             * each segment over it with MAP_FIXED and the protection that
             * segment actually wants. Keeping the frame but *not* applying
             * the new flags would leave every one of those segments with
             * the reservation's permissions, so a write to the GOT would
             * fault. Keep the page, take the new protection - and keep
             * PAGE_SHARED, which is not a protection but a fact about who
             * owns the frame. */
            paging_map_page(va, pte & ~0xFFFu, flags | (pte & PAGE_SHARED));
            continue;
        }
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(va, phys, flags);
        kmemset((void*)va, 0, PAGE_SIZE); /* anonymous memory reads as zero */
    }
    return 1;
}

static void unmap_range(uint32_t base, uint32_t bytes) {
    for (uint32_t va = base; va < base + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);
        if (!(pte & (PAGE_PRESENT | PAGE_RESERVED))) continue;
        paging_unmap_page(va);   /* clears a reservation too */
        if (!(pte & PAGE_PRESENT)) continue;
        /* A shared mapping's frames are the file's, not this process's -
         * unmapping is the whole of what munmap does to them. */
        if (!(pte & PAGE_SHARED)) pmm_free_frame(pte & ~0xFFFu);
    }
}

/* mmap2(addr, length, prot, flags, fd, pgoffset). Only anonymous private
 * mappings are supported - file-backed mmap needs a page cache, which
 * this kernel does not have. */
static int32_t sys_mmap2(uint32_t addr, uint32_t length, int prot,
                         int flags, int fd, uint32_t pgoff) {
    (void)pgoff;
    if (length == 0) return -EINVAL;
    /* File-backed MAP_PRIVATE is real since Milestone 22, because it is
     * how a dynamic linker maps a shared library - ld-linux.so.2 does
     * essentially nothing else. With no page cache, a private file
     * mapping degenerates to "allocate pages and read the bytes in",
     * which is behaviourally correct for MAP_PRIVATE: the mapping is a
     * private copy, and nothing here ever writes it back.
     *
     * MAP_SHARED is real since Milestone 30, and needed no page cache: a
     * mappable file's bytes live in *page-aligned* storage, so page N of
     * the file is exactly one physical frame and the mapping hands that
     * frame to user space. The file's contents and the mapping are then
     * the same memory rather than two copies to keep in step, which is
     * what makes two processes mapping one file see each other's writes.
     * Wine needs exactly that: wineserver publishes the Windows
     * KUSER_SHARED_DATA page to every client this way. */
    int file_backed = !(flags & MAP_ANONYMOUS) && fd >= 0;
    int shared = file_backed && (flags & MAP_SHARED);
    if (!file_backed && fd >= 0 && !(flags & MAP_ANONYMOUS)) return -ENOSYS;

    posix_proc_t* P = posix_current();
    uint32_t bytes = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t base;

    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
        /* The range has to exist and has to be the program's. Wine's
         * preloader asks for two gigabytes at a time and halves until
         * something fits, so both of these are answers it expects. */
        if (addr + bytes < addr) return -ENOMEM;            /* wrapped */
        if (addr + bytes > USER_SPACE_END) return -ENOMEM;  /* the kernel's */
        /* MAP_FIXED_NOREPLACE differs from MAP_FIXED in exactly one way,
         * and it is the way that matters to a loader: it refuses rather
         * than evicting. A reservation counts as occupied - it is
         * somebody's plan for that address. */
        if (flags & MAP_FIXED_NOREPLACE) {
            for (uint32_t p = 0; p < bytes; p += PAGE_SIZE) {
                if (paging_get_entry(addr + p) & (PAGE_PRESENT | PAGE_RESERVED)) {
                    return -EEXIST;
                }
            }
        }
        /* A fixed mapping is the one request that can name an address the
         * kernel is already using. The first 4MB is identity-mapped and
         * shared into every address space, so a mapping there replaces
         * the kernel's own view of itself - Wine's preloader reserves the
         * DOS area on startup, that range covers the kernel's load
         * address at 0x100000, and the machine hung. Refusing is what
         * Linux effectively does too (those addresses are not the
         * program's to take), and a caller that cannot have the range it
         * asked for is told so rather than being given a machine that
         * stops. See ROADMAP.md Milestone 27. */
        if (paging_region_conflict(addr, addr + bytes)) return -ENOMEM;
        base = addr;
    } else {
        if (P->mmap_next + bytes > MMAP_ARENA_END) return -ENOMEM;
        base = P->mmap_next;
        P->mmap_next += bytes;
    }

    /* PROT_NONE with nothing behind it is a *reservation*, and it costs a
     * page table per 4MB rather than a frame per page. Wine's preloader
     * opens by reserving whatever it can of the low 2GB so that Windows
     * images can be placed there later; committing that would be
     * committing the machine. mprotect() below turns it into memory when
     * the program comes back for it. */
    if (prot == PROT_NONE && !file_backed) {
        for (uint32_t p = 0; p < bytes; p += PAGE_SIZE) {
            uint32_t old = paging_get_entry(base + p);
            if ((old & PAGE_PRESENT) && !(old & PAGE_SHARED)) {
                paging_unmap_page(base + p);
                pmm_free_frame(old & ~0xFFFu);
            }
            if (!paging_reserve_page(base + p)) {
                unmap_range(base, p);
                return -ENOMEM;
            }
        }
        return (int32_t)base;
    }

    uint32_t pte_flags = prot_to_pte(prot);

    if (shared) {
        fd_entry_t* e = fd_get(fd);
        if (!e || e->kind != FD_FILE) return -EBADF;
        uint32_t off = pgoff * PAGE_SIZE;

        /* Give the file page-aligned storage covering the mapping, then
         * hand over the frames that storage is made of. */
        if (!vfs_make_mappable(e->node, off + bytes)) return -ENOMEM;

        for (uint32_t p = 0; p < bytes; p += PAGE_SIZE) {
            uint32_t kva = (uint32_t)e->node->data + off + p;
            uint32_t kpte = paging_get_entry(kva);
            if (!(kpte & PAGE_PRESENT)) { unmap_range(base, p); return -ENOMEM; }

            /* MAP_FIXED over something this process already had: that
             * frame is going out of use here and now, so give it back
             * rather than leaking it. */
            uint32_t old = paging_get_entry(base + p);
            if ((old & PAGE_PRESENT) && !(old & PAGE_SHARED)) {
                pmm_free_frame(old & ~0xFFFu);
            }
            paging_map_page(base + p, kpte & ~0xFFFu,
                            pte_flags | PAGE_SHARED);
        }

        /* The mapping keeps the file alive. Wine closes the descriptor
         * as soon as the mapping exists - and wineserver unlinked the
         * file before that - so without this the node would be freed
         * while its frames were still mapped into two processes. */
        posix_pin_mapped(e->node);
        return (int32_t)base;
    }

    if (!map_range(base, bytes, pte_flags)) {
        unmap_range(base, bytes);
        return -ENOMEM;
    }

    if (!file_backed) {
        /* Anonymous memory reads as zero - *always*, including when it
         * lands on pages that were already mapped. map_range() only zeroes
         * pages it newly allocates, which is not enough here: a dynamic
         * linker maps a library's .bss with MAP_FIXED|MAP_ANONYMOUS
         * directly over pages the file mapping already populated, and
         * without this those pages keep the file's bytes.
         *
         * The symptom was a program that ran perfectly and then faulted on
         * exit reading a garbage pointer - libc's exit-handler list lives
         * in .bss, and it held whatever happened to be at that file
         * offset instead of NULL. */
        kmemset((void*)base, 0, bytes);
    }

    if (file_backed) {
        fd_entry_t* e = fd_get(fd);
        if (!e || e->kind != FD_FILE) {
            unmap_range(base, bytes);
            return -EBADF;
        }
        /* pgoff is in pages, as mmap2's name promises - the whole reason
         * mmap2 exists is to express a file offset beyond 4GB/4096 in a
         * 32-bit argument. */
        uint32_t off = pgoff * PAGE_SIZE;
        uint32_t avail = (off < e->node->length) ? e->node->length - off : 0;
        uint32_t want = length < avail ? length : avail;
        if (want) vfs_read(e->node, off, want, (uint8_t*)base);
        /* Anything past the end of the file stays zero, which is what the
         * bss tail of a shared library's data segment needs. */
    }
    return (int32_t)base;
}

static int32_t sys_munmap(uint32_t addr, uint32_t length) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (length == 0) return -EINVAL;
    unmap_range(addr, (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    return 0;
}

static int32_t sys_mprotect(uint32_t addr, uint32_t length, int prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    uint32_t bytes = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t flags = prot_to_pte(prot);

    for (uint32_t va = addr; va < addr + bytes; va += PAGE_SIZE) {
        uint32_t pte = paging_get_entry(va);

        if (!(pte & PAGE_PRESENT)) {
            /* A reservation being cashed in. This is the other half of
             * PROT_NONE costing nothing: the address was kept for the
             * program, and now the program wants the memory. Anything
             * that was never reserved at all is still -ENOMEM. */
            if (!(pte & PAGE_RESERVED)) return -ENOMEM;
            if (prot == PROT_NONE) continue;      /* already exactly that */
            uint32_t frame = pmm_alloc_frame();
            if (!frame) return -ENOMEM;
            paging_map_page(va, frame, flags);
            kmemset((void*)va, 0, PAGE_SIZE);     /* it reads as zero */
            continue;
        }

        /* Keep the frame, change the permission bits - but not the bit
         * that says the frame is somebody else's. PROT_NONE is a
         * permission here like any other (see prot_to_pte): the page
         * stays mapped and stops being the program's, so the bytes are
         * still there when it asks for them back. */
        paging_map_page(va, pte & ~0xFFFu, flags | (pte & PAGE_SHARED));
    }
    return 0;
}

/* brk(0) reports the break; brk(addr) moves it. Growing maps pages,
 * shrinking unmaps them. */
static int32_t sys_brk(uint32_t addr) {
    posix_proc_t* P = posix_current();
    if (addr == 0) return (int32_t)P->brk_current;
    if (addr < BRK_ARENA_START || addr > BRK_ARENA_END) {
        return (int32_t)P->brk_current; /* Linux returns the unchanged break */
    }

    uint32_t want = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (want > P->brk_mapped) {
        if (!map_range(P->brk_mapped, want - P->brk_mapped,
                       PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            return (int32_t)P->brk_current;
        }
        P->brk_mapped = want;
    } else if (want < P->brk_mapped) {
        unmap_range(want, P->brk_mapped - want);
        P->brk_mapped = want;
    }
    P->brk_current = addr;
    return (int32_t)P->brk_current;
}

/* --- files -------------------------------------------------------------- */

/* Path resolution is real since Milestone 26 - every component of
 * "/tmp/a/b" has to exist and be a directory.
 *
 * The one deliberate exception is the basename fallback, and it is what
 * keeps dynamic linking working. ld-linux.so.2 searches a list of
 * directories, so it probes "/lib/i386-linux-gnu/libc.so.6",
 * "/usr/lib/libc.so.6" and several more; the initrd's contents are all at
 * the root, and none of those directories exist. So a path whose
 * *directory part does not resolve* falls back to matching the last
 * component at the root.
 *
 * The condition matters. If the directory does resolve, a miss inside it
 * is a genuine ENOENT - otherwise unlinking /tmp/x/hello.txt and then
 * opening it would find the root's hello.txt and report success, which
 * is exactly the kind of quiet wrongness this fallback could have
 * introduced. */
/* A relative path is resolved against the process's working directory,
 * which is real since Milestone 29 - Wine builds its prefix path out of
 * getcwd(), and a kernel that answers "/" to every process makes two
 * processes disagree about where they are.
 *
 * The joined result lives in a static buffer, which is safe for exactly
 * the reason the rest of this file's user-pointer dereferencing is: a
 * syscall runs to completion in the address space that made it, and the
 * kernel does not re-enter itself. */
#define PATH_JOIN_MAX (POSIX_PATH_MAX * 2)
static char path_buf[PATH_JOIN_MAX];

static const char* abs_into(const char* path, char* out, uint32_t size) {
    if (!path) return 0;
    if (path[0] == '/') return path;

    posix_proc_t* p = posix_current();
    uint32_t n = 0;
    for (const char* c = p->cwd; *c && n < size - 2; c++) out[n++] = *c;
    if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    /* "." and "./x" are the two forms a program actually writes; anything
     * else with a dot component is left alone, because this filesystem has
     * no ".." to resolve against and pretending otherwise would be worse
     * than not trying. */
    if (path[0] == '.' && path[1] == '/') path += 2;
    else if (path[0] == '.' && path[1] == '\0') path += 1;
    for (const char* c = path; *c && n < size - 1; c++) out[n++] = *c;
    /* "/x/" and "/x" name the same thing, and lookup wants the second
     * form. A bare "/" keeps its slash. */
    while (n > 1 && out[n - 1] == '/') n--;
    out[n] = '\0';
    return out;
}

static const char* absolute(const char* path) {
    return abs_into(path, path_buf, sizeof(path_buf));
}

const char* posix_resolve_path(const char* path, char* buf, uint32_t size) {
    return abs_into(path, buf, size);
}

static vfs_node_t* resolve_for_read(const char* path) {
    path = absolute(path);
    if (!path) return 0;

    vfs_node_t* n = vfs_lookup(path);
    if (n) return n;

    const char* leaf = 0;
    if (vfs_resolve_parent(path, &leaf)) return 0;  /* the directory exists */

    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
    if (!*base) return 0;
    return vfs_root ? vfs_finddir(vfs_root, base) : 0;
}

#define O_CLOEXEC 0x80000

static int32_t sys_open(const char* path, int flags, uint32_t mode) {
    if (!path) return -EFAULT;

    posix_proc_t* P = posix_current();
    int want_write = (flags & (O_WRONLY | O_RDWR)) != 0;
    char joined[PATH_JOIN_MAX];
    path = abs_into(path, joined, sizeof(joined));
    vfs_node_t* node = resolve_for_read(path);

    if (!node) {
        if (!(flags & O_CREAT)) return -ENOENT;

        const char* leaf = 0;
        vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
        if (!dir) return -ENOENT;
        node = vfs_create(dir, leaf, VFS_FILE);
        if (!node) return -ENOSPC;
        node->mode = mode & 07777;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }

    if (node->flags & VFS_DIRECTORY) {
        /* Opening a directory read-only is legal and is how getdents64
         * gets a descriptor; opening one for writing is not. */
        if (want_write || (flags & O_CREAT)) return -EISDIR;
    } else if (flags & O_TRUNC) {
        if (want_write) vfs_truncate(node, 0);
    }

    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;

    P->fds[fd].kind = FD_FILE;
    P->fds[fd].node = node;
    /* A descriptor keeps the file alive even after its name is gone. */
    vfs_node_ref(node);
    P->fds[fd].offset = (want_write && (flags & O_APPEND)) ? node->length : 0;
    P->fds[fd].writable = want_write;
    P->fds[fd].append = (flags & O_APPEND) != 0;
    P->fds[fd].dir_index = 0;
    P->fds[fd].cloexec = (flags & O_CLOEXEC) != 0;
    return fd;
}

static int32_t sys_close(int fd) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (fd < 3) return 0; /* closing a standard stream is a no-op here */
    if (e->kind == FD_SOCKET) socket_close(e->sock);
    if (e->kind == FD_FILE) vfs_node_unref(e->node);
    e->kind = FD_FREE;
    e->node = 0;
    e->sock = 0;
    return 0;
}

/* --- descriptor duplication (Milestone 29) -------------------------------
 *
 * dup2 is how a program wires a pipe onto a child's stdout before exec,
 * and Wine's loader does exactly that. It is also the first thing in this
 * kernel to make two descriptors name one object, which is why sockets
 * grew a reference count in the same milestone. */
static int32_t fd_dup_to(int oldfd, int newfd, int cloexec) {
    posix_proc_t* P = posix_current();
    fd_entry_t* e = fd_get(oldfd);
    if (!e) return -EBADF;
    if (newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (newfd == oldfd) return newfd;

    if (P->fds[newfd].kind != FD_FREE) sys_close(newfd);
    P->fds[newfd] = *e;
    P->fds[newfd].cloexec = cloexec;
    if (e->kind == FD_SOCKET) socket_ref(e->sock);
    if (e->kind == FD_FILE) vfs_node_ref(e->node);
    return newfd;
}

static int32_t sys_dup(int oldfd) {
    if (!fd_get(oldfd)) return -EBADF;
    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;
    return fd_dup_to(oldfd, fd, 0);
}

/* --- pipes ---------------------------------------------------------------
 *
 * Two descriptors onto one connected pair; see socket_pipe(). O_CLOEXEC
 * is the only flag pipe2 is asked for in practice and it is honoured,
 * because the whole point of a pipe across an exec is that exactly one
 * end survives it. */
static int32_t sys_pipe(int32_t* fdv, int flags) {
    posix_proc_t* P = posix_current();
    if (!fdv) return -EFAULT;

    socket_t *r = 0, *w = 0;
    int32_t e = socket_pipe(&r, &w);
    if (e < 0) return e;

    int rfd = posix_fd_install_socket(r);
    int wfd = rfd >= 0 ? posix_fd_install_socket(w) : -EMFILE;
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) sys_close(rfd);
        else socket_close(r);
        socket_close(w);
        return -EMFILE;
    }

    P->fds[rfd].writable = 0;
    P->fds[rfd].cloexec = (flags & O_CLOEXEC) != 0;
    P->fds[wfd].cloexec = (flags & O_CLOEXEC) != 0;
    fdv[0] = rfd;
    fdv[1] = wfd;
    return 0;
}

/* access(path, mode). Everything present is now readable, writable and
 * executable - the filesystem has no permissions, and claiming otherwise
 * would make W_OK lie in the direction that breaks programs. The dynamic
 * linker probes with this before it opens, so answering it is what lets
 * it find a library at all. */
static int32_t sys_access(const char* path, int mode) {
    (void)mode;
    if (!path) return -EFAULT;
    return resolve_for_read(path) ? 0 : -ENOENT;
}

static int32_t sys_read(int fd, char* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (count == 0) return 0;

    if (e->kind == FD_CONSOLE) {
        /* Only fd 0 reads, and only a line at a time - the console has no
         * character-at-a-time path. */
        if (fd != 0) return -EBADF;
        return -ENOSYS;
    }
    if (e->kind == FD_SOCKET) return socket_read(e->sock, buf, count, cur_regs);

    if (e->offset >= e->node->length) return 0; /* EOF */
    uint32_t left = e->node->length - e->offset;
    if (count > left) count = left;
    uint32_t got = vfs_read(e->node, e->offset, count, (uint8_t*)buf);
    e->offset += got;
    return (int32_t)got;
}

static int32_t sys_write(int fd, const char* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (e->kind == FD_SOCKET) return socket_write(e->sock, buf, count);
    if (e->kind != FD_CONSOLE) {
        if (!e->writable) return -EBADF;
        /* O_APPEND is not "seek to the end when you open it" - it is
         * "every write goes to the end", re-evaluated each time, which is
         * what makes two descriptors appending to one file not overwrite
         * each other. */
        if (e->append) e->offset = e->node->length;
        int32_t n = vfs_write(e->node, e->offset, count, (const uint8_t*)buf);
        if (n > 0) e->offset += (uint32_t)n;
        return n;
    }

    /* stderr in red, so a program's diagnostics are distinguishable from
     * its output the way they are on a real terminal. terminal_write()
     * takes a length, which matters here: a Linux write() is counted
     * bytes, not a NUL-terminated string, and may legitimately contain
     * embedded NULs or no terminator at all. */
    if (fd == 2) {
        uint8_t saved = terminal_getcolor();
        terminal_setcolor(VGA_COLOR_LIGHT_RED);
        terminal_write(buf, count);
        terminal_setcolor(saved);
    } else {
        terminal_write(buf, count);
    }
    return (int32_t)count;
}

/* writev(fd, iov, iovcnt) - glibc's printf reaches for this before
 * write(), so a program built against a real libc needs it. */
typedef struct {
    uint32_t iov_base;
    uint32_t iov_len;
} iovec_t;

static int32_t sys_writev(int fd, const iovec_t* iov, int iovcnt) {
    if (!iov || iovcnt < 0) return -EFAULT;
    int32_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int32_t n = sys_write(fd, (const char*)iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total ? total : n;
        total += n;
    }
    return total;
}

static int32_t sys_lseek(int fd, int32_t offset, int whence) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind != FD_FILE) return -ESPIPE;

    int32_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int32_t)e->offset; break;
        case SEEK_END: base = (int32_t)e->node->length; break;
        default: return -EINVAL;
    }
    int32_t target = base + offset;
    if (target < 0) return -EINVAL;
    e->offset = (uint32_t)target;
    return target;
}

/* struct stat64's layout is fixed by the ABI. Only the fields a program
 * actually reads are filled in; the rest stay zero. */
/* i386's struct stat64, by byte offset. Only the fields something
 * actually reads are filled in, but which ones those are is not
 * obvious - see st_ino below. */
#define ST_DEV_OFFSET    0   /* st_dev, 64-bit */
#define ST_INO32_OFFSET 12   /* __st_ino, the 32-bit legacy field */
#define ST_MODE_OFFSET  16
#define ST_NLINK_OFFSET 20
#define ST_SIZE_OFFSET  44   /* st_size, 64-bit */
#define ST_BLKSIZE_OFFSET 52
#define ST_BLOCKS_OFFSET  56 /* 64-bit, in 512-byte units */
#define ST_INO_OFFSET   88   /* st_ino, the real 64-bit field */
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFSOCK 0140000

/* The device every initrd file claims to be on. Any non-zero value does;
 * it only has to be consistent and not collide with 0. */
#define NOVARIS_INITRD_DEV 0x0701u

/* `ino` is not decoration, and getting it wrong cost most of Milestone
 * 22. glibc's _dl_map_object_from_fd, having opened a shared library,
 * stats it and walks the list of already-loaded objects looking for one
 * with the same st_ino and st_dev - that is how a library named two
 * different ways gets loaded once rather than twice. With every file
 * reporting st_ino = 0, libc.so.6 "matched" an object already in the
 * list, so the linker closed the file and returned that map instead of
 * loading libc at all.
 *
 * The symptom was nothing like the cause: five copies of "no version
 * information available", then "undefined symbol: __libc_start_main".
 * The syscall trace is what found it - the host maps libc with five
 * mmap2 calls after the stat, and Novaris went straight from stat to
 * close. */
static int32_t stat_common(uint8_t* st, uint32_t size, uint32_t mode,
                           uint32_t ino) {
    if (!st) return -EFAULT;
    kmemset(st, 0, 96);
    *(uint32_t*)(st + ST_DEV_OFFSET) = NOVARIS_INITRD_DEV;
    *(uint32_t*)(st + ST_INO32_OFFSET) = ino;
    /* `mode` arrives with both halves already in it: the file type from
     * the caller and the permission bits the node was created with. The
     * permissions are recorded rather than enforced - there is nobody to
     * enforce them against - but they are the program's own numbers, and
     * a program that checks them (wineserver checks that its directory is
     * private) is entitled to see what it asked for. */
    *(uint32_t*)(st + ST_MODE_OFFSET) = mode;
    *(uint32_t*)(st + ST_NLINK_OFFSET) = 1;
    *(uint64_t*)(st + ST_SIZE_OFFSET) = size;
    *(uint32_t*)(st + ST_BLKSIZE_OFFSET) = PAGE_SIZE;
    *(uint64_t*)(st + ST_BLOCKS_OFFSET) = (size + 511u) / 512u;
    *(uint64_t*)(st + ST_INO_OFFSET) = ino;
    return 0;
}

/* A stable, unique, non-zero inode per file. The VFS has no inode of its
 * own, so the node's address stands in: distinct per file, constant for
 * the life of the mount, and never 0. */
static uint32_t node_ino(const vfs_node_t* node) {
    uint32_t v = (uint32_t)node;
    return v ? v : 1u;
}

static int32_t sys_fstat64(int fd, uint8_t* st) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind == FD_CONSOLE) {
        /* The three console descriptors are one character device, and
         * distinct from every file. */
        return stat_common(st, 0, S_IFCHR | 0620, 1u + (uint32_t)fd);
    }
    uint32_t mode = (e->node->flags & VFS_DIRECTORY) ? S_IFDIR
                  : (e->node->flags & VFS_SOCKET)    ? S_IFSOCK
                                                     : S_IFREG;
    return stat_common(st, e->node->length, mode | (e->node->mode & 07777),
                       node_ino(e->node));
}

static int32_t sys_stat64(const char* path, uint8_t* st) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    int32_t r = sys_fstat64(fd, st);
    sys_close(fd);
    return r;
}

/* getcwd. Real since Milestone 29: the working directory is per process,
 * survives fork and exec, and is what a relative path resolves against.
 * Linux returns the length including the NUL and writes the string into
 * the buffer. */
static int32_t sys_getcwd(char* buf, uint32_t size) {
    posix_proc_t* P = posix_current();
    if (!buf) return -EFAULT;
    uint32_t n = kstrlen(P->cwd) + 1;
    if (size < n) return -ERANGE;
    kmemcpy(buf, P->cwd, n);
    return (int32_t)n;
}

static int32_t sys_chdir(const char* path) {
    posix_proc_t* P = posix_current();
    if (!path) return -EFAULT;
    char joined[PATH_JOIN_MAX];
    const char* abs = abs_into(path, joined, sizeof(joined));

    vfs_node_t* n = vfs_lookup(abs);
    if (!n) return -ENOENT;
    if (!(n->flags & VFS_DIRECTORY)) return -ENOTDIR;
    kstrlcpy(P->cwd, abs, sizeof(P->cwd));
    return 0;
}

/* readlink. There are no symlinks, so the only link that resolves is the
 * one every Unix program expects to find: /proc/self/exe. Wine reads it
 * to work out where it was installed, and answering -EINVAL ("not a
 * symlink") for everything else is what Linux does for a real file. */
static int path_is(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int32_t sys_readlink(const char* path, char* buf, uint32_t size) {
    if (!path || !buf) return -EFAULT;
    int is_self_exe =
        path_is(path, "/proc/self/exe") || path_is(path, "/proc/curproc/file");
    if (!is_self_exe) {
        return resolve_for_read(path) ? -EINVAL : -ENOENT;
    }
    const char* exe = posix_current()->exe_path;
    uint32_t n = kstrlen(exe);
    if (n > size) n = size;
    kmemcpy(buf, exe, n);
    return (int32_t)n;   /* not NUL-terminated, exactly as Linux leaves it */
}

/* --- the writable half (Milestone 26) -----------------------------------
 *
 * All of these are thin: kernel/ramfs.c owns the tree and the errno
 * values, and these only split a path and hand it over. The *at variants
 * ignore their dirfd, because there is no per-process working directory
 * yet and every path here is resolved from the root - which is honest for
 * AT_FDCWD, the only value glibc passes. */

static int32_t sys_unlink(const char* path, int want_dir) {
    if (!path) return -EFAULT;
    char joined[PATH_JOIN_MAX];
    path = abs_into(path, joined, sizeof(joined));
    const char* leaf = 0;
    vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
    if (!dir) return -ENOENT;
    int32_t r = vfs_unlink(dir, leaf, want_dir);
    /* Removing a socket's file removes the binding with it. */
    if (r == 0) socket_unbind_path(path);
    return r;
}

static int32_t sys_mkdir(const char* path, uint32_t mode) {
    if (!path) return -EFAULT;
    char joined[PATH_JOIN_MAX];
    path = abs_into(path, joined, sizeof(joined));
    const char* leaf = 0;
    vfs_node_t* dir = vfs_resolve_parent(path, &leaf);
    if (!dir) return -ENOENT;
    if (vfs_finddir(dir, leaf)) return -EEXIST;
    vfs_node_t* n = vfs_create(dir, leaf, VFS_DIRECTORY);
    if (!n) return -ENOSPC;
    n->mode = mode & 07777;
    return 0;
}

/* chmod. Recorded, not enforced, for the same reason the mode is: a
 * program that sets a mode and reads it back must see what it set. */
static int32_t sys_chmod(const char* path, uint32_t mode) {
    vfs_node_t* n = resolve_for_read(path);
    if (!n) return -ENOENT;
    n->mode = mode & 07777;
    return 0;
}

static int32_t sys_rename(const char* from, const char* to) {
    if (!from || !to) return -EFAULT;
    /* Two joined paths at once, so two buffers - the shared one would
     * have the second call overwrite the first. */
    char a[PATH_JOIN_MAX], b[PATH_JOIN_MAX];
    return vfs_rename(abs_into(from, a, sizeof(a)), abs_into(to, b, sizeof(b)));
}

static int32_t sys_ftruncate(int fd, uint32_t length) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (e->kind == FD_CONSOLE) return -EINVAL;
    if (!e->writable) return -EBADF;
    return vfs_truncate(e->node, length);
}

/* getdents64. The struct is Linux's:
 *
 *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[];
 *
 * so d_name starts at offset 19 and each record is padded to a multiple
 * of 8. "." and ".." are synthesised - they are not nodes in the tree,
 * but every Linux directory has them and a program that sorts its
 * readdir output would otherwise disagree between the two systems. */
#define DT_DIR 4
#define DT_REG 8

static int32_t sys_getdents64(int fd, uint8_t* buf, uint32_t count) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;
    if (!buf) return -EFAULT;
    if (e->kind == FD_CONSOLE || !(e->node->flags & VFS_DIRECTORY)) {
        return -ENOTDIR;
    }

    uint32_t written = 0;
    for (;;) {
        const char* name;
        vfs_node_t* target;

        if (e->dir_index == 0) {
            name = ".";
            target = e->node;
        } else if (e->dir_index == 1) {
            name = "..";
            target = e->node->parent ? e->node->parent : e->node;
        } else {
            target = vfs_readdir(e->node, e->dir_index - 2);
            if (!target) break;
            name = target->name;
        }

        uint32_t namelen = kstrlen(name);
        uint32_t reclen = (19 + namelen + 1 + 7u) & ~7u;
        if (written + reclen > count) {
            /* Not even one record fits: Linux says EINVAL rather than
             * silently returning nothing, so a caller with a too-small
             * buffer finds out. */
            if (written == 0) return -EINVAL;
            break;
        }

        uint8_t* rec = buf + written;
        kmemset(rec, 0, reclen);
        *(uint64_t*)(rec + 0)  = node_ino(target);
        *(uint64_t*)(rec + 8)  = (int64_t)(e->dir_index + 1);
        *(uint16_t*)(rec + 16) = (uint16_t)reclen;
        rec[18] = (target->flags & VFS_DIRECTORY) ? DT_DIR : DT_REG;
        kmemcpy(rec + 19, name, namelen);

        written += reclen;
        e->dir_index++;
    }
    return (int32_t)written;
}

/* --- two processes at once (Milestone 29) --------------------------------
 *
 * Wine's start_server() is fork(), then execve(wineserver), then
 * waitpid() - three calls, and until now none of them could exist,
 * because every piece of POSIX state in this kernel was a global and two
 * processes would have shared it. include/posix_proc.h is what changed;
 * these are what it was for.
 *
 * There is no copy-on-write here, and there is no fault machinery to
 * build it on: a page fault in this kernel means a signal or a dead
 * program, not a mapping to be filled in. So fork copies every page the
 * parent has, eagerly. That is the honest cost of the design, and it is
 * why a process's memory is released the moment it exits rather than at
 * the end of the batch - a fork/exec pair holds two copies for exactly as
 * long as it takes the child to exec. */

/* Wakes a parent suspended in vfork. Called when the child execs into a
 * different image and when it exits - the two ways a vfork child stops
 * being the thing its parent is waiting on. The result written into the
 * parent's frame is the child's pid, which is what clone returns. */
static void vfork_release(posix_proc_t* child) {
    if (!child || !child->vfork_parent) return;
    child->vfork_parent = 0;
    scheduler_wake_on((uint32_t)child, 1, child->pid);
}

/* fork, and the two shapes of clone that are really fork.
 *
 * `child_stack` is 0 for a plain fork - the child resumes on its own copy
 * of the parent's stack - and an address for a clone that supplied one,
 * which is how glibc's posix_spawn hands its child a scratch stack.
 *
 * `vfork` suspends the caller until the child execs or exits. Linux does
 * that by sharing the address space, so the child's writes are the
 * parent's; Novaris copies instead, and the honest consequence is written
 * down in ROADMAP.md - a vfork child here can report back through its exit
 * status but not through memory. What it preserves is the part that
 * matters to a spawn: the parent does not resume, and therefore does not
 * free the stack the child is standing on, until the child is gone. */
static int32_t fork_common(registers_t* regs, uint32_t child_stack, int vfork) {
    posix_proc_t* parent = posix_current();

    /* fork needs somewhere to put the child, and the older
     * "one program, blocking the shell" path in process.c has no queue to
     * put it in. Every ELF program has run as a scheduler task since
     * Milestone 20, so this is a real restriction only for flat binaries,
     * which have no libc to call fork from. */
    if (!scheduler_is_active()) return -ENOSYS;

    uint32_t child_pd = paging_create_address_space();
    if (!child_pd) return -ENOMEM;

    if (!paging_copy_user_space(child_pd)) {
        paging_destroy_address_space(child_pd);
        return -ENOMEM;
    }

    posix_proc_t* child = posix_proc_clone(parent, child_pd);
    if (!child) {
        paging_destroy_address_space(child_pd);
        return -EAGAIN;   /* what Linux says when the table is full */
    }

    int tid = scheduler_fork_current(regs, child_pd, vfork ? "vfork" : "fork");
    if (tid < 0) {
        posix_proc_reap(child);
        paging_destroy_address_space(child_pd);
        return -EAGAIN;
    }

    /* A clone that supplied a stack runs the child there instead of on
     * its copy of the parent's. The copy is still made - the child needs
     * everything else the parent had - but its first instruction runs on
     * the stack it was given. */
    if (child_stack) scheduler_fork_set_stack(tid, child_stack);

    if (vfork) {
        child->vfork_parent = parent->pid;
        /* Park on the child's structure until it execs or exits. The
         * result written into this frame when that happens is the child's
         * pid, which is the same value returned below - so a caller that
         * could not be blocked (nothing else runnable, which cannot
         * happen here since the child is ready) gets the same answer. */
        scheduler_block_current(regs, (uint32_t)child, 0);
    }
    return child->pid;
}

static int32_t sys_fork(registers_t* regs) {
    return fork_common(regs, 0, 0);
}

/* execve's arguments live in the address space that is about to be
 * destroyed, so they have to be copied out of it first - into the kernel
 * heap, which is the same memory at the same address on both sides of the
 * change. Getting this wrong is not subtle: the strings would be read
 * back out of pages that had been handed to the physical allocator. */
#define EXEC_MAX_ARGS 32
#define EXEC_ARG_BYTES 4096

typedef struct {
    char  store[EXEC_ARG_BYTES];
    const char* v[EXEC_MAX_ARGS];
    int   n;
} exec_vec_t;

static void exec_vec_copy(exec_vec_t* out, uint32_t user_vec) {
    out->n = 0;
    uint32_t w = 0;
    if (!user_vec) return;

    const char* const* v = (const char* const*)user_vec;
    for (int i = 0; i < EXEC_MAX_ARGS && v[i]; i++) {
        const char* s = v[i];
        if (w >= EXEC_ARG_BYTES - 1) break;
        out->v[out->n++] = &out->store[w];
        while (*s && w < EXEC_ARG_BYTES - 1) out->store[w++] = *s++;
        out->store[w++] = '\0';
    }
}

static int32_t sys_execve(const char* path, uint32_t uargv, uint32_t uenvp,
                          registers_t* regs) {
    posix_proc_t* P = posix_current();
    if (!path) return -EFAULT;

    /* Everything that has to survive the address space comes out of it
     * now: the path, the arguments, the environment, and the image
     * itself. After this the user half is gone. */
    char joined[PATH_JOIN_MAX];
    char exec_path[POSIX_PATH_MAX];
    /* A program that wants to re-execute itself has no other way to name
     * itself: it was started by a path it never saw. Linux answers this
     * one, so this does too - and it is the only symlink either kernel has
     * here. */
    if (path_is(path, "/proc/self/exe")) {
        kstrlcpy(exec_path, P->exe_path, sizeof(exec_path));
    } else {
        kstrlcpy(exec_path, abs_into(path, joined, sizeof(joined)),
                 sizeof(exec_path));
    }

    vfs_node_t* node = resolve_for_read(exec_path);
    if (!node) return -ENOENT;
    if (node->flags & VFS_DIRECTORY) return -EACCES;

    static exec_vec_t argv_copy, envp_copy;   /* too big for a kernel stack */
    exec_vec_copy(&argv_copy, uargv);
    exec_vec_copy(&envp_copy, uenvp);
    if (argv_copy.n == 0) {
        argv_copy.v[0] = exec_path;
        argv_copy.n = 1;
    }

    /* The image is read *in place* where the filesystem already holds it,
     * rather than copied into the kernel heap first. That is not a
     * micro-optimisation: wineserver is four megabytes, and a copy of it
     * has to be found on a heap that is also holding everything the
     * process being replaced allocated - the first attempt at this failed
     * with -ENOMEM at exactly that point, and Wine reported "could not
     * exec wineserver". An initrd file's bytes are already in memory and
     * identity-mapped into every address space, so there is nothing to
     * copy. A file the program wrote itself lives on the kernel heap,
     * which is equally reachable.
     *
     * Both survive the teardown below for the same reason: neither is in
     * the half of the address space that is about to be emptied. */
    uint8_t* image = node->data;
    uint32_t size = node->length;
    uint8_t* owned = 0;
    if (!image) {
        owned = (uint8_t*)kmalloc(size ? size : 1);
        if (!owned) return -ENOMEM;
        size = vfs_read(node, 0, size, owned);
        image = owned;
    }

    if (!elf_is_valid(image, size)) {
        if (owned) kfree(owned);
        return -ENOEXEC;
    }

    /* The point of no return. From here a failure cannot be reported to
     * the caller - there is no caller left to report it to - so
     * everything that could fail has been done above. */
    paging_release_user_pages();
    scheduler_exec_current();

    uint32_t entry = 0, esp = 0;
    int ok = process_load_elf_image(image, size, argv_copy.n, argv_copy.v,
                                    envp_copy.n, envp_copy.v, &entry, &esp);
    if (owned) kfree(owned);
    if (!ok) {
        /* An ELF that passed validation and then would not load leaves a
         * process with no image at all. Killing it is the only honest
         * outcome, and it is what Linux does too. */
        posix_exit_status(127);
        return 0;
    }

    /* Descriptors survive an exec, which is the whole reason a shell can
     * redirect a child's output - except the ones the program asked to be
     * closed. */
    for (int i = 0; i < MAX_FDS; i++) {
        if (P->fds[i].kind != FD_FREE && P->fds[i].cloexec) sys_close(i);
    }

    /* Handlers do not: their addresses belonged to an image that no
     * longer exists. The *mask* does survive, which is Linux's rule and
     * matters to a program that blocked a signal before exec'ing. */
    uint32_t keep_mask = P->blocked;
    posix_signal_reset();
    P->blocked = keep_mask;

    P->mmap_next = MMAP_ARENA_START;
    P->brk_current = BRK_ARENA_START;
    P->brk_mapped = BRK_ARENA_START;
    posix_set_exe_path(exec_path);

    /* This process has become something else, which is the other half of
     * what a vfork parent is waiting for. */
    vfork_release(P);

    /* And the trap frame *is* the new program's initial state: rewriting
     * it and returning is the whole of "start executing something else".
     * The general registers are cleared rather than inherited, because a
     * fresh process on i386 is entered with them undefined and glibc's
     * _start assumes as much (it zeroes ebp itself and reads argc off the
     * stack). */
    regs->eip = entry;
    regs->useresp = esp;
    regs->eax = 0; regs->ebx = 0; regs->ecx = 0; regs->edx = 0;
    regs->esi = 0; regs->edi = 0; regs->ebp = 0;
    regs->ds = regs->es = regs->fs = regs->gs = 0x23;
    regs->cs = 0x1B;
    regs->ss = 0x23;
    regs->eflags = 0x202;
    return 0;
}

/* --- wait4 ---------------------------------------------------------------
 *
 * The status word is Linux's encoding, and a program reads it through
 * WIFEXITED/WEXITSTATUS rather than directly: a normal exit is the low
 * byte of the code shifted up eight, and death by a signal is the signal
 * number in the low seven bits. Both are produced here because a program
 * that forks is entitled to know which happened. */
#define WNOHANG 1

static int32_t sys_wait4(int pid, int32_t* status, int options,
                         registers_t* regs) {
    posix_proc_t* P = posix_current();

    int have_child = 0;
    for (int i = 0; ; i++) {
        posix_proc_t* c = posix_proc_at(i);
        if (!c) {
            if (i >= POSIX_MAX_PROCS) break;
            continue;
        }
        if (c->ppid != P->pid) continue;
        if (pid > 0 && c->pid != pid) continue;
        have_child = 1;
        if (!c->exited) continue;

        int collected = c->pid;
        if (status) *status = c->exit_status;
        posix_proc_reap(c);
        return collected;
    }

    if (!have_child) return -ECHILD;
    if (options & WNOHANG) return 0;

    /* Park on this process's own structure. A child that exits wakes
     * exactly its parent, by that address - see process_teardown(). */
    posix_request_block((uint32_t)P, regs);
    return 0;   /* not used: the call is going to happen again */
}

/* --- poll and select -----------------------------------------------------
 *
 * Wine's server loop waits on every descriptor it holds at once rather
 * than blocking on one, so a socket layer without this is a socket layer
 * wineserver cannot use. Milestone 28 said so and this is the answer.
 *
 * Waiting is the interesting part, and it is a compromise stated openly.
 * The blocking path can park a task on exactly one address, and poll is
 * by definition interested in several - so a waiting poll registers on
 * the first socket in its set *and* takes a one-tick deadline. The first
 * gives an immediate wake for the common single-descriptor case; the
 * second means nothing in the set can go unnoticed for longer than 10ms.
 * A wake-on-any-of-N would need a wait queue per object, which is a
 * bigger change than this milestone is. */

static uint32_t fd_ready_mask(int fd) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return POLLNVAL;
    switch (e->kind) {
        case FD_CONSOLE:
            /* stdout and stderr are always writable; there is no
             * character-at-a-time path into the console, so stdin never
             * reports readable rather than reporting a read that would
             * then fail. */
            return (fd == 0) ? 0u : (uint32_t)POLLOUT;
        case FD_FILE:
            /* A regular file is always ready, both ways - which is what
             * Linux reports, because a read from one never blocks. */
            return POLLIN | POLLOUT;
        case FD_SOCKET:
            return socket_poll_mask(e->sock);
        default:
            return POLLNVAL;
    }
}

/* The deadline a waiting poll is working to, remembered across the
 * retries that implement the wait - a relative timeout recomputed every
 * retry would never expire. 0 means "no timeout given". */
static uint32_t poll_deadline_for(posix_proc_t* P, uint32_t key,
                                  int timeout_ms, int* expired) {
    *expired = 0;
    if (timeout_ms < 0) return 0;              /* wait forever */
    if (timeout_ms == 0) { *expired = 1; return 0; }

    if (P->poll_key != key || !P->poll_deadline) {
        uint32_t ticks = ((uint32_t)timeout_ms + 9u) / 10u;   /* 100Hz PIT */
        P->poll_key = key;
        P->poll_deadline = pit_get_ticks() + (ticks ? ticks : 1u);
        if (!P->poll_deadline) P->poll_deadline = 1;
    }
    if ((int32_t)(pit_get_ticks() - P->poll_deadline) >= 0) {
        P->poll_key = 0;
        P->poll_deadline = 0;
        *expired = 1;
        return 0;
    }
    return P->poll_deadline;
}

/* One tick from now, so a poll waiting on several descriptors re-tests
 * them promptly even though it could only register on one. */
static uint32_t poll_next_wake(uint32_t deadline) {
    uint32_t tick = pit_get_ticks() + 1u;
    if (!tick) tick = 1u;
    if (deadline && (int32_t)(deadline - tick) < 0) return deadline;
    return tick;
}

static int32_t sys_poll(k_pollfd_t* fds_u, uint32_t nfds, int timeout_ms,
                        registers_t* regs) {
    posix_proc_t* P = posix_current();
    if (nfds && !fds_u) return -EFAULT;
    if (nfds > MAX_FDS * 2) return -EINVAL;

    int ready = 0;
    uint32_t first_sock = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        if (fds_u[i].fd < 0) { fds_u[i].revents = 0; continue; }
        uint32_t want = (uint32_t)(uint16_t)fds_u[i].events;
        uint32_t got = fd_ready_mask(fds_u[i].fd);
        /* POLLERR, POLLHUP and POLLNVAL are always reported, whether or
         * not the caller asked for them - Linux's rule, and the reason a
         * server loop can find out a client vanished without having
         * subscribed to the possibility. */
        uint32_t rev = (got & (want | POLLERR | POLLHUP | POLLNVAL));
        fds_u[i].revents = (int16_t)rev;
        if (rev) ready++;
        if (!first_sock) {
            fd_entry_t* e = fd_get(fds_u[i].fd);
            if (e && e->kind == FD_SOCKET) first_sock = (uint32_t)e->sock;
        }
    }

    if (ready) {
        P->poll_key = 0;
        P->poll_deadline = 0;
        return ready;
    }

    int expired = 0;
    uint32_t deadline = poll_deadline_for(P, (uint32_t)fds_u, timeout_ms,
                                          &expired);
    if (expired) return 0;

    posix_request_block_until(first_sock, poll_next_wake(deadline), regs);
    return 0;   /* not used: the call is going to happen again */
}

/* select's fd_set is a bitmap; only the words covering `nfds` descriptors
 * are read or written, because the caller's set may be far wider than
 * this kernel's table (glibc's is 1024 bits). */
static uint32_t fdset_get(const uint32_t* set, int fd) {
    return set ? ((set[fd / 32] >> (fd % 32)) & 1u) : 0u;
}

static int32_t sys_select(int nfds, uint32_t* rd, uint32_t* wr, uint32_t* ex,
                          uint32_t* tv, registers_t* regs) {
    posix_proc_t* P = posix_current();
    if (nfds < 0 || nfds > MAX_FDS) {
        if (nfds < 0) return -EINVAL;
        nfds = MAX_FDS;
    }

    uint32_t rout = 0, wout = 0, eout = 0;
    int ready = 0;
    uint32_t first_sock = 0;

    for (int fd = 0; fd < nfds; fd++) {
        int want_r = fdset_get(rd, fd), want_w = fdset_get(wr, fd);
        int want_e = fdset_get(ex, fd);
        if (!want_r && !want_w && !want_e) continue;

        uint32_t got = fd_ready_mask(fd);
        if (got & POLLNVAL) return -EBADF;
        if (want_r && (got & (POLLIN | POLLHUP))) { rout |= 1u << fd; ready++; }
        if (want_w && (got & POLLOUT))            { wout |= 1u << fd; ready++; }
        if (want_e && (got & POLLPRI))            { eout |= 1u << fd; ready++; }

        if (!first_sock) {
            fd_entry_t* e = fd_get(fd);
            if (e && e->kind == FD_SOCKET) first_sock = (uint32_t)e->sock;
        }
    }

    /* Only the words that cover `nfds` descriptors are written back. The
     * caller's fd_set may be much wider (glibc's is 1024 bits) or much
     * narrower (a program using raw syscalls declares what it needs), and
     * writing a fixed 128 bytes would corrupt the second kind. */
    int words = (nfds + 31) / 32;
    if (words < 1) words = 1;

    if (ready) {
        if (rd) { rd[0] = rout; for (int i = 1; i < words; i++) rd[i] = 0; }
        if (wr) { wr[0] = wout; for (int i = 1; i < words; i++) wr[i] = 0; }
        if (ex) { ex[0] = eout; for (int i = 1; i < words; i++) ex[i] = 0; }
        P->poll_key = 0;
        P->poll_deadline = 0;
        return ready;
    }

    /* struct timeval { tv_sec, tv_usec }, and a null pointer means "wait
     * forever" - not "do not wait", which is what a zeroed one means. */
    int timeout_ms = -1;
    if (tv) {
        uint32_t ms = tv[0] * 1000u + tv[1] / 1000u;
        timeout_ms = (int)ms;
        if (timeout_ms == 0 && (tv[0] || tv[1])) timeout_ms = 1;
    }

    int expired = 0;
    uint32_t deadline = poll_deadline_for(P, (uint32_t)rd + (uint32_t)wr + 1u,
                                          timeout_ms, &expired);
    if (expired) {
        if (rd) for (int i = 0; i < K_FDSET_WORDS; i++) rd[i] = 0;
        if (wr) for (int i = 0; i < K_FDSET_WORDS; i++) wr[i] = 0;
        if (ex) for (int i = 0; i < K_FDSET_WORDS; i++) ex[i] = 0;
        return 0;
    }

    posix_request_block_until(first_sock, poll_next_wake(deadline), regs);
    return 0;   /* not used: the call is going to happen again */
}

/* --- fcntl ---------------------------------------------------------------
 *
 * Every one of these came out of running real Wine: it duplicates
 * descriptors onto known numbers before exec, marks the ones the child
 * must not inherit, and switches its server socket between blocking and
 * not. There is no file locking behind F_SETLK - a single machine with one
 * process tree has nothing to lock against - and saying "granted" is what
 * Linux would say for an uncontended lock anyway. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_SETOWN         8
#define F_GETOWN         9
#define F_GETLK64       12
#define F_SETLK64       13
#define F_SETLKW64      14
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC       1
#define O_NONBLOCK  0x800

static int32_t sys_fcntl(int fd, int cmd, uint32_t arg) {
    fd_entry_t* e = fd_get(fd);
    if (!e) return -EBADF;

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int newfd = fd_alloc_from(posix_current(), (int)arg);
            if (newfd < 0) return -EMFILE;
            return fd_dup_to(fd, newfd, cmd == F_DUPFD_CLOEXEC);
        }
        case F_GETFD: return e->cloexec ? FD_CLOEXEC : 0;
        case F_SETFD: e->cloexec = (arg & FD_CLOEXEC) ? 1 : 0; return 0;
        case F_GETFL: {
            int32_t fl = e->writable ? O_RDWR : O_RDONLY;
            if (e->append) fl |= O_APPEND;
            if (e->kind == FD_SOCKET && socket_get_nonblock(e->sock)) {
                fl |= O_NONBLOCK;
            }
            return fl;
        }
        case F_SETFL:
            /* Only the two bits that can be changed after the fact are;
             * the access mode is fixed at open time and Linux ignores an
             * attempt to change it rather than refusing. */
            e->append = (arg & O_APPEND) ? 1 : 0;
            if (e->kind == FD_SOCKET) {
                socket_set_nonblock(e->sock, (arg & O_NONBLOCK) ? 1 : 0);
            }
            return 0;
        case F_GETLK: case F_SETLK: case F_SETLKW:
        case F_GETLK64: case F_SETLK64: case F_SETLKW64:
            return 0;    /* granted: there is nobody to contend with */
        case F_SETOWN: return 0;
        case F_GETOWN: return posix_current()->pid;
        default: return -EINVAL;
    }
}

/* --- everything else ---------------------------------------------------- */

static int32_t sys_uname(uint8_t* buf) {
    if (!buf) return -EFAULT;
    /* struct utsname: six 65-byte fields. */
    kmemset(buf, 0, 6 * 65);
    kstrlcpy((char*)(buf + 0 * 65), "Novaris", 65);
    kstrlcpy((char*)(buf + 1 * 65), "novaris", 65);
    kstrlcpy((char*)(buf + 2 * 65), "0.18", 65);
    kstrlcpy((char*)(buf + 3 * 65), "Novaris Milestone 18", 65);
    kstrlcpy((char*)(buf + 4 * 65), "i686", 65);
    kstrlcpy((char*)(buf + 5 * 65), "(none)", 65);
    return 0;
}

static int32_t sys_nanosleep(const uint32_t* req) {
    if (!req) return -EFAULT;
    uint32_t ticks = req[0] * 100u + req[1] / 10000000u; /* 100Hz PIT */
    uint32_t target = pit_get_ticks() + ticks;
    __asm__ __volatile__("sti");
    while (pit_get_ticks() < target) __asm__ __volatile__("hlt");
    __asm__ __volatile__("cli");
    return 0;
}

static void report_unimplemented(uint32_t number) {
    posix_proc_t* P = posix_current();
    P->unimplemented++;
    if (P->unimplemented > 8) return; /* one screenful is plenty */
    char buf[12];
    terminal_writestring_color("[posix] ", VGA_COLOR_LIGHT_BROWN);
    terminal_writestring("unimplemented syscall ");
    ku32_to_dec(number, buf);
    terminal_writestring(buf);
    terminal_writestring(" -> -ENOSYS\n");
}

/* --- tracing ------------------------------------------------------------ */

static int trace_on = 0;

void posix_set_trace(int on) { trace_on = on; }
int posix_trace_enabled(void) { return trace_on; }

static const char* syscall_name(uint32_t n) {
    switch (n) {
        case SYS_exit: return "exit";
        case SYS_read: return "read";
        case SYS_write: return "write";
        case SYS_open: return "open";
        case SYS_readlink: return "readlink";
        case SYS_getcwd: return "getcwd";
        case SYS__llseek: return "_llseek";
        case SYS_prctl: return "prctl";
        case SYS_socketcall: return "socketcall";
        case SYS_unlink: return "unlink";
        case SYS_unlinkat: return "unlinkat";
        case SYS_mkdir: return "mkdir";
        case SYS_mkdirat: return "mkdirat";
        case SYS_rmdir: return "rmdir";
        case SYS_rename: return "rename";
        case SYS_renameat: return "renameat";
        case SYS_renameat2: return "renameat2";
        case SYS_getdents64: return "getdents64";
        case SYS_ftruncate64: return "ftruncate64";
        case SYS_ftruncate: return "ftruncate";
        case SYS_truncate: return "truncate";
        case SYS_truncate64: return "truncate64";
        case SYS_fsync: return "fsync";
        case SYS_chdir: return "chdir";
        case SYS_close: return "close";
        case SYS_lseek: return "lseek";
        case SYS_getpid: return "getpid";
        case SYS_access: return "access";
        case SYS_kill: return "kill";
        case SYS_brk: return "brk";
        case SYS_ioctl: return "ioctl";
        case SYS_munmap: return "munmap";
        case SYS_uname: return "uname";
        case SYS_mprotect: return "mprotect";
        case SYS_writev: return "writev";
        case SYS_pread64: return "pread64";
        case SYS_rt_sigaction: return "rt_sigaction";
        case SYS_rt_sigprocmask: return "rt_sigprocmask";
        case SYS_sigaltstack: return "sigaltstack";
        case SYS_sigreturn: return "sigreturn";
        case SYS_rt_sigreturn: return "rt_sigreturn";
        case SYS_mmap2: return "mmap2";
        case SYS_stat64: return "stat64";
        case SYS_fstat64: return "fstat64";
        case SYS_fstatat64: return "fstatat64";
        case SYS_openat: return "openat";
        case SYS_faccessat: return "faccessat";
        case SYS_faccessat2: return "faccessat2";
        case SYS_readlinkat: return "readlinkat";
        case SYS_set_thread_area: return "set_thread_area";
        case SYS_clone: return "clone";
        case SYS_futex: return "futex";
        case SYS_gettid: return "gettid";
        case SYS_exit_group: return "exit_group";
        case SYS_set_tid_address: return "set_tid_address";
        case SYS_set_robust_list: return "set_robust_list";
        case SYS_getrandom: return "getrandom";
        case SYS_ugetrlimit: return "ugetrlimit";
        case SYS_statx: return "statx";
        case SYS_rseq: return "rseq";
        case SYS_clock_gettime: return "clock_gettime";
        case SYS_clock_gettime64: return "clock_gettime64";
        default: return 0;
    }
}

static void trace_hex(uint32_t v) {
    char b[12];
    ku32_to_hex(v, b, 0, 8);
    terminal_writestring("0x");
    terminal_writestring(b);
}

static void trace_enter(uint32_t n, uint32_t a, uint32_t b, uint32_t c,
                        uint32_t d, uint32_t e, uint32_t f) {
    char buf[12];
    terminal_writestring_color("[trace] ", VGA_COLOR_LIGHT_CYAN);
    const char* name = syscall_name(n);
    if (name) {
        terminal_writestring(name);
    } else {
        terminal_writestring("syscall#");
        ku32_to_dec(n, buf);
        terminal_writestring(buf);
    }
    terminal_writestring("(");
    trace_hex(a); terminal_writestring(", ");
    trace_hex(b); terminal_writestring(", ");
    trace_hex(c); terminal_writestring(", ");
    trace_hex(d); terminal_writestring(", ");
    trace_hex(e); terminal_writestring(", ");
    trace_hex(f);
    terminal_writestring(")");
    /* The first argument of the path-taking calls is worth seeing
     * literally - which library the linker is reaching for is usually the
     * whole question. */
    const char* path = 0;
    if (n == SYS_open || n == SYS_access || n == SYS_stat64) path = (const char*)a;
    if (n == SYS_openat || n == SYS_faccessat || n == SYS_faccessat2) {
        path = (const char*)b;
    }
    if (path) {
        terminal_writestring(" \"");
        terminal_writestring(path);
        terminal_writestring("\"");
    }
}

static void trace_exit(int32_t r) {
    char buf[12];
    terminal_writestring(" = ");
    if (r < 0 && r > -4096) {
        terminal_writestring("-");
        ku32_to_dec((uint32_t)(-r), buf);
        terminal_writestring(buf);
    } else {
        trace_hex((uint32_t)r);
    }
    terminal_writestring("\n");
}

void posix_syscall(registers_t* regs) {
    cur_regs = regs;
    uint32_t n = regs->eax;
    uint32_t a = regs->ebx, b = regs->ecx, c = regs->edx;
    uint32_t d = regs->esi, e = regs->edi, f = regs->ebp;
    int32_t r;

    if (trace_on) trace_enter(n, a, b, c, d, e, f);

    switch (n) {
        case SYS_write:    r = sys_write((int)a, (const char*)b, c); break;
        case SYS_read:     r = sys_read((int)a, (char*)b, c); break;
        case SYS_writev:   r = sys_writev((int)a, (const iovec_t*)b, (int)c); break;
        case SYS_open:     r = sys_open((const char*)a, (int)b, c); break;
        case SYS_access:
            /* The dynamic linker probes with access() before it opens, so
             * answering this is what lets it find a library at all. */
            r = sys_access((const char*)a, (int)b);
            break;
        case SYS_faccessat:
            r = sys_access((const char*)b, (int)c);
            break;
        case SYS_faccessat2:
            r = sys_access((const char*)b, (int)c);
            break;

        case SYS_openat:
            /* (dirfd, path, flags, mode). There is no per-process working
             * directory, so a relative path resolves from the root and
             * dirfd carries no information - which is exactly right for
             * AT_FDCWD, the only value glibc passes. */
            r = sys_open((const char*)b, (int)c, d);
            break;

        /* --- the writable half (Milestone 26) ------------------------- */
        case SYS_unlink:   r = sys_unlink((const char*)a, 0); break;
        case SYS_rmdir:    r = sys_unlink((const char*)a, 1); break;
        case SYS_mkdir:    r = sys_mkdir((const char*)a, b); break;
        case SYS_unlinkat:
            /* (dirfd, path, flags). AT_REMOVEDIR is 0x200 and is how
             * glibc's rmdir() reaches this. */
            r = sys_unlink((const char*)b, (c & 0x200) ? 1 : 0);
            break;
        case SYS_mkdirat:  r = sys_mkdir((const char*)b, c); break;
        case SYS_chmod:    r = sys_chmod((const char*)a, b); break;
        case SYS_fchmodat: r = sys_chmod((const char*)b, c); break;
        case SYS_fchmod: {
            fd_entry_t* fe = fd_get((int)a);
            if (!fe || fe->kind != FD_FILE) { r = -EBADF; break; }
            fe->node->mode = b & 07777;
            r = 0;
            break;
        }
        case SYS_rename:   r = sys_rename((const char*)a, (const char*)b); break;
        case SYS_renameat: r = sys_rename((const char*)b, (const char*)d); break;
        case SYS_renameat2:
            /* (olddirfd, old, newdirfd, new, flags). glibc's rename()
             * uses this with flags 0; the RENAME_NOREPLACE/EXCHANGE forms
             * are refused rather than silently treated as a plain rename,
             * and glibc falls back cleanly. */
            if (e) { r = -EINVAL; break; }
            r = sys_rename((const char*)b, (const char*)d);
            break;
        case SYS_ftruncate:
            /* The 32-bit forms. glibc uses the 64-bit ones, but a program
             * writing raw syscalls reaches for these - and so does
             * anything built before large-file support. */
            r = sys_ftruncate((int)a, b);
            break;
        case SYS_truncate: {
            vfs_node_t* t = resolve_for_read((const char*)a);
            r = t ? vfs_truncate(t, b) : -ENOENT;
            break;
        }
        case SYS_ftruncate64:
            /* (fd, length_lo, length_hi) - the 64-bit length arrives as a
             * register pair, and anything needing the high half is far
             * beyond what fits in RAM here. */
            if (c) { r = -EINVAL; break; }
            r = sys_ftruncate((int)a, b);
            break;
        case SYS_truncate64: {
            if (c) { r = -EINVAL; break; }
            vfs_node_t* t = vfs_lookup((const char*)a);
            r = t ? vfs_truncate(t, b) : -ENOENT;
            break;
        }
        case SYS_getdents64:
            r = sys_getdents64((int)a, (uint8_t*)b, c);
            break;
        case SYS_fsync:
            /* There is nothing behind the filesystem to flush to. */
            r = fd_get((int)a) ? 0 : -EBADF;
            break;
        case SYS_chdir:
            /* Honoured since Milestone 29: the working directory is per
             * process and every relative path resolves against it. */
            r = sys_chdir((const char*)a);
            break;
        case SYS_pread64: {
            /* (fd, buf, count, offset_lo, offset_hi) - a positioned read
             * that does not disturb the descriptor's own offset. The
             * dynamic linker reads ELF headers this way. */
            fd_entry_t* e = fd_get((int)a);
            if (!e || e->kind != FD_FILE) { r = -EBADF; break; }
            uint32_t off = d;
            if (off >= e->node->length) { r = 0; break; }
            uint32_t left = e->node->length - off;
            uint32_t n = c < left ? c : left;
            r = (int32_t)vfs_read(e->node, off, n, (uint8_t*)b);
            break;
        }
        case SYS_pwrite64: {
            /* (fd, buf, count, offset_lo, offset_hi) - a positioned write
             * that leaves the descriptor's own offset alone. wineserver
             * writes its registry and its lock file this way, and the
             * three -ENOSYS reports it produced without this were the
             * "file_set_error() can't map error" messages it printed
             * back. */
            fd_entry_t* fe = fd_get((int)a);
            if (!fe || fe->kind != FD_FILE) { r = -EBADF; break; }
            if (!fe->writable) { r = -EBADF; break; }
            if (e) { r = -EINVAL; break; }   /* the high half of the offset */
            r = vfs_write(fe->node, d, c, (const uint8_t*)b);
            break;
        }
        case SYS_statfs64:
        case SYS_fstatfs64: {
            /* struct statfs64, 84 bytes on i386. There is one filesystem,
             * it is in RAM, and the fields a program reads are the block
             * size and how much of it there is. */
            uint32_t* sf = (uint32_t*)((n == SYS_statfs64) ? c : c);
            if (!sf) { r = -EFAULT; break; }
            kmemset(sf, 0, 84);
            sf[0] = 0x858458F6u;                  /* f_type: RAMFS_MAGIC */
            sf[1] = PAGE_SIZE;                    /* f_bsize */
            ((uint64_t*)(sf + 2))[0] = pmm_total_frames();   /* f_blocks */
            ((uint64_t*)(sf + 4))[0] = pmm_free_frames();    /* f_bfree */
            ((uint64_t*)(sf + 6))[0] = pmm_free_frames();    /* f_bavail */
            sf[19] = 255;                         /* f_namelen */
            r = 0;
            break;
        }
        case SYS_close:    r = sys_close((int)a); break;
        case SYS_lseek:    r = sys_lseek((int)a, (int32_t)b, (int)c); break;
        case SYS_fstat64:  r = sys_fstat64((int)a, (uint8_t*)b); break;
        case SYS_stat64:   r = sys_stat64((const char*)a, (uint8_t*)b); break;

        case SYS_mmap2:    r = sys_mmap2(a, b, (int)c, (int)d, (int)e, f); break;
        case SYS_munmap:   r = sys_munmap(a, b); break;
        case SYS_mprotect: r = sys_mprotect(a, b, (int)c); break;
        case SYS_brk:      r = sys_brk(a); break;

        case SYS_getpid:   r = posix_current()->pid; break;
        case SYS_getppid:  r = posix_current()->ppid; break;
        case SYS_gettid:   r = scheduler_current_pid() ? scheduler_current_pid()
                                                       : posix_current()->pid;
                           break;
        case SYS_getuid32: case SYS_geteuid32:
        case SYS_getgid32: case SYS_getegid32: r = 0; break;
        /* There are no sessions, process groups or file-mode masks here.
         * Answering rather than reporting is what Linux effectively does
         * for a process that is already its own group leader, and umask's
         * previous value is the 022 every shell starts with. */
        case SYS_getpgrp:  r = posix_current()->pid; break;
        case SYS_setpgid:  r = 0; break;
        case SYS_setsid:   r = posix_current()->pid; break;
        case SYS_umask:    r = 022; break;

        /* --- two processes at once (Milestone 29) --------------------- */
        case SYS_fork:
            r = sys_fork(regs);
            break;
        case SYS_clone:
            /* Three shapes reach one syscall, and CLONE_THREAD is what
             * separates them - not CLONE_VM, which was the obvious guess
             * and the wrong one.
             *
             *   - no CLONE_VM at all: fork. glibc's fork() arrives this
             *     way rather than through syscall 2.
             *   - CLONE_VM without CLONE_THREAD: a *child process* that
             *     Linux lets share its parent's memory - glibc's
             *     posix_spawn, which is how Wine starts wineserver. Almost
             *     always with CLONE_VFORK, meaning the parent must not run
             *     until the child has exec'd.
             *   - CLONE_VM with CLONE_THREAD: a thread.
             */
            if (!(a & 0x00000100u /* CLONE_VM */)) { r = sys_fork(regs); break; }
            if (!(a & 0x00010000u /* CLONE_THREAD */)) {
                r = fork_common(regs, b, (a & 0x00004000u /* CLONE_VFORK */) != 0);
                if (r > 0 && (a & 0x00100000u /* CLONE_PARENT_SETTID */) && c) {
                    *(uint32_t*)c = (uint32_t)r;
                }
                break;
            }
            r = posix_sys_clone(a, b, (uint32_t*)c, d, (uint32_t*)e, regs);
            break;
        case SYS_vfork:
            r = fork_common(regs, 0, 1);
            break;
        case SYS_execve: {
            /* On success this rewrites the trap frame to be the new
             * program's initial state, so returning through the tail of
             * this function would overwrite its eax with a result nobody
             * is waiting for. On failure it changed nothing and the errno
             * goes back to the caller as usual. */
            int32_t rc = sys_execve((const char*)a, b, c, regs);
            if (rc == 0) return;
            r = rc;
            break;
        }
        case SYS_waitpid:
            r = sys_wait4((int)a, (int32_t*)b, (int)c, regs);
            break;
        case SYS_wait4:
            r = sys_wait4((int)a, (int32_t*)b, (int)c, regs);
            break;

        case SYS_dup:      r = sys_dup((int)a); break;
        case SYS_dup2:     r = fd_dup_to((int)a, (int)b, 0); break;
        case SYS_dup3:
            /* dup2 with flags, and the one flag is O_CLOEXEC. Duplicating
             * a descriptor onto itself is an error here where dup2 makes
             * it a no-op - Linux draws exactly that distinction. */
            if (a == b) { r = -EINVAL; break; }
            r = fd_dup_to((int)a, (int)b, (c & O_CLOEXEC) ? 1 : 0);
            break;
        case SYS_pipe:     r = sys_pipe((int32_t*)a, 0); break;
        case SYS_pipe2:    r = sys_pipe((int32_t*)a, (int)b); break;

        case SYS_poll:
            r = sys_poll((k_pollfd_t*)a, b, (int)c, regs);
            break;
        case SYS_ppoll: {
            /* (fds, nfds, timespec*, sigmask, sigsetsize). The mask is
             * ignored: it exists to close a race between unblocking a
             * signal and waiting, and this kernel delivers signals only on
             * the way back to ring 3, so the race it closes cannot happen
             * here. Saying so beats refusing the call glibc's poll()
             * actually makes. */
            int ms = -1;
            if (c) {
                const int32_t* ts = (const int32_t*)c;
                ms = ts[0] * 1000 + ts[1] / 1000000;
            }
            r = sys_poll((k_pollfd_t*)a, b, ms, regs);
            break;
        }
        case SYS__newselect:
            r = sys_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d,
                           (uint32_t*)e, regs);
            break;
        case SYS_pselect6: {
            /* Same five sets, but the timeout is a timespec and there is a
             * sixth argument nobody here needs. glibc's select() uses this
             * on a current kernel. */
            uint32_t tv[2];
            uint32_t* tvp = 0;
            if (e) {
                const uint32_t* ts = (const uint32_t*)e;
                tv[0] = ts[0];
                tv[1] = ts[1] / 1000u;
                tvp = tv;
            }
            r = sys_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d,
                           tvp, regs);
            break;
        }

        case SYS_uname:    r = sys_uname((uint8_t*)a); break;

        /* --- what a real glibc asks for on the way to main() ------------
         * Every one of these was found by running an actual
         * glibc-linked binary and reading the -ENOSYS reports it
         * provoked, rather than guessed at from a list. */

        case SYS_ugetrlimit: {
            /* struct rlimit { rlim_cur, rlim_max }. glibc reads RLIMIT_STACK
             * (resource 3) to size its own guard pages. */
            uint32_t* lim = (uint32_t*)b;
            if (!lim) { r = -EFAULT; break; }
            lim[0] = (a == 3) ? ELF_STACK_BYTES : 0xFFFFFFFFu;
            lim[1] = lim[0];
            r = 0;
            break;
        }

        case SYS_getrandom: {
            /* Not cryptographic - the PIT tick is the only entropy here -
             * and saying so matters, because glibc seeds pointer mangling
             * from it. Enough to be *different* per call, not enough to be
             * unguessable. */
            uint8_t* buf = (uint8_t*)a;
            if (!buf) { r = -EFAULT; break; }
            static uint32_t seed = 0;
            if (!seed) seed = pit_get_ticks() | 1u;
            for (uint32_t i = 0; i < b; i++) {
                seed = seed * 1103515245u + 12345u;
                buf[i] = (uint8_t)(seed >> 16);
            }
            r = (int32_t)b;
            break;
        }

        case SYS_clock_gettime: {
            /* struct timespec { tv_sec, tv_nsec }, 100Hz resolution. */
            uint32_t* ts = (uint32_t*)b;
            if (!ts) { r = -EFAULT; break; }
            uint32_t ticks = pit_get_ticks();
            ts[0] = rtc_unix_time();
            ts[1] = (ticks % 100u) * 10000000u;
            r = 0;
            break;
        }

        case SYS_clock_gettime64: {
            /* Same, with a 64-bit tv_sec - what a 32-bit glibc built past
             * the 2038 transition calls first. */
            uint32_t* ts = (uint32_t*)b;
            if (!ts) { r = -EFAULT; break; }
            uint32_t ticks = pit_get_ticks();
            ts[0] = rtc_unix_time();
            ts[1] = 0;
            ts[2] = (ticks % 100u) * 10000000u;
            ts[3] = 0;
            r = 0;
            break;
        }

        /* Accepted and ignored: robust-futex bookkeeping for a process
         * that never dies holding one, and restartable sequences, which
         * glibc treats as optional and falls back from cleanly. */
        case SYS_set_robust_list: r = 0; break;
        case SYS_rseq:            r = -ENOSYS; break;

        /* Genuinely absent, and answered rather than reported: there are
         * no symlinks and no statx, and glibc falls back to fstat64. */
        case SYS_fstatat64:
            /* (dirfd, path, statbuf, flags). AT_EMPTY_PATH (0x1000) means
             * "stat the descriptor itself", which is how glibc's fstat is
             * routed on a kernel new enough to have this; otherwise the
             * path is relative to a directory, and the initrd is flat so
             * there is only one. */
            if (d & 0x1000u) {
                r = sys_fstat64((int)a, (uint8_t*)c);
            } else {
                r = sys_stat64((const char*)b, (uint8_t*)c);
            }
            break;

        case SYS_readlink:
            r = sys_readlink((const char*)a, (char*)b, c);
            break;
        case SYS_readlinkat:
            /* (dirfd, path, buf, size) - what glibc's readlink() actually
             * calls. */
            r = sys_readlink((const char*)b, (char*)c, d);
            break;
        case SYS_getcwd:  r = sys_getcwd((char*)a, b); break;
        case SYS__llseek: {
            /* (fd, offset_high, offset_low, loff_t* result, whence).
             * glibc's stdio reaches for this rather than lseek, so a
             * program that reads a file through FILE* needs it - which is
             * how it was found: Wine's ntdll asks glibc for the passwd
             * entry, glibc could not seek, and getpwuid() returned NULL
             * for Wine to dereference. Offsets past 4GB are refused
             * rather than truncated. */
            if (a > 0x7FFFFFFFu) { r = -EINVAL; break; }
            if (b) { r = -EINVAL; break; }   /* the high half of the offset */
            int32_t pos = sys_lseek((int)a, (int32_t)c, (int)e);
            if (pos < 0) { r = pos; break; }
            if (!d) { r = -EFAULT; break; }
            *(int64_t*)d = pos;
            r = 0;
            break;
        }
        case SYS_socketcall:
            /* i386 multiplexes every socket operation through one call:
             * the operation in ebx, a pointer to its argument block in
             * ecx. See kernel/socket.c. */
            r = socket_syscall(a, (uint32_t*)b, regs);
            break;
        case SYS_prctl:
            /* PR_SET_NAME and friends: accepted and ignored. Wine's
             * preloader sets its process name, and there is nowhere here
             * for a process name to be shown. */
            r = 0;
            break;
        case SYS_statx:      r = -ENOSYS; break;

        /* --- what real Wine asked for next (Milestone 29) ---------------
         * Every one of these was found the way the rest of this file's
         * entries were: by running Wine and reading the -ENOSYS reports it
         * provoked on the way to starting a wineserver. */
        case SYS_fcntl:
        case SYS_fcntl64:
            r = sys_fcntl((int)a, (int)b, c);
            break;

        case SYS_getrlimit:
        case SYS_prlimit64: {
            /* prlimit64 is (pid, resource, new*, old*) with 64-bit
             * values; getrlimit is (resource, rlimit*) with 32-bit ones.
             * Nothing here enforces a limit, so a new one is accepted and
             * dropped and the old one is reported. */
            if (n == SYS_prlimit64) {
                if (a && a != (uint32_t)posix_current()->pid) { r = -ESRCH; break; }
                uint32_t* old = (uint32_t*)d;
                if (old) {
                    uint64_t v = (b == 3) ? ELF_STACK_BYTES : 0xFFFFFFFFFFFFFFFFull;
                    ((uint64_t*)old)[0] = v;
                    ((uint64_t*)old)[1] = v;
                }
            } else {
                uint32_t* lim = (uint32_t*)b;
                if (!lim) { r = -EFAULT; break; }
                lim[0] = (a == 3) ? ELF_STACK_BYTES : 0xFFFFFFFFu;
                lim[1] = lim[0];
            }
            r = 0;
            break;
        }
        case SYS_setrlimit: r = 0; break;

        case SYS_getrusage:
            /* struct rusage is 72 bytes on i386 and there is no per-process
             * accounting to fill it from. Zeroed is honest; refusing would
             * fail programs that only report it. */
            if (!b) { r = -EFAULT; break; }
            kmemset((void*)b, 0, 72);
            r = 0;
            break;

        case SYS_gettimeofday: {
            /* struct timeval { tv_sec, tv_usec }, 100Hz resolution - the
             * PIT tick is all there is. A null timezone pointer is the
             * only kind anything passes any more. */
            uint32_t* tv = (uint32_t*)a;
            if (tv) {
                tv[0] = rtc_unix_time();
                tv[1] = (pit_get_ticks() % 100u) * 10000u;
            }
            if (b) { ((uint32_t*)b)[0] = 0; ((uint32_t*)b)[1] = 0; }
            r = 0;
            break;
        }

        case SYS_clock_getres: {
            /* 10ms, which is the PIT rate and therefore the truth. */
            uint32_t* ts = (uint32_t*)b;
            if (ts) { ts[0] = 0; ts[1] = 10000000u; }
            r = 0;
            break;
        }

        case SYS_symlink:
        case SYS_symlinkat:
            /* There are no symlinks in this filesystem and there is no
             * honest way to fake one. Wine makes them inside its prefix
             * (the dosdevices drive letters) and carries on when they
             * fail, which is why -EPERM rather than -ENOSYS: the operation
             * is understood and refused, not unknown. */
            r = -EPERM;
            break;

        case SYS_lstat64:
            /* No symlinks, so lstat and stat cannot differ. */
            r = sys_stat64((const char*)a, (uint8_t*)b);
            break;

        case SYS_fchdir: {
            /* Real, because wineserver opens its config directory and
             * then moves into it by descriptor rather than by name -
             * which is the safe way to do it on a real system, and was
             * the last thing standing between it and starting. The path
             * is rebuilt from the tree, since no node stores one. */
            fd_entry_t* fe = fd_get((int)a);
            if (!fe || fe->kind != FD_FILE) { r = -EBADF; break; }
            if (!(fe->node->flags & VFS_DIRECTORY)) { r = -ENOTDIR; break; }
            char here[POSIX_PATH_MAX];
            if (!vfs_path_of(fe->node, here, sizeof(here))) { r = -ENAMETOOLONG; break; }
            kstrlcpy(posix_current()->cwd, here, POSIX_PATH_MAX);
            r = 0;
            break;
        }
        case SYS_setpriority:
        case SYS_getpriority:
            /* One priority, and it is the only one. Round-robin with a
             * fixed slice has nothing to raise or lower. */
            r = 0;
            break;

        case SYS_sched_getaffinity: {
            /* (pid, cpusetsize, mask). One CPU, so one bit. Wine reads
             * this to size its thread pools; answering "processor 0, and
             * that is all" is the truth here. */
            if (c == 0) { r = -EFAULT; break; }
            if (b < 4) { r = -EINVAL; break; }
            uint32_t* mask = (uint32_t*)c;
            mask[0] = 1;
            r = 4;
            break;
        }
        case SYS_sched_setaffinity:
            /* Accepted: there is one processor and every mask that names
             * it is satisfied by doing nothing. */
            r = 0;
            break;

        case SYS_sched_yield:
            /* A real yield: the caller is telling the scheduler it has
             * nothing to do, and there is a scheduler to tell. */
            scheduler_yield_from_trap(regs);
            r = 0;
            break;

        case SYS_madvise:
            /* Advice, and this kernel has no paging policy to advise. */
            r = 0;
            break;

        case SYS_epoll_create:
        case SYS_epoll_create1:
            /* Refused deliberately rather than half-built. wineserver asks
             * for an epoll descriptor once at startup and falls back to
             * poll() for the whole of its main loop if it does not get
             * one - which is the path Novaris implements properly. A
             * half-working epoll would be chosen over a working poll. */
            r = -ENOSYS;
            break;

        case SYS_sysinfo:
            /* struct sysinfo, 64 bytes on i386. Uptime and memory are the
             * two fields anything reads and both are known. */
            if (!a) { r = -EFAULT; break; }
            kmemset((void*)a, 0, 64);
            ((uint32_t*)a)[0] = pit_get_ticks() / 100u;          /* uptime */
            ((uint32_t*)a)[4] = pmm_total_frames() * PAGE_SIZE;  /* totalram */
            ((uint32_t*)a)[5] = pmm_free_frames() * PAGE_SIZE;   /* freeram */
            ((uint32_t*)a)[13] = 1;                              /* procs */
            ((uint32_t*)a)[14] = PAGE_SIZE;                      /* mem_unit */
            r = 0;
            break;

        case SYS_time:     r = (int32_t)rtc_unix_time(); break;
        case SYS_nanosleep: r = sys_nanosleep((const uint32_t*)a); break;
        case SYS_clock_nanosleep:
            /* (clockid, flags, request, remain). TIMER_ABSTIME (1) would
             * mean the timespec is a deadline rather than a duration;
             * nothing here asks for it, and treating one as the other
             * would sleep for fifty-odd years. */
            if (b & 1) { r = -ENOSYS; break; }
            r = sys_nanosleep((const uint32_t*)c);
            break;

        /* Real since Milestone 19 - see kernel/posix_signal.c. */
        case SYS_rt_sigaction:
            r = posix_sys_rt_sigaction((int)a, (const k_sigaction_t*)b,
                                       (k_sigaction_t*)c, d);
            break;
        case SYS_rt_sigprocmask:
            r = posix_sys_rt_sigprocmask((int)a, (const uint32_t*)b,
                                         (uint32_t*)c, d);
            break;
        case SYS_sigaltstack:
            r = posix_sys_sigaltstack((const k_stack_t*)a, (k_stack_t*)b);
            break;
        case SYS_sigreturn:
            /* The *legacy* sigreturn, which glibc's restorer issues for a
             * handler installed without SA_SIGINFO. It differs from the
             * rt_ one only in where the restorer left esp - see
             * posix_sigreturn_common(). A program whose handler was not
             * SA_SIGINFO could not return from it at all before this. */
            posix_sys_sigreturn(regs);
            return;
        case SYS_rt_sigreturn:
            /* Restores the interrupted context wholesale, so eax is set
             * by the restore rather than by the usual `regs->eax = r`
             * below - returning here would overwrite it. */
            posix_sys_rt_sigreturn(regs);
            return;

        case SYS_kill:
            /* Since Milestone 29 the pid is real and a signal can cross
             * between processes: a program can kill its own child, which
             * is how a parent that gives up waiting ends one. A pid that
             * is this process's own still takes the local path, so
             * raise() behaves exactly as it did. */
            if ((int)a == posix_current()->pid || (int)a == 0) {
                r = posix_raise_from((int)b, SI_USER, (int32_t)a);
            } else {
                r = posix_raise_pid((int)a, (int)b, SI_USER,
                                    posix_current()->pid);
            }
            break;
        case SYS_tgkill:
            /* Targets a *thread*, and threads share this process's
             * structure - so this is always the local path. It differs
             * from kill only in the si_code an SA_SIGINFO handler sees,
             * which is how a program tells the two apart. */
            r = posix_raise_from((int)c, SI_TKILL, (int32_t)a);
            break;

        /* Real since Milestone 20 - see kernel/posix_thread.c. */
        case SYS_set_thread_area:
            r = posix_sys_set_thread_area(a);
            break;
        case SYS_futex:
            r = posix_sys_futex(a, (int)b, c, d, regs);
            break;
        case SYS_set_tid_address:
            /* glibc calls this at startup to register where to clear the
             * tid. One process, so the value is simply reported back. */
            r = scheduler_current_pid() ? scheduler_current_pid() : 0x100;
            break;

        /* isatty() is ioctl(TCGETS) under the covers; saying "yes, a
         * terminal" for the console streams and "no" otherwise is enough
         * for a libc to pick line buffering. */
        case SYS_ioctl:
            r = (a < 3) ? 0 : -EINVAL;
            break;

        case SYS_exit:
            if (trace_on) trace_exit(0);
            /* A thread exiting has to clear the word its creator asked
             * for, or a joiner waits for ever. exit_group takes the whole
             * program with it, so it does not. */
            posix_thread_exiting();
            /* fall through */
        case SYS_exit_group:
            /* The status is real since Milestone 29, and in Linux's
             * encoding rather than raw: a normal exit is the low byte of
             * the code shifted up eight, which is what WIFEXITED and
             * WEXITSTATUS read. A parent in wait4 is woken by this. */
            posix_exit_status((int)((a & 0xFFu) << 8));
            return; /* eax is not written: this task is finished */

        default:
            report_unimplemented(n);
            r = -ENOSYS;
            break;
    }

    /* An implementation can ask for the whole call to happen again rather
     * than return - futex(FUTEX_WAIT) does. Restoring eax matters: the
     * syscall number lives there and would otherwise be replaced by this
     * call's result, so the re-executed `int $0x80` would dispatch to
     * whatever that value happened to name. `int $0x80` is two bytes. */
    if (posix_retry_pending()) {
        if (trace_on) { terminal_writestring(" = <retry>\n"); }
        regs->eax = n;
        regs->eip -= 2;
        /* An implementation that named something to wait on gets a real
         * block rather than a spin: the frame above is already rewound to
         * re-execute, so the task can simply be parked until whatever it
         * is waiting for happens. Milestone 25 built the parking; this is
         * the retry loop finally using it. Waking must not overwrite eax,
         * which now holds the syscall number - see PROC_BLOCKED's
         * wait_retry in the scheduler. */
        uint32_t waiting_on = block_addr;
        uint32_t until = block_deadline;
        block_addr = 0;
        block_deadline = 0;
        if ((waiting_on || until) &&
            scheduler_block_current_retry(regs, waiting_on, until)) {
            return;
        }
        /* Nothing else could run, so parking would leave the machine with
         * nobody to wake anyone. With a deadline there is still something
         * to wait *for* - the clock - and idling in the kernel is far
         * cheaper than returning to ring 3 only to re-enter this syscall.
         * The syscall gate is an interrupt gate, so IF has to be set by
         * hand before hlt or the timer that ends the wait never arrives.
         * (posix_sys_futex takes the same path, for the same reason.) */
        if (until) {
            __asm__ __volatile__("sti");
            while ((int32_t)(pit_get_ticks() - until) < 0) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            return;
        }
        scheduler_yield_from_trap(regs);
        return;
    }

    if (trace_on) trace_exit(r);
    regs->eax = (uint32_t)r;
}
