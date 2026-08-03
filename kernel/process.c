#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "user_hello.h"
#include "elf.h"
#include "pe.h"
#include "win32.h"
#include "idt.h"
#include "console.h"
#include "kstring.h"
#include "posix.h"
#include "scheduler.h"
#include "pit.h"
#include "vfs.h"
#include "fat32.h"
#include "kheap.h"

/* Referenced from process_asm.s: the kernel-side stack pointer/frame
 * pointer to restore when the user program exits, so control returns to
 * process_run_demo_user_program()'s caller as if it were an ordinary
 * blocking function call. */
uint32_t kernel_resume_esp = 0;
uint32_t kernel_resume_ebp = 0;

/* Also referenced from process_asm.s - see process_set_user_fs(). */
uint32_t user_fs_selector = 0x23;

#define USER_LOAD_VADDR 0x40000000u /* must match userland/user.ld */
#define USER_STACK_TOP      0x40100000u /* 1MB above the program; grows down */
#define PAGE_SIZE 4096u
/* 64KB of stack for an ELF program - see process_run_elf(). */
#define ELF_STACK_SIZE (16u * PAGE_SIZE)
/* Where a position-independent image goes. Two distinct bases, because a
 * PIE program and its interpreter are both ET_DYN and both have to be
 * loaded at once - and they are far apart so a mistake shows up as a
 * fault rather than as one quietly overwriting the other. */
#define PIE_BASE     0x56000000u
#define INTERP_BASE  0x5A000000u

/* Where an ELF program's stack goes, and it is not where a flat binary's
 * goes (USER_STACK_TOP, just above its 0x40000000 load address).
 *
 * Linux/i386 puts the initial stack at the very top of user space, a
 * little under 0xC0000000, and Milestone 32 found out that this is not
 * cosmetic - real programs have heuristics keyed to it. Wine's
 * mmap_init() takes the address of a local variable and asks whether the
 * stack is above 0x7FFE0000. On Linux it is, and Wine reserves the space
 * below the stack for Windows images and stops at 0xC0000000. With the
 * stack at 0x40100000 the answer was no, and Wine instead tried to
 * reserve everything from 0x7FFE0000 to the top of the 32-bit address
 * space - walking straight into the kernel's half, being refused page
 * after page, and ending up with no reserved area to place images in at
 * all. `mmap refused: above user space addr=0xc0040000` in a boot log,
 * over and over, is what that looked like from here.
 *
 * 16KB below the top rather than exactly at it, so that the argv/envp
 * block build_initial_stack() writes has somewhere to live and a
 * one-page overrun faults instead of wrapping. */
#define ELF_STACK_TOP 0xBFFF0000u

/* Non-zero between entering ring 3 and coming back out of it. Only the
 * fault path reads it, and only to decide whether a CPU exception belongs
 * to a user program or to the kernel. */
static int user_active = 0;

/* --- the running program's address space -------------------------------
 *
 * Milestone 15. Milestone 14 built page directories that a process could
 * run in; this is where one actually does.
 *
 * The whole thing rests on a single property of Milestone 14's design,
 * and it is worth being explicit about it because it is what keeps this
 * change small: the kernel half is *identical* in every address space. So
 * once CR3 holds the process's directory, the kernel keeps working
 * unchanged - its code, its stack, kmalloc, the console, the initrd are
 * all still exactly where they were - while the user half is now the
 * process's own. Which means an `int 0x81` handler that reads a program's
 * arguments by casting a ring-3 pointer, as the entire Win32 layer does,
 * *still reads the right memory*, because the kernel never leaves the
 * process's address space while servicing it.
 *
 * That is why win32.c, pe.c and win32_callback.c did not have to be
 * rewritten to walk page tables. The rule they now depend on is narrower
 * than "one flat address space" but just as simple: never switch CR3
 * while holding a pointer into a process.
 */
static uint32_t current_as = 0;   /* the running process's directory */
static uint32_t previous_as = 0;  /* what to go back to when it ends */

uint32_t process_current_address_space(void) {
    return current_as;
}

int process_enter_address_space(void) {
    uint32_t as = paging_create_address_space();
    /* Out of memory, or paging_finalize_kernel_space() never ran. Running
     * in the kernel's own directory is what every milestone before this
     * one did, so falling back to it is a real degradation but not a
     * broken one - and saying so beats failing to start the program. */
    if (!as) return 0;

    previous_as = paging_current_address_space();
    current_as = as;
    paging_switch_address_space(as);
    return 1;
}

