#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "pit.h"
#include "pmm.h"
#include "process.h"
#include "vfs.h"
#include "kheap.h"
#include "elf.h"
#include "pe.h"
#include "win32.h"
#include "kstring.h"
#include "scheduler.h"
#include "task_a.h"
#include "task_b.h"
#include "task_c.h"

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

/* Reads a file off the initrd into a kmalloc'd buffer. Returns 0 and
 * prints the reason on failure; the caller owns the buffer. */
static uint8_t* read_file(const char* name, uint32_t* out_size) {
    vfs_node_t* node = vfs_root ? vfs_finddir(vfs_root, name) : 0;
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

static void cmd_run(const char* fname) {
    uint32_t size = 0;
    uint8_t* buffer = read_file(fname, &size);
    if (!buffer) return;

    if (elf_is_valid(buffer, size)) {
        if (!process_run_elf(buffer, size)) {
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
    } else if (streq(line, "meminfo")) {
        terminal_writestring("Physical frames: ");
        print_uint(pmm_free_frames());
        terminal_writestring(" free / ");
        print_uint(pmm_total_frames());
        terminal_writestring(" total (");
        print_uint(pmm_free_frames() * 4);
        terminal_writestring("KB free)\n");
    } else if (streq(line, "ls")) {
        if (!vfs_root) {
            terminal_writestring("No filesystem mounted.\n");
        } else {
            uint32_t idx = 0;
            vfs_node_t* n;
            while ((n = vfs_readdir(vfs_root, idx))) {
                terminal_writestring("  ");
                terminal_writestring(n->name);
                terminal_writestring("  (");
                print_uint(n->length);
                terminal_writestring(" bytes)\n");
                idx++;
            }
            if (idx == 0) terminal_writestring("(no files)\n");
        }
    } else if (starts_with(line, "cat")) {
        const char* fname = line + 3;
        while (*fname == ' ') fname++;
        vfs_node_t* n = vfs_root ? vfs_finddir(vfs_root, fname) : 0;
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
    } else if (starts_with(line, "run")) {
        const char* fname = line + 3;
        while (*fname == ' ') fname++;
        if (*fname == '\0') {
            terminal_writestring("usage: run <filename>\n");
        } else {
            cmd_run(fname);
        }
    } else if (streq(line, "multitask")) {
        terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_GREEN);
        terminal_writestring("Spawning 3 demo processes under the preemptive scheduler...\n");

        /* Distinct, non-overlapping load addresses - see scheduler.h's
         * top-of-file comment for why concurrently-scheduled tasks can't
         * share USER_LOAD_VADDR (0x40000000, used by `run`/`runuser`)
         * the way single-process programs do: there's still only one
         * shared page directory, so "concurrent" here means "distinct
         * addresses", not "isolated address spaces". */
        int pa = scheduler_spawn_flat("task_a", task_a_bin, task_a_bin_len,
                                       0x50000000u, 0x50100000u);
        int pb = scheduler_spawn_flat("task_b", task_b_bin, task_b_bin_len,
                                       0x50200000u, 0x50300000u);
        int pc = scheduler_spawn_flat("task_c", task_c_bin, task_c_bin_len,
                                       0x50400000u, 0x50500000u);

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
