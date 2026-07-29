# Novaris

A real, from-scratch, bootable x86 operating system — built incrementally,
one subsystem at a time. This is legally and entirely yours: every line is
either written here or copied from public-domain reference material (the
Multiboot spec), no Windows/macOS/Linux source is used or referenced.

This is **not** a clone of Windows/macOS/Linux — those are billions of lines
of code built by huge teams over decades, and legally reimplementing their
exact behavior (like ReactOS does for Windows) requires careful clean-room
engineering specifically to avoid copyright issues. What we're doing instead
is building a real, simple, original OS the way every OS starts: a
bootloader hands off to a kernel, and the kernel grows from there. Over time
you can shape it to *feel* like whichever OS inspires you (windowing system,
shell conventions, UI style) without copying anyone's code.

## Current status: Milestone 10 complete ✅

- [x] Multiboot bootloader handoff (via GRUB), 32-bit protected mode
- [x] Freestanding C kernel — no libc, we own the whole stack
- [x] Own GDT, IDT, all 32 CPU exception handlers, remapped 8259 PIC
- [x] PIT timer, PS/2 keyboard and mouse drivers
- [x] Higher-half kernel with 4KB paging, a physical frame allocator,
      and a `kmalloc`/`kfree` heap
- [x] Ring 3 user mode via a TSS and an `int 0x80` syscall interface
- [x] Initrd (a GRUB module) behind a VFS layer, plus a tiny libc
- [x] Linear framebuffer desktop with a bitmap font console
- [x] Round-robin preemptive multitasking with real context switching
- [x] ELF32 loader
- [x] **Runs real, unmodified Windows `.exe` files** against an emulated
      Win32 API — see below

See `ROADMAP.md` for the full history and what's next, in order.

## Running Windows programs

Novaris loads PE32 executables and implements enough of the Win32 API
underneath them that ordinary Windows console programs run. The test
binaries in `userland/pe_test/` are compiled with stock `mingw-w64`
against the real Windows headers and import libraries — nothing about
them is written for Novaris, and they would run unchanged on Windows.

```
novaris> run hellowin.exe
Hello from a real Windows .exe running on Novaris!
  compiled by mingw-w64, linked against msvcrt.dll

integers:   42 -7 4000000000 00042 42    | +42
floats:     3.141593 2.50 1.234568e+04 0.0001
heap:       "malloc + strcpy + strlen" (24 bytes)
fib(20):    6765
```

What that involves: the PE loader (`kernel/pe.c`) maps the image, applies
base relocations when it can't have its preferred address, and binds the
import table; the Win32 layer (`kernel/win32*.c`) provides ~500 APIs
across kernel32, msvcrt, ntdll and user32, reached through generated
ring-3 thunks that trap into the kernel on `int 0x81`. `include/win32.h`
explains the mechanism.

Three shell commands drive it:

| Command | What it does |
| --- | --- |
| `run prog.exe` | Loads and runs a program (also handles ELF and flat binaries) |
| `peinfo prog.exe` | Dumps a PE's headers and shows, per symbol, which imports resolve |
| `winapi [module]` | Lists the emulated DLLs, or one module's exports |

**What this is not**: a Windows clone, a Wine port, or a general-purpose
compatibility layer. There is no window manager, no registry, no
networking, no threads. `ROADMAP.md` Milestone 10 is precise about where
the boundary is and why. A GUI program gets an honest failure from
`CreateWindowEx` rather than a blank screen; a 64-bit binary is told it's
64-bit; a program that calls an API Novaris doesn't have prints exactly
which one.

## Project layout