void process_leave_address_space(void) {
    if (!current_as) return;
    /* Order matters: callers unmap the program's pages first, while still
     * standing in its address space, so their frames go back to the PMM
     * through the ordinary paths. Destroying the directory afterwards
     * reclaims anything they missed rather than being the only thing that
     * reclaims anything. */
    paging_switch_address_space(previous_as);
    paging_destroy_address_space(current_as);
    current_as = 0;
    previous_as = 0;
}

void process_set_user_fs(uint16_t selector) {
    user_fs_selector = selector;
}

int process_user_active(void) {
    return user_active;
}

/* Every path into ring 3 goes through here, so `user_active` can't get
 * out of step with reality. */
static void enter_user_mode(uint32_t entry, uint32_t stack_top) {
    user_active = 1;
    /* File descriptors and the mmap/brk arenas belong to the process, so
     * they are set up per run rather than once at boot - otherwise the
     * second program to run would inherit the first one's open files. */
    posix_process_begin();
    process_run_user_mode(entry, stack_top);
    posix_process_end();
    user_active = 0;
}

int process_map_image(uint32_t load_vaddr, const uint8_t* image,
                      uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(load_vaddr + i * PAGE_SIZE, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* dest = (uint8_t*)load_vaddr;
    for (uint32_t i = 0; i < size; i++) dest[i] = image[i];
    return 1;
}

void process_run_flat_binary(const uint8_t* image, uint32_t size) {
    int owns_as = process_enter_address_space();

    process_map_image(USER_LOAD_VADDR, image, size);

    /* One page of user stack, mapped just below USER_STACK_TOP so the
     * initial ESP we hand to ring 3 is a valid "one past the mapped
     * region" descending-stack pointer. */
    uint32_t stack_phys = pmm_alloc_frame();
    paging_map_page(USER_STACK_TOP - PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_RW | PAGE_USER);

    enter_user_mode(USER_LOAD_VADDR, USER_STACK_TOP);

    /* Everything this program mapped lived in its own directory, so
     * destroying it is the unmap - there is no page-by-page teardown to
     * do here the way there was before Milestone 15. */
    if (owns_as) process_leave_address_space();
}

void process_run_demo_user_program(void) {
    process_run_flat_binary(user_hello_bin, user_hello_bin_len);
}

/* Reads a file off the initrd into a kmalloc'd buffer, by a path that may
 * be absolute. The initrd is a flat archive with no directories, so
 * "/lib/ld-linux.so.2" is matched on its last component - which is a
 * simplification the kernel is honest about rather than a filesystem:
 * two files with the same basename in different directories cannot be
 * told apart, and there are none. Caller owns the buffer. */
uint8_t* process_read_file(const char* path, uint32_t* out_size) {
    const char* name = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, name) : 0;
    if (!node) return 0;

    uint8_t* buf = (uint8_t*)kmalloc(node->length);
    if (!buf) return 0;
    vfs_read(node, 0, node->length, buf);
    *out_size = node->length;
    return buf;
}

/* --- the initial process stack ------------------------------------------
 *
 * The System V i386 ABI says a program is entered with esp pointing at
 * argc, followed by argv, a NULL, envp, a NULL, and the auxiliary vector.
 * Novaris did not build any of that until Milestone 21, and got away with
 * it because every ELF program in the tree was freestanding and never
 * looked. A real one looks immediately: glibc's `_start` is
 *
 *     xor %ebp, %ebp
 *     pop %esi          <- argc, and the first instruction to touch the stack
 *
 * which faulted, because the esp handed over was the top of the mapping
 * and there was nothing at it. That fault is what this exists to fix, and
 * finding it is what "run a real glibc binary" was for.
 *
 * Returns the esp to enter the program with. */

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID   11
#define AT_EUID  12
#define AT_GID   13
#define AT_EGID  14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

/* argv is real since Milestone 27, and it had to become real for a
 * concrete reason: Wine's own loader takes the thing it is loading as
 * argv[1]. Until then every ELF program on Novaris was entered with one
 * hardcoded argument, "program", and nothing had ever looked - so nothing
 * had noticed. The first real Wine binary noticed immediately, and said
 * so in its usage message. */
