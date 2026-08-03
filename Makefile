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
       $(BUILD_DIR)/posix_proc.o \
       $(BUILD_DIR)/process.o $(BUILD_DIR)/process_asm.o \
       $(BUILD_DIR)/user_hello_blob.o $(BUILD_DIR)/vfs.o $(BUILD_DIR)/ramfs.o $(BUILD_DIR)/socket.o \
       $(BUILD_DIR)/blockdev.o $(BUILD_DIR)/ata.o $(BUILD_DIR)/fat32.o \
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

.PHONY: all clean run run-nographic iso test test-qemu test-posix test-wine \
        test-wine-threads test-qemu-disk test-wine-prefix zip wine-initrd \
        disk wine-disk

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

$(BUILD_DIR)/posix_proc.o: kernel/posix_proc.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process.o: kernel/process.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/process_asm.o: kernel/process_asm.s | $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/user_hello_blob.o: kernel/user_hello_blob.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: kernel/vfs.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ramfs.o: kernel/ramfs.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/socket.o: kernel/socket.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Milestone 32: the disk. blockdev.c is the seam between the two, which is
# what lets tests/fat32_host_test.c drive fat32.c against a file.
$(BUILD_DIR)/blockdev.o: kernel/blockdev.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ata.o: kernel/ata.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: kernel/fat32.c $(HEADERS) | $(BUILD_DIR)
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

# Also dynamically linked, and for the same reason: this one reads
# siginfo_t and ucontext_t out of a signal handler using glibc's own
# headers, so what it checks is the layout glibc says exists rather than
# the layout this project believes Linux has. See ROADMAP.md Milestone 23.
$(BUILD_DIR)/user/uctest.elf: userland/ucontext_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -O2 -Wall -o $@ $<

# The dynamic linker and the C library are *copied from the host
# toolchain* rather than committed to this repository: they are build
# inputs, like mingw-w64's import libraries, and vendoring several
# megabytes of LGPL glibc into a hobby kernel's source tree would be both
# bloat and a licensing question nobody needs. The paths are where a
# Debian/Ubuntu multilib install puts them.
HOST_LIB32 ?= /lib32

# --- Wine (Milestone 27, Path A step 5) ----------------------------------
#
# Wine is *not* vendored. It is fetched and built separately and pointed
# at with WINE_BUILD, exactly the way ld-linux.so.2 and libc.so.6 are
# copied from the host toolchain rather than committed: they are build
# inputs, and several hundred megabytes of LGPL source in a hobby
# kernel's tree would be both bloat and a licensing question nobody
# needs. Wine's own code stays behind that boundary and Novaris's stays
# in front of it.
#
#   git clone --depth 1 -b stable https://github.com/wine-mirror/wine
#   cd wine && CC="gcc -m32" ./configure --enable-archs=i386 #       --disable-tests --without-x --without-freetype ...
#   make -j4
#   cd ../NovarisOS && make WINE_BUILD=../wine wine-initrd
#
# With WINE_BUILD set, `make wine-initrd` builds an ISO carrying Wine's
# loader, ntdll.so and the two files glibc's getpwuid needs. See
# ROADMAP.md Milestone 27 for exactly how far that gets.
WINE_BUILD ?=
WINE_STRIP ?= 1

# Wine's PE builtins, the subset a console program needs. Kept as a list
# rather than a wildcard so that what ships is a decision rather than
# whatever happened to be built, and so the initrd stays small enough to
# read into RAM.
WINE_PE_DLLS = ntdll apisetschema kernel32 kernelbase win32u user32 gdi32 advapi32 \
               sechost rpcrt4 msvcrt ucrtbase ws2_32 setupapi version \
               imm32 combase ole32 oleaut32 shell32 shlwapi shcore winex11 \
               wow64cpu cryptbase bcrypt userenv coml2 wininet mpr
WINE_PE_PROGS = wineboot start conhost services explorer rundll32 cmd

