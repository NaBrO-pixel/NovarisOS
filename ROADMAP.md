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
- [x] Real windowing/compositing (multiple draggable/resizable windows)
      was deliberately *not* attempted here - the "terminal window" was
      static chrome, not a window manager. Input routing, z-ordering and
      redraw damage tracking are a large enough feature to deserve their
      own milestone, and got one: see Milestone 11.

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

## Milestone 11 — A windowing desktop ✅ DONE

The whole point of Milestone 7's framebuffer was to earn this. Novaris now
boots into a compositing desktop; the shell is an app inside it.

- [x] **A compositor** (`kernel/gfx.c`, `include/gfx.h`). Drawing happens
      into off-screen 32-bit surfaces and reaches VRAM through exactly one
      call, `fb_blit`, once per frame. That single indirection is what
      makes translucency, soft shadows and antialiased edges possible at
      all: each needs to read the pixel underneath, and reading back from
      VRAM over the PCI bus is far too slow to do per pixel.
  - A signed-distance function for a rounded rectangle turned out to be
    the whole geometry engine: "how far is this pixel from the shape's
    edge" answers both "how much of this pixel is covered" (antialiasing)
    and "how dark is the shadow here" from the same three lines of
    fixed-point math. All of it is fixed point - the kernel doesn't save
    FPU state across interrupts, so floating point is off the table.
  - A two-pass sliding-window box blur backs the frosted menu bar and
    Dock. Panels that blur their backdrop have to be recomposited whole
    whenever any part of them is damaged, or the blur samples stale
    pixels - `expand_for_panels()` in `desktop.c` is that rule.
- [x] **An antialiased proportional UI font** (`tools/gen_uifont.py` ->
      `kernel/uifont.c`): four faces of 8-bit coverage maps with real
      bearings and advances, rendered from DejaVu Sans. The 1-bit 8x16
      font from Milestone 7 stays exactly where it was - it's the terminal
      font, and a fixed cell is what a character grid wants. Proportional
      UI text at this resolution needs coverage values, not thresholded
      bits.
- [x] **A window manager** (`kernel/wm.c`, `include/wm.h`): windows with
      their own backing surfaces, z-order, click-to-focus, title-bar
      dragging, edge resizing, close/minimize/zoom, and damage tracking.
      The backing surfaces are the load-bearing decision - moving a window
      full of text is a blit, not a re-render, which is the difference
      between a desktop that drags smoothly and one that doesn't.
- [x] **Input, rewritten as events.** The keyboard driver grew a second
      queue carrying modifiers, press/release and the keys with no ASCII
      (arrows, Escape, function keys); the mouse driver grew buttons, a
      negotiated scroll wheel, and stopped drawing the cursor itself. That
      last part matters: the old save-and-restore-the-pixels-underneath
      cursor is incoherent once a window can move out from under the
      pointer, so the cursor became the compositor's topmost layer.
- [x] **A desktop shell** (`kernel/desktop.c`): wallpaper, a translucent
      menu bar with working menus and a clock off the CMOS RTC, a Dock
      with pointer magnification, running indicators and live thumbnails
      of minimized windows, and a Spotlight-style launcher over apps and
      initrd files.
- [x] **The shell became an app.** `console.c` grew a sink: a callback
      that receives each character and the color it was written in, so
      every existing `terminal_writestring()` call site in the kernel
      lands in a Terminal window without knowing windows exist. The COM1
      mirror still happens first, so the serial transcript that
      `tools/qemu_test.py` asserts against is byte-for-byte unchanged, and
      `shell_run()`'s loop was turned inside out into
      `shell_init()`/`shell_feed_char()` so the desktop can own the event
      loop.
- [x] **Built-in apps**: Terminal, Files (browses the initrd, opens text
      files, runs programs), Activity Monitor (live memory, uptime, and
      what every open window costs in surface memory), About This Novaris
      (processor brand string straight out of CPUID), a text viewer, and
      alert panels.
- [x] Verified in QEMU by driving the real thing: scripted pointer moves
      and clicks through the QEMU monitor to open Dock apps, drag a window
      by its title bar, resize it from the corner, minimize it and restore
      it from its Dock thumbnail, open menus, and run a Windows `.exe`
      with its output streaming into the window - screenshotting each
      step. `make test` and `make test-qemu` both still pass unchanged,
      which is the real proof the console redirection is transparent.

Known limits, stated plainly:

- [ ] Apps are kernel code with a paint callback, not processes. There is
      no display-server protocol and no way for a ring-3 program to open a
      window; that needs kernel-to-ring-3 callbacks (see Milestone 10's
      follow-ups).
- [ ] Redraws are whole-window: an app repaints its entire surface when
      any part of it changes. Cheap at these sizes, and the damage
      tracking means only the changed screen region is recomposited, but
      it isn't per-widget invalidation.
- [ ] One Terminal window, because there is one shell with one line being
      edited. Two windows would be two views of one conversation.
- [ ] No drag-and-drop, no clipboard, no window animations beyond the Dock
      bounce, and minimizing doesn't animate.

## Milestone 12 — Windows-style windowing ✅ DONE

Milestone 11 built the machinery - surfaces, z-order, damage tracking,
compositing - and dressed it as a Mac. This milestone kept every bit of
the machinery and replaced the interaction model and the chrome with
Windows'. Nothing in `gfx.c` changed; almost all of `wm.c` and `desktop.c`
did.

