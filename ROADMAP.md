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

## Milestone 17 — Real Win32 threads ✅ DONE

Milestone 16 built threads for the kernel's own scheduler and was explicit
that a Win32 program still could not use them: `CreateThread` was a stub
returning 0. This closes that gap. A real mingw-compiled `.exe` now calls
`CreateThread`, gets genuinely concurrent execution, joins with
`WaitForSingleObject`, and is protected by a `CRITICAL_SECTION` that
actually locks.

### The main thread becomes a scheduler task

The change that makes everything else possible is one substitution in
`win32_run_pe()`:

```c
-   process_run_user_mode(start, esp);
+   int main_pid = scheduler_spawn_win32_thread("main", start, esp, teb_address);
+   scheduler_run_until_idle();
```

`scheduler_run_until_idle()` blocks its caller in exactly the way
`process_run_user_mode()` did, so everything around it — the loader, the
teardown, the address space, the fault path — is unchanged. But now the
program is a *task*, and a second thread is just a second task in the same
address space, which is precisely what Milestone 16 built.

Thread stacks come from `w32_mem_alloc_pages()` rather than being mapped
by the scheduler, so they are freed wholesale by `w32_mem_reset()` when
the program exits — nothing for the reaper to double-free. Each thread
gets its own TEB, and `switch_to()` repoints the TEB GDT descriptor on
every switch; without that, two threads would share one `errno` and one
SEH chain.

### Waiting, without being able to block