```
novaris/
├── boot/boot.s               # Assembly entry point + Multiboot header
├── kernel/
│   ├── kernel.c               # kernel_main() - C entry point
│   ├── console.c              # Framebuffer console + desktop chrome
│   ├── vga_text.c             # VGA text-mode fallback driver
│   ├── framebuffer.c, font8x16.c, mouse.c
│   ├── gdt.c / gdt_flush.s    # Global Descriptor Table + TSS + TEB
│   ├── idt.c / idt_flush.s    # Interrupt Descriptor Table + dispatch
│   ├── isr.s                  # Exception/IRQ entry stubs (asm)
│   ├── pic.c, pit.c, keyboard.c, serial.c, rtc.c
│   ├── pmm.c / paging.c / kheap.c   # Memory management
│   ├── vfs.c / initrd.c       # Filesystem layer
│   ├── syscall.c              # int 0x80 syscalls
│   ├── process.c / process_asm.s    # Ring 0 <-> ring 3
│   ├── scheduler.c / scheduler_asm.s # Preemptive multitasking
│   ├── elf.c                  # ELF32 loader
│   ├── pe.c                   # PE32 loader: relocations, imports, TLS
│   ├── win32.c                # Win32 core: thunks, dispatch, arenas
│   ├── win32_kernel32.c       # kernel32 + ntdll
│   ├── win32_msvcrt.c         # The C runtime an .exe links against
│   ├── win32_user32.c         # user32, gdi32, advapi32, shell32
│   ├── win32_format.c         # The printf engine
│   ├── win32_dtoa.c           # IEEE-754 double -> decimal
│   └── kstring.c              # Shared string/memory primitives
├── include/                   # One header per subsystem
├── userland/
│   ├── libc/                  # Tiny libc for native Novaris programs
│   ├── pe_test/               # Windows test programs (built with mingw)
│   └── mkinitrd.py            # Packs the initrd image
├── tests/                     # Host-side tests of kernel code
├── tools/qemu_test.py         # Boots the ISO and drives the shell
├── linker.ld                  # Places the kernel at the 1MB mark
├── grub.cfg                   # GRUB menu config baked into the ISO
├── Makefile
└── ROADMAP.md                 # Ordered plan for what to build next
```

## Building

You need: `nasm`, `gcc` (with 32-bit support), `ld`, `grub-mkrescue`,
`xorriso`, `mtools`, and `mingw-w64`. On Ubuntu/Debian:

```bash
apt-get install nasm grub-pc-bin grub-common xorriso mtools \
    qemu-system-x86 build-essential gcc-multilib mingw-w64
```

`mingw-w64` builds only the Windows test programs in `userland/pe_test/`
— the kernel itself has no such dependency. `gcc-multilib` is only needed
for the host-side tests.

```bash
make          # builds build/novaris.bin and novaris.iso
```

## Running it

```bash
make run                 # opens a QEMU window (needs a display)
qemu-system-i386 -cdrom novaris.iso   # same thing, manually
```

Headless, the kernel mirrors its console to COM1, so a plain serial log
is the easiest way to see what it did:

```bash
qemu-system-i386 -cdrom novaris.iso -display none -serial stdio
```

## Testing

```bash
make test        # host-side tests: the printf engine and the PE loader
make test-qemu   # boots the ISO, drives the shell, asserts on the output
```

`make test` links the *actual* kernel sources into a host binary and
drives them directly — the format engine is checked conversion by
conversion against the host C library's own `printf`, and the PE loader
is run against the real mingw-built binaries, including verifying every
entry in a relocation table it applied. `make test-qemu` boots the ISO in
QEMU, types commands through the QEMU monitor, and matches the serial
transcript against expected output (`tools/qemu_test.py`).

You can also burn `novaris.iso` to a USB stick with a tool like `dd` or Rufus
and boot a **real PC** from it — that's genuinely how far this goes.

## Why it's structured this way

- **Multiboot instead of a hand-written bootloader**: writing your own
  16-bit real-mode bootloader (disk reads, A20 line, GDT, protected mode
  switch) is a whole rabbit hole on its own. Using GRUB via the Multiboot
  spec is what most hobby OS projects (and this one) do first, so we can
  focus energy on the kernel. Writing a custom bootloader is a legitimate
  later milestone if you want it (see ROADMAP.md).
- **C, not the assembly the whole way**: after the tiny entry stub, we
  switch to C as fast as possible because it's dramatically more
  productive for everything else (drivers, memory management, data
  structures).
- **No libc**: a hosted OS's libc assumes an OS underneath it (for
  malloc, file I/O, etc.) — we *are* the OS, so we write our own minimal
  primitives as we need them.