- [x] **Windows window chrome** (`kernel/wm.c`): the window's icon and
      title read from the left, and minimize / maximize / close sit flush
      in the top-right corner in that order, with the close button turning
      red under the pointer and showing the two-square "restore" mark once
      the window is maximized.
  - The buttons are hit-tested from the window's *right* edge rather than
    from an absolute offset, so they stay put through a resize.
  - Nothing trims the close button's red fill to the window's rounded
    corner: the fill runs to the edge of the surface, and the corner
    rounding that happens at composite time (`gfx_blit_rounded`) cuts it.
    Trying to round the fill itself would have needed a per-corner radius
    the compositor doesn't have.
- [x] **Eight-way resizing.** Every edge and corner drags, with the
      pointer changing to the matching double-headed arrow. This is why a
      press now resolves to one of nine frame regions *before* anything
      else looks at it, and why a resize drag records which edges are
      anchored rather than tracking a corner - dragging the left edge has
      to hold the right edge still, which the old corner-only code never
      had to express.
- [x] **Aero-style snapping.** Dragging a window against the top of the
      screen maximizes it and against a side fills half the screen (with
      quarter-screen corners), previewed as a translucent overlay before
      the button comes up. `Win`+arrows do the same from the keyboard.
      Dragging a snapped or maximized window away restores its floating
      size and re-anchors the pointer proportionally along the title bar,
      so the window doesn't jump out from under the cursor.
  - A window remembers exactly one frame to restore to, saved on the way
    *into* a maximize or a snap and never between two of them - otherwise
    maximize-then-snap-then-restore lands on the snap instead of on the
    size the window actually had.
- [x] **A taskbar** (`kernel/desktop.c`), replacing the menu bar and the
      Dock: Start button, search field, one button per window with an
      accent running indicator, a system tray, and a two-line clock. The
      bar lays its fixed furniture out from both ends and gives what's
      left to the window buttons, so opening a tenth window narrows the
      other nine instead of pushing them off the screen. Hovering a window
      button for a moment raises a live thumbnail of that window, scaled
      from its backing surface.
- [x] **A Start menu**: a search field, a pinned grid, the initrd listed
      as "Recommended", and a power flyout. Typing filters apps and files
      together - this is Milestone 11's launcher, rebuilt around a single
      entry list that both the painter and the hit test read, so a tile
      can't be drawn somewhere it can't be clicked.
- [x] **The rest of the shell**: icons on the desktop with selection and
      double-click-to-open, right-click context menus on the desktop, the
      taskbar and any title bar (the window system menu, also on
      `Alt-Space`), and an `Alt-Tab` switcher over live thumbnails that
      holds until Alt is released.
- [x] **`Alt` and `Win` came apart.** Milestone 11 treated them as one
      "Command" modifier, which a Windows-style shell can't do: `Alt-Tab`
      and `Win-Tab` are different gestures. Tapping `Win` on its own opens
      Start, but using it as a modifier doesn't - tracked by watching for
      any other key between the press and the release, which is what
      Windows does.
- [x] **All the icons are drawn, not stored** (`kernel/icons.c`). There
      are no image files anywhere in Novaris and adding a bitmap format
      plus a decoder to draw a folder would be a lot of machinery for a
      folder. Everything is drawn from primitives at whatever size the
      caller asks for, so one routine serves the 16px title-bar icon, the
      24px taskbar button and the 40px desktop icon.

**Verified with a new host test** (`tests/wm_host_test.c`, wired into
`make test`), which links the real `wm.c`, `desktop.c`, `gfx.c` and
`icons.c` and stubs only what's below the compositor. Window management is
mostly geometry, and geometry is exactly the kind of code that looks right
in a screenshot and is off by two pixels: "the right edge did not move
during a left-edge resize" is a claim a test can make and an eyeball
can't. 52 assertions cover resize anchoring, snap previews and their
follow-through, maximize/restore round trips, the caption buttons,
`Alt-Tab`, `Win`+arrows, show-desktop, the taskbar and the Start menu. It
also renders frames to PPM (`--shots DIR`), which is how the chrome gets
looked at without booting anything.

Two real bugs it caught, both invisible until something asserted on them:

  1. `wm_toggle_show_desktop()` decided which way to go from whether it
     had a remembered list, not from what was actually on screen. Close
     the windows it hid and open new ones, and the next press would
     "restore" an empty list while leaving the new windows sitting there.
     It now looks at what's visible.
  2. Pressing the title bar didn't clear the caption hover, so grabbing a
     window right after passing over its close button left that button
     lit red for the whole drag - the drag path returns early and never
     looks at hovering again.

Known limits, unchanged from Milestone 11 unless noted:

- [ ] Apps are still kernel code with a paint callback, not processes.
- [ ] Snapping has no "snap assist" - filling one half doesn't offer the
      other half to a second window.
- [ ] No window animations: maximizing, minimizing and snapping all
      happen in one frame.
- [ ] The taskbar doesn't group multiple windows of one app behind a
      single button, and can't be moved off the bottom edge.

## Milestone 13 — Calling ring 3 from the kernel ✅ DONE

The item Milestone 10 called "the single highest-leverage item on this
list". Milestone 10 shipped ~500 Win32 functions and left three of them
conspicuously missing: `qsort`, `bsearch` and `atexit`. Each takes a
function pointer belonging to the program and is supposed to *call* it,
and the kernel implements these APIs from inside an `int 0x81` interrupt
handler, in ring 0, with no way to run ring-3 code and come back.
`win32_msvcrt.c` said so in a comment and left them unresolved on
purpose, because a stubbed `qsort` that silently doesn't sort is worse
than an honest "unimplemented API called" report.

