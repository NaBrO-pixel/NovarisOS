# Roadmap

This file is the source of truth for project state across sessions. When
you come back (in a new conversation), start by reading this file, then
`README.md`. Update the checkboxes as things get done.

## Milestone 1 — Boot & display ✅ DONE

- [x] Multiboot header + assembly entry stub (`boot/boot.s`)
- [x] Linker script placing kernel at 1MB (`linker.ld`)
- [x] Freestanding C kernel entry point (`kernel/kernel.c`)
- [x] VGA text-mode driver: colors, scrolling (`kernel/vga.c`)
- [x] Read + display Multiboot memory info
- [x] Build system (`Makefile`) producing a bootable ISO
- [x] Verified booting in QEMU

## Milestone 2 — CPU fundamentals ✅ DONE

The CPU needs proper descriptor tables before we can safely do much more.

- [x] **GDT (Global Descriptor Table)**: define our own flat code/data
      segments instead of relying on whatever GRUB left set up
      (`kernel/gdt.c`, `kernel/gdt_flush.s`). Kernel + user code/data
      segments are both set up now so Milestone 5 won't need to revisit
      this file.
- [x] **IDT (Interrupt Descriptor Table)**: table of handlers for CPU
      exceptions (divide-by-zero, page fault, etc.) and hardware
      interrupts (`kernel/idt.c`, `kernel/idt_flush.s`).
- [x] **ISRs (Interrupt Service Routines)**: assembly stubs + C handlers
      for the 32 CPU exception vectors (`kernel/isr.s`). Unhandled
      exceptions print a diagnostic "kernel panic" screen naming the
      exception instead of silently resetting.
- [x] **PIC remapping**: the legacy 8259 PIC's default interrupt numbers
      collide with CPU exceptions; remapped IRQs 0-15 to vectors 32-47
      (`kernel/pic.c`).
- [x] Enable interrupts (`sti`) once handlers exist.
- [x] Verified end-to-end in QEMU: kernel installs the GDT and IDT, enables
      interrupts, fires a real `int3` (breakpoint) interrupt, the assembly
      stub routes it through the IDT to a registered C handler, and
      execution resumes normally afterwards. Also has a generic
      `register_interrupt_handler()` API so the PIT/keyboard drivers in
      Milestone 3 can plug in without touching this code again.

## Milestone 3 — Input & timing ✅ DONE

- [x] **PIT (Programmable Interval Timer)**: IRQ0 at 100Hz, tick counter
      (`kernel/pit.c`).
- [x] **PS/2 keyboard driver**: IRQ1, scancode set 1 → ASCII (shift
      handling included), ring-buffer input (`kernel/keyboard.c`).
- [x] Simple kernel shell: line editing with backspace, `help`, `clear`,
      `about`, `uptime`, `echo` (`kernel/shell.c`).
- [x] Verified in QEMU: booted, scripted keystrokes through the QEMU
      monitor to type a deliberately mistyped command, backspaced it out,
      retyped it correctly, and confirmed the corrected command executed.

## Milestone 4 — Memory management ✅ DONE

- [x] **Physical memory manager**: bitmap allocator over the Multiboot
      memory map, with mem_lower/mem_upper fallback (`kernel/pmm.c`).
- [x] **Paging**: higher-half kernel at 0xC0000000+1MB. `boot.s` builds a
      boot-time 4MB-page identity+alias mapping (needed before any C code
      can run), then `paging.c` replaces it with a real 4KB-granularity
      page directory using recursive page-table mapping. Required
      reworking `linker.ld` (VMA/LMA split via `AT()`) and `boot.s`.
- [x] **kmalloc/kfree**: first-fit free-list heap allocator with
      splitting and address-order coalescing (`kernel/kheap.c`).
- [x] Verified in QEMU with `-d int,cpu_reset` fault tracing (no triple
      faults / resets) plus a live kmalloc-write-read-free round-trip
      printed at boot, and the `meminfo` shell command exercising the PMM
      interactively.

## Milestone 5 — Getting to userspace ✅ DONE