#define MAX_ARGS 16
/* An environment is real since Milestone 29, for the same kind of reason
 * argv became real in Milestone 27: Wine reads WINEPREFIX, WINEDLLPATH and
 * half a dozen others out of it to decide where its installation is, and a
 * program handed an empty environment gets defaults instead of what it was
 * told. execve carries the caller's across; `run` supplies a small one. */
#define MAX_ENVS 16

static uint32_t build_initial_stack(uint32_t stack_top, int argc,
                                    const char* const* argv,
                                    int envc, const char* const* envp,
                                    const elf_info_t* info,
                                    uint32_t interp_base) {
    uint32_t sp = stack_top;

    if (argc < 1) argc = 1;
    if (argc > MAX_ARGS) argc = MAX_ARGS;
    if (envc < 0 || !envp) envc = 0;
    if (envc > MAX_ENVS) envc = MAX_ENVS;

    /* Strings and blobs first, at the very top, so the vector below them
     * can point at fixed addresses. Copied downward in reverse so that
     * argv[0] ends up highest, which is the order a real stack has them
     * and the order /proc/self/cmdline would report. */
    uint32_t envpp[MAX_ENVS];
    for (int i = envc - 1; i >= 0; i--) {
        uint32_t len = kstrlen(envp[i]) + 1;
        sp -= len;
        envpp[i] = sp;
        for (uint32_t j = 0; j < len; j++) ((char*)sp)[j] = envp[i][j];
    }

    uint32_t argp[MAX_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = kstrlen(argv[i]) + 1;
        sp -= len;
        argp[i] = sp;
        for (uint32_t j = 0; j < len; j++) ((char*)sp)[j] = argv[i][j];
    }
    uint32_t argv0 = argp[0];

    static const char platform[] = "i686";
    sp -= sizeof(platform);
    uint32_t plat = sp;
    for (uint32_t i = 0; i < sizeof(platform); i++) {
        ((char*)plat)[i] = platform[i];
    }

    /* AT_RANDOM: sixteen bytes glibc reads to seed its stack guard and
     * pointer mangling. Not cryptographically anything - the PIT tick is
     * the only entropy this kernel has - but it has to be *there*, and
     * differ run to run, or every program gets the same canary. */
    sp -= 16;
    uint32_t randbytes = sp;
    uint32_t t = pit_get_ticks();
    for (int i = 0; i < 16; i++) {
        t = t * 1103515245u + 12345u;
        ((uint8_t*)randbytes)[i] = (uint8_t)(t >> 16);
    }

    sp &= ~15u;

    /* Now the vector, laid out downward and then filled in forward. */
    uint32_t aux[] = {
        AT_PHDR,   info->phdr,
        AT_PHENT,  info->phent,
        AT_PHNUM,  info->phnum,
        AT_PAGESZ, PAGE_SIZE,
        /* Where the dynamic linker was loaded, and 0 for a static image.
         * ld-linux.so.2 relocates *itself* using this before it can do
         * anything else, so getting it wrong means the linker faults
         * before it runs a single instruction of the program. */
        AT_BASE,   interp_base,
        AT_FLAGS,  0,
        AT_ENTRY,  info->entry,
        AT_UID,    0,
        AT_EUID,   0,
        AT_GID,    0,
        AT_EGID,   0,
        AT_SECURE, 0,
        AT_CLKTCK, 100,         /* the PIT rate kernel_main() sets */
        AT_HWCAP,  0,
        AT_PLATFORM, plat,
        AT_EXECFN, argv0,
        AT_RANDOM, randbytes,
        AT_NULL,   0,
    };
    uint32_t aux_words = sizeof(aux) / sizeof(aux[0]);

    /* argc, the argv pointers, NULL, the envp pointers, NULL, then the
     * aux pairs. */
    uint32_t words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 + aux_words;
    sp -= words * 4;
    sp &= ~15u;   /* esp must be 16-byte aligned at entry */

    uint32_t* v = (uint32_t*)sp;
    uint32_t k = 0;
    v[k++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) v[k++] = argp[i];
    v[k++] = 0;        /* argv terminator */
    for (int i = 0; i < envc; i++) v[k++] = envpp[i];
    v[k++] = 0;        /* envp terminator */
    for (uint32_t i = 0; i < aux_words; i++) v[k++] = aux[i];

    return sp;
}