This milestone builds the missing mechanism.

### How it works

`kernel/win32_callback.c` + `kernel/win32_callback_asm.s`, a new
`int 0x82` vector, and about 150 lines of C:

1. The kernel builds a cdecl call frame on the *program's own* stack,
   just below the ring-3 esp captured when the API call trapped in:
   arguments right-to-left, then a return address.
2. That return address points at a two-byte stub the kernel planted in
   the thunk arena at boot — literally `CD 82`, `int 0x82`.
3. `win32_callback_enter` saves the kernel's esp/ebp into a caller-
   supplied frame and `iret`s to ring 3, the same trick
   `process_run_user_mode()` uses to start a whole program.
4. The callback runs, returns, lands on the stub, and traps back in.
   The handler reads its return value out of `eax`, restores the saved
   kernel esp, and finishes `win32_callback_enter`'s epilogue — so from
   the API implementation's side, `win32_callback_call()` was an ordinary
   blocking function call that returned a value.

The resume context is passed in by address rather than kept in globals
(which is what `process_asm.s` does), because callbacks nest: a qsort
comparator is free to call qsort. The C side keeps a stack of them,
capped at 8 deep.

### The part that actually took the time

The `iret`-out-and-trap-back-in shape works on the first try. What does
not is the kernel stack, and the failure is subtle enough to be worth
writing down.

Every ring3 → ring0 transition loads esp from the **same fixed
`TSS.esp0`**. So while a callback is running in ring 3, the kernel frames
of the `qsort` that started it are still live between `esp0` and wherever
the kernel had got to — and any trap that callback takes re-enters ring 0
with esp reset to `esp0` and starts pushing straight over them. Not just
the callback's own `int 0x82`: an `int 0x81` from a comparator that calls
`printf`, or a plain timer IRQ, does it too.

The fix is one line — lower `TSS.esp0` below the current kernel esp
before entering ring 3, restore it on the way back — and it is what makes
nesting work at all.

This was verified, not assumed. Building the identical kernel with that
single `gdt_set_kernel_stack()` call removed and nothing else changed:

```
novaris> run qsorttest.exe
qsort/bsearch/atexit test - real callbacks into ring 3

before:  42 -7 0 1000 3 3 -100 17 8 99 5 5 64 2 1

[win32] Unhandled exception in qsorttest.exe:
        EXCEPTION_ACCESS_VIOLATION at eip=0x7b001fd2 accessing 0x00000000
```

The very first callback round trip kills the program, and `eip` lands at
`0x7b001fd2` — inside the thunk arena (`WIN32_THUNK_BASE` is
`0x7B000000`), i.e. the corrupted frame sent execution into the middle of
a thunk. With the line restored the same binary runs to completion.

One honest correction to the story: `win32_call_t` also grew a `useresp`
field, snapshotting the ring-3 esp by value at the top of
`win32_dispatch()` instead of re-reading `regs->useresp` (a pointer into
the trap frame) later. That looked like the load-bearing fix while the
bug was being chased. It is not. A second control build — `esp0` handling
intact, `qsort` deliberately re-reading `call->regs->useresp` the old way
— passes the full test unchanged, because once `esp0` is lowered the trap
frame `regs` points at is no longer overwritten. The snapshot is worth
keeping as defensive clarity about which stack pointer belongs to which
call, but it is not what fixes anything, and Milestone 13 should not be
remembered as though it were.

### What this bought

- `qsort` — quicksort with insertion sort under 8 elements, recursing
  into the smaller partition and looping on the larger, so kernel stack
  stays O(log n) frames. Every comparison is a ring-3 callback.
- `bsearch` — binary search, same callback per comparison.
- `atexit` / `_onexit` / `_crt_atexit` / `_register_onexit_function` —
  handlers run in reverse registration order from `w32_exit_process()`,
  which is the one point every exit path converges on (`exit()`,
  `abort()`, `ExitProcess()`, and `return` from `main` via the exit
  trampoline). Re-entrant `exit()` from inside a handler is ignored, as
  C requires.
- **C++ global destructors, for free.** Nobody set out to fix these.
  `cppinit.exe` has a global object whose destructor prints, and the
  source comment said it was "expected NOT to appear" — mingw's C++
  runtime registers destructors through `atexit`, which Novaris accepted
  and never called back. Making `atexit` real made them run. The test
  source's message has been corrected to say so; nothing else in it
  changed.

### Verified

`tools/qemu_test.py --iso novaris.iso --script tools/tests/win32_smoke.txt`,
with `qsorttest.exe` added to the script. The test program is built with
real `i686-w64-mingw32-gcc` against real `msvcrt.dll` imports and is
written the way any C program uses these functions — it would produce
identical output on Windows.

```
novaris> run qsorttest.exe
qsort/bsearch/atexit test - real callbacks into ring 3

before:  42 -7 0 1000 3 3 -100 17 8 99 5 5 64 2 1
after:   -100 -7 0 1 2 3 3 5 5 8 17 42 64 99 1000
sorted: yes

comparator that prints (nested int 0x81 inside a callback):
    comparator called from ring 3: 30 vs 10
    comparator called from ring 3: 30 vs 20
    comparator called from ring 3: 10 vs 20
  result: 10 20 30
  comparator ran 3 time(s)

nested qsort inside comparator: 1 2 3 4 5
sorted: yes

strings: alpha bravo charlie delta echo

bsearch    64 -> found
bsearch  -100 -> found
bsearch  1000 -> found
bsearch     7 -> not found

qsort test done.
atexit: handler registered SECOND, so it runs FIRST
atexit: handler registered FIRST, so it runs LAST
```

