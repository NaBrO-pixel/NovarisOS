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
       $(BUILD_DIR)/wmdev.o \
       $(BUILD_DIR)/pci.o $(BUILD_DIR)/rtl8139.o \
       $(BUILD_DIR)/net.o $(BUILD_DIR)/dhcp.o \
       $(BUILD_DIR)/tcp.o $(BUILD_DIR)/dns.o $(BUILD_DIR)/http.o \
       $(BUILD_DIR)/update.o \
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
        test-wine-threads test-qemu-disk test-wine-prefix test-desktop \
        test-wine-persist test-inet test-listen test-winsock \
        zip disk

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

$(BUILD_DIR)/pci.o: kernel/pci.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rtl8139.o: kernel/rtl8139.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/net.o: kernel/net.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dhcp.o: kernel/dhcp.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/update.o: kernel/update.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tcp.o: kernel/tcp.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dns.o: kernel/dns.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/http.o: kernel/http.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wmdev.o: kernel/wmdev.c $(HEADERS) | $(BUILD_DIR)
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
#   cd wine && CC="gcc -m32" ./configure --enable-archs=i386 \
#       --disable-tests --without-x --without-vulkan --without-opengl
#   make -j4
#   cd ../NovarisOS && make WINE_BUILD=../wine
#
# Not --without-freetype, however headless this looks. Without a font
# engine win32u reads the caption font's tm.tmHeight as garbage, derives
# SM_CYCAPTION from it, and every window comes out 952x1 with no error
# printed anywhere. See the note in README.md.
#
# With WINE_BUILD set, the ordinary `make` installs Wine into the OS image
# it builds - see tools/install_wine.sh and the initrd rule below. Without
# it, the same `make` builds the same OS with no Wine in it. Switching
# back needs a `make clean`: the staging directory is built up rather than
# rebuilt, so an install already in it stays until it is wiped.
WINE_BUILD ?=
WINE_STRIP ?= 1

# Which of Wine's 601 DLLs ship is decided in tools/install_wine.sh, and
# only there.
#
# Three variables used to be defined here - WINE_PE_DLLS, WINE_PE_PROGS
# and WINE_UNIX_DLLS - naming the same things the script names. Nothing
# ever read them. They were a second copy of a list that had already
# drifted from the real one (the script had gained comctl32, comdlg32,
# winspool.drv and five more programs; this copy had not), and the way
# that drift would have been discovered is somebody adding a DLL here,
# rebuilding, and finding the initrd unchanged. Milestone 43 deleted
# them rather than wiring them up: the script is runnable on its own and
# has to hold the list anyway, so a second copy can only ever be wrong.
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

# Milestone 41: the same socketcall ABI, but AF_INET - a TCP connection
# opened by a *process* rather than by the shell. Unlike the other
# programs here it is not run on the host and diffed, because it needs a
# server to talk to and the two machines do not have the same one; what
# it is compared against is the transcript `make test-inet` expects.
$(BUILD_DIR)/user/inettest.elf: userland/inet_test.c | $(BUILD_DIR)
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