int process_load_elf_image(const uint8_t* image, uint32_t size,
                           int argc, const char* const* argv,
                           int envc, const char* const* envp,
                           uint32_t* out_entry, uint32_t* out_esp) {
    if (!elf_is_valid(image, size)) return 0;

    /* Look before loading: an image that wants an interpreter has to be
     * placed alongside it, and a PIE has no address of its own. */
    elf_info_t peek;
    if (!elf_peek(image, size, &peek)) return 0;

    uint32_t bias = peek.interp[0] ? PIE_BASE : 0;
    /* An ET_DYN with no interpreter is a static PIE and still needs a
     * base; elf_load_at ignores the bias for ET_EXEC. */
    if (!bias && ((const Elf32_Ehdr*)image)->e_type == ET_DYN) bias = PIE_BASE;

    elf_info_t info;
    if (!elf_load_at(image, size, bias, &info)) return 0;

    /* If the program named an interpreter, load that too and enter *it*
     * rather than the program. Everything after this point is
     * ld-linux.so.2's job: it maps the shared libraries, relocates them,
     * resolves symbols, and eventually jumps to AT_ENTRY. The kernel's
     * entire contribution to dynamic linking is these few lines plus a
     * correct auxiliary vector. */
    uint32_t start = info.entry;
    uint32_t interp_base = 0;
    if (info.interp[0]) {
        uint32_t isize = 0;
        uint8_t* iimg = process_read_file(info.interp, &isize);
        if (!iimg) {
            terminal_writestring_color("[elf] ", VGA_COLOR_LIGHT_RED);
            terminal_writestring("interpreter not found: ");
            terminal_writestring(info.interp);
            terminal_writestring("\n");
            return 0;
        }
        elf_info_t iinfo;
        int ok = elf_load_at(iimg, isize, INTERP_BASE, &iinfo);
        kfree(iimg);
        if (!ok) {
            terminal_writestring_color("[elf] ", VGA_COLOR_LIGHT_RED);
            terminal_writestring("could not load the interpreter\n");
            return 0;
        }
        interp_base = INTERP_BASE;
        start = iinfo.entry;
    }

    /* An ELF program gets a larger stack than the single page a flat
     * binary makes do with: since Milestone 20 it can clone() threads,
     * and each of those needs somewhere to put a stack of its own out of
     * its mmap arena, but the main thread's own frames also get deeper
     * once there is a real program on top. */
    for (uint32_t va = ELF_STACK_TOP - ELF_STACK_SIZE; va < ELF_STACK_TOP;
         va += PAGE_SIZE) {
        if (paging_get_entry(va) & PAGE_PRESENT) continue;
        uint32_t phys = pmm_alloc_frame();
        if (!phys) break;
        paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        /* Zeroed for the same reason elf.c zeroes a freshly mapped
         * segment page: a frame out of the allocator holds the last
         * program's bytes, and Linux hands a process a stack that reads
         * as zero. glibc's startup walks past the end of the auxiliary
         * vector looking for terminators. */
        kmemset((void*)va, 0, PAGE_SIZE);
    }

    static const char* default_argv[1] = { "program" };
    if (argc < 1 || !argv) { argc = 1; argv = default_argv; }

    *out_entry = start;
    *out_esp = build_initial_stack(ELF_STACK_TOP, argc, argv, envc, envp,
                                   &info, interp_base);
    return 1;
}

/* The environment a program started from the shell is handed. The
 * defaults are small on purpose - these are the variables a Unix program
 * looks for before it has been told anything, and inventing more would be
 * inventing facts - and the shell's `setenv` adds to them. */
#define ENV_STORE_BYTES 512
static char env_store[ENV_STORE_BYTES];
static const char* env_ptr[PROCESS_MAX_ENV];
static int env_count = 0;
static uint32_t env_used = 0;
static int env_ready = 0;

static void env_defaults(void) {
    static const char* const defaults[] = {
        "PATH=/", "USER=root", "TMPDIR=/tmp", "SHELL=/novaris",
    };
    env_count = 0;
    env_used = 0;
    /* Set before the process_env_set() calls below, not after: those call
     * back in here through the `if (!env_ready)` guard, and a flag raised
     * at the end of this function would make that recursion unbounded. */
    env_ready = 1;
    /* HOME follows the disk. Milestone 32: a home directory is where a
     * program puts the things it means to keep, and until there was a
     * disk the only honest answer was "/" - the root of a filesystem that
     * exists until the machine is switched off.
     *
     * It matters most to Wine, whose prefix is $HOME/.wine: hundreds of
     * files that wine.inf builds, a registry it rewrites, and a drive
     * table made of symbolic links. On the ramfs that is a node table
     * away from full and gone at the next boot. On the disk it is an
     * installation. */
    process_env_set(fat32_mounted() ? "HOME=/disk" : "HOME=/");
    for (uint32_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        process_env_set(defaults[i]);
    }
}

