# Novaris build system
#
# We compile freestanding 32-bit C (no libc, no OS underneath us) and
# assemble the boot stub, then link them at 1MB per the linker script.

CC = gcc
AS = nasm
LD = ld

CFLAGS = -std=gnu99 -ffreestanding -fno-builtin -fno-stack-protector \
         -fno-pic -m32 -Wall -Wextra -O2 -Iinclude
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

BUILD_DIR = build
ISO_DIR = iso

# Every object depends on every header. Coarse, but the alternative here
# was no header dependencies at all, and that bites hard: changing a
# struct in include/idt.h left objects compiled against the old layout
# linked into the same kernel, which fails at runtime (garbage register
# values in a syscall handler) rather than at build time. This tree is
# small enough that rebuilding it wholesale costs a couple of seconds.
HEADERS = $(wildcard include/*.h)

OBJS = $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o \
       $(BUILD_DIR)/vga_text.o $(BUILD_DIR)/console.o $(BUILD_DIR)/framebuffer.o \
       $(BUILD_DIR)/font8x16.o $(BUILD_DIR)/mouse.o \
       $(BUILD_DIR)/uifont.o $(BUILD_DIR)/gfx.o $(BUILD_DIR)/uikit.o \
       $(BUILD_DIR)/icons.o \
       $(BUILD_DIR)/wm.o $(BUILD_DIR)/desktop.o $(BUILD_DIR)/cpu.o \
       $(BUILD_DIR)/app_terminal.o $(BUILD_DIR)/app_files.o \
       $(BUILD_DIR)/app_monitor.o $(BUILD_DIR)/app_about.o \
       $(BUILD_DIR)/gdt.o $(BUILD_DIR)/gdt_flush.o \
       $(BUILD_DIR)/idt.o $(BUILD_DIR)/idt_flush.o \
       $(BUILD_DIR)/isr.o $(BUILD_DIR)/pic.o \
       $(BUILD_DIR)/pit.o $(BUILD_DIR)/keyboard.o $(BUILD_DIR)/shell.o \
       $(BUILD_DIR)/serial.o \
       $(BUILD_DIR)/pmm.o $(BUILD_DIR)/paging.o $(BUILD_DIR)/kheap.o \
       $(BUILD_DIR)/syscall.o $(BUILD_DIR)/fpu.o $(BUILD_DIR)/posix.o $(BUILD_DIR)/posix_signal.o $(BUILD_DIR)/posix_thread.o \
       $(BUILD_DIR)/process.o $(BUILD_DIR)/process_asm.o \
       $(BUILD_DIR)/user_hello_blob.o $(BUILD_DIR)/vfs.o $(BUILD_DIR)/initrd.o \
       $(BUILD_DIR)/elf.o $(BUILD_DIR)/pe.o $(BUILD_DIR)/kstring.o \
       $(BUILD_DIR)/rtc.o \
       $(BUILD_DIR)/win32.o $(BUILD_DIR)/win32_kernel32.o \
       $(BUILD_DIR)/win32_msvcrt.o $(BUILD_DIR)/win32_user32.o \
       $(BUILD_DIR)/win32_dtoa.o $(BUILD_DIR)/win32_format.o \
       $(BUILD_DIR)/win32_callback.o $(BUILD_DIR)/win32_callback_asm.o \
       $(BUILD_DIR)/scheduler.o $(BUILD_DIR)/scheduler_asm.o \
       $(BUILD_DIR)/task_a_blob.o $(BUILD_DIR)/task_b_blob.o $(BUILD_DIR)/task_c_blob.o \
       $(BUILD_DIR)/thread_demo_blob.o

KERNEL = $(BUILD_DIR)/novaris.bin
ISO = novaris.iso

.PHONY: all clean run run-nographic iso test test-qemu test-posix zip

all: $(ISO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: boot/boot.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: kernel/kernel.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vga_text.o: kernel/vga_text.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.o: kernel/console.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/framebuffer.o: kernel/framebuffer.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/font8x16.o: kernel/font8x16.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mouse.o: kernel/mouse.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: kernel/gdt.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt_flush.o: kernel/gdt_flush.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/idt.o: kernel/idt.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/idt_flush.o: kernel/idt_flush.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/isr.o: kernel/isr.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/pic.o: kernel/pic.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pit.o: kernel/pit.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: kernel/keyboard.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell.o: kernel/shell.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: kernel/serial.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pmm.o: kernel/pmm.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/paging.o: kernel/paging.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kheap.o: kernel/kheap.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: kernel/syscall.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/posix.o: kernel/posix.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fpu.o: kernel/fpu.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/posix_signal.o: kernel/posix_signal.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/posix_thread.o: kernel/posix_thread.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process.o: kernel/process.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process_asm.o: kernel/process_asm.s | $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/user_hello_blob.o: kernel/user_hello_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: kernel/vfs.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/initrd.o: kernel/initrd.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf.o: kernel/elf.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pe.o: kernel/pe.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kstring.o: kernel/kstring.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rtc.o: kernel/rtc.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# --- The desktop: compositor, window manager, and the built-in apps ------
# (see include/gfx.h and include/wm.h for how the layers fit together)
$(BUILD_DIR)/uifont.o: kernel/uifont.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gfx.o: kernel/gfx.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/uikit.o: kernel/uikit.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/icons.o: kernel/icons.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wm.o: kernel/wm.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/desktop.o: kernel/desktop.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/cpu.o: kernel/cpu.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/app_terminal.o: kernel/app_terminal.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/app_files.o: kernel/app_files.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/app_monitor.o: kernel/app_monitor.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/app_about.o: kernel/app_about.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# --- The Win32 emulation layer (see include/win32.h) ---
$(BUILD_DIR)/win32.o: kernel/win32.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_kernel32.o: kernel/win32_kernel32.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_msvcrt.o: kernel/win32_msvcrt.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_user32.o: kernel/win32_user32.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_format.o: kernel/win32_format.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_callback.o: kernel/win32_callback.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/win32_callback_asm.o: kernel/win32_callback_asm.s | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/win32_dtoa.o: kernel/win32_dtoa.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/scheduler.o: kernel/scheduler.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/scheduler_asm.o: kernel/scheduler_asm.s | $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/task_a_blob.o: kernel/task_a_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task_b_blob.o: kernel/task_b_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task_c_blob.o: kernel/task_c_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/thread_demo_blob.o: kernel/thread_demo_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Userland: a tiny libc + demo C program, shipped via the initrd ---
# These are separate flat binaries (not linked into the kernel image),
# assembled/compiled/linked with their own flags and linker script
# (userland/user.ld), then packed into build/initrd.img alongside the
# static files in userland/initrd_files/.

USER_CFLAGS = $(CFLAGS) -fno-asynchronous-unwind-tables -Iuserland/libc

$(BUILD_DIR)/user/crt0.o: userland/libc/crt0.s | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/user/libc.o: userland/libc/libc.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/user/hello_c.o: userland/hello_c.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/user/hello_c.elf: $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/hello_c.o $(BUILD_DIR)/user/libc.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ \
	    $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/hello_c.o $(BUILD_DIR)/user/libc.o

$(BUILD_DIR)/user/hello_c.bin: $(BUILD_DIR)/user/hello_c.elf
	objcopy -O binary $< $@

$(BUILD_DIR)/user/hello_elf.o: userland/hello_elf.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

# posix_test.c is deliberately built with the *host* gcc and no library of
# any kind, not with the in-tree userland toolchain: it is a program
# written for Linux, and the point of the test is that Novaris runs it
# unmodified. The same binary is executed on the build host during
# `make test` and inside QEMU during `make test-qemu`, and the two
# transcripts are compared - see ROADMAP.md Milestone 18.
#
# -ffreestanding -fno-builtin matters: without it gcc recognises the
# hand-written strlen loop in the file and emits a call to the real
# strlen, which does not exist in a -nostdlib link.
$(BUILD_DIR)/user/posixtest.elf: userland/posix_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

# The only test here built against a real C library rather than
# freestanding: plain `gcc -m32 -static`, so what runs on Novaris is
# hundreds of KB of production glibc that has never heard of it. See
# ROADMAP.md Milestone 21.
$(BUILD_DIR)/user/glibc.elf: userland/glibc_hello.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -O2 -o $@ $<

# The same program linked *dynamically*, which is the ordinary way a
# Linux program is built - it needs ld-linux.so.2 to load libc.so.6 at
# runtime. See ROADMAP.md Milestone 22.
$(BUILD_DIR)/user/dyn.elf: userland/glibc_hello.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -O2 -o $@ $<

# The dynamic linker and the C library are *copied from the host
# toolchain* rather than committed to this repository: they are build
# inputs, like mingw-w64's import libraries, and vendoring several
# megabytes of LGPL glibc into a hobby kernel's source tree would be both
# bloat and a licensing question nobody needs. The paths are where a
# Debian/Ubuntu multilib install puts them.
HOST_LIB32 ?= /lib32
$(BUILD_DIR)/user/ld-linux.so.2: | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	cp $(HOST_LIB32)/ld-linux.so.2 $@
$(BUILD_DIR)/user/libc.so.6: | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	cp $(HOST_LIB32)/libc.so.6 $@

$(BUILD_DIR)/user/crashelf.elf: userland/crash_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

$(BUILD_DIR)/user/pthtest.elf: userland/thread_posix_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

$(BUILD_DIR)/user/sigtest.elf: userland/signal_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

$(BUILD_DIR)/user/hello_elf.elf: $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/hello_elf.o $(BUILD_DIR)/user/libc.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ \
	    $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/hello_elf.o $(BUILD_DIR)/user/libc.o

# --- Windows test binaries -------------------------------------------------
#
# These are built with the real mingw-w64 toolchain and are *not* written
# for Novaris: hello_win.c and win32_api.c are ordinary Windows programs
# that would run unchanged on Windows, linked against the real msvcrt and
# kernel32 import libraries. That is what makes them a meaningful test of
# the Win32 emulation layer rather than a demonstration of itself.
#
# Needs mingw-w64 (`apt install mingw-w64`) - only for these binaries; the
# kernel has no such dependency. Without it, `make` fails on these targets
# specifically.
MINGW_LD = i686-w64-mingw32-ld
MINGW_CC = i686-w64-mingw32-gcc
MINGW_CXX = i686-w64-mingw32-g++
MINGW_CC64 = x86_64-w64-mingw32-gcc
MINGW_CFLAGS = -O2 -Wall

$(BUILD_DIR)/user/hello_pe.obj: userland/pe_test/hello_pe.asm | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(AS) -f win32 $< -o $@

$(BUILD_DIR)/user/hello_pe.exe: $(BUILD_DIR)/user/hello_pe.obj
	$(MINGW_LD) -e start --subsystem console -o $@ $<

$(BUILD_DIR)/user/hellowin.exe: userland/pe_test/hello_win.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $<

$(BUILD_DIR)/user/winapi.exe: userland/pe_test/win32_api.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $< -lgdi32

# A C++ binary, for its global constructors (see the source): they are
# what exercises the ring-3 _initterm implementation.
$(BUILD_DIR)/user/cppinit.exe: userland/pe_test/cpp_init.cpp | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CXX) $(MINGW_CFLAGS) -static-libstdc++ -static-libgcc -o $@ $<

# -mwindows makes this a GUI-subsystem binary with a WinMain entry point.
$(BUILD_DIR)/user/guiapp.exe: userland/pe_test/gui_app.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -mwindows -o $@ $<

# The same program linked at an image base Novaris will not hand out
# (0x50000 is below the 1MB floor the loader enforces), which forces the
# base relocation path: the loader has to move it into the relocation
# arena and rewrite every absolute address in it. Same source as
# hellowin.exe, so any difference in its output is the relocator's doing.
$(BUILD_DIR)/user/lowbase.exe: userland/pe_test/hello_win.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -Wl,--image-base,0x50000 \
	    -Wl,--dynamicbase -o $@ $<

# A program that faults on purpose, to prove a bad .exe takes only itself
# down and not the kernel.
$(BUILD_DIR)/user/crash.exe: userland/pe_test/crash.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $<

# Exercises the kernel -> ring-3 callback mechanism: qsort/bsearch
# comparators and atexit handlers are all functions the kernel has to call
# back into ring 3 (see include/win32_callback.h).
$(BUILD_DIR)/user/qsorttest.exe: userland/pe_test/qsort_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $<

# Real Win32 threads: CreateThread, WaitForSingleObject, a CRITICAL_SECTION
# and InterlockedIncrement (see include/win32_callback.h's sibling work in
# ROADMAP.md Milestone 17).
$(BUILD_DIR)/user/threads.exe: userland/pe_test/threads.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $<

# A 64-bit PE, built purely so the "this is a 64-bit binary" diagnostic
# has something real to fire on. -nostdlib keeps it to a couple of KB.
$(BUILD_DIR)/user/hello64.exe: userland/pe_test/x64_marker.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC64) -O2 -nostdlib -e mainCRTStartup -o $@ $<

# Everything that ends up packed into the initrd: the static files in
# userland/initrd_files/ plus the compiled demo programs.
$(BUILD_DIR)/initrd_staging/helloelf.elf: $(BUILD_DIR)/user/hello_c.bin \
        $(BUILD_DIR)/user/hello_elf.elf $(BUILD_DIR)/user/posixtest.elf $(BUILD_DIR)/user/sigtest.elf \
        $(BUILD_DIR)/user/pthtest.elf $(BUILD_DIR)/user/crashelf.elf \
        $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/user/dyn.elf \
        $(BUILD_DIR)/user/ld-linux.so.2 $(BUILD_DIR)/user/libc.so.6 \
        $(BUILD_DIR)/user/hello_pe.exe \
        $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/user/winapi.exe \
        $(BUILD_DIR)/user/cppinit.exe $(BUILD_DIR)/user/guiapp.exe \
        $(BUILD_DIR)/user/hello64.exe $(BUILD_DIR)/user/lowbase.exe \
        $(BUILD_DIR)/user/crash.exe $(BUILD_DIR)/user/qsorttest.exe \
        $(BUILD_DIR)/user/threads.exe
	mkdir -p $(BUILD_DIR)/initrd_staging
	cp userland/initrd_files/* $(BUILD_DIR)/initrd_staging/
	cp $(BUILD_DIR)/user/hello_c.bin $(BUILD_DIR)/initrd_staging/helloc.bin
	cp $(BUILD_DIR)/user/hello_elf.elf $(BUILD_DIR)/initrd_staging/helloelf.elf
	cp $(BUILD_DIR)/user/posixtest.elf $(BUILD_DIR)/initrd_staging/posixtest.elf
	cp $(BUILD_DIR)/user/sigtest.elf $(BUILD_DIR)/initrd_staging/sigtest.elf
	cp $(BUILD_DIR)/user/pthtest.elf $(BUILD_DIR)/initrd_staging/pthtest.elf
	cp $(BUILD_DIR)/user/crashelf.elf $(BUILD_DIR)/initrd_staging/crashelf.elf
	cp $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/initrd_staging/glibc.elf
	cp $(BUILD_DIR)/user/dyn.elf $(BUILD_DIR)/initrd_staging/dyn.elf
	cp $(BUILD_DIR)/user/ld-linux.so.2 $(BUILD_DIR)/initrd_staging/ld-linux.so.2
	cp $(BUILD_DIR)/user/libc.so.6 $(BUILD_DIR)/initrd_staging/libc.so.6
	cp $(BUILD_DIR)/user/hello_pe.exe $(BUILD_DIR)/initrd_staging/hellope.exe
	cp $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/initrd_staging/hellowin.exe
	cp $(BUILD_DIR)/user/winapi.exe $(BUILD_DIR)/initrd_staging/winapi.exe
	cp $(BUILD_DIR)/user/cppinit.exe $(BUILD_DIR)/initrd_staging/cppinit.exe
	cp $(BUILD_DIR)/user/guiapp.exe $(BUILD_DIR)/initrd_staging/guiapp.exe
	cp $(BUILD_DIR)/user/hello64.exe $(BUILD_DIR)/initrd_staging/hello64.exe
	cp $(BUILD_DIR)/user/lowbase.exe $(BUILD_DIR)/initrd_staging/lowbase.exe
	cp $(BUILD_DIR)/user/crash.exe $(BUILD_DIR)/initrd_staging/crash.exe
	cp $(BUILD_DIR)/user/qsorttest.exe $(BUILD_DIR)/initrd_staging/qsorttest.exe
	cp $(BUILD_DIR)/user/threads.exe $(BUILD_DIR)/initrd_staging/threads.exe

$(BUILD_DIR)/initrd.img: $(BUILD_DIR)/initrd_staging/helloelf.elf userland/mkinitrd.py
	python3 userland/mkinitrd.py $(BUILD_DIR)/initrd_staging $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

iso: $(KERNEL) $(BUILD_DIR)/initrd.img
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/novaris.bin
	cp $(BUILD_DIR)/initrd.img $(ISO_DIR)/boot/initrd.img
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_DIR) 2>/dev/null

$(ISO): iso

# Run in QEMU with a graphical window (won't work headless - use run-nographic)
run: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

# Run in QEMU headlessly and dump a screenshot to screenshot.png
run-nographic: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -display none -serial file:serial.log &
	sleep 2 && kill %1 2>/dev/null || true

# --- Tests -----------------------------------------------------------------
#
# Two layers, deliberately. The host tests link the *actual* kernel
# sources (not reimplementations) into a host binary and drive them
# directly, which is where a failure tells you exactly what broke. The
# QEMU test boots the real ISO and drives the shell, which is the only
# thing that proves the whole stack works together.
#
# Both are 32-bit host builds, because the code under test assumes 32-bit
# pointers throughout - `apt install gcc-multilib` if the link fails.

HOST_CFLAGS = -std=gnu99 -m32 -Wall -Wextra -Iinclude -g

$(BUILD_DIR)/test/format_test: tests/format_host_test.c kernel/win32_format.c \
        kernel/win32_dtoa.c kernel/kstring.c $(HEADERS)
	mkdir -p $(BUILD_DIR)/test
	$(CC) $(HOST_CFLAGS) -o $@ tests/format_host_test.c kernel/win32_format.c \
	    kernel/win32_dtoa.c kernel/kstring.c

$(BUILD_DIR)/test/pe_test: tests/pe_host_test.c kernel/pe.c kernel/kstring.c $(HEADERS)
	mkdir -p $(BUILD_DIR)/test
	$(CC) $(HOST_CFLAGS) -o $@ tests/pe_host_test.c kernel/pe.c kernel/kstring.c

# The window manager and the desktop shell, driven on the host. desktop.c
# is #included by the test (it needs the shell's file-scope state), so it
# is a prerequisite but not a source on the command line.
WM_TEST_SRCS = kernel/wm.c kernel/gfx.c kernel/icons.c kernel/uikit.c \
               kernel/uifont.c kernel/font8x16.c kernel/kstring.c

$(BUILD_DIR)/test/wm_test: tests/wm_host_test.c kernel/desktop.c \
        $(WM_TEST_SRCS) $(HEADERS)
	mkdir -p $(BUILD_DIR)/test
	$(CC) $(HOST_CFLAGS) -o $@ tests/wm_host_test.c $(WM_TEST_SRCS)

# The PE test loads binaries out of build/user, so they have to exist.
test: $(BUILD_DIR)/test/format_test $(BUILD_DIR)/test/pe_test \
        $(BUILD_DIR)/test/wm_test \
        $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/user/lowbase.exe \
        $(BUILD_DIR)/user/guiapp.exe $(BUILD_DIR)/user/hello64.exe
	@echo "=== printf/dtoa engine ==="
	@$(BUILD_DIR)/test/format_test
	@echo
	@echo "=== PE loader ==="
	@$(BUILD_DIR)/test/pe_test
	@echo
	@echo "=== window manager / desktop shell ==="
	@$(BUILD_DIR)/test/wm_test --shots $(BUILD_DIR)/test

# Boots the ISO and drives the shell, asserting on the serial transcript.
# Slow (a couple of minutes) and needs qemu-system-i386.
# The same binary, run on Linux and on Novaris, transcripts compared.
# See tools/posix_compare.py and ROADMAP.md Milestone 18.
.PHONY: test-posix
test-posix: $(ISO) $(BUILD_DIR)/user/posixtest.elf $(BUILD_DIR)/user/sigtest.elf \
           $(BUILD_DIR)/user/pthtest.elf $(BUILD_DIR)/user/glibc.elf \
           $(BUILD_DIR)/user/dyn.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/posixtest.elf --name posixtest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/sigtest.elf --name sigtest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/pthtest.elf --name pthtest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/glibc.elf --name glibc.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/dyn.elf --name dyn.elf

test-qemu: $(ISO)
	python3 tools/qemu_test.py --iso $(ISO) --script tools/tests/win32_smoke.txt \
	    --boot-wait 16 --settle 5 \
	    --expect "Hello from a real Windows .exe running on Novaris" \
	    --expect "floats:     3\.141593 2\.50 1\.234568e\+04 0\.0001" \
	    --expect "fib\(20\):    6765" \
	    --expect "\[ctor\]  a C\+\+ global constructor ran before main" \
	    --expect "HeapAlloc round-trip" \
	    --expect "64-bit \(PE32\+\) binary" \
	    --expect "53 of 53 imports resolve" \
	    --reject "KERNEL PANIC"

# Repackages the source tree as novaris.zip, which is how this project
# gets moved between machines. Regenerated rather than hand-assembled so
# it can't drift out of step with the tree it's built from; build output
# is excluded, since `make` reproduces all of it.
ZIP_CONTENTS = boot include kernel tests tools userland Makefile linker.ld \
               grub.cfg README.md ROADMAP.md .gitignore \
               docs-proof-of-boot.png docs-desktop.png

zip:
	rm -rf $(BUILD_DIR)/pkg novaris.zip
	mkdir -p $(BUILD_DIR)/pkg/novaris
	cp -r $(ZIP_CONTENTS) $(BUILD_DIR)/pkg/novaris/
	cd $(BUILD_DIR)/pkg && zip -q -r $(CURDIR)/novaris.zip novaris
	rm -rf $(BUILD_DIR)/pkg
	@echo "Wrote novaris.zip"

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO) serial.log screenshot.png