# Milestone 36: a ring-3 process that owns a window on the desktop, via
# /dev/wm. The first thing outside the kernel ever to draw one.
$(BUILD_DIR)/user/wmtest.elf: userland/wm_test.c | $(BUILD_DIR)
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
# Milestone 41's open question, as a program. Whether a *Windows* binary
# can reach the network now that Novaris has BSD sockets: Wine's ws2_32
# Unix half implements AFD on top of socket()/connect()/send(), which are
# exactly the calls that arrived with AF_INET. Nothing about that path is
# obviously complete, so this runs it.
$(BUILD_DIR)/user/winsock.exe: userland/pe_test/winsock.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/user
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ $< -lws2_32

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
        $(BUILD_DIR)/user/socktest.elf $(BUILD_DIR)/user/inettest.elf \
        $(BUILD_DIR)/user/forktest.elf \
        $(BUILD_DIR)/user/mmaptest.elf $(BUILD_DIR)/user/wmtest.elf \
        $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/user/dyn.elf \
        $(BUILD_DIR)/user/uctest.elf \
        $(BUILD_DIR)/user/ld-linux.so.2 $(BUILD_DIR)/user/libc.so.6 \
        $(BUILD_DIR)/user/hello_pe.exe \
        $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/user/winapi.exe \
        $(BUILD_DIR)/user/winsock.exe \
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
	cp $(BUILD_DIR)/user/inettest.elf $(BUILD_DIR)/initrd_staging/inettest.elf
	cp $(BUILD_DIR)/user/forktest.elf $(BUILD_DIR)/initrd_staging/forktest.elf
	cp $(BUILD_DIR)/user/mmaptest.elf $(BUILD_DIR)/initrd_staging/mmaptest.elf
	cp $(BUILD_DIR)/user/wmtest.elf $(BUILD_DIR)/initrd_staging/wmtest.elf
	cp $(BUILD_DIR)/user/crashelf.elf $(BUILD_DIR)/initrd_staging/crashelf.elf
	cp $(BUILD_DIR)/user/glibc.elf $(BUILD_DIR)/initrd_staging/glibc.elf
	cp $(BUILD_DIR)/user/dyn.elf $(BUILD_DIR)/initrd_staging/dyn.elf
	cp $(BUILD_DIR)/user/uctest.elf $(BUILD_DIR)/initrd_staging/uctest.elf
	cp $(BUILD_DIR)/user/ld-linux.so.2 $(BUILD_DIR)/initrd_staging/ld-linux.so.2
	cp $(BUILD_DIR)/user/libc.so.6 $(BUILD_DIR)/initrd_staging/libc.so.6
	cp $(BUILD_DIR)/user/hello_pe.exe $(BUILD_DIR)/initrd_staging/hellope.exe
	cp $(BUILD_DIR)/user/hellowin.exe $(BUILD_DIR)/initrd_staging/hellowin.exe
	cp $(BUILD_DIR)/user/winapi.exe $(BUILD_DIR)/initrd_staging/winapi.exe
	cp $(BUILD_DIR)/user/winsock.exe $(BUILD_DIR)/initrd_staging/winsock.exe
	cp $(BUILD_DIR)/user/cppinit.exe $(BUILD_DIR)/initrd_staging/cppinit.exe
	cp $(BUILD_DIR)/user/guiapp.exe $(BUILD_DIR)/initrd_staging/guiapp.exe
	cp $(BUILD_DIR)/user/hello64.exe $(BUILD_DIR)/initrd_staging/hello64.exe
	cp $(BUILD_DIR)/user/lowbase.exe $(BUILD_DIR)/initrd_staging/lowbase.exe
	cp $(BUILD_DIR)/user/crash.exe $(BUILD_DIR)/initrd_staging/crash.exe
	cp $(BUILD_DIR)/user/qsorttest.exe $(BUILD_DIR)/initrd_staging/qsorttest.exe
	cp $(BUILD_DIR)/user/threads.exe $(BUILD_DIR)/initrd_staging/threads.exe