Each block is there to fail differently: the printing comparator is a
nested `int 0x81` taken several kernel frames deep inside the `qsort`
that called it — precisely the case a fixed `esp0` corrupts. The
recursive comparator puts a second callback in flight while the first is
still on the stack. `bsearch 7` must miss. The `atexit` lines arrive
*after* `main` has already returned, from the exit path rather than from
inside an API call.

The other eight smoke cases are byte-identical to the Milestone 12
transcript apart from the new `[dtor]` line and the expected
memory/clock variance.

### Honest scope

- [x] Callbacks with up to 8 dword arguments, cdecl, nesting 8 deep.
- [ ] **Not** yet used for window procedures. `CreateWindowEx` still
      fails. The mechanism this needed now exists — that is what
      Milestone 13 was for — but wiring the window manager to deliver
      `WM_PAINT` into a ring-3 `WndProc` is its own piece of work and
      was not done here.
- [ ] Not used for thread entry points either; there are still no
      threads.
- [ ] The callback layer reads and writes the program's stack by casting
      the ring-3 pointer, which works only because Novaris still runs one
      program in one flat address space. Milestone 14 changes that, and
      this is one of the places that has to change with it.
- [ ] A callback that faults is handled by the existing unhandled-
      exception path (the program dies, the shell comes back), not by
      anything callback-aware. `win32_callback_reset()` puts `esp0` and
      the depth counter back when a program ends either way.

## Milestone 14 — Per-process address spaces ✅ DONE (mechanism)

> Milestone 15 below is the other half of this: it is where processes
> actually start running in these address spaces. Read them together.

The first of the prerequisites for **Path A** — see the note below on why
the project pivoted to porting Wine rather than hand-writing more Win32.
Wine assumes a POSIX-shaped kernel underneath it, and the very first
thing that assumption needs is real, separate page directories per
process.

Through Milestone 13 Novaris ran one program at a time in one flat,
unprotected address space. That was deliberate and it bought a lot: it is
why the Win32 layer can read a program's stack by casting a pointer, why
`pe.c` can memcpy an image into place, and why the callback layer in
Milestone 13 could build a ring-3 call frame with a plain store. All of
that has to be unwound eventually, and unwinding it is not this
milestone. **This milestone builds the mechanism and proves it works.**

### What was built

`kernel/paging.c` grew an address-space API. An address space is
identified by the physical address of its page directory — the value
that goes in CR3 — rather than by a struct, so there is nothing to keep
in step and `paging_current_address_space()` can answer by reading the
register.

```
paging_reserve_kernel_tables(start, end)   pre-create tables for ranges that grow
paging_finalize_kernel_space()             freeze what "the kernel half" means
paging_create_address_space()              -> pd_phys
paging_destroy_address_space(pd_phys)
paging_switch_address_space(pd_phys)
paging_map_page_in(pd_phys, virt, phys, flags)
paging_get_entry_in(pd_phys, virt)
```

Two design decisions carried the weight.