A thread that calls `WaitForSingleObject` or a contended
`EnterCriticalSection` has to wait. The obvious implementation — mark the
task blocked and switch away — does not work here, because the scheduler
switches tasks by restoring a *trap frame*, and a task waiting inside a
syscall has a live kernel-side C call chain that a trap frame cannot
represent. Supporting that properly means a second, kernel-context switch
path (what Linux's `__switch_to` does), which is a redesign.

The trick used instead costs seven bytes of arithmetic. A thunk is:

```
+0: B8 <id32>   mov eax, <slot id>     ; 5 bytes
+5: CD 81       int 0x81               ; 2 bytes
+7: C2 nn 00    ret n
```

so rewinding `regs->eip` by 7 makes the entire call happen again — the
`mov` included, which is what puts the slot id back in `eax`. Rewind, give
up the slice, and when this thread is next scheduled it re-executes the
call and re-checks the condition. Other threads run in between. The API
implementation returns normally in the meantime and its return value is
discarded, so it must not have done anything it would mind doing again —
which is why the "acquire" branch of `EnterCriticalSection` comes first
and the retry only happens on the path that changed nothing.

Honest consequence, stated plainly: **a waiting thread stays runnable and
burns its time slices retrying rather than sleeping.** That is fine for
three threads on one core and would not be fine for a real workload.

### What became real

- `CreateThread` / `ExitThread` / `GetExitCodeThread` — a start routine is
  *called*, not jumped to, with a thread-exit trampoline as its return
  address, so `return 0;` from a thread procedure ends the thread the same
  way returning from `main` ends the program.
- `WaitForSingleObject` on a thread handle, with a real join.
- `GetCurrentThreadId` — was a hardcoded `0x104`, now the scheduler PID,
  which is the entire point of the call.
- `EnterCriticalSection` / `LeaveCriticalSection` /
  `TryEnterCriticalSection` / `DeleteCriticalSection` — were no-ops
  through Milestone 16, which was correct for one thread and exactly wrong
  for several. State lives in the program's own `CRITICAL_SECTION` struct
  at the documented offsets, because programs read `RecursionCount` and
  `OwningThread` out of it directly.
- `Sleep` no longer holds the CPU in `sti; hlt` for its whole duration,
  which would have starved a program's other threads. It yields, and falls
  back to halting only when nothing else is runnable.

### Two bugs found on the way

**`scheduler_tick` did not check the interrupted ring.** A timer landing
while a task was inside a syscall would save a same-privilege interrupt
frame — which has no `useresp`/`ss` on it at all — as though it were a
`registers_t`, and resuming it would return to garbage. Latent until now:
the only tasks the scheduler ran were demo programs whose syscalls never
re-enabled interrupts, and `Sleep()` deliberately does. Fixed with
`if ((regs->cs & 3) != 3) return;`.

**`ExitThread` hung the machine.** The first version ended with
`for (;;) hlt` on the theory that a function called ExitThread should not
return. But `scheduler_exit_current()` does not switch stacks itself — it
records the decision and lets the call chain unwind to isr.s's epilogue,
which is the only place it is safe to repoint `esp`. Halting instead meant
the switch never happened, with interrupts off inside an interrupt gate,
forever. Symptom: worker 1 ran to completion and the machine froze. The
function must return normally; the *thread* never resumes, but the C
function does.

### Verified

`userland/pe_test/threads.c`, built with real `i686-w64-mingw32-gcc`
against the real kernel32 import library — three workers, each doing an
`InterlockedIncrement` and a deliberately non-atomic guarded increment:

```
novaris> run threads.exe
[win32] running in its own address space, page directory 0x00cf1000
Win32 threads test - real CreateThread on Novaris

main thread GetCurrentThreadId() = 1

created worker 1, thread id 2
created worker 2, thread id 3
created worker 3, thread id 4

  worker 1 starting, GetCurrentThreadId() = 2
  worker 2 starting, GetCurrentThreadId() = 3
  worker 3 starting, GetCurrentThreadId() = 4
  worker 1 done
  worker 2 done
  worker 3 done

all 3 workers joined
  worker 1 exit code 100
  worker 2 exit code 200
  worker 3 exit code 300

interlocked counter: 60, expected 60  -> ok
guarded counter:     60, expected 60  -> ok

threads test done.
[win32] threads this program ran 4
[win32] times a critical section was found already held 2
```

All three workers start before any finishes, so they really are
concurrent, and distinct thread ids come back from `GetCurrentThreadId`.

**The decisive evidence is the negative control.** A passing lock test
proves nothing if the threads never actually raced. Rebuilding with
`EnterCriticalSection` reverted to the Milestone 16 no-op, and nothing
else changed:

```
interlocked counter: 60, expected 60  -> ok
guarded counter:     57, expected 60  -> WRONG (the lock did nothing)
[win32] times a critical section was found already held 0
```

Three increments lost to a real read-modify-write race. So the race is
genuine, the threads are genuinely concurrent, and the critical section is
genuinely what prevents it. The contention counter is reported by the
kernel for the same reason: "the total came out right" and "the total came
out right *and* the lock was contended while it did" are different claims.

The other 16 smoke cases are unchanged. Repeated runs cost **zero
frames** — 29482 → 29417 (one-time arena high-water) → 29417 → 29417 —
so multi-threaded programs release everything they take. `crash.exe`
still faults, is still reported, and still returns to the shell, which
now means the fault path unwinds correctly out of a *multi-threaded*
program via `scheduler_terminate_all()`. Host tests: 38 + 30 + 52 checks,
0 failures.

### Honest scope

- [x] Real preemptive threads in a real Win32 program, with working
      joins and working mutual exclusion.
- [ ] **Waiting is a retry loop, not a block.** A waiting thread is still
      scheduled and still burns CPU. This says real blocking needs "a
      second switch path that can suspend and resume a task's kernel
      stack" — Milestone 25 showed that is simply not true, and built
      blocking for POSIX futexes out of the trap frame the scheduler
      already saves. Win32's waits have not been moved onto it yet, so
      this entry stands for them.
- [ ] `WaitForSingleObject` ignores finite timeouts (treats them as
      `INFINITE`); only a 0 timeout is honoured, as a poll.
      `WaitForMultipleObjects` is still a stub.
- [ ] No `Event`, `Mutex` or `Semaphore` objects — `CreateEvent` and
      friends still hand back inert handles. Only thread handles are
      really waitable.
- [ ] No thread-local storage per thread: `TlsAlloc`/`TlsSetValue` still
      use one global slot table, so two threads share a TLS slot. The TEB
      is per-thread, but its TLS array is not yet used.
- [ ] `CREATE_SUSPENDED` is ignored (with a diagnostic); no
      `SuspendThread`/`ResumeThread`, no priorities, no `TerminateThread`.
- [ ] 8 threads per program, 8 tasks total, 8KB kernel stack each.
- [ ] Still one *process* at a time.

## Milestone 18 — The Linux syscall ABI ✅ DONE (memory + files)

Item 3 on the Path A list, first instalment. The item is stated there as
"a POSIX-ish syscall surface — `mmap`/`munmap`/`mprotect`, file
descriptors, `pthread_*`, something signal-shaped — this is what Wine's
own build actually links against", and as being comparable in scope to
everything built so far. This is the memory-and-files half of it.

### Why the old syscalls had to go

Milestone 5 defined three ad-hoc calls: `SYS_EXIT=0`, `SYS_WRITE=1`,
`SYS_MMAP=2`. They were enough to prove ring 3 worked, and they are the
wrong shape for what comes next. Wine is not a Windows program — it is a
*Unix* program that happens to implement Windows — so cross-compiling it
means giving it a kernel whose syscall numbers, argument registers and
error convention it recognises.

The two numberings cannot coexist: Linux's `exit` is 1, which was
Novaris's `write`. So this is a replacement, not an addition, and the
handful of in-tree demos that used the old numbers were migrated rather
than bridged — `user_hello.s`, `task_a/b/c.s`, `thread_demo.s` and
`userland/libc/`.

The ABI is now Linux's, and not ours to choose:

```
eax = syscall number
ebx, ecx, edx, esi, edi, ebp = arguments 1..6
eax = result; errors come back as -errno in [-4095, -1]
```

### What is implemented

`kernel/posix.c`, with `kernel/syscall.c` reduced to the IDT registration.

- **Files**: `open`, `close`, `read`, `write`, `writev`, `lseek`,
  `stat64`, `fstat64`. A real per-process descriptor table — 0/1/2 are the
  console, the rest are initrd files. `write` takes counted bytes rather
  than a NUL-terminated string, which is a real behavioural change: the
  old `SYS_WRITE` could not have written a buffer containing an embedded
  NUL, and the test does exactly that. `writev` is there because a real
  libc's `printf` reaches for it before `write`.
- **Memory**: `mmap2`, `munmap`, `mprotect`, `brk`. Real page-level work
  on the process's own page directory (Milestone 15), so a mapping is
  private, `munmap` genuinely frees its frames, and `mprotect` genuinely
  rewrites the page-table permission bits.
- **Misc**: `uname`, `getpid`, `gettid`, `time`, `nanosleep`, the
  `get[ug]id32` family, `ioctl` enough for `isatty`, and
  `rt_sigaction`/`rt_sigprocmask`/`set_thread_area` accepted-and-ignored
  so a libc does not give up at startup.
- Anything else returns `-ENOSYS` **and says so on the console**, the same
  way the Win32 layer reports an unimplemented API rather than returning a
  plausible-looking zero.

### Verified: the same binary, on Linux and on Novaris

This is the part worth reading. `userland/posix_test.c` contains nothing
that knows Novaris exists. It makes raw `int $0x80` calls with Linux's
numbers, is built with an ordinary
`gcc -m32 -static -nostdlib -ffreestanding`, and links against no library
of any kind — not even the tiny one in `userland/libc/`. It is a Linux
program.

`tools/posix_compare.py` runs that **one binary** twice — once on the
Linux build host, once inside QEMU on Novaris — and diffs the transcripts:

```
$ make test-posix
  expected difference: host 'sysname = Linux' vs novaris 'sysname = Novaris'
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)
PASS: the same binary behaves identically on Linux and Novaris
```

Forty of forty-one lines are byte-identical. The one that differs is
`uname()`'s sysname, and its differing is itself useful — it proves the
QEMU transcript really came from Novaris rather than from a stale host
run.

The twenty checks are chosen to fail rather than pass by accident:

- a `write()` of eight bytes containing a NUL in the middle, which a
  string-based write would truncate at three;
- `writev` returning exactly 22 across three iovecs;
- `fstat64`'s `st_size` agreeing with `lseek(SEEK_END)` on a real file,
  then seeking back and re-reading;
- `open` of a missing file returning exactly `-ENOENT` (`-2`), not just
  "something negative";
- fresh anonymous memory reading as zero *before* anything writes it;
- writing and reading back all 8192 bytes of a two-page mapping, so a
  mapping that only really covered one page fails;
- `brk` growing, the new memory being usable at both ends, and shrinking
  back to exactly where it started.

The transcripts are also literally "binary files differ" to `diff` —
because of that embedded NUL, which is its own small proof that `write`
carries counted bytes.

### Migration, verified by everything else still working

Every in-tree program that made a syscall was rewritten for the new ABI,
and the full 21-case smoke suite is the check on that: the boot-time ring-3
demo, `multitask`'s three tasks, `threadtest`'s two threads, the ELF and
flat-binary demos, and all the Win32 programs (which go through `int 0x81`
and were unaffected) all still produce their expected output.

Repeated runs cost **zero frames** — 29478 across five successive
`meminfo` calls with `posixtest.elf` and `helloelf.elf` run twice each —
so descriptors and mappings are genuinely released with the process.
Host tests: 38 + 30 + 52 checks, 0 failures.

### Honest scope — what a real Wine build would still miss

This is the memory-and-files half of item 3. The rest is not here.

- [ ] **No signals.** `rt_sigaction` and `rt_sigprocmask` return success
      and nothing is ever delivered. Wine needs real signal delivery —
      it uses `SIGSEGV` for its own page-fault handling.
- [ ] **No `clone`/`futex`**, so no `pthread_create` and no POSIX
      threading. Novaris has real threads (Milestones 16-17), but they are
      reached through the Win32 side, not through a POSIX API.
- [ ] **No file-backed `mmap`.** `MAP_ANONYMOUS` only; a file-backed
      request is refused with `-ENOSYS` rather than quietly returning
      zeroed memory. A dynamic linker will need this.
- [ ] The initrd is read-only, so `open` for writing returns `-EROFS` and
      there is no `unlink`, `mkdir`, `rename` or `creat`.
- [ ] `PROT_EXEC` is accepted and ignored — 32-bit x86 without PAE has no
      NX bit, so "readable but not executable" cannot be expressed.
- [ ] No `dup`/`dup2`/`pipe`, no `poll`/`select`, no sockets.
- [ ] `read()` on fd 0 returns `-ENOSYS`; the console has no
      character-at-a-time path.
- [ ] Only 32 descriptors, and `getpid` is a constant.

## Milestone 19 — Signals ✅ DONE

The second instalment of item 3, and the piece Wine needs most. Wine does
not merely tolerate signals: it installs a `SIGSEGV` handler and uses page
faults as a control-flow mechanism. A kernel that cannot deliver a signal
to a handler *and resume the faulting instruction afterwards* cannot run
it, however much of the rest of the ABI it implements.

Milestone 18 accepted `rt_sigaction` and `rt_sigprocmask` and did nothing
with them, and said so. This makes them real.

### The mechanism

The whole thing turns on identifying the right moment. A signal can only
be delivered on the way back to ring 3, because the trap frame the kernel
is about to `iret` from **is** the thread's user-mode state. So delivery
is:

1. copy that frame, plus the current signal mask, onto the user stack
   below the interrupted `esp` — the sigframe;
2. push the handler's cdecl arguments and, above them, a return address;
3. rewrite `eip` and `useresp` in the trap frame to point at the handler
   and the frame just built;
4. `iret`, which now enters the handler instead of resuming what was
   interrupted.

When the handler returns it lands on a restorer that issues
`rt_sigreturn`, and the kernel copies the saved frame back over the trap
frame. The interrupted instruction resumes as if nothing had happened —
which is the property Wine is built on.

Delivery hooks into `isr_handler` and `irq_handler` in `idt.c`, and into
the ring-3 fault path in `process.c` where a page fault becomes `SIGSEGV`,
a divide error `SIGFPE`, an invalid opcode `SIGILL`.

### Three things that had to match Linux rather than be sensible

**`SA_SIGINFO` is not just a richer handler signature.** On i386 it
selects which *frame* and which *return syscall* are in play: without it
Linux builds the old sigframe and expects `sigreturn` (119); with it, the
rt\_ frame and `rt_sigreturn` (173) go together. Getting this wrong is how
the test first failed — on Linux, not on Novaris. The rt\_ path is the one
glibc and Wine use, and is what Novaris implements.

**A missing `SA_RESTORER` must be accepted.** The first implementation
returned `-EINVAL`, on the reasonable-sounding grounds that a handler with
no way back would run off its own end. Linux accepts it and plants a
trampoline itself, so Novaris now does too — into the signal frame, which
works for the same reason Linux's does: 32-bit x86 without PAE has no NX
bit, so the user stack is executable. Being stricter was defensible in
isolation and would have made the same binary behave differently on the
two systems, which is the one thing this ABI must not do.

**Never restore user-supplied `cs`, `ss` or `eflags` wholesale.**
`rt_sigreturn` takes its data from the user stack, so a program could ask
for ring-0 selectors or `IOPL=3` on the way back. The restore keeps the
segment registers the kernel already trusts and masks `eflags` down to the
arithmetic flags.

### Verified: the same binary again, with nothing differing at all

`userland/signal_test.c`, same rules as Milestone 18's — raw `int $0x80`,
Linux numbers, `gcc -m32 -static -nostdlib -ffreestanding`, linked against
nothing:

```
$ make test-posix
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)
PASS: posixtest.elf behaves identically on Linux and Novaris
29 lines compared, 17 checks, 0 failing, 0 unexpected difference(s)
PASS: sigtest.elf behaves identically on Linux and Novaris
```

The signal test has **no expected differences at all** — all 29 lines are
byte-identical between Linux and Novaris, unlike the POSIX test where
`uname` legitimately differs.

What the 17 checks cover:

- a handler runs, receives the right signal number, runs again, and
  ordinary locals survive across it — the last of which is what
  `rt_sigreturn` is responsible for;
- a handler installed *without* `SA_RESTORER` still runs, on the
  kernel-planted trampoline;
- `rt_sigprocmask` genuinely blocks: the signal does **not** run its
  handler while blocked, stays pending, and arrives on unblock;
- and the one that matters:

```
4. SIGSEGV handler that fixes the fault and returns
  [ok]   rt_sigaction(SIGSEGV) accepted
  [ok]   mapped a page to make unwritable
  [ok]   made the page read-only
  [ok]   SIGSEGV handler ran once
  the faulting store now reads back as 1 (1 = it completed)
  [ok]   the faulting instruction was retried and succeeded
```

A store to a read-only page faults; the handler `mprotect`s it back to
read/write and returns; the store is retried and succeeds. That is Wine's
pattern, running unmodified.

The full 22-case smoke suite passes, repeated runs cost zero frames
(29474 across three `meminfo` calls with `sigtest.elf` run twice), and
host tests are 38 + 30 + 52 with no failures.

### Honest scope

- [x] `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`,
      delivery from both `kill` and CPU faults, blocking and pending,
      `SA_NODEFER`, `SA_RESETHAND`, per-handler `sa_mask`.
- [x] ~~**`ucontext_t` is not populated.** An `SA_SIGINFO` handler gets a
      valid, readable third argument full of zeroes.~~ Done in Milestone
      23, in both directions — a handler can also write registers back.
- [x] ~~`siginfo_t` carries only `si_signo`; `si_addr` (the faulting
      address) and `si_code` are zero.~~ Done in Milestone 23.
- [ ] One signal is delivered per return to ring 3, and `pending` is a
      bitmask, so a signal raised twice while blocked is delivered once.
      Real-time signal queueing is not implemented.
- [ ] No `sigaltstack`, no `sigsuspend`, no `sigpending`, no
      `sigtimedwait`. No `SIGALRM`, because there are no timers.
- [ ] Signal state is per *process*, not per thread: `kill` targets the
      whole program, and a Win32 program's several threads share one
      disposition table. Per-thread masks need `clone`/`gettid` semantics
      that do not exist yet.
- [ ] `SIGKILL` cannot be blocked, but nothing can send it from outside —
      there is one process and no `ps`.

## Milestone 20 — POSIX threads: clone, futex and TLS ✅ DONE

Third instalment of item 3. Wine is threaded, and it reaches threads the
Unix way: `clone()` to create them, `futex()` to make them wait, and a
gs-based TLS block so each has its own errno and thread pointer.

Novaris has had real preemptive threads since Milestone 16 and has run
Win32 programs on them since Milestone 17. What was missing was the POSIX
doorway to the same machinery — which is most of what this milestone is.
`clone` lands on `scheduler_spawn_posix_thread()`, and the scheduler that
already switches CR3 and the TEB descriptor now switches a TLS descriptor
too.

### What was actually new

**A TLS segment.** i386 TLS lives behind `gs` exactly as a Windows TEB
lives behind `fs`, so GDT entry 7 (selector `0x3B`) was added alongside
Milestone 10's entry 6, and `switch_to()` reprograms it per thread.
Without that, two threads would share one thread pointer, which is the
one thing TLS may not do.

**`clone` returning twice.** The child starts at the instruction after
the caller's `int $0x80` — which is exactly `regs->eip`, since the CPU has
already advanced past it — on the stack the caller supplied, with `eax`
set to 0 in its synthetic trap frame. The parent gets the tid from the
syscall's return. Only the thread shape (`CLONE_VM`) is supported; a
fork-shaped clone is refused with `-ENOSYS` rather than handed back a
child with a shared address space it does not expect.

**`futex`.** `FUTEX_WAIT` sleeps only if the word still holds the value
the caller passed, which is what closes the race between testing a lock
and going to sleep on it. Waiting reuses Milestone 17's mechanism —
rewind the trap frame so the whole syscall re-executes, and yield —
because the scheduler resumes tasks from a trap frame and cannot suspend
one mid-syscall. One detail that bit: the syscall number lives in `eax`
and is overwritten by the result, so the retry restores it before
rewinding `eip` past the two bytes of `int $0x80`; without that the
re-executed call would dispatch to whatever the return value happened to
name.

**`CLONE_CHILD_CLEARTID`.** The kernel zeroes a caller-nominated word
when the thread exits. That pairing with `futex` *is* `pthread_join`, and
implementing it is what lets a joiner stop waiting.

ELF programs also moved onto the scheduler, the same change Milestone 17
made for Win32 — a program's main thread has to be a scheduler task
before a second one can join it.

### Verified: a third binary, on Linux and on Novaris

`userland/thread_posix_test.c` uses no pthreads, because there is no libc
here — it is what pthreads is *made of*. Same rules as before: raw
`int $0x80`, `gcc -m32 -static -nostdlib -ffreestanding`, linked against
nothing.

```
$ make test-posix
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)
PASS: posixtest.elf behaves identically on Linux and Novaris
29 lines compared, 17 checks, 0 failing, 0 unexpected difference(s)
PASS: sigtest.elf behaves identically on Linux and Novaris
26 lines compared, 8 checks, 0 failing, 0 unexpected difference(s)
PASS: pthtest.elf behaves identically on Linux and Novaris
```

All 26 lines byte-identical, including:

```
4. per-thread TLS
  worker 1 read gs:0 = 2000
  worker 2 read gs:0 = 2001
  worker 3 read gs:0 = 2002
  [ok]   each worker read its own TLS tag
  [ok]   the main thread's TLS survived
```

Each thread reading its own tag out of `gs:0` is a direct test that the
segment really is per-thread, and the main thread's tag surviving three
context switches is a direct test that the descriptor is restored rather
than left wherever the last thread put it.

**The negative control.** A passing mutex test proves nothing if the
threads never raced. Rebuilding with `lock()` returning immediately, on
both systems:

| | with the mutex | without it |
| --- | --- | --- |
| Linux host | 60 | **36** |
| Novaris | 60 | **40** |

Twenty and twenty-four lost increments respectively — the race is real on
both, and the futex mutex is what prevents it.

That control also caught something first time round: with a 20,000-cycle
critical section the Novaris run came out at 60 *without* any lock,
because a 20ms time slice was long enough for each worker to finish
inside one and the threads never overlapped. The section had to be
widened to 300,000 cycles before the lock was genuinely under test — the
same lesson Milestone 17 taught, and worth recording twice.

### Two things the host taught, not the guest

Both were found because the test ran on Linux first.

**`CLONE_SETTLS` cannot allocate a TLS entry**, only use one. And the
entry number is not portable: i386 Linux uses GDT entries 6–8, a 32-bit
process on an x86-64 kernel gets 12–14, and Novaris has one, at 7. The
portable sequence — and what glibc does — is `set_thread_area(-1)` once to
allocate and *learn* the number, then name that entry in every subsequent
clone. The first version hardcoded `-1` everywhere and failed with
`-EINVAL` on Linux.

**Where the boundaries are is a kernel's choice, not the ABI's.** The
test computes its `gs` selector from the entry number the kernel returned
rather than hardcoding one, which is why the same binary works on a
kernel that answers 12 and a kernel that answers 7.

The full 23-case smoke suite passes, repeated runs cost zero frames
(29470 across three `meminfo` calls with `pthtest.elf` run twice, cloned
threads and all), and host tests are 38 + 30 + 52 with no failures.

### Honest scope

- [x] `clone` (thread shape), `futex` WAIT/WAKE, `set_thread_area`,
      per-thread TLS, `CLONE_CHILD_CLEARTID`, `CLONE_PARENT_SETTID`,
      `set_tid_address`, and thread-vs-process exit.
- [x] ~~**`futex` still does not block.** A waiter stays runnable and
      re-tests its word every slice. `FUTEX_WAKE` therefore has nothing to
      wake and only reports a count. Correct, and wasteful.~~ Done in
      Milestone 25 — and it turned out to be a correctness problem too,
      not only a wasteful one.
- [x] ~~timeouts — a `FUTEX_WAIT` timeout argument is ignored.~~ Done in
      Milestone 25.
- [ ] No `FUTEX_REQUEUE`, `FUTEX_WAIT_BITSET` or `FUTEX_LOCK_PI`.
- [ ] **No `fork`.** A clone without `CLONE_VM` returns `-ENOSYS`; a real
      fork needs copy-on-write, which does not exist.
- [ ] One TLS entry, not three. A program asking for a second gets
      `-EINVAL` rather than a descriptor aliasing the first.
- [ ] Signal state is still per process, so a `kill` cannot target one
      thread and `CLONE_SIGHAND` is accepted without meaning anything.
- [ ] 8 tasks total across the whole system, kernel stacks 8KB each.
- [ ] `gettid` returns the scheduler PID, and `getpid` is still a
      constant, so they disagree in a way real code could notice.

## Milestone 21 — A real glibc program runs ✅ DONE

Milestones 18–20 were verified with freestanding binaries: raw
`int $0x80`, no library, nothing between the program and the kernel. That
proves the ABI is right, and it quietly avoids the question of whether a
*real* program works — because a freestanding test only exercises what it
was written to exercise.

So this milestone asked the harder question directly: take an ordinary
`gcc -m32 -static` binary, linked against the host's production glibc,
and run it. Hundreds of kilobytes of C library that has never heard of
Novaris — its startup code, its allocator, its SSE2 string functions, its
printf, its float formatting.

It works now. Every obstacle below was **found by running it**, not
predicted from a list.

### Obstacle 1: no initial process stack

It faulted on the *second instruction* of `_start`:

```
xor %ebp, %ebp
pop %esi          <- argc, and the first instruction to touch the stack
```

The System V i386 ABI says a program is entered with `esp` pointing at
`argc`, then `argv`, a NULL, `envp`, a NULL, and the auxiliary vector.
Novaris had never built any of it, and had got away with it because every
ELF program in the tree was freestanding and never looked. The esp handed
over was the top of the mapping, with nothing at it.

`build_initial_stack()` in `process.c` now lays out the whole thing,
including the auxv entries glibc actually reads: `AT_PHDR`/`AT_PHENT`/
`AT_PHNUM` (how it finds its own program headers, and thence `PT_TLS`),
`AT_PAGESZ`, `AT_ENTRY`, `AT_CLKTCK`, `AT_SECURE`, and `AT_RANDOM` — the
sixteen bytes glibc reads to seed its stack guard and pointer mangling.

### Obstacle 2: SSE was never enabled

Next it hit an invalid opcode at `__strrchr_sse2_bsf`:

```
movd 0x8(%esp),%xmm1
```

glibc's string functions are SSE2. With `CR4.OSFXSR` clear, every one of
them is an illegal instruction. Novaris had ignored the FPU for twenty
milestones because the kernel never used it and one program at a time
meant nobody's state could be clobbered.

Enabling it (`CR0.MP` set, `CR0.EM` clear, `CR4.OSFXSR|OSXMMEXCPT`) is
three lines. The consequence is not: **once SSE is on, the XMM registers
are live state that preemption can corrupt**, and Novaris has had
preemptive threads since Milestone 16. Two threads inside glibc's `memcpy`
would quietly destroy each other's registers. So `kernel/fpu.c` also gives
every task a 16-byte-aligned 512-byte `FXSAVE` area, saved and restored in
`switch_to()` — enabling SSE and saving its state are one job, not two.

### Obstacle 3: eight missing syscalls

Read straight off the kernel's own `-ENOSYS` reports, rather than guessed:
`ugetrlimit`, `getrandom`, `clock_gettime`, `clock_gettime64`,
`set_robust_list`, `rseq`, `readlinkat`, `statx`, `fstatat64`. Most are
now implemented; `rseq` and `statx` are answered with `-ENOSYS`
deliberately, because glibc treats both as optional and falls back
cleanly — which is a different thing from not knowing about them.

### A real bug the probe exposed

Running a program that faults early revealed that Milestone 20 had broken
the fault path. Moving ELF programs onto the scheduler left
`handle_user_fault()` unwinding through `process_exit_to_kernel()`, whose
`kernel_resume_esp` by then held a stale value from whatever last used the
blocking path — so a faulting ELF program printed its diagnostic and then
**panicked the kernel**.

Nothing in the suite caught it, because no ELF test program had ever
faulted. `userland/crash_test.c` now does, and the fix routes through
`posix_exit_process()`, which picks the right unwind for how the program
was started.

The same probe also found that `elf_load()` mapped a fresh frame for a
page a previous segment had already populated, silently discarding the
earlier segment's bytes. Harmless for two-segment binaries; wrong for
anything with a `GNU_RELRO` tail, which is every real one.

### Verified

`glibc.elf` joins the transcript comparison, and it is the strictest of
the four — **nothing at all may differ**:

```
$ make test-posix
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)
PASS: posixtest.elf behaves identically on Linux and Novaris
29 lines compared, 17 checks, 0 failing, 0 unexpected difference(s)
PASS: sigtest.elf behaves identically on Linux and Novaris
26 lines compared,  8 checks, 0 failing, 0 unexpected difference(s)
PASS: pthtest.elf behaves identically on Linux and Novaris
 5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)
PASS: glibc.elf behaves identically on Linux and Novaris
```

```
novaris> run glibc.elf
hello from a real glibc-linked program
malloc and strcpy work
42 str 3.14
strlen=43 strchr=quick brown fox jumps over the lazy dog strrchr=og
sorted: -7 0 3 42 1000
```

That is glibc's allocator on Novaris's `brk`/`mmap`, glibc's `printf` and
float formatting on the x87 unit, glibc's SSE2 `strchr`/`strrchr`, and
glibc's `qsort` calling back into the program — with no `-ENOSYS` report
left.

25-case smoke suite passes. The first `glibc.elf` run costs 173 frames
(it is a 707KB image), and every run after it costs **zero**. Host tests
38 + 30 + 52, no failures.

### Honest scope — and what this does *not* mean

- [x] A statically linked, real-glibc C program runs correctly.
- [ ] **Static only.** There is still no dynamic linker, so a normally
      linked binary — anything needing `ld-linux.so.2` — does not run.
      That is item 4 on the Path A list and is untouched.
- [ ] `AT_BASE` is 0 and `PT_INTERP` is ignored, because there is no
      interpreter to load.
- [ ] No environment: `envp` is an empty vector, so `getenv` returns NULL
      for everything.
- [ ] `argv` is one entry, the literal string `"program"` — the shell does
      not pass a command line through to ELF programs yet.
- [ ] `getrandom` and `AT_RANDOM` are seeded from the PIT tick. They are
      *different* per run and nowhere near unguessable, which matters
      because glibc seeds pointer mangling from them.
- [ ] FPU state is saved per task, but there is no lazy switching
      (`CR0.TS`), so every context switch pays 512 bytes of FXSAVE
      whether the task used the FPU or not.

## Milestone 22 — Dynamic linking ✅ DONE

Item 4 on the Path A list. **A dynamically linked glibc program now runs
on Novaris**, through the real `ld-linux.so.2` loading the real
`libc.so.6` at runtime — the ordinary way a Linux program is built, and
the way Wine is built.

```
novaris> run dyn.elf
hello from a real glibc-linked program
malloc and strcpy work
42 str 3.14
strlen=43 strchr=quick brown fox jumps over the lazy dog strrchr=og
sorted: -7 0 3 42 1000
```

Byte-identical to the same binary on the Linux host.

### The kernel's half

Worth being clear about how little of dynamic linking is the kernel's
job. No relocations, no symbol resolution, no `.so` loading — all of that
is `ld-linux.so.2`'s. The kernel loads two images, hands the interpreter
a correct auxiliary vector, and gets out of the way:

- **ET_DYN images** at a kernel-chosen bias (`PIE_BASE 0x56000000`,
  `INTERP_BASE 0x5A000000`, far apart so a mistake faults rather than one
  overwriting the other).
- **`PT_INTERP`** — read the interpreter path, load that image too, enter
  *it* rather than the program.
- **`AT_BASE`**, which the linker uses to relocate itself before it can
  execute anything meaningful.
- **File-backed `MAP_PRIVATE` `mmap`.** With no page cache this
  degenerates to "allocate pages and read the bytes in", which is
  behaviourally right for a private mapping. `MAP_SHARED` is refused
  rather than faked.
- **`openat`, `pread64`, `access`/`faccessat`, `fstatat64`.** The linker
  probes with `access()` before it opens.
- **`MAP_FIXED` applies its protection** to pages already mapped — the
  reserve-then-overlay pattern a linker uses.
- Initrd paths resolve on their last component, so
  `/lib/i386-linux-gnu/libc.so.6` and `libc.so.6` name the same file.

### The two bugs, and how each was found

Neither was findable by reading code, and the tools that found them are
the durable part of this milestone.

**`st_ino` was 0 for every file.** The linker opened `libc.so.6`, read
its header, stat'ed it — and closed it without mapping anything. glibc's
`_dl_map_object_from_fd`, having opened a library, walks the list of
already-loaded objects looking for one with the same `st_ino` and
`st_dev`; that is how a library named two different ways gets loaded
once. With every file reporting inode 0, `libc.so.6` "matched" an object
already in the list, so the linker closed the file and returned *that*
map instead.

The symptom was nothing like the cause — five copies of `no version
information available`, then `undefined symbol: __libc_start_main`. What
found it was a **syscall tracer** (`strace <program>` in the shell, new
in this milestone) run against the host's `strace` of the same binary:
the host maps libc with five `mmap2` calls after the stat, and Novaris
went straight from stat to `close`. Files now report a stable, unique,
non-zero inode.

**Anonymous `MAP_FIXED` did not zero pages that were already mapped.**
With libc loading, the program ran perfectly and then faulted on exit
reading a garbage pointer. A dynamic linker maps a library's `.bss` with
`MAP_FIXED|MAP_ANONYMOUS` directly over pages the file mapping already
populated — and `map_range()` only zeroed pages it newly allocated, so
those kept the file's bytes. libc's exit-handler list lives in `.bss`, so
it held whatever happened to be at that file offset instead of NULL.

What found it was making the fault report say which address and what
kind: `accessing 0x444c05a4 (read, not mapped)` rather than just "a fault
happened". Anonymous memory now reads as zero unconditionally.

### Verified

`dyn.elf` joins the transcript comparison — five binaries now run on both
the Linux host and Novaris with their output diffed:

```
$ make test-posix
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)  posixtest.elf
29 lines compared, 17 checks, 0 failing, 0 unexpected difference(s)  sigtest.elf
26 lines compared,  8 checks, 0 failing, 0 unexpected difference(s)  pthtest.elf
 5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  glibc.elf
 5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  dyn.elf
```

26-case smoke suite passes. `dyn.elf` costs 52 frames on its first run
and **zero** on every run after. Host tests 38 + 30 + 52, no failures.

`ld-linux.so.2` and `libc.so.6` are copied from the host toolchain by the
Makefile rather than committed: they are build inputs, like mingw-w64's
import libraries, and vendoring megabytes of LGPL glibc into a hobby
kernel's source tree would be bloat and a licensing question nobody
needs.

### Honest scope

- [x] A dynamically linked program runs, through the real interpreter and
      the real shared C library.
- [ ] **One shared library deep.** `dyn.elf` needs only `libc.so.6`.
      Nothing has exercised a library that itself pulls in others, or
      `dlopen`.
- [ ] No `/etc/ld.so.cache`, so every lookup is a path search. Harmless
      here; slow with many libraries.
- [ ] The initrd is flat, so path resolution matches on the last
      component only. Two files with the same basename in different
      directories could not be told apart.
- [ ] No `PT_GNU_RELRO` enforcement by the kernel - the linker's own
      `mprotect` does it, which is where it belongs, but nothing checks.
- [ ] `st_ino` is the VFS node's address. Unique and stable for a mount,
      and not meaningful across boots.
- [ ] Still no writable filesystem, so `open` for writing is `-EROFS`.

## Milestone 23 — `ucontext_t` and `siginfo_t` ✅ DONE

The first item Milestone 19 left open, and the most concrete single thing
blocking Wine. An `SA_SIGINFO` handler is called as

```c
void handler(int signo, siginfo_t *info, void *ucontext);
```

and since Milestone 19 the second and third arguments had been valid,
readable pointers to zeroes. That is enough for a handler that reads its
signal number and no use at all to Wine, whose entire exception machinery
is *those two arguments*: it reads the faulting registers out of the
ucontext to build a Windows `CONTEXT`, decides what the `__except` chain
wants, writes the modified registers back into the same ucontext, and
returns. Both directions are now real.

### Both directions, and why the second one is the hard half

Reading is the obvious half: fill in the Linux/i386 `struct sigcontext`
inside `ucontext_t` from the trap frame, which is right there. The layout
is in `include/posix.h` and is written so every member is four bytes wide
— on Linux the segment selectors are a 16-bit value plus a 16-bit pad,
which is what lets glibc's `gregset_t` treat the whole thing as an array
of 19 longs indexed by `REG_GS`(0) … `REG_SS`(18). A handler indexes it
by offset and does not care what the kernel calls the fields.

Writing is the half that changes the design. Milestone 19's sigframe kept
a private `registers_t` copy of the interrupted state and `rt_sigreturn`
restored from *that*. Adding a ucontext beside it would have passed every
readable-state test and silently discarded every edit — which is exactly
the shape of bug that would make Wine's exception dispatch a no-op that
is very hard to see. So the private copy is gone: the interrupted state
now lives **only** in `uc.uc_mcontext`, the memory the handler was given
a pointer to, and `rt_sigreturn` restores from there. The signal mask
comes back from `uc_sigmask` for the same reason.

The safety rules from Milestone 19 survive intact. `cs`, `ss` and the
segment selectors are still never taken from the frame — a program that
asked for ring-0 selectors on the way back would get them — and `eflags`
is still masked down to the arithmetic flags plus a forced `IF`.

`siginfo_t` is filled in the same spirit. `si_code` distinguishes a
`kill` (`SI_USER`) from a `tgkill` (`SI_TKILL`) from a fault, and for a
page fault it splits `SEGV_MAPERR` (nothing mapped there) from
`SEGV_ACCERR` (mapped, but the access was refused) off the fault's error
code bit 0 — the distinction Wine uses to tell a guard-page hit from a
genuinely bad pointer. `si_addr` and `uc_mcontext.cr2` are `cr2`, read at
fault time. `trapno`, `err` and `cr2` are recorded on every fault and
reported in *every* subsequent sigcontext whether or not that signal was
a fault, because that is what Linux does — a signal delivered by `kill`
carries whatever the last real fault left behind, and matching even a
meaningless number is cheaper than a binary seeing two different answers.

### Three things checked against the host rather than assumed

Guessing what Linux puts in a field and then implementing the guess makes
a test that passes for the wrong reason. Three fields were wrong in the
first draft and were fixed by asking the host:

- **`uc_stack.ss_flags` is 0, not `SS_DISABLE`.** With no alternate
  signal stack the obvious answer is `SS_DISABLE` (2). A current Linux
  stores the task's own `sas_ss_flags` field there, not
  `sas_ss_flags(sp)`, so it reports 0 and lets `ss_size == 0` carry the
  "disabled" meaning.
- **`uc_flags` is 1 on Linux and 0 on Novaris**, and that is correct
  rather than a bug: it is `UC_FP_XSTATE`, set when the kernel has
  attached extended FP state. Novaris attaches none and leaves `fpstate`
  null, so 0 and a null pointer are consistent with each other. Nothing
  in the tests prints it.
- **`sizeof(ucontext_t)` is 364, not the 348 the field list adds up to.**
  A current glibc has grown a shadow-stack field past `__fpregs_mem`. The
  frame reserves 392 bytes so a handler that copies the whole struct — as
  `getcontext` does — cannot run off the end of it.

### Verified: the same test twice, with different headers

`userland/signal_test.c` grows six new sections (tests 5–10), and every
check is a *relation* between values the program already knows —
`si_addr == the address the store was aimed at`, `gregs[REG_EIP] == the
instruction after the interrupted syscall`. No absolute address, selector
or pid is printed, because those legitimately differ between the two
systems and a comparison that demanded they match would be testing the
wrong thing.

That test writes the two structures out by hand, because it links against
nothing — which means it proves Novaris matches *what this project
believes* Linux's layout to be. If the belief were wrong in both places
the test would still pass. So there is a sixth binary,
`userland/ucontext_test.c` → `uctest.elf`: an ordinary dynamically linked
glibc program where `siginfo_t`, `ucontext_t` and `REG_EIP` come from
`/usr/include`, running through the real `ld-linux.so.2` and `libc.so.6`.
It is almost literally Wine's access pattern:

```c
ucontext_t *context = sigcontext;
context->uc_mcontext.gregs[REG_EIP] = ...;
```

```
$ make test-posix
41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)  posixtest.elf
83 lines compared, 59 checks, 0 failing, 0 unexpected difference(s)  sigtest.elf
26 lines compared,  8 checks, 0 failing, 0 unexpected difference(s)  pthtest.elf
 5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  glibc.elf
 5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  dyn.elf
30 lines compared, 21 checks, 0 failing, 0 unexpected difference(s)  uctest.elf
```

Both transcripts are byte-identical between the Linux host and Novaris.
What `uctest.elf` proves, on Novaris:

```
2. a page fault, read through glibc's ucontext_t
  [ok]   si_code is SEGV_MAPERR (nothing mapped there)
  [ok]   si_addr is the address the store was aimed at
  [ok]   gregs[REG_TRAPNO] is 14 (page fault)
  [ok]   gregs[REG_ERR] says write, not-present, from user mode
  [ok]   uc_mcontext.cr2 agrees with si_addr
  [ok]   gregs[REG_EIP] is inside the faulting probe

3. the handler's write to gregs[REG_EIP] took effect
  [ok]   execution resumed where the handler pointed eip
  [ok]   the faulting store never happened
```

The last two are the Wine move in miniature: the handler does *not* fix
the fault, it redirects execution somewhere else by rewriting `eip`, and
the store that faulted is proved never to have happened.

Host tests are 38 + 30 + 52 with no failures, the 26-case smoke suite
passes, and `sigtest.elf` and `uctest.elf` both cost zero frames on every
run after the first.

### The negative controls, and one that mattered

A test that passes on the first run is a test to be suspicious of, so all
of this was broken deliberately three ways:

- **`rt_sigreturn` restores a private shadow copy instead of the
  ucontext** — the Milestone 19 design with a ucontext bolted on. Every
  read-only check still passes; `gregs[REG_EAX] written in the handler is
  what kill returned` fails, and the `eip`-rewriting test loops for ever
  retrying the faulting store, so the transcript never reaches the end
  and the comparison fails on length. This is the control that justifies
  deleting the shadow copy rather than keeping one for safety.
- **`si_code` and `si_addr` left at zero** — six checks fail, across
  `tgkill`, both `SEGV_` codes and both `si_addr`s.
- **The ucontext present but zeroed**, which is exactly what Milestone 19
  shipped — the program dies at the *first* signal, because
  `rt_sigreturn` now restores `eip = 0`:

```
[kernel] User program faulted (exception 14) at eip=0x00000000 accessing
         0x00000000 (read, protection)
```

### Honest scope

- [x] `ucontext_t` with a fully populated `sigcontext`: all 19 `gregs`,
      `trapno`, `err`, `cr2`, `oldmask`, `uc_sigmask`, `uc_stack`.
- [x] Edits a handler makes to `uc_mcontext` take effect on return —
      general registers, `eip`, `esp`, the arithmetic flags and the
      signal mask.
- [x] `siginfo_t` with `si_signo`, `si_errno`, `si_code`, `si_addr` for
      the fault signals and `si_pid`/`si_uid` for `kill`/`tgkill`.
      `SI_USER`, `SI_TKILL`, `SI_KERNEL`, `SEGV_MAPERR`, `SEGV_ACCERR`,
      `FPE_INTDIV`, `ILL_ILLOPN`.
- [ ] **No FP state.** `uc_mcontext.fpstate` is null and `uc_flags` is 0.
      Wine checks that pointer before using it, so this is survivable, but
      a handler that wants the x87/SSE registers of the faulting
      instruction cannot have them, and edits to FP state cannot be made
      at all. This is the obvious next piece of the same milestone.
- [ ] **`cs`, `ss`, `ds`, `es`, `fs` and `gs` are reported but not
      restored.** A handler can read them; writing them back is ignored.
      Wine does change `fs` in a sigcontext when it switches TEB
      selectors, so this will have to be revisited — safely, which means
      validating the selector rather than trusting it, and probably means
      LDT support that does not exist yet.
- [ ] `si_uid` is always 0, and `si_pid` is the argument the caller
      passed rather than a checked identity — there is one process.
- [ ] `siginfo` is one record per signal *number*, not a queue, because
      `pending` is still a bitmask. Two `SIGSEGV`s raised while blocked
      deliver once, with the second one's `si_addr`.
- [ ] Still no `sigaltstack`, so a handler cannot run on its own stack —
      which is how a real kernel survives a stack-overflow `SIGSEGV`, and
      which Wine uses.
- [x] ~~**No FP state.** `uc_mcontext.fpstate` is null and `uc_flags` is
      0.~~ Done in Milestone 24.
- [ ] **`cs`, `ss`, `ds`, `es`, `fs` and `gs` are reported but not
      restored.** A handler can read them; writing them back is ignored.
      Wine does change `fs` in a sigcontext when it switches TEB
      selectors, so this will have to be revisited — safely, which means
      validating the selector rather than trusting it, and probably means
      LDT support that does not exist yet.
- [ ] Signal state is still per *process*, not per thread.

## Milestone 24 — FP state in the signal frame ✅ DONE

The one thing Milestone 23 left null. `sigcontext.fpstate` pointed
nowhere, which had two consequences — one obvious and one that was a
live bug nobody had noticed.

The obvious one: a handler could not see the x87/SSE registers of the
faulting instruction. Wine casts that pointer straight to a Windows
`FLOATING_SAVE_AREA` and copies it into `CONTEXT.FloatSave`, so with a
null pointer it has nothing to report and nothing to restore.

The one that was a bug: because the FPU was neither saved nor reset
around delivery, **a signal handler that did any arithmetic at all
silently destroyed the interrupted computation's registers.** A handler
that pushed three values onto the x87 stack left the interrupted code
with three fewer, and its `st(0)` gone. Nothing in the suite caught it,
because no handler had ever done floating-point work.

### One structure in two shapes

`struct _fpstate` has its history visible in it. The first 112 bytes are
the legacy i387 environment — control, status and tag words, then eight
10-byte x87 registers — which is byte for byte Windows'
`FLOATING_SAVE_AREA`. Everything from offset 112 on is simply the
512-byte FXSAVE image, unaltered, which is where XMM and MXCSR live. The
`magic` field says whether that image is there: Wine tests it as
`fpstate->status >> 16`, reading `status` and `magic` as one 32-bit word,
which is why they are adjacent 16-bit fields rather than two ints.

Novaris writes both halves, and honours both on return, in the same
lopsided way Linux does: the FXSAVE image supplies XMM and MXCSR, and
the legacy fields then overwrite the x87 part of it. Both directions
matter because Wine writes both — `CONTEXT.FloatSave` into the legacy
half, `CONTEXT.ExtendedRegisters` into the image — and honouring only
one would leave half of a resumed thread's FP state stale.

### The bug in the tag word, and how the host found it

The two tag words are not the same thing. FXSAVE's is one bit per
register, "in use" or not; the legacy i387 one is two bits, "valid" /
"zero" / "special" / "empty", so building it means looking at each
register the CPU marked in use and classifying its value.

The first implementation did that and was still wrong, because **the two
are indexed differently**: the abridged word is by *physical* register,
while FXSAVE stores the registers in *stack* order with `ST(0)` first.
Bit *i* is about physical register *i*, which is stack slot `(i - TOP) &
7`. Without that rotation the conversion looks at the wrong registers
and, with three values pushed, reports `0x57FF` — "three empty registers
that hold zero" — where Linux reports `0x03FF`, "three registers holding
valid numbers".

This was invisible while `TOP` was 0 and would have been invisible
forever without a host to compare against. The test now asserts the exact
value `0xFFFF03FF` after a known `fninit` and three pushes, which is
precisely the check that distinguishes the two.

Two other fields were settled by asking the host rather than reasoning:
the handler is entered on a **completely** clean unit (empty x87 stack,
control word `0x037f`, **zeroed XMM registers**, MXCSR `0x1f80` — so
`fninit` alone is not enough, since it leaves XMM and MXCSR untouched),
and the legacy `cssel`/`datasel` come from the interrupted frame's `cs`
and `ds` rather than from the FXSAVE image's own FCS/FDS.

### The flags a handler is entered with

Chasing the direction flag through the same probes turned up a third
thing Novaris was not doing. Linux clears `DF`, `TF` and `RF` before
entering a handler and keeps all three in the saved copy, so they come
back on return. `DF` is the one that matters: the cdecl ABI lets a
function assume the direction flag is clear on entry and compiled code
does, so a program that legitimately had it set when the signal arrived
was handing its handler a string library that ran backwards.

### Verified

```
$ make test-posix
 41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)  posixtest.elf
 93 lines compared, 67 checks, 0 failing, 0 unexpected difference(s)  sigtest.elf
 26 lines compared,  8 checks, 0 failing, 0 unexpected difference(s)  pthtest.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  glibc.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  dyn.elf
 60 lines compared, 45 checks, 0 failing, 0 unexpected difference(s)  uctest.elf
```

Both signal transcripts remain byte-identical between the Linux host and
Novaris. `uctest.elf` reads the state through glibc's `struct _fpstate`;
`sigtest.elf` asserts the same behaviour through no structure at all, so
it holds whatever the layout turns out to be:

```
11. state a handler must not be able to damage
  [ok]   the handler was entered with the direction flag clear
  [ok]   the handler was given an empty x87 stack
  [ok]   the handler's own arithmetic gave the answer it should
  [ok]   the interrupted x87 stack survived (TOP back to 7)
  [ok]   and still holds the value pushed before the signal
  [ok]   the direction flag came back set
```

Host tests 38 + 30 + 52, the 26-case smoke suite passes, and both
binaries still cost zero frames on every run after the first.

### The negative controls

Four deliberate breaks, each failing a different, named set of checks:

- **`fpstate` left null**, as Milestone 23 shipped it — 15 checks fail.
- **No clean unit for the handler** — the handler sees the interrupted
  x87 stack, XMM and MXCSR, and its own arithmetic eats them.
- **`DF`/`TF`/`RF` not cleared** — the direction-flag checks fail in
  both binaries.
- **The tag conversion without the TOP rotation** — exactly one check
  fails, `the tag word marks three registers valid (0x03FF)`, which is
  the bug that was actually there.

### Honest scope

- [x] `sigcontext.fpstate` points at a real `struct _fpstate`: legacy
      i387 environment, all eight x87 registers, and the full FXSAVE
      image with XMM0–7 and MXCSR.
- [x] Restored on `rt_sigreturn`, from both halves, so a handler can
      edit FP state as well as read it.
- [x] The handler runs on a clean FPU and with `DF`/`TF`/`RF` clear;
      the interrupted thread gets all of it back.
- [x] MXCSR is masked to its defined bits before `FXRSTOR`, so a program
      cannot fault the kernel with a reserved bit set.
- [ ] **No XSAVE / extended state.** `uc_flags` is 0 where Linux reports
      `UC_FP_XSTATE`, and there is no xstate header past the FXSAVE
      image, so AVX registers are neither saved nor reported. Novaris
      does not enable AVX at all, so nothing can currently notice; a
      program that used it would lose `ymm` state across a signal.
- [ ] The FXSAVE image is copied through the user stack unvalidated
      apart from MXCSR. `FXRSTOR` tolerates any other bit pattern, so
      this is safe, but a program can put nonsense in its own x87
      registers — which is true on Linux too.
- [ ] `fpstate` is not shared with `sigaltstack`, which still does not
      exist. The frame is 624 bytes larger than it was, all of it on the
      user stack.

## Milestone 25 — futexes that really block ✅ DONE

Milestone 20 implemented `FUTEX_WAIT` as a retry loop and said the cost
out loud: a waiting thread stayed runnable and burned its slices
re-testing its word. This milestone makes it a real block — and the
interesting part is that the reason it had not been done was wrong.

### The thing that was believed to be hard

Milestone 20's note, repeated in every summary since, said real blocking
needed *"a second switch path that can suspend and resume a task's kernel
stack, because the scheduler currently resumes only from a trap frame."*

That is exactly backwards. The scheduler resuming only from a trap frame
is not the obstacle, it is the *mechanism*. `scheduler_yield_from_trap()`
already saves `regs` — the task's complete ring-3 state, the frame the
kernel is about to `iret` from — as its resume point. That is a full,
resumable snapshot of the task. Blocking is that same save plus a state
the scheduler declines to pick:

```c
current->esp = (uint32_t)regs;
current->wait_addr = uaddr;
current->state = PROC_BLOCKED;
switch_to(next);
```

Nothing about kernel stacks needed inventing. The whole change is one
enum value, two fields on `process_t`, three small functions, and
`pick_next_ready()` skipping `PROC_BLOCKED` the way it already skipped
`PROC_ZOMBIE`.

### It was also wrong, not just wasteful

That is the part the old tests could not see, and it is why this is a
correctness milestone rather than a performance one. Three things a
program can observe were different on Novaris than on Linux:

- **`FUTEX_WAIT` returned `-EAGAIN` when it was woken, not `0`.** Under
  retry the syscall re-executed until the word changed, and the call that
  finally noticed the change reported "the value was not what you
  expected" — which is a different answer to the same question.
- **`FUTEX_WAKE` always returned 0.** There was no wait queue, so there
  was nothing to count. Callers use that number.
- **The timeout argument was ignored**, which quietly turns every bounded
  wait into an unbounded one.

Waking a blocked task has to supply the return value, and that falls out
of the same observation: `p->esp` points at the task's saved trap frame,
so writing `eax` *there* is writing the syscall's return value.

### Three things that had to be got right

**`pthread_join` stopped working for free.** `CLONE_CHILD_CLEARTID`
zeroes a word when a thread exits, and under retry that *was* the wake —
the joiner was on the run queue and would notice. A joiner that really
sleeps has to be told, so thread exit now issues an explicit wake.

**Blocking the last runnable task would wedge the machine.** If nothing
else can run there is nobody to hand the CPU to and nobody left to
perform the wake, so `scheduler_block_current()` declines and reports it,
and the caller falls back to Milestone 20's retry. That is not a
compromise: when there is no one else to give the CPU to, spinning costs
nothing.

**Every task blocked at once is a deadlock in the program, and must not
become one in the machine.** `pick_next_ready()` returning 0 while tasks
are still alive would have let callers unwind past them. It now wakes
every blocked task with `-EAGAIN` first — a value every futex caller
already handles, since it means "look again" — so the program degrades to
the spin it would have done before rather than vanishing.

### The number that makes the case

A timed wait is the one case where blocking cannot help, because a
single-threaded program waiting on a timeout has no other task to hand
the CPU to. Under the retry loop the syscall is re-entered from ring 3
every time round:

```
--- retry-only kernel, a 1.5s timed wait ---
Futex waits: 1248002 (0 blocked, 1248002 spun)
```

1.25 million round trips through `int $0x80` to wait one and a half
seconds. So that path now idles in the kernel instead — `sti; hlt` until
the deadline or the word changes, exactly as `sys_nanosleep` does (the
syscall gate is an interrupt gate, so `IF` has to be set by hand or the
timer that ends the wait can never arrive).

The whole of `pthtest.elf`, which spawns three threads, contends a futex
mutex 60 times, joins all three, blocks a fourth thread on a wake and
then waits 1.5 seconds on a timeout:

```
novaris> futexinfo
Futex waits: 6 (5 blocked, 0 spun, 1 idled), woken: 2, blocked now: 0
```

Six waits. Five parked and cost nothing until woken; one idled. Zero
slices spent re-testing anything.

### Verified

`pthtest.elf` grows two sections, and they assert the return values
rather than only the effects — which is what nothing had done before:

```
6. futex return values
  [ok]   FUTEX_WAKE reports how many it woke
  [ok]   FUTEX_WAIT returns 0 when it is woken
  [ok]   a wake with nobody waiting reports 0

7. futex with a timeout
  [ok]   a wait nobody satisfies returns -ETIMEDOUT
  [ok]   and it really waited
```

The elapsed check in the second one matters: returning `-ETIMEDOUT`
immediately would satisfy the return value and none of the meaning.

```
$ make test-posix
 41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)  posixtest.elf
 93 lines compared, 67 checks, 0 failing, 0 unexpected difference(s)  sigtest.elf
 37 lines compared, 15 checks, 0 failing, 0 unexpected difference(s)  pthtest.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  glibc.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  dyn.elf
 60 lines compared, 45 checks, 0 failing, 0 unexpected difference(s)  uctest.elf
```

Host tests 38 + 30 + 52, the 26-case smoke suite passes, and repeated
runs still cost zero frames.

### The negative control

Forcing the old behaviour — `scheduler_block_current()` never called —
does not merely fail a check. `pthtest.elf` **hangs** at section 6 and
the transcript never reaches the end, because the waiter's word is
deliberately never changed: only a real wake can end that wait, and under
retry there is no such thing. That is the strongest form the control
could take, and it is why the section is written that way.

### Honest scope

- [x] `FUTEX_WAIT` blocks, `FUTEX_WAKE` wakes and counts, timeouts work,
      and a woken waiter gets the return value Linux gives it.
- [x] `pthread_join`'s `CLONE_CHILD_CLEARTID` wake.
- [x] A timed wait with nothing else runnable idles in the kernel rather
      than re-entering the syscall a million times.
- [ ] **The wait queue is a linear scan of the process table**, which is
      8 entries. Fine at this size and wrong at any real one.
- [ ] **No `FUTEX_REQUEUE`, `FUTEX_WAKE_OP`, `FUTEX_WAIT_BITSET` or
      priority inheritance.** glibc's condition variables use requeue on
      some paths; they have not been exercised here.
- [ ] Waiters are woken in table order, not FIFO, and there is no
      fairness guarantee.
- [ ] An untimed wait with no other runnable task still spins. It is a
      deadlock in the program either way, but Linux would leave the
      process asleep rather than burning the CPU.
- [ ] `FUTEX_PRIVATE_FLAG` is masked off and ignored: there is one
      address space per program and no shared memory between programs, so
      every futex is private in practice.
- [ ] Win32's `WaitForSingleObject` and critical sections still use
      Milestone 17's retry loop. They could be moved onto this, and
      should be.

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
   space), and for Win32 programs in Milestone 17 (`CreateThread`,
   `WaitForSingleObject`, working critical sections). Milestone 20 added
   per-thread TLS and Milestone 25 real blocking, for POSIX threads.
   Still to do: moving Win32's waits onto the same blocking path, and
   event/mutex objects.