# Wine, installed into the OS image rather than bolted onto a second one.
#
# Milestone 35. Through Milestone 34 there were two ISOs: novaris.iso, and
# a novaris-wine.iso built by a separate target that dumped Wine's files
# at the root of a flat initrd. That is Wine as a tool you carry around
# next to an OS. An operating system that runs Windows programs has Wine
# *installed in it*, at /usr/lib/wine and /usr/share/wine, the way any
# other Unix does - which is also, not coincidentally, the only layout
# Wine's own path arithmetic gets right. See tools/install_wine.sh.
#
# One image, then. With WINE_BUILD pointing at a built Wine tree, `make`
# produces a novaris.iso with Wine in it; without, it produces the same
# OS without Wine, and the `wine` command in the shell says so rather
# than failing three layers down. Nothing else about the build changes.
$(BUILD_DIR)/wine-installed.stamp: $(BUILD_DIR)/initrd_staging/helloelf.elf \
                                   tools/install_wine.sh tools/build_wine_driver.sh \
                                   $(wildcard wine/winenovaris.drv/*)
ifeq ($(strip $(WINE_BUILD)),)
	@echo "No WINE_BUILD set - building without Wine."
	@echo "  (set WINE_BUILD=/path/to/a/built/wine to install it into the OS)"
	@touch $@
else
	sh tools/build_wine_driver.sh $(WINE_BUILD)
	HOST_LIB32=$(HOST_LIB32) sh tools/install_wine.sh \
	    $(WINE_BUILD) $(BUILD_DIR)/initrd_staging $(WINE_STRIP)
	@touch $@
endif

$(BUILD_DIR)/initrd.img: $(BUILD_DIR)/wine-installed.stamp userland/mkinitrd.py
	python3 userland/mkinitrd.py $(BUILD_DIR)/initrd_staging $@ > $(BUILD_DIR)/initrd.list
	@echo "initrd: $$(grep -c '^  ' $(BUILD_DIR)/initrd.list) files, $$(du -h $@ | cut -f1)"

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

# Milestone 36: a window on the desktop that the kernel did not draw.
#
# This is the only test here whose real assertion is a picture. The serial
# transcript proves the mechanism - the device opened, the window was
# created, its pixels mapped, a frame damaged, and input arrived - but it
# cannot prove that anything appeared, and appearing is the point. So the
# run ends with a screendump, and build/wm-test.ppm is the artifact.
#
# The pointer work is what generates the input: the window opens centered,
# and moving the pointer across the middle of the screen crosses it, which
# the window manager routes to the process as WM_EV_MOUSE. "events
# received: 0" would mean a window nothing could reach.
#
# The program draws for a minute and then gives up on its own, so the run
# is bounded whether or not anything closes it.
test-wm: $(ISO)
	python3 tools/qemu_test.py --iso $(ISO) --memory 512 \
	    --boot-wait 30 --settle 40 --timeout 200 \
	    --cmd "run wmtest.elf" \
	    --post-click "640,400,move" \
	    --post-click "660,410,move" \
	    --post-click "620,390,move" \
	    --click-settle 2 \
	    --screenshot $(BUILD_DIR)/wm-test.ppm \
	    --expect "opened /dev/wm" \
	    --expect "created a window" \
	    --expect "it is the size we asked for" \
	    --expect "mapped its pixels" \
	    --expect "showed the first frame" \
	    --expect "input reached the window" \
	    --reject "\[FAIL\]" \
	    --reject "KERNEL PANIC"
	@python3 tools/check_wm_window.py $(BUILD_DIR)/wm-test.ppm
	@echo
	@echo "--- and the same program, allowed to finish, twice ---"
# Three seconds each rather than a minute, so the run reaches the exit
# path: the descriptor closes, the node's last reference goes, the slot
# is released and the window disappears. Twice, because the second open
# has to be handed the slot the first one gave back - a leak here would
# show up as the second run failing to create a window at all.
	python3 tools/qemu_test.py --iso $(ISO) --memory 512 \
	    --boot-wait 30 --settle 15 --timeout 200 \
	    --cmd "run wmtest.elf 60" \
	    --cmd "run wmtest.elf 20" \
	    --expect "frames drawn: 60" \
	    --expect "frames drawn: 20" \
	    --expect "wm test done" \
	    --expect "Back in the shell" \
	    --reject "\[FAIL\]" \
	    --reject "KERNEL PANIC"
	@echo
	@echo "--- and Milestone 42: the window resizes ---"
# Win-Up maximizes the focused window, and maximizing is a resize: the
# client area goes from 480x320 to the whole work area, and the program
# prints the WM_EV_RESIZE it gets.
#
# It is a keystroke rather than a corner drag because a corner drag could
# not be aimed. The resize border is RESIZE_BORDER = 5 pixels inside the
# window (kernel/wm.c), and Novaris's mouse is a *relative* PS/2 device,
# so the harness positions the pointer by slamming it into a corner and
# counting deltas out from there - which is accurate enough to hit a
# window, and not accurate enough to hit five pixels of it. Two attempts
# at the drag landed in the client area instead and were delivered to the
# program as ordinary mouse input, which is exactly what "input reached
# the window" in the first stanza above means.
#
# (tools/qemu_test.py grew a real drag primitive on the way - press,
# move without re-slamming, release. It is still the right way to test a
# drag; it is not the right way to test a resize.)
#
# What this proves that the stride check above cannot: that the buffer
# outlives the resize. Before Milestone 42 the mapping was exactly the
# client area, so growing the window past it would have had to move the
# mapping - which is the thing there was no safe moment to do.
	python3 tools/qemu_test.py --iso $(ISO) --memory 512 \
	    --boot-wait 30 --settle 30 --timeout 260 \
	    --cmd "run wmtest.elf 1200" \
	    --post-key "meta_l-up" \
	    --click-settle 3 \
	    --screenshot $(BUILD_DIR)/wm-resize.ppm \
	    --expect "resized to" \
	    --reject "\[FAIL\]" \
	    --reject "KERNEL PANIC"

test-qemu: $(ISO)
	python3 tools/qemu_test.py --iso $(ISO) --script tools/tests/win32_smoke.txt \
	    --memory 512 --boot-wait 22 --settle 5 \
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

# Boots the OS and asserts that a real Windows .exe runs under real Wine.
#
# Since Milestone 35 this is the *default* configuration and the test says
# so by asking for nothing: one ISO, no disk, no setup commands, symbolic
# links on, and `wine hellowin.exe` typed at the shell. Wine is installed
# in the image at /usr/lib/wine and /usr/share/wine, so it finds wine.inf,
# builds its own prefix in /root/.wine, starts wineserver, runs wineboot
# and services.exe, and runs the program.
#
# Two lines of the transcript are worth knowing about because they look
# like failures and are not. "err:winediag:nodrv_CreateWindow" is Wine
# having no display backend to load, which is true and is a milestone of
# its own. "err:environ:run_wineboot boot event wait timed out" is
# wineboot taking longer than the five minutes Wine allows it - the run
# then carries on and works.
#
# `argv[0]: Z:\hellowin.exe` is the assertion that says the prefix really
# was used: `Z:\` is the DOS drive table, built out of symbolic links in
# the prefix. Every milestone before 34 got `C:\windows\hellowin.exe`,
# which is the fallback Wine takes when it has no drives at all.
#
# 768MB because the initrd carries Wine and is read into RAM whole, and
# because a fork of a Wine process copies its address space eagerly.
# Fifteen minutes of settle with --stop-when-matched, so only a failing
# run ever spends it.
test-wine: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --cmd "wine hellowin.exe" \
	    --expect "floats:     3\.141593 2\.50 1\.234568e\+04 0\.0001" \
	    --expect "fib\(20\):    6765" \
	    --expect "argv\[0\]: Z:.hellowin\.exe" \
	    --expect "Exiting with code 0" \
	    --reject "No Wine installed" \
	    --reject "could not load ntdll\.so" \
	    --reject "wine\.inf not found" \
	    --reject "\[fat32\] cannot create" \
	    --reject "\[ata\]" \
	    --reject "Failed to get shared session object" \
	    --reject "Too many open files" \
	    --reject "unimplemented syscall" \
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
# Same default configuration as test-wine - see the comment there.
#
# The three "worker N starting, GetCurrentThreadId() = ..." lines are
# deliberately *not* asserted, though they are printed. Several Wine
# processes share COM1 and a line from one lands in the middle of a line
# from another, and the ones most likely to be cut in half are the ones
# printed while the other threads are still noisy - a run containing
# "  worker 1 star0024:err:sync:RtlpWaitForCriticalSection ..." is a run
# that worked. The two counters below are the proof that all three ran
# anyway: the interlocked one only reaches 60 if every increment landed.
test-wine-threads: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --cmd "wine threads.exe" \
	    --expect "Win32 threads test - real CreateThread on Novaris" \
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
# Milestone 41. The one test that has to be run rather than reasoned
# about: a ring-3 process opening a TCP connection, and then asking a
# real nameserver a real question over UDP. 10.0.2.3 is QEMU's own DNS
# forwarder, so the second half needs a working resolver on this machine
# and nothing more.
#
# The server is this machine. tools/qemu_test.py serves --http-dir from
# the build host, which QEMU's user-mode stack presents to the guest as
# 10.0.2.2 - both its default gateway and us - so the test needs nothing
# from the outside world and cannot fail because the outside world is
# down.
#
# The same binary runs on the Linux host (build/user/inettest.elf
# 127.0.0.1 <port> against any local server) and prints the same lines,
# which is the point: nothing in it knows what a Novaris is.
test-inet: $(ISO)
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 40 --settle 60 --timeout 260 \
	    --http-dir $(BUILD_DIR)/test/www \
	    --setup "net up" \
	    --cmd 'run inettest.elf 10.0.2.2 $$HTTP 10.0.2.3 example.com' \
	    --expect "\\[ok\\] connect" \
	    --expect "\\[ok\\] HTTP/1.0 200 OK" \
	    --expect "\\[ok\\] udp socket" \
	    --expect "answer\\(s\\)" \
	    --expect "a process opened a TCP connection" \
	    --reject "KERNEL PANIC" \
	    --stop-when-matched

# Milestone 41, the other direction: a *server* on Novaris.
#
# QEMU's user-mode stack blocks inbound connections, so the guest's
# listening port is forwarded to this machine's loopback (--hostfwd) and
# the client runs out here (--host-connect). That split is not an
# awkwardness of the harness, it is the test: a connection has to be
# opened from somewhere that is not the machine being tested, or the
# three-way handshake never has Novaris on the answering side.
test-listen: $(ISO)
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 40 --settle 90 --timeout 220 \
	    --hostfwd 18080:8080 --host-connect 18080 \
	    --setup "net up" \
	    --cmd 'run inettest.elf listen 8080' \
	    --expect "\\[ok\\] listening on 8080" \
	    --expect "accepted a connection from port" \
	    --expect "a program on this machine accepted a connection" \
	    --reject "KERNEL PANIC" \
	    --stop-when-matched

# Milestone 41's last question: a *Windows* program on the network.
#
# Not a Novaris binary using Novaris sockets, but a mingw-built .exe
# linked against ws2_32, running under real Wine, doing WSAStartup and
# connect and recv. Wine's ws2_32 implements AFD on top of BSD sockets,
# so this passes or fails on whether the socket layer underneath is
# complete enough for somebody else's implementation to sit on - which
# is a stronger question than whether it satisfies its own test.
#
# The settle is long because Wine's startup dominates it; --stop-when-
# matched means a passing run ends as soon as the last line appears.
test-winsock: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 40 --settle 700 --timeout 850 \
	    --http-dir $(BUILD_DIR)/test/www \
	    --setup "net up" \
	    --cmd 'wine winsock.exe 10.0.2.2 $$HTTP' \
	    --expect "\\[ok\\] WSAStartup" \
	    --expect "\\[ok\\] connect" \
	    --expect "\\[ok\\] HTTP/1.0 200 OK" \
	    --expect "a Windows program reached the network" \
	    --reject "KERNEL PANIC" \
	    --stop-when-matched

test-winsock: $(BUILD_DIR)/test/www

# A directory for test-inet's server to serve. One small file is enough:
# what is being tested is the socket, not the transfer.
$(BUILD_DIR)/test/www: | $(BUILD_DIR)
	mkdir -p $@
	printf 'Novaris reached this from a process.\n' > $@/index.html

test-inet: $(BUILD_DIR)/test/www

test-qemu-disk: $(ISO)
	rm -f $(BUILD_DIR)/persist.img
	python3 tools/mkfat32.py $(BUILD_DIR)/persist.img --size 64M --label PERSIST
	python3 tools/qemu_test.py --iso $(ISO) --disk $(BUILD_DIR)/persist.img \
	    --memory 512 --boot-wait 22 --settle 3 --timeout 50 \
	    --cmd "mkdir /disk/afterboot" \
	    --cmd "cp readme.txt /disk/afterboot/kept.txt" \
	    --cmd "ln -s afterboot/kept.txt /disk/shortcut" \
	    --cmd "sync" \
	    --expect "linked /disk/shortcut" \
	    --expect "^synced" \
	    --reject "KERNEL PANIC"
	python3 tools/qemu_test.py --iso $(ISO) --disk $(BUILD_DIR)/persist.img \
	    --memory 512 --boot-wait 22 --settle 3 --timeout 50 \
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

# Milestone 35 removed `make wine-disk` and WINE_PREFIX_DIR. They existed
# because Novaris could not build a Wine prefix and one had to be built by
# the same Wine on a host and written into a disk image. It can now: with
# Wine installed in the OS, wineboot finds wine.inf and runs rundll32 over
# it exactly as it would anywhere else. A disk is still where a prefix
# ought to live, because a prefix is an installation - but it is Wine that
# puts it there now, on first use, and `make disk` is all it takes to have
# somewhere for it to go.

# The prefix is an installation, so: build one, switch the machine off,
# switch it on, and use it.
#
# Two boots of one disk image, and the assertions are in the second. The
# first is an empty FAT32 volume: Wine finds no prefix, builds one, runs
# the program, and syncs. The second must run the program *without*
# building anything - "created the configuration directory" is rejected,
# because a prefix that is rebuilt every time is not an installation, it
# is a cache with extra steps.
#
# It is also the run that shows what a prefix is worth: the second boot
# does not run rundll32 over wine.inf, so it is several minutes shorter
# than the first. Nothing asserts that, because a stopwatch is not a
# property, but it is why this arrangement is the one a person would use.
test-wine-persist: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	rm -f $(BUILD_DIR)/wine-persist.img
	python3 tools/mkfat32.py $(BUILD_DIR)/wine-persist.img --size 512M --label WINEDISK
	@echo "--- first boot: an empty disk, so Wine builds its prefix ---"
	python3 tools/qemu_test.py --iso $(ISO) \
	    --disk $(BUILD_DIR)/wine-persist.img --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --cmd "wine hellowin.exe" \
	    --post-cmd "sync" \
	    --expect "created the configuration directory '/disk/\.wine'" \
	    --expect "Exiting with code 0" \
	    --reject "KERNEL PANIC"
	@echo "--- second boot: the same disk, and the prefix is already there ---"
	python3 tools/qemu_test.py --iso $(ISO) \
	    --disk $(BUILD_DIR)/wine-persist.img --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --cmd "wine hellowin.exe" \
	    --expect "FAT32 volume mounted at /disk" \
	    --expect "fib\(20\):    6765" \
	    --expect "argv\[0\]: Z:.hellowin\.exe" \
	    --expect "Exiting with code 0" \
	    --reject "created the configuration directory" \
	    --reject "wine\.inf not found" \
	    --reject "\[fat32\] cannot create" \
	    --reject "KERNEL PANIC"

# Double-clicking a .exe, which is the one thing no amount of typing at
# the shell can test.
#
# Three pointer gestures, no keyboard at all: double-click the File
# Explorer icon on the desktop, click the maximize button so the whole
# listing fits, and double-click hellowin.exe. What has to come back is the
# program's own output - which means the desktop routed the double-click to
# app_open_path(), app_open_path() saw a .exe and Wine installed, and Wine
# ran it.
#
# The coordinates are a screen layout and therefore brittle, and there is
# no way around that: they are what a user's hand does. They were read off
# a screenshot, and that is how to redo them if the desktop moves -
# `qemu-system-i386 ... -monitor tcp:...` and `screendump out.ppm` at the
# monitor prompt gives the picture the numbers came from.
#
#   60,233   the File Explorer desktop icon (third in the left column)
#   881,66   the maximize button on the window it opens
#   300,478  the hellowin.exe row in the maximized listing
test-desktop: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --click-settle 3 \
	    --click "60,233,double" \
	    --click "881,66" \
	    --click "300,478,double" \
	    --expect "starting hellowin\.exe under Wine" \
	    --expect "fib\(20\):    6765" \
	    --expect "argv\[0\]: Z:.hellowin\.exe" \
	    --expect "Exiting with code 0" \
	    --reject "KERNEL PANIC"

# Milestone 37: a Windows program with a window, under real Wine.
#
# Everything `make test-wine` asserts is about a *console* program - what
# it printed, and which prefix it printed it from. This is the other kind,
# and the only place its result exists is on the screen: winenovaris.drv
# hands Wine a surface, GDI draws into it, the driver copies it into a
# /dev/wm mapping and the compositor blits it. Nothing in that chain
# prints anything.
#
# So the run ends with a screendump, and tools/check_wine_window.py looks
# for the one thing no other part of this desktop is: a large solid block
# of 0xD4D0C8, COLOR_3DFACE, the grey behind every Win32 dialog.
#
# The transcript still has one thing to say, and it is the line this whole
# milestone is about not seeing: "no driver could be loaded".
test-wine-gui: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 \
	    --cmd "wine notepad.exe" \
	    --screenshot $(BUILD_DIR)/wine-gui.ppm \
	    --reject "no driver could be loaded" \
	    --reject "The graphics driver is missing" \
	    --reject "refused a .* window" \
	    --reject "KERNEL PANIC"
	@python3 tools/check_wine_window.py $(BUILD_DIR)/wine-gui.ppm

# The same thing, with nothing typed.
#
# This is the one the whole Wine effort was for: open the File Explorer,
# double-click a .exe, and a Windows program opens in a window. No shell,
# no command, no `wine` prefix - the desktop works out that a .exe is run
# under Wine (kernel/app_files.c), and Wine works out that a window means
# /dev/wm (wine/winenovaris.drv).
#
# The coordinates are the File Explorer desktop icon, the maximize button
# on the window it opens, and notepad.exe in the alphabetical listing.
# They are what they are because this is a pointer test: nothing about
# double-clicking can be tested by typing.
#
# The third one is coupled to the *contents* of the initrd root, which is
# worth knowing before it wastes anybody's afternoon: the listing is plain
# alphabetical and the rows are ROW_H (28px, kernel/app_files.c) apart, so
# adding one file that sorts before "notepad.exe" moves the target down by
# exactly one row and the test double-clicks whatever is now there. That
# is how Milestone 41 broke it - inettest.elf sorts between hellowin.exe
# and notepad.exe - and the symptom was a passing boot that ran the wrong
# program, not an error.
test-desktop-gui: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	python3 tools/qemu_test.py --iso $(ISO) --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 \
	    --click-settle 3 \
	    --click "60,233,double" \
	    --click "881,66" \
	    --click "240,674,double" \
	    --screenshot $(BUILD_DIR)/desktop-gui.ppm \
	    --expect "starting notepad\.exe under Wine" \
	    --reject "no driver could be loaded" \
	    --reject "The graphics driver is missing" \
	    --reject "KERNEL PANIC"
	@python3 tools/check_wine_window.py $(BUILD_DIR)/desktop-gui.ppm

# The same thing on a disk, and the difference is that the prefix is an
# installation rather than something rebuilt at every boot.
#
# `make test-wine` runs with no disk, so $HOME is /root and Wine builds
# its prefix in RAM - correct, and gone when the machine is switched off.
# Here the disk is an *empty* FAT32 volume, $HOME is /disk, and Wine
# builds /disk/.wine on it. Milestone 35 is what made that possible: until
# it, a prefix had to be built by the same Wine on a host and written into
# a disk image with a `make wine-disk` target that no longer exists.
#
# So this asserts something test-wine cannot: that the prefix Wine builds
# lands on a real filesystem. "wine: created the configuration directory
# '/disk/.wine'" is Wine's own account of doing it, on the FAT32 volume,
# and the program running afterwards is the same prefix being used.
#
# Two things it used to assert and no longer can, because they were both
# *failure* messages: Wine naming the prefix in "failed to update ...,
# wine.inf not found", and the "scmdatabase_autostart_services" fixme a
# service prints when it does not start. Neither line appears any more.
#
# The --reject lines are the things this transcript used to say every
# time, each of which was a milestone:
#
#   "could not find DOS drive" - no prefix at all, through Milestone 31.
#   "wine.inf not found" - Wine installed nowhere in particular, so it
#     could not find its own data directory (Milestone 35).
#   "cannot create" - the FAT32 driver refusing to make a directory,
#     which is what one missing ATA timeout looked like (Milestone 35).
#   "Failed to get shared session object" - a client's read-only
#     MAP_PRIVATE view of wineserver's session file was a snapshot rather
#     than the file, so there was no desktop window (Milestone 34).
#   "Too many open files" and the c000011f cascade under it - a process
#     out of descriptors while loading DLLs, reported upwards as an
#     invalid image format (Milestone 34).
#
# The *end* of the program's output is what is asserted rather than the
# start, and that is deliberate: several Wine processes share COM1 here
# and a line from one can land in the middle of a line from another. A run
# where "Hello from a real Windows .exe..." has a preloader warning
# spliced through it is a run that worked. By the time the program reaches
# its last few lines the others have gone quiet.
test-wine-prefix: $(ISO)
	@python3 tools/check_wine_installed.py $(BUILD_DIR)/initrd_staging
	rm -f $(BUILD_DIR)/wine-prefix.img
	python3 tools/mkfat32.py $(BUILD_DIR)/wine-prefix.img --size 512M --label WINEDISK
	python3 tools/qemu_test.py --iso $(ISO) \
	    --disk $(BUILD_DIR)/wine-prefix.img --memory 768 \
	    --boot-wait 30 --settle 900 --timeout 1000 --stop-when-matched \
	    --cmd "wine hellowin.exe" \
	    --expect "FAT32 volume mounted at /disk" \
	    --expect "created the configuration directory '/disk/\.wine'" \
	    --expect "fib\(20\):    6765" \
	    --expect "argv\[0\]: Z:.hellowin\.exe" \
	    --expect "Exiting with code 0" \
	    --reject "could not find DOS drive" \
	    --reject "wine\.inf not found" \
	    --reject "Failed to get shared session object" \
	    --reject "failed to create desktop window" \
	    --reject "Too many open files" \
	    --reject "c000011f" \
	    --reject "kernel heap exhausted" \
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