**A second recursive slot.** Editing another address space's page tables
normally means either switching CR3 to it (can't — the kernel is running)
or a scratch-page mapping dance (needs an allocator, and the invalidation
is easy to get subtly wrong). Instead, directory entry **1022** points at
the *foreign* directory. The CPU then treats that directory as an
ordinary page table, so its entries become the PTEs of the 4MB window at
`0xFF800000` — foreign page table N reads and writes at
`0xFF800000 + N*4096` — and because entry 1022 is itself reachable
through the existing 1023 recursion, the foreign directory itself sits at
`0xFFFFE000`. Attach, edit, detach. Entry 1022 is deliberately *not*
shared, so every address space has its own window and no process can see
another's attachment.

**An empirically frozen kernel set.** "The kernel range is identical in
every address space" is easy to say and easy to get wrong: copy the
kernel's directory entries at creation time, then add a kernel mapping
later, and every address space created before that point silently lacks
it. The fix is to make kernel directory entries immutable.
`paging_finalize_kernel_space()` snapshots which entries exist at the end
of boot and calls that set global.

Snapshotting rather than hardcoding an index range is deliberate: the
kernel's mappings are scattered and two of them move. On this boot they
land at directory entries 0 (the identity-mapped low 4MB), 768 (the
kernel image at `0xC0000000`), 832 (the heap at `0xD0000000`), and 1012
(the linear framebuffer, wherever the bootloader put it — QEMU chose
`0xFD000000` here, and the initrd's entry depends on where GRUB dropped
the module). Any hand-written list of those is a list that goes stale.

The heap is the one kernel range that grows after boot, so `kernel.c`
calls `paging_reserve_kernel_tables()` over its full 48MB before
freezing. And `paging_map_page()` now *records* any attempt to add a
kernel-half directory entry after the freeze — `vmtest` reports it. That
is a design error to find and fix at the call site, not a runtime
condition to recover from, so it is surfaced rather than papered over.

### Verified

A `vmtest` shell command, wired into `tools/tests/win32_smoke.txt` (twice
— before and after the programs run, so an address space that leaks
frames shows up as a `meminfo` difference):

```
novaris> vmtest
Per-process address spaces (ROADMAP Milestone 14)
  [vm] kernel page directory at 0x001a1000
  [vm] created two more: 0x00c7d000 and 0x00c7e000
  [vm] mapped 0x30000000 -> frame 0x00c7f000 in the first, 0x00c80000 in the second
  [vm] wrote 0x11111111 in the first, 0x22222222 in the second
  [vm] read back 0x11111111 and 0x22222222  <- isolated
  [vm] same address in the kernel's space: unmapped, as it should be
  [vm] kernel heap page at 0xd0000000 is the same frame in all three
  [vm] destroyed both; frames reclaimed: 6
vmtest done - the kernel is still running, on its own page directory.
```

The test is arranged so the interesting failures are loud:

- **One virtual address, two contents.** `0x30000000` holds `0x11111111`
  and `0x22222222` *at the same time*, in two directories. This is the
  thing a single flat address space cannot do.
- **Written through the virtual address, not the frame.** The mapping is
  what's under test, not the bookkeeping.
- **Interrupts stay on.** While a foreign directory is in CR3, the timer
  and keyboard IRQs keep firing and running kernel code on the kernel
  stack. If the shared kernel half were not genuinely identical in all
  three directories, this would triple-fault rather than print.
- **The private half is private.** `0x30000000` is checked to be unmapped
  in the kernel's own space — through the page tables, since
  dereferencing it is exactly what should fault.
- **6 frames reclaimed**, which is exactly right: two directories, two
  page tables, two data frames. `meminfo` before and after the whole
  smoke run is unchanged, so nothing leaks.

The other ten smoke cases (including Milestone 13's `qsorttest.exe`) are
unchanged.

### Honest scope — what this does *not* yet do

This is the load-bearing caveat, and it should not be read as more than
it is.

- [ ] **No process actually runs in one yet.** `win32_run_pe()`,
      `process_run_flat_binary()` and `process_run_elf()` still all run
      in the kernel's own directory, exactly as before. Nothing in the
      normal boot or `run` path switches CR3. The mechanism exists and is
      proven; wiring processes onto it is the next milestone.
- [ ] The single-address-space assumption is still baked into `win32.c`,
      `pe.c`, the heap/stack arena logic and `win32_callback.c` — every
      one of them reaches user memory by casting a pointer. Unwinding
      that is the bulk of the work and was explicitly not attempted here.
- [ ] No copy-on-write, no `fork`, no demand paging, no swapping. Mapping
      is eager and explicit.
- [ ] No per-process kernel stacks, which real preemption between
      address spaces will need. `TSS.esp0` is still one global value
      (borrowed and restored around ring-3 callbacks, see Milestone 13).
- [ ] The scheduler (Milestone 9) does not know address spaces exist and
      does not switch CR3 on a context switch.

## Milestone 15 — Processes actually run in their own address spaces ✅ DONE

Milestone 14 built page directories a process *could* run in and was
explicit that none did. This is the milestone where they do. Every path
that reaches ring 3 — `win32_run_pe()`, `process_run_elf()`,
`process_run_flat_binary()`, including the boot-time demo — now creates a
private page directory, runs the program in it, and destroys it on exit.

### The idea that kept this small

The obvious expectation was that this would require rewriting `win32.c`,
`pe.c` and `win32_callback.c` to walk page tables, because all three
reach into a program's memory by casting a ring-3 pointer. Milestone 10's
own header comment says as much: *"there are no per-process address
spaces yet, so the kernel can read it directly"*.

None of that was needed, and the reason is worth stating plainly because
it is the whole design:

> The kernel half is **identical** in every address space. So once CR3
> holds the process's directory, the kernel keeps working unchanged — its
> code, its stack, `kmalloc`, the console, the initrd are all exactly
> where they were — while the user half is now the process's own. An
> `int 0x81` handler that reads a program's arguments by casting a
> pointer **still reads the right memory**, because the kernel never
> leaves the process's address space while servicing it.

The rule the emulation layer now depends on is narrower than "one flat
address space", but just as simple: *never switch CR3 while holding a
pointer into a process*. The bracketing in `process.h` is the whole
mechanism:

```c
int owns_as = process_enter_address_space();   /* now on a private CR3 */
... load the image, map the stack, run it, unmap ...
if (owns_as) process_leave_address_space();    /* back on the kernel's */
```

Ordering inside the bracket matters and is commented at each site: the
existing teardown (`w32_mem_reset`, `unmap_stack`, `pe_unload`) runs
*before* leaving, while still standing in the program's directory, so its
frames go back to the PMM through the ordinary paths. Destroying the
directory afterwards reclaims anything they missed rather than being the
only thing that reclaims anything.

If creating an address space fails (out of memory), the program runs in
the kernel's directory exactly as it did through Milestone 14. That is a
real degradation, and it says so on the console rather than pretending.

### Where the line between shared and private actually falls

The freeze point moved: `paging_finalize_kernel_space()` now runs *after*
`win32_init()`. That puts the Win32 thunk and data arenas into the shared
set, and it is deliberate — they are built exactly once at boot and a
program's import address table points straight into them, so they have to
be visible in every address space, the same way a vDSO is.

`vmtest` prints the resulting set, which on this boot is:

```
  [vm] shared kernel directory entries: 0(0x00000000) 492(0x7b000000)
       496(0x7c000000) 768(0xc0000000) 832-843(0xd0000000) 1012(0xfd000000)
```

That is: the identity-mapped low 4MB, the thunk arena, the data arena,
the kernel image, the 48MB heap, and the framebuffer. **Everything mapped
after that line is private** — a program's image, its stack, and its
Win32 heap all get page tables belonging to whichever address space is
current when they are mapped.

Two guards keep that honest rather than aspirational:

- `pe.c`'s `region_is_available()` now also requires
  `paging_range_is_private()`. An image placed in a shared page table
  would land in *every* address space at once — and would still run
  correctly, which is exactly why nothing else would catch it. The shared
  set is 4MB-granular, so this can reject a range nowhere near anything
  the kernel uses; the loader just relocates elsewhere.
- After a program exits and its directory is destroyed, `win32_run_pe()`
  checks that the image base is no longer mapped in the kernel's address
  space. Silent when it holds; loud when it doesn't.

### Verified

The full smoke script, extended to 14 cases — the ten Win32 ones, `vmtest`
before and after, and the ELF, flat-binary and scheduler paths that
Milestone 15 also touched.

Every program now announces its directory:

```
novaris> run hellowin.exe
[win32] running in its own address space, page directory 0x00d0c000
Hello from a real Windows .exe running on Novaris!
...
```

The strongest evidence is what *didn't* change. Every program's output is
**byte-identical** to the Milestone 14 transcript apart from those
address-space lines — which is what a correct isolation change looks
like: `printf`, `malloc`, `qsort`'s ring-3 callbacks, the exit trampoline
and the deliberate null-pointer crash in `crash.exe` all behave exactly
as before, while running on a different page directory than the kernel.

Other things the run establishes:

- The same physical frame is handed back and reused as the page directory
  across successive runs, which is the destroy path working.
- No `LEAK:` report from any run — no image outlived its address space.
- `crash.exe` still faults, is still reported as
  `EXCEPTION_ACCESS_VIOLATION`, and still returns to the shell — the
  fault path unwinds correctly *out of* a foreign address space.
- `qsorttest.exe` still passes in full, so ring-3 callbacks (which build
  a call frame on the program's stack by storing through a pointer) work
  unchanged inside a private address space.

### Two real bugs this milestone's testing found

`meminfo` across the whole suite did *not* come out even: 29542 frames
free before, 29371 after. Worth chasing rather than waving at, so it was
measured per command — `meminfo` between every run, then the same
programs a second time:

| after | 1st pass | 2nd pass |
| --- | --- | --- |
| `run hellowin.exe` | −62 | 0 |
| `run winapi.exe` | 0 | 0 |
| `run cppinit.exe` | −87 | 0 |
| `run lowbase.exe` | 0 | 0 |
| `run crash.exe` | 0 | — |
| `run qsorttest.exe` | 0 | — |
| `multitask` | −8 | −6 |
| `vmtest` | 0 | 0 |

Two different things are mixed together there, and separating them is the
whole point of measuring rather than eyeballing a total.

**The address-space work itself was clean.** `vmtest` is exactly
balanced, and so is every repeated program run — which is what Milestone
14 claimed and this confirms.

**Bug 1 — the Win32 data arena never gave anything back (since Milestone
10).** `w32_data_alloc()` is a bump allocator and `data_next` was never
reset. The fake module headers and data exports allocated at boot are
meant to last forever, but `build_teb()` and `build_peb()` allocate out
of the same arena *per program* — a page-aligned TEB plus a PEB — and
nothing reclaimed them. That is the steady −2 per PE run. The arena is
capped at 1MB, so after roughly 120 program runs `w32_data_alloc()` would
have started returning 0 and programs would have stopped starting, for no
reason a user could have diagnosed. Fixed by recording where boot-time
allocation stopped (`data_reset_mark`) and rewinding the bump pointer to
it on exit; the pages stay mapped and get reused, so the fix costs
nothing.

**Bug 2 — the scheduler unmapped pages without freeing frames (since
Milestone 9).** `reap_all()` called `paging_unmap_page()` on each demo
task's code and stack pages and stopped there. Unmapping removes the
translation; the physical frame stays marked in use forever. Six frames
— three tasks × (code + stack) — vanished every time `multitask` ran.
Fixed by freeing the frame behind each page, the same way `win32.c`'s
`unmap_stack()` already did.

After both fixes, the second pass costs **zero frames for every single
command, `multitask` included**. What remains is one-time only: −62 on
the first PE run (the data arena being populated), −87 on the first
`cppinit.exe` (a much larger binary pushing the arenas' high-water mark),
−2 on the first `multitask` (kernel heap growth for task stacks). Those
are bounded high-water marks, not leaks — they do not recur.

The reason both surfaced now is worth noting: nothing before Milestone 15
had a reason to check that a program gave back everything it took.
Address spaces made that question worth asking, and the answer was no,
twice, in code that had been shipping since Milestones 9 and 10.

### Honest scope

- [x] One process at a time, in a real private address space, with its
      image, stack and heap invisible to the kernel's own space.
- [x] A program's frames are fully reclaimed on exit — steady-state cost
      of running one is zero frames, verified by repeated runs.
- [ ] **Still one process at a time.** This is isolation, not
      concurrency. Two programs cannot run simultaneously, so the
      isolation is not yet load-bearing against a hostile neighbour — it
      is the foundation that makes such a thing possible.
- [ ] **The scheduler still does not switch CR3.** The `multitask` demo's
      three tasks are spawned into whatever address space is current
      (the kernel's) and share it, exactly as they did in Milestone 9.
      Making the scheduler address-space-aware is threads' problem, and
      is the next prerequisite on the Path A list.
- [ ] The Win32 thunk and data arenas are shared, so the TEB/PEB are
      global rather than per-process. Harmless with one process; it has
      to change before there are two.
- [ ] No copy-on-write, no `fork`, no demand paging. Mapping is still
      eager, and a program's whole image and 1MB stack are committed up
      front.
- [ ] `TSS.esp0` is still a single global kernel stack (borrowed and
      restored around ring-3 callbacks, see Milestone 13). Real
      preemption across address spaces needs per-process kernel stacks.

## Milestone 16 — Address-space-aware scheduling, and threads ✅ DONE

Item 2 on the Path A list. Milestone 15 put one process in its own
address space, but the scheduler still knew nothing about them: the
`multitask` demo's three tasks shared whatever directory happened to be
loaded, which is why they had to be assembled at three *different*
addresses to avoid overwriting each other.

This milestone teaches the scheduler about address spaces, and gets
threads almost for free as a consequence.

### One line in the switch path

Each task now records the page directory it runs in, and the switch path
loads CR3 when the next task's differs from the outgoing one. That is
essentially the whole change:

```c
static void switch_to(process_t* next) {
    if (next->page_directory != current->page_directory) {
        paging_switch_address_space(next->page_directory);
    }
    current = next;
    current->state = PROC_RUNNING;
    gdt_set_kernel_stack(current->kernel_stack_top);
    scheduler_next_esp = current->esp;
}
```

Doing the CR3 load *there* — several C frames deep, on the outgoing
task's kernel stack, before the isr.s epilogue performs the actual stack
swap — is safe for the same reason Milestone 15's change was small: the
kernel half is identical in every address space, so this code, this
stack, and the frame `scheduler_next_esp` points at are all at the same
addresses before and after the load. Only the user half changes, and
nothing on the switch path touches that.

Skipping the load when the directory is unchanged is not just an
optimisation — a CR3 write flushes the entire TLB, and it is what makes
switching between two threads of one process cheap.

### Threads are what is left when you take the address space away

The existing `scheduler_spawn_flat()` was split into a `spawn_common()`
that builds a kernel stack, a user stack and a synthetic trap frame, plus
a thin wrapper that loads an image. Three public spawns now sit on top:

| | address space | image | owns the directory |
| --- | --- | --- | --- |
| `scheduler_spawn_flat` | the current one | loads | no |
| `scheduler_spawn_process` | a brand-new one | loads | yes |
| `scheduler_spawn_thread` | the current one | none | no |

A thread is that third row: `load_pages = 0` ("I own no code") and
`owns_page_directory = 0` ("the directory I run in is somebody else's").
What remains — a stack and a register context — *is* the thread. No new
scheduling machinery was needed; the round-robin loop, the preemption and
the synthetic-frame trick all work on threads unmodified.

`scheduler_spawn_process()` loads into its new address space by simply
standing in it:

```c
paging_switch_address_space(as);
pages = load_image(image, size, load_vaddr);
pid   = spawn_common(...);
paging_switch_address_space(caller_as);
```

Neither `load_image()` nor `spawn_common()` knows address spaces exist —
they call the ordinary paging functions, which act on whatever is in CR3.
Safe because the image bytes are read out of a kernel buffer that is at
the same address either side of the switch.

Reaping got two passes, because a task's pages can only be unmapped from
inside its own address space, and a directory cannot be destroyed while
it is loaded in CR3: switch to each task in turn and free its pages, then
come back and destroy the directories whose owners created them.

### Verified: `multitask`, rewritten to prove isolation

All three demo tasks are now assembled at the **same** `ORG 0x50000000`
and spawned with `scheduler_spawn_process()` at the same load address and
the same stack address. Through Milestone 15 that would have been three
tasks scribbling over each other's code.

```
novaris> multitask
[kernel] Spawning 3 demo processes under the preemptive scheduler...
PIDs: 1, 2, 3 - if you see their tags interleaved below
rather than grouped, the timer is really preempting them:

[A]
[A]
[B]
[B]
[B]
[C]
[C]
[C]
[A]
...
```

The evidence is in the tags. All three images live at `0x50000000` — if
the address spaces were not real, the last one spawned would have
overwritten the other two and every line would read `[C]`. Getting `[A]`,
`[B]` *and* `[C]`, interleaved by the timer, means three different
physical pages are answering to one virtual address while the scheduler
switches between them.

### Verified: `threadtest`, proving the opposite

Isolation is only half the story; threads need the other half, which is
tasks that genuinely *do* share memory. `userland/thread_demo.s` is one
image with two entry points, run by two tasks in one address space, both
incrementing the same word:

```
novaris> threadtest
Two threads in ONE address space (ROADMAP Milestone 16)
  page directory 0x00c7b000, one image at 0x51000000
  thread PIDs 4 and 5, separate stacks at 0x51100000 and 0x51200000

[thread 0]
[thread 0]
[thread 1]
[thread 1]
[thread 1]
[thread 0]
...

  shared counter at 0x51000200 reads 16, expected 16
  both threads incremented the SAME word - they share one address space
threadtest done.
```

Two threads × 8 iterations = 16, read back by the kernel after both have
exited. Had they been given private copies the count would have been 8.
The interleaved tags show the timer preempting between them, and the
separate stacks at `0x51100000` and `0x51200000` show they are genuinely
independent contexts rather than one task looping twice.

The shell owns the address space in this demo rather than the scheduler,
for a specific reason worth recording: the counter lives in the image, so
if a *task* owned the directory the reaper would destroy it — and the
counter with it — before there was anything left to read.

### Frame accounting

`meminfo` before and after two full rounds of `multitask` + `threadtest`:
**29541 → 29541 → 29541**. Exactly balanced, with five address spaces
created and destroyed per round. The full 16-case smoke suite is
unchanged from Milestone 15 apart from the added `threadtest` output, and
its 149-frame delta is entirely the one-time arena high-water marks
documented there.

Host tests: 38 + 30 + 52 checks, 0 failures.

### Honest scope

- [x] Preemptive round-robin across separate address spaces, switching
      CR3 on the task switch.
- [x] Multiple threads per address space, preemptively scheduled, sharing
      memory.
- [ ] **These are still only the scheduler's tasks, not the shell's
      programs.** `run prog.exe` remains the synchronous "one process,
      blocks the shell" path from Milestone 15. A Win32 program still
      cannot spawn a thread — `CreateThread` and `_beginthreadex` are
      still stubs. Wiring the Win32 layer onto this scheduler is the
      obvious next step and is *not* done here.
- [ ] No synchronisation primitives at all: no mutex, no semaphore, no
      condition variable, no futex. `threadtest` gets away with `lock inc`
      because a single core can only preempt between whole instructions.
      Anything Wine needs will want real ones.
- [ ] No thread-local storage. Every thread in an address space shares one
      TEB, which is wrong the moment a second thread runs Win32 code.
- [ ] No thread exit/join semantics beyond "every task calls sys_exit and
      the batch ends". No detach, no return values.
- [ ] `MAX_PROCESSES` is 8, and kernel stacks are a fixed 4KB each.
- [ ] Still no priorities, no blocking/sleeping, no I/O wait — a task that
      wants to wait busy-spins.

## Path A — porting Wine (the current direction)

Milestone 8 laid out two ways to run Windows binaries: port Wine (Path
A) or reimplement the API surface (Path B). Milestone 10 took a third,
smaller road — hand-written clean-room Win32, no Wine or ReactOS source
anywhere — and Milestones 10–13 took it as far as ~500 APIs, real
`.exe` loading, and now real ring-3 callbacks.

That road works, and it stays narrower than real Win32 forever. The
project has chosen **Path A**: port real Wine on top of Novaris, and get
actual breadth instead of a bespoke subset. This is an honest
architectural pivot, not an incremental feature, and the prerequisites
are forced into an order:

1. **Per-process address spaces** — ✅ done. Milestone 14 built the
   mechanism; Milestone 15 put processes in it. Still to do: make the
   scheduler address-space-aware, which is really item 2's problem.
2. **Real threads** — ✅ done for the kernel's own scheduler in Milestone
   16 (address-space-aware switching, multiple threads per address
   space). Still to do: synchronisation primitives, thread-local storage,
   and connecting Win32's `CreateThread` to any of it.
3. **A POSIX-ish syscall surface** — `mmap`/`munmap`/`mprotect`, file
   descriptors, `pthread_*`, something signal-shaped. This is what
   Wine's own build actually links against.
4. **A dynamic linker** — Wine's DLL-equivalents load and relocate at
   runtime.
5. **Pull in real Wine source**, cross-compile it against that surface,
   and find out what is still missing. Expect that to surface more gaps;
   a display backend for GDI is the obvious one.

Each is roughly milestone-sized. (3) and (4) are each comparable in scope
to everything built so far combined. No Wine or ReactOS source gets
vendored in before step 5, and when it does it stays clearly separated
from Novaris's own clean-room code — Wine is LGPL-2.1, which is fine, but
the boundary has to stay visible.

**Stated honestly:** the goal is real Win32 GUI programs, and eventually
broader real-world Windows software, running on Novaris via Wine. It is
not a claim that anything at all will run. Chrome will not: that needs
networking, GPU acceleration, sandboxing and hundreds of DLLs far beyond
this project's scope, Wine or no Wine.

## Later / open-ended

- Networking (a NIC driver + a minimal TCP/IP stack) — big undertaking.
- SMP (multi-core) support.
- Porting a real libc (newlib) instead of hand-rolling one.
- A custom bootloader instead of relying on GRUB, if you want the whole
  boot chain to be your own code too.

Follow-ups specifically unlocked by Milestone 10, roughly in order of how
much each would widen what runs:

- ~~**Calling ring-3 callbacks from the kernel**~~ — done in Milestone
  13, and it landed exactly as described: a synthesized user frame plus a
  trampoline that re-enters the kernel when the callback returns. It got
  `qsort`, `bsearch` and `atexit` (plus C++ global destructors as a side
  effect). Window procedures, `EnumWindows`-style APIs and thread entry
  points are now *unblocked* by it but not yet built on it.
- **Structured exception handling** — walking the `fs:[0]` chain on a
  fault instead of terminating, so `__try`/`__except` works.
- **Loading real DLL files**, so a program can ship its own libraries.
  The PE loader already does everything needed except handling exports
  and forwarders; most of the work is the module-list bookkeeping.
- ~~**A window manager**~~ - done in Milestone 11. `CreateWindowEx` still
  fails, though, and now for a different reason: the window manager exists
  but a Windows program can't be given a window, because delivering
  `WM_PAINT` means calling a ring-3 window procedure from the kernel. That
  is the first item on this list, and it is what unblocks this one.
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
