#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "pit.h"
#include "pmm.h"
#include "process.h"
#include "vfs.h"
#include "blockdev.h"
#include "ata.h"
#include "fat32.h"
#include "kheap.h"
#include "paging.h"
#include "posix.h"
#include "posix_proc.h"
#include "elf.h"
#include "pe.h"
#include "win32.h"
#include "kstring.h"
#include "scheduler.h"
#include "socket.h"
#include "task_a.h"
#include "task_b.h"
#include "task_c.h"
#include "thread_demo.h"

#define CMD_BUFFER_SIZE 128

/* No libc here, so tiny hand-rolled helpers again. */
static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static void print_uint(uint32_t n) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (n == 0) {
        terminal_writestring("0");
        return;
    }
    while (n > 0 && i > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    terminal_writestring(&buf[i]);
}

static void print_hex(uint32_t n, int width) {
    char buf[16];
    ku32_to_hex(n, buf, 0, width);
    terminal_writestring("0x");
    terminal_writestring(buf);
}

/* Reads a file into a kmalloc'd buffer. Returns 0 and prints the reason
 * on failure; the caller owns the buffer.
 *
 * Milestone 32 made this take a path. It used to look only at the root's
 * children, which was honest when the only filesystem was a flat initrd
 * and cost nothing to say. With a disk mounted at /disk it made `sum
 * /disk/.../gdi32.dll` - the exact question "did this file come off the
 * disk intact?" - unanswerable. The bare-name form still works, because
 * the initrd is still flat and every existing use of it passes one. */
static uint8_t* read_file(const char* name, uint32_t* out_size) {
    vfs_node_t* node = vfs_lookup(name);
    if (!node && vfs_root) node = vfs_finddir(vfs_root, name);
    if (node && (node->flags & VFS_DIRECTORY)) node = 0;
    if (!node) {
        terminal_writestring_color("File not found: ", VGA_COLOR_LIGHT_RED);
        terminal_writestring(name);
        terminal_writestring("\n");
        return 0;
    }
    uint8_t* buffer = (uint8_t*)kmalloc(node->length);
    if (!buffer) {
        terminal_writestring_color("Out of memory reading ", VGA_COLOR_LIGHT_RED);
        terminal_writestring(name);
        terminal_writestring("\n");
        return 0;
    }
    vfs_read(node, 0, node->length, buffer);
    *out_size = node->length;
    return buffer;
}

static const char* subsystem_name(uint16_t subsystem) {
    switch (subsystem) {
        case IMAGE_SUBSYSTEM_NATIVE:      return "native";
        case IMAGE_SUBSYSTEM_WINDOWS_GUI: return "Windows GUI";
        case IMAGE_SUBSYSTEM_WINDOWS_CUI: return "Windows console";
        default: return "unknown";
    }
}

/* Called once per imported symbol by pe_visit_imports(), printing each
 * with whether the Win32 layer can actually satisfy it. */
static void print_import(const char* dll, const char* name, uint16_t ordinal,
                          void* ctx) {
    uint32_t* missing = (uint32_t*)ctx;
    int have = win32_have_export(dll, name, ordinal);
    if (!have) (*missing)++;

    terminal_writestring(have ? "    [ok]      " : "    [MISSING] ");
    terminal_writestring(dll);
    terminal_writestring("!");
    if (name) {
        terminal_writestring(name);
    } else {
        terminal_writestring("#");
        print_uint(ordinal);
    }
    terminal_writestring("\n");
}

static void cmd_peinfo(const char* fname) {
    uint32_t size = 0;
    uint8_t* buffer = read_file(fname, &size);
    if (!buffer) return;

    pe_info_t info;
    pe_load_result_t r = pe_inspect(buffer, size, &info);

    if (!info.valid) {
        terminal_writestring_color("Not a PE executable.\n", VGA_COLOR_LIGHT_RED);
        kfree(buffer);
        return;
    }

    terminal_writestring("  machine       ");
    if (info.machine == IMAGE_FILE_MACHINE_I386) terminal_writestring("i386 (32-bit)");
    else if (info.machine == IMAGE_FILE_MACHINE_AMD64) terminal_writestring("x86-64");
    else if (info.machine == IMAGE_FILE_MACHINE_ARM64) terminal_writestring("ARM64");
    else print_hex(info.machine, 4);
    terminal_writestring("\n  subsystem     ");
    terminal_writestring(subsystem_name(info.subsystem));
    terminal_writestring("\n  image base    ");
    print_hex(info.image_base, 8);
    terminal_writestring("\n  image size    ");
    print_uint(info.size_of_image);
    terminal_writestring(" bytes in ");
    print_uint(info.num_sections);
    terminal_writestring(" sections\n  entry point   ");
    print_hex(info.image_base + info.entry_rva, 8);
    terminal_writestring("\n  relocations   ");
    terminal_writestring(info.has_relocs ? "present (can be relocated)"
                                         : "none (must load at its image base)");
    terminal_writestring("\n  imports       ");
    print_uint(info.import_func_count);
    terminal_writestring(" symbol(s) from ");
    print_uint(info.import_dll_count);
    terminal_writestring(" DLL(s)\n");

    if (info.import_func_count > 0) {
        uint32_t missing = 0;
        pe_visit_imports(buffer, size, print_import, &missing);
        terminal_writestring("  ");
        print_uint(info.import_func_count - missing);
        terminal_writestring(" of ");
        print_uint(info.import_func_count);
        terminal_writestring(" imports resolve against the emulated Win32 API.\n");
    }

    terminal_writestring("  verdict       ");
    if (r == PE_OK) {
        terminal_writestring_color("loadable\n", VGA_COLOR_LIGHT_GREEN);
    } else {
        terminal_writestring_color(pe_result_string(r), VGA_COLOR_LIGHT_RED);
        terminal_writestring("\n");
    }
    kfree(buffer);
}