# Milestone 31. A Wine builtin is two halves: a PE .dll the Windows
# program links against, and a Unix .so it reaches through
# __wine_init_unix_call(). Shipping only the first half is not "that
# feature is missing" - it is a DLL whose process attach *fails*, and
# ntdll's loader turns one of those into "Initializing dlls for
# wineboot.exe failed". Which is exactly what stopped wineboot, and why
# the prefix had no drive mappings and Wine reported "could not find DOS
# drive for the current working directory" ever since Milestone 30. It
# was read as "no networking, so ws2_32 cannot work"; the truth was a
# file that had been built and not copied.
#
# ntdll.so and wineserver are handled separately - they are the loader,
# not builtins.
WINE_UNIX_DLLS = win32u ws2_32 bcrypt
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

# The Milestone 26 filesystem test. Freestanding like the rest, and it
# works entirely inside a directory it creates and removes under /tmp, so
# the host run leaves nothing behind.
$(BUILD_DIR)/user/fstest.elf: userland/fs_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

# Milestone 27's kernel-integrity regression. Deliberately not part of
# test-posix: on Linux the mapping it asks for succeeds, and it must not.
$(BUILD_DIR)/user/kmaptest.elf: userland/kmap_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

$(BUILD_DIR)/user/socktest.elf: userland/sock_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

# Milestone 29: fork, execve, waitpid, pipes, dup2 and poll. It re-executes
# itself through /proc/self/exe to test execve, so the binary is both the
# test and its own exec target.
# Milestone 30: shared file mappings, PROT_NONE reservations,
# MAP_FIXED_NOREPLACE, and a file that outlives its name.
$(BUILD_DIR)/user/mmaptest.elf: userland/mmap_test.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(CC) -m32 -static -nostdlib -ffreestanding -fno-builtin -O2 -Wall \
	    -o $@ $<

$(BUILD_DIR)/user/forktest.elf: userland/fork_test.c | $(BUILD_DIR)
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
        $(BUILD_DIR)/user/fstest.elf $(BUILD_DIR)/user/kmaptest.elf \
        $(BUILD_DIR)/user/socktest.elf $(BUILD_DIR)/user/forktest.elf \
        $(BUILD_DIR)/user/mmaptest.elf \
        $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/user/dyn.elf \
        $(BUILD_DIR)/user/uctest.elf \
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
	cp $(BUILD_DIR)/user/fstest.elf $(BUILD_DIR)/initrd_staging/fstest.elf
	cp $(BUILD_DIR)/user/kmaptest.elf $(BUILD_DIR)/initrd_staging/kmaptest.elf
	cp $(BUILD_DIR)/user/socktest.elf $(BUILD_DIR)/initrd_staging/socktest.elf
	cp $(BUILD_DIR)/user/forktest.elf $(BUILD_DIR)/initrd_staging/forktest.elf
	cp $(BUILD_DIR)/user/mmaptest.elf $(BUILD_DIR)/initrd_staging/mmaptest.elf
	cp $(BUILD_DIR)/user/crashelf.elf $(BUILD_DIR)/initrd_staging/crashelf.elf
	cp $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/initrd_staging/glibc.elf
	cp $(BUILD_DIR)/user/dyn.elf $(BUILD_DIR)/initrd_staging/dyn.elf
	cp $(BUILD_DIR)/user/uctest.elf $(BUILD_DIR)/initrd_staging/uctest.elf
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

# Milestone 32. The FAT32 driver, the VFS it plugs into and the block
# layer under it, all linked into a host binary and pointed at a real
# image built by tools/mkfat32.py. See the comment at the top of the test.
FAT_TEST_SRCS = kernel/fat32.c kernel/ramfs.c kernel/vfs.c kernel/blockdev.c \
                kernel/kstring.c

$(BUILD_DIR)/test/fat32_test: tests/fat32_host_test.c $(FAT_TEST_SRCS) $(HEADERS)
	mkdir -p $(BUILD_DIR)/test
	$(CC) $(HOST_CFLAGS) -o $@ tests/fat32_host_test.c $(FAT_TEST_SRCS)