int process_env_set(const char* assignment) {
    /* Seed the defaults rather than start from nothing. The old line here
     * was `if (!env_ready) { env_ready = 1; env_count = 0; ... }`, which
     * made the *first* `setenv` in a session silently discard HOME, PATH,
     * USER, TMPDIR and SHELL - so `setenv WINEDEBUG=+module` did not add
     * a variable, it replaced the whole environment with one.
     *
     * Found the way these things are: a Wine run under tracing built its
     * prefix in `/.wine` instead of `/disk/.wine` and nothing else in the
     * transcript explained why. HOME had been thrown away by the command
     * that turned tracing on. */
    if (!env_ready) env_defaults();
    if (!assignment || !*assignment) return 0;

    uint32_t len = kstrlen(assignment) + 1;
    if (len > ENV_STORE_BYTES - env_used) return 0;

    /* Replacing an existing NAME= rather than shadowing it: two entries
     * with the same name is a thing a real environment can hold and a
     * thing nothing sensible reads. The name is everything up to '='. */
    uint32_t namelen = 0;
    while (assignment[namelen] && assignment[namelen] != '=') namelen++;

    int slot = -1;
    for (int i = 0; i < env_count; i++) {
        uint32_t j = 0;
        while (j < namelen && env_ptr[i][j] == assignment[j]) j++;
        if (j == namelen && env_ptr[i][namelen] == '=') { slot = i; break; }
    }
    if (slot < 0) {
        if (env_count >= PROCESS_MAX_ENV) return 0;
        slot = env_count++;
    }

    char* dst = &env_store[env_used];
    for (uint32_t i = 0; i < len; i++) dst[i] = assignment[i];
    env_used += len;
    env_ptr[slot] = dst;
    return 1;
}

int process_env_count(void) {
    if (!env_ready) env_defaults();
    return env_count;
}

const char* process_env_at(int i) {
    if (!env_ready) env_defaults();
    return (i >= 0 && i < env_count) ? env_ptr[i] : 0;
}

void process_env_reset(void) { env_ready = 0; env_defaults(); }

int process_run_elf(const uint8_t* image, uint32_t size,
                    int argc, const char* const* argv) {
    /* Validated before the address space is created, so a file that isn't
     * an ELF doesn't cost a page directory. `image` itself is a kernel
     * heap buffer, which is mapped identically either side of the switch. */
    if (!elf_is_valid(image, size)) return 0;

    int owns_as = process_enter_address_space();

    /* The process structure has to exist before the image is loaded:
     * loading writes the program's own path into it, and since Milestone
     * 29 there is a structure per address space rather than one global. */
    posix_process_begin();
    /* So readlink("/proc/self/exe") can answer, which is how Wine's
     * loader works out where it was installed. argv[0] is the path the
     * program was named by, which is the honest answer and the one execve
     * records for the programs it starts. */
    if (argc >= 1 && argv) posix_set_exe_path(argv[0]);

    uint32_t start = 0, esp = 0;
    if (!env_ready) env_defaults();
    if (!process_load_elf_image(image, size, argc, argv,
                                env_count, env_ptr, &start, &esp)) {
        posix_process_end();
        if (owns_as) process_leave_address_space();
        return 0;
    }

    /* Run as a scheduler task rather than through the older blocking
     * path. That is what makes clone() possible at all: a second thread
     * is simply a second task in this same address space, and
     * scheduler_run_single() blocks its caller exactly the way
     * process_run_user_mode() did. */
    user_active = 1;
    scheduler_run_single("main", start, esp);
    posix_process_end();
    user_active = 0;

    if (owns_as) process_leave_address_space();
    return 1;
}