static void cmd_winapi(const char* argument) {
    if (*argument == '\0') {
        terminal_writestring("Emulated Win32 modules (see ROADMAP.md Milestone 10):\n");
        for (uint32_t i = 0; i < win32_module_count(); i++) {
            uint32_t total = win32_module_exports(i);
            if (total == 0) continue; /* an alias for another module */
            terminal_writestring("  ");
            terminal_writestring(win32_module_name(i));
            for (uint32_t p = kstrlen(win32_module_name(i)); p < 16; p++) {
                terminal_writestring(" ");
            }
            print_uint(win32_module_implemented(i));
            terminal_writestring(" implemented / ");
            print_uint(total);
            terminal_writestring(" exported\n");
        }
        terminal_writestring("\nMany more DLL names alias onto these (ucrtbase, the\n");
        terminal_writestring("api-ms-win-crt-* family, kernelbase, ...).\n");
        terminal_writestring("Run 'winapi <module>' to list one module's exports;\n");
        terminal_writestring("'*' marks a stub, '=' marks a variable.\n");
        return;
    }

    for (uint32_t i = 0; i < win32_module_count(); i++) {
        if (streq(win32_module_name(i), argument) && win32_module_exports(i)) {
            win32_list_exports(i, 0);
            return;
        }
    }
    terminal_writestring_color("No such emulated module: ", VGA_COLOR_LIGHT_RED);
    terminal_writestring(argument);
    terminal_writestring("\n(run 'winapi' with no argument for the list)\n");
}

/* Splits "prog a b c" into argv. The storage is static because the
 * pointers have to outlive this function - build_initial_stack() copies
 * the strings onto the program's own stack, but not until later. */
#define SHELL_MAX_ARGS 16
static char argv_store[256];
static const char* argv_ptr[SHELL_MAX_ARGS];

static int split_args(const char* line, const char** argv_out[]) {
    int argc = 0;
    uint32_t w = 0;
    const char* p = line;
    while (*p && argc < SHELL_MAX_ARGS && w < sizeof(argv_store) - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv_ptr[argc++] = &argv_store[w];
        while (*p && *p != ' ' && w < sizeof(argv_store) - 1) {
            argv_store[w++] = *p++;
        }
        argv_store[w++] = '\0';
    }
    *argv_out = argv_ptr;
    return argc;
}