# The image the test runs against, and the staging tree it is built from.
# Symbolic links are part of the tree on purpose: they are what Milestone
# 32 switched on, and a link that only ever exists in RAM proves nothing
# about the on-disk representation.
$(BUILD_DIR)/test/fat32.img: tools/mkfat32.py | $(BUILD_DIR)
	rm -rf $(BUILD_DIR)/test/fatstage
	mkdir -p $(BUILD_DIR)/test/fatstage/sub/deeper
	printf 'hello from the disk\n' > $(BUILD_DIR)/test/fatstage/readme.txt
	printf 'a long file name lives here\n' \
	    > "$(BUILD_DIR)/test/fatstage/a-very-long-file-name-indeed.dat"
	printf 'nested\n' > $(BUILD_DIR)/test/fatstage/sub/deeper/note.txt
	python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31+(i>>8))&0xff for i in range(200000)))" \
	    > $(BUILD_DIR)/test/fatstage/blob.bin
	ln -sf ../readme.txt $(BUILD_DIR)/test/fatstage/sub/link-to-readme
	ln -sf /disk $(BUILD_DIR)/test/fatstage/rootlink
	python3 tools/mkfat32.py $@ $(BUILD_DIR)/test/fatstage --size 64M

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
        $(BUILD_DIR)/test/wm_test $(BUILD_DIR)/test/fat32_test \
        $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/user/lowbase.exe \
        $(BUILD_DIR)/user/guiapp.exe $(BUILD_DIR)/user/hello64.exe
	@echo "=== printf/dtoa engine ==="
	@$(BUILD_DIR)/test/format_test
	@echo
	@echo "=== PE loader ==="
	@$(BUILD_DIR)/test/pe_test
	@echo
	@echo "=== FAT32 driver ==="
	@$(MAKE) --no-print-directory $(BUILD_DIR)/test/fat32.img >/dev/null
	@# On a copy, because the test writes to the volume and has to start
	@# from a known one every run. The pristine image stays put so it can
	@# be mounted on the host and looked at.
	@cp $(BUILD_DIR)/test/fat32.img $(BUILD_DIR)/test/fat32-work.img
	@$(BUILD_DIR)/test/fat32_test $(BUILD_DIR)/test/fat32-work.img
	@# And then let somebody else's checker look at what the driver wrote.
	@# This is the strongest single statement available about the driver:
	@# fsck.vfat has no idea Novaris exists, and it walks every cluster
	@# chain, every directory entry and every long-name checksum on a
	@# volume that has been written, truncated, renamed, grown and filled
	@# to capacity. Skipped rather than failed when dosfstools is not
	@# installed - it is a check, not a build dependency.
	@if command -v fsck.vfat >/dev/null 2>&1; then \
	    echo "--- fsck.vfat on the volume the driver wrote ---"; \
	    fsck.vfat -n $(BUILD_DIR)/test/fat32-work.img; \
	else \
	    echo "(fsck.vfat not installed - skipping the independent check;"; \
	    echo " apt-get install dosfstools)"; \
	fi
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
           $(BUILD_DIR)/user/dyn.elf $(BUILD_DIR)/user/uctest.elf \
           $(BUILD_DIR)/user/fstest.elf $(BUILD_DIR)/user/socktest.elf \
           $(BUILD_DIR)/user/forktest.elf $(BUILD_DIR)/user/mmaptest.elf
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
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/uctest.elf --name uctest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/fstest.elf --name fstest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/socktest.elf --name socktest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/forktest.elf --name forktest.elf
	python3 tools/posix_compare.py --iso $(ISO) \
	    --binary $(BUILD_DIR)/user/mmaptest.elf --name mmaptest.elf

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
	    --expect "MAP_FIXED across the kernel's identity map is refused" \
	    --expect "the parent's copy is untouched" \
	    --expect "the exec'd image kept the descriptor it was given" \
	    --expect "poll waits, and a write from another process wakes it" \
	    --expect "fork test done" \
	    --expect "a store through it is visible through read" \
	    --expect "the bytes are still there" \
	    --expect "mmap test done" \
	    --expect "the kernel is still running" \
	    --reject "KERNEL PANIC"