- [x] GDT user-mode segments (already added in Milestone 2's GDT work).
- [x] **TSS**: ring-0 stack (`esp0`/`ss0`) so interrupts/syscalls from
      ring 3 land on a safe kernel stack automatically (`kernel/gdt.c`).
- [x] **System calls**: `int 0x80`, DPL=3 IDT gate, `eax` = syscall
      number convention. Implemented `SYS_WRITE` and `SYS_EXIT`
      (`kernel/syscall.c`).
- [x] **Ring0↔ring3 transition**: hand-written asm (`kernel/process_asm.s`)
      using the classic iret trick to enter ring 3, and a saved
      esp/ebp + manual `pop ebp; ret` to return to the kernel caller when
      the user program calls `sys_exit` — acts like a blocking function
      call without a real scheduler yet.
- [x] Loads and runs a trivial flat ring-3 "hello world" binary
      (`userland/user_hello.s`, embedded as a byte array - no ELF loader
      or filesystem yet, that's Milestone 6). Mapped `PAGE_USER`, given
      its own stack page.
- [x] Verified in QEMU with `-d int` interrupt tracing: confirmed real
      `cpl=3` execution at the user program's mapped address for both
      syscalls (`sys_write` then `sys_exit`), correct user CS/SS selectors
      (0x1b/0x23), and a clean return to the kernel shell afterward with
      no GPFs or page faults. Also exposed as the `runuser` shell command.
- [x] Full **process structure** (PID, saved register state for more than
      one process) and real **context switching** between multiple
      processes were deferred here and picked up later in **Milestone
      9**, once there was a filesystem (Milestone 6) and real programs to
      run concurrently. This section's original single-process,
      synchronous model (`process_run_user_mode()`) is still exactly
      what `run`/`runuser` use, though - Milestone 9 added a *second*
      path alongside it rather than replacing it.

## Milestone 6 — A filesystem & real programs ✅ DONE (FAT deferred, see note)

- [x] Basic **VFS abstraction**: generic node struct with read/readdir/
      finddir function pointers, one flat root directory for now
      (`kernel/vfs.c`, `include/vfs.h`).
- [x] **Initial ramdisk (initrd)**: a GRUB multiboot module carries a
      custom archive format (header + per-file entries + raw data,
      packed by `userland/mkinitrd.py`) that `kernel/initrd.c` parses
      into VFS nodes. Wiring this in required reserving the module's
      physical memory in the PMM *before* `paging_init()` runs (otherwise
      early page-table allocations could have overwritten the
      not-yet-parsed initrd) and explicitly identity-mapping its pages
      rather than assuming it landed inside the boot-time 0-4MB mapping.
- [x] **Minimal libc** for user programs: `userland/libc/crt0.s` (`_start`
      → `main()` → `exit()`) plus thin `int 0x80` wrappers for `write`/
      `exit` (`userland/libc/libc.c`). `process.c`'s user-program loader
      was generalized from "run the one embedded demo" into
      `process_run_flat_binary(image, size)`, so both the boot-embedded
      asm demo and anything read from the initrd share the same path.
- [x] A real user-mode **C program** (`userland/hello_c.c`), compiled
      against that libc, linked as a flat binary (`userland/user.ld`),
      and shipped *inside the initrd* rather than embedded in the kernel
      image - proving the file-loading path, not just the syscall path.
- [x] Shell commands: `ls` (list initrd files via `vfs_readdir`), `cat`
      (read a file via `vfs_read` into a `kmalloc`'d buffer), `run` (read
      a file, then hand it to `process_run_flat_binary`).
- [x] Verified in QEMU with `-d int` interrupt tracing: confirmed the
      *file-loaded* `hello_c.bin` (not the embedded demo) executed at
      `cpl=3` and made real `sys_write`/`sys_exit` calls, with a clean
      return to the shell afterward and zero unexpected faults/resets.
- [ ] **Real on-disk filesystem (FAT12/16) read support** is deferred.
      The initrd fully covers "ship files in the ISO and read them" for
      now; parsing an actual FAT-formatted volume (BPB, cluster chains,
      directory entries) is a self-contained enough chunk of work that
      it deserves its own pass rather than being squeezed in here -
      noted as a pickup point for a future milestone rather than quietly
      dropped.

## Milestone 7 — "It looks like an OS" ✅ DONE

- [x] Switched from VGA text mode to a **linear framebuffer**, requested
      via the Multiboot 1 video-mode header field (`boot.s`) and read
      back from `mbi->framebuffer_*` (never assumed the request was
      granted exactly - QEMU handed back 1280x800 when 1024x768 was
      requested, and the chrome/text layout is computed from whatever
      was actually granted). Falls back to the original VGA text-mode
      driver (renamed to `vga_text.c`) if no usable framebuffer is
      reported - real hardware without VBE support still gets a working
      console, not a blank screen.
- [x] A hand-generated 8x16 bitmap font (`tools/gen_font.py`, rendered
      from DejaVu Sans Mono - Bitstream Vera Fonts License, which
      explicitly permits embedding modified/rendered forms - and
      thresholded to 1bpp) for framebuffer text rendering
      (`kernel/font8x16.c`).
- [x] Went with a **Windows-95-style single desktop + taskbar** layout:
      a title bar, a bordered "terminal window" the console renders
      inside, and a taskbar (`kernel/console.c`).
- [x] **Mouse driver** (PS/2, IRQ12): i8042 aux-port init, packet
      parsing with resync-on-bit3, and a procedurally-drawn arrow cursor
      that saves/restores the pixels underneath it (`kernel/mouse.c`).
      Needed a new `pic_unmask_irq()` - IRQ12 isn't unmasked by default
      the way IRQ0/1 happen to be.
- [x] Verified in QEMU with `-d int,cpu_reset` fault tracing (no faults),
      pixel-level inspection of the screenshot confirming every chrome
      color matches its source constant exactly, and a QEMU-monitor
      `mouse_move` test whose screenshot diff bounding box starts exactly
      at the computed screen-center default cursor position and lands
      where the injected relative deltas predict - real hardware-level
      confirmation, not just "it compiled."
- [ ] Real windowing/compositing (multiple draggable/resizable windows)
      is *not* implemented - the "terminal window" is static chrome, not
      a window manager. That's a large enough feature (input routing,
      z-ordering, redraw damage tracking) to deserve its own milestone
      rather than being half-built here.

## Milestone 8 — Windows binary compatibility (Wine) (far future, well past Milestone 6/7)

Goal: run real, unmodified Win32 programs. Two architectural options were
considered:

- **Path A (chosen direction)**: Build enough of a POSIX-compatible
  syscall interface in Novaris (paging, `mmap`-equivalent, threads,
  signals, an ELF loader) that actual upstream Wine source can be
  compiled against it — the same idea as Linux binary-compatibility
  layers on BSD. Heavy kernel work up front, but Wine's entire Win32
  implementation comes "for free" afterward — no Windows API code has
  to be hand-written.
- **Path B (not chosen)**: Do what ReactOS actually did — reuse Wine's
  DLL source (`user32.dll`, `gdi32.dll`, `comctl32.dll`, etc.) but rewire
  the low-level glue so those DLLs call Novaris's own NT-style kernel
  syscalls instead of Unix ones. This only works well if the kernel is
  deliberately built to mimic the NT syscall surface. Since Novaris's
  kernel is a conventional monolithic design (closer to a tiny Unix than
  to NT), this would mean a much bigger architectural rewrite than Path A.

**Decision: Path A.** The concrete first step was a minimal POSIX-ish
syscall layer — useful groundwork on its own, testable independently,
and not requiring a line of Wine's source to be touched.

**Honest scope check**: actually compiling and running unmodified Wine
was never realistic for this pass, and isn't now either — it's a
multi-hundred-thousand-line codebase needing a cross-toolchain targeting
Novaris specifically, real dynamic linking, a working scheduler/threads,
and hundreds of libc/POSIX functions. ReactOS took a full team over 20
years to get this far with a *more* Windows-like starting point. What's
below is genuine, verified first-step groundwork toward Path A, not a
Wine port.

- [x] **ELF32 loader** for user-mode executables: parses `PT_LOAD`
      segments, maps each at its own ELF-specified virtual address
      (`PAGE_USER|PAGE_RW`), zero-fills the `memsz`-minus-`filesz` `.bss`
      tail (`kernel/elf.c`). No dynamic linking / `.so` loading yet -
      that's real separately-scoped follow-up work Wine specifically
      needs and this deliberately doesn't attempt.
- [x] **`mmap`-equivalent syscall** (`SYS_MMAP`): a bump-allocator arena
      for anonymous memory (`kernel/syscall.c`), exposed via the tiny
      libc as `mmap_anon()`.
- [x] Shell's `run` command now auto-detects ELF vs. flat binary (ELF
      magic check) and dispatches accordingly.