int process_run_pe(const uint8_t* image, uint32_t size, const char* name,
                   uint32_t* exit_code) {
    /* All the interesting work - loading, imports, the TEB, the Win32 API
     * itself - lives in kernel/win32.c. This wrapper exists so the shell
     * keeps calling process_run_*() uniformly for every binary format. */
    user_active = 1;
    pe_load_result_t result = win32_run_pe(image, size, name, name, 0, exit_code);
    user_active = 0;
    return (int)result;
}

/* --- faults in ring 3 --------------------------------------------------- */

/* Registered with the IDT for the CPU exception vectors. A fault in a
 * user program used to reach idt.c's panic screen and halt the machine,
 * which is the wrong outcome for "the program someone typed `run` on has
 * a bug in it" - and much more so now that the programs in question can
 * be arbitrary .exe files. This kills the program and returns to the
 * shell instead, deferring to the panic path only for faults that really
 * did happen in the kernel. */
static int handle_user_fault(registers_t* regs) {
    if (!user_active) return 0;
    if ((regs->cs & 3) != 3) return 0; /* the kernel itself faulted */

    /* Before anything else, and before the program is told about it at
     * all: the one page fault that is not an error. A write to a page a
     * private file mapping still shares with the file gets that page a
     * copy of its own, and the instruction is simply retried. See
     * PAGE_COW - a Wine client takes hundreds of these. */
    if (regs->int_no == 14 && posix_handle_cow_fault(regs)) return 1;

    /* A Win32 program gets the more detailed, Windows-shaped report. */
    if (win32_process_active()) {
        return win32_handle_user_fault(regs); /* does not return */
    }

    /* A POSIX program that installed a handler for the signal this fault
     * maps to gets it delivered instead of being killed - and, crucially,
     * gets to *return* from the handler, at which point the faulting
     * instruction is retried. That is the pattern Wine is built on: fault
     * on purpose, fix the mapping in the handler, carry on. */
    if (posix_handle_fault_signal(regs, regs->int_no)) return 1;

    char buf[16];
    terminal_writestring("\n");
    terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("User program faulted (exception ");
    ku32_to_dec(regs->int_no, buf);
    terminal_writestring(buf);
    terminal_writestring(") at eip=0x");
    ku32_to_hex(regs->eip, buf, 0, 8);
    terminal_writestring(buf);

    /* For a page fault, which address and what kind - without these the
     * report says a fault happened and nothing about why, which is most
     * of the work. err_code bit 0 = the page was present (a protection
     * violation rather than a missing mapping), bit 1 = it was a write,
     * bit 2 = from ring 3. */
    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        terminal_writestring(" accessing 0x");
        ku32_to_hex(cr2, buf, 0, 8);
        terminal_writestring(buf);
        terminal_writestring(regs->err_code & 2 ? " (write" : " (read");
        terminal_writestring(regs->err_code & 1 ? ", protection)" : ", not mapped)");
    }
    terminal_writestring("\n         Terminating it and returning to the shell.\n");

    /* `user_active` is cleared only on the path that has no scheduler
     * behind it. While the scheduler is running it belongs to the batch,
     * not to this task: clearing it here let the *next* thread's fault
     * fall past this hook and into the kernel's panic handler, which is
     * how a threaded Windows program under Wine took the machine down
     * instead of the program. scheduler_run_until_idle() returns to
     * process_run_elf(), which clears it. */
    if (!scheduler_is_active()) user_active = 0;

    /* Which way out depends on how this program was started, and getting
     * it wrong panics the kernel. Since Milestone 20 an ELF program runs
     * as a scheduler task, so its unwind target is the one
     * scheduler_run_until_idle() saved - not process_run_user_mode()'s
     * kernel_resume_esp, which by then holds a stale value from whatever
     * last used the blocking path. posix_exit_process() picks correctly.
     *
     * This was found by running a real glibc-linked binary, which faults
     * early; the fault report printed and the kernel then page-faulted
     * unwinding into a dead frame. Nothing in the suite caught it because
     * no ELF test program had ever faulted - userland/crash_test.c now
     * does. */
    /* And it takes every thread of the process with it, not just the one
     * that faulted - an unhandled SIGSEGV ends the thread group. Other
     * *processes* keep running, which matters here: when Wine's client
     * crashes, wineserver is still a program in the middle of its own
     * work. */
    posix_exit_process_group(regs->int_no == 14 ? SIGSEGV : SIGILL);
    return 1;
}

void process_install_fault_handler(void) {
    idt_set_user_fault_hook(handle_user_fault);
}