# Repackages the source tree as novaris.zip, which is how this project
# gets moved between machines. Regenerated rather than hand-assembled so
# it can't drift out of step with the tree it's built from; build output
# is excluded, since `make` reproduces all of it.
ZIP_CONTENTS = boot include kernel tests tools userland Makefile linker.ld \
               grub.cfg README.md ROADMAP.md .gitignore \
               docs-proof-of-boot.png docs-desktop.png

# Builds a *separate* ISO carrying Wine on top of the ordinary initrd.
# Separate on purpose: novaris.iso must stay reproducible from this tree
# alone, and Wine is not in this tree.
wine-initrd: $(BUILD_DIR)/initrd.img $(KERNEL)
	@test -n "$(WINE_BUILD)" || \
	    { echo "set WINE_BUILD=/path/to/a/built/wine (see the comment in the Makefile)"; exit 1; }
	@test -f "$(WINE_BUILD)/loader/wine-preloader" || \
	    { echo "$(WINE_BUILD) does not look like a built Wine tree"; exit 1; }
	rm -rf $(BUILD_DIR)/wine_staging $(BUILD_DIR)/wine_iso
	mkdir -p $(BUILD_DIR)/wine_staging $(BUILD_DIR)/wine_iso/boot/grub
	cp $(BUILD_DIR)/initrd_staging/* $(BUILD_DIR)/wine_staging/
	cp $(WINE_BUILD)/loader/wine-preloader $(BUILD_DIR)/wine_staging/preload.elf
	cp $(WINE_BUILD)/loader/wine $(BUILD_DIR)/wine_staging/wineldr.elf
	cp $(WINE_BUILD)/dlls/ntdll/ntdll.so $(BUILD_DIR)/wine_staging/ntdll.so
	# Milestone 29: wineserver, which is the process Wine forks and execs
	# before it can do anything with a handle. Until fork/execve/waitpid
	# existed there was no point carrying it.
	cp $(WINE_BUILD)/server/wineserver $(BUILD_DIR)/wine_staging/wineserver
	# Also under the names Wine's own path logic derives. init_paths()
	# takes the directory ntdll.so was loaded from and looks for "wine"
	# and "wine-preloader" beside it, and the initrd is flat - so those
	# are the names re-exec finds. Both sets ship: the documented
	# `run preload.elf wineldr.elf --version` still works, and Wine
	# re-execing itself finds what it expects without being told.
	cp $(WINE_BUILD)/loader/wine $(BUILD_DIR)/wine_staging/wine
	cp $(WINE_BUILD)/loader/wine-preloader $(BUILD_DIR)/wine_staging/wine-preloader
	# The NLS tables. Not decoration and not optional: wineserver calls
	# fatal_error() if it cannot load l_intl.nls, and kernelbase's
	# init_locale walks sortdefault.nls without checking that it got it -
	# with that one missing it parsed whatever pointer was left over,
	# read a zero count, indexed to "the last of nothing" and took an
	# access violation twenty-four bytes before the mapping.
	#
	# The whole directory rather than a hand-picked list, for exactly
	# that reason: a missing table does not announce itself.
	cp $(WINE_BUILD)/nls/*.nls $(BUILD_DIR)/wine_staging/
	# --- Wine's Windows side (Milestone 30) ---------------------------
	#
	# The PE builtins. Everything above this line is Wine's *Unix* half;
	# these are the DLLs it loads as Windows modules once the server
	# handshake is done, and without them the loader gets as far as
	# "failed to load start.exe" and stops.
	#
	# Not all 601 of them: a console program needs the core plus what
	# ntdll's loader touches on the way, and the initrd is read into RAM
	# whole. They are stripped on the way in - debug information is three
	# quarters of the bytes and nothing here reads it.
	#
	# The initrd is flat, so these land at the root and the last-component
	# fallback resolves Wine's "<dir>/i386-windows/ntdll.dll" to them.
	for d in $(WINE_PE_DLLS); do \
	    cp $(WINE_BUILD)/dlls/$$d/i386-windows/$$d.dll \
	       $(BUILD_DIR)/wine_staging/$$d.dll 2>/dev/null || true; \
	done
	for p in $(WINE_PE_PROGS); do \
	    cp $(WINE_BUILD)/programs/$$p/i386-windows/$$p.exe \
	       $(BUILD_DIR)/wine_staging/$$p.exe 2>/dev/null || true; \
	done
	# The Unix halves of the builtins above - see WINE_UNIX_DLLS.
	for d in $(WINE_UNIX_DLLS); do \
	    cp $(WINE_BUILD)/dlls/$$d/$$d.so \
	       $(BUILD_DIR)/wine_staging/$$d.so 2>/dev/null || true; \
	done
	test -z "$(WINE_STRIP)" || i686-w64-mingw32-strip \
	    $(BUILD_DIR)/wine_staging/*.dll \
	    $(BUILD_DIR)/wine_staging/*.exe 2>/dev/null || true
	strip $(BUILD_DIR)/wine_staging/ntdll.so $(BUILD_DIR)/wine_staging/*.so \
	      $(BUILD_DIR)/wine_staging/wineserver 2>/dev/null || true
	# glibc's getpwuid() needs these, and Wine dereferences its result
	# without checking. The initrd is flat, so they land at the root and
	# the last-component fallback resolves /etc/passwd to them.
	printf 'root:x:0:0:root:/:/bin/sh\n' > $(BUILD_DIR)/wine_staging/passwd
	printf 'passwd: files\ngroup: files\n' > $(BUILD_DIR)/wine_staging/nsswitch.conf
	# Milestone 31. glibc unwinds a thread out of itself with
	# _Unwind_ForcedUnwind, which lives in libgcc_s.so.1 and is reached by
	# dlopen()ing it the first time pthread_exit() is called - so a
	# library nothing links against is a hard requirement for a *thread*
	# to end. Without it glibc prints "libgcc_s.so.1 must be installed
	# for pthread_exit to work" and aborts, which is what every Wine
	# worker thread did the moment it returned. Copied from the host
	# toolchain, like ld-linux.so.2 and libc.so.6 and for the same
	# reasons.
	cp $(HOST_LIB32)/libgcc_s.so.1 $(BUILD_DIR)/wine_staging/libgcc_s.so.1
	# Milestone 32. Every other shared library anything here links
	# against, worked out rather than listed.
	#
	# The hand-written list was wrong and had been for two milestones:
	# win32u.so needs libm.so.6 and nothing shipped it, so the Unix half
	# of win32u could not be dlopen'd - and win32u is the Unix backend
	# for both gdi32 and user32. Wine said so, once, in a warning inside
	# a trace channel nobody had turned on:
	#
	#   warn:module:get_builtin_unix_funcs failed to load
	#     "//i386-unix/win32u.so": libm.so.6: cannot open shared object file
	#
	# The same lesson as the NLS tables above, so the same answer: ask the
	# binaries what they need instead of remembering. objdump reports the
	# DT_NEEDED entries; anything already staged is skipped, and anything
	# that is not in HOST_LIB32 is left to announce itself at run time
	# rather than silently breaking the build.
	@for f in $(BUILD_DIR)/wine_staging/*.so $(BUILD_DIR)/wine_staging/wineserver \
	          $(BUILD_DIR)/wine_staging/wine $(BUILD_DIR)/wine_staging/preload.elf; do \
	    objdump -p $$f 2>/dev/null | awk '/NEEDED/ { print $$2 }'; \
	done | sort -u | while read lib; do \
	    test -f $(BUILD_DIR)/wine_staging/$$lib && continue; \
	    test -f $(HOST_LIB32)/$$lib || continue; \
	    echo "  + $$lib (needed by a Wine .so)"; \
	    cp $(HOST_LIB32)/$$lib $(BUILD_DIR)/wine_staging/$$lib; \
	done
	python3 userland/mkinitrd.py $(BUILD_DIR)/wine_staging \
	    $(BUILD_DIR)/wine_iso/boot/initrd.img
	cp $(BUILD_DIR)/novaris.bin $(BUILD_DIR)/wine_iso/boot/novaris.bin
	cp grub.cfg $(BUILD_DIR)/wine_iso/boot/grub/grub.cfg
	grub-mkrescue -o novaris-wine.iso $(BUILD_DIR)/wine_iso 2>/dev/null
	@echo "Wrote novaris-wine.iso - try: run wine-preloader wine hellowin.exe"

# Boots novaris-wine.iso and asserts that a real Windows .exe runs under
# real Wine. Separate from `make test-qemu` for the same reason
# novaris-wine.iso is separate from novaris.iso: it needs a built Wine
# tree, and this repository must stay buildable without one.
#
#   make WINE_BUILD=../wine wine-initrd test-wine
#
# 768MB because the initrd carries Wine's PE builtins and the NLS tables
# and is read into RAM whole, and because a fork of a Wine process copies
# its address space eagerly.
# `symlinks off` first, and it is the interesting line in this file.
#
# Milestone 32 built symbolic links and switched them on, because what
# they unlock - Wine's DOS drive table, and with it the startup path a
# real installation takes - was waiting on a disk and a prefix, and both
# now exist. Turning them on is what the milestone was for.
#
# It is also what stops this test. With drives, Wine stops falling back to
# the Windows directory and does what it does on a real machine: it finds
# the prefix, starts wineserver, starts services.exe, loads its builtins
# off the disk - and then wineboot stops inside shell32's delay-load of
# SHGetFolderPathW and does not come back. The Windows program never runs.
#
# So this test says which of the two paths it is testing instead of
# leaving it to a default. The kernel's default is that symlinks work,
# because they do and because userland/fs_test.c compares them against
# Linux byte for byte; the transcript test that needs Wine's *older* path
# asks for it. `make test-wine-prefix` is the other half - it asserts how
# far the new path gets, so the progress is a test rather than a claim.
#
# Milestone 33 is what removes this line. See ROADMAP.md.
test-wine:
	@test -f novaris-wine.iso || \
	    { echo "run 'make WINE_BUILD=/path/to/wine wine-initrd' first"; exit 1; }
	python3 tools/qemu_test.py --iso novaris-wine.iso --memory 768 \
	    --boot-wait 30 --settle 200 \
	    --setup "symlinks off" \
	    --cmd "run wine-preloader wine hellowin.exe" \
	    --expect "Hello from a real Windows \.exe running on Novaris" \
	    --expect "compiled by mingw-w64, linked against msvcrt\.dll" \
	    --expect "floats:     3\.141593 2\.50 1\.234568e\+04 0\.0001" \
	    --expect "fib\(20\):    6765" \
	    --expect "argv\[0\]: C:.windows.hellowin\.exe" \
	    --expect "Exiting with code 0" \
	    --reject "KERNEL PANIC"

# Milestone 31: the same, for a *threaded* Windows program. threads.exe is
# the Milestone 17 binary - CreateThread, WaitForSingleObject and a
# CRITICAL_SECTION - running through Wine's own kernel32 and ntdll rather
# than Novaris's Win32 layer, so every Win32 thread is a real pthread on a
# real clone().
#
# The two counters are the point. The interlocked one proves the workers
# all ran and their increments all landed; the guarded one proves the
# critical section serialised them, because it is incremented with a
# deliberately racy read-modify-write.
#
# What is deliberately *not* asserted is the absence of
# "err:sync:RtlpWaitForCriticalSection ... wait timed out". Wine prints
# that when a lock wait reaches its timeout, which is what a thread
# starved of the CPU looks like from inside Wine. Milestone 31 took it
# from three a run to none, then wineboot started getting far enough to
# launch services.exe and it came back at about one - the machine is
# simply busier now, and one wait in a run still loses five seconds
# somewhere. It is a latency defect and the counters above prove it is
# not a correctness one; asserting its absence would be asserting
# something that is not reliably true. See ROADMAP.md Milestone 31.
# `symlinks off` for the reason spelled out above test-wine.
test-wine-threads:
	@test -f novaris-wine.iso || \
	    { echo "run 'make WINE_BUILD=/path/to/wine wine-initrd' first"; exit 1; }
	python3 tools/qemu_test.py --iso novaris-wine.iso --memory 768 \
	    --boot-wait 30 --settle 200 \
	    --setup "symlinks off" \
	    --cmd "run wine-preloader wine threads.exe" \
	    --expect "Win32 threads test - real CreateThread on Novaris" \
	    --expect "worker 1 starting, GetCurrentThreadId" \
	    --expect "worker 2 starting, GetCurrentThreadId" \
	    --expect "worker 3 starting, GetCurrentThreadId" \
	    --expect "all 3 workers joined" \
	    --expect "worker 3 exit code 300" \
	    --expect "interlocked counter: 60, expected 60  -> ok" \
	    --expect "guarded counter:     60, expected 60  -> ok" \
	    --expect "threads test done" \
	    --reject "libgcc_s\.so\.1 must be installed" \
	    --reject "KERNEL PANIC"

# --- The disk (Milestone 32) ----------------------------------------------
#
# `make disk` builds a small empty FAT32 volume; `make run-disk` boots the
# ordinary ISO with it attached, which is all it takes to have somewhere
# that survives a reboot.
#
# The image is *not* a build product of the ISO and is deliberately not
# rebuilt by `make`: it is storage, and rebuilding storage on a `make`
# would be the one behaviour nobody wants from a disk. Delete it to start
# over.
DISK_IMAGE = novaris-disk.img
DISK_SIZE  = 256M

disk:
	@test -f $(DISK_IMAGE) || \
	    python3 tools/mkfat32.py $(DISK_IMAGE) --size $(DISK_SIZE) --label NOVARIS
	@echo "$(DISK_IMAGE) is ready - boot with: make run-disk"

.PHONY: run-disk
run-disk: $(ISO) disk
	qemu-system-i386 -cdrom $(ISO) -boot d \
	    -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk

# The only test of a filesystem that means anything: write, reboot, look.
#
# Two boots of the same image, and the second one is where the assertions
# are. The first writes a file, a directory, a nested file and a symbolic
# link and syncs; the second reads them back on a machine that has been
# switched off and on in between, so nothing can have been served out of a
# cache that did not survive.
test-qemu-disk: $(ISO)
	rm -f $(BUILD_DIR)/persist.img
	python3 tools/mkfat32.py $(BUILD_DIR)/persist.img --size 64M --label PERSIST
	python3 tools/qemu_test.py --iso $(ISO) --disk $(BUILD_DIR)/persist.img \
	    --boot-wait 16 --settle 3 --timeout 40 \
	    --cmd "mkdir /disk/afterboot" \
	    --cmd "cp readme.txt /disk/afterboot/kept.txt" \
	    --cmd "ln -s afterboot/kept.txt /disk/shortcut" \
	    --cmd "sync" \
	    --expect "linked /disk/shortcut" \
	    --expect "^synced" \
	    --reject "KERNEL PANIC"
	python3 tools/qemu_test.py --iso $(ISO) --disk $(BUILD_DIR)/persist.img \
	    --boot-wait 16 --settle 3 --timeout 40 \
	    --cmd "ls /disk" \
	    --cmd "ls /disk/afterboot" \
	    --cmd "cat /disk/shortcut" \
	    --cmd "df" \
	    --expect "FAT32 volume mounted at /disk" \
	    --expect "afterboot/  \\(directory\\)" \
	    --expect "shortcut -> afterboot/kept\\.txt" \
	    --expect "kept\\.txt" \
	    --expect "Novaris" \
	    --expect "label \\\"PERSIST\\\"" \
	    --reject "KERNEL PANIC"

# Builds a disk carrying a Wine prefix, from a prefix directory built
# elsewhere. There is no way to build one on Novaris yet - that is what
# Milestone 33 is about - so the prefix comes from a host that has the
# same Wine tree:
#
#   WINEPREFIX=/tmp/pfx ../wine/loader/wine wineboot -u
#   make WINE_PREFIX_DIR=/tmp/pfx wine-disk
#
# 512MB because a 32-bit prefix is a little over 200MB once the PE files
# are stripped, and a filesystem wants room to write in.
WINE_PREFIX_DIR =
WINE_DISK_IMAGE = novaris-wine-disk.img
WINE_DISK_SIZE  = 512M

wine-disk:
	@test -n "$(WINE_PREFIX_DIR)" || \
	    { echo "set WINE_PREFIX_DIR=/path/to/a/wine/prefix (see the Makefile)"; exit 1; }
	@test -d "$(WINE_PREFIX_DIR)/drive_c" || \
	    { echo "$(WINE_PREFIX_DIR) does not look like a Wine prefix"; exit 1; }
	rm -rf $(BUILD_DIR)/disk_staging
	mkdir -p $(BUILD_DIR)/disk_staging
	cp -a $(WINE_PREFIX_DIR) $(BUILD_DIR)/disk_staging/.wine
	# Stripped for the same reason the initrd's builtins would be: debug
	# information is three quarters of the bytes and nothing here reads
	# it. 700MB becomes 220MB, and the difference is a disk image that
	# can be moved between machines.
	find $(BUILD_DIR)/disk_staging -name '*.dll' -o -name '*.exe' \
	    -o -name '*.drv' -o -name '*.sys' \
	    | xargs -r -n 40 i686-w64-mingw32-strip 2>/dev/null || true
	python3 tools/mkfat32.py $(WINE_DISK_IMAGE) $(BUILD_DIR)/disk_staging \
	    --size $(WINE_DISK_SIZE) --label WINEDISK
	@echo "Wrote $(WINE_DISK_IMAGE) - the prefix lands at /disk/.wine, which"
	@echo "is where HOME points when a disk is mounted."

# How far Wine's *real* startup path gets, with symlinks on and a prefix
# on the disk. Separate from test-wine because it asserts something
# different: not that a Windows program runs - it does not yet - but that
# every step before the one that stops is now reached.
#
# What it proves, in order: the drive table exists (Wine names the prefix
# as Z:\disk\.wine rather than reporting that it cannot find a DOS drive
# for the working directory, which is what Milestones 30 and 31 got);
# wineserver starts; services.exe starts and works through the auto-start
# list; and Wine's PE builtins load out of the prefix on the disk.
#
# What it deliberately does not assert is the absence of the failures
# after that point. wineboot stops in shell32's delay-load of
# SHGetFolderPathW and the transcript says so. Asserting a clean run would
# be asserting something untrue; asserting the failure line would freeze
# in place the exact thing Milestone 33 has to change.
test-wine-prefix:
	@test -f novaris-wine.iso || \
	    { echo "run 'make WINE_BUILD=/path/to/wine wine-initrd' first"; exit 1; }
	@test -f $(WINE_DISK_IMAGE) || \
	    { echo "run 'make WINE_PREFIX_DIR=/path/to/prefix wine-disk' first"; exit 1; }
	cp $(WINE_DISK_IMAGE) $(BUILD_DIR)/wine-disk-run.img
	python3 tools/qemu_test.py --iso novaris-wine.iso \
	    --disk $(BUILD_DIR)/wine-disk-run.img --memory 768 \
	    --boot-wait 30 --settle 200 --timeout 260 \
	    --cmd "run wine-preloader wine hellowin.exe" \
	    --expect "FAT32 volume mounted at /disk" \
	    --expect "Z:.*disk.*wine" \
	    --expect "scmdatabase_autostart_services" \
	    --expect "C:.*windows.*system32" \
	    --reject "could not find DOS drive" \
	    --reject "KERNEL PANIC"

zip:
	rm -rf $(BUILD_DIR)/pkg novaris.zip
	mkdir -p $(BUILD_DIR)/pkg/novaris
	cp -r $(ZIP_CONTENTS) $(BUILD_DIR)/pkg/novaris/
	cd $(BUILD_DIR)/pkg && zip -q -r $(CURDIR)/novaris.zip novaris
	rm -rf $(BUILD_DIR)/pkg
	@echo "Wrote novaris.zip"

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR) $(ISO) serial.log screenshot.png