3. **A POSIX-ish syscall surface** — ⏳ in progress. Milestone 18 adopted
   the real Linux/i386 ABI and implemented the memory and file halves
   (`mmap2`/`munmap`/`mprotect`/`brk`, a descriptor table,
   `open`/`read`/`write`/`writev`/`lseek`/`fstat64`), verified by running
   one binary on both Linux and Novaris. Milestone 19 added real signals
   — delivery, masking, `rt_sigreturn`, and a `SIGSEGV` handler that can
   fix a fault and resume the faulting instruction, which is the pattern
   Wine is built on. Milestone 20 added `clone`, `futex` and per-thread
   TLS — enough for what pthreads is made of. Milestone 23 filled in
   `ucontext_t` and `siginfo_t`, in both directions: a handler can read
   the faulting registers and write them back, which is the whole of
   Wine's exception dispatch. Milestone 24 added the x87/SSE half of the
   same picture, and Milestone 25 made futexes really block. Still to do:
   a writable filesystem.
4. **A dynamic linker** — ✅ done in Milestone 22. `ld-linux.so.2` loads
   `libc.so.6` at runtime and a dynamically linked glibc program runs,
   with output identical to Linux. The kernel's half is ET_DYN images,
   `PT_INTERP`, `AT_BASE`, file-backed `mmap` and the file syscalls; the
   linking itself is the interpreter's job.
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
   make && make test && make test-qemu && make test-posix
   ```

   `mingw-w64` builds the Windows test programs in `userland/pe_test/`;
   `gcc-multilib` builds the host-side tests in `tests/`. Since Milestone
   10 the kernel mirrors its console to COM1, so verification no longer
   means OCR'ing framebuffer screenshots — `make test-qemu` boots the
   ISO, drives the shell through the QEMU monitor, and matches the serial
   transcript against expected output.