static void cmd_run(const char* line) {
    const char** argv = 0;
    int argc = split_args(line, &argv);
    if (argc < 1) return;
    const char* fname = argv[0];

    uint32_t size = 0;
    uint8_t* buffer = read_file(fname, &size);
    if (!buffer) return;

    if (elf_is_valid(buffer, size)) {
        /* process_run_elf() records argv[0] as the program's own path, so
         * readlink("/proc/self/exe") can answer it - which is how Wine's
         * loader works out where it was installed. */
        if (!process_run_elf(buffer, size, argc, argv)) {
            terminal_writestring_color("Failed to load ELF: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(fname);
            terminal_writestring("\n");
        }
    } else if (pe_is_valid(buffer, size)) {
        uint32_t exit_code = 0;
        pe_load_result_t result =
            (pe_load_result_t)process_run_pe(buffer, size, fname, &exit_code);
        if (result != PE_OK) {
            terminal_writestring_color("Cannot run ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(fname);
            terminal_writestring(": ");
            terminal_writestring(pe_result_string(result));
            terminal_writestring("\n(try 'peinfo ");
            terminal_writestring(fname);
            terminal_writestring("' for the details)\n");
        } else if (exit_code != 0 && !win32_last_run_faulted()) {
            /* A program killed by a fault has already had the whole story
             * printed; its "exit code" is just the exception code. */
            terminal_writestring_color("[win32] ", VGA_COLOR_LIGHT_BROWN);
            terminal_writestring("Program exited with code ");
            print_uint(exit_code);
            terminal_writestring("\n");
        }
    } else {
        process_run_flat_binary(buffer, size);
    }

    kfree(buffer);
    terminal_writestring("Back in the shell.\n");
}

/* --- wine ---------------------------------------------------------------
 *
 * `wine hellowin.exe`, and that is the whole invocation.
 *
 * Milestone 34 made this shorthand for `run wine-preloader wine
 * hellowin.exe`, because that was the only sequence that worked: Wine's
 * files were a heap at the root of the initrd, so the preloader had to be
 * named explicitly and handed the loader explicitly. Milestone 35
 * installed Wine into the OS instead - /usr/bin/wine, /usr/lib/wine,
 * /usr/share/wine - and an installed Wine does that part itself. The
 * wrapper at /usr/bin/wine loads ntdll.so, works out where the real
 * loader is, and re-execs itself through the preloader.
 *
 * So this command is now what it looks like: run /usr/bin/wine, with the
 * arguments the user gave. */
#define WINE_LOADER "/usr/bin/wine"

/* Whether this OS was built with Wine in it. The desktop asks, because
 * double-clicking a .exe means something different in each case: real
 * Wine when it is here, the built-in Win32 layer when it is not. */
int shell_wine_installed(void) {
    return vfs_lookup(WINE_LOADER) != 0;
}

static void cmd_wine(const char* args) {
    static char cmdline[CMD_BUFFER_SIZE + 24];

    if (!*args) {
        terminal_writestring("usage: wine <program.exe> [args]\n");
        terminal_writestring("Runs a Windows program under real Wine. The prefix is $HOME/.wine:\n");
        terminal_writestring("/disk/.wine when a disk is mounted, /root/.wine otherwise.\n");
        return;
    }

    /* An OS built without Wine is a supported thing to have - Wine is a
     * build input, not a vendored tree - and "file not found" three
     * layers down is a much worse answer than this one. */
    if (!vfs_lookup(WINE_LOADER)) {
        terminal_writestring_color("No Wine installed. ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("This OS was built without it;\n"
                             "rebuild with 'make WINE_BUILD=/path/to/a/built/wine'.\n"
                             "The built-in Win32 layer runs a .exe directly: "
                             "try 'run <program.exe>'.\n");
        return;
    }

    uint32_t w = 0;
    const char* prefix = WINE_LOADER " ";
    while (*prefix && w < sizeof(cmdline) - 1) cmdline[w++] = *prefix++;
    while (*args && w < sizeof(cmdline) - 1) cmdline[w++] = *args++;
    cmdline[w] = '\0';

    cmd_run(cmdline);
}

/* --- vmtest -------------------------------------------------------------
 *
 * Milestone 14's evidence. Two address spaces, one virtual address, two
 * different values living there at the same time - which is exactly the
 * thing a single flat address space cannot do, and exactly what
 * everything from here on (threads, mmap, Wine) is built on.
 *
 * The test is arranged so that the interesting failure modes are loud
 * rather than quiet:
 *
 *   - It runs with interrupts on. While a user address space is loaded,
 *     the timer and keyboard IRQs keep firing and running kernel code on
 *     the kernel stack, and every line it prints goes through the
 *     framebuffer console and the kernel heap. If the shared kernel half
 *     were not genuinely identical in all three directories, the machine
 *     would triple-fault here instead of printing.
 *   - It writes through the *virtual* address in each space rather than
 *     poking the physical frames, so the mapping is what is being
 *     checked, not the bookkeeping.
 *   - It checks the address is unmapped back in the kernel's own space,
 *     confirming the private half really is private.
 */
#define VMTEST_VADDR 0x30000000u

static void vmtest_step(const char* text) {
    terminal_writestring_color("  [vm] ", VGA_COLOR_LIGHT_CYAN);
    terminal_writestring(text);
}

static void cmd_vmtest(void) {
    uint32_t kernel_as = paging_kernel_address_space();

    terminal_writestring("Per-process address spaces (ROADMAP Milestone 14)\n");
    vmtest_step("kernel page directory at ");
    print_hex(kernel_as, 8);
    terminal_writestring("\n");

    /* The shared set, as ranges of 4MB directory slots. Worth printing:
     * which entries are shared decides where a process may put its own
     * mappings, and two of them (the framebuffer, the initrd) land
     * wherever the bootloader chose this boot. */
    vmtest_step("shared kernel directory entries:");
    uint32_t i = paging_next_global_pde(0);
    while (i < 1024) {
        uint32_t run_end = i;
        while (run_end < 1024 && paging_next_global_pde(run_end) == run_end) run_end++;
        terminal_writestring(" ");
        print_uint(i);
        if (run_end - 1 != i) {
            terminal_writestring("-");
            print_uint(run_end - 1);
        }
        terminal_writestring("(");
        print_hex(i << 22, 8);
        terminal_writestring(")");
        i = paging_next_global_pde(run_end);
    }
    terminal_writestring("\n");

    const char* clash = paging_region_conflict(VMTEST_VADDR,
                                               VMTEST_VADDR + 0x1000);
    if (clash) {
        terminal_writestring("  test address is reserved by: ");
        terminal_writestring(clash);
        terminal_writestring(" - aborting\n");
        return;
    }

    uint32_t as1 = paging_create_address_space();
    uint32_t as2 = paging_create_address_space();
    if (!as1 || !as2) {
        terminal_writestring("  could not create two address spaces (out of memory)\n");
        if (as1) paging_destroy_address_space(as1);
        if (as2) paging_destroy_address_space(as2);
        return;
    }
    vmtest_step("created two more: ");
    print_hex(as1, 8);
    terminal_writestring(" and ");
    print_hex(as2, 8);
    terminal_writestring("\n");

    /* One frame each, mapped at the *same* virtual address, written into
     * the other address space's tables through the foreign window - no
     * CR3 switching involved yet. */
    uint32_t f1 = pmm_alloc_frame();
    uint32_t f2 = pmm_alloc_frame();
    if (!f1 || !f2 ||
        !paging_map_page_in(as1, VMTEST_VADDR, f1,
                            PAGE_PRESENT | PAGE_RW | PAGE_USER) ||
        !paging_map_page_in(as2, VMTEST_VADDR, f2,
                            PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
        terminal_writestring("  mapping failed (out of memory)\n");
        paging_destroy_address_space(as1);
        paging_destroy_address_space(as2);
        return;
    }
    vmtest_step("mapped ");
    print_hex(VMTEST_VADDR, 8);
    terminal_writestring(" -> frame ");
    print_hex(f1, 8);
    terminal_writestring(" in the first, ");
    print_hex(f2, 8);
    terminal_writestring(" in the second\n");

    /* Now switch into each and write through the virtual address. */
    volatile uint32_t* p = (volatile uint32_t*)VMTEST_VADDR;

    paging_switch_address_space(as1);
    *p = 0x11111111u;
    paging_switch_address_space(as2);
    *p = 0x22222222u;
    paging_switch_address_space(kernel_as);
    vmtest_step("wrote 0x11111111 in the first, 0x22222222 in the second\n");

    /* Read both back. Values are collected before printing so the console
     * (kernel heap, framebuffer) is only touched from the kernel's own
     * address space - not because it would fail otherwise, but so a
     * failure here can only mean the mapping. */
    paging_switch_address_space(as1);
    uint32_t v1 = *p;
    paging_switch_address_space(as2);
    uint32_t v2 = *p;
    paging_switch_address_space(kernel_as);

    vmtest_step("read back ");
    print_hex(v1, 8);
    terminal_writestring(" and ");
    print_hex(v2, 8);
    terminal_writestring(v1 == 0x11111111u && v2 == 0x22222222u
                             ? "  <- isolated\n"
                             : "  <- WRONG, they are not isolated\n");

    /* And the same address in the kernel's own space, which never had it
     * mapped. Checked through the page tables rather than by
     * dereferencing, since dereferencing it is precisely what should
     * fault. */
    vmtest_step("same address in the kernel's space: ");
    terminal_writestring(paging_get_entry(VMTEST_VADDR) & PAGE_PRESENT
                             ? "MAPPED - leaked\n"
                             : "unmapped, as it should be\n");

    /* Prove the kernel half is shared rather than copied per space: the
     * heap's directory entry must be the identical physical page table in
     * all three, or an interrupt taken on as1 would have died above. */
    uint32_t kern_pte = paging_get_entry(KHEAP_VIRTUAL_BASE);
    uint32_t as1_pte = paging_get_entry_in(as1, KHEAP_VIRTUAL_BASE);
    uint32_t as2_pte = paging_get_entry_in(as2, KHEAP_VIRTUAL_BASE);
    vmtest_step("kernel heap page at ");
    print_hex(KHEAP_VIRTUAL_BASE, 8);
    terminal_writestring(kern_pte == as1_pte && kern_pte == as2_pte
                             ? " is the same frame in all three\n"
                             : " DIFFERS between address spaces\n");

    if (paging_kernel_pde_violated()) {
        terminal_writestring("  WARNING: a kernel page-directory entry was added\n");
        terminal_writestring("           after the address space was frozen - some\n");
        terminal_writestring("           address spaces are missing a kernel mapping.\n");
    }

    uint32_t free_before = pmm_free_frames();
    paging_destroy_address_space(as1);
    paging_destroy_address_space(as2);
    vmtest_step("destroyed both; frames reclaimed: ");
    print_uint(pmm_free_frames() - free_before);
    terminal_writestring("\n");
    terminal_writestring("vmtest done - the kernel is still running, on its own "
                         "page directory.\n");
}

/* --- threadtest ---------------------------------------------------------
 *
 * Milestone 16's evidence for threads, as opposed to processes.
 *
 * `multitask` shows three tasks that cannot see each other - three page
 * directories, same virtual address, three different physical pages. This
 * shows the opposite and equally necessary half: two tasks that *can* see
 * each other, because they share one directory.
 *
 * The shell owns the address space here rather than the scheduler, for a
 * specific reason: the counter the two threads increment lives in the
 * image, and if a task owned the directory the reaper would destroy it -
 * and the counter with it - before there was anything left to read. So
 * the shell creates the address space, maps the image itself, spawns two
 * threads that own no image and no directory, and reads the shared word
 * back afterwards while still standing in that address space.
 */
#define THREAD_DEMO_BASE   0x51000000u
#define THREAD_DEMO_STACK0 0x51100000u
#define THREAD_DEMO_STACK1 0x51200000u

static void cmd_threadtest(void) {
    terminal_writestring("Two threads in ONE address space (ROADMAP Milestone 16)\n");

    if (!process_enter_address_space()) {
        terminal_writestring_color("Could not create an address space.\n",
                                   VGA_COLOR_LIGHT_RED);
        return;
    }

    /* One image, mapped once. Both threads run out of these same pages. */
    if (!process_map_image(THREAD_DEMO_BASE, thread_demo_bin,
                           thread_demo_bin_len)) {
        terminal_writestring_color("Out of memory mapping the demo image.\n",
                                   VGA_COLOR_LIGHT_RED);
        process_leave_address_space();
        return;
    }

    int t0 = scheduler_spawn_thread("thread0", THREAD_DEMO_BASE,
                                    THREAD_DEMO_STACK0);
    int t1 = scheduler_spawn_thread("thread1",
                                    THREAD_DEMO_BASE + THREAD_DEMO_THREAD1_ENTRY,
                                    THREAD_DEMO_STACK1);
    if (t0 < 0 || t1 < 0) {
        terminal_writestring_color("Could not spawn both threads.\n",
                                   VGA_COLOR_LIGHT_RED);
        process_leave_address_space();
        return;
    }

    terminal_writestring("  page directory ");
    print_hex(process_current_address_space(), 8);
    terminal_writestring(", one image at ");
    print_hex(THREAD_DEMO_BASE, 8);
    terminal_writestring("\n  thread PIDs ");
    print_uint((uint32_t)t0);
    terminal_writestring(" and ");
    print_uint((uint32_t)t1);
    terminal_writestring(", separate stacks at ");
    print_hex(THREAD_DEMO_STACK0, 8);
    terminal_writestring(" and ");
    print_hex(THREAD_DEMO_STACK1, 8);
    terminal_writestring("\n\n");

    scheduler_run_until_idle();

    /* Both threads have exited and the reaper has taken their stacks, but
     * the image is the shell's, so the counter is still here to read. */
    uint32_t counter = *(volatile uint32_t*)(THREAD_DEMO_BASE +
                                             THREAD_DEMO_COUNTER);
    uint32_t expected = THREAD_DEMO_ITERATIONS * 2;

    terminal_writestring("\n  shared counter at ");
    print_hex(THREAD_DEMO_BASE + THREAD_DEMO_COUNTER, 8);
    terminal_writestring(" reads ");
    print_uint(counter);
    terminal_writestring(", expected ");
    print_uint(expected);
    terminal_writestring(counter == expected
                             ? "\n  both threads incremented the SAME word - "
                               "they share one address space\n"
                             : "\n  WRONG - the threads did not share memory\n");

    process_leave_address_space();
    terminal_writestring("threadtest done.\n");
}

static void print_prompt(void) {
    terminal_writestring_color("novaris", VGA_COLOR_LIGHT_CYAN);
    terminal_writestring_color("> ", VGA_COLOR_LIGHT_GREY);
}

static void run_command(char* line) {
    while (*line == ' ') line++; /* skip leading spaces */
    if (*line == '\0') return;   /* empty line: just re-prompt */

    if (streq(line, "help")) {
        terminal_writestring("Available commands:\n");
        terminal_writestring("  help    - show this list\n");
        terminal_writestring("  clear   - clear the screen\n");
        terminal_writestring("  about   - about Novaris\n");
        terminal_writestring("  uptime  - timer ticks since boot\n");
        terminal_writestring("  meminfo - physical memory frame usage\n");
        terminal_writestring("  ls [dir] / cat / cp / rm / mkdir - the filesystem,\n");
        terminal_writestring("            writable since Milestone 26\n");
        terminal_writestring("  df / sync / ln -s <target> <link> - the disk at /disk,\n");
        terminal_writestring("            a real FAT32 volume since Milestone 32\n");
        terminal_writestring("  sockinfo - Unix socket traffic (Milestone 28)\n");
        terminal_writestring("  futexinfo - futex waiting: how many waits blocked\n");
        terminal_writestring("            rather than spun (Milestone 25)\n");
        terminal_writestring("  threadtest - two threads sharing one address space,\n");
        terminal_writestring("            incrementing one shared counter (Milestone 16)\n");
        terminal_writestring("  run <file> [args] - run a program, with a real argv\n");
        terminal_writestring("  wine <prog.exe> [args] - run a Windows program under\n");
        terminal_writestring("            real Wine (novaris-wine.iso only)\n");
        terminal_writestring("  strace  - the same, with every syscall logged\n");
        terminal_writestring("  setenv NAME=VALUE, env - the environment programs are given\n");
        terminal_writestring("  ps      - the process table (fork gave it more than one row)\n");
        terminal_writestring("  sum <file> - size, checksum and first bytes, for comparing with a host\n");
        terminal_writestring("  vmtest  - prove per-process address spaces work:\n");
        terminal_writestring("            two page directories, one virtual address,\n");
        terminal_writestring("            two different contents (ROADMAP Milestone 14)\n");
        terminal_writestring("  ls      - list files on the initrd\n");
        terminal_writestring("  cat     - print a file, e.g. cat hello.txt\n");
        terminal_writestring("  run     - run a program: flat binary, ELF, or a real\n");
        terminal_writestring("            Windows .exe, e.g. run hellowin.exe\n");
        terminal_writestring("  peinfo  - inspect a .exe: headers, and which of its\n");
        terminal_writestring("            imports the Win32 layer can satisfy\n");
        terminal_writestring("  winapi  - list the emulated Win32 modules and APIs\n");
        terminal_writestring("  runuser - run the demo ring-3 user program\n");
        terminal_writestring("  multitask - run 3 demo programs concurrently under\n");
        terminal_writestring("            real preemptive scheduling (see ROADMAP.md\n");
        terminal_writestring("            Milestone 9) - watch their output interleave\n");
        terminal_writestring("  echo    - print text back\n");
        terminal_writestring("\nThis shell runs inside the desktop's Terminal\n");
        terminal_writestring("window. Press the Windows key for Start, Alt-Tab to\n");
        terminal_writestring("switch windows, Alt-F4 to close one, and Win-Left or\n");
        terminal_writestring("Win-Right to snap. Dragging a window to a screen edge\n");
        terminal_writestring("snaps it too; the taskbar does the rest.\n");
    } else if (streq(line, "clear")) {
        console_clear();
    } else if (streq(line, "about")) {
        terminal_writestring_color("Novaris", VGA_COLOR_LIGHT_CYAN);
        terminal_writestring(" -- a hobby OS kernel. See ROADMAP.md for progress.\n");
    } else if (streq(line, "uptime")) {
        terminal_writestring("Ticks since boot: ");
        print_uint(pit_get_ticks());
        terminal_writestring("\n");
    } else if (starts_with(line, "sum ")) {
        /* A checksum and the first bytes of a file, so "does the kernel
         * serve this file's contents correctly?" can be answered against
         * the host rather than argued about. Written while chasing a
         * fault deep inside Wine's locale tables, where the question was
         * exactly that. */
        uint32_t size = 0;
        uint8_t* buf = read_file(line + 4, &size);
        if (buf) {
            uint32_t h = 2166136261u;      /* FNV-1a, 32-bit */
            for (uint32_t i = 0; i < size; i++) {
                h = (h ^ buf[i]) * 16777619u;
            }
            terminal_writestring("  bytes ");
            print_uint(size);
            terminal_writestring("  fnv1a ");
            print_hex(h, 8);
            terminal_writestring("\n  head ");
            for (uint32_t i = 0; i < 16 && i < size; i++) {
                print_hex(buf[i], 2);
                terminal_writestring(" ");
            }
            terminal_writestring("\n");
            kfree(buf);
        }
    } else if (streq(line, "ps")) {
        /* What Milestone 29 made possible to ask: there is more than one
         * process now, and they outlive each other. A zombie is listed
         * with its status because that is a real state - a child nobody
         * has waited for is still in the table. */
        terminal_writestring("  PID  PPID  STATE     PROGRAM\n");
        for (int i = 0; ; i++) {
            if (i >= POSIX_MAX_PROCS) break;
            posix_proc_t* p = posix_proc_at(i);
            if (!p) continue;
            terminal_writestring("  ");
            print_uint((uint32_t)p->pid);
            terminal_writestring("   ");
            print_uint((uint32_t)p->ppid);
            terminal_writestring("    ");
            if (p->exited) {
                terminal_writestring("zombie(");
                print_uint((uint32_t)((p->exit_status >> 8) & 0xff));
                terminal_writestring(")  ");
            } else {
                terminal_writestring("running   ");
            }
            terminal_writestring(p->exe_path);
            terminal_writestring("\n");
        }
        if (posix_proc_count() == 0) {
            terminal_writestring("  (nothing running - `run` a program)\n");
        }
    } else if (starts_with(line, "setenv ")) {
        /* The environment every program started from here is handed.
         * Wine reads more of it than most - WINEDEBUG turns on its own
         * tracing, WINEPREFIX says where its installation is - so being
         * able to set one is the difference between guessing at what it
         * is doing and being told. */
        if (!process_env_set(line + 7)) {
            terminal_writestring_color("Environment full or malformed.\n",
                                       VGA_COLOR_LIGHT_RED);
        }
    } else if (streq(line, "env")) {
        for (int i = 0; i < process_env_count(); i++) {
            terminal_writestring("  ");
            terminal_writestring(process_env_at(i));
            terminal_writestring("\n");
        }
    } else if (streq(line, "meminfo")) {
        terminal_writestring("Physical frames: ");
        print_uint(pmm_free_frames());
        terminal_writestring(" free / ");
        print_uint(pmm_total_frames());
        terminal_writestring(" total (");
        print_uint(pmm_free_frames() * 4);
        terminal_writestring("KB free)\n");
        /* The kernel heap, which since Milestone 32 is where a mapped
         * file's bytes live - so "how much of it is gone" is the question
         * behind every "failed to load library" Wine prints. */
        uint32_t hused = 0, htotal = 0;
        kheap_stats(&hused, &htotal);
        terminal_writestring("Kernel heap: ");
        print_uint(hused / 1024u);
        terminal_writestring("KB used of ");
        print_uint(htotal / 1024u);
        terminal_writestring("KB mapped, ceiling ");
        print_uint(KHEAP_MAX_SIZE / (1024u * 1024u));
        terminal_writestring("MB\n");
        terminal_writestring("VFS nodes: ");
        print_uint(vfs_node_count());
        terminal_writestring(", file bytes held: ");
        print_uint(vfs_heap_bytes() / 1024u);
        terminal_writestring("KB\n");
        if (pmm_double_frees()) {
            /* Never expected, and worth shouting about: a frame freed
             * twice may already belong to somebody else, and the damage
             * surfaces in whatever runs next rather than here. */
            terminal_writestring_color("  double frees: ", VGA_COLOR_LIGHT_RED);
            print_uint(pmm_double_frees());
            terminal_writestring("\n");
        }
    } else if (streq(line, "futexinfo") || streq(line, "futexinfo reset")) {
        /* Waiting getting cheaper is not visible in any program's output,
         * so the kernel has to be asked. `waits` counts FUTEX_WAIT calls
         * that actually had to wait; `blocked` how many of those slept
         * instead of spinning. Under Milestone 20's retry loop every wait
         * re-executed the syscall every slice, so `waits` ran into the
         * thousands for the same program. */
        uint32_t w, b, r, idle, k;
        posix_futex_stats(&w, &b, &r, &idle, &k);
        terminal_writestring("Futex waits: ");
        print_uint(w);
        terminal_writestring(" (");
        print_uint(b);
        terminal_writestring(" blocked, ");
        print_uint(r);
        terminal_writestring(" spun, ");
        print_uint(idle);
        terminal_writestring(" idled), woken: ");
        print_uint(k);
        terminal_writestring(", blocked now: ");
        print_uint((uint32_t)scheduler_blocked_count());
        terminal_writestring("\n");
        if (line[9]) posix_futex_stats_reset();
    } else if (streq(line, "sockinfo")) {
        terminal_writestring("Unix sockets open: ");
        print_uint(socket_open_count());
        terminal_writestring(", bytes carried: ");
        print_uint(socket_bytes_moved());
        terminal_writestring(", descriptors passed: ");
        print_uint(socket_fds_passed());
        terminal_writestring("\n");
    } else if (streq(line, "threadtest")) {
        cmd_threadtest();
    } else if (streq(line, "vmtest")) {
        cmd_vmtest();
    } else if (streq(line, "ls") || starts_with(line, "ls ")) {
        /* Since Milestone 26 the tree has real directories, so `ls` takes
         * a path and marks them. */
        const char* where = line + 2;
        while (*where == ' ') where++;
        vfs_node_t* dir = *where ? vfs_lookup(where) : vfs_root;
        if (!vfs_root) {
            terminal_writestring("No filesystem mounted.\n");
        } else if (!dir) {
            terminal_writestring_color("Not found: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(where);
            terminal_writestring("\n");
        } else if (!(dir->flags & VFS_DIRECTORY)) {
            terminal_writestring_color("Not a directory: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(where);
            terminal_writestring("\n");
        } else {
            uint32_t idx = 0;
            vfs_node_t* n;
            while ((n = vfs_readdir(dir, idx))) {
                terminal_writestring("  ");
                terminal_writestring(n->name);
                if (n->flags & VFS_DIRECTORY) {
                    terminal_writestring("/  (directory)\n");
                } else if (n->flags & VFS_SYMLINK) {
                    /* Shown the way `ls -l` shows one, because a link that
                     * looks like an ordinary file is a link nobody
                     * notices is there. */
                    char target[VFS_SYMLINK_MAX];
                    int32_t tn = vfs_readlink(n, target, sizeof(target) - 1);
                    target[tn > 0 ? tn : 0] = '\0';
                    terminal_writestring(" -> ");
                    terminal_writestring(target);
                    terminal_writestring("\n");
                } else {
                    terminal_writestring("  (");
                    print_uint(n->length);
                    terminal_writestring(" bytes)\n");
                }
                idx++;
            }
            if (idx == 0) terminal_writestring("(empty)\n");
        }
    } else if (starts_with(line, "mkdir")) {
        const char* p = line + 5;
        while (*p == ' ') p++;
        const char* leaf = 0;
        vfs_node_t* dir = *p ? vfs_resolve_parent(p, &leaf) : 0;
        if (!*p) {
            terminal_writestring("usage: mkdir <path>\n");
        } else if (!dir || !vfs_create(dir, leaf, VFS_DIRECTORY)) {
            terminal_writestring_color("mkdir failed: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(p);
            terminal_writestring("\n");
        }
    } else if (starts_with(line, "ln -s ")) {
        /* Milestone 32. `ln -s target name` - the shell's way of proving
         * that the symlinks Wine's DOS drive table is built out of are
         * real and survive a reboot. */
        char target[VFS_SYMLINK_MAX], linkpath[128];
        const char* p = line + 6;
        while (*p == ' ') p++;
        uint32_t i = 0;
        while (*p && *p != ' ' && i < sizeof(target) - 1) target[i++] = *p++;
        target[i] = '\0';
        while (*p == ' ') p++;
        i = 0;
        while (*p && i < sizeof(linkpath) - 1) linkpath[i++] = *p++;
        linkpath[i] = '\0';

        const char* leaf = 0;
        vfs_node_t* dir = linkpath[0] ? vfs_resolve_parent(linkpath, &leaf) : 0;
        if (!target[0] || !linkpath[0]) {
            terminal_writestring("usage: ln -s <target> <linkpath>\n");
        } else if (!dir || vfs_symlink(dir, leaf, target) != 0) {
            terminal_writestring_color("ln failed: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(linkpath);
            terminal_writestring("\n");
        } else {
            terminal_writestring("linked ");
            terminal_writestring(linkpath);
            terminal_writestring(" -> ");
            terminal_writestring(target);
            terminal_writestring("\n");
        }
    } else if (streq(line, "df")) {
        /* Milestone 32. What is on the disk, and how much of it is left.
         * The block counters are here rather than in `meminfo` because
         * they answer a question nothing else can: whether a file that
         * appeared instantly came off the disk or out of the node cache. */
        if (!blockdev_count()) {
            terminal_writestring("No block devices. Boot with -drive to attach one.\n");
        }
        for (uint32_t i = 0; i < blockdev_count(); i++) {
            blockdev_t* d = blockdev_get(i);
            if (!d) continue;
            terminal_writestring("  ");
            terminal_writestring(d->name);
            terminal_writestring("  ");
            print_uint(d->sectors / 2048u);
            terminal_writestring("MB  ");
            terminal_writestring(ata_model(i));
            terminal_writestring("\n");
        }
        if (fat32_mounted()) {
            uint32_t total = 0, freec = 0;
            fat32_space(&total, &freec);
            uint32_t per_mb = 1024u * 1024u / fat32_cluster_bytes();
            terminal_writestring("  /disk  FAT32 on ");
            terminal_writestring(fat32_device_name());
            terminal_writestring(" label \"");
            terminal_writestring(fat32_volume_label());
            terminal_writestring("\"\n         ");
            print_uint((total - freec) / per_mb);
            terminal_writestring("MB used, ");
            print_uint(freec / per_mb);
            terminal_writestring("MB free, ");
            print_uint(fat32_cluster_bytes());
            terminal_writestring(" bytes/cluster\n");
        } else {
            terminal_writestring("  /disk  nothing mounted\n");
        }
        uint32_t reads = 0, writes = 0;
        blockdev_stats(&reads, &writes);
        terminal_writestring("  sectors read ");
        print_uint(reads);
        terminal_writestring(", written ");
        print_uint(writes);
        terminal_writestring("\n");
    } else if (starts_with(line, "symlinks")) {
        /* Milestone 32. Symbolic links decide which of two startup paths
         * Wine takes - with them it finds its DOS drives and does what a
         * real installation does; without them it falls back. Being able
         * to try both on one boot is worth a command. */
        const char* p = line + 8;
        while (*p == ' ') p++;
        if (streq(p, "on")) posix_set_symlinks(1);
        else if (streq(p, "off")) posix_set_symlinks(0);
        else if (*p) { terminal_writestring("usage: symlinks [on|off]\n"); }
        terminal_writestring("symlink() is ");
        terminal_writestring(posix_symlinks_enabled() ? "enabled\n"
                                                      : "refused (-EPERM)\n");
    } else if (streq(line, "sync")) {
        /* Pushes the one thing that is not already written through: the
         * FAT sector the allocator is working in. Worth having as a
         * command because "reboot and see whether it is still there" is
         * the only real test of a filesystem, and it should not depend on
         * having been lucky about cache eviction. */
        if (fat32_sync() == 0) {
            terminal_writestring("synced\n");
        } else {
            terminal_writestring_color("sync failed\n", VGA_COLOR_LIGHT_RED);
        }
    } else if (starts_with(line, "rm ")) {
        const char* p = line + 2;
        while (*p == ' ') p++;
        const char* leaf = 0;
        vfs_node_t* dir = vfs_resolve_parent(p, &leaf);
        vfs_node_t* victim = dir ? vfs_finddir(dir, leaf) : 0;
        int32_t r = victim
            ? vfs_unlink(dir, leaf, (victim->flags & VFS_DIRECTORY) ? 1 : 0)
            : -1;
        if (r != 0) {
            terminal_writestring_color("rm failed: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(p);
            terminal_writestring("\n");
        }
    } else if (starts_with(line, "cp ")) {
        /* Two arguments, split on the first space after the command. */
        char src[128], dst[128];
        const char* p = line + 2;
        while (*p == ' ') p++;
        uint32_t i = 0;
        while (*p && *p != ' ' && i < sizeof(src) - 1) src[i++] = *p++;
        src[i] = '\0';
        while (*p == ' ') p++;
        i = 0;
        while (*p && i < sizeof(dst) - 1) dst[i++] = *p++;
        dst[i] = '\0';

        vfs_node_t* from = src[0] ? vfs_lookup(src) : 0;
        if (!src[0] || !dst[0]) {
            terminal_writestring("usage: cp <src> <dst>\n");
        } else if (!from || (from->flags & VFS_DIRECTORY)) {
            terminal_writestring_color("cp: cannot read ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(src);
            terminal_writestring("\n");
        } else {
            const char* leaf = 0;
            vfs_node_t* dir = vfs_resolve_parent(dst, &leaf);
            vfs_node_t* to = dir ? vfs_finddir(dir, leaf) : 0;
            if (dir && !to) to = vfs_create(dir, leaf, VFS_FILE);
            uint8_t* buf = from->length ? (uint8_t*)kmalloc(from->length) : 0;
            if (!to || (from->length && !buf)) {
                terminal_writestring_color("cp failed\n", VGA_COLOR_LIGHT_RED);
            } else {
                uint32_t got = vfs_read(from, 0, from->length, buf);
                vfs_truncate(to, 0);
                if (got) vfs_write(to, 0, got, buf);
                terminal_writestring("copied ");
                print_uint(got);
                terminal_writestring(" bytes to ");
                terminal_writestring(dst);
                terminal_writestring("\n");
            }
            if (buf) kfree(buf);
        }
    } else if (starts_with(line, "cat")) {
        const char* fname = line + 3;
        while (*fname == ' ') fname++;
        vfs_node_t* n = *fname ? vfs_lookup(fname) : 0;
        if (n && (n->flags & VFS_DIRECTORY)) n = 0;
        if (*fname == '\0') {
            terminal_writestring("usage: cat <filename>\n");
        } else if (!n) {
            terminal_writestring_color("File not found: ", VGA_COLOR_LIGHT_RED);
            terminal_writestring(fname);
            terminal_writestring("\n");
        } else {
            uint8_t* buf = (uint8_t*)kmalloc(n->length + 1);
            uint32_t r = vfs_read(n, 0, n->length, buf);
            buf[r] = '\0';
            terminal_writestring((char*)buf);
            terminal_writestring("\n");
            kfree(buf);
        }
    } else if (streq(line, "runuser")) {
        process_run_demo_user_program();
        terminal_writestring("Back in the shell.\n");
    } else if (starts_with(line, "peinfo")) {
        const char* fname = line + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            terminal_writestring("usage: peinfo <filename.exe>\n");
        } else {
            cmd_peinfo(fname);
        }
    } else if (starts_with(line, "winapi")) {
        const char* argument = line + 6;
        while (*argument == ' ') argument++;
        cmd_winapi(argument);
    } else if (starts_with(line, "strace")) {
        /* `run` with every syscall logged. Added in Milestone 22 to
         * compare a dynamic linker's behaviour here against the same
         * program's strace on Linux - reading two traces side by side
         * beats reasoning about which of a hundred calls differs. */
        const char* fname = line + 6;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            terminal_writestring("usage: strace <filename> [args]\n");
        } else {
            posix_set_trace(1);
            cmd_run(fname);
            posix_set_trace(0);
        }
    } else if (starts_with(line, "wine")) {
        const char* p = line + 4;
        while (*p == ' ') p++;
        cmd_wine(p);
    } else if (starts_with(line, "run")) {
        const char* fname = line + 3;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            terminal_writestring("usage: run <filename> [args]\n");
        } else {
            cmd_run(fname);
        }
    } else if (streq(line, "multitask")) {
        terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_GREEN);
        terminal_writestring("Spawning 3 demo processes under the preemptive scheduler...\n");

        /* The same load address and the same stack address for all three,
         * which through Milestone 15 would have meant three tasks
         * scribbling over each other's code. Since Milestone 16 each gets
         * its own page directory, so 0x50000000 resolves to a different
         * physical page in each - "concurrent" now means isolated address
         * spaces, not merely distinct addresses. */
        int pa = scheduler_spawn_process("task_a", task_a_bin, task_a_bin_len,
                                          0x50000000u, 0x50100000u);
        int pb = scheduler_spawn_process("task_b", task_b_bin, task_b_bin_len,
                                          0x50000000u, 0x50100000u);
        int pc = scheduler_spawn_process("task_c", task_c_bin, task_c_bin_len,
                                          0x50000000u, 0x50100000u);

        if (pa < 0 || pb < 0 || pc < 0) {
            terminal_writestring_color("Failed to spawn one or more demo processes.\n", VGA_COLOR_LIGHT_RED);
        } else {
            terminal_writestring("PIDs: ");
            print_uint((uint32_t)pa);
            terminal_writestring(", ");
            print_uint((uint32_t)pb);
            terminal_writestring(", ");
            print_uint((uint32_t)pc);
            terminal_writestring(" - if you see their tags interleaved below\n");
            terminal_writestring("rather than grouped, the timer is really preempting them:\n\n");
            scheduler_run_until_idle();
            terminal_writestring("\n");
            terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_GREEN);
            terminal_writestring("All demo processes exited. Back in the shell.\n");
        }
    } else if (starts_with(line, "echo")) {
        const char* rest = line + 4;
        while (*rest == ' ') rest++;
        terminal_writestring(rest);
        terminal_writestring("\n");
    } else {
        terminal_writestring_color("Unknown command: ", VGA_COLOR_LIGHT_RED);
        terminal_writestring(line);
        terminal_writestring("\n(type 'help' for a list)\n");
    }
}

/* The line being edited. It's file-scope rather than a local of a loop
 * because the shell no longer owns a loop - see shell.h. */
static char line_buffer[CMD_BUFFER_SIZE];
static uint32_t line_len;

void shell_init(void) {
    line_len = 0;
    terminal_writestring("\nType 'help' for a list of commands.\n");
    print_prompt();
}

void shell_feed_char(char c) {
    if (c == '\n') {
        terminal_putchar('\n');
        line_buffer[line_len] = '\0';
        run_command(line_buffer);
        line_len = 0;
        print_prompt();
    } else if (c == '\b') {
        if (line_len > 0) {
            line_len--;
            terminal_backspace();
        }
    } else if (line_len < CMD_BUFFER_SIZE - 1) {
        line_buffer[line_len++] = c;
        terminal_putchar(c);
    }
}

void shell_run_line(const char* line) {
    /* Abandon whatever was half-typed, echo the command, then run it
     * through the same path a typed line takes. */
    line_len = 0;
    while (*line) shell_feed_char(*line++);
    shell_feed_char('\n');
}

void shell_run(void) {
    shell_init();
    for (;;) shell_feed_char(keyboard_getchar());
}
