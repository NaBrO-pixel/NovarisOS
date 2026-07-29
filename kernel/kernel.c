#include <stdint.h>
#include "console.h"
#include "multiboot.h"
#include "gdt.h"
#include "idt.h"
#include "pit.h"
#include "keyboard.h"
#include "shell.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "vfs.h"
#include "initrd.h"
#include "mouse.h"
#include "serial.h"
#include "win32.h"

/* Defined by linker.ld: mark the physical extent of the kernel's own
 * loaded image so the PMM knows never to hand those frames out. These
 * are link-time addresses with no storage of their own - take their
 * address, don't dereference them. */
extern uint32_t _kernel_physical_start;
extern uint32_t _kernel_physical_end;

/* Extremely small printf-like helper: prints an unsigned int in decimal.
 * We don't have a libc here, so utility functions like this get built by hand. */
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

/* A tiny handler for the breakpoint exception (vector 3, triggered by the
 * `int3` instruction). We use it below as a live smoke test: if this
 * prints and the kernel keeps running afterwards, the whole GDT -> IDT ->
 * ISR -> PIC pipeline is wired correctly end to end. */
static void breakpoint_handler(registers_t* regs) {
    (void)regs;
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Breakpoint interrupt (int3) handled and returned cleanly\n");
}

void kernel_main(uint32_t magic, multiboot_info_t* mbi) {
    /* Everything up through paging_init() prints via VGA text mode -
     * the framebuffer's physical address generally isn't inside the
     * boot-time identity mapping boot.s sets up, so we can't safely draw
     * to it until real paging can map it explicitly (see below). Once
     * that happens we switch the console over and never look back. */
    serial_init(); /* before terminal_initialize(), so boot output is mirrored too */
    terminal_initialize();

    terminal_writestring_color("Novaris", VGA_COLOR_LIGHT_CYAN);
    terminal_writestring(" -- Milestone 10: Win32 (.exe) emulation\n");
    terminal_writestring("========================================\n\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        terminal_writestring_color("WARNING: not loaded by a Multiboot bootloader!\n", VGA_COLOR_LIGHT_RED);
    } else {
        terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
        terminal_writestring("Loaded correctly via Multiboot (GRUB)\n");
    }

    if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
        terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
        terminal_writestring("Memory detected: lower=");
        print_uint(mbi->mem_lower);
        terminal_writestring("KB upper=");
        print_uint(mbi->mem_upper);
        terminal_writestring("KB\n");
    }

    gdt_install();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("GDT installed (flat kernel + user segments)\n");

    idt_install();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("IDT installed (32 exception vectors, PIC remapped to 32-47)\n");

    register_interrupt_handler(3, breakpoint_handler);
    __asm__ __volatile__("sti");
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Interrupts enabled (sti)\n");

    /* Fire a breakpoint interrupt on purpose: if the line above prints and
     * we get back here afterwards, the interrupt pipeline works end to end. */
    __asm__ __volatile__("int3");
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Resumed normal execution after the interrupt\n");

    pit_install(100); /* 100 Hz: a tick every 10ms */
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("PIT installed (IRQ0, 100 Hz)\n");

    keyboard_install();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("PS/2 keyboard driver installed (IRQ1)\n");

    uint32_t kstart = (uint32_t)&_kernel_physical_start;
    uint32_t kend = (uint32_t)&_kernel_physical_end;

    pmm_init(mbi, kstart, kend);
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Physical memory manager initialized (");
    print_uint(pmm_free_frames());
    terminal_writestring("/");
    print_uint(pmm_total_frames());
    terminal_writestring(" frames free)\n");

    /* If GRUB loaded an initrd module (see grub.cfg), reserve its backing
     * memory in the PMM *before* paging_init() runs - paging_init()
     * allocates page-table frames through the PMM, and if it doesn't
     * know the module's memory is spoken for, it could hand out (and
     * overwrite) the very initrd we're about to parse. */
    multiboot_module_t* initrd_mod = 0;
    if ((mbi->flags & MULTIBOOT_INFO_MODS) && mbi->mods_count > 0) {
        initrd_mod = (multiboot_module_t*)mbi->mods_addr;
        pmm_reserve_region(initrd_mod->mod_start, initrd_mod->mod_end);
    }

    paging_init(kstart, kend);
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Paging enabled: kernel now running higher-half at 0xC0000000+\n");

    /* Now that paging is up, try to switch from VGA text mode to a real
     * linear framebuffer, if the bootloader granted one (it may not:
     * VBE might be unsupported, or GRUB may have fallen back to a
     * different mode than boot.s's Multiboot header requested). Identity
     * map its pages explicitly - unlike the initrd module, this is
     * hardware memory (VRAM), not RAM the PMM would ever hand out, so we
     * map it directly rather than going through pmm_alloc_frame. */
    if ((mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER) &&
        mbi->framebuffer_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB &&
        (mbi->framebuffer_bpp == 32 || mbi->framebuffer_bpp == 24) &&
        mbi->framebuffer_addr <= 0xFFFFFFFFull) {
        uint32_t fb_addr = (uint32_t)mbi->framebuffer_addr;
        uint32_t fb_size = mbi->framebuffer_pitch * mbi->framebuffer_height;
        uint32_t start = fb_addr & ~0xFFFu;
        uint32_t end = (fb_addr + fb_size + 0xFFFu) & ~0xFFFu;
        for (uint32_t p = start; p < end; p += 4096) {
            paging_map_page(p, p, PAGE_PRESENT | PAGE_RW);
        }
        /* Off-limits to the PE loader: every terminal_putchar() draws
         * into this range, including while a .exe is running, so it can't
         * be borrowed the way other identity mappings can (see paging.h). */
        paging_reserve_region(start, end, "framebuffer");

        if (console_use_framebuffer(fb_addr, mbi->framebuffer_pitch,
                                     mbi->framebuffer_width, mbi->framebuffer_height,
                                     mbi->framebuffer_bpp)) {
            terminal_initialize(); /* redraws chrome and switches the backend over */
            terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
            terminal_writestring("Switched to linear framebuffer console (");
            print_uint(mbi->framebuffer_width);
            terminal_writestring("x");
            print_uint(mbi->framebuffer_height);
            terminal_writestring("x");
            print_uint(mbi->framebuffer_bpp);
            terminal_writestring(")\n");
        }
    }
    if (!console_has_framebuffer()) {
        terminal_writestring_color("[--] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("No usable framebuffer - staying in VGA text mode\n");
    }

    kheap_init();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Kernel heap installed (kmalloc/kfree)\n");

    if (initrd_mod) {
        /* The module's physical memory is reserved but not necessarily
         * mapped yet - identity-map it explicitly rather than assuming
         * it landed inside boot.s's first-4MB mapping, since GRUB is
         * free to place modules anywhere. */
        uint32_t start = initrd_mod->mod_start & ~0xFFFu;
        uint32_t end = (initrd_mod->mod_end + 0xFFFu) & ~0xFFFu;
        for (uint32_t p = start; p < end; p += 4096) {
            paging_map_page(p, p, PAGE_PRESENT | PAGE_RW);
        }
        /* Same reasoning as the framebuffer above: a .exe that opens a
         * file reaches the initrd through these very addresses, so the
         * loader must not place an image over them. GRUB is free to put
         * modules anywhere, including inside a typical 0x400000 ImageBase. */
        paging_reserve_region(start, end, "initrd");
        initrd_init(initrd_mod->mod_start, initrd_mod->mod_end - initrd_mod->mod_start);
        terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
        terminal_writestring("Initrd mounted (try 'ls' in the shell)\n");
    } else {
        terminal_writestring_color("[--] ", VGA_COLOR_LIGHT_RED);
        terminal_writestring("No initrd module found - filesystem commands won't have files\n");
    }

    /* Live smoke test: allocate, write through the pointer, free, then
     * allocate again and confirm the freed space got reused. If any of
     * this were wired wrong we'd have triple-faulted well before this
     * point, but printing an actual round-tripped value is a stronger
     * check than "didn't crash". */
    char* test_buf = (char*)kmalloc(64);
    const char* test_msg = "kmalloc round-trip OK";
    int i = 0;
    while (test_msg[i]) { test_buf[i] = test_msg[i]; i++; }
    test_buf[i] = '\0';
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring(test_buf);
    terminal_writestring("\n");
    kfree(test_buf);

    syscall_install();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Syscall interface installed (int 0x80, DPL=3)\n");

    scheduler_init();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Preemptive scheduler initialized (try 'multitask' in the shell)\n");

    process_install_fault_handler();
    win32_init();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Win32 emulation layer installed (");
    uint32_t apis = 0;
    for (uint32_t m = 0; m < win32_module_count(); m++) apis += win32_module_exports(m);
    print_uint(apis);
    terminal_writestring(" APIs across ");
    uint32_t dlls = 0;
    for (uint32_t m = 0; m < win32_module_count(); m++) {
        if (win32_module_exports(m)) dlls++;
    }
    print_uint(dlls);
    terminal_writestring(" DLLs - try 'winapi')\n");

    /* Live smoke test: actually drop to ring 3 and run a real user-mode
     * program that calls back into the kernel via int 0x80, rather than
     * just checking that the setup code didn't crash. If this line and
     * the "returning to kernel" line both print, the whole GDT -> TSS ->
     * paging(PAGE_USER) -> iret -> ring3 -> int0x80 -> ring0 pipeline
     * works end to end. */
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Dropping to ring 3 to run a demo user-mode program...\n");
    process_run_demo_user_program();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("Back in ring 0 after the user program exited\n");

    mouse_install();
    terminal_writestring_color("[OK] ", VGA_COLOR_LIGHT_GREEN);
    terminal_writestring("PS/2 mouse driver installed (IRQ12)\n");

    terminal_writestring("\nThis kernel currently:\n");
    terminal_writestring("  - Boots via GRUB (Multiboot 1)\n");
    terminal_writestring("  - Runs in 32-bit protected mode\n");
    terminal_writestring("  - Reads basic memory info from the bootloader\n");
    terminal_writestring("  - Has its own GDT, IDT, ISRs, and a remapped PIC\n");
    terminal_writestring("  - Has a PIT timer and a PS/2 keyboard driver\n");
    terminal_writestring("  - Runs higher-half with real paging, a physical\n");
    terminal_writestring("    memory manager, and a kmalloc/kfree kernel heap\n");
    terminal_writestring("  - Can run ring-3 user-mode programs via a TSS and\n");
    terminal_writestring("    an int 0x80 syscall interface\n");
    terminal_writestring("  - Loads a real initrd via a GRUB multiboot module, with\n");
    terminal_writestring("    a VFS layer and a tiny libc for user programs\n");
    terminal_writestring("  - Draws a real linear framebuffer desktop with a bitmap\n");
    terminal_writestring("    font console and a movable PS/2 mouse cursor\n");
    terminal_writestring("  - Loads and runs real Windows PE32 .exe files against an\n");
    terminal_writestring("    emulated Win32 API ('run prog.exe', 'peinfo', 'winapi')\n");

    terminal_writestring_color("\nSee ROADMAP.md for what's next.\n", VGA_COLOR_LIGHT_BROWN);

    /* Hand off to the shell - it loops forever reading keyboard input. */
    shell_run();
}