- [ ] Threads and signals are **still not implemented**. Milestone 9
      later added a real round-robin preemptive *process* scheduler
      (Milestone 5 had deferred this - see its notes), which is
      necessary groundwork but not sufficient on its own: threads need
      *shared-address-space* scheduling (Milestone 9's tasks each get
      their own load address, deliberately, since there's still no
      per-process paging - see its notes) and signals need handler
      registration plus a sigreturn trampoline, neither of which exist
      yet.
- [ ] Compiling/running actual Wine remains far out of reach, per the
      scope check above.

**Verified with real bugs caught and fixed along the way** - this
wasn't just "it compiled": QEMU monitor `sendkey` combos turned out to
mangle punctuation in ways that looked like kernel bugs at first (typed
commands silently corrupted mid-string), which led to building a
pixel-level OCR verification tool (matching screenshot text against
Novaris's own font bitmap) instead of trusting eyeballed screenshots.
That rigor caught two real, previously-unverified bugs:
  1. `userland/mkinitrd.py` stored file offsets relative to the archive's
     data section, but `kernel/initrd.c` read them as relative to the
     archive start - landing reads in the middle of a filename's
     null-padding instead of the actual file content. This meant
     Milestone 6's "loads and runs a file from the initrd" claim had
     never actually been exercised correctly; fixed and reverified with
     an interrupt trace showing the right syscalls firing in order
     (`write` → `mmap` → `write` → `exit`) and the correct program output
     on screen.
  2. `kernel/pmm.c` tracked a `used_frames_count` spanning the entire 4GB
     bitmap while `pmm_total_frames()` only reported the much smaller
     known-RAM range, underflowing `meminfo`'s reported free memory into
     a huge bogus number. Replaced with an on-demand scan over the known
     range, which can't drift out of sync the same way.

### Milestone 8b — Basic PE32 (`.exe`) loading (separately scoped, not Wine)

Goal for this pass: given the scope check above, make "does Novaris do
anything at all with a `.exe` file" concretely true, without pretending
that's the same thing as Win32 program compatibility.

- [x] **PE32 header parser + loader** (`include/pe.h`, `kernel/pe.c`):
      validates the MZ/PE signatures, rejects PE32+ (64-bit) and non-i386
      images, maps every section at `ImageBase + VirtualAddress`
      (`PAGE_USER|PAGE_RW`), zero-fills the `VirtualSize`-minus-raw-size
      tail, copies in the file bytes, and hands back
      `ImageBase + AddressOfEntryPoint`. Structurally the same shape as
      `elf_load()`. No relocation-table processing - images must run at
      their preferred `ImageBase` (true of every test binary here; a
      real-world `.exe` with `/DYNAMICBASE` could still fail even past
      the import check below).
- [x] **Import-table gate, not a silent trap**: a real Windows binary
      imports from `kernel32.dll`/etc, which this OS has no DLLs for. If
      `pe_load()` mapped one and jumped to its entry point anyway, it'd
      "load" and then immediately fault on the first API call - `run`
      would look broken rather than honestly unsupported. So `pe_load()`
      walks the import descriptor table and refuses
      (`PE_ERR_HAS_IMPORTS`) if it finds a real (non-terminator) entry.
      One subtlety caught while building this: even an *import-free*
      binary commonly still gets a non-empty import directory from the
      linker (one all-zero terminator descriptor, `Size == 20` bytes,
      `VirtualAddress` non-zero) - checking `DataDirectory[IMPORT].Size
      != 0` alone would have wrongly rejected it. Fixed by actually
      resolving the RVA to a file offset via the section table and
      inspecting the first descriptor's fields.
- [x] **Shell integration**: `run` now checks for the PE `MZ` signature
      the same way it already checks for ELF, and prints *why* a PE
      failed to load (bad header vs. needs unsupported imports) instead
      of a flat failure.
- [x] **Verified against real binaries, not synthetic bytes**: built two
      test `.exe` files with the actual `mingw-w64` toolchain -
      `userland/pe_test/hello_pe.asm` (assembled with `nasm -f win32`,
      linked with `i686-w64-mingw32-ld`, zero imports, talks to Novaris
      via its own `int 0x80` convention - this is *not* a Win32 program,
      it just proves the loader's bytes-to-execution path is real) and a
      throwaway `i686-w64-mingw32-gcc`-compiled "hello world" with normal
      CRT/kernel32 imports. Checked three ways:
      1. A host-side harness linked directly against `kernel/pe.c`
         (stubbing `paging`/`pmm`) confirmed `pe_load()` finds the
         correct entry point and that the bytes/string it copies there
         exactly match the source (`mov eax,1` / `mov ebx,msg` +
         the literal message text), and separately confirmed the
         import-using binary is correctly rejected with
         `PE_ERR_HAS_IMPORTS`.
      2. Booted the real ISO in QEMU, drove the shell via the QEMU
         monitor's `sendkey`, and OCR'd the framebuffer screendump
         (same rigor as the Milestone 6/7 verification below): `run
         hellope.exe` printed the loaded binary's actual message text
         and returned cleanly to the shell.
      3. Same process for the import-using binary: `run realwin.exe`
         printed the "imports a Win32 API this OS doesn't implement"
         message and returned to the shell rather than hanging or
         faulting.
- [x] **Since superseded by Milestone 10**, which added relocation
      processing and the Win32 API surface, and replaced the
      import-table refusal with real import resolution. The
      "specially-built PE with no imports" test binary
      (`userland/pe_test/hello_pe.asm`) is still shipped and still works
      - it now exercises the no-imports path through the new loader.
- [ ] **Still not attempted after Milestone 10**: PE32+ (64-bit), which
      a 32-bit kernel structurally cannot run.

**Toolchain note**: building `hello_pe.exe` needs `mingw-w64` (only for
that one demo binary - `apt install mingw-w64`; the kernel itself has no
such dependency). If it's missing, `make` fails specifically on
`build/user/hello_pe.exe`.

**Licensing note**: Wine is LGPL-2.1. That's fine to use/adapt — not a
legal problem — but it changes the project's framing from "100%
original, entirely mine" to "original kernel + an LGPL-licensed
compatibility layer." Keep Wine's code in its own clearly-marked,
separately-licensed component rather than blending it into the original
kernel code, so the two remain distinguishable.

## Milestone 9 — Real process structures + preemptive multitasking ✅ DONE

Picks up the process-structure/context-switching work Milestone 5 and
Milestone 8 both explicitly deferred (see their notes above). This is
*additive*, not a rewrite: `process.c`'s original single-process,
synchronous "blocks the caller until sys_exit" model (used by the
shell's `run`/`runuser` commands) is untouched and still works exactly
as before. This milestone adds a second, independent path for running
several ring-3 programs *concurrently* under real timer-driven
preemption, exercised by a new `multitask` shell command.

- [x] **Real `process_t` structures** (`include/scheduler.h`,
      `kernel/scheduler.c`): PID, name, state (READY/RUNNING/ZOMBIE), a
      dedicated per-process kernel stack, and the process's own load
      address / user stack region. A fixed-size table (`MAX_PROCESSES`
      8) - no dynamic process creation from user code yet (no `fork`),
      just kernel-side `scheduler_spawn_flat()`.
- [x] **Genuine preemptive context switching**, not cooperative
      yielding: hooked into the existing IRQ0/PIT handler
      (`kernel/pit.c`), so a process gets switched out mid-execution on
      a timer, whether or not its code ever calls back into the kernel.
      The core trick (see the long comment at the top of
      `kernel/scheduler.c`): a process that's never run yet and a
      process that got preempted mid-flight are made to look identical
      to the switch code, by hand-building a synthetic initial trap
      frame (at the top of a fresh kernel stack) in exactly the layout
      `isr.s`'s existing shared stub epilogue already knows how to pop
      and `iret` from. That meant adding one small, symmetric hook to
      `isr.s` itself (`scheduler_next_esp` - see the comment there)
      rather than writing a second, parallel switching mechanism, so
      "switch to this process" and "resume from where a real interrupt
      left off" are the same code path with no special-casing.
- [x] **Per-process kernel stacks + TSS `esp0` retargeting**: each
      process gets its own 4KB kernel stack (`kmalloc`'d), and
      `gdt_set_kernel_stack()` (already existed, added in Milestone 5
      but unused until now) is updated to point at the *new* current
      process's stack every time the scheduler switches, before that
      process can take its next interrupt/syscall.
- [x] **Round-robin scheduling**, preempting every 2 PIT ticks (~20ms
      at the existing 100Hz rate) - `scheduler_tick()`.
- [x] **Symmetric spawn → run → exit lifecycle**: `scheduler_spawn_flat()`
      adds a process to the ready queue without running it;
      `scheduler_run_until_idle()` starts multitasking and blocks the
      caller (same "looks like an ordinary function call" trick
      `process.c` already used, reused here via a new, parallel
      `kernel/scheduler_asm.s` pair - `scheduler_bootstrap_save_and_jump()`
      / `scheduler_return_to_caller()`) until every spawned process has
      exited, then frees their kernel stacks and unmaps their pages.
      `SYS_EXIT` (`kernel/syscall.c`) now checks `scheduler_is_active()`
      and routes to `scheduler_exit_current()` instead of the old
      `process_exit_to_kernel()` when multitasking is live, so the same
      syscall correctly terminates a process into either model depending
      on how it was started.
- [x] **`multitask` shell command**: spawns three tiny demo programs
      (`userland/task_a.s` / `task_b.s` / `task_c.s`, each looping 12
      times printing `[A]`/`[B]`/`[C]` with a busy-wait spin between
      prints) at distinct load addresses and runs them concurrently.
- [ ] **Still shared address space**: there is still only one page
      directory (no CR3 switching / per-process virtual address spaces)
      - see the Milestone 6/8 notes above for why that's real,
      separately-scoped work. This is why concurrently-scheduled tasks
      have to be spawned at caller-chosen, non-overlapping load
      addresses rather than all reusing the fixed `USER_LOAD_VADDR` the
      single-process path uses - not a limitation of the scheduler
      itself, just of not having per-process paging yet.
- [ ] **No blocking/sleeping/IPC primitives**: a scheduled process is
      either RUNNING, READY, or a ZOMBIE - there's no way for one to
      voluntarily give up its slice early (no `yield`), block on I/O, or
      wait on another process. Real synchronization primitives (mutexes,
      wait queues) are natural follow-up work once something actually
      needs to block.
- [ ] **Priorities/fairness**: purely round-robin, fixed time slice, no
      priority levels or nice values.

**Verified with real hardware-level evidence, same rigor as every prior
milestone**: booted the ISO in QEMU with `-d int,cpu_reset` fault
tracing across a full session (`multitask` immediately followed by
`runuser` and two `run` commands, to check the old and new code paths
back-to-back) - zero triple faults, zero unexpected CPU exceptions (no
GPFs, no page faults; the only vectors that fired were the expected
IRQ0/IRQ1/IRQ12 hardware interrupts, the boot-time `int3` self-test, and
47 `int 0x80` syscalls, matching the writes/exits actually issued).
Pixel-level OCR (same tool built for Milestone 6/7's verification,
matching screenshot text against Novaris's own font bitmap) against the
framebuffer confirmed:
  - All three demo tasks completed **exactly 12 iterations each** (36
    total `[A]`/`[B]`/`[C]` lines, none dropped or duplicated).
  - Their output is **genuinely interleaved** throughout, not run
    back-to-back in three blocks - e.g. one captured run's actual
    sequence was `A A B B B C C C A B B C A A B C A B B C C A B C A A
    B C A B B C C A A C`, which is only possible if the timer really is
    switching between them mid-execution, not if `multitask` just ran
    each one to completion in turn.
  - Immediately afterward, `runuser` and `run helloc.bin`/`run
    hellope.exe` all still printed their expected output and the
    original `"[kernel] User process called sys_exit, returning to
    kernel."` message (only reachable via the *old*, non-scheduler exit
    path) - confirming `scheduler_is_active()` correctly flips back off
    once a multitasking batch finishes, and that this milestone didn't
    regress the pre-existing single-process model it builds alongside.

One real bug caught in the process: the first working version of the
demo tasks used a tight loop with no delay between prints. A single
~20ms scheduler time slice turned out to be enormous relative to a
handful of `int 0x80` calls in an emulated CPU - task A would race
through all 12 of its iterations before the *first* preemption ever
happened, so the very first test run showed A's output as one unbroken
block before B/C got a chance to run at all. That's not a scheduling
bug (the switch itself was firing correctly on schedule the whole time,
confirmed via the tick counts), but it made for an unconvincing
demonstration - fixed by adding a busy-wait spin between each task's
prints so a slice reliably lands mid-task, which is what produces the
interleaved sequence quoted above.

## Milestone 10 — Actually running Windows `.exe` files ✅ DONE

Milestone 8b could load a PE image and jump to it, but deliberately
*refused* anything with a real import table - which is every Windows
binary ever shipped - because there was no Win32 API underneath to import
from. This milestone builds that API, and with it, ordinary Windows
console programs run.

**What "runs" means here, precisely.** `userland/pe_test/hello_win.c` is
compiled with stock `i686-w64-mingw32-gcc` against the real mingw-w64
headers and import libraries. Nothing in it is written for Novaris; it
would run unchanged on Windows. It arrives importing 53 symbols from
`msvcrt.dll` and `KERNEL32.dll`, runs mingw's real CRT startup code
before `main()`, and prints correct output for every printf conversion,
the heap, and a recursive computation. That is the bar this milestone
set, and it is met.

### The architecture (a third option, not Path A or Path B)

ROADMAP.md's Milestone 8 laid out two ways to run Windows binaries: port
Wine (Path A) or reimplement the API on an NT-shaped kernel (Path B).
This is neither, and the distinction matters:

**Path C, taken here**: implement the subset of Win32 that ordinary
programs actually call, natively in the kernel, with no DLL files
involved anywhere. No Wine source, no LGPL component, no NT syscall
surface - so the licensing note at the end of Milestone 8 doesn't apply
to any of this. The whole thing stays original work.

How a call gets from ring 3 into the kernel (`include/win32.h` has the
long version):

1. At boot, `win32_init()` walks the module tables and emits one 16-byte
   *thunk* per exported function into user-executable memory:
   `mov eax, <slot>` / `int 0x81` / `ret <bytes>`.
2. The PE loader writes those thunk addresses into the image's import
   address table, so `call [__imp__WriteFile]` lands on one.
3. `int 0x81` traps into `win32_dispatch()`, which finds the slot from
   `eax` and calls the C implementation with a pointer to the arguments
   still sitting on the ring-3 stack.
4. The thunk's `ret <bytes>` performs the stdcall callee-cleanup the
   caller expects.

Step 4 is why every export declares an argument count: get it wrong and
the caller's stack is silently corrupted, which is far nastier to debug
than a missing function.

### What was built

- [x] **A real PE loader** (`kernel/pe.c`, rewritten): maps the whole
      `SizeOfImage` in one piece including the headers (programs really
      do walk their own PE headers after `GetModuleHandle(NULL)`),
      applies the **base relocation table** when the preferred address
      isn't available, resolves the **import directory** by name and by
      ordinal through a caller-supplied resolver, handles bound-import
      tables, and sets up the **TLS directory** including running its
      callbacks. `pe_inspect()` reports the specific reason an image
      can't run - 64-bit, .NET, a DLL, wrong architecture, malformed -
      instead of a flat failure.
- [x] **Borrowing the low identity map.** A PE wants to live at its
      `ImageBase`, classically `0x400000`, which is inside the region
      `paging_init()` identity-maps. Rather than refuse, the loader saves
      the page table entries it is about to overwrite and puts them back
      when the program exits (`paging_get_entry`/`paging_set_entry`).
      The two mappings that genuinely can't be borrowed - the initrd and
      the framebuffer, both of which the kernel dereferences *while* a
      program runs - are registered with `paging_reserve_region()` and
      the loader relocates around them.
- [x] **~500 emulated APIs** across kernel32, ntdll, msvcrt and user32,
      with gdi32/advapi32/shell32 present as honest failures. Dozens more
      DLL names alias onto these - `ucrtbase`, the whole
      `api-ms-win-crt-*` family, `kernelbase` - because the same
      functions live under different names across toolchains and Windows
      versions. Console I/O, files over the initrd, the process heap,
      virtual memory, critical sections, TLS, interlocked operations,
      time, locales, and the C runtime including a full `printf`.
- [x] **A TEB and a real `fs` segment.** mingw's CRT installs an SEH
      handler with `mov fs:[0], esp` before `main()` runs, so `fs` has to
      be a segment whose base *is* the thread environment block - GDT
      entry 6, pointed at a per-process TEB by `gdt_set_teb()`. This also
      forced a fix in `isr.s`: the interrupt epilogue used to reload all
      four segment registers from the single saved `ds`, which was
      harmless while every user selector was identical and silently fatal
      once `fs` differed.
- [x] **Data exports.** `_iob`, `_fmode`, `_commode` and friends are
      *variables*, not functions - a program reads and writes them
      through the IAT. Those get real backing storage in the Win32 data
      arena rather than a thunk.
- [x] **Ring-3 native implementations** for the handful of APIs that must
      run user code: `_initterm` and `_initterm_e` walk an array of
      function pointers and call each one, which the kernel cannot do
      from inside an interrupt. They're emitted as hand-assembled machine
      code into the thunk arena instead. This is what makes C++ global
      constructors work. `sqrt`/`fabs` use the same mechanism for a
      different reason: they return in `st(0)`, and the kernel
      deliberately never touches the FPU.
- [x] **An exact `printf`, including floats** (`kernel/win32_format.c`,
      `kernel/win32_dtoa.c`). Two constraints shaped the latter: the
      value never becomes a C `double` anywhere in the kernel (that would
      emit x87 instructions inside an interrupt handler and trash the
      user program's own FPU state), and scaling by powers of ten has to
      be exact, which needs a small fixed-size bignum since 1e308 is over
      a thousand bits. Checked conversion by conversion against the host
      C library.
- [x] **A fault in ring 3 kills the program, not the kernel.** Running
      arbitrary `.exe` files means running buggy ones. CPU exceptions
      that happen at CPL 3 now go to a hook (`idt_set_user_fault_hook`)
      that prints a Windows-shaped unhandled-exception report - naming
      the exception, the faulting address, and the accessed address - and
      unwinds to the shell. `userland/pe_test/crash.c` exists purely to
      demonstrate this.
- [x] **Shell integration**: `run` handles PE alongside ELF and flat
      binaries; `peinfo` dumps a binary's headers and shows per-symbol
      which imports would resolve; `winapi` lists the emulated modules
      and their exports.
- [x] **A real test suite**, which this milestone needed and previous
      ones did without. `make test` links the actual kernel sources into
      host binaries: the format engine is checked against the host
      `printf` (38 assertions), and the PE loader is driven against the
      real mingw-built binaries with `mmap` standing in for `paging_map_
      page` (30 assertions, including verifying all 497 entries of a
      relocation table it applied). `make test-qemu` boots the ISO,
      drives the shell through the QEMU monitor, and asserts on the
      serial transcript.
- [x] **A serial console mirror** (`kernel/serial.c`). Every prior
      milestone verified itself by OCR'ing framebuffer screenshots
      against the kernel's own font bitmap - impressive, and far too slow
      to lean on for a subsystem with hundreds of entry points. The
      console now mirrors to COM1, so the harness reads plain text.

### One real bug caught along the way

The exit path out of ring 3 (`process_exit_to_kernel`, from Milestone 5)
restored `esp` and `ebp` but not `ebx`/`esi`/`edi`. That is fine as long
as the resuming function keeps nothing in a callee-saved register across
the call - which was true until this milestone added a function that did.
The symptom was a correct program run followed by a nonsense error
message, because control returned from deep inside an interrupt handler
with those registers holding whatever the interrupt path had left in
them. Fixed by saving and restoring them properly in `process_asm.s` and
the equivalent `scheduler_asm.s` pair.

### Verified

- `make test`: 68 host-side assertions, all passing.
- `make test-qemu`: boots the ISO, runs eight programs, asserts on the
  transcript. Confirms specifically: a mingw C program's full printf
  output including floats, a C++ global constructor running before
  `main()`, `HeapAlloc` round-tripping, a relocated image producing
  identical output to the same program at its preferred base, the 64-bit
  diagnostic firing, all 53 of hellowin.exe's imports resolving, and no
  kernel panic anywhere.
- The direct Win32 test (`userland/pe_test/win32_api.c`) reads back what
  each API returns rather than just calling it: `GetModuleHandle`,
  `GetProcAddress`, `GetSystemInfo`, `GlobalMemoryStatus`,
  `GetSystemTime` (a real date, from a new CMOS RTC driver),
  `HeapAlloc`, `VirtualAlloc` written through at both ends, `Sleep`
  advancing `GetTickCount` by exactly the requested 150ms, `CreateFileA`
  + `ReadFile` on an initrd file, console attributes, `MessageBoxA`, and
  `ExitProcess(7)` arriving back at the shell as exit code 7.
- No regressions: `runuser`, `run helloc.bin` and `multitask` all still
  behave exactly as Milestones 5, 6 and 9 describe, with the demo tasks
  still visibly interleaving under the preemptive scheduler.

### Honest scope: what this does not do

The boundary is a real one and worth stating plainly.

- [ ] **No window manager, so no GUI programs.** `MessageBoxA` is
      genuinely implemented (drawn as a box on the console), because
      console-adjacent programs actually call it. `CreateWindowEx`
      reports failure, deliberately: a program that checks the return -
      most do - then takes its own error path, which is far more useful
      than handing back a handle it would draw into and get nothing from.
      Real windowing is Milestone 7's unchecked item and a large piece of
      work in its own right.
- [ ] **No callbacks from kernel into user code.** `qsort`, `bsearch` and
      `atexit`'s handler list all take a function pointer the C library
      is supposed to *call*, and the kernel cannot call into ring 3 from
      inside an interrupt. `_initterm` is solved by emitting ring-3 code,
      but that trick doesn't generalize to an arbitrary signature. These
      are deliberately left unimplemented rather than stubbed, so a
      program that calls one gets a report naming it instead of `qsort`
      silently not sorting. A general mechanism - returning to ring 3
      with a synthesized frame and a trampoline that re-enters the kernel
      - is real, separately-scoped follow-up work.
- [ ] **No structured exception handling.** The TEB has an exception list
      and programs install handlers into it; nothing walks the chain. A
      fault terminates the program instead of giving it a chance to
      recover. A program that uses `__try`/`__except` for control flow
      will not behave correctly.
- [ ] **No threads, no processes, no networking, no registry, no
      writable filesystem.** `CreateThread` fails, `CreateProcess` fails,
      the `Reg*` family reports "not found", and the initrd is read-only
      (see Milestone 6). Each of those is an honest failure a caller can
      handle, not a silent one.
- [ ] **Unknown imports can leave the stack unbalanced.** A symbol with
      no entry in the tables gets a stub that logs its name and returns
      0, but with no declaration to consult, the stub doesn't know how
      many arguments to pop - so it pops none. That's the least-bad
      option (popping the wrong number is worse), and it's reported at
      the moment it happens. `run` prints a summary afterwards of how
      many such APIs a program imported and how many it actually called.
- [ ] **No DLL loading.** `LoadLibrary` and `GetProcAddress` work against
      the emulated modules only. There is no code path that reads a
      `.dll` file off disk and maps it, which is what a program shipping
      its own libraries would need.
- [ ] **PE32+ (64-bit) is structurally out of reach** for a 32-bit
      kernel, and always will be without a full port.

So: "every `.exe` works" is not the claim. "An ordinary Windows console
program, compiled by a real Windows toolchain, runs correctly and
unmodified" is - and anything outside that says exactly what it needed
and why it couldn't have it.

## Later / open-ended

- Networking (a NIC driver + a minimal TCP/IP stack) — big undertaking.
- SMP (multi-core) support.
- Porting a real libc (newlib) instead of hand-rolling one.
- A custom bootloader instead of relying on GRUB, if you want the whole
  boot chain to be your own code too.

Follow-ups specifically unlocked by Milestone 10, roughly in order of how
much each would widen what runs:

- **Calling ring-3 callbacks from the kernel** — a synthesized user frame
  plus a trampoline that re-enters the kernel when the callback returns.
  This one mechanism gets `qsort`, `bsearch`, `atexit` handlers, window
  procedures, `EnumWindows`-style APIs and thread entry points all at
  once. The single highest-leverage item on this list.
- **Structured exception handling** — walking the `fs:[0]` chain on a
  fault instead of terminating, so `__try`/`__except` works.
- **Loading real DLL files**, so a program can ship its own libraries.
  The PE loader already does everything needed except handling exports
  and forwarders; most of the work is the module-list bookkeeping.
- **A window manager** (Milestone 7's unchecked item), which is what
  `CreateWindowEx` would need in order to stop being an honest failure.
- **Threads**, which need shared-address-space scheduling — see
  Milestone 8's note on why the Milestone 9 scheduler isn't sufficient
  on its own.

## How to resume work in a new session

1. Have Claude read `ROADMAP.md` and `README.md` in this project folder.
2. Say which milestone/checkbox you want to tackle next (or "continue
   the roadmap" to let Claude pick the next unchecked item).
3. Claude should install the toolchain, run `make`, and verify changes
   actually work before considering a step done — same as every session
   so far:

   ```bash
   apt-get install nasm grub-pc-bin grub-common xorriso mtools \
       qemu-system-x86 build-essential gcc-multilib mingw-w64
   make && make test && make test-qemu
   ```

   `mingw-w64` builds the Windows test programs in `userland/pe_test/`;
   `gcc-multilib` builds the host-side tests in `tests/`. Since Milestone
   10 the kernel mirrors its console to COM1, so verification no longer
   means OCR'ing framebuffer screenshots — `make test-qemu` boots the
   ISO, drives the shell through the QEMU monitor, and matches the serial
   transcript against expected output.
