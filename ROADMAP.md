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
- [x] ~~The initrd is read-only, so `open` for writing returns `-EROFS`
      and there is no `unlink`, `mkdir`, `rename` or `creat`.~~ Done in
      Milestone 26.
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
- [x] ~~The initrd is flat, so path resolution matches on the last
      component only.~~ Done in Milestone 26 — resolution is real, with
      the fallback kept only for paths whose directory does not exist,
      which is what the linker's search needs.
- [ ] No `PT_GNU_RELRO` enforcement by the kernel - the linker's own
      `mprotect` does it, which is where it belongs, but nothing checks.
- [ ] `st_ino` is the VFS node's address. Unique and stable for a mount,
      and not meaningful across boots.
- [x] ~~Still no writable filesystem, so `open` for writing is
      `-EROFS`.~~ Done in Milestone 26.

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

## Milestone 26 — a writable filesystem ✅ DONE

The last item on Path A's "still to do" list before pulling in Wine
itself. `open` for writing returned `-EROFS`, there was no `unlink`,
`mkdir` or `rename`, and a path resolved on its last component alone — so
`/lib/i386-linux-gnu/libc.so.6` and `libc.so.6` named the same file.

Those look like two problems and are one. Wine does not merely want to
write files, it wants a *tree*: a prefix directory it creates, a registry
it rewrites, directories it makes and removes. A flat read-only archive
is not a filesystem with writing missing — it is a different thing.

So the initrd stops being the filesystem and becomes what it always
actually was: a source of initial contents. `kernel/initrd.c` is gone and
`kernel/ramfs.c` replaces it — a hierarchical, writable in-memory
filesystem, populated from the archive at boot.

### What did not change, which is the point

Every existing consumer — the shell, the desktop's file browser, the ELF
loader, the PE loader, `win32_kernel32.c` — needed **no changes at all**.
`vfs_node_t` kept its `read`/`readdir`/`finddir` hooks and grew tree
links beside them, so `vfs_readdir(vfs_root, i)` still walks the root's
children and `vfs_finddir(vfs_root, name)` still finds one. The
Milestone 6 VFS abstraction earned its keep twenty milestones later.

### Copy-on-write, and the number that justifies it

An initrd-backed file's bytes are **not** copied at mount. `data` points
into the initrd image and `from_initrd` says so; the copy happens on the
first write to that file, and only to that file.

That is not a micro-optimisation, and the control measures it. Copying
everything at boot instead:

```
--- no copy-on-write, at boot ---
Physical frames: 26760 free / 32736 total     (with copy-on-write: 28663)
```

**1903 frames — 7.4 MB — spent at boot to duplicate an archive already in
memory**, most of it `libc.so.6`. With copy-on-write the whole filesystem
costs the node pool and nothing else, and `meminfo` before and after a
`fstest.elf` that creates, writes, renames and removes a dozen files is
unchanged.

Verified end to end from the shell, which is also the only place the
copy-on-write is directly observable:

```
novaris> cp readme.txt hello.txt
copied 2110 bytes to hello.txt
novaris> cat hello.txt
Welcome to the Novaris initrd!  ...     <- readme's bytes
novaris> cat readme.txt
Welcome to the Novaris initrd!  ...     <- and the archive is intact
```

### The fallback that had to be conditional

Real path resolution breaks dynamic linking, because `ld-linux.so.2`
searches a list of directories: it probes `/lib/i386-linux-gnu/libc.so.6`,
`/usr/lib/libc.so.6` and several more, and none of those directories
exist. So a path whose *directory part does not resolve* still falls back
to matching the last component at the root.

The condition is the whole of it. Applied unconditionally — which is what
a flat archive effectively does — unlinking `/tmp/x/hello.txt` and then
opening it would find the root's `hello.txt` and report success. The test
checks exactly this, and the control confirms it is the only thing
standing between the two behaviours:

```
--- basename fallback applied unconditionally ---
  [FAIL] a name that exists elsewhere does not shadow a miss here
```

That is one check, failing for one reason. A fallback that quietly finds
the wrong file is worse than no fallback at all.

### Verified

A seventh comparison binary, `userland/fs_test.c` → `fstest.elf`, raw
`int $0x80` and linked against nothing like the rest. Everything happens
under `/tmp/novaris_fstest`, which it creates and removes, so the host run
owns everything it touches and leaves nothing behind.

File semantics are unusually good material for this, because they are
*precise*. `rmdir` on a non-empty directory is `ENOTEMPTY` and not
`ENOENT`; `unlink` on a directory is `EISDIR` and not `EPERM`; `O_APPEND`
re-evaluates the end on every write rather than seeking once. Each is a
specific number the two systems either agree on or do not.

```
$ make test-posix
 41 lines compared, 20 checks, 0 failing, 0 unexpected difference(s)  posixtest.elf
 93 lines compared, 67 checks, 0 failing, 0 unexpected difference(s)  sigtest.elf
 37 lines compared, 15 checks, 0 failing, 0 unexpected difference(s)  pthtest.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  glibc.elf
  5 lines compared,  0 checks, 0 failing, 0 unexpected difference(s)  dyn.elf
 60 lines compared, 45 checks, 0 failing, 0 unexpected difference(s)  uctest.elf
 67 lines compared, 45 checks, 0 failing, 0 unexpected difference(s)  fstest.elf
```

45 checks across `mkdir`/`rmdir`, create/write/read-back, `O_APPEND`,
`lseek`, `ftruncate` and `O_TRUNC`, `rename`, `getdents64` (including the
synthesised `.` and `..`), and eight distinct error cases — byte-identical
to the Linux host. Host tests 38 + 30 + 52, the 26-case smoke suite
passes, and repeated runs cost zero frames.

`dyn.elf` still runs, which is the other thing this milestone could
plausibly have broken: real path resolution plus the conditional fallback
still lets `ld-linux.so.2` find `libc.so.6` through paths that do not
exist.

### The negative controls

- **The basename fallback made unconditional** — one check fails, the one
  written for it.
- **Writes accepted and discarded** — three checks fail, and exactly the
  three about *content* rather than size, since `length` is still
  updated. Precisely attributable is the point.
- **Copy-on-write removed** — nothing fails, and 1903 frames disappear at
  boot. A control does not have to break a test to be worth running.

### The shell got a filesystem too

`ls [dir]` takes a path and marks directories; `mkdir`, `rm` and `cp` are
new. They are what make the feature usable by hand rather than only by a
test, and `cp` is how the copy-on-write above was demonstrated.

### Honest scope

- [x] A hierarchical, writable filesystem: `open` with
      `O_CREAT`/`O_TRUNC`/`O_APPEND`/`O_EXCL`, `write`, `ftruncate64`,
      `truncate64`, `unlink`, `unlinkat`, `mkdir`, `mkdirat`, `rmdir`,
      `rename`, `renameat`, `renameat2`, `getdents64`, `fsync`, `chdir`,
      and `stat` that knows what a directory is.
- [x] Real path resolution, `.` and `..`, and directories that nest.
- [ ] **Nothing persists.** It is a RAM filesystem: every boot starts
      from the initrd again. Persistence needs a block driver and a real
      on-disk format, which is a milestone of its own.
- [ ] **No per-process working directory.** `chdir` is accepted for a
      directory that exists and then ignored, and every relative path
      resolves from the root. `getcwd` always says `/`. That is wrong for
      any program that relies on it, and is the next thing to fix here.
- [ ] No permissions, ownership or timestamps. Everything is mode 0755
      and `access()` says yes to anything that exists — which is a
      simplification in the direction that does not break programs, but
      is a simplification.
- [ ] No hard links, symlinks, or `O_DIRECTORY` enforcement. `st_nlink`
      is always 1.
- [ ] 256 nodes, fixed. Files grow by doubling and are one contiguous
      heap block each, so a very large file needs a very large
      allocation.
- [ ] `rename` will not move a directory onto an existing name, and there
      is no cross-directory loop check — there is no way to make a cycle
      without directory renames, so nothing can currently trip it.
- [ ] The initrd is still capped (64 entries), still packed by
      `userland/mkinitrd.py`, and still has no directories of its own -
      everything it carries lands at the root.

## Milestone 27 — real Wine runs ✅ DONE (as far as it goes)

Path A step 5: *"Pull in real Wine source, cross-compile it against that
surface, and find out what is still missing."*

**Wine 11.0 runs on Novaris and prints its own version string.**

```
novaris> run preload.elf wineldr.elf --version
preloader: Warning: failed to reserve range 00010000-00110000
[posix] unimplemented syscall 102 -> -ENOSYS
[posix] unimplemented syscall 102 -> -ENOSYS
wine-11.0
```

That is real Wine, unmodified, built from wine-mirror's `stable` branch
with `gcc -m32` and `i686-w64-mingw32-gcc`. The chain behind those three
lines is the whole of Milestones 18–26 working at once:

1. `wine-preloader` — a static freestanding ELF — loads and runs;
2. it reserves what address space it can, and prints *its own warning*
   about what it cannot;
3. it maps the real `wine` loader segment by segment, then
   `ld-linux.so.2`, and jumps to it;
4. the interpreter loads `libc.so.6`, relocates, sets up TLS;
5. Wine's `main()` runs, finds its own installation through
   `readlink("/proc/self/exe")`, and loads **`ntdll.so` — 3.2 MB of real
   Wine**;
6. ntdll initialises far enough to parse the command line and answer it.

Read the honest version of that: what runs is Wine's *loader and its
Unix-side ntdll*, far enough to handle `--version` and `--help`. No
Windows program has been run. The gap between here and that is described
below and it is not small.

### What step 5 was for: what it found

Every item below was found by running the thing, not by reading it.

**A ring-3 `mmap` could hang the machine, and it was not Wine's fault.**
The preloader's second reservation is `0x10000-0x110000`, the DOS area.
Novaris identity-maps the first 4 MB into every address space because the
kernel's own image lives at physical `0x100000` and has to stay reachable
there — so `MAP_FIXED` across that range replaced the kernel's view of
itself. The machine stopped dead: no panic, no output, nothing.

Six lines of C reproduce it with no Wine involved, which is how it was
isolated, and a 64 KB-at-a-time bisection named the exact page:

```
mapping 0x00010000 ... ok      <- everything below 1MB is fine,
   ...                            including straight over the VGA buffer
mapping 0x000f0000 ... ok
mapping 0x00100000 ...         <- and here the machine stops
```

`paging_reserve_region()` already existed and already knew which ranges
belong to the kernel — the framebuffer, the initrd, the Win32 arenas.
`sys_mmap2` had simply never asked it. It does now, and
`userland/kmap_test.c` is the regression: it asserts not a return value
but a *survival*, since the kernel has to still be running to report
anything at all. It is deliberately not part of `make test-posix`,
because on Linux that same mapping legitimately succeeds.

**`argv` had to become real.** Every ELF program on Novaris was entered
with one hardcoded argument, `"program"`, and nothing had ever looked —
so nothing had noticed. Wine's loader takes what it is running as
`argv[1]`, and said so in its usage message on the very first run:

```
novaris> strace preload.elf
Usage: program wine_binary [args]      <- "program" is Novaris's argv[0]
```

`run` and `strace` now take arguments and pass a real `argv`.

**Four syscalls, each found the same way — by something failing.**

- `readlink("/proc/self/exe")` (85). Wine finds its own installation this
  way; without it, `wine: could not load ntdll.so: (null)`.
- `getcwd` (183) — the fallback for the same question. It had a number in
  `posix.h` and no implementation.
- `_llseek` (140). glibc's stdio reaches for this rather than `lseek`, so
  reading a file through `FILE*` needs it. Its absence was invisible
  until it was not: Wine asked glibc for the passwd entry, glibc could
  not seek, `getpwuid()` returned NULL, and Wine dereferenced it. The
  failure looked like a null-pointer bug in Wine and was a missing
  syscall two layers down.
- `prctl` (172) — `PR_SET_NAME`. Accepted and ignored.

**Two files, not code.** glibc's `getpwuid()` wants `/etc/passwd` and
`/etc/nsswitch.conf`. The `wine-initrd` target supplies both. Worth
recording because it is the kind of thing an ABI checklist does not have
on it.

### What stops it going further, in order

**`socketcall` (102), and it is architectural.** Wine is a client/server
system: ntdll talks to `wineserver` over a Unix domain socket, and every
process, thread, handle and synchronisation object lives on the far side
of it. Novaris has no sockets of any kind — not because the syscall is
missing but because there is no IPC to put behind it. Nothing beyond
ntdll's own initialisation can work until there is. This is a milestone
of its own, and it is the real answer to "what is missing".

**Address-space reservation.** The preloader asks for `0x110000-0x68000000`
— 1.66 GB of `PROT_NONE|MAP_NORESERVE` — so that Windows images can be
loaded low later. Novaris's `mmap` commits a physical frame per page,
which would need 435 000 frames out of 32 736. It never came up in this
run only because the preloader drops any range its own stack sits inside,
and Novaris's user stack is at `0x40100000`, inside that range. That is
luck, not design. A reservation that costs nothing is the next piece of
memory-management work, and `mprotect` making it real on demand is the
other half.

**No per-process working directory.** `getcwd` answers `/` always. Wine
builds its prefix path from it.

**`statx` and `rseq` return `-ENOSYS`** and glibc falls back cleanly, so
they cost nothing today — recorded because that is worth knowing rather
than rediscovering.

### How to reproduce it

Wine is **not vendored**, for the same reason `libc.so.6` is not: it is a
build input, and several hundred megabytes of LGPL source in a hobby
kernel's tree would be bloat and a licensing question nobody needs.
Wine's code stays behind a `WINE_BUILD` path and Novaris's stays in front
of it.

```bash
git clone --depth 1 -b stable https://github.com/wine-mirror/wine
cd wine
CC="gcc -m32" ./configure --enable-archs=i386 --disable-tests \
    --without-x --without-freetype --without-vulkan --without-opengl ...
make -j4
cd ../NovarisOS && make WINE_BUILD=../wine wine-initrd
qemu-system-i386 -cdrom novaris-wine.iso
novaris> run preload.elf wineldr.elf --version
```

`novaris.iso` stays reproducible from this tree alone; `novaris-wine.iso`
is a separate target that needs the Wine tree.

### Verified

Everything that was green stays green. Host tests 38 + 30 + 52; the smoke
suite is now 10 transcript assertions (two of them the new kernel-mapping
guard); all seven comparison binaries remain byte-identical to the Linux
host:

```
 41 lines, 20 checks  posixtest.elf      60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf        67 lines, 45 checks  fstest.elf
 37 lines, 15 checks  pthtest.elf         5 lines,  0 checks  glibc.elf
                                          5 lines,  0 checks  dyn.elf
```

### Honest scope

- [x] Real Wine builds against Novaris's syscall surface with the real
      toolchain, and its loader, `ntdll.so` and command-line handling run.
- [x] A ring-3 program can no longer map over the kernel.
- [x] Real `argv`; `readlink("/proc/self/exe")`, `getcwd`, `_llseek`,
      `prctl`.
- [ ] **No Windows program has been run.** `wine notepad.exe` needs
      wineserver, which needs Unix sockets, which need IPC that does not
      exist. Everything past ntdll's initialisation is blocked on that
      one thing.
- [ ] No address-space reservation: `PROT_NONE` still commits frames.
- [ ] No `fork`/`exec`, so Wine could not start a server process even if
      it could talk to one.
- [ ] Wine's PE builtins (`kernel32`, `user32`, …) were built by
      mingw-w64 and have never been loaded — that is ntdll's job, past
      the point reached here.
- [ ] No GDI backend, no display, no registry. Milestone 8's estimate
      that a display backend "is the obvious one" was wrong only in
      ordering: IPC comes first.

## Milestone 28 — Unix domain sockets ✅ DONE

Milestone 27 ran real Wine as far as `wine --version` and found that what
stopped it going further was not a missing syscall but a missing
*subsystem*. Wine is a client/server system: ntdll talks to wineserver
over a Unix domain socket, and every process, thread, handle and
synchronisation object lives on the far side of it.

So: `AF_UNIX`, `SOCK_STREAM`, and the one feature that makes it more than
a pipe — `SCM_RIGHTS`.

### One syscall, eighteen operations

On i386 every socket operation comes through `socketcall` (102): the
operation number in `ebx`, a pointer to *that operation's* argument block
in `ecx`. A 1990s space saving that every i386 libc still speaks, and the
reason a program cannot simply call `socket`.

The shape of a stream socket underneath is two byte queues and a peer
pointer — what you write goes into the peer's queue, what you read comes
out of your own. `socketpair`, `bind`/`listen`/`connect`/`accept`,
`shutdown` and end-of-stream are bookkeeping around that.

**Waiting is real, and it reuses Milestone 25.** A receive with nothing
to read parks the task and the sender wakes it. Getting there needed one
new piece: an implementation that cannot resume mid-call needs its whole
syscall re-executed on wake, and the retry path already rewinds `eip` for
exactly that — so the two were joined. `scheduler_block_current_retry()`
parks a task whose frame is already rewound, and waking such a task must
*not* write a result into `eax`, because `eax` now holds the syscall
number and overwriting it would send the re-executed `int $0x80` to
whatever that value happened to name. That is Milestone 20's retry loop
and Milestone 25's blocking finally being the same mechanism.

### SCM_RIGHTS, which is the point

A descriptor sent over a socket arrives at the other end as a descriptor.
That is how wineserver hands a client the fd behind a Windows `HANDLE`,
and a socket layer without it would look complete and be useless for the
thing it was built for.

What travels is the *object*, not the number — the number is the
sender's and means nothing at the other end. `kernel/socket.c` owns
sockets and `kernel/posix.c` owns descriptors, so the seam between them
is four functions, of which `posix_fd_export`/`posix_fd_import` are the
whole of SCM_RIGHTS.

### Three error numbers, checked rather than guessed

The first draft of the test got two of these wrong, and the host said so:

- **Connecting to a name nobody bound is `ENOENT`, not `ECONNREFUSED`.**
  The name is a filesystem path and there is no file. `ECONNREFUSED` is
  for a name that *is* bound with nobody listening. Wine reads which one
  it got to decide whether to start a wineserver of its own, so the
  number matters and not just the failure.
- **An unknown socket type is `EINVAL`; an unknown protocol is
  `EPROTONOSUPPORT`.** Linux distinguishes them.
- And `AF_INET` had to come out of the test entirely: on Linux it
  succeeds, because Linux has networking. Novaris refuses it, which is
  correct here and not comparable there.

### Verified

An eighth comparison binary, `userland/sock_test.c` → `socktest.elf`,
raw `int $0x80` like the rest, working entirely under `/tmp` and cleaning
up after itself:

```
$ make test-posix
 41 lines, 20 checks  posixtest.elf     60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf       67 lines, 45 checks  fstest.elf
 37 lines, 15 checks  pthtest.elf       34 lines, 23 checks  socktest.elf
  5 lines,  0 checks  glibc.elf          5 lines,  0 checks  dyn.elf
```

23 checks across socketpair in both directions, end-of-stream after
`shutdown`, bind/listen/connect/accept with a client that connects before
the server accepts, passing a file descriptor and reading the file
through the descriptor that arrived — byte-identical to the Linux host.
Host tests 38 + 30 + 52, smoke suite 10 assertions.

And real Wine uses it. With sockets present, ntdll's server connection
attempt now runs:

```
[trace] socketcall(0x1, ...) = 0x00000003     <- socket(AF_UNIX, SOCK_STREAM)
[trace] socketcall(0x3, ...) = -2             <- connect(): nothing bound
[trace] close(0x3, ...)      = 0
```

which is exactly right: there is no wineserver, so there is nothing to
connect to, and Wine gets the `ENOENT` it uses to decide to start one.

### The negative controls

- **SCM_RIGHTS accepted and the descriptor dropped** — two checks fail,
  and precisely the two about the descriptor arriving and being readable.
- **`connect` returning `ECONNREFUSED` for an unbound name** — one check
  fails, the one written for it.
- **The pair made one-directional, i.e. a pipe** — the test *hangs*, at
  `it carries traffic the other way too`, because the reverse receive
  blocks for data that can never come. Worth noting for what it proves
  as well as what it breaks: the blocking receive genuinely blocks rather
  than spinning or returning something wrong.

### Honest scope

- [x] `AF_UNIX`/`SOCK_STREAM`: `socket`, `socketpair`, `bind`, `listen`,
      `connect`, `accept`, `accept4`, `send`, `recv`, `sendto`,
      `recvfrom`, `sendmsg`, `recvmsg`, `shutdown`, `getsockname`,
      `getpeername`, `setsockopt`, `getsockopt`.
- [x] `SCM_RIGHTS`, and blocking receives that really block.
- [x] Sockets are ordinary descriptors: `read`, `write` and `close` work
      on them, and Wine uses them both ways.
- [ ] **No `SOCK_DGRAM`**, no `MSG_OOB`, no `SO_PEERCRED`, no
      `poll`/`select`/`epoll`. A program that multiplexes descriptors
      rather than blocking on one has nothing to use yet, and Wine's
      server loop does exactly that — this is the next piece.
- [ ] **A descriptor passed by SCM_RIGHTS gets its own file offset**,
      where Linux would have both refer to one open file description and
      share it. There is no open-file-description layer here to share.
- [ ] No networking of any kind. `AF_INET` is refused rather than faked.
- [ ] 32 sockets, 8 bound names, 16 KB per direction, 8 descriptors in
      flight — all fixed, all small, all deliberate.
- [ ] A bound name lives in a table, not in the filesystem: nothing shows
      it in `ls`, and `unlink` will not remove it.

### fork/exec: not built, and why

The other half of what was asked for is **not done**, and it is worth
being exact about why rather than shipping something that looks like it.

Wine's `start_server()` is `fork()`, then `execve(wineserver)`, then
`waitpid()`. The fork is not the hard part — the scheduler has run
multiple tasks in separate address spaces since Milestone 16, and copying
an address space is mechanical. The hard part is that **Novaris's POSIX
state is a set of globals**: one descriptor table, one mmap arena, one
brk, one signal disposition table, one socket table, all file-scope in
`kernel/posix.c` and its siblings, because until now there has only ever
been one process at a time.

Two concurrent processes would share all of it and corrupt each other
immediately — and a `fork` that quietly shared its parent's descriptor
table would pass a simple test and be wrong in exactly the way this
project has spent twenty-eight milestones avoiding.

So the real prerequisite is per-process POSIX state, keyed by address
space so that threads still share it. That is a milestone, not an
afternoon, and it is the next one:

1. Move the globals into a per-address-space structure.
2. `fork`: new address space, copy the user mappings, copy the state,
   spawn a task with the parent's frame and `eax = 0`.
3. `execve`: replace the current address space with a new image.
4. `waitpid`, and process exit status.

Nothing above is speculative — each is named by something Wine does.

## Milestone 29 — two processes at once, and a real wineserver ✅ DONE

Milestone 28 ended by naming the four things that had to exist before
Wine could start a server, and why none of them could: **Novaris's POSIX
state was a set of globals.** One descriptor table, one mmap arena, one
break, one signal disposition table, because there had only ever been one
process. A `fork` that quietly shared its parent's descriptor table would
have passed a simple test and been wrong.

So the milestone is those four things, in the order they force:

1. per-process POSIX state, keyed by address space;
2. `fork`;
3. `execve`;
4. `wait4`, and an exit status worth waiting for.

Plus the one Milestone 28 named separately — `poll`, because wineserver's
main loop waits on every descriptor it holds rather than blocking on one —
and `pipe`/`dup2`, because that is how a spawn wires a child up before it
becomes something else.

**Real Wine now starts a real wineserver on Novaris, and the two speak
the wineserver protocol to each other.**

```
novaris> setenv WINEDEBUG=+server
novaris> run wine-preloader wine hellowin.exe
wine: created the configuration directory '/.wine'
wineserver: starting (pid=258)
0020: *fd* 03a2 -> 22
0024: *fd* 7 <- 22
0024: init_first_thread( unix_pid=256, unix_tid=1, reply_fd=7, wait_fd=9 )
0024: *fd* 9 <- 23
0024: init_first_thread() = 0 { pid=0020, tid=0024, session_id=00000001, ... }
0024: open_mapping( name=L"\KernelObjects\__wine_user_shared_data" )
0024: open_mapping() = 0 { handle=0004 }
0024: get_handle_fd( handle=0004 )
0024: *fd* 0004 -> 18
0024: get_handle_fd() = 0 { type=1, cacheable=1, access=000f001f }
```

That is wineserver's own trace, from wineserver itself, running on
Novaris. Read it for what it is: two Novaris processes, one of them 4MB of
unmodified Wine, exchanging requests, replies and *file descriptors* over
a Unix socket. Milestone 27 could not start a server; Milestone 28 could
open a socket to a server that did not exist. This is the server existing.

Where it stops is recorded below, and it is a different subsystem again.

### The key is the address space

One decision does most of the work: a process's POSIX state is looked up
by *page directory* (`include/posix_proc.h`). Threads share a page
directory, so they find the same structure and share its descriptors, its
break and its signal handlers — which is what POSIX says threads do. A
forked child gets its own directory, so it finds its own — which is what
POSIX says processes do. Neither path contains a check for which it is.

What is deliberately *not* in that structure is the open file itself.
Descriptors are per process; the objects behind them are shared. That is
why fork gives the child a copy of the table and a *reference* to each
object, and why sockets grew a reference count in the same milestone —
without one, a child closing its copy of the server connection would tear
down its parent's.

### fork copies, and says so

There is no copy-on-write and no machinery to build it on: a page fault
here means a signal or a dead program, not a mapping to be filled in. So
`fork` copies every page the parent has, eagerly
(`paging_copy_user_space()`). The honest consequence is that a fork of
Wine — tens of megabytes — costs tens of megabytes, for as long as it
takes the child to exec.

Which is why a process's memory is now released the moment it exits
rather than when the batch ends. `paging_release_user_pages()` is the
other half of the same function, and `execve` uses it too: exec is
"empty this address space, put a different program in it, and rewrite the
trap frame". The frame *is* the new program's initial state, so rewriting
it and returning is the whole of starting something else.

### vfork, which is what Wine actually asks for

`start_server()` is not `fork()`. It is glibc's `posix_spawn`, which is
`clone(CLONE_VM|CLONE_VFORK|SIGCHLD)` — a child process that shares its
parent's memory, with the parent *suspended* until the child execs or
exits. The suspension is not an optimisation: the parent's next act is to
`munmap` the stack it gave the child. Novaris ran the child as a thread
and returned immediately, so the parent unmapped the ground the child was
standing on, and the child faulted at the address the stack had been at.

`CLONE_THREAD`, not `CLONE_VM`, is what separates a thread from a process
here — the obvious guess and the wrong one. Novaris copies the address
space where Linux shares it and suspends the parent where Linux does, so
the part that matters to a spawn is preserved. What is lost is written
down under "honest scope": a vfork child here reports back through its
exit status, not through memory.

### Five bugs, each found by something failing

**A program after the first got the previous one's memory.** Freshly
allocated frames were never zeroed, and neither an ELF segment's tail nor
a process's stack covers every byte of every page it touches. Linux
guarantees that memory reads as zero and a dynamic linker depends on it:
`ld-linux.so.2` loaded after any other program had run relocated itself
through whatever pointers happened to be lying there, and jumped into the
middle of the kernel. It only ever failed on the *second* program of a
session, because the first gets memory the machine zeroed at reset — which
is exactly why the test suite had never seen it. Two lines, in `elf.c` and
`process.c`.

**A blocked task was woken with a result written over its syscall
number.** `scheduler_block_current_retry()` set the "do not write eax"
flag *after* `switch_to()`, so it set it on the task being switched *to*.
The task that actually blocked came back with 0 in eax, and the
re-executed `int $0x80` dispatched to syscall 0. Milestone 28 could not
see it: the retry path only blocks when some other task can run, and
until fork there never was one.

**`recvmsg` lost the descriptor it was waiting for.** `msg_controllen` is
both an input (how much room the caller has) and an output (how much was
used), and a receive that has to wait re-executes the whole syscall.
Writing the output value before the wait told the second attempt the
caller had no room for control data at all. Wine's client connected to
wineserver, got its four bytes of greeting, and then wrote to descriptor
-1.

**Ancillary data belonged to the socket instead of to a message.** On a
real Unix socket a descriptor is attached to a point in the byte stream,
and a receive never merges the control data of two messages. Novaris
queued them on the socket, so wineserver — which receives with a 256-byte
control buffer — collected both of the client's passed descriptors in one
call, used the first and discarded the second. It then killed the client
for failing to send a descriptor it had in fact sent. Each passed
descriptor now records the stream offset it was attached at, and a read
stops at the next one.

**A bound socket did not exist in the filesystem.** Milestone 28 said so
and thought it a cosmetic simplification. It is not: Wine's client
`lstat()`s the socket path and waits for it to appear, which is how it
knows the server it just started is ready. A name that lives only in the
socket layer never appears, so the client waited, gave up, and reported
that a server seemed to be running but could not be reached. `bind()` now
creates a real node (`VFS_SOCKET`), `stat` reports `S_IFSOCK`, and
`unlink` takes the binding with it.

### And the smaller things Wine asked for

Every one found the same way — by running it and reading the `-ENOSYS`
reports. `fcntl64` (the whole of it: `F_DUPFD`, the descriptor flags, the
status flags, and locks that are granted because there is nobody to
contend with), `pwrite64`, `prlimit64`, `getrusage`, `gettimeofday`,
`clock_getres`, `clock_nanosleep`, `sched_yield` (a real yield, since
there is a scheduler to yield to), `madvise`, `lstat64`, `statfs64`,
`sysinfo`, `setpriority`, `chmod`, `sigaltstack` — real, because a handler
that runs on the faulting thread's own stack cannot do anything about a
stack overflow, and Wine's exception handler is installed `SA_ONSTACK`.

Two answered by *refusing* rather than half-building:

- **`symlink` is `-EPERM`, not `-ENOSYS`.** There are no symlinks and no
  honest way to fake one. Wine makes them inside its prefix and carries
  on when they fail; `-EPERM` says "understood and refused" rather than
  "unknown".
- **`epoll_create` is `-ENOSYS` on purpose.** wineserver asks for an
  epoll descriptor once and falls back to `poll()` for the whole of its
  main loop if it does not get one — and `poll()` is the path Novaris
  implements properly. A half-working epoll would be chosen over a
  working poll.

Three things became real that had been accepted-and-ignored:

- **A working directory per process.** `chdir` is honoured, `getcwd`
  answers it, every relative path resolves against it, and it survives
  fork and exec. `fchdir` too — wineserver opens its config directory and
  moves into it *by descriptor*, which is the safe way to do it on a real
  system and was the last thing standing between it and starting.
- **A file mode.** Recorded, not enforced — there are no users to enforce
  it against — but a program that sets a mode and reads it back sees what
  it set. wineserver creates its directory `0700` and refuses to run if
  `stat` says otherwise, because on a real machine a server directory
  other users can reach is a security hole. A fixed `0755` failed that.
- **An environment.** `setenv NAME=VALUE` in the shell, carried through
  `execve`. Wine reads more of it than most programs.

### poll, and what it honestly is

The blocking path can park a task on exactly one address, and `poll` is
by definition interested in several. A waiting `poll` registers on the
first socket in its set *and* takes a one-tick deadline: the first gives
an immediate wake for the common single-descriptor case, the second means
nothing in the set goes unnoticed for longer than 10ms. Wake-on-any-of-N
would need a wait queue per object, which is a bigger change than this
milestone.

### Verified

A ninth comparison binary, `userland/fork_test.c` → `forktest.elf`, raw
`int $0x80` like the rest, **byte-identical to the Linux host**:

```
$ make test-posix
 41 lines, 20 checks  posixtest.elf     60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf       67 lines, 45 checks  fstest.elf
 37 lines, 15 checks  pthtest.elf       34 lines, 23 checks  socktest.elf
  5 lines,  0 checks  glibc.elf          5 lines,  0 checks  dyn.elf
 51 lines, 34 checks  forktest.elf
```

34 checks across fork (the child has its own pid, its own parent, a copy
of its parent's memory that the parent does not see written), pipes and
end of stream, `dup2` and what closing one of two descriptors onto one
object does, `poll` (empty, ready, woken by another process, and hung
up), `execve` of `/proc/self/exe` with a descriptor wired on across it,
`wait4` with and without `WNOHANG`, `ECHILD`, and a working directory
that a child inherits and can move without moving its parent's.

Two rules keep that transcript deterministic on a preemptive kernel: only
the parent prints, and children report by writing into a pipe. The one
exception is the exec'd image, which prints while the parent is blocked in
`waitpid` and therefore cannot interleave with it.

Host tests 38 + 30 + 52; the smoke suite is now 14 transcript assertions,
four of them the new one. `meminfo` grew a double-free counter — zero, and
written while chasing a bug that turned out to be something else, but a
frame freed twice is the one error this allocator cannot survive quietly
and it is worth being able to ask.

### Honest scope

- [x] Per-process POSIX state: descriptors, arenas, break, signal
      dispositions, working directory, exe path — keyed by address space,
      so threads share and processes do not.
- [x] `fork`, `vfork`, and the `clone` shapes that are really fork.
- [x] `execve`, including `/proc/self/exe`, argv and envp, descriptors
      that survive and `O_CLOEXEC` descriptors that do not.
- [x] `wait4`/`waitpid` with Linux's status encoding, `WNOHANG`,
      `ECHILD`; `kill` across processes.
- [x] `pipe`/`pipe2`, `dup`/`dup2`/`dup3`, `poll`/`ppoll`,
      `select`/`pselect6`.
- [x] **Real Wine starts a real wineserver**, which creates the Wine
      prefix, writes `system.reg`/`user.reg`/`userdef.reg`, and completes
      `init_first_thread` and `get_handle_fd` with the client.
- [ ] **No Windows program has been run.** Wine stops at
      `virtual_map_user_shared_data`, which needs `MAP_SHARED` — two
      processes mapping the *same physical frames* of a file. Novaris's
      `mmap` allocates a private copy and refuses `MAP_SHARED` rather
      than pretending, so this is a missing subsystem (a page cache, or
      at least shared file mappings), not a missing syscall. It is the
      next milestone.
- [ ] Wine's PE builtins (`ntdll.dll`, `kernel32.dll`, …) are not in the
      initrd either, so even a working shared mapping would meet that
      next. The `wine-initrd` target carries the loader, `ntdll.so`,
      `wineserver` and six NLS tables; the PE side is untouched.
- [ ] `fork` copies eagerly. No copy-on-write, and no fault machinery to
      build it on.
- [ ] A vfork child's memory is copied, not shared, so it can report back
      through its exit status but not through memory. `posix_spawn`
      reporting an exec failure is the case that costs — the child exits
      127 and the parent reads that instead of an errno.
- [ ] `poll` wakes on one descriptor plus a 10ms tick rather than on any
      of N.
- [ ] No `epoll`, `userfaultfd`, `clone3`, `statx`, or `sigreturn` (the
      legacy non-`rt_` one, which a handler installed without
      `SA_SIGINFO` would need).
- [ ] 8 processes, 16 tasks, 32 descriptors each, 32 sockets — all fixed,
      all small, all deliberate.
- [ ] No sessions, process groups, users or permissions. `setsid`,
      `setpgid` and `umask` are answered rather than implemented, and the
      mode bits are recorded rather than enforced.

### How to reproduce the Wine run

Wine is still **not vendored**, for the reason Milestone 27 gave. What
changed is what the `wine-initrd` target carries: `wineserver` and the
NLS tables it will not start without, and the loader under the names
Wine's own `init_paths()` derives (`wine`, `wine-preloader`) as well as
the documented ones.

```bash
git clone --depth 1 -b stable https://github.com/wine-mirror/wine
cd wine
CC="gcc -m32" ./configure --enable-archs=i386 --disable-tests \
    --without-x --without-freetype --without-vulkan --without-opengl ...
make -j4
cd ../NovarisOS && make WINE_BUILD=../wine wine-initrd
qemu-system-i386 -cdrom novaris-wine.iso
novaris> setenv WINEDEBUG=+server
novaris> run wine-preloader wine hellowin.exe
```


## Milestone 30 — a Windows program runs under real Wine ✅ DONE

```
novaris> run wine-preloader wine hellowin.exe
Hello from a real Windows .exe running on Novaris!
  compiled by mingw-w64, linked against msvcrt.dll

integers:   42 -7 4000000000 00042 42    | +42
hex/octal:  beef BEEF 0xbeef 10
strings:    [abc] [       abc] [abc       ] [abc]
floats:     3.141593 2.50 1.234568e+04 0.0001
64-bit:     1234567890123
heap:       "malloc + strcpy + strlen" (24 bytes)
fib(20):    6765
argc:       1, argv[0]: C:\windows\hellowin.exe

Exiting with code 0.
```

That is `userland/pe_test/hello_win.c`, built by mingw-w64 against the
real msvcrt import library - the same binary Milestone 10 runs against
Novaris's hand-written Win32 - running instead on **Wine 11.0**, on
Novaris, through Wine's own PE ntdll, kernel32, kernelbase and msvcrt,
with a wineserver behind it. `make test-wine` asserts seven lines of it.

The whole of Path A, end to end: an unmodified Unix program that
implements Windows, cross-compiled against this kernel's syscall surface,
loading unmodified Windows PE modules and running an unmodified Windows
executable.

### What Milestone 29 was stopped by, and what it actually took

Milestone 29 named the blocker precisely and got the size of it wrong.
`MAP_SHARED` was an afternoon; the six things behind it were the
milestone.

**MAP_SHARED, and why there is no page cache.** Two processes mapping one
file have to see the same memory - it is how wineserver publishes the
Windows `KUSER_SHARED_DATA` page to every client. The implementation is
not a cache: a mappable file's bytes are moved into *page-aligned*
storage, so page N of the file is exactly one physical frame, and the
mapping hands that frame to user space. The file's contents and the
mapping are then the same memory, and there is nothing to keep coherent.

What it cost is one page-table bit. `PAGE_SHARED` means "mapped here, but
not this address space's to free", and every teardown path checks it -
munmap, process exit, execve, and the destruction of an address space.
fork checks it too, and *shares* rather than copies, which is what makes
a shared mapping shared across a fork.

**PROT_NONE was a mapping, not a reservation.** Novaris answered "reserve
this address range and give me nothing" by allocating a frame per page
and marking it unreadable: the same observable behaviour and a completely
different cost. Wine's preloader asks for two gigabytes of it before it
does anything else. On a 512MB machine the reservation *succeeded*, ate
every frame there was, and the loader stopped on the next allocation. A
reservation is now a not-present entry with `PAGE_RESERVED` set - a page
table per 4MB, two megabytes for Wine's two gigabytes - and `mprotect`
turns it into memory when the program comes back for it.

**And `mprotect(PROT_NONE)` has to keep the page.** The first version
freed the frame and allocated a zeroed one on the way back, which passes
any test that does not look. Wine's heap marks a block inaccessible and
reuses it, so what it looked like from outside was Wine reading its own
heap headers out of a page of zeroes. PROT_NONE is a *permission* here:
the frame stays and `PAGE_USER` goes, so ring 3 faults and the bytes
survive.

**`MAP_FIXED_NOREPLACE` was ignored.** "This address or nothing" is how a
loader asks for a Windows image's base address, and reading it as
"anywhere will do" produced an arbitrary address and the error
`out of memory for 0x400000` - a message about the one thing that was not
the problem.

**0x400000 was the initrd's.** GRUB puts modules just above the kernel,
Novaris identity-mapped them there, and every Windows executable ever
built wants to load at 0x400000. The initrd never needed to be at its
physical address - only to be reachable - so it moved to the kernel half
at `INITRD_VIRTUAL_BASE`, and the classic image range became the
program's.

**Linux gives a thread three TLS descriptors; Novaris gave one.** That
was enough while glibc was the only thing asking. Wine asks too: its i386
code reaches the Windows TEB through `fs` the way glibc reaches its TLS
through `gs`, and it gets that segment by calling `set_thread_area()`.
With one descriptor to go round, Wine's request silently overwrote
glibc's, and the next glibc function to touch `errno` read it out of the
middle of a TEB. What that looked like was Wine dereferencing a null
pointer inside its own SIGSEGV handler.

**A file has to outlive its name.** Open a file, unlink it, keep using
the descriptor: that is how anonymous shared memory is made on Unix and
exactly what wineserver does. Novaris freed the node on unlink, so an
open descriptor and a live mapping pointed at reused heap. VFS nodes are
reference counted now, and a shared mapping holds a reference too.

### And the ones only findable by running it

**`sortdefault.nls` was not in the initrd**, and kernelbase's
`init_locale` walks it without checking that it got it. With the table
missing it parsed whatever pointer was left over, read a count of zero,
computed "the last entry of nothing", and took an access violation
twenty-four bytes *before* the mapping. Four instructions of
disassembly - `mov (%eax),%edx; lea (%edx,%edx,2),%ecx; shl $3,%ecx;
lea -0x18(%eax,%ecx,1),%edi` - identified it, because a count of zero is
the only input that produces that address. The initrd now carries the
whole `nls` directory: a missing table does not announce itself.

**The legacy `sigreturn` frame is two arguments up, not three.** glibc's
non-`SA_SIGINFO` restorer pops the signal number before the syscall.
Novaris only implemented `rt_sigreturn`, so a handler installed without
`SA_SIGINFO` could not return from itself at all.

**wineserver ran out of descriptors at 32.** It holds a listening socket,
a connection, and a request, reply and wait pipe per client, plus an open
descriptor for every file any client has mapped. It reported
`STATUS_TOO_MANY_OPENED_FILES` while opening ntdll.dll and the client
turned that into "invalid image format" - a true statement about the
wrong thing. The table is 128 now.

**A faulting thread took the kernel with it.** The fault path cleared the
"ring 3 is running" flag and terminated one task; the *other* threads
kept running, and the next one to fault fell straight past the hook into
the panic handler. An unhandled fault now ends the whole thread group -
which is what Linux does - and leaves the other processes of the batch
alone, because when Wine's client crashes wineserver is still a running
program.

**And the console had to learn to read.** Novaris's console is a
framebuffer and a font. Wine's conhost draws a Windows console by
positioning a VT cursor: every space is "erase to end of line, cursor
forward one", every character is bracketed by hide-cursor/show-cursor. So
the first Windows program to run here produced perfectly correct text
with an escape sequence between each letter of it. `kernel/console.c` now
reads the handful of sequences a console actually uses, and swallows
anything else rather than printing it.

Plus the syscalls, each found by an `-ENOSYS` report: `fcntl64` in full,
`pwrite64`, `prlimit64`, `getrusage`, `gettimeofday`, `clock_getres`,
`clock_nanosleep` (and the 64-bit-time forms), `sched_yield`,
`sched_getaffinity`, `madvise`, `lstat64`, `statfs64`, `sysinfo`,
`setpriority`, `chmod`, `ftruncate`, `truncate`, `fchdir` (real, by
rebuilding the path from the tree - wineserver moves into its config
directory by descriptor), and `sigaltstack` (real, because a handler on
the faulting thread's own stack cannot do anything about a stack
overflow).

Two answered by refusing rather than half-building: `symlink` is `-EPERM`
rather than `-ENOSYS`, because the operation is understood and refused
rather than unknown; and `epoll_create` is refused *on purpose*, because
wineserver falls back to `poll()` for its whole main loop if it does not
get an epoll descriptor, and `poll()` is the path Novaris implements
properly. A half-working epoll would have been chosen over a working
poll.

### Verified

A tenth comparison binary, `userland/mmap_test.c` -> `mmaptest.elf`, raw
`int $0x80` like the rest, **byte-identical to the Linux host**:

```
$ make test-posix
 41 lines, 20 checks  posixtest.elf     60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf       67 lines, 45 checks  fstest.elf
 37 lines, 15 checks  pthtest.elf       34 lines, 23 checks  socktest.elf
 51 lines, 34 checks  forktest.elf      39 lines, 24 checks  mmaptest.elf
  5 lines,  0 checks  glibc.elf          5 lines,  0 checks  dyn.elf
```

24 checks across a shared mapping seen through `read()`, two mappings of
one file seeing each other, a shared mapping surviving a fork, a private
mapping staying private, a megabyte reserved with `PROT_NONE`,
`MAP_FIXED_NOREPLACE` refusing a reserved address and `MAP_FIXED` taking
it, `mprotect` round-tripping through `PROT_NONE` without losing data,
and a file read through a descriptor after its name is gone.

And `make test-wine`, which boots `novaris-wine.iso` and asserts seven
lines of a real Windows program's output.

Host tests 38 + 30 + 52, smoke suite 17 assertions, all ten comparison
binaries identical.

### Honest scope

- [x] **A real Windows `.exe` runs to completion under real Wine on
      Novaris** - console output, printf with floats and 64-bit integers,
      the heap, argv, and the exit code.
- [x] `qsorttest.exe` too: `qsort`, `bsearch` and nested callbacks,
      through Wine rather than through Novaris's own Win32 layer.
- [x] `MAP_SHARED`, `PROT_NONE` reservations, `MAP_FIXED_NOREPLACE`,
      three TLS descriptors, reference-counted files, and a console that
      reads ANSI.
- [ ] **Threaded Windows programs do not work.** `threads.exe` starts,
      creates a worker, and the worker faults reading its thread block -
      a new thread's `fs` is not set up the way Wine expects before it
      runs. It fails *cleanly* - the process dies, the shell comes back,
      every frame is reclaimed - but it fails.
- [ ] **No GUI.** `winex11` is not built and there is no display backend,
      so a windowed program has nowhere to draw. Novaris has a window
      manager of its own and connecting the two is a milestone.
- [ ] **No networking**, so `ws2_32.dll` fails to initialise and anything
      needing it will not start. `wineboot` does not, which is why the
      prefix has no drive mapping and Wine reports "could not find DOS
      drive for the current working directory" and starts in the Windows
      directory instead.
- [ ] `fork` still copies eagerly, `poll` still wakes on one descriptor
      plus a tick, and a vfork child's memory is copied rather than
      shared.
- [ ] The Wine ISO needs 768MB of guest RAM and carries a 40MB initrd
      read into memory whole. That wants a real filesystem on a real
      disk, and there is not one.

### How to reproduce it

Wine is still **not vendored**. What `make wine-initrd` carries has grown:
the loader and `ntdll.so` as before, plus `wineserver`, `win32u.so`, the
whole `nls` directory, and the PE builtins a console program needs -
`WINE_PE_DLLS` and `WINE_PE_PROGS` in the Makefile name them, so what
ships is a decision rather than whatever happened to be built.

```bash
git clone --depth 1 -b stable https://github.com/wine-mirror/wine
cd wine
CC="gcc -m32" ./configure --enable-archs=i386 --disable-tests \
    --without-x --without-freetype --without-vulkan --without-opengl ...
make -j4
cd ../NovarisOS && make WINE_BUILD=../wine wine-initrd test-wine
```

## Milestone 31 — threaded Windows programs, under real Wine ✅ DONE

```
novaris> run wine-preloader wine threads.exe
Win32 threads test - real CreateThread on Novaris

main thread GetCurrentThreadId() = 36

created worker 1, thread id 56
created worker 2, thread id 60
created worker 3, thread id 64

  worker 1 starting, GetCurrentThreadId() = 56
  worker 2 starting, GetCurrentThreadId() = 60
  worker 3 starting, GetCurrentThreadId() = 64
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
```

That is `userland/pe_test/threads.c`, the Milestone 17 binary — real
`CreateThread`, `WaitForSingleObject`, `InterlockedIncrement` and a
`CRITICAL_SECTION` — running through Wine's own kernel32 and ntdll rather
than Novaris's hand-written Win32 layer. Every Win32 thread is a real
pthread on a real `clone()`. `make test-wine-threads` asserts eleven
lines of it.

The two counters are the whole point. The interlocked one proves the
workers all ran and every increment landed; the guarded one is
incremented with a deliberately racy read-modify-write, so it only comes
out at 60 if the critical section really serialised three threads.

### What Milestone 30 named, and what it actually was

Milestone 30 said it precisely: "a new thread's `fs` is not set up the
way Wine expects before it runs". That was one of four things, and the
other three were bigger.

**A thread inherits its creator's segmentation, and inherited none of
it.** Linux copies the parent's three TLS descriptors into the child in
`copy_thread()` and only *then* applies `CLONE_SETTLS`. glibc cannot tell
the difference — it sets its own descriptor before the new thread runs a
line of its own code — and Wine can: its i386 ntdll allocates **one**
descriptor for the whole process in `signal_init_threading()`, and every
thread reaches its TEB through `fs` on that entry before ntdll gets as
far as calling `set_thread_area()` for itself. With the entry's base left
at zero in the child, that is a read through a null pointer, in the
worker, which is exactly what was seen and could not be explained.

So a POSIX thread's initial frame is a copy of its creator's now, the way
`fork`'s already was — which is what carries the segment registers across
— and all three TLS descriptors are inherited before `CLONE_SETTLS`
overwrites the one it names. *Which* one it names comes from the
`user_desc`'s `entry_number` rather than being assumed to be glibc's; a
process where Wine allocated first has glibc on another. And the
scheduler programs all three descriptors on every switch, including the
ones a task has not set — only reprogramming the ones in use left the
others holding the outgoing task's bases, so a stale selector addressed
another thread's TLS block instead of faulting, which is the one failure
mode that looks like working code.

**`libgcc_s.so.1` is a hard requirement for a thread to end.** glibc
unwinds a thread out of itself with `_Unwind_ForcedUnwind`, which it
reaches by `dlopen()`ing a library nothing links against. Without it,
every Wine worker printed `libgcc_s.so.1 must be installed for
pthread_exit to work` and aborted. It is copied from the host toolchain
into the Wine initrd now, like `ld-linux.so.2` and `libc.so.6` and for
the same reasons.

**"Nothing else can run" is a fact about a moment, not about a timeout.**
A futex wait that could not park — because every sibling was blocked —
idled in the kernel until its own deadline, holding the CPU across every
tick that expired somebody else's wait. So a Wine thread waiting on a
critical section held by a thread whose own timed wait had just expired
starved it for the whole five seconds. It re-offers the CPU on every tick
now, and parks properly the moment there is anyone to hand it to.
`posix_syscall()`'s retry path had the identical loop and the identical
fix, and `nanosleep` had the same shape — a Windows program calling
`Sleep()` suspended every other thread of itself for the duration — so it
parks too, on a remembered absolute deadline the way `poll()` already
did.

**And `poll` registered on one descriptor.** Milestone 28 parked on the
*first* socket in the set plus a one-tick deadline and called the
compromise openly. This is the bill: a wineserver round trip is one poll
wake, and the socket carrying it is not the first in wineserver's set —
the listening socket is. So every round trip cost 10ms, and Wine's
console makes one per escape sequence while drawing a Windows console. A
worker thread spent five seconds inside a single `printf`, holding
msvcrt's lock, and every other thread of the program timed out waiting
for it. "Threads work but are slow" was one wake latency wearing a
disguise. A task can be parked on a *set* of addresses now, so a poll is
woken by whichever descriptor moved.

### The builtins that were built and not shipped

Milestone 30 listed "no networking, so `ws2_32.dll` fails to initialise"
as a limitation of the machine. It was not. A Wine builtin is two halves
— a PE `.dll` the Windows program links against, and a Unix `.so` it
reaches through `__wine_init_unix_call()` — and only the first half was
being copied into the initrd. ws2_32's process attach *is* that call and
nothing else, so it failed, and ntdll's loader turns a failed process
attach into "Initializing dlls for wineboot.exe failed". The diagnosis
had been "a subsystem is missing"; the fix is three lines of Makefile.

With its Unix half present, wineboot got past ws2_32 and into the next
things shipped by halves: shell32 needs shlwapi needs shcore,
services.exe needs userenv, ole32 needs coml2, and shell32 delay-loads
wininet (which needs mpr) to make the browser cache folders. Each missing
one was not "that feature is unavailable" but an abort into a debugger
that is not there. wineboot now runs to the end of what it can do without
a prefix.

### Four bugs the busier Wine then found

**A signal aimed at another process's thread was delivered to the
sender.** `tgkill`'s target is a thread, and Milestone 30 read that as
"threads share this process's structure, so this is always the local
path" — true of a sibling, false of a thread somewhere else. wineserver
suspends a client's threads by tid, so the signal landed on wineserver,
which has no `SIGUSR1` handler, and killed the one process every client
is waiting on. The whole batch then hung. A tid is a *task* id, so it
resolves through the scheduler to the address space it runs in and from
there to the process that owns it; `kill()` falls back to the same lookup
rather than reporting `ESRCH` for a thread that exists.

**A futex wake matched the address in every process.** A futex word is a
*virtual* address, and two processes started the same way get the same
mmap arena and hand out the same addresses — so a `FUTEX_WAKE` could wake
an unrelated task in another process and leave the thread it was meant
for to time out. Invisible while a Wine batch was three processes; a
handful of times a run once wineboot reached services.exe. Wakes that
name a user address are scoped to the caller's address space now; wakes
that name a kernel object still are not, because their waiters are
deliberately elsewhere.

**Draining a socket did not wake anyone waiting for it to be writable.**
Harmless for as long as every poll also re-tested everything on a
one-tick timer, and a hang the moment poll was really woken by its
descriptors. The backstop stays for that reason, at half a second rather
than 10ms: the registration is only exhaustive if every readiness
transition has a wake behind it, and one did not.

**`ioctl` answered 0 to everything and wrote nothing.** A successful
ioctl that fills in no answer is worse than a refused one — it tells the
caller "here is your answer" and leaves it reading its own uninitialised
stack. Wine asks for the terminal size at startup and passes it to
conhost, so conhost was told to draw a console `--width 0`. `TIOCGWINSZ`
is real now and reports the console's actual character grid; everything
else says `ENOTTY`.

Plus process tables sized for a Wine batch of three. A wineboot that gets
as far as services.exe needs more, and running out was silent from
outside: fork returned `-EAGAIN`, Wine reported nothing, and the Windows
program simply never ran. 24 processes, 32 tasks.

### Symbolic links: built, working, and deliberately not enabled

They were built and they work. A VFS node whose contents are a path;
resolution that walks *through* a link in the middle of a path and stops
at one on the end when the caller means `lstat`; real `readlink`; `ELOOP`
rather than `ENOENT` for a link that points at itself, because a program
testing for a file's absence must not read "could not tell" as "not
there"; `unlink` that removes the link and never the target. Twenty-four
checks in `fstest.elf`, byte-identical to Linux on the first run.

They are not enabled, and the reason is what they unlock rather than
anything wrong with them.

Wine's prefix has no DOS drives without symlinks — `dosdevices/c:` and
`dosdevices/z:` **are** symlinks, made by two lines of
`create_config_dir()` in its ntdll — and that missing drive table is the
whole of the "could not find DOS drive for the current working directory"
message Milestone 30 reported. With symlinks, that message goes away,
Wine resolves `Z:\hellowin.exe` properly instead of falling back to the
Windows directory, and it then does what a real installation does next:
allocates a real Windows console by starting `conhost.exe`.

conhost is where it stops. The Windows program loads, its three DLLs
load, `init_console creating unix console (size 155 43)` is the last
thing it says, and nothing prints. A working conhost needs the prefix
`wine.inf` builds, and `wine.inf` needs a filesystem that is not 40MB of
initrd read into RAM whole. That is the next milestone, and it should be
entered deliberately.

So `symlink()` still answers `-EPERM` — understood and refused, which is
what Milestone 30 chose it for. Turning it on would trade a Windows
program that runs for one that does not.

*(Milestone 32 turned it on, and the trade turned out to be exactly the
one described here: with drives Wine reaches the prefix, the server, the
services and its builtins on the disk, and then stops in shell32's
imports. It is a switch now - `symlinks on|off` in the shell - and the
Wine transcript tests ask for `off` explicitly. See Milestone 32.)*

### Verified

```
$ make test-wine-threads
All 11 transcript assertion(s) passed.

$ make test-wine
All 7 transcript assertion(s) passed.

$ make test-posix
 41 lines, 20 checks  posixtest.elf     60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf       67 lines, 45 checks  fstest.elf
 50 lines, 25 checks  pthtest.elf       34 lines, 23 checks  socktest.elf
 51 lines, 34 checks  forktest.elf      39 lines, 24 checks  mmaptest.elf
  5 lines,  0 checks  glibc.elf          5 lines,  0 checks  dyn.elf
```

`pthtest.elf` grew a section that fails without the kernel change and
passes with it, and it is the interesting kind of test: a *second* TLS
descriptor behind `fs`, a thread cloned with `CLONE_SETTLS` naming only
`gs`, and the worker reading `fs` before touching it. Everything above it
in that file uses one segment, so none of it could see the difference
between "the child got its creator's descriptors" and "the child got one
descriptor and two zeroes".

Host tests 38 + 30 + 52, smoke suite 17 assertions, all ten comparison
binaries byte-identical to Linux.

### Honest scope

- [x] **A threaded Windows `.exe` runs to completion under real Wine on
      Novaris** — three worker threads, a real join, correct exit codes,
      and a critical section that really serialises.
- [x] Thread-local storage inherited the way Linux inherits it, and the
      descriptor `CLONE_SETTLS` names rather than the one glibc happens
      to use.
- [x] A blocked thread no longer starves the thread it is waiting for,
      in the futex path, the syscall retry path or `nanosleep`.
- [x] `poll` woken by the descriptor that moved rather than by a timer.
- [x] Signals aimed at another process's thread reach that process.
- [x] Wine's builtins ship with both halves, so wineboot runs to the end
      of what it can do without a prefix.
- [ ] **One lock wait per run still times out.** Wine prints
      `err:sync:RtlpWaitForCriticalSection ... wait timed out`, waits its
      five seconds, retries and succeeds. It was three a run before this
      milestone and none once the scheduler stopped starving threads —
      and then wineboot started getting far enough to launch
      services.exe, the machine got busier, and one came back. The
      counters prove it is a latency defect and not a correctness one,
      which is why `make test-wine-threads` asserts the counters and does
      not assert its absence.
- [ ] **No Wine prefix.** `wineboot` cannot run `wine.inf`, so there is
      no `C:\windows`, no registry beyond what wineserver creates, and no
      real console. Symlinks are built and would give it the drives; the
      rest needs a real filesystem on a real disk.
- [ ] **No GUI**, **no networking**, and `fork` still copies eagerly —
      unchanged from Milestone 30.

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
   same picture, Milestone 25 made futexes really block, and Milestone 26
   made the filesystem writable and hierarchical. This item is now
   substantially done; what is left will be found by step 5 rather than
   guessed at.
4. **A dynamic linker** — ✅ done in Milestone 22. `ld-linux.so.2` loads
   `libc.so.6` at runtime and a dynamically linked glibc program runs,
   with output identical to Linux. The kernel's half is ET_DYN images,
   `PT_INTERP`, `AT_BASE`, file-backed `mmap` and the file syscalls; the
   linking itself is the interpreter's job.
5. **Pull in real Wine source**, cross-compile it against that surface,
   and find out what is still missing. — ✅ **done**. Milestone 30 runs a
   real Windows `.exe` under real Wine on Novaris. The history below is
   worth keeping, because each step found the *next* missing subsystem
   rather than the one that looked obvious. Started in Milestone 27, and
   it has done what it was for three times over. Wine 11.0 builds against
   this kernel; Milestone 27 got it as far as `wine --version` and found
   that what stopped it was not a missing syscall but a missing
   *subsystem* — Unix domain sockets, because Wine is a client/server
   system and every handle lives in wineserver. A GDI display backend was
   the guess; IPC was the answer. Milestone 28 built the sockets.
   Milestone 29 built the other half — per-process state, fork, execve,
   waitpid, poll — and **real Wine now starts a real wineserver**, which
   creates the Wine prefix and registry and completes the protocol
   handshake with the client. The next thing that stops it is a
   subsystem again, and again not the one that looked obvious:
   `MAP_SHARED`. Two processes have to be able to map the same physical
   frames of a file, and this kernel has no page cache. Milestone 30
   built that and ran a Windows program; Milestone 31 made a *threaded*
   one work, and found that the last thing between Novaris and a real
   Wine installation is a filesystem — `wine.inf` builds the prefix, and
   forty megabytes of initrd read into RAM is not somewhere to build
   one. Milestone 32 built the filesystem: an ATA disk, FAT32 from
   scratch, and the symbolic links Wine's DOS drive table is made of.
   With them Wine stops falling back and takes its real startup path -
   it finds a prefix on the disk, names it by its DOS path, starts
   wineserver and services.exe, and loads its builtins off the disk -
   and stops in shell32's imports. Which is, once again, a subsystem
   rather than a syscall, and once again not the one that looked
   obvious.

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

## Milestone 32 — a real filesystem on a real disk ✅ DONE

Named by Milestone 31 rather than guessed at, which is how every step of
Path A has gone. Everything left on the Wine list ran through it: the
prefix `wineboot` builds, the symlinks that give Wine its DOS drives, and
a 40MB initrd read into RAM whole.

There is now an ATA PIO driver, a FAT32 filesystem written from scratch,
a VFS that can have more than one filesystem in it, and symbolic links —
switched on, compared against Linux byte for byte, and stored on the disk
in a form that survives a reboot.

### What was built

**An ATA PIO driver** (`kernel/ata.c`, `include/ata.h`). Polling, not
DMA; both legacy IDE buses, master and slave; LBA28, so 128GB is the
ceiling and the images here are measured in hundreds of megabytes. The
two things that actually bite are in the code with the reasons attached:
the 400ns settle after a drive select, without which the status register
answers for the *previous* drive, and FLUSH CACHE after a write, without
which the image looks right in RAM and wrong after a reboot — which is
precisely the property this milestone exists to provide.

**A block layer** (`kernel/blockdev.c`, `include/blockdev.h`). Four
functions and a vtable, and it is there for one concrete reason: with a
device behind an interface, `tests/fat32_host_test.c` hands the *same*
FAT32 driver a device backed by a file on the host. A mis-set cluster is
a file that reads as somebody else's bytes three operations later, and
from inside a VM the serial log says nothing at all.

**FAT32** (`kernel/fat32.c`, `include/fat32.h`). Reading and writing
files of any size the volume holds, growing and truncating them, creating
and removing files and directories, renaming within the volume, long
filenames in both directions, MBR partition tables and bare volumes, and
the FSInfo block. Chosen for the reason Milestone 6 first considered it:
it is the only filesystem both simple enough to write from scratch and
universally readable, so the image can be mounted on the host and looked
at — and `mdir` reading back what this driver wrote is a kind of evidence
that no amount of self-consistency testing provides.

Two decisions are worth stating, because they are what the milestone is
actually about:

- **Nodes are cached, bytes are not.** The first time anything looks
  inside a directory, its entries are read off the disk and turned into
  VFS nodes, which then stay. File contents are never cached — a read
  goes to the disk every time — unless something has to see the whole
  file at once, which is `mmap` and `exec`. So the cost of a filesystem
  became proportional to what is *used* rather than to what exists. A
  Wine prefix of 1,117 files costs about 280KB of nodes where the initrd
  charged forty megabytes for every file whether or not it was opened.

- **Metadata is written through, immediately.** No journal, no delayed
  allocation. A create writes the directory entry before it returns; a
  write updates the size in the directory entry before it returns. The
  only thing held back is the FAT sector the allocator is working in,
  and `sync` pushes that out. Slower than a real driver, and it means a
  machine that stops mid-write loses at most the one operation in
  flight — which for a kernel with no fsck is the right trade.

**A VFS that can hold two filesystems** (`include/vfs.h`,
`kernel/ramfs.c`). `vfs_write`, `vfs_create`, `vfs_unlink`,
`vfs_truncate` and `vfs_rename` used to *be* the ramfs implementations;
they now dispatch on the node's `ops`, and a node with none is a ramfs
node — so every existing caller and every existing node behaves exactly
as it did. Path resolution stopped walking `first_child` directly and
started going through the finddir hook, which is not tidiness: a FAT
directory's children do not exist as nodes until something asks, and the
hook is where "read this directory off the disk" happens.

The node table moved off a fixed 384-entry array onto the heap, in
blocks. Milestone 26 chose a fixed ceiling for the reason the process
table has one, and that was right until a volume with thousands of files
turned "the initrd is too small" into "the node table is too small" — the
same wall, one milestone later.

**Symbolic links**, at last. Milestone 31 built them and took them back
out; they are in, and `symlink()` answers. FAT has no link type, so a
link is stored the way every other system that has had to put one on a
DOS filesystem stores it: a small file with the SYSTEM attribute whose
contents are `NVSYMLNK` followed by the target. Nothing else on the
volume sets SYSTEM, so the check costs one sector read for a handful of
files, and a link written here reads back elsewhere as an ordinary small
file rather than as corruption.

**Tooling.** `tools/mkfat32.py` builds a FAT32 image from a directory
tree — reproducible, symlink-aware, and a completely separate
implementation of the format from the driver's. That separation is the
point: the writer and the reader agreeing is evidence; a driver reading
back its own output is not.

### What it cost elsewhere, and why

Four limits moved, and every one of them moved because something failed
rather than because a number looked small:

- `VFS_NAME_MAX` 64 → 128 and `POSIX_PATH_MAX` 128 → 256. A Wine
  prefix's winsxs assembly directories have 91-character names, and a
  name too long for a node is a file reachable only by its 8.3 short
  name.
- `MAX_SOCKETS` 64 → 256. With a prefix on the disk, wineboot gets far
  enough to start `services.exe`, which starts a process per service,
  each of which is a server connection plus two pipes with the other end
  held by the server. Sixty-four ran out four services in, and every
  service after that failed with "pipe: Too many open files in system".
- `POSIX_MAX_PROCS` 24 → 40, for the same reason.
- `KHEAP_MAX_SIZE` 48MB → 192MB. There is no page cache, so a mapped
  file's bytes *are* heap. `shell32.dll` alone is nine megabytes and
  wineboot maps dozens; at 48MB the heap ran out part way through and
  Wine reported it as "Loading library gdi32.dll failed", which is what
  a failed `mmap` looks like from inside a PE loader. The ceiling is now
  the address space rather than a guess.

`getxattr` and its five relatives are answered with `-EOPNOTSUPP` rather
than reported as unimplemented. Wine looks for `user.DOSATTRIB` on every
file it opens, so on a filesystem without extended attributes this is not
an unusual call, it is one per file — and `-EOPNOTSUPP` is the specific
answer Linux gives for a filesystem mounted without them, which is what
Wine checks for before falling back.

### Verified

```
$ make test
=== printf/dtoa engine ===   38 check(s), 0 failure(s)
=== PE loader ===            30 check(s), 0 failure(s)
=== FAT32 driver ===         64 check(s), 0 failure(s)
    fsck.vfat on the volume the driver wrote:
    build/test/fat32-work.img: 75 files, 479/129022 clusters   (clean)
=== window manager ===       52 checks, 0 failures

$ make test-qemu-disk
All 7 transcript assertion(s) passed.   (write, reboot, read back)
$ fsck.vfat -n build/persist.img
build/persist.img: 4 files, 8/129022 clusters                  (clean)

$ make test-wine-prefix     All 6 transcript assertion(s) passed.

$ make test-posix
 41 lines, 20 checks  posixtest.elf     60 lines, 45 checks  uctest.elf
 93 lines, 67 checks  sigtest.elf       82 lines, 58 checks  fstest.elf
 50 lines, 25 checks  pthtest.elf       34 lines, 23 checks  socktest.elf
 51 lines, 34 checks  forktest.elf      39 lines, 24 checks  mmaptest.elf
  5 lines,  0 checks  glibc.elf          5 lines,  0 checks  dyn.elf

$ make test-qemu            All 17 transcript assertion(s) passed.
$ make test-wine            All 7 transcript assertion(s) passed.
$ make test-wine-threads    All 11 transcript assertion(s) passed.
```

`fstest.elf` grew from 45 checks to 58, and the thirteen new ones are the
symbolic-link section - all of them compared line for line against what
the same binary printed on Linux.

`fsck.vfat` is the strongest single line in that list, and it is in `make
test` for that reason. It has no idea Novaris exists; it walks every
cluster chain, every directory entry and every long-name checksum on a
volume this driver has written, truncated, renamed, grown past a cluster
boundary and filled to capacity - and on one the *kernel* wrote inside
QEMU. It also found a real defect the driver's own tests could not:
`fat_write()` updated a file's directory entry before flushing the FAT
sector holding its new clusters, so a machine stopping in between left an
entry pointing at a cluster the table still called free. The two are now
ordered the other way round, where the worst case is a chain nothing
points at - space lost until a check, rather than data lost.

The FAT32 host test is the substantial one. It mounts an image built by
`mkfat32.py`, reads what the *other* implementation wrote, then writes,
truncates, grows a directory past its first cluster, renames across
directories, unlinks a file whose name used long-filename entries, and
unmounts and remounts to check every one of those survived. The last
section fills the volume and then checks that an ordinary small file
still works — which is a test of the rollback in `ensure_chain()`, and
that rollback exists because without it the first program to ask for too
much left clusters allocated that no directory entry mentioned, and the
volume stayed full for ever.

`fstest.elf` gained a symbolic-link section that runs on both Linux and
Novaris and is compared line by line: create a link with a relative
target, `lstat` it and find `S_IFLNK`, `stat` it and find `S_IFREG`, read
the target back, open straight through it, walk a path *through* a link
in a non-final position, and get `EEXIST` and `EINVAL` in the two places
Linux gives them. Those are exactly the operations Wine performs on the
two links its drive table is made of.

Host tests 38 + 30 + 64 + 52, and the whole existing suite unchanged.

### Where Wine actually stands now, which is the honest part

With a disk attached, `$HOME` is `/disk`, so Wine's prefix lands on real
storage. With `symlinks off` — which is what `make test-wine` and
`make test-wine-threads` now ask for explicitly — everything Milestone 31
could do, it still does:

```
$ make test-wine            All 7 transcript assertion(s) passed.
$ make test-wine-threads    All 11 transcript assertion(s) passed.
```

With symlinks **on**, which is the kernel's default because they are
correct and because `fstest.elf` compares them against Linux, Wine stops
falling back and does what it does on a real machine. A disk built with
`make WINE_PREFIX_DIR=... wine-disk` gets it much further than any
milestone before:

- The DOS drive table exists. Wine names the prefix `Z:\disk\.wine`
  instead of reporting that it cannot find a DOS drive for the working
  directory — which is what Milestones 30 and 31 both got, and what
  Milestone 30's write-up identified as the missing piece.
- `wineserver` starts, the handshake completes, `wineboot` runs, and
  `services.exe` starts and works through the auto-start service list,
  starting a process per service.
- Wine's PE builtins load **off the disk**. `sum` on
  `/disk/.wine/drive_c/windows/system32/gdi32.dll` inside Novaris gives
  `fnv1a 0x966bebe6` over 514,062 bytes, which is byte for byte what the
  host computes for the same file. The filesystem is not the problem.

And then `wineboot` stops. `shell32.dll` fails to import `gdi32.dll`,
`shlwapi.dll` and `user32.dll` with `c000011f`
(`STATUS_INVALID_IMAGE_FORMAT`), the delay-load of
`SHGetFolderPathW` fails, Wine tries to start a debugger it does not
have, and no Windows program runs. It is not the disk and it is not
memory — the heap ceiling above was raised because of an *earlier*
instance of this and it moved the failure rather than removing it. Which
of the three loads fails first varies run to run, and that is the most
useful fact about it: it is timing- or ordering-dependent, so it belongs
to the loader or the address space rather than to the filesystem.

`symlink()` is therefore a switch — `symlinks on|off` in the shell — and
not because the code is doubtful. It is a switch because which of two
startup paths Wine takes is a question worth being able to answer by
trying both on one boot, and because a transcript test should say which
path it is testing rather than depend on a default.

### Honest scope

- [x] **A real disk.** ATA PIO, both buses, IDENTIFY, LBA28, read, write
      and flush. `df` shows the drive and the model string it reported.
- [x] **A real filesystem.** FAT32 from scratch: files, directories,
      long names, growth, truncation, rename, MBR or bare volume,
      FSInfo. 64 host-test checks against an image built by a separate
      implementation.
- [x] **It survives a reboot.** `make test-qemu-disk` writes a file, a
      directory and a symlink, syncs, powers the machine off, boots it
      again and reads all three back.
- [x] **Symbolic links**, on, and byte-identical to Linux across
      thirteen comparisons in `fstest.elf`.
- [x] **The initrd stopped being the ceiling.** A file on the disk costs
      a node until something opens it, so a prefix of a thousand files
      costs kilobytes rather than megabytes.
- [x] **Wine's prefix lives on it**, and Wine finds it, names it by its
      DOS path, and loads its builtins out of it.
- [ ] **A Windows program does not yet run on that path.** With symlinks
      on, wineboot stops in shell32's imports. Milestone 33.
- [ ] **Novaris cannot build a prefix itself.** `wine.inf` needs
      `rundll32` and setupapi's file-copy machinery, and the prefix used
      here was built by the same Wine on a host. That is a real gap, not
      a packaging detail.
- [ ] **No page cache.** A mapped file's bytes are heap, and they are not
      given back until the node is dropped. It is why the heap ceiling
      had to move, and it is the obvious next thing the disk makes
      worth building.
- [ ] **No DMA, no interrupt-driven I/O.** Every sector is moved a word
      at a time by the CPU, with the machine stopped. Fine at these
      sizes; measurable at larger ones.
- [ ] **One volume, no mount table.** `/disk` is where it goes.
- [ ] **No GUI**, **no networking**, and `fork` still copies eagerly —
      unchanged from Milestone 30.

## Milestone 33 — Wine's real startup path (in progress)

Milestone 32 ended by naming one question: **why does `shell32.dll` fail
to import `gdi32.dll`, `shlwapi.dll` and `user32.dll` with
`STATUS_INVALID_IMAGE_FORMAT`, once Wine has its DOS drives?**

It had four answers, none of which was about shell32, gdi32 or image
formats. Each was found by making the kernel or Wine say something it had
not been saying, rather than by reasoning about the message.

### 1. A shared library that was never shipped

Wine's builtins come in halves: a PE `.dll` the Windows program links
against, and a Unix `.so` it reaches through `__wine_init_unix_call()`.
Milestone 31 learned that shipping only the first half breaks the DLL and
listed the three `.so` files to copy. It did not list what *those* link
against, and `win32u.so` needs `libm.so.6`.

`win32u` is the Unix backend for **both gdi32 and user32**. Without it
neither can initialise, and shell32 imports both. So the reported failure
was three DLLs deep from a missing maths library, and Wine said so
exactly once, in a warning on a trace channel nobody had turned on:

```
warn:module:get_builtin_unix_funcs failed to load "//i386-unix/win32u.so":
    libm.so.6: cannot open shared object file: No such file or directory
```

The fix is not "also copy libm". `wine-initrd` now runs `objdump -p` over
everything it stages and copies whatever the binaries say they need -
the same answer Milestone 30 reached about the NLS tables, for the same
reason: a missing file does not announce itself.

### 2. An mmap arena that never gave anything back

`mmap_next` only ever went up. `munmap` returned the frames and kept the
*addresses*, so a process that mapped and unmapped repeatedly walked the
pointer to the end of the 96MB arena and had every anonymous mapping
refused from then on.

A PE loader maps a header, maps each section and unmaps the lot again for
every DLL it merely *probes* - so `wineboot`, which probes dozens, ran the
arena out while every shorter-lived process around it was fine. That is
why the same DLL loaded in eight processes and failed in the ninth, and
why the failure moved when unrelated things changed.

`mmap` now does first fit from a rotating hint, and a page that is
present *or reserved* counts as taken. The second half matters on its
own: a PROT_NONE reservation is somebody's plan for an address, and the
bump pointer had been handing those addresses out to somebody else.

### 3. A stack in the wrong place

Wine's `mmap_init()` takes the address of a local variable and asks
whether the stack is above `0x7FFE0000`. On Linux/i386 it is - the
initial stack sits just under `0xC0000000` - and Wine reserves the space
*below* the stack for Windows images and stops at `0xC0000000`.

Novaris put an ELF program's stack at `0x40100000`, so the answer was no,
and Wine instead tried to reserve everything from `0x7FFE0000` to the top
of the 32-bit address space: straight into the kernel's half, refused
page after page, ending with no reserved area to place images in at all.
The kernel now says so out loud when it refuses a mapping, and a boot log
full of

```
[posix] mmap refused: above user space addr=0xc0040000 len=65536
```

is what that had looked like from here all along. An ELF program's stack
now goes where Linux puts it, `0xBFFF0000`.

### 4. A four-entry table shared between a dozen processes

A shared mapping has to outlive both the descriptor it was made from and
the file's *name* - creating a file, opening it, unlinking it and mapping
it is how anonymous shared memory is made on Unix, and it is what
wineserver does. Milestone 30 held a reference for each such node in a
table it sized at four "because Wine uses one".

The table was global rather than per process, and past the fourth entry
the old code took **no reference at all**. The descriptor was then closed
- Wine closes it as soon as the mapping exists - and with the name
already gone the node was freed and its storage reused while two
processes still had its frames mapped.

Per process now, thirty-two of them, and an overflow keeps the reference
instead of dropping it: a node held for the life of the machine is a
leak, and a node freed under a live mapping is memory corruption.

### What that bought

The shell32 cascade is gone. Wine gets through its builtins, `wineboot`
runs past the point it used to stop at, `services.exe` works through the
auto-start list, and `explorer.exe` starts. `make test-wine-prefix`
asserts the path as far as it reliably goes.

Two other bugs turned up on the way and are fixed: `setenv` used to
*replace* the environment rather than add to it, so the first `setenv` in
a session silently discarded `HOME` - which sent a whole debugging run to
the wrong prefix before the transcript gave it away - and `fsync()` now
pushes the disk's FSInfo block out instead of reporting that there is
nothing behind the filesystem.

### Double-clicking a .exe

Which was the other half of "part of the OS": Wine that only a shell
command can reach is still something you have to know about.

The File Explorer's double-click used to build a `run` command - the
hand-written Win32 layer, about five hundred APIs, enough for the test
programs in `userland/pe_test` and not for a Windows program in general.
It goes to Wine now when Wine is installed, and `run` is still there for
the built-in layer and still what `make test-qemu` asserts.

`app_open_path()` is the one place that decides, because there are three
ways to ask - a double-click in the File Explorer, a hit in the Start
menu's search, `Enter` on a selected row - and they should not be able to
disagree. It also prints what it is about to do *before* it does it: the
first Wine launch of a boot takes minutes while the prefix is built, and
`terminal_sink()` pushes frames out as output arrives, so the window fills
in rather than the desktop appearing to hang.

The File Explorer also learned directories, which it needed the moment
Wine was installed into `/usr/lib/wine`: a browser that can only see the
root cannot reach most of the machine. Folders sort as their own kind,
double-clicking one enters it, Backspace goes up, and the toolbar shows
the path instead of the filter name.

**`make test-desktop` is three mouse gestures and no keyboard**: double-
click the File Explorer icon, click maximize, double-click `hellowin.exe`,
and assert the program's own output comes back. `tools/qemu_test.py` grew
`--click` for it - absolute positioning by slamming the pointer into the
corner and moving out, because QEMU's `mouse_move` is relative for the
PS/2 mouse that is the only kind Novaris has a driver for. The
coordinates are a screen layout and therefore brittle, and there is no way
around that: they are what a user's hand does.

### Still open: the shared session object

`NtUserRegisterClassExWOW` fails in every process with "Failed to get
shared session object for window class", and without it there is no
desktop window and no GUI. Traced, the message under it is:

```
warn:winstation:find_shared_session_object Session object id doesn't
    match expected id 1
```

The client maps wineserver's session file successfully and then reads an
object header that is not the one the server says it wrote. So the two
processes' `MAP_SHARED` views of one file disagree.

What is already ruled out: the mapping mechanism itself.
`userland/mmap_test.c` gained a section that reproduces wineserver's
exact pattern - grow a file with `ftruncate` while it is mapped, map only
the newly added tail at a non-zero offset, keep the earlier mappings, and
have a *separate process* open the same file and write through its own
view - and Novaris matches Linux on all ten checks. The remaining
difference between that test and wineserver is how the second process
gets the file: by name in the test, and as a descriptor passed over a
Unix socket, for a file that has already been unlinked, in Wine.

That is the next thing to test, and the test is the obvious one: teach
`sock_test.c` to pass a descriptor for a mapped, unlinked file over
`SCM_RIGHTS` and check coherence through it.

Two other things are worth saying honestly about the current state:

- **The failures vary between runs of the same image.** Which DLL fails,
  and how many do, is not stable. Something is still timing- or
  ordering-dependent, and the remaining `c000011f` reports are almost
  certainly a second cause rather than noise from the first.
- **`CreateProcess` still reports `STATUS_NO_MEMORY`** when starting
  explorer, on a machine with two gigabytes free. `fork` copies the
  address space eagerly, and that is the obvious suspect.

## Milestone 34 — the shared session object, and what it really was ✅ DONE

Milestone 33 ended on one sentence: *"the client maps wineserver's session
file successfully and then reads an object header that is not the one the
server says it wrote, so the two processes' `MAP_SHARED` views of one file
disagree."*

Every word of that is true except the last two. The client's view was
never `MAP_SHARED`.

### `MAP_PRIVATE` is not a snapshot

Wine maps a read-only view of a section with `MAP_PRIVATE`, on purpose,
and says why in a comment in `dlls/ntdll/unix/virtual.c`:

```c
/* changes to the file are not guaranteed to be visible in read-only
 * MAP_PRIVATE mappings, but they are on Linux so we take advantage of it */
#ifdef __linux__
    flags |= MAP_PRIVATE;
```

That is the whole bug. On Linux a private file mapping is not a copy of
the file: it is the file's own pages, and it stops being them one page at
a time, when this process writes to that page. Until then a writer
elsewhere shows through it. Novaris answered `MAP_PRIVATE` by allocating
pages and reading the bytes in — behaviourally correct about every
question `userland/mmap_test.c` had ever asked it, and a snapshot taken at
the instant of the call.

So win32u mapped the session block, read the objects wineserver had
written *so far*, and never saw another one. `NtUserRegisterClassExWOW`
looked for a window class whose object the server had created after the
mapping existed, found the header of whatever had been at that offset
before, and reported the id mismatch. Sixteen times in a run, once per
class, in every process — and no desktop window, because
`get_desktop_window` registers a class first.

Nothing about the search in Milestone 33 was wrong, and its conclusion was
sound: *the mapping mechanism itself is not at fault*. It was looking at
the wrong mapping.

### Copy-on-write

Two page-table bits, both from the three the CPU leaves to the OS:

- `PAGE_COW` (bit 11): this page is a private view of memory that belongs
  to a file. It carries `PAGE_SHARED` too, so no teardown path frees the
  frame.
- `PAGE_COW_RW` (bit 10): the mapping it belongs to permits writes. Both
  kinds have `PAGE_RW` clear, because both have to *fault* on a write —
  one to be given its copy, the other because writing to a read-only
  mapping is an access violation. Bit 10 is `PAGE_RESERVED`, which only
  ever means anything on an entry that is not present; a COW page is
  present, so the readings never overlap.

`paging_break_cow()` allocates a frame, copies the page through a page of
BSS (the destination cannot be mapped anywhere else while the source is
still at the only address that reaches it), remaps and drops all three
bits. `handle_user_fault()` calls it before the program is told anything,
because this is the one page fault that is a normal part of running a
program — a Wine client takes hundreds.

`fork` needed no change: it already shared `PAGE_SHARED` pages verbatim
rather than copying them, which is exactly right for a COW page, and both
sides then break it independently.

Three other things had to move with it:

- **`mprotect` on a COW page** changes what the program may do, not whose
  frame it is. The page keeps faulting on a write however writable it has
  been made; what changes is `PAGE_COW_RW`, and therefore what the fault
  does. This is not a corner: Wine maps every section view writable and
  mprotects it down afterwards, so a shared session block spends its whole
  life as a page that has been through here.
- **Anonymous `MAP_FIXED` over a file's frames** has to *replace* them. It
  used to keep the frame and apply the new protection, which was a
  harmless waste when a private file mapping was a copy and is corruption
  of somebody else's file now that it is not. A dynamic linker does this
  every time it maps a library: data segment from the file, then the
  `.bss` tail over the end of it with `MAP_FIXED|MAP_ANONYMOUS`.
- **The kernel's own writes.** A ring-3 write to a COW page faults; a
  ring-0 write does not, because this is an i386 with `CR0.WP` clear. So
  `read()` into a buffer inside a private file mapping would go straight
  into the file. Nothing Wine does lands there, and
  `break_cow_for_kernel_write()` is called by `read`, `pread64` and
  `getdents64` anyway, because "nothing does it today" is not a property
  this kernel can check.

`userland/mmap_test.c` sections 4a and 4b are the test, and they are the
usual kind: run the same binary on Linux and on Novaris and diff. Before
the change Novaris failed exactly three checks — a store through a shared
mapping showing through a private one, the same from another process, and
a page that was never written still tracking the file. After it, all
forty-three match.

### Two bugs found on the way, both mine

Both were introduced by the change above and both were found the same way,
by making the kernel say something it had not been saying.

**A growth reserve, re-decided.** `vfs_make_mappable()` gives a mapped
file page-aligned storage, and gives a file that this kernel's own memory
backs four megabytes of headroom so that growing it later does not move
it — wineserver's session file is what that was measured against. A file
out of the initrd cannot grow, and since private mappings became
copy-on-write *every* library a dynamic linker maps comes through here, so
four megabytes each for Wine's few dozen is a hundred megabytes of heap
spent on headroom for growth that cannot happen. Excluding initrd files
was right and the test for it was wrong: `from_initrd` is cleared by the
copy that makes the file mappable, so the *second* mapping of the same
library found a file that no longer looked like an initrd one, took the
reserve, and reallocated — moving every byte while the mapping made a
moment earlier still pointed at the old frames. ld.so maps a library four
times over (the whole file, then each segment), and what it read out of
the second mapping was the third one's memory:

```
wine: could not load ntdll.so: eh\xdd\xdd\xdd: cannot open shared object file
```

— a `DT_NEEDED` name read out of a freed heap block. The reserve is a
decision made once now, when the storage is first laid out and nothing can
be mapped through it yet. `vfs_make_mappable` also says so out loud when
it replaces storage that was already mappable, because silently that
arrives as a corrupt file three subsystems away.

And separately: sizing a file's storage to *the mapping* rather than to
the file was wrong for the same reason. A mapping routinely runs past the
end of the file it is of — a loader maps a segment rounded up to a page,
or a section whose tail is bss — and asking for storage that far grows the
buffer and moves it. Sized to the file, the first mapping settles the
storage and no later one can move it; the pages past the end get ordinary
zeroed memory, which is what they read as anyway.

**A loop variable.** `mprotect` cleared `PAGE_RW` from the flags it was
about to apply, inside the loop, so the first COW page in a range decided
how every ordinary page after it was mapped. One `mprotect(0x7bf13000,
0x2000, PROT_READ|PROT_WRITE)` therefore left the second of its two pages
read-only, and a write to it arrived in Wine as `Unhandled exception code
c0000005 addr 0x7bf7f5cd`. What found it was a new `[fault]` line under
`strace`: vector, eip, address, direction, and the page table entry.
`pte=0x0444aa25` reads as present, user, read-only, shared, COW, and *not*
`PAGE_COW_RW` — which is the whole answer, in a line the kernel had never
printed before.

### The second cause behind `c000011f`

Milestone 33 said the remaining invalid-image-format reports were "almost
certainly a second cause" and that they moved between runs. They were, and
this is it:

```
err:winediag:NtCreateFile Too many open files, ulimit -n probably needs
    to be increased
```

`POSIX_MAX_FDS` was 128. Once the desktop window existed, wineboot got as
far as explorer, services.exe, svchost and winemenubuilder — which walks
the Start menu writing an icon file per entry — and 128 was gone again.
Every DLL that then failed to open was reported upwards as an invalid
image format, which is why the failures would not stay still: a limit is
reached at a different point every run. 512 now, sixteen kilobytes per
process slot.

`ugetrlimit`/`prlimit64` also tell the truth about `RLIMIT_NOFILE` now
rather than answering "unlimited". Unlimited is not generous, it is a lie
the caller acts on: wineserver sizes its own table from it and the client
caches descriptors up to it, so `EMFILE` arrives at a Wine that has
already decided it cannot happen.

`kmalloc` says so once when the heap is exhausted, for the same reason —
this kernel has no page cache, a mapped nine-megabyte DLL costs nine
megabytes of heap and keeps costing it, and running out arrives everywhere
else as something else entirely.

### What that bought

Against the same prefix image, on the same Wine, the error profile of a
whole run:

| | Milestone 33 | Milestone 34 |
| --- | --- | --- |
| `Failed to get shared session object` | 16 | 0 |
| `failed to create desktop window` | 17 | 0 |
| `service ... failed to start` (timeout) | 4 | 0 |
| `Loading library ... failed (c000011f)` | yes | 0 |
| `unimplemented function shell32.SHGetFolderPathW` | aborts the run | not reached |

Wine registers its window classes, creates its desktop window, starts its
services, runs explorer — **and a Windows program runs to completion on
the real path**, with the prefix on the disk and symbolic links on:

```
novaris> wine hellowin.exe
Hello from a real Windows .exe running on Novaris!
  compiled by mingw-w64, linked against msvcrt.dll
...
fib(20):    6765
argc:       1, argv[0]: Z:\hellowin.exe

Exiting with code 0.
```

`Z:\hellowin.exe` is the line that matters. Every earlier milestone got
`C:\windows\hellowin.exe`, which is the fallback Wine takes when it has no
DOS drives at all; `Z:\` means the drive table built out of symbolic links
in the prefix is what resolved the path. `make test-wine-prefix` asserts
that transcript and rejects every line in the table above, so the progress
is a test rather than a claim, and `make test-wine` and
`make test-wine-threads` still assert what they always did.

### Seamless: `wine hellowin.exe`

The shell has a `wine` command. `run wine-preloader wine hellowin.exe`
still works and is what `make test-wine` uses on purpose — a test that
spells out the whole invocation tests one layer fewer — but the three
names in it are the preloader that reserves the low address space, the
loader it hands control to, and the program, and only the last is the
user's business. On the ordinary `novaris.iso`, which carries no Wine,
`wine` says so and points at `run` instead of failing three layers down.

### Still open

- **Novaris still cannot build or update a prefix.** wineboot reaches
  `update_wineprefix()`, which is `rundll32` over `wine.inf` and
  setupapi's file-copy machinery. A prefix therefore still has to be built
  by the same Wine on a host and written to the image with `make
  wine-disk`. What is new is that it is only the *building* that is
  missing now: given a prefix, using one works.
- **It is slow.** wineboot takes longer than the five minutes Wine allows
  for its own boot event, so a successful run contains
  `err:environ:run_wineboot` — Wine giving up on waiting and carrying on.
  Most of that is this kernel having no page cache: a mapped DLL is read
  off an ATA PIO disk and copied into the heap in full, per file, and
  wineboot touches a lot of files. Making it quick is a separate problem
  from making it work, and this milestone did the second one.
- **No GUI and no networking.** There is no display backend for Wine to
  drive (`winex11.drv` and friends do not load), and the services that
  want a network — `nsiproxy`, `NDIS` — fail as they should.
- **A mapped file's storage is never given back.** It is the heap, it is
  the whole file, and nothing evicts it. That is the price of having no
  page cache, and it is now paid by private mappings too.

## Milestone 35 — Wine, installed ✅ DONE

Milestone 34 got a Windows program running under Wine on the real startup
path. It did not make Wine part of this operating system, and the
difference is not cosmetic.

What "a tool sitting next to an OS" looked like, concretely:

- **Two ISOs.** `novaris.iso` was the OS; `novaris-wine.iso` was a
  separate image built by a separate target.
- **No paths.** The initrd format had a 60-byte name field and no
  directories, so everything shipped in it landed at the root:
  `/ntdll.so`, `/gdi32.dll`, `/wine-preloader`, `/passwd`.
- **A fallback holding it together.** `/lib32/libc.so.6` resolved only
  because the kernel matches the last component of a path whose
  directories do not exist. That fallback is load-bearing for a flat
  archive and invisible until you look.
- **An invocation nobody would guess.** `run wine-preloader wine
  hellowin.exe`, because the preloader had to be named and handed the
  loader by hand.
- **A prefix built somewhere else.** Novaris could not make one, so it was
  built by the same Wine on a host and written into a disk image with
  `make wine-disk`.

The last two were consequences of the second. Wine finds itself by
`dladdr`-ing ntdll.so and then does *relative* arithmetic on that path
(`dlls/ntdll/unix/loader.c`, `init_paths` and `build_relative_path`):

    dll_dir    = <prefix>/lib/wine          the builtins, both halves
    bin_dir    = <prefix>/bin               wineserver
    data_dir   = <prefix>/share/wine        wine.inf and the NLS tables
    wineloader = <dll_dir>/i386-unix/wine   what it re-execs

With ntdll.so at `/`, all four came out as nonsense. Three of them were
survivable because the fallback found the files anyway. The fourth was
not: a path that does not resolve at all has no NT form, so `WINEDATADIR`
was never set, so `get_wine_inf_path()` returned NULL, so wineboot printed

```
wine: failed to update L"\??\Z:\disk\.wine", wine.inf not found
```

and returned. That one line was the whole of Wine not being installed, and
it is why every milestone up to 34 needed a prefix built on a host.

### Directories in the initrd

The archive format grew them. `'STLR'` became `'STL2'`, the name field
went from 60 bytes to 124, and a name is a path now:
`mkinitrd.py` walks the tree, and `ramfs.c` makes each directory on the
way to a file the first time it sees one. Different magic so a stale image
is refused rather than read as gibberish.

### Installed

`tools/install_wine.sh` puts Wine where `make install` puts it:

```
/usr/lib/wine/i386-unix/     ntdll.so, win32u.so, ws2_32.so, bcrypt.so,
                             wine, wine-preloader
/usr/lib/wine/i386-windows/  the PE builtins - 29 DLLs and 10 programs
/usr/bin/                    wine, wineserver
/usr/share/wine/             wine.inf
/usr/share/wine/nls/         the NLS tables
/lib32/, /lib/               the host's ld-linux.so.2, libc, libm, libgcc
/etc/                        passwd, nsswitch.conf
```

Wine's arithmetic lands on that without being told anything.

One trap on the way, and it is a good one. `/usr/bin/wine` is a *different
binary* from the loader in `i386-unix`: the loader finds ntdll.so beside
itself, which is only true where it is installed, while the bin-directory
wrapper (`tools/wine/wine.c`) works out bindir from its own path, converts
that to libdir with the same relative arithmetic, and dlopens
`<libdir>/wine/i386-unix/ntdll.so`. Installing the loader in `/usr/bin`
instead gets

```
wine: could not load ntdll.so: /usr/bin/ntdll.so: cannot open shared
    object file: No such file or directory
```

which is what the first attempt at this did.

### And then Wine built its own prefix

With `/usr/share/wine/wine.inf` where Wine expects it, wineboot stops
saying it cannot find it and runs `rundll32` over it — the thing
`ROADMAP.md` had listed as out of reach since Milestone 32. setupapi
copies the files, writes the registry, makes the drive table, and the
program runs:

```
novaris> wine hellowin.exe
Hello from a real Windows .exe running on Novaris!
  compiled by mingw-w64, linked against msvcrt.dll
...
fib(20):    6765
argc:       1, argv[0]: Z:\hellowin.exe

Exiting with code 0.
```

No disk, no host, no setup commands, symbolic links on, one ISO.
`Z:\hellowin.exe` is the DOS drive table resolving the path — the prefix
Wine built for itself, on Novaris, a minute earlier.

Two smaller things had to be true for that:

- **`/root` exists, and `$HOME` is it.** Wine creates `$HOME/.wine` but
  only the last component of it, so a missing `/root` is "wine: chdir to
  /root/.wine : No such file or directory". The initrd carries files, and
  an empty directory is not a file, so the kernel makes it — beside `/tmp`
  and `/disk`, which it already made for the same reason.
- **Files have a modification time.** setupapi sets the times on every
  file it copies, and `utimensat` was not implemented. Seventy-six
  `Converting errno 38 to STATUS_UNSUCCESSFUL` lines in one wineboot, each
  one a file copy reporting failure. `vfs_node_t` has an `mtime` now, set
  on create, write and truncate, reported by `stat`, and settable through
  `utimensat`/`utime`/`utimes`/`futimesat`. The `*removexattr` family is
  answered too, which was the only other unimplemented syscall left in a
  run.

### One write per boot failed, and had since Milestone 32

The prefix belongs on the disk, so `make test-wine-prefix` puts an empty
512MB volume in front of Wine and lets it build one there. Wine said:

```
wine: chdir to /disk/.wine : No such file or directory
```

`mkdir /disk/.wine` from the shell failed too — and then the *next* one
worked, and so did every write after it. One write per boot, always the
first, on any volume, of any size, for any name. `make test-qemu-disk` had
never caught it because it writes a file first and asserts on the second
operation onwards; nothing had ever looked at the very first write.

The chase is worth recording because three plausible answers were wrong.
It was not the leading dot in `.wine` (a plain `mkdir` failed identically).
It was not the 4096-byte clusters (a one-sector write failed too). It was
not the driver's logic at all: `tests/fat32_host_test.c`'s harness, which
links the real FAT32 driver against an image file, created both
directories on the same 512MB image without complaint — which pointed
squarely at the one thing the host harness does not have, the ATA driver.

Two new diagnostics found it in one run each. `fat_create()` says which of
its five failure paths it took, and `ata_report()` prints the status and
error registers. Together:

```
[ata] busy after flush: status 0xc0 error 0x00
[fat32] cannot create "x": write error zeroing its cluster
```

`0xc0` is BSY|DRDY: the CACHE FLUSH at the end of the write had not
finished, and `ata_wait_not_busy()` gave up on it. Its timeout was
`ATA_TIMEOUT`, four million iterations of a polling loop — and **a spin
count is not a duration**. One `inb` is a bus cycle on real hardware and a
trap into the emulator here, two figures orders of magnitude apart, and
four million of the latter is a few seconds. The first flush of a boot is
the one where the host has to allocate and `fsync` a sparse image it has
never written to, so it is the one flush that takes longer than that.
Every later flush is fast, which is exactly why exactly one write failed.

The timeout is in PIT ticks now — five seconds, a real duration — with the
old spin count kept as a backstop for a wait that happens with interrupts
disabled, where the tick counter cannot move.

### One image

There is one ISO. `make` builds the OS; with `WINE_BUILD` pointing at a
built Wine, `make` installs Wine into it. Without, it builds the same OS
without Wine, and `wine` in the shell says so and points at `run` instead
of failing three layers down. `make wine-initrd`, `novaris-wine.iso`,
`make wine-disk` and `WINE_PREFIX_DIR` are all gone.

`make test-wine` and `make test-wine-threads` now run the default
configuration and ask for nothing: no `symlinks off`, no separate image,
`wine hellowin.exe` typed at the shell. Between them they reject
`wine.inf not found`, `Failed to get shared session object`, `Too many
open files`, `could not load ntdll.so` and `unimplemented syscall` —
every line that used to be normal. `make test-wine-prefix` is the same
thing on an *empty* disk, where Wine builds its prefix on real storage and
names it `Z:\disk\.wine`.

`tools/qemu_test.py` grew `--stop-when-matched`, because a Wine run takes
minutes and varies: the settle is a budget now rather than a fixed cost,
and only a failing run spends it.

### Still open

- **No display backend, and it is the only thing between here and a
  Windows program with a window.** Measured rather than assumed: Wine's
  own `notepad.exe` was staged and run, and it loads, initialises its
  DLLs, and gets all the way to creating its window before stopping on

  ```
  err:winediag:nodrv_CreateWindow Application tried to create a window,
      but no driver could be loaded.
  err:winediag:nodrv_CreateWindow L"The graphics driver is missing."
  ```

  Everything before that point works. What is missing is a driver:
  win32u loads one Unix `.so` and calls `__wine_set_user_driver()` with a
  `struct user_driver_funcs` (`include/wine/gdi_driver.h`). Wine builds
  four - `winex11.drv`, `winewayland.drv`, `winemac.drv`,
  `wineandroid.drv` - and every one of them needs a display server that
  does not exist here. A `winenovaris.drv` would talk to `kernel/wm.c`
  instead, which is what Milestone 11's compositor was built for.

  The interesting part is how *little* of that struct is load-bearing.
  `pCreateWindowSurface` hands back a buffer the program draws into,
  `pWindowPosChanged` says where it goes, and `pProcessEvents` pumps
  input; the keyboard, IME, clipboard, systray, Vulkan and OpenGL
  entry points can all be null. The rest of the work is not in Wine at
  all - it is a way for a ring-3 process to get a window from a window
  manager that currently only serves kernel code.

  One thing to decide before starting: a driver is *Wine* source, and
  this project has kept Wine as an untouched build input. Either it is
  built out-of-tree against Wine's headers, or the tree acquires a patch.
- **It is slow.** wineboot takes longer than the five minutes Wine allows
  for its own boot event, so a successful run contains
  `err:environ:run_wineboot`. Most of it is having no page cache: a mapped
  DLL is read in full and copied into the heap, per file.
- **The DLL set is curated.** 33 of Wine's 601, so parts of `wine.inf`
  have nothing to install and say so (`SetupDefaultQueueCallbackW copy
  error 1812`, `register_fake_dll failed to create IRegistrar`). Widening
  it is a question of how big an initrd may be, which is a question about
  the page cache above. — *Milestone 43 widened it to 52, and found that
  the 33 was really 31: `winex11` and `winspool.drv` were names in the
  list that resolved to no file.*
- **The prefix is rebuilt at every boot when there is no disk**, because
  `$HOME` is `/root` and `/root` is a ramfs. With a disk it is built once
  and stays, which is what `make test-wine-persist` now proves: two boots
  of one image, the first building the prefix and the second running the
  program without building anything. That second boot rejects "created the
  configuration directory", because a prefix rebuilt every time is not an
  installation.
- **Building the prefix is intermittent; using one is not.** Once in about
  four runs, `make test-wine-threads` - which has no disk, so Wine builds
  a prefix in RAM every time - dies during the build with

  ```
  err:module:loader_init "user32.dll" failed to initialize, aborting
  err:module:loader_init Initializing dlls for
      L"C:\windows\system32\rundll32.exe" failed, status c0000005
  ```

  and passes on a re-run. That is `rundll32` taking an access violation
  while installing `wine.inf`, which is the heaviest thing Wine does here
  and the newest: Milestone 35 is the first time it ran at all.

  Worth separating from "Wine works", because the two halves behave
  differently. Every run that *has* a prefix has been reliable -
  `make test-wine-persist`'s second boot, which skips the install
  entirely, and every `make test-wine-prefix`. It is the install that is
  not solid yet, and a disk turns it into a once-ever event rather than a
  once-per-boot one.

- **The pointer leaves a black trail.** Sweeping the mouse horizontally
  across the desktop turns a sixteen-pixel-tall band, the full width of
  the screen, pure black, and it stays black. It has *nothing to do with
  Wine*: it was found in a screenshot taken after a Wine run, and it
  reproduces on a freshly booted desktop with no program running at all -
  boot, move the pointer from one side to the other, screendump. About
  ninety seconds, which is the useful part of this entry.

  Four things are ruled out. It is not the framebuffer console writing
  behind the compositor: `fb_set_compositor_owned()` makes
  `kernel/framebuffer.c` report the first such write by name, and it never
  fires. It is not `render_wallpaper()`, which fills every row. It is not
  `gfx_copy_region()`, `gfx_present()` or `fb_blit()`, all of which clamp
  correctly. And it is not the double-click path or any program's output.

  What is left is the damage bookkeeping between `damage_cursor()`,
  `wm_take_damage()` and `compose()`. The two numbers to explain are the
  height - sixteen, where the cursor's own damage rect is forty-three -
  and the colour, pure black, which is what a `gfx_surface_create()`
  allocation reads as before anything has been drawn into it.
- **Networking.** Unchanged, and still the reason `nsiproxy` and `NDIS`
  fail to start.

## Milestone 36 — a window a program owns ✅ DONE

Every window this desktop has ever drawn was drawn by kernel code. An app
is a paint callback compiled into the kernel (`kernel/app_*.c`), and
Milestone 11's note has stood since: the window manager *"is not reachable
from `CreateWindowEx`"*. Milestone 35 finished with Wine running a console
program and printing `err:winediag:nodrv_CreateWindow` for anything with a
window, because Wine had no display driver to load.

Those are the same sentence from two sides. This milestone is the piece
underneath both: **`/dev/wm`**, a device any process can open to get a
window on the desktop and a buffer to draw into.

```
fd = open("/dev/wm", O_RDWR);         a window slot of this process's own
ioctl(fd, WMIO_CREATE, &create);      size and title; the window appears
px = mmap(0, w*h*4, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
... draw into px, 0x00RRGGBB, w pixels per row ...
ioctl(fd, WMIO_DAMAGE, &rect);        "this changed, show it"
ioctl(fd, WMIO_POLL, &event);         input, or WM_EV_NONE
close(fd);                            the window goes away
```

`userland/wm_test.c` is a freestanding Linux binary - raw `int $0x80`,
`-nostdlib`, linked against nothing - that does exactly that, and
`make test-wm` boots it and screendumps the result. The picture is the
assertion; `tools/check_wm_window.py` reads the PPM and looks for the
gradient, frame and marker the program draws, in a block one window wide.

Three things are worth writing down.

**The mmap needed no new code.** The device's per-open node points its
`data` at the pixel buffer, so the ordinary `MAP_SHARED` path from
Milestone 30 - the one that hands over the frames a file's bytes live in -
gives the process the very memory the compositor reads. A frame is one
`gfx_blit` and no copies.

**The event loop had to become callable.** A ring-3 program runs inside a
shell command, and a shell command runs inside the desktop's event loop -
so while such a program draws, the loop it needs is on the stack beneath
it, blocked. Its window would appear when it exited, which is not a
window. So `desktop_pump()` is now a function rather than the body of a
`for(;;)`, and the program's own ioctls turn the crank: a frame per
damage, input per poll. Nesting is the point, so the guard is around
compositing only, and `shell_busy` stops a keystroke dispatched from a
nested pump starting a second command on top of the first.

**It found a leak with a symptom.** A window is freed when the last
reference to its node goes, so the first thing `/dev/wm` did was show that
`mmap`'s references were never given back: `posix_unpin_all()` asked
`posix_current()` for the process to unpin, but `posix_proc_exit()` clears
a process's `page_directory` first - deliberately, so a later process
allocated the same frame is never mistaken for it - and by then nothing
claims that address space. `posix_current()` registered a *fresh* entry
and unpinned its empty table. The pins are now dropped in
`posix_exit_process()`, where the process is still known, and the sweep at
the end of a run walks the table instead of asking. From the outside this
looked like a dead program's window staying on the desktop, still holding
the keyboard focus - so the shell stopped accepting commands after
`run wmtest.elf`. A page-cache-shaped bug caught by a window manager.

### What this does not do yet

- **The buffer does not follow a resize.** The window resizes and the
  process is told (`WM_EV_RESIZE`); the buffer keeps its size and the
  paint letterboxes. Resizing the mapping means unmapping frames out from
  under a process that is drawing into them.
- **One window per open.** Wine's driver will want one per `HWND`. The
  interface does not have to change for that; `kernel/wmdev.c` does.
- **It is not a Wine driver yet.** That is the next step and the reason
  this exists: `win32u` loads one Unix `.so` and asks it for a surface to
  draw into (`pCreateWindowSurface`), tells it where the window went
  (`pWindowPosChanged`) and pumps input through it (`pProcessEvents`).
  Those three are the three ioctls above, which is not a coincidence -
  it is why the interface is shaped this way rather than around what a
  kernel app happens to need.

## Milestone 37 — a Windows program with a window ✅ DONE

`wine notepad.exe`, and Notepad opens on the Novaris desktop: a window
with a title bar reading "Untitled - Notepad", a taskbar button, and a
client area Wine draws into. `make test-wine-gui` boots it and asserts it
from the screendump.

`notepad.exe` and `winemine.exe` are also installed at the root of the
filesystem, which is what the File Explorer opens on - so double-clicking
one runs it under Wine and a window appears, with nothing typed. That is
the whole of Milestone 33's "double-clicking a .exe" and Milestone 37's
window in one gesture.

Milestone 35 ended with the sentence this milestone deletes:

```
err:winediag:nodrv_CreateWindow Application tried to create a window,
    but no driver could be loaded.
err:winediag:nodrv_CreateWindow L"The graphics driver is missing.
    Check your build!"
```

Wine had no display backend. Milestone 36 built the thing a display
backend would need - `/dev/wm`, a window and its pixels for any process -
and this is the driver that uses it: **`wine/winenovaris.drv`**, in this
repository, grafted into a Wine tree and built there by
`tools/build_wine_driver.sh`.

### The whole driver, in four entry points

`win32u` draws a window into a *window surface*: a plain top-down 32-bit
DIB. A display driver gets that DIB onto a screen and sends input back.

| Wine asks for | Novaris answers with |
| --- | --- |
| `pUpdateDisplayDevices` | one monitor, the desktop work area (`WMIO_SCREEN`) |
| `pCreateWindowSurface` | a `window_surface` over an ordinary DIB |
| `pWindowPosChanged` | `open("/dev/wm")`, `WMIO_CREATE`, `mmap` |
| the surface's `flush` | the dirty rows copied in, then `WMIO_DAMAGE` |
| `pProcessEvents` | `WMIO_POLL`, into `NtUserSendHardwareInput` |

That last one matters more than it looks. Input goes back through the
same call X11 and Wayland make, so from `user32` upwards there is no
difference between a click on a Novaris window and a click on an X11 one -
no special cases anywhere above the driver.

`pGetWindowStyleMasks` is the fifth, and it is about *not* doing
something: it tells win32u that `WS_CAPTION`, `WS_DLGFRAME` and
`WS_THICKFRAME` are drawn by the host. Getting it to work took two goes
and a font engine - see below.

### Three things that had to be true first

**explorer has to be able to find the driver.** It walks a list of names
from `HKCU\Software\Wine\Drivers\Graphics` and falls back to a built-in
`"mac,x11,wayland"`. `tools/install_wine.sh` sets that registry value
through `wine.inf`, which is the supported way and the one a user could
change - but a prefix that already exists never sees it, and a prefix here
is built by a `wineboot` that does not always finish. So
`tools/build_wine_driver.sh` also puts `novaris` at the front of
explorer's default: one `sed`, idempotent, and an installed driver becomes
a used driver whatever state the prefix is in.

**fork has to count the references its mappings hold.** This one was
already there and had never mattered. `posix_proc_clone()` copies the
whole process structure and then takes a reference for every *descriptor* -
but not for the nodes in `pinned[]`, where a shared mapping's reference
lives. Nothing ever gave a pin back either, because `posix_unpin_all()`
asked `posix_current()` for the process to unpin and `posix_proc_exit()`
had already cleared the `page_directory` that identifies one - so it
registered a fresh entry and unpinned its empty table. A leak balanced by
another leak.

Milestone 36 needed the unpin to work, because a `/dev/wm` window is freed
when the last reference to its node goes. Making it work turned the pair
of leaks into a use-after-free: a forked child exiting dropped references
it had never taken and freed nodes its parent still had mapped. Wine
forks constantly, and the prefix build went from "fails one run in four"
to "faults every run" at two stable addresses. The fix is one loop in
`posix_proc_clone()`.

**Not every HWND is a window.** Wine creates a desktop window, a
message-only window per thread, child controls and tool windows. The first
version of this driver gave all of them a `/dev/wm` slot, and the first
screenshot of Notepad had four empty black windows behind it. The test is
Wine's own, from `nodrv_CreateWindow` and X11's `is_window_managed`: the
desktop must be its parent, and it must look like something a user would
drag.

### The window frame is measured in glyphs

The first Notepad had no text in it, and it wore two title bars - its
own, drawn by Wine inside the client area, and the one Novaris drew
around it. Those looked like two unrelated cosmetic problems. They were
one problem, and it was not a cosmetic one.

That Wine was configured `--without-freetype`, because the host had no
32-bit FreeType to link against. No font engine means no font, and no
font means `get_text_metr_size()` fills a `TEXTMETRICW` with whatever was
on the stack. `normalize_nonclientmetrics()` then does:

```c
get_text_metr_size( hdc, &pncm->lfCaptionFont, &tm, NULL );
pncm->iCaptionHeight = max( pncm->iCaptionHeight, 2 + tm.tmHeight );
```

`tm.tmHeight` came out at about six and three quarter million, so
`SM_CYCAPTION` was six and three quarter million, and so was every
rectangle computed from it:

```
DBG hwnd 0x10050 style 04cf0000 win (0,0)-(960,570)
    cli (4,6750323)-(956,6750323) vis (4,6750323)-(956,6750324)
```

The window rect is right; the client rect is a strip one pixel tall at
y=6750323.

That is why implementing `pGetWindowStyleMasks` - the correct fix for the
double title bar, since it tells win32u the host draws the caption -
made Notepad disappear entirely, with a transcript that still passed
every assertion. win32u turns the mask into a rectangle with
`NtUserAdjustWindowRect`, and that rectangle is built from
`SM_CYCAPTION`. The window came out 952 pixels wide and one tall.

So: `apt-get install libfreetype-dev:i386`, reconfigure Wine
`--with-freetype`, and `tools/install_wine.sh` ships three things it did
not before -

- Wine's thirteen bundled `.ttf` faces into `/usr/share/wine/fonts`,
  which is where `get_fonts_data_dir_path()` looks. 484KB.
- `libfreetype.so.6` and what it needs (`libpng16`, `libz`, `libbz2`,
  `libbrotlidec`, `libbrotlicommon`) into `/lib32`. Named explicitly,
  because win32u `dlopen`s FreeType by SONAME - it is in no binary's
  `NEEDED` list, so the "ask the binaries what they need" loop that
  ships every other library cannot see it.
- and `pGetWindowStyleMasks` is now implemented, because the numbers it
  depends on are real.

Notepad now has a menu bar reading File / Edit / Format / View / Help, a
white document area with a scroll bar, a status bar saying "Ln 1, Col 1",
and exactly one title bar. `tools/check_wine_window.py` was rewritten
around that: it used to look for one flat block of `COLOR_3DFACE`, which
is what a window with no font engine looks like, and it now wants a white
`COLOR_WINDOW` document area *with* grey chrome around it - the shape of
a window that works rather than one that merely exists.

### What it does not do yet

- ~~**The window does not resize.**~~ — addressed in Milestone 42, by
  making the buffer bigger than the window rather than by resizing it.
  Written and compiling, **not yet booted**; until `make test-wm` and
  `make test-wine-gui` have run, treat this item as open.
- **There is a copy per frame.** `flush` memcpy's the dirty rows from the
  surface's DIB into the `/dev/wm` mapping. It could hand the mapping to
  `window_surface_create()` as the bitmap's bits and have GDI draw
  straight into the window - but only once resizing works, because a
  surface is recreated on every size change.
- **The keyboard is a table, not a layout.** `vkey_for()` maps Novaris's
  character-plus-code to a virtual key well enough to type; a real
  keyboard needs `pKbdLayerDescriptor` and a `KBDTABLES`.
- **No OpenGL, no Vulkan, no clipboard, no IME, no cursor shapes, no
  display-mode changes.** All optional entry points, all absent, and
  win32u has sensible behaviour for a driver that does not fill them in.
- **And `chrome.exe` is still not reachable.** A browser needs a 64-bit
  Wine build (this one is 32-bit only), a GPU, a sandbox built on NT APIs
  this kernel does not have, and far more than the 52 of Wine's 601 DLLs
  that ship here (31 when this was written; Milestone 43 raised it). Milestones 38-40 gave the machine networking, which
  removes one item from that list and none of the others. What works is a
  Windows program with a window - `notepad.exe`, `winemine.exe` - and that
  is a different sentence from "Windows programs work".

## Milestone 38 — a network card, an address, and a packet that comes back ✅ DONE

`net up`, and the machine has an IP address it was given rather than one
it was told. `ping 10.0.2.2`, and the reply comes back.

- **PCI** (`kernel/pci.c`): configuration mechanism #1, ports 0xCF8 and
  0xCFC. A bus scan, a device list, `pci_find`, bus mastering, and the
  I/O base out of a BAR. `net pci` prints what is on the bus.
- **RTL8139** (`kernel/rtl8139.c`): chosen because it is the smallest
  real Ethernet controller — receive is one circular buffer, transmit is
  four registers and four buffers, and there are no descriptor rings on
  either side. Every other card worth having is better hardware and more
  code, and the thing being built is the stack above it.
- **Ethernet, ARP, IPv4, ICMP, UDP** (`kernel/net.c`), and **DHCP**
  (`kernel/dhcp.c`): DISCOVER, OFFER, REQUEST, ACK.

DMA is the part that needed something new underneath: the card addresses
memory itself, so its buffers must be physically contiguous and known by
physical address (`pmm_alloc_contiguous`, `paging_alloc_dma`).

## Milestone 39 — TCP, DNS and HTTP ✅ DONE

`fetch http://host/path [file]`, and the bytes are on the machine.

- **TCP** (`kernel/tcp.c`): a connection table, a segment builder, a
  receive path that is one switch on the state, and a tick that
  retransmits. Sequence numbers are compared through the sign of a
  *signed* difference, which is correct across the wrap; comparing them
  with `<` is a bug that appears after four gigabytes and never in a test.
- **DNS** (`kernel/dns.c`): A records, with a jump-counting walker for
  compression pointers so a malicious answer cannot loop it.
- **HTTP** (`kernel/http.c`): HTTP/1.0 with a Host header. The server's
  close is the framing.

No TLS, and deliberately so: a client that says "https" while validating
nothing would be worse than not having one.

### The two bugs that were the milestone

Both looked like slowness rather than breakage, which is why they took
the longest.

**Partial segment acceptance.** Taking *part* of a segment looks like
progress and is the opposite: `rcv_next` lands in the middle of what the
sender has moved past, so every following segment is out of order and
dropped until the retransmission timer fires. A 200KB download stopped
dead at 53KB with every byte it had correct. Segments are now all-or-
nothing.

**The acknowledgement nobody sent.** `ack_pending` is set by the receive
path, which runs in the card's interrupt, and was cleared *after*
`send_ack()` returned, which does not — so a segment arriving during the
send set the flag and the store then wiped it. Nobody asked again,
because asking is what the flag was for. The sender waited on an
acknowledgement that would never come, and the whole transfer ran at one
window per retransmission timeout: **8.2 KB/s**. Clearing the flag before
the send instead cannot lose one.

Counters were what found it — over one transfer the tick ran 416 times
and failed to send *none*, yet only five acknowledgements left the
machine for fourteen segments. `net` and `fetch` still print them.

**2,000,000 bytes in 110ms, byte for byte.** Two related fixes went with
it: `http.c` acknowledged before reading, so every window advertised was
the one the sender had just filled rather than the one the reader had
just opened; and `rx_drain()` is reached from both the interrupt and the
poll, which share the ring's read pointer.

## Milestone 40 — the OS updates itself ✅ DONE

`update apply <manifest-url>`, reboot, and the machine is a new version.

The manifest is plain text — `key = value`, one per line — naming a
kernel and an initrd with a size and a checksum for each. Both are
downloaded and both are verified **before either is written**, because
the failure being designed against is a machine with half an update on
it: a new kernel with an old initrd does not boot, and a machine that
does not boot cannot be updated again. The version marker is written
**last**, so its presence is what says the two images beside it are
complete.

GRUB reads FAT32 natively, so nothing has to be installed on the disk:
`search --no-floppy --file --set=root /boot/version` finds the disk's
copy and boots it, and falls back to the disc's when there is none. The
disc stays the thing that boots; the disk holds what it boots *into*.

The checksum is FNV-1a 32 — a *hash*, not a signature. It catches a
truncated or corrupted download and nothing at all about who served it.
Over plain HTTP this trusts the network, and `include/update.h` says so.

### The test is the milestone

An updater is the one thing that cannot be tested by reading its own
output, because what it claims to have done and what it did are the same
sentence. `tools/tests/update_e2e.sh` builds the tree twice, serves the
newer build with a manifest, installs it from inside the guest, and boots
again — and the machine comes back up saying `This is Novaris Milestone
41 (version 41)`.

Writing it was what exposed **a quadratic FAT32**. `fat_write_raw` asked
`chain_nth` for each cluster and `chain_nth` walked from the head of the
chain, so a 48MB initrd was ninety thousand clusters squared — four
billion link steps for a file that arrived in a second. Reading had the
same line and the same shape; nothing in the tree had been big enough to
notice. `chain_nth` now remembers where its last walk ended, which is one
step per cluster for sequential access and a fallback to the walk for
anything else.

## Milestone 41 — the network, from inside a process ✅ DONE

Milestone 39 gave the machine TCP and Milestone 40 gave it an updater,
and both of them ran *in the kernel*. From inside a program - and
therefore from inside anything running under Wine - there was still no
network at all. README.md said so in as many words, which is how this
milestone got picked.

So: **AF_INET/SOCK_STREAM**, through the same `socketcall` ABI that
Milestone 28 built for AF_UNIX. `socket`, `connect`, `send`, `recv`,
`shutdown`, `getsockname`, `getpeername`, `close` and `poll`, backed by
kernel/tcp.c.

`userland/inet_test.c` is the proof and it is written to the same rule as
everything else in that directory: raw `int $0x80`, Linux syscall
numbers, `gcc -m32 -static -nostdlib`, linked against nothing that has
heard of Novaris. It speaks HTTP/1.0 by hand, runs on the Linux build
host, and prints the same lines on both machines:

```
connecting to 10.0.2.2:34797
[ok] socket
[ok] connect
[ok] peer port 34797
[ok] sent 52 bytes
[ok] HTTP/1.0 200 OK
[ok] read 579 bytes to end of stream
[ok] close
inet_test: a process opened a TCP connection
```

`make test-inet` is that run, against a server on the build host - which
QEMU presents to the guest as 10.0.2.2, its own gateway, so the test
needs nothing from the outside world.

### The bug that only a blocking syscall could have

The first version connected and then hung for ever, with no error and no
fault. The cause is one hex digit in `kernel/idt.c`:

```c
idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);   /* interrupt gate */
```

`int 0x80` is an **interrupt** gate, so the CPU clears IF on the way in
and every syscall body runs with interrupts disabled. For a syscall that
returns promptly that is invisible, and until now they all did. A syscall
that *waits* is a different matter, and it fails twice over: `hlt` with
IF clear is a halt nothing can end, and spinning instead would not help
either, because `pit_get_ticks()` only advances on a timer interrupt - so
the deadline meant to end the wait never arrives. Both loops waited for
ever.

The fix is local rather than global: interrupts go on for the duration of
a wait and back to how they were after. Changing the gate to a trap gate
would have fixed it too, and would have quietly changed the conditions
every other syscall in the kernel has always run under.

### And datagrams, which is what a resolver needs

`SOCK_DGRAM` too, over net.c's existing UDP ports: `sendto`, `recvfrom`,
`bind`, and a `connect` that connects to nothing and simply records where
send and recv should default to. A datagram that does not fit the
caller's buffer is truncated rather than kept, because that is what makes
it a datagram and not a stream.

The test is a **DNS query built by hand** - header, encoded name,
question - sent to QEMU's forwarder at 10.0.2.3 and read back:

```
[ok] udp socket
[ok] sent a DNS question for example.com
[ok] a reply from port 53 with 2 answer(s)
```

One datagram out and one back is the shape of every resolver ever
written, which is the point: the kernel has had a resolver since
Milestone 39, but a *program* could not ask anything until now.

Two bugs, both of which produced a plausible partial success rather than
an obvious failure:

- **A datagram send is the first packet to an address.** net_send_ip()
  returns short while ARP resolves, and the caller is expected to poll
  and retry - which TCP never had to notice, because its first packet is
  a SYN that gets retransmitted anyway. A resolver's first query is the
  first thing anyone sends to a nameserver, so it failed with "network
  down" against a network that was working perfectly.

- **`net_udp_bind` returns 1 for success, not 0.** Reading that backwards
  meant every real success looked like a failure: the loop kept trying
  ports until it had filled net.c's whole table, and then treated the one
  genuine failure as success. The query went out from a port that had
  never been bound and the answer was delivered to nobody. It looked
  exactly like a nameserver that did not reply.

### And the other direction: a server on Novaris

`bind`, `listen`, `accept`. tcp.c grew the passive half of the state
machine it had been missing - `TCP_LISTEN` and `TCP_SYN_RECEIVED`, a SYN
for a listening port answered with SYN+ACK, and the client's ACK
completing the handshake - so a connection can now be opened *to* this
machine and reach a program on it.

There is no backlog argument, because there is no backlog: a half-open
connection occupies an ordinary slot in tcp.c's connection table, so the
depth is however many of its four slots are free. A `listen(fd, 128)`
that promised more than the machine has would be a lie told in a
signature.

Testing it needed the harness to grow two options, and the awkwardness is
the point rather than an accident of tooling: QEMU's user-mode stack
blocks inbound connections, so `--hostfwd` forwards the guest's port to
the build host's loopback and `--host-connect` runs the client out
there. A connection has to be opened from somewhere that is not the
machine under test, or the handshake never has Novaris on the answering
side. `make test-listen`:

```
novaris> run inettest.elf listen 8080
[ok] bind
[ok] listening on 8080
[ok] accepted a connection from port 51942
[ok] received 20 bytes
[ok] replied
```

and on the host, `guest replied b'novaris\n'`.

### What is deliberately not here

- **No raw sockets, no IPv6.**
- **No `getaddrinfo`.** A process can now ask a nameserver itself, which
  is the hard half, but the parsing and the `/etc/resolv.conf` reading
  belong in a libc rather than in a kernel.
- **A blocking call still holds the whole machine.** There is one event
  loop and no other thread to turn the crank, so a read that waits is a
  read the desktop waits behind. Every one of them therefore takes a
  deadline: ten seconds to connect, thirty of no progress to fail.

### The part that was already true and nobody had checked

A Windows program under Wine can use the network, and could as soon as
this milestone landed. `userland/pe_test/winsock.c` is a mingw-built
`.exe` linked against `ws2_32` that does `WSAStartup`, `connect`, `send`
and `recv`; under Wine on Novaris it fetches a page and reads 579 bytes.

Wine's `ws2_32` implements `AFD` on top of ordinary BSD sockets, so the
Windows side was already written - it only ever needed the Unix
primitives underneath, which is what this milestone added. The write-up
above (and README.md) claimed the opposite for a while, on the strength
of a sentence that predated the socket layer.

One real bug stood between the two, and it was in this file's own work
rather than in Wine: the `socketcall` dispatcher sent `SENDMSG` and
`RECVMSG` to the AF_UNIX implementation whatever the socket was. Wine's
AFD writes with `send()` and reads with `recvmsg()`, so output took the
inet route and input landed in an empty Unix ring buffer - whose "nothing
here and no peer" answer is indistinguishable from end of stream. The
program connected, sent its request, and read `0 bytes to end of stream`,
which is precisely what a server that says nothing looks like.

`nsiproxy` and `NDIS` do still fail to start, and that is narrower than
it used to sound: they enumerate interfaces, addresses and routes through
NT device objects, which is a different job from carrying a connection.

## Milestone 42 — a window that resizes ✅ DONE (kernel half)

> **Status.** `make test-wm` passes, including a new third stanza that
> maximizes the window and asserts the program is told:
>
> ```
> novaris> run wmtest.elf 1200
>   [ok]   the buffer is at least the window
>   resized to 1280x728
> ```
>
> A 480x320 window became 1280x728 — 2.7x wider and 2.3x taller — through
> a mapping that was never moved, never remapped, and never unmapped.
> `make test-qemu` and `make test` still pass.
>
> The **Wine half is still unverified**: `make test-wine-gui` needs a
> built Wine tree, which this machine does not have yet.

Every milestone since 36 has carried the same note: the buffer does not
follow a resize. The reason given was that resizing a mapping means
unmapping frames out from under a process that is drawing into them,
which there is no safe moment to do.

That is true, and it is answered by not doing it. **The buffer is not the
window.** It is allocated once at the largest client area the window could
ever be given — the desktop work area, since the window manager never
makes a window bigger than that — and the client size is a *sub-rectangle*
of it. A row is `stride` pixels, which is the buffer's width; `w` and `h`
say how much of it is shown.

So a resize reallocates nothing, moves nothing, and unmaps nothing. It is
two stores and a `WM_EV_RESIZE`, and the mapping the process made at
startup is exactly as valid afterwards as it was before.

The compositor needed no work at all: `gfx_surface_t` has carried `pitch`
and `cap_h` since it was written, so `wmdev_paint` says
`src.pitch = s->alloc_w` and the existing blit clips correctly.

### What it costs, and why that is the trade

Memory. A 1280x800 desktop makes every window a 4MB allocation whatever
size it is shown at, and `WMDEV_MAX_SLOTS` is 16 — so the worst case is
64MB of a 192MB heap. That is a real cost, it is bounded, and it buys the
removal of the one thing that made resizing unsafe. Wine has one to three
real windows in practice, not sixteen.

### The compatibility break, which is the part to watch

`stride` is not optional and it is not backward compatible. The buffer is
now wider than the window **even on a window that has never been
resized**, so any client that indexes `px[y * width + x]` draws a
staircase from the first frame. There are two clients and both were
changed:

- `wine/winenovaris.drv/window.c` — `flush()` steps the destination by
  `data->stride`, and `window_create()` gets it from `WMIO_GETINFO`.
- `userland/wm_test.c` — a `stride` global replacing `W` in three places.

A third client written against the old protocol would compile and run and
draw nonsense, which is the failure mode worth knowing about. `WMIO_GETINFO`
is the only way to learn the stride; `WMIO_GETSIZE` still answers the
client area and cannot say anything about the buffer.

### And the driver follows a resize now, in both directions

- **The program resizes itself.** `WindowPosChanged` used to ignore a size
  change on purpose. It now updates `width`/`height` — and that is the
  whole change, because the buffer and the mapping are already right.
- **The user drags an edge.** `WM_EV_RESIZE` used to be traced and
  dropped. It now calls `NtUserSetWindowPos`, which is what makes win32u
  recreate the surface at the new size and send the program `WM_SIZE`.
  The re-entry terminates: `SetWindowPos` reaches `WindowPosChanged`,
  which changes bookkeeping and sends nothing back to `/dev/wm`.

### What the test actually proves

The picture is the assertion, and the stride is what it checks. Before the
resize `tools/check_wm_window.py` finds *a solid 476x316 block* at
(363,84); after maximizing it finds a solid 476x316 block at (2,34). The
program still draws 480-pixel rows, but the buffer under it is now 1280
wide — so those rows are no longer contiguous, and the only reason the
block comes out solid rather than as a diagonal staircase is that both
sides are using `stride`. A stride bug is not a subtle failure here; it
is a picture that looks like a venetian blind.

`kmalloc` of 4MB at window-create was the other worry and it is answered:
the window opens, on a 512MB machine, every run.

### What is still not verified

- **The Wine half.** `NtUserSetWindowPos` re-entry from `ProcessEvents`,
  the driver's stride in `flush()`, and `WindowPosChanged` following a
  size change have not been compiled, let alone run. That needs a built
  Wine tree.
- **A corner drag**, as opposed to Win-Up. Both go through the same
  `WM_EV_RESIZE` path in `wmdev_paint`, so the kernel side is the same
  code — but the drag has never been driven, for the reason in the next
  section.
- **The newly-exposed region.** Growing a window shows buffer contents
  the program has not drawn into. The test program does not repaint on
  resize, so the screenshot shows exactly that: a 480x320 picture in a
  1280x728 window. Wine's win32u recreates and flushes the whole surface
  on a size change, so it should not be visible there. "Should" is doing
  work in that sentence.

### Two things the test harness needed

Neither is Milestone 42, and both were found by trying to test it.

**A drag primitive.** `tools/qemu_test.py` could click and move but not
press-move-release, so a corner drag was not expressible. It is now:
`--post-click "X,Y,drag,TX,TY"`. The subtlety is that it cannot be built
out of separate down/move/up steps, because every other gesture positions
the pointer by slamming it into the top-left corner and counting deltas
out from there — Novaris's mouse is a relative PS/2 device and there is no
other way to know where the pointer is. Doing that with the button held is
a drag *to the corner*, which collapses the window to its minimum on the
way past. So the drag form tracks the position and moves relative to it.

**And then the drag was not used.** `RESIZE_BORDER` is 5 pixels
(`kernel/wm.c:63`), and corner-slam-plus-deltas is accurate enough to hit
a window and not accurate enough to hit five pixels of one. Two attempts
landed in the client area and were delivered to the program as ordinary
mouse input — which is what `input reached the window` in the first
stanza means, and it was quietly true while the resize assertion failed.
So the test maximizes with `Win-Up` instead, through a new `--post-key`,
and needs no pointer precision at all. The drag primitive stays because it
is the right way to test a *drag*; it is not the right way to test a
resize.

## Milestone 43 — more of Wine ships ⚠️ PARTLY VERIFIED

> **Status.** Three levels of confidence, kept apart on purpose.
>
> The **plumbing** is verified against a mock tree: the drift, the `.drv`
> naming bug, the automatic Unix half, the reporting.
>
> The **list** is verified against a real Wine 11.0 source tree
> (`db11d0f`, "Release 11.0"): all twenty-one added modules exist, and
> which of them have Unix halves is now known rather than guessed — see
> below, because the answer changed the design.
>
> What is **still unverified** is a boot. Whether these DLLs load, and
> what they do to wineboot, needs a *built* Wine, and building one needs
> `libfreetype-dev:i386`, which was not installed on this machine.

### Three bugs, none of them in the list

The list was the intended work. Getting to it turned up three things that
had been wrong for several milestones, and all three were invisible for
the same reason: every copy in this script ended in `2>/dev/null || true`,
so a file that was asked for and not found produced silence.

**A second copy of the list, in the Makefile.** `WINE_PE_DLLS`,
`WINE_PE_PROGS` and `WINE_UNIX_DLLS` were defined at `Makefile:407` and
read by nothing. They had already drifted from the real list in
`tools/install_wine.sh` — the script had gained comctl32, comdlg32,
winspool.drv and five programs; the Makefile copy had not. The way that
drift would have been found is somebody adding a DLL to the Makefile,
rebuilding, and finding the initrd unchanged. Deleted rather than wired
up: the script runs standalone and has to hold the list anyway.

**`winspool.drv` had never been installed.** Wine builds a `.drv` module
under its own name — `dlls/winspool.drv/i386-windows/winspool.drv`, no
`.dll` on the end — and the loop appended `.dll` unconditionally, so it
looked for a file that cannot exist. It had been in the list since
Milestone 37 and had never once shipped.

**`winex11` had never been installed either**, for the same reason
(`winex11.drv`), and is now removed rather than fixed: Novaris configures
Wine `--without-x` and has its own display driver, so spelled correctly
there would still be nothing to install.

So the real count before this milestone was **31** DLLs, not the 33 that
`README.md` and Milestone 37 both claimed. Two of the 33 were names that
resolved to nothing.

### The Unix half is discovered now, not listed

`UNIX_DLLS="win32u ws2_32 bcrypt"` is gone. Every builtin that ships gets
its Unix half if the tree built one, worked out from the tree — the same
reasoning as the "ask the binaries" loop that already staged the host
libraries, applied one level up.

This is the failure this repository has hit most often and paid most for:
Milestone 31 records `ws2_32.so` being built and not copied, read as "no
networking, so ws2_32 cannot work", costing two milestones. A list that
has to be kept in step with another list is how that happens.

**And it turned out not to be optional.** Asked of the real Wine 11.0
tree, exactly eight of the modules that ship have a `UNIXLIB`:

| module | Unix half |
| --- | --- |
| ntdll | `ntdll.so` (the loader; installed separately) |
| win32u, ws2_32, bcrypt | as before |
| **winspool.drv** | **`winspool.so`** |
| **crypt32, dnsapi, netapi32** | **new in this milestone** |

Three of the five DLLs added in tier 4 have Unix halves. Under the old
hardcoded `UNIX_DLLS="win32u ws2_32 bcrypt"` they would have shipped as
PE only — which is not "that feature is missing", it is a DLL whose
process attach fails and takes wineboot down with it. Adding them to the
list without this change would have reproduced Milestone 31 exactly.

**And the Unix half's name cannot be derived from the module's.**
`winspool.drv`'s is `winspool.so`, dropping an extension that `ws2_32.so`
keeps. So the name is read out of the module's `Makefile.in`, where Wine
writes it, rather than guessed — verified against all eight above.

### And the script says what it did

Every run now prints which builtins came with both halves, which came
with one, and — loudly — which were asked for and not found in the tree.
A name in the "PE only" line is not necessarily wrong, since most of
Wine's DLLs genuinely have no Unix half. A name that *moves into* it
between two builds is. This is what found two of the three bugs above,
about a minute after it was written.

### The list: 31 DLLs to 52, in five tiers

Tiers because they are how to bisect this. A builtin that is absent fails
only for the program that imports it; one that is *present* and cannot
initialise takes its whole process down, and shell32 and explorer will
use a DLL if they find one. If a boot that worked stops working, drop the
last tier and try again.

| Tier | Added | Why |
| --- | --- | --- |
| 2 | uxtheme, usp10, oleacc, msimg32, riched20, riched32 | what comctl32 reaches for, and the edit control anything bigger than Notepad uses |
| 3 | windowscodecs, gdiplus, propsys, urlmon | decoding a picture and drawing one |
| 4 | crypt32, winhttp, dnsapi, iphlpapi, netapi32 | the network above a socket, which Milestone 41 stopped one layer short of |
| 5 | winmm, psapi, imagehlp, cabinet, wintrust | what installers and long-lived programs expect to find |

`winmm` is there for `timeGetTime` and the multimedia timers, not for
audio: this machine has no audio driver at all.

`iphlpapi` is the one to watch. It answers through `nsiproxy.sys`, which
Milestone 41 records as still failing to start, so it may well not work —
shipping it is how that gets diagnosed rather than assumed, and a program
that only *asks* about interfaces failing is much narrower than one that
cannot connect.

Four programs were added too: `wordpad`, `winefile`, `taskmgr`, `winver`.
wordpad and winefile are the point — real GUI programs, bigger than
Notepad, that resize and use the controls tier 2 added, which makes them
the first honest test of Milestone 42 as well.

### What to check when this is built

1. **The install report.** It is printed by every `make` with
   `WINE_BUILD` set. Anything under "NOT FOUND" is a DLL this milestone
   claims to ship and does not.
2. **`make test-wine` and `make test-wine-gui` still pass.** These are
   regression tests now: twenty-one new DLLs is twenty-one new chances
   for a process attach to fail, and the console test is the canary.
3. **Then `wine wordpad.exe`**, which is the first program here that
   exercises Milestone 42 and tiers 2-3 at once.
4. **The initrd size.** It is read into RAM whole. 52 DLLs is a lot more
   than 31 and nobody has measured what that does.

## Milestone 44 — 64-bit: long mode, descriptors, interrupts ⚠️ IN PROGRESS

The port to x86-64, started because the target is `chrome.exe` and every
other blocker in front of it is downstream of this one. Milestone 37's
list said a browser needs a 64-bit Wine, a GPU, an NT sandbox and far more
of Wine's DLLs; of those, the architecture is the one everything else has
to be rebuilt on top of, so it goes first.

**What is done and booted.** Three layers, each verified by running it
rather than by reading it:

- **Long mode.** `boot/boot64.s` takes GRUB's 32-bit protected-mode
  handoff, builds a PML4/PDPT/PD with 2MB pages, and performs the switch
  itself: CR4.PAE, CR3, EFER.LME, CR0.PG, then a far jump through a
  descriptor with the L bit set. Multiboot 1 is kept - nothing about a
  64-bit kernel needs Multiboot 2, it only changes who does the switching.
- **The higher half moved.** 0xFFFFFFFF80000000, not 0xC0000000, because
  `-mcmodel=kernel` lets gcc assume every symbol is reachable by a
  sign-extended 32-bit displacement. That is true only in the top 2GB, and
  linking elsewhere under that model miscompiles silently.
- **Descriptors.** `gdt64.c` - a GDT whose code/data base and limit the
  hardware ignores, a 16-byte TSS descriptor, and the IST.
- **Interrupts.** `idt64.c` and `isr64.s` - 16-byte gates, fifteen
  registers pushed by hand, `iretq`, and the PIC EOI in the dispatcher.

`make -f Makefile.amd64 test` boots the ISO and asserts 19 things about
the machine's actual state. It is deliberately not a banner test: a
bootstrap that silently stayed in compatibility mode prints too. EFER.LMA
is the bit the CPU sets itself when long mode goes active, and the test
fails on that rather than on any claim the kernel makes about itself.

### Two bugs that cost the session, both worth reading

**NASM has no `lretq`.** That is a gas spelling, and NASM treats an
unrecognised mnemonic as a *label definition* - so it assembled to zero
bytes and `gdt64_flush` fell through into `idt64_flush`. The only
complaint was a "label alone on a line without a colon" warning, which is
now `-w+error=label-orphan` in ASFLAGS: any 64-bit mnemonic NASM does not
know will stop the build rather than delete an instruction.

**A label's address is not a resume point at -O2.** The page-fault test
first used GCC's labels-as-values (`&&recovered`) and had the handler set
`rip` to it. The optimiser duplicates and reorders basic blocks, so the
address resolved to a *copy* of the block, reachable with different
register state; resuming there re-entered an earlier test and leaked a
stack frame per iteration until the stack overflowed and the machine
triple-faulted. Both the faulting instruction and the recovery point are
real symbols in `isr64.s` now - the same shape as the exception tables a
Unix kernel keeps for exactly this purpose.

### Why a separate Makefile.amd64

The 32-bit tree is the only thing that currently works end to end, and
this port is long. Breaking `make` on day one would mean no working
reference to test against for as long as it lasts. `Makefile.amd64`
collapses back into `Makefile` when the 64-bit tree can do what the
32-bit one can.

### What is not done

Everything else, and it is most of the kernel: roughly 40,000 lines and
six more assembly files. In dependency order, each of these milestone-sized
on its own:

1. ~~**Paging and the PMM**~~ - done in Milestone 45. `pmm64.c` and
   `paging64.c`, four levels, and the recursive-mapping trick reworked
   for a PML4.
2. **The heap, kstring, and the drivers** - the heap and kstring are done
   in Milestone 46; **the drivers are not**. Port I/O is already known
   portable; `serial64.c` exists only because the real serial driver
   pulls in the console and the framebuffer, and that is still true.
3. ~~**Processes, the scheduler and ring 3**~~ - done across Milestones
   47-49: ring 3 and `syscall`/`sysret` (47, and why the GDT's user
   selectors are ordered as they are), address spaces (48), and
   preemptive round-robin scheduling from the timer (49). No
   `process_asm.s` or `scheduler_asm.s` was needed - the interrupt frame
   already *is* the saved context. What is still missing is a per-task
   kernel stack, which is what a task that blocks inside a syscall
   needs; see Milestone 49.
4. **The syscall ABI** - Milestones 50-51. A real static Linux x86-64
   executable runs (50), and then **real glibc** runs (51): `printf`
   through stdio, `malloc`, TLS via `arch_prctl`, the auxiliary vector,
   all byte-identical to the same binary on the host. Around twenty
   calls of ~350, and no threads, no signals, no filesystem.
   Novaris implements *Linux's i386 syscall ABI*, which is what lets real
   glibc and real Wine run unmodified. x86-64 Linux has different syscall
   numbers, a different register convention, and different structure
   layouts. Milestones 19-31 all get re-earned here.
5. **PE32+ and a 64-bit Wine** - **PE32+ is done** in Milestone 52: a
   real mingw-built 64-bit Windows executable loads, binds its kernel32
   imports and runs in ring 3. Wine itself is untouched and still needs
   reconfiguring `--enable-archs=x86_64`, or both for WoW64.

Item 4 is the honest sting in this milestone. The 32-bit tree's ability to
run unmodified Linux binaries came from implementing one specific ABI very
carefully across a dozen milestones, and none of that work transfers.

## Milestone 45 — 64-bit: frames, four levels, and what Chrome actually needs ✅ DONE

Two things, one on each side of the gap: the next layer of the 64-bit
kernel, and a measurement of how far the *other* end of the problem
really is.

### The kernel half

`pmm64.c` and `paging64.c`, both booted and asserted rather than read.
`make -f Makefile.amd64 test` now checks 56 things about the machine, up
from 23, and the new 33 are the interesting ones.

(Milestone 44 above says the test asserts 19 things. It prints 23, and
did before this milestone touched it - counted from the serial log of a
pristine Milestone 44 build, `599643e` on origin/main. The 19 was
already wrong, and is left standing there rather than quietly edited,
since the count in *this* section is the one now checked against the
log. Worth knowing while comparing hashes: local `main` is `e399a85`
and origin/main is `599643e`, and `git diff` between them is empty -
same tree, rewritten history.)

**The frame allocator places its bitmap instead of declaring it.** The
32-bit `pmm.c` sizes a bitmap for a full 4GB address space at compile
time - 128KB of `.bss` - and stops worrying. That does not survive the
port: a 64-bit machine can hold more RAM than that bitmap describes, and
sizing statically for the architectural maximum means one bit per 4KB
frame across 2^52 frames, which is half a petabyte of bitmap. So the
bitmap is sized from the memory map at run time and placed immediately
above the kernel image, inside the one gigabyte `boot64.s` maps at
KERNEL_VMA. Init checks that it fits there rather than assuming it, and
reports itself not ready if it would not.

The arithmetic is checkable from the test's own output, which is why it
prints: 32736 frames for QEMU's default 128MB, 32455 of them free. The
281-frame difference is 256 (the first megabyte, never handed out) + 24
(the 96KB kernel image) + 1 (the bitmap). If any of the three reserve
passes were wrong that sum stops working.

**Four levels, and the recursive map reworked.** With PML4 slot 510
pointing at the PML4 itself, a hardware walk spends one of its four steps
going round the self-reference, so it ends one level early - at a page
table rather than a data page. Repeat the slot twice and it ends at a
page directory, three times a PDPT, four times the PML4. That gives every
table an address without needing a physical-to-virtual mapping to already
exist, which matters more here than it did at 32 bits: a newly allocated
page table can sit anywhere the frame allocator chooses, including above
the gigabyte `boot64.s` mapped, and the recursive address reaches it
anyway. A new table is installed into its parent *first* and zeroed
through that address second, because it has no address until the parent
entry exists.

Slot 510 is free because slot 0 is the boot identity map and 511 is the
higher half; being >= 256 also makes every derived address canonical
without further care.

**What the paging test proves, as opposed to observes.** Mapping a page
and reading back what you wrote to it passes even if the mapping is
nonsense, so the test also reads the same physical frame through the
KERNEL_VMA window and checks the bytes are there. Unmapping is checked by
faulting on the address afterwards - reusing the probe/recover symbol
pair from Milestone 44 - not by asking the code whether it thinks it
unmapped something.

The test was also falsified before being believed: with the kernel-image
reserve pass removed, `no frame overlaps the kernel image` fails, and the
machine then dies before the end of `kernel_main`, because a kernel that
hands out its own frames corrupts itself. That is the failure that pass
exists to prevent, and it is now known to be observable.

**Deliberately not done: splitting huge pages.** The higher half is 2MB
pages, and mapping a 4KB page into one needs the huge page broken up
first. `paging64_map` returns `PAGING64_HUGE_IN_WAY` instead, and the
test asserts it does. Refusing is honest; silently overwriting the entry
would not be.

**Deliberately not done: a second address space.** Recursive mapping
edits the *current* one only. Editing another process's tables needs a
temporary mapping or a second recursive slot, and that belongs to
whichever milestone first has two address spaces - item 3 below.

### The Chrome half, measured rather than guessed

`tools/pe_imports.py` reads a PE (32- or 64-bit), lists every DLL and
function it imports - the ordinary import table *and* the delay-load
table, which is where Chrome keeps user32, gdi32 and the graphics stack -
and answers, from Wine's own `.spec` files, which of them Wine can
supply. It was checked against `objdump -p` on chrome.exe and agrees
exactly on the static imports (231/5/3/2 across the four DLLs objdump
sees).

Getting a `chrome.exe` to point it at is four layers of nested archive,
and worth writing down because none of it is guessable:

```bash
curl -L -o chrome64.exe \
    https://dl.google.com/chrome/install/standalonesetup64.exe
7z x chrome64.exe -oA        # -> updater.7z
7z x A/updater.7z -oB        # -> bin/Offline/{...}/<ver>_chrome_installer.exe
7z x B/.../<ver>_chrome_installer.exe -oC   # -> chrome.7z
7z x C/chrome.7z -oD         # -> D/Chrome-bin/chrome.exe
```

The outer installer is a 32-bit `UpdaterSetup.exe`, which is why `file`
says i386 on it and means nothing about the browser inside.

Run against the real thing - Chrome 151.0.7922.138:

| binary | DLLs | imported functions | no Wine export anywhere |
|---|---|---|---|
| `chrome.exe` | 16 (12 delay) | 360 | 2 |
| `chrome.dll` | 67 (59 delay) | 1316 | 8 |

**The headline is that Wine's Win32 coverage is not the blocker.** Of
1316 functions `chrome.dll` imports, Wine has an export for all but 25,
and 17 of those 25 are `chrome_elf.dll` - Chrome's own DLL, which ships
inside the install and is not Wine's to provide. The genuine gaps are
eight, all delay-loaded, all on optional paths:
`AddConditionalAce` and `DeriveAppContainerSidFromAppContainerName`
(the sandbox), `GetPointerDevice` (touch), `UiaDisconnectAllProviders`
(accessibility), three `Ndf*` (network diagnostics), `PowerReadACValue`.

Getting that number right took two corrections, both worth keeping:
API-set names (`api-ms-win-core-synch-l1-2-0.dll` and friends) resolve
through kernelbase/ntdll/combase rather than existing as DLLs, and a
`.spec` line that is commented out (`# @ stub AddConditionalAce`) is not
an export. Before the first fix the tool claimed 13 gaps in chrome.exe
where there are 2.

The tool also diffs against `tools/install_wine.sh`, since that file is
the only place the ship list lives. Chrome needs **28 DLLs that Wine has
and Novaris does not currently install** - mfplat, secur32, ncrypt,
esent, hid, dbghelp, d3d9/11/12, dxgi, dwmapi, dcomp and the rest. That
is a list of names, not a research problem.

### What this changes about the plan

Milestone 37 listed four things between here and a browser: 64-bit, a
GPU, an NT sandbox, and more of Wine's DLLs. Two of those just got much
better understood.

- **Chrome is 64-bit only.** The question ROADMAP.md left open - whether
  a 32-bit chrome.exe still exists to fall back on - is answered: the
  shipping installer contains a PE32+ x86-64 `chrome.exe` and a 285MB
  x86-64 `chrome.dll`. There is no 32-bit build. The port is not the
  preferable path, it is the only one.
- **The DLL count is the smallest of the four items, not a large one.**
  It is 28 names in a shell variable, and the API surface behind them is
  already implemented.

Which leaves the architecture as the gate, exactly as Milestone 44 said,
and sharpens why: items 2-5 of Milestone 44's own list - the heap and
drivers, processes and ring 3, the syscall ABI, PE32+ - are still all of
the work. Item 4 remains the sting. Novaris implements *Linux's i386
syscall ABI*, which is what lets real glibc and real Wine run unmodified;
x86-64 Linux has different numbers, a different register convention and
different structure layouts, and Milestones 19-31 get re-earned there.

## Milestone 46 — 64-bit: a heap that stays fast, and one kstring ✅ DONE

Milestone 44's item 2, minus the drivers. `make -f Makefile.amd64 test`
checks 79 things now, up from 56.

### The heap is a rewrite, not a port

`kheap.c` keeps one singly-linked list of every block, used and free
alike, and `kmalloc` walks it from the start. So an allocation costs the
number of allocations before it, and N allocations cost N squared. That
is not a theoretical objection - Milestone 43 measured Wine's syscall
rate collapsing by twenty-one times partway through startup and traced
it here. Porting that faithfully would have carried the bug to the
architecture that is supposed to run a browser.

`kheap64.c` keeps two structures instead:

- blocks doubly linked in **address order**, which makes coalescing a
  freed block with its neighbours O(1) rather than a rescan of the whole
  heap (`kfree` in the 32-bit version walks the entire block list on
  every single free);
- free blocks additionally on a **free list**, which is the only thing
  `kmalloc64` walks.

The free-list pointers live in the payload of the free block itself, so
they cost nothing in a block that is in use. That is the only reason the
minimum allocation is 16 bytes.

**The measurement, printed by the test rather than claimed here:**

```
NOVARIS64: walk = 1000 steps for 1000 allocations (quadratic would be ~500000)
NOVARIS64: heap = 0 used / 200704 mapped, 1 free blocks
```

One step per allocation. And the second line is the coalescing result:
after a thousand allocations and frees plus a 128KB allocation that grew
the heap, the whole thing collapses back to **a single free block** with
nothing in use. No fragmentation left at all.

Coalescing is checked from outside rather than taken on trust - the test
allocates three adjacent blocks, asserts the exact 96-byte layout that
makes them adjacent (32-byte header, 64-byte payload), frees them in a
deliberately awkward order (middle, first, last), and requires the free
list to return to the length it had before. Falsified, as usual, before
being believed: with the two merge lines removed from `kfree64`, that
assertion fails and the free-block count goes from 1 to **1006**.

There is no console in the 64-bit tree, so heap exhaustion returns NULL
and shows in the counters rather than printing. The 32-bit version prints
a warning here, and that dependency on `console.c`/the framebuffer is
one of the things that made it non-portable.

### kstring is now shared, not forked

`kstring.c` compiles for both architectures and is in both builds. The
only change it needed: size parameters became `size_t` instead of
`uint32_t`. On i386 those are the same type, so the 32-bit build is
byte-for-byte unaffected in behaviour; on x86-64 a `uint32_t` length
would have silently capped every copy in the kernel at 4GB.

`kdiv64`/`kdiv_hi_lo` were the part expected to be i386-only, and are
not: they wrap a 32-bit `divl`, which is perfectly legal in long mode.
They are pointless there - x86-64 divides 64-bit values natively, and
they only exist because a freestanding i386 kernel cannot call libgcc's
`__udivmoddi4` - but they still have to be *correct* if they are going
to be compiled, so the test checks one division.

**Verified no regression on the 32-bit side**, since `kstring.c` is used
tree-wide: a clean `make` produces zero warnings and zero errors under
`-Wall -Wextra`, `make test` passes (64 filesystem checks, 52 window
manager checks, 0 failures), and `make test-qemu` passes all 17
transcript assertions.

### What is left of Milestone 44's list

Items 3, 4 and 5, and they are the large ones: processes/scheduler/ring
3, the syscall ABI, and PE32+. The heap and kstring were the portable-C
part of the port; everything remaining is architecture-specific by
nature. Item 4 is still the sting - Novaris implements Linux's *i386*
syscall ABI, which is exactly what lets real glibc and real Wine run
unmodified, and none of that transfers to x86-64.

## Milestone 47 — 64-bit: ring 3, SYSCALL, and the IRQ bug it uncovered ✅ DONE

The mechanism half of Milestone 44's item 3. `make -f Makefile.amd64
test` checks 88 things now, up from 79, and one of them is a bug that had
been sitting in the tree since Milestone 44.

### The bug: IRQ0 was still arriving as a double fault

`idt64_install` has always installed its IRQ gates at vectors 32-47. But
nothing ever remapped the 8259 pair, so the hardware was still delivering
on the defaults the BIOS leaves behind - **IRQ0 at vector 8**, which is
`#DF`. The gates were written for a remap that had never been performed.

No previous milestone noticed because none of them ever set IF. This one
does, on the way into ring 3, and the failure is worth recording because
of how it presented: an intermittent double fault whose register dump was
shifted by exactly one quadword.

```
NOVARIS64: *** unhandled exception: double fault (vector 8)
NOVARIS64:   err=0x0000700000000000 rip=0x0000000000000023 cs=0x0000000000000202
```

`err` is the user rip, `rip` is the user CS, `cs` is the user RFLAGS. The
handler for vector 8 pops an error code because a real `#DF` pushes one;
a hardware interrupt does not, so every field reads one slot low. **A
shifted exception dump is the signature of an IRQ arriving on an
exception vector**, and it is worth knowing on sight.

It was intermittent because it only fired if a timer tick happened to
land inside the few microseconds spent in ring 3 - the same code passed,
then failed, then passed.

`pic64_remap()` now runs at the end of `idt64_install`, after the table
is loaded so that an already-asserted line lands on a gate that exists.
Every line is left **masked**: the 64-bit tree has no drivers, so an
unmasked line can only produce an interrupt nobody will service.
`idt64_irq_set_mask()` unmasks one line at a time, and unmasking anything
on the slave unmasks IRQ2 with it, since that is the line it cascades on.

The test proves the remap rather than assuming it: it programs the PIT to
1kHz, unmasks IRQ0, sets IF, and requires three ticks to arrive **at
vector 32**. Before the fix that first tick was a `#DF` and the machine
halted, so reaching the assertion at all is half the result.

### Ring 3, and a syscall that comes back

`int 0x80` is not how a 64-bit kernel is entered. SYSCALL/SYSRET do no
descriptor lookup and - the part that has to be handled by hand - **no
stack switch**: on entry `rsp` is still the ring-3 stack, so the entry
stub cannot push anything until it has moved off it. SYSCALL also
destroys `rcx` and `r11`, which is where it puts the return rip and
rflags, so both are preserved by hand.

The selectors come from MSRs rather than the IDT, computed from one base
by fixed offsets, which is *why* `gdt64.h` orders the user pair
data-before-code:

```
SYSCALL:  CS = STAR[47:32],      SS = STAR[47:32] + 8
SYSRET :  CS = STAR[63:48] + 16, SS = STAR[63:48] + 8   (both forced RPL 3)
```

STAR[47:32] = 0x08 gives kernel 0x08/0x10; STAR[63:48] = 0x10 gives user
0x23/0x1B. Reordering the GDT silently changes which selectors land here,
which is the kind of coupling worth writing down next to both.

A multiprocessor kernel does the stack switch with `swapgs` and a per-CPU
block. This one is single-CPU during bring-up and uses RIP-relative
globals; `swapgs` is what that becomes when there is more than one CPU to
tell apart.

**How the test avoids proving nothing.** The ring-3 program reads its own
`CS` and hands it to the kernel. That is the one value ring 0 could not
have produced - the same code at ring 0 reports 0x08 - and it is checked
to be 0x23, the user code selector with RPL 3. It then feeds the value
the kernel returned into its exit call, so the recorded exit code
(0x1134 = 0x23 + 0x1111) also proves SYSRET went *back* to ring 3 rather
than the program never resuming.

Falsified before believed, as usual: entering with kernel selectors
instead of user ones makes it report `user cs = 0x08` and fails both
assertions.

The boot test was run six times after the fix and passed six times; the
intermittency is gone, because IRQ0 is masked again before the ring-3
section so that section does not depend on tick timing.

### What is deliberately not done

- **An interrupt taken while in ring 3** - the path that switches to the
  TSS's `rsp0`. The `rsp0` is set and correct, but nothing has yet been
  interrupted while at CPL 3. The scheduler is what needs that path and
  what should test it.
- **A syscall ABI.** This is the mechanism only: two made-up call numbers
  and one argument. Novaris's 32-bit kernel implements Linux's *i386*
  ABI - the numbers, the register convention, the structure layouts - and
  that is exactly what lets real glibc and real Wine run unmodified. None
  of it transfers. That rewrite is still the sting in item 4.
- **Processes.** One ring-3 program, entered from `kernel_main` and
  returning to it. No address spaces, no scheduler, no context switch.

## Milestone 48 — 64-bit: address spaces, and a program that lives in one ✅ DONE

The rest of Milestone 44's item 3 except the scheduler. 108 assertions
now, up from 88.

### One PML4 per process, sharing the kernel

The split is decided by the sign bit: PML4 slots 0-255 are the low half
and belong to the process, 256-511 are the high half and are the kernel,
identical in every space. That is what lets a syscall or an interrupt
find the kernel without touching CR3, and it is why creating a space is
mostly a copy of 256 entries.

Two details that are easy to get wrong and were not:

- **The self-reference must be the space's own.** `vmspace64_create`
  copies the kernel's high half, which includes slot 510 - the recursive
  slot - and then *overwrites* it to point at the new PML4. Leaving the
  copied value there would give every new space a recursive mapping onto
  the kernel's tables, so it would appear to work and would silently edit
  the wrong space.
- **A new PML4 cannot be reached recursively yet.** Until its
  self-reference exists there is no recursive address for it, so it is
  initialised through a scratch mapping and only then used normally.

### Editing a space you are not in

`paging64.c` reaches tables through the recursive slot, which by
construction describes only the current space. The textbook answer is a
second recursive slot pointing at the other space's PML4, with every
derived address shifted a level down.

This does the simpler thing: switch CR3, use the ordinary mapping code,
switch back. That is safe *because* of the split above - the kernel's
code, stack, heap and frame bitmap are all high-half and mapped
identically in both spaces, so the switch changes nothing the mapping
code is standing on. It costs two CR3 loads per edit, which matters while
a process is being built and never afterwards.

`vmspace64_destroy` frees the space's page tables but **not** the frames
they point at: this layer cannot know whether those are program pages, a
shared buffer, or something mapped from a device, so ownership stays
with whoever mapped them.

### What the tests establish

The isolation test maps *the same virtual address* in two spaces to two
different frames, writes a different value through each, and requires
each space to still read its own. Falsified by pointing both at one
frame, which makes A read B's value and fails exactly that assertion.

Frame accounting is checked rather than assumed: free frames before and
after creating two spaces, mapping in both, and destroying both come back
identical (32386 either side). One space is created and destroyed before
the count is taken, because the first create ever made allocates the
kernel-half tables for its scratch mapping and those are permanent and
shared - counting from before them would look like a leak that is not
one.

Then both halves at once, which is what a process actually is: a ring-3
program whose code and stack exist only in its own low half, entered with
CR3 pointing at its space. It runs, its `syscall` finds the kernel
through the shared high half, and it exits - while the same addresses
translate to nothing at all in the kernel's own space.

### Still missing before this is a scheduler

No context switch, no run queue, no saving of a process's registers - the
program is entered from `kernel_main` and returns to it. The path that
takes an interrupt *while at CPL 3* and switches to the TSS's `rsp0` is
set up correctly but still unexercised, and that is the piece a
preemptive scheduler is built on.

And item 4 is untouched and unchanged: this is the mechanism, not an ABI.
Novaris's 32-bit kernel implements Linux's *i386* syscall ABI, which is
exactly what lets real glibc and real Wine run unmodified, and none of it
transfers to x86-64.

## Milestone 49 — 64-bit: two tasks, preempted ✅ DONE

The end of Milestone 44's item 3. 115 assertions, up from 108.

### The context switch is a struct copy

There is no `process_asm.s` and no `scheduler_asm.s` in the 64-bit tree,
and there does not need to be, because of where the state already is.
When the CPU takes an interrupt at CPL 3 it switches to the stack in the
TSS and pushes the ring-3 `ss:rsp`, `rflags` and `cs:rip`; `isr64.s` then
pushes all fifteen general-purpose registers. By the time a handler runs,
**the entire user-visible state of the interrupted task is already laid
out in memory as a `registers64_t`**.

So switching tasks is: copy that struct into the outgoing task, copy the
incoming task's struct over it, load the incoming CR3, return. `iretq`
reloads from exactly the memory the handler just rewrote - the same
property Milestone 44 built the interrupt path around, and the reason
that milestone insisted on proving a handler could move `rip`.

This is also the first thing in the 64-bit tree to be interrupted *while
at CPL 3*, so it is the first real use of the TSS `rsp0` that Milestone
47 set up and could not exercise.

### Why it works, and where it stops working

It works because these tasks only ever run in ring 3 and are never
interrupted inside the kernel, so no task has a kernel stack that needs
preserving - one `rsp0` serves all of them. **A task that can block
inside a syscall needs its own kernel stack**, and that is the next thing
this has to grow. There are no priorities, no accounting and no run
queue: a fixed array walked in order.

### Ending the run without measuring the machine

The two tasks increment a counter forever and never exit on their own.
The scheduler stops the run by rewriting `rip` to an exit stub on a
resume, which works precisely because every other register - including
the pointer the task keeps its counter address in - is restored
untouched.

That makes the test end after a fixed number of **ticks** rather than a
fixed number of iterations, and the difference is the whole point: five
consecutive runs report `switches = 8` every time while the counters
range from 326,672 to 726,002 depending on how fast the emulator felt
like being. An iteration-counted test would have been a coin flip
between finishing before the first preemption and taking too long.

```
NOVARIS64: switches= 8, task0 = 591070, task1 = 719048
```

Both counters non-zero is the result. Each counter lives in a physical
frame reachable only from its own task's address space, mapped at the
*same* virtual address in both - so one task running would leave the
other at exactly zero. Falsified by making `sched64_tick` never pick a
different task: the first task then loops forever, the kernel never gets
control back, and the run dies on the harness timeout.

## Milestone 50 — 64-bit: a real Linux binary runs ✅ DONE

The beginning of Milestone 44's item 4. 123 assertions, and a second
PASS line, because this milestone's real result is a differential rather
than an assertion inside the kernel.

### What actually ran

`userland/hello64.s` is an ordinary static x86-64 Linux executable. It is
assembled and linked by the host toolchain, uses Linux's syscall numbers
and register convention, and knows nothing whatsoever about Novaris.
`file` calls it what it is:

```
ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked
```

The same file - not a rebuild, the same bytes - now runs in two places
and does the same thing in both:

```
$ ./build64/hello64.elf
hello from a real x86-64 ELF, loaded by Novaris
$ echo $?
42
```

```
NOVARIS64: --- its output follows ---
hello from a real x86-64 ELF, loaded by Novaris
NOVARIS64: --- end of its output ---
NOVARIS64: wrote   = 48 bytes, exit 42
```

`make -f Makefile.amd64 test` now runs the host copy itself and requires
its output to appear in the guest's serial log and its status to be 42.
That is a much stronger statement than "the guest printed something": it
says this kernel implements *Linux's ABI*, not something shaped like it.
Which is the entire reason the 32-bit tree can run real glibc and real
Wine, and the property the whole port exists to re-earn.

### The pieces

**`elf64.c`**, a separate reader rather than an ifdef through the 32-bit
`elf.c`. The two formats disagree about field *order* as well as width -
in ELF64 `p_flags` moves from second-to-last in the program header to
directly after `p_type`, so reading an ELF64 with ELF32 field offsets
takes the segment's alignment as its permissions and produces a mapping
that looks plausible.

It zeroes each page before copying into it, which is what gives a segment
its `.bss` (`p_memsz` past `p_filesz` is defined to read as zero), and it
reuses an existing mapping where two segments share the page one ends and
the next begins, rather than stranding the frame already there.

**Three syscalls, with Linux's numbers**: `write` (1), `exit` (60),
`exit_group` (231), and unimplemented calls answer `-ENOSYS` (-38)
because that is what programs check for. The entry stub shifts arguments
from Linux's `rdi, rsi, rdx` into the SysV registers the C is compiled
for, which it can only do after `rcx` is saved, since `rcx` arrives
holding the return address.

`write` dereferences a ring-3 pointer directly, which works because the
kernel is running in the caller's address space - the high half is
shared, so a syscall never leaves the space it was called from. **It is
also completely unchecked**, and that is the honest state of this ABI: a
real implementation validates the range is mapped and belongs to the
caller, without racing the caller. Nothing here does that yet.

### Two bugs found by trying to falsify the result

**The build had no header dependencies.** The falsification for this
milestone was to set `SYS64_WRITE` to 4 - the *i386* number - and check
the test noticed. It did not: the test passed. The reason was that
`Makefile.amd64` built objects from `.c` alone, so editing
`include/syscall64.h` rebuilt nothing at all and the change was never
compiled. `CFLAGS` now carries `-MMD -MP` and the generated `.d` files
are included.

With that fixed the falsification does what it should, and it is worth
recording what it looks like, because it is exactly the failure item 4
warns about:

```
NOVARIS64: FAIL  it wrote to stdout
NOVARIS64: wrote   = 0 bytes, exit 42
```

A real Linux binary, on a kernel that got one syscall number wrong,
silently prints nothing and exits successfully. Nothing faults. Nothing
says why.

### How far this is from glibc

Three calls out of roughly 350, and the program is deliberately built
`-nostdlib`. glibc's startup alone wants `arch_prctl` to set up the TLS
base, `brk` or `mmap`, `set_tid_address`, `rseq`, `readlink` on
`/proc/self/exe`, `getrandom` for the stack guard, and an `auxv` on the
initial stack that nothing here constructs yet. That, and not the three
above, is the shape of the remaining work in item 4.

## Milestone 51 — 64-bit: real glibc runs ✅ DONE

132 assertions and a third PASS line. `userland/hello_glibc64.c` is an
ordinary C program - `printf`, `malloc`, `strcpy` - compiled by the host
with `gcc -static -O2` and never told what it would run on:

```
NOVARIS64: --- its output follows ---
hello from glibc on Novaris
malloc ok, 1..10 sums to 55
NOVARIS64: --- end of its output ---
NOVARIS64: wrote   = 56 bytes, exit 7, pages 34, enosys 3
```

Byte-identical to the same file run on Linux, exit status included, and
`make -f Makefile.amd64 test` now checks that line by line rather than
taking it on trust.

The difference between this and Milestone 50 is the whole point.
`hello64.s` issues three syscalls by hand. This one goes through glibc's
startup first: it sets up thread-local storage, reads its own program
headers out of the auxiliary vector, initialises malloc, and builds a
buffered stdio stream. glibc was compiled long before Novaris existed and
none of those steps are negotiable.

### The bug that mattered

Linux's syscall ABI preserves **every** register except `rax`, `rcx` and
`r11`. The C dispatcher is under no such obligation - SysV lets it
destroy `rdi`, `rsi`, `rdx`, `r8`, `r9` and `r10` - so the entry stub has
to put them back. The first version did not.

What that looks like from the outside is worth recording, because it
points nowhere near the cause: the *first* syscall returns a correct
value, and the **second** one arrives with a nonsense number and a kernel
address for an argument.

```
[call] 12(0x0, 0xc, 0x1f) = 0x4ac000          <- brk(0), correct
[call] 2149396368(0x4acd40, 0xffffffff801d2f90, ...)  <- registers gone
```

The program had been resumed with its registers quietly rearranged and
ran off into nonsense; the syscall layer was simply the first place that
became visible.

### Two more, found the same way

**A silent hang with nothing on the serial port.** The first glibc
attempt produced no output and no fault message at all. The cause was the
page-fault handler left over from Milestone 45's tests, which resumes
execution at a *kernel* recovery address - from CPL 3 that faults
immediately, forever, and never escalates to anything that prints.
There is now a `pf_diagnose` mode that reports `cr2`, `rip`, the error
code and `cs` and halts, which is what turned the hang into
`cr2=0xdb8 rip=0x403fd7 err=6` and then into a one-line disassembly of
`__libc_setup_tls`.

**The kernel was compiled with SSE.** Linux builds itself `-mno-sse`
precisely because its syscall ABI preserves xmm registers and glibc's
string and memory routines keep live data in them across a call. A kernel
that lets gcc emit an xmm move for an ordinary struct assignment corrupts
its caller occasionally and unreproducibly. `CFLAGS` now carries
`-mno-sse -mno-mmx`, and `objdump -d build64/novaris64.bin | grep -c xmm`
is 0. `boot64.s` still enables CR4.OSFXSR, because ring 3 does use SSE.

### What glibc actually asked for

Discovered by running it, since there is no strace here -
`syscall64_set_trace(1)` prints every call and its result, and that is
how the list below was found rather than guessed:

`brk` (four times), `arch_prctl(ARCH_SET_FS)`, `set_tid_address`,
`set_robust_list`, `getrandom`, `mprotect` (for RELRO), `fstat` on fd 1,
`ioctl` (the `TCGETS` that asks whether stdout is a terminal),
`writev`/`write`, `exit_group`.

Three calls answer `-ENOSYS` and glibc carries on regardless: `rseq`
(334), `prlimit64` (302) and `readlinkat` (267). That is not an
accident - they are all optional paths with fallbacks, and answering
-ENOSYS rather than something plausible is what lets glibc take them.

Two of the implementations are deliberately dishonest and say so in the
source: `getrandom` is a deterministic PRNG, because a kernel with no
entropy source that pretends otherwise is worse than one that admits it
(it makes glibc's stack guard predictable), and `mprotect` accepts
everything and does nothing, since nothing yet depends on *removing*
permissions.

`write` and `writev` dereference ring-3 pointers **entirely unchecked**.
A real implementation validates that the range is mapped and belongs to
the caller, without racing the caller. That is the largest known hole in
this ABI.

### How far this is from Wine

Still no threads (`clone`), no signal *delivery*, no filesystem, no
`mmap` of a file, no dynamic loader. Wine needs all of them. But the
thing Milestone 44 called the sting - that none of the 32-bit tree's ABI
work transfers - now has its first counter-example: the mechanism is
real, and a real libc runs on it.

## Milestone 52 — 64-bit: a Windows executable runs ✅ DONE

141 assertions. `userland/hello_pe64.c` is built by
`x86_64-w64-mingw32-gcc` into an ordinary PE32+ image - the format
chrome.exe is in - importing `GetStdHandle`, `WriteFile` and
`ExitProcess` from kernel32, linked at 0x140000000, with no idea Novaris
exists:

```
NOVARIS64: base    = 0x0000000140000000
NOVARIS64: entry   = 0x0000000140001000
NOVARIS64: imports = 3, relocs 0, pages 7
NOVARIS64: --- its output follows ---
hello from a 64-bit Windows PE on Novaris
NOVARIS64: --- end of its output ---
NOVARIS64: wrote   = 42 bytes, exit 3
```

Milestone 30 ran a Windows program on the 32-bit kernel. This is the
64-bit equivalent, and it is the first thing in this tree that is the
same *kind* of object as the target.

### PE32+ is not PE32 with wider fields

`pe64.c` is a separate reader from `pe.c` for reasons that would each
have produced a plausible-looking wrong answer:

- the optional header's magic changes, and **`BaseOfData` disappears**,
  which shifts every field after it;
- `ImageBase` becomes 64-bit;
- the import thunk arrays go from 4 bytes to 8, and the
  import-by-ordinal flag moves from bit 31 to **bit 63**.

Base relocations are implemented (`IMAGE_REL_BASED_DIR64`) even though
this image carries none and loads where it asked. chrome.exe will not:
a loader that silently skips relocations produces a program that runs
until its first absolute address.

### The calling convention, and why imports are thunks

Windows x86-64 passes arguments in `rcx, rdx, r8, r9` with 32 bytes of
shadow space. This kernel is compiled for SysV and its syscalls take
`rdi, rsi, rdx, r10`. Rather than compile a Win32 layer twice, the
loader writes a 20-byte thunk per import:

```
48 89 CF   mov rdi, rcx     Windows arg 1 -> SysV arg 1
48 89 D6   mov rsi, rdx     arg 2, before rdx is overwritten
4C 89 C2   mov rdx, r8      arg 3
4D 89 CA   mov r10, r9      arg 4 - r10, since SYSCALL destroys rcx
B8 nn..    mov eax, number
0F 05      syscall
C3         ret
```

That also solves a problem that has nothing to do with registers: an
imported function has to be something a *ring-3* program can call. A
kernel address in the IAT would fault on first use. A thunk in a user
page that traps is the right shape.

**The move order is load-bearing**, and it was falsified to prove it:
putting `rdx <- r8` before `rsi <- rdx` loses the second argument, and
what that produces is `WriteFile` receiving 42 - the byte count - as its
buffer pointer, and the *kernel* faulting at address 0x2a while
dereferencing it.

Which is worth dwelling on, because it demonstrates the hole recorded in
Milestone 51 from the other side: `cs=0x08` in that fault means a ring-3
program's bad pointer took down the kernel. Win32 arguments are as
unchecked here as Linux's are.

### What this does not have

Windows' syscall numbers are not implemented and never will be: Windows
has no stable syscall interface - a program calls kernel32, kernel32
calls ntdll, and ntdll's numbers change between builds - so the
compatibility that matters is at the *API* boundary, which is what the
thunks bridge.

Four functions of kernel32 are implemented. That is not a plan, and the
honest way to widen it is Milestone 45's `tools/pe_imports.py`, which
answers exactly which functions a given `.exe` needs. Run against this
one it says three, from kernel32, and the loader bound three.

Also absent: importing **by ordinal** (rejected outright, since it needs
the exporting DLL's export table), TLS callbacks, delay imports, SEH and
`.pdata` unwinding, and any DLL at all - there is nothing to load a
`.dll` from, and no export tables to resolve against. chrome.exe needs
all of those, and 285MB of `chrome.dll` besides.

## Milestone 53 — 64-bit: a Windows program that ships its own DLL ✅ DONE

151 assertions. `dlluser64.exe` imports two functions from
`dlllib64.dll` and three from kernel32; the DLL imports from kernel32
itself. That is chrome.exe's shape - a small executable whose real work
lives in a library beside it, the way chrome.exe leans on
`chrome_elf.dll`.

```
NOVARIS64: dll     = 0x0000000392eb0000, relocs 1
NOVARIS64: exe     = 0x0000000140000000
NOVARIS64: --- its output follows ---
hello from a DLL, called by a 64-bit PE
hello from a DLL, called by a 64-bit PE
NOVARIS64: --- end of its output ---
```

Two things happen here that Milestone 52 could not do. An import
resolves against a **real export table** rather than a kernel stub - so
the call is an ordinary ring-3 call into the DLL with no thunk and no
syscall in the middle - and the DLL is **loaded away from the base it
was linked at**, with its relocations applied.

### The relocation is load-bearing, not decorative

A DLL built `-nostdlib` from simple code has no `.reloc` section at all:
everything the compiler emits is RIP-relative, so there is nothing to
fix up, and a loader that skipped relocation entirely would pass. The
DLL therefore exports a *data pointer* initialised to an address, which
is a value that has to exist in memory as a full 64 bits and cannot be
folded into a `lea`. That is what makes the linker emit a `.reloc`, and
the Makefile fails the build if it ever stops doing so.

The executable then calls `DllGetMessage`, which returns that pointer,
and prints through it. Loaded at a forced bias of 0x10000000, the
correct answer is 0x392eb3000. Falsified by counting relocations without
applying them: the pointer stays at the address the linker chose,
0x382eb3000, and the program faults reading it.

### The bug: rdi and rsi

Milestone 52's thunk was wrong, and worked by luck.

**`rdi` and `rsi` are callee-saved in the Windows x64 ABI and volatile
in SysV.** A Windows program may keep a live value in either across a
call, and mingw does exactly that - holding the message pointer in `rsi`
across a `GetStdHandle` call - while the SysV C the syscall lands in is
entitled to destroy both. The thunk now pushes and pops them.

Milestone 52's executable happened to keep nothing live in those two
registers, which is the only reason it passed.

What the failure looked like is worth recording, because it is the same
shape as Milestone 51's register bug seen from the Windows side: the
call that *breaks* is not the one that clobbered anything. `GetStdHandle`
returned correctly, and then `WriteFile` was called with 0x28 - the
string's length - as its buffer pointer, because the pointer that should
have been in `rsi` was gone.

The other half of the same ABI difference is already covered: Windows
also treats xmm6-xmm15 as callee-saved, and Milestone 51 made the kernel
`-mno-sse`, so it cannot touch them at all.

### Forwarders are rejected rather than followed

An export whose RVA lands inside the export directory is not code, it is
a string like `"OTHERDLL.OtherFunc"`, and resolving it means loading that
module too. `module_export` detects the case and refuses, rather than
returning a pointer to a string as though it were a function. Chrome's
DLLs use forwarders heavily, so this will have to be implemented; failing
loudly is the honest interim.

### What is still missing

`DllMain` is **not called**. Doing it properly means entering ring 3,
running it, and coming back, before the executable that needs it starts -
which is a scheduler question rather than a loader one, and the DLLs here
have no initialisation to do.

There is still no filesystem, so a module is "loaded" only in the sense
that the kernel already had its bytes: `pe64_load_dll` is handed an image
that was linked into the kernel. A real `LoadLibrary` needs somewhere to
read a `.dll` from, and that is the next thing this needs.

Also still absent: TLS callbacks, delay imports, SEH and `.pdata`
unwinding, and bound imports.

## Milestone 54 — 64-bit: a filesystem, and LoadLibrary ✅ DONE

160 assertions. Until now every binary the 64-bit tree ran was compiled
into the kernel: the ELF, the glibc program, the PE, its DLL. That is
the constraint this removes.

`initrd64.c` reads the archive GRUB loads as a Multiboot module, using
the format `userland/mkinitrd.py` already writes - so the same tooling
serves both trees. It is deliberately the smallest thing that deserves
the name "filesystem": no disk driver, no FAT32, no writing. The 32-bit
`ramfs.c` is 877 lines because it is writable and backed by a real disk;
none of that is needed to answer the question this unblocks.

`LoadLibraryA`, `GetProcAddress` and `FreeLibrary` sit on top of it.
`userland/loader64.c` imports **only** kernel32 - its import table does
not mention the DLL at all - and finds the library by string at run time:

```
NOVARIS64: initrd  = 1 file(s), 8319 bytes
NOVARIS64: --- its output follows ---
LoadLibraryA returned a module
hello from a DLL, called by a 64-bit PE
NOVARIS64: --- end of its output ---
NOVARIS64: exit    = 11, modules loaded 1
```

That is chrome.exe's shape rather than an analogy for it: `chrome.exe`
imports `chrome_elf.dll`, and `chrome_elf` *loads* `chrome.dll` at run
time rather than importing it.

The program diagnoses itself - each failure path exits with its own
number, so a break says which step broke - and it checks three things
beyond the happy path: that a second `LoadLibraryA` of the same name
returns the **same handle** (a DLL's data is per-process; two copies
would give it two sets), that `GetProcAddress` finds a function the
import table never named, and that a missing DLL fails without taking
the process down.

Falsified by building the initrd with no files in it: the archive drops
to 8 bytes, `initrd64_file_count()` is 0, and the program prints
`LoadLibraryA failed`. The DLL genuinely comes from the file.

### The bug this would have caused

GRUB puts the module in ordinary RAM, which `pmm64_init` has just been
told is free. Without reserving it the archive is handed out as a page
like any other and quietly overwritten by whatever allocates next -
surfacing much later as a corrupt archive, nowhere near the cause.
`initrd64_init` reserves its own pages and runs immediately after
`pmm64_init`, before anything allocates.

### What is deliberately missing

`LoadLibraryA` does no address-space bookkeeping: it loads each module
at its preferred base and nothing notices if two DLLs want the same one.
Chrome ships dozens of DLLs and some of them will collide, so a real
loader has to track occupied ranges and relocate on conflict - the
relocation machinery is there since Milestone 53, the arbitration is
not. There is also no `DllMain` call, no reference counting (`FreeLibrary`
accepts and ignores), no search path (the name is matched exactly against
the archive), and no `LoadLibraryW`.

## Milestone 55 — 64-bit: threads, and Wine configures ✅ DONE

167 assertions and a fourth differential. Two results: `clone(2)` works,
and Wine reconfigured for x86-64 **configures cleanly**, which is the
first evidence that the endgame is reachable at all.

### clone(2)

`userland/thread64.s` mmaps a stack, calls `clone`, and the two threads
share memory: the child writes a value, the parent spins until it sees
it. That spin is the test - nothing yields, so the parent only ever
unblocks if the timer preempts it and the scheduler runs the child. A
kernel that created the thread but never ran it would hang, and the
harness timeout would catch that rather than the run passing quietly.

This is what makes it different from Milestone 49's two tasks: those had
an address space each and could not have shared a variable if they
wanted to. These have one between them.

Three pieces had to change:

- **The syscall stub now saves the callee-saved registers**, which SysV
  entitles the C dispatcher to ignore. `clone` has to hand a new thread
  a complete register set and cannot invent one from registers it cannot
  see. With them, `syscall64_args_t` describes the entire 128-byte frame,
  the saved `rflags` and return `rip` included - which is where the
  child's starting point comes from.
- **The scheduler swaps the thread pointer.** `fs_base` lives in an MSR,
  not in the interrupt frame, so nothing else would have saved it; each
  thread has its own, and `CLONE_SETTLS` is how it gets one.
- **The child is its parent with three differences**, exactly as Linux
  specifies: a different stack, its own thread pointer, and `rax = 0` so
  the two can tell each other apart on return.

A `clone` without `CLONE_VM` is a fork, which would have to copy the
address space. Nothing here does that, so it is refused with -ENOSYS
rather than silently producing a thread where a process was asked for.

### What the differential caught this time

Run on Linux, the first version of the program **hung**. `exit` (60)
ends the calling *thread*; with the child parked in a loop, the process
stayed alive. The program meant `exit_group` (231).

Novaris does not yet tell those two apart - both leave ring 3 and end the
run - so **the guest alone could not have caught this**, and would have
gone on passing. That divergence is now the top of the list below.

### Wine, configured for x86-64

```
$ ../wine/configure --enable-archs=x86_64 --disable-tests --without-x \
      --with-freetype --without-vulkan --without-opengl
configure: Finished.  Do 'make' to compile Wine.
$ grep PE_ARCHS config.log
PE_ARCHS=' x86_64'
```

Built out-of-tree in `wine64-build/`, so the working i386 tree beside it
is untouched. Warnings only: no sound system, no gnutls (so no
schannel), no gettext. None of those block a browser - Chrome carries
its own TLS in BoringSSL - and none of them are what stands in the way.

### Still missing, in the order Wine will hit them

1. ~~**Thread exit**~~ - done in Milestone 56.
2. ~~**futex**~~ - done in Milestone 57.
3. **Per-task kernel stacks** - still absent, and Milestone 57 explains
   why futex did not need them.
4. ~~Signals~~ (fault signals, Milestone 58 - asynchronous ones are
   not), ~~a writable filesystem~~ (59), ~~file-backed `mmap`~~ (60).
   **All four structural prerequisites are now present.**
2. **futex**, which is how every real thread library waits. The spin in
   this test is what a program does when it has no futex.
3. **Per-task kernel stacks.** One syscall stack is shared by all
   threads. That is safe *today* only because `FMASK` clears IF on entry
   and nothing re-enables it, so a syscall cannot be preempted - and it
   stops being safe the moment any syscall blocks, which is the moment
   futex arrives.
4. Signals, a writable filesystem, file-backed `mmap`.

## Milestone 56 — 64-bit: exit(2) ends a thread, not the process ✅ DONE

168 assertions. Milestone 55 recorded the divergence its own differential
had exposed; this closes it.

`exit` (60) now ends the calling thread. If a sibling is still runnable
the thread simply stops existing and that sibling continues; only the
last thread out ends the process. `exit_group` (231) always ends the
process. `userland/thread64.s`'s child now calls `exit`, and the parent
still prints afterwards - on Novaris and on Linux alike.

**A thread that has exited cannot be returned to**, which is what makes
this more than a bookkeeping change. The scheduler's ordinary switch
happens inside an interrupt and rewrites the frame it was handed; there
is no such frame here, so `sched64_resume` in `syscall64.s` builds an
`iretq` frame from a saved `registers64_t` and enters the sibling
directly. That assembly reaches into the struct by hand-written byte
offsets, so `sched64.c` now carries `_Static_assert`s on all nine of
them - if the struct changes shape, the build stops instead of the
kernel jumping somewhere arbitrary.

### It broke Milestone 49, which was correct of it

The preemption test hung. Its counting tasks ended the run with `exit`,
which had always torn everything down; now it hands the CPU to the other
counting task and the kernel never gets it back. The test meant
`exit_group`, and says so now.

The same change had a second consequence worth recording: the scheduler's
task table outlives a test layer, so a stale live task would capture the
*next* layer's `exit` and never return. Milestone 49's layer clears the
table when it finishes. In a real kernel the process teardown does this;
here the layers share one scheduler, and that is the seam.

### And the assertion caught its own author

`syscall64_thread_exits() == 1` failed on the first run - earlier layers
call `exit` too, so the counter does not start at zero. It measures the
delta now. Worth noting only because it is the third time in this port
that a test's first failure was the test being wrong rather than the
kernel, and each time the alternative was a green run that proved
nothing.

## Milestone 57 — 64-bit: futex, and a full x86-64 Wine ✅ DONE

175 assertions and a fifth differential. Two results again: threads can
now **sleep**, and a complete 64-bit Wine exists.

### Blocking without a per-task kernel stack

Milestone 55 said futex would force per-task kernel stacks, because one
shared syscall stack is safe only while no syscall blocks. It did not,
and the reason is worth writing down.

A thread that blocks in `FUTEX_WAIT` does not leave a half-finished
kernel call behind. Its entire continuation is a `registers64_t` built
from the syscall frame - the same construction `clone` uses - so the
kernel call *ends*, the shared stack is free, and the scheduler enters
another thread through `sched64_resume`. Waking the thread is setting
`rax = 0` in that saved frame and marking it runnable; the next switch
returns to ring 3 exactly where it left.

That works because these syscalls have no kernel-side work left to do
after they block. A syscall that must block, wake, *and then continue in
the kernel* - a real `read` on a device, say - has nowhere to keep its
locals, and that is the one that will finally require per-task stacks.

`FUTEX_PRIVATE_FLAG` is masked off rather than acted on: it lets Linux
skip a global hash lookup, which is an optimisation rather than a
semantic, and with no shared memory here private and shared behave
identically.

### The test had to be made honest twice

**First**, the program's output proves nothing on its own. A kernel that
implemented `FUTEX_WAIT` as `return 0` still prints the right line - the
parent just falls back into its recheck loop and spins. So the test
asserts on kernel counters: a thread *really blocked*, and a wakeup
*really woke one*. Falsified exactly that way, and the counter assertion
is what fails while the output stays correct.

**Second**, and more interesting: the first version was **flaky, about
one run in three**. Nothing in the protocol forces the parent to reach
`FUTEX_WAIT` before the child sets the flag - and when the child wins,
the parent takes the fast path, never blocks, and the counters are
legitimately zero. The kernel was fine; the assertion was asserting
something the program did not guarantee.

The fix uses the kernel as the oracle. `FUTEX_WAKE` returns how many
threads it woke, so the child spins on it until it returns non-zero -
which makes "the parent is asleep" an **observed fact** rather than a
hope - and only then hands over. Five consecutive runs now report
`waits = 1, wakes = 1`.

A flaky test that passes two runs in three is worse than a failing one,
because the third run gets blamed on the machine.

### A full x86-64 Wine

```
Wine build complete.
$ ls dlls/*/x86_64-windows/*.dll | wc -l
602
$ file dlls/ntdll/x86_64-windows/ntdll.dll
PE32+ executable for WINE (DLL), x86-64, 19 sections
```

602 PE32+ x86-64 DLLs and the `wine` loader, built out-of-tree in
`wine64-build/` so the working i386 tree is untouched. **That is the
same format `pe64.c` already reads.**

It is not running on Novaris and nothing here has tried. Wine's loader is
an ELF binary that wants a filesystem, a prefix it can write, file-backed
`mmap` and signal delivery - none of which exist yet. But the artifact
Chrome ultimately needs now exists on this machine, in the right
architecture, and the gap to it is a list rather than a question.

## Milestone 58 — 64-bit: a fault becomes a signal ✅ DONE

182 assertions and a sixth differential. A ring-3 fault is no longer the
end of a program: it is delivered to a handler, which can look at the
saved registers and decide where to resume.

This is Wine's exception dispatch in miniature, and not by analogy. Wine
installs a SIGSEGV handler; when a Windows program faults it reads the
register set out of the `ucontext`, builds an `EXCEPTION_RECORD`, and
very often **writes RIP back** so execution continues somewhere else. A
handler that cannot do that is no use to it, which is why the test does
exactly that and nothing less.

```
NOVARIS64: --- its output follows ---
caught SIGSEGV, rewrote RIP, and carried on
NOVARIS64: --- end of its output ---
NOVARIS64: signals = 1 delivered, 1 returned, exit 41
```

### The layout is copied, not invented

`ucontext`, `sigcontext_64` and `rt_sigframe` are Linux's exactly, field
order included. That is what makes the sixth differential possible:
`userland/signal64.s` reads the saved RIP at `uc + 40 + 128` because
that is where Linux puts it, and the same binary catches its fault and
recovers on both systems. A frame that were merely Novaris-shaped would
be self-consistent and untestable.

The offsets are `_Static_assert`ed. **Falsified by inserting one padding
field into `ucontext`**, which does not produce a subtly wrong handler -
it stops the build:

```
error: static assertion failed: "uc_mcontext must be at offset 40 in ucontext"
```

That is a better guarantee than a runtime check, because it cannot be
forgotten and cannot pass by luck.

### Details that are load-bearing

- **The red zone.** The frame goes 128 bytes below `rsp`, because the
  ABI lets a leaf function use that space without adjusting the stack
  pointer. Writing the frame at `rsp` would corrupt the locals of the
  function that faulted.
- **`SA_RESTORER` is mandatory** on x86-64 and rejected without, the way
  Linux does. The kernel supplies no return trampoline, so a handler
  installed without one returns into whatever `pretcode` happened to be.
- **`rt_sigreturn` does not return.** It rebuilds a register set from
  the frame and resumes the thread through `sched64_resume` - the same
  mechanism as futex and thread exit. Three different syscalls now end
  by becoming a thread rather than by returning to one.
- **The selectors and IF are the kernel's, not the frame's.** A process
  that could write its own `cs` through a signal frame, or clear IF,
  would be writing itself into ring 0. Only the flag bits a program may
  legitimately change are taken from the saved context.

### What is not here

Asynchronous signals: no `kill`, no `tgkill`, no queueing, no masking
(`rt_sigprocmask` is still accepted and ignored), no `SA_ONSTACK`. All
of that matters for a signal *sent* to a thread; a fault is delivered to
the thread that caused it, at the instant it causes it, and that is the
half Wine's exception path needs.

The frame is also written to the faulting thread's stack **unchecked**.
A thread that faulted *because* its stack pointer was bad will fault
again inside the kernel while the frame is being written. Linux handles
that with an alternate signal stack; this does not.

## Milestone 59 — 64-bit: a filesystem that can be written to ✅ DONE

190 assertions and a seventh differential. Milestone 54's initrd could
be *read*; this one can be written, which is the difference Wine needs -
a prefix is thousands of files Wine creates.

`userland/fs64.s` is ordinary POSIX: open, write, lseek, read, compare,
close, unlink. It runs on Linux against a real filesystem and on Novaris
against a RAM one, and both have to agree. It writes under `/tmp` and
removes what it made, so a host run leaves nothing behind.

```
NOVARIS64: --- its output follows ---
wrote a file, read it back, and removed it
NOVARIS64: --- end of its output ---
NOVARIS64: fs      = 2 nodes, exit 53
```

Syscalls: `open`, `openat`, `close`, `read`, `write`, `lseek`, `mkdir`,
`unlink`. `write` now tells a descriptor from a console: 1 and 2 still
go to the serial port, 3 and up go to the filesystem.

### The read-back is the assertion

Every other check in that program is a syscall return value, and a
filesystem that accepted writes and stored nothing would satisfy all of
them. The comparison is what catches it. **Falsified exactly that way** -
`ramfs64_write` made to accept the data and drop it - and the program
exits **75**, which is the status it reserves for a mismatched
read-back. Each failure path has its own number, so the failure names
the step rather than only reporting that one happened.

### What it is, honestly

A flat table keyed by whole path, not a directory tree. `/a/b/c` is an
entry whose name contains slashes and a lookup is an exact string match.
That is enough for open/read/write on known paths; it is not enough for
`readdir`, for renaming a directory, or for `..`, and Wine will want all
three. The 32-bit tree's `ramfs.c` is 877 lines for those reasons.

Also missing, and each one is a real divergence rather than an omission:

- **No reference counting.** `unlink` frees the data immediately, so a
  program that unlinks a file it still has open - a common idiom for
  temporary files - reads freed memory here and works on Linux.
- **No permissions.** The mode argument to `open` is accepted and
  ignored, and every file is readable and writable by everyone.
- **Nothing survives a reboot**, since there is no disk driver. The
  initrd seeds the filesystem at boot and writes go nowhere else.

## Milestone 60 — 64-bit: a file, mapped ✅ DONE

196 assertions and an eighth differential. This is how a loader loads:
`ld.so` maps an ELF's segments and Wine maps a PE's. Every image this
kernel has run so far was *copied in by the kernel* rather than mapped
by the program.

```
NOVARIS64: --- its output follows ---
mapped a file, read it through the mapping, kept it private
NOVARIS64: --- end of its output ---
NOVARIS64: maps    = 1 file-backed, exit 61
```

### MAP_PRIVATE is a copy, and that is not a shortcut

The filesystem keeps a file's bytes in a heap allocation, which is not
page aligned and cannot be handed to the MMU directly. So a file mapping
allocates fresh frames and copies into them - which is exactly what
`MAP_PRIVATE` means anyway: the pages are the process's own and writing
them does not change the file.

`MAP_SHARED` would have to write back, so it is permitted only where
there is nothing to write back - a read-only mapping - and refused with
-ENODEV otherwise, rather than silently giving a program private pages
where it asked for shared ones.

The pages arrive zeroed, so a mapping running past the end of the file
reads as zeros there, which is what Linux does for the tail of the last
page. Past that last page Linux raises SIGBUS; this keeps reading zeros.

### The test checks the thing that is easy to fake

Two assertions, and the second is the one worth having:

1. **The bytes are the file's.** A kernel returning a zeroed anonymous
   page hands back a perfectly good pointer and fails only here.
   Falsified by skipping the copy: the program exits **94**, its status
   for wrong contents - and the `file_maps` counter still read 1, so the
   counter alone would not have caught it.
2. **Private means private.** The program writes through the mapping and
   then reads the same offset with `read(2)`; the original byte must
   still be there. A kernel that shared the frame instead of copying
   passes (1) and fails this with status 96.

Pages are mapped writable while the kernel copies the file in, then
narrowed if the caller asked for `PROT_READ` alone - so a read-only
mapping really is read-only, unlike `mprotect`, which remains a no-op.

## The four prerequisites are done. What that does and does not mean

Milestone 55 listed what Wine needs from this kernel: threads, signals,
a writable filesystem, file-backed `mmap`. All four now exist, and each
is checked against Linux with the same binary.

That is worth stating precisely, because it is easy to over-read. It
means the **mechanisms** are present and behave like Linux's on the
paths a test exercises. It does not mean Wine runs. Wine will exercise
them far past where these tests stop:

- The filesystem is a **flat path table**, and a Wine prefix is a deep
  directory tree that Wine walks with `readdir` and `..`, neither of
  which exist.
- There is **no `execve`**, so nothing can start a second program; Wine
  is a loader whose whole job is starting programs.
- Signals are **fault-only**: no `kill`, no masking, no queueing.
- `unlink` has **no reference counting**, and Wine uses the
  unlink-while-open idiom.
- `mprotect` is a **no-op**, and Wine relies on it for PE section
  permissions.

The next honest step is not another prerequisite. It is to point Wine's
loader at this kernel and read the first thing it complains about - the
`[enosys]` line from Milestone 51's tracer is designed for exactly that.
Everything after that is driven by what it asks for, rather than by a
list written in advance.

## Milestone 61 — 64-bit: the dynamic loader, and what it asked for ✅ DONE

206 assertions. Wine's loader is a dynamically linked PIE needing
`/lib64/ld-linux-x86-64.so.2` and `libc.so.6`, so **nothing about Wine
can be attempted until ld.so itself runs**. This milestone ran it and
followed what it asked for, which is a different way of working from
every milestone before it: the list was written by the program, not in
advance.

`PT_INTERP` support, `elf64_load_at` with a load bias so an ET_DYN image
can be placed, `AT_BASE` on the initial stack, and the host's real
`ld-linux-x86-64.so.2` and `libc.so.6` in the initrd at the paths ld.so
actually searches.

### What it asked for, in the order it asked

Each of these was found by running it and reading the trace, then fixed,
then run again. Five kernel defects, and only the first was a missing
feature:

1. **`access(2)`** - ld.so probes `/etc/ld.so.preload` before anything
   else. Missing.
2. **`newfstatat(2)`** and **`pread64(2)`** - missing. `pread64` is how
   it reads a library's program headers without losing its place.
3. **`fstat` lied.** Since Milestone 51 it answered "character device,
   size 0" for *every* descriptor. Right for stdout, fatal for a
   library: ld.so fstats what it just opened, sees something unmappable
   with no length, and gives up.
4. **`st_dev`/`st_ino` were always 0.** ld.so identifies an
   already-loaded object by that pair, so every file looked like the
   same file - it opened libc, compared it against the main executable,
   concluded they were the same object, and closed it without mapping.
   What that looks like from outside is `undefined symbol:
   __libc_start_main`, a very long way from the stat that caused it.
5. **`MAP_FIXED` was ignored, and worse, ignored twice.** A loader
   reserves a library's whole range read-only, then maps each segment
   into place with `MAP_FIXED`. The address was being reallocated
   instead of honoured; and once that was fixed, the pages already
   present from the reservation kept the reservation's *read-only*
   flags, so libc's data segment was read-only and ld.so faulted
   relocating its own library.

The trace prints paths for path-taking calls now. Without that, a
dynamic loader's trace is a wall of pointers and the one question worth
asking - which file could it not find - is the one thing it does not
answer.

### Where it stops

ld.so now finds libc, maps all four of its segments at the right
addresses, applies RELRO with `mprotect`, sets up TLS, and begins
executing **inside glibc**. It dies there, in glibc's own `atexit`
machinery, on `mov 0x8(%rsi),%rax` with `rsi = 0x20` - a list pointer
that should be a pointer and is not.

That is not asserted as working, and there is no green check pretending
it is. What *is* asserted is that the kernel survives it: an unhandled
ring-3 fault now **kills the program** with status 139 (128 + SIGSEGV),
the way a real kernel does, instead of halting the machine.
`enter_user_mode64_abort` does that, and it is useful well beyond this
layer - a bring-up test that asserts nothing faults wants a halt, and
one whose purpose is finding out how far a program gets does not.

Remaining `-ENOSYS` in that run: 5, all tolerated - `rseq`, `prlimit64`,
and `readlinkat`.

### The pattern worth keeping

Four of the five defects were **lies rather than gaps**: a stub that
returned a plausible answer instead of the true one. A missing syscall
announces itself with `-ENOSYS` and gets fixed in minutes; a stub that
answers wrongly is found three layers downstream, wearing a symptom that
points somewhere else entirely. The `fstat` and `st_ino` bugs had both
been sitting in the tree since Milestone 51 and passed every test up to
this one, because nothing before ld.so ever asked them a question whose
answer mattered.

## Milestone 62 — 64-bit: a dynamically linked program runs ✅ DONE

207 assertions and a ninth differential. Milestone 61 left ld.so dying
inside glibc's `atexit` machinery. This is that bug, and it was one
line - in a place the symptom pointed nowhere near.

```
NOVARIS64: --- its output follows ---
a dynamically linked program reached main
NOVARIS64: exit    = 67
```

67 is what `dynhello64.c`'s `main` returns, and it is reachable only
through the whole chain: ld.so relocated itself, found libc, mapped it,
relocated it, resolved `__libc_start_main`, and called `main`.

### MAP_ANONYMOUS was handing back somebody else's data

The fault was a read through a pointer that should have been NULL. The
register dump - added for exactly this - showed the list *head* was a
perfectly good address in libc's `.bss`, so relocation had worked; it
was a `next` pointer one node in that read `0x20`.

`.bss` is supposed to be zeros, and it was not. A loader maps a
library's whole span from the file to reserve the address space, then
maps each segment over it, and finally maps the `.bss` over the tail
with `MAP_ANONYMOUS|MAP_FIXED`. Those pages already existed - from the
file-backed reservation - and `map_anon` *reused* them without zeroing.
So the `.bss` contained whatever bytes libc's file happened to have at
that offset, and one of them was a list terminator that was not NULL.

**An anonymous mapping reading as zeros is the entire content of the
word "anonymous"**, and the one case where it is easy to get wrong is a
page being recycled from an earlier mapping.

### And the over-correction, which broke something else

Zeroing every recycled page broke the *static* glibc test with
`*** stack smashing detected ***` - from a program that never touched
its stack. `brk` grows the heap through the same helper, starting at the
current break, which is **not page aligned**: the page holding the break
is full of live heap, and zeroing it destroys the allocator's own
bookkeeping.

So the zeroing is now a parameter rather than a policy: `mmap` asks for
it, because a fresh mapping owes the caller zeros; `brk` does not,
because it is extending a region whose existing contents are the point.

Two bugs, opposite in shape - one from not zeroing what it should, one
from zeroing what it should not - and both presented as corruption a
long way from the mapping code.

### What this unblocks

Wine's loader is a dynamically linked PIE needing exactly this. Running
it is now a question of whether the syscalls it makes are implemented,
rather than of whether it can start at all.

## Milestone 63 — Wine runs ✅ DONE

```
NOVARIS64: --- what Wine asks for ---
...
NOVARIS64: [call] 1(0x1, 0x55555555abd0, 0xa)wine-11.0
NOVARIS64: [call] 231(0x0, ...) = 0x0
```

Wine's loader started, loaded `ntdll.so`, initialised, parsed its
arguments, answered `--version` and exited 0. On a kernel written from
nothing.

208 assertions. The layer asserts this **only when a 64-bit Wine has
been built beside the tree** - `make test` must not require an hour of
compiling Wine first - and says so when there is none.

### What it took, in the order Wine asked

The whole milestone was reading a trace and answering it. Nothing here
was predicted in advance:

1. **`readlink("/proc/self/exe")`** - missing, and Wine derives the path
   to `ntdll.so` from it. Without it Wine computed that path as `(null)`
   and stopped. There is no `/proc` here, so the loader records the
   answer instead.
2. **`ntdll.so` at `/ntdll.so`** - Wine takes the directory of
   `/proc/self/exe` and appends the library name. The loader is at
   `/wine`, so that is where it looked.
3. **`libgcc_s.so.1`** - `ntdll.so` needs it for its unwinder.
4. **An environment.** Programs here had *no environment variables at
   all*. glibc looks the user up to answer `getpwuid`, Wine asks it
   where `HOME` is, and the absence turned into a NULL dereference deep
   inside a library. `HOME`, `USER`, `WINEPREFIX` and `WINEDLLPATH` are
   handed over now, and `uspace64_build_stack` lays out a real `envp`.
5. **`/etc/passwd` and `/etc/nsswitch.conf`** - created by `ramfs64_init`,
   because a passwd lookup that returns NULL is checked by nobody.
6. **`argv`.** The stack builder took a single `argv0`; `wine --version`
   needs two arguments, so it takes an array.

### What this does and does not mean

It means the Linux ABI this kernel implements is good enough to start
Wine and get it through `ntdll.so`'s initialisation - the syscalls, the
dynamic loader, the auxiliary vector, TLS, signals, the filesystem and
`mmap` all held up under a program that was not written with them in
mind and cannot be adjusted to suit them.

It does not mean Wine can run a Windows program. `--version` is answered
before Wine needs any of the things it is actually missing here:

- **No second process.** Wine's architecture is client-server, and
  `wineserver` is a separate process. There is no `fork` and no
  `execve`.
- **No prefix.** Creating one means thousands of files in a deep
  directory tree, and this filesystem is a flat path table with no
  `readdir` and no `..`.
- **Only `ntdll.so` is present.** The other 601 DLLs are not in the
  initrd, and would not fit: the initrd is RAM-backed and user pages
  must come from the first gigabyte.

Those are the next three walls, in that order, and the first is the one
everything else waits on.

## Milestone 64 — fork, execve, wait ✅ DONE

217 assertions and a tenth differential. The wall everything else was
waiting on: Wine's architecture is client-server and `wineserver` is a
separate process, so a kernel that cannot start a second program cannot
host a program-starting program.

```
NOVARIS64: --- its output follows ---
  (the execed child is running)
forked, execed a different program, and waited for it
NOVARIS64: procs   = 1 left, exit 71
```

### First, the state had to belong to a process

Open files lived in a static array in `syscall64.c`, the heap and mmap
bookkeeping in another in `uspace64.c`, and the address space was
whatever the kernel had last switched to. That is indistinguishable from
correct while there is one program and wrong the moment there are two: a
child sharing its parent's descriptor table is not a child, it is the
same process with two register sets. `proc64.c` owns that state now and
the scheduler switches process and thread together.

**fork copies the whole address space** - every mapped page in the low
half, into fresh frames. A real fork shares them copy-on-write and
duplicates on the first write; this pays the entire resident size up
front. That is the right trade while the question is whether fork works
and the wrong one the moment anything forks in a loop.

**wait4 blocks and *restarts*.** A parent that calls it before its child
has been scheduled has to wait, and there is no way to return a value
that was not known when it blocked - so the frame it resumes from has
`rip` rewound by the two bytes of the `syscall` instruction and the
number put back in `rax`. Waking re-executes the call and re-checks.
Linux does exactly this, for exactly this reason.

### Four bugs, each visible only through another one's symptom

1. **`exit_group` handed the CPU to a sibling.** It must end *every*
   thread of the process; ending only the caller left the preemption
   test's second task running and the kernel never got control back.
2. **`sched64_add` never set a pid**, so a timer tick set the current
   process to one nothing owned. Three layers later a syscall
   dereferenced a null process - in the kernel, in `uspace64_brk`,
   nowhere near the tick that caused it.
3. **`execve` did not update the scheduler task's address space.** The
   task carries its own copy and the scheduler reloads it on every
   switch, so the next tick put the *old* space back while `rip` pointed
   into the new program. Both test binaries link at 0x400000, so that
   landed in real code and kept running - the child appeared to re-enter
   `_start` and fork again, four times, until the process table filled.
   A crash would have been kinder.
4. **`sched64_wake` forced `rax = 0`**, which is right for futex and
   destroys a restarting syscall's number. The restarted `wait4` came
   back as call 0 - `read`. The wake value is per-task now.

### And a C lesson worth keeping

The first attempt made the per-process fields reachable through macros
named after them - `#define fds (proc64_current()->fds)`. That expands
inside `p->fds` too, producing a syntax error in code that looks
correct. Accessor functions, or the macro used consistently, but never a
macro sharing a name with the field it wraps.

### What is still missing

`fork` has no copy-on-write, `execve` does not honour `#!` or search a
`PATH`, `wait4` ignores its options and its rusage argument, there is no
process group, no signal to a process, and `PROC64_MAX` is 4. Zombies
are real: a child nobody waits for keeps its slot, which is correct
behaviour and a leak in a kernel with four of them.

## Milestone 65 — a directory tree ✅ DONE

226 assertions and an eleventh differential. The filesystem was a flat
table keyed by whole path: `/a/b/c` was an entry whose name happened to
contain slashes, and a lookup was a string compare.

That is enough to open a file you can already name, and not enough for
anything that *explores*. `readdir` has nothing to enumerate. `..` has
nowhere to go. Creating a file in a directory that does not exist
succeeds. A Wine prefix is a deep tree that Wine walks, so the flat
version stopped being a simplification and started being a lie.

Each node now holds one path component and its parent; the root is node
0; a path is resolved by walking it.

### What the test does that the old one could not

`userland/tree64.s` is ordinary POSIX and runs on both systems:

- **nested `mkdir`**, where the inner one needs the outer to exist
- **open through `..`** - `/tmp/nvtree/sub/../sub/leaf` has to *resolve*,
  and a string compare cannot
- **`getdents64`**, which needs a directory to have children, and
  synthesises `.` and `..` the way a real filesystem does
- **`rmdir` refusing a directory that still has something in it**,
  because otherwise "empty" means nothing

Falsified by making `..` a no-op rather than a step to the parent: the
program exits **63**, which is the status it reserves for exactly that,
and the kernel-side assertion catches it independently.

### Two bugs in the test itself, both mine

The failure labels were defined after a helper function, and NASM scopes
a local label to the last *non-local* one - so `.fail_mkdir` silently
became `streq.fail_mkdir` and every jump to it failed to assemble.

Then `streq` used `cl`/`ch` while the caller held `d_reclen` in `rcx`.
`ch` is the high byte of that register, so the scan stepped by a
corrupted length and missed the entry it was looking for - which
presents as "the directory does not contain what it contains". Both were
caught by running it on Linux first, where the answer is known.

### Still missing

No working directory, so every path is absolute - there is no process
attribute to make one relative to yet. No hard links, no symlinks, no
permissions, no timestamps, and `unlink` still frees a file that is
still open. `RAMFS64_MAX_NODES` is 192, which a Wine prefix would exhaust
immediately.

## Milestone 66 — the memory ceiling ✅ DONE

237 assertions. Until now every page a process could own had to come from
the first gigabyte of physical memory.

Not because of a policy - because of an accident of the boot code.
`boot64.s` maps the first 1GB of RAM twice, once identity and once at
`KERNEL_VMA`, and that second window was the *only* way the kernel could
reach a physical address. But the kernel has to touch every page it hands
out: it zeroes anonymous memory, it copies program text into it, it
copies pages on fork. So four separate files carried the same line -

```c
if (frame >= PHYS_WINDOW) { /* refuse */ }
```

- and `chrome.dll` alone is 285MB. The ceiling was not a tuning
parameter, it was a wall.

### The direct map

PML4 slot 272, `0xFFFF880000000000`, every byte of physical memory mapped
in 2MB pages. `phys64_to_virt(phys)` is now the answer to "where can I
touch this frame", and the four refusals are gone rather than raised.

2MB pages, not 4KB, and the reason is arithmetic: 8GB of RAM needs four
page directories at 2MB granularity and two million page tables at 4KB.

### One thing the direct map cannot do for itself

`paging64_physmap_init` allocates the page tables it maps with, so it
needs the frame allocator, so the allocator's bitmap still lives in the
1GB boot window. That is the one remaining thing whose physical address
matters, and it now says so in a comment rather than being a coincidence.

The same constraint reordered boot. The initrd is read through the direct
map, the direct map needs the allocator, and the allocator must not hand
out the initrd's frames - which has exactly one legal order:

```
pmm64_init  →  initrd64_reserve  →  paging64_init
            →  paging64_physmap_init  →  initrd64_init
```

`initrd64_reserve` was split out of `initrd64_init` for precisely this.

### User memory now comes from the top

`pmm64_alloc_high()` allocates downwards; `pmm64_alloc_frame()` still
allocates upwards. User pages take the former, page tables and the bitmap
the latter.

This is the part worth arguing for. A single test that reaches high
memory on purpose proves the mechanism works and nothing about whether
anything *uses* it. Serving every user page from the top of RAM means all
eleven differentials, the ELF loader, the PE loader, fork's page copy and
the dynamic loader now run on memory above the old ceiling on any machine
with more than 1GB - so the claim "where a frame lives no longer matters"
is carried by the ordinary path instead of by one assertion.

Measured: with 2GB the test program's own text loads at `0x7ffde000`;
with 6GB, at `0x1bfffe000`, on the far side of the 4GB PCI hole.

### A bug that had been passing for twenty milestones

The test machine had to grow from QEMU's default 128MB to 2GB - with
128MB there is no frame above 1GB and the milestone's whole claim is
untestable. That immediately produced 20 failures, and the cause was not
in any of the new code.

`pmm64_init` placed the frame bitmap "immediately above the kernel
image". GRUB loads the initrd a few pages above the kernel image. At
128MB the bitmap is 4KB and stopped 20KB short of the module; at 2GB it
is 64KB and lands squarely on the initrd's header.

The bitmap's size scales with RAM. Its distance from the module does not.
It presented as "no filesystem" - eight files became zero - which is
nowhere near "the frame allocator wrote on the initrd", and it had been
one configuration change away from happening since Milestone 35.

This is the same shape as the M61 lesson: the code was not missing, it
was *lying* - stating a placement rule that was true only for the size it
had been tested at.

### Falsified three ways

- Drop `PAGE64_HUGE` from the PD entry: the kernel triple-faults, because
  the direct map becomes 512 page-table pointers into arbitrary RAM.
- Restore the old bitmap placement: 20 failures, the initrd unreadable.
- Zero user pages through `KERNEL_VMA` again: the kernel dies writing to
  an unmapped address, because user pages are now above the window.

Regression-tested at 128MB (the direct-map assertions report SKIP and say
so, rather than passing by not running) and at 6GB.

### Still missing

Fork still copies every page rather than sharing them copy-on-write, so
the ceiling being gone means a fork now costs high memory instead of
failing. Nothing unmaps the direct map's tables, which is correct - they
are permanent - but it does mean `paging64_tables_allocated()` counts
them. `RAMFS64_MAX_NODES` is still 192.

## Milestone 67 — the display ✅ DONE

260 assertions, and a test that asks QEMU what is on the screen rather
than asking the kernel.

### The framebuffer was already there

`boot64.s` has had the Multiboot `VIDMODE` bit set and a 1024x768x32
request in its header since it was written. GRUB has been granting it
every boot since. Nothing ever read the answer, so for twenty-two
milestones this kernel booted onto a working linear framebuffer and drew
nothing on it.

Measured before anything was built, by printing the handoff:

```
flags=0x1a6f fb=0xfd000000 w=1024 h=768 bpp=32 type=1 pitch=4096
```

### Why it could not have been done before Milestone 66

VRAM is at `0xFD000000`. RAM stops at `0x7FFE0000`. The framebuffer is
not merely above the old 1GB boot window - it is above *memory*.

Until the direct map there was no mechanism in this kernel for reaching a
physical address the boot window did not cover, so the 64-bit tree had no
display for a reason that had nothing to do with graphics. The ceiling
and the blank screen were the same fact.

The direct map does not cover it either, and should not: mapping RAM is
`paging64_physmap_init`'s job, and stretching it to `0xFD000000` would
mean mapping the 1.9GB hole in between, most of which decodes to nothing.
Device memory gets its own mapping at PML4 slot 273, in 2MB pages,
uncacheable.

### The part that matters for Wine

A driver only the kernel can draw with is not a display driver. The
milestone is really the interface:

- **`/dev/fb0`**, a device node - the filesystem gained the concept, and
  deliberately knows nothing beyond a number, exactly as Linux keeps its
  device model out of tmpfs.
- **`FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`**, filled at Linux's
  own structure offsets, so a program asks the driver what the screen is
  instead of being told out of band.
- **`mmap` that genuinely shares**. Every other file mapping in this
  kernel is a private copy; this one hands the process the actual VRAM
  frames, because a framebuffer nobody else can see is not a framebuffer.

`userland/fbdraw64.c` is an ordinary Linux fbdev client - open, two
ioctls, mmap, draw - built by the host compiler and running unmodified.
It is the shape of every display driver that would sit above this kernel,
Wine's included.

### Two tests that a weaker version would have passed

The kernel reading back what the kernel just wrote proves the mapping and
nothing else: a framebuffer mapped over ordinary RAM passes every such
assertion and shows a black screen. So:

- **`tools/fbtest.py`** boots the ISO, waits for the kernel to say it has
  finished, and takes a QEMU `screendump` - the emulated display device's
  own surface, the far side of the driver. 16 pixels are checked,
  including four *outside* each block, because a picture drawn at the
  wrong offset or with the wrong stride passes an inside-only check.
- **The kernel checks the band the ring-3 program drew.** A private copy
  would let `fbdraw64` exit 0 - from inside the process the two are
  indistinguishable - while the screen stayed empty. Confirmed by
  falsification: with the device mmap replaced by an anonymous mapping,
  the program still reports success and the kernel sees nothing.

### One ordering bug, and one falsification that passed

`mmap` refused every writable `MAP_SHARED` before asking what was being
mapped, so `/dev/fb0` could be opened, described, and never mapped. The
device case now comes first.

Then the useful failure. The clipping assertion was

```c
fb64_put_pixel(fb64_width(), 0, WHITE);
check("writing outside the screen is ignored",
      fb64_get_pixel(fb64_width(), 0) == 0);
```

which passes with the bounds check deleted, because `fb64_get_pixel`
clamps too - the assertion was reading a *different function's* clip.
Deleting the check and watching the test still pass is how it was found,
which is the same lesson as Milestone 50: a falsification that passes is
information.

It now writes a known value at `(0, 1)` and asserts an unclipped store at
`(width, 0)` - which computes that exact address - does not destroy it.
Both `put_pixel` and `fill_rect` clipping fail their tests when removed.

### Verified

260 assertions, 0 failures, 3/3 deterministic runs, 11 host/guest
differentials, plus the screendump check. Also booted at **800x600** and
**1280x800** (asked for 1366x768; GRUB granted 1280x800, and the kernel
used what it was given) - the driver reads its geometry rather than
assuming it, and the two fixed-coordinate tests report SKIP on a screen
too small instead of failing. Booted with `-vga none`: reports
`FB64_NO_MODE` and carries on, 0 failures.

### Still missing, and two things not to claim

**No input at all** - no keyboard, no mouse. The 32-bit tree has both;
none of it was ported. A display without input is half a desktop.

**The uncacheable bits are not tested.** `PAGE64_PCD | PAGE64_PWT` is
architecturally required - a write-back mapping of VRAM leaves the screen
showing whatever the cache last evicted - but QEMU does not emulate cache
incoherency, so removing them passes every test including the screenshot.
It is correct by argument, not by measurement.

**A padded pitch is never exercised.** `pitch` is threaded through every
calculation, but every mode this bootloader and device produce has
`pitch == width * bytes-per-pixel` (4096, 3200, 5120 at the three
resolutions tried), so the one case where stride handling matters has
not actually occurred.

Beyond that: no write-combining (which needs PAT, and is what makes
full-frame drawing fast rather than merely correct), no double buffering,
no damage tracking, no text console, no compositor. The 32-bit tree's
`gfx.c`, `wm.c` and `console.c` have no counterpart here.

## Milestone 68 — a working directory, and links ✅ DONE

266 assertions, and the first milestone whose size was chosen by
measuring the thing it is for rather than by guessing.

### What a prefix actually costs

`wineboot -u` was run against the Wine tree beside this one and the
result counted rather than estimated:

```
1049 nodes    119 directories, 917 files, 13 symlinks
901 MB
depth 18      longest component 93 chars, longest path 170
```

Every one of those numbers was over a limit in `ramfs64`:

| | before | needed | now |
|---|---|---|---|
| nodes | 192 | 1049 | 4096 |
| name | 64 | 93 | 256 |
| path | 128 | 170 | 1024 |
| symlinks | none | 13 | yes |

A Wine prefix could not have been unpacked into this filesystem, let
alone used. That is a more useful statement than "the filesystem is
small", and it is the reason this milestone exists.

### The two absences

Neither was a bug. Both were things the tree had never needed before and
said so in its own comments — `syscall64.c` on `openat`: *"there is no
working directory here, so a relative path has nothing to be relative
to"*, and `ramfs64.h`: *"no links, and no working directory"*.

**Relative paths were answered `-ENOENT`.** Not an obscure gap: ld.so,
make, `configure` and Wine all spend most of their path handling
relative to where they are, and Wine's first act on a prefix is to
`chdir` into it. The 32-bit tree's log carries the same failure from
before it had one — "chdir to /disk/.wine : No such file or directory",
recorded three separate times in `ata.c`, `fat32.c` and `ramfs.c`.

**There were no symlinks.** A prefix is held together with them:
`dosdevices/c:` is a link to `../drive_c`, and Wine reaches drive C by
reading it.

### What the child list is for

Raising `RAMFS64_MAX_NODES` is one line. The line underneath it is the
milestone: every lookup scanned the whole table, so resolving one path
cost depth x MAX_NODES, and `create` scanned it again for a free slot.
At 192 that is invisible. At 4096, with Wine opening a prefix's worth of
files, it is Milestone 46's quadratic `kmalloc` in a different file.

So directories now keep a child list and free nodes are threaded onto a
free list. Neither is a tidy-up; both are the difference between a
constant that was raised and a filesystem that can be used at the new
size.

### The working directory is stored as text, and canonically

Per process, inherited by `fork`, kept across `execve`. Two decisions
worth recording:

- **Text, not a node index.** A node index goes stale — Linux lets a
  process's directory be removed underneath it, and the process keeps
  its cwd while every relative path from it fails.
- **The canonical path, rebuilt from the tree**, not the text the
  program supplied. So `getcwd` after `chdir("a/../b")` answers `/b`,
  and after a chdir through a symlink answers where it landed rather
  than how it got there.

Every path syscall now passes through one function that makes it
absolute against the cwd. `chdir`, `fchdir`, `getcwd`, `lstat`,
`symlink`, `symlinkat`, `readlinkat`, `mkdirat` and `unlinkat` joined
the table; `readlink` grew a second kind of answer beside its
synthesised `/proc/self/exe`.

`PROC64_MAX` went 4 → 32. Building a prefix runs wineserver, wineboot,
services.exe and explorer.exe at once, and the fifth process to start is
the one that fails.

### The test picks nothing

`userland/cwd64.c` is an ordinary C program against real glibc — the
same argument as Milestone 51. It does not issue syscalls; it calls
`mkdir`, `chdir`, `getcwd`, `symlink`, `readlink`, `stat`, `lstat` and
`remove`, and **glibc** decides those mean `openat(AT_FDCWD)`,
`unlinkat(AT_REMOVEDIR)` and `newfstatat`. This kernel has to answer
whichever it picks, which is a much stronger claim than answering the
calls a test chose to make.

40 assertions, byte-identical on host and guest, exit 89. Among them:
a relative target resolved against the directory the link sits in and
not against the cwd (the classic symlink bug — it would resolve
`../drive_c` to `/tmp/drive_c`), `stat` and `lstat` disagreeing about
the same path, a dangling link that opens `ENOENT` but still answers
`readlink`, and `unlink` on a link leaving the target alone.

The differential compares **line by line**. The other twelve use
`grep -qFf`, which succeeds if *any* pattern line matches — a guest that
printed one `ok` and then died would pass that check. With forty
assertions it would have passed it easily.

### The sizing is tested, not asserted

The kernel builds 1260 files across 60 directories and looks every one
of them up again — 1338 nodes of 4096 — because nothing else in this
tree builds 193 of anything, and a raised constant that is never
exercised is exactly the kind of plausible-looking claim this repo keeps
finding layers downstream.

### Falsified

Each mechanism was removed and the tests re-run, because a test that
passes with the feature gone is testing nothing:

| broken | failures |
|---|---|
| `abs_path` refuses relative paths (the pre-M68 behaviour) | 31 |
| `walk` never follows a symlink | 9 |
| `unlink` follows the final symlink | 4 |
| `free_node` leaves the node in its parent's child list | 1 |

The symlink loop limit is the one that cannot be falsified this way:
removing it does not fail an assertion, it hangs the kernel, and the
run times out instead.

### Verified

266 assertions, 0 failures, 13 host/guest differentials, at **2G and
6G** — the RAM axis Milestone 66 established, and it matters more here
than usual because the node table is now a 1.2MB heap allocation rather
than BSS.

### Still missing, and one thing not to claim

**A prefix has not been created on this kernel.** This milestone removes
the reasons it could not have been — the node ceiling, the name and path
ceilings, the missing links, the missing working directory — and proves
each of them individually. It does not run `wineboot`. The distance
between "every ingredient is present and tested" and "the thing works"
is exactly what this repo keeps getting wrong, and it is not being
claimed here.

Also still missing, and all of it needed before that run: **no
copy-on-write fork** (the address space is copied eagerly, and
wineserver forks), no rename, no file times, no permissions, no
reference counting on an unlinked-but-open file, and 901MB of prefix
against a filesystem that holds everything in the heap.

## Milestone 69 — fork that shares ✅ DONE

The eager clone copied every page of the parent, and its page list
stopped at `CLONE_MAX_PAGES` — 4096 entries, so **16MB of address
space**. Wine's wineserver forks, and a process with a prefix mapped is
far past 16MB before it does.

### Why the direct map is what made this possible

The eager version had to collect the source into a list and replay it
into the destination, because the recursive page-table addresses only
ever describe the space that is *loaded*. That is the whole reason it
had a ceiling.

Milestone 66 made every physical address reachable at
`PHYSMAP64_BASE + phys`. Walking the tables through the direct map
instead means both sets are readable and writable at once, neither
space has to be current, and there is no intermediate list to size. The
ceiling did not get raised; it stopped existing.

### The three parts

- **Frames carry an owner count**, one byte each, beside the pmm bitmap
  and reserved with it. Zero extra owners is the default, so nothing
  that allocated before this had to change, and a frame nobody shared
  is still freed on the first free.
- **`PAGE64_COW` marks a shared page, `PAGE64_COW_RW` remembers whether
  it had been writable.** Two bits, not one, because after the fact the
  two are indistinguishable — with only the first, every read-only page
  in a forked child silently becomes writable the moment it is touched.
- **`CR0.WP` is set.** Without it the write-protect bit in a PTE binds
  ring 3 only and the kernel writes straight through a read-only
  mapping. A `read()` into a forked child's buffer would land in the
  page the *parent* is still using, with no fault and nothing to
  notice. GRUB leaves it clear and `boot64.s` only ever touched the low
  16 bits of CR0, so nothing before this had it on.

### Two bugs, neither of which looked like fork

**glibc has no `fork(2)`.** It calls
`clone(CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD)`, and `clone`
without `CLONE_VM` returned `-ENOSYS` here — the comment on it even said
"-ENOSYS: fork". So the raw-assembly fork test passed and *every* C
program's `fork()` failed. A kernel can have a complete `SYS_fork` and
no fork at all as far as any ordinary program is concerned.

**`sched64_block_current` never saved the blocking thread's `fs_base`.**
The timer's switch-out does save it, so a thread preempted at least once
after setting up its TLS carried a correct value by luck; one that
blocked first woke with whatever its slot was created with — 0 for the
first task of a process. From ring 3 that is `fs:0x10` reading as zero,
and the crash lands inside the *next* syscall wrapper
(`__internal_syscall_cancel`, `mov 0x308(%rbx)` with `rbx` zero) with
nothing pointing back at the `wait4` that caused it. It was found by
disassembling the faulting address in the very ELF the guest was
running, which is a technique worth remembering: the host has the same
file.

### Measured

An 8MB process forks for **14 frames**, against the 2048+ an eager copy
charges, and **4109 pages** are copied lazily as the two sides write.
0 double frees.

`userland/cowfork64.c` deliberately cannot tell the two apart — both
produce its output exactly. Correctness is what it checks: that the
child sees the parent's memory, that the child's 8MB of writes do not
appear in the parent, and that the parent can still write afterwards.
The *cost* is measured by the kernel across the fork syscall, which is
the only place the difference is visible.

### Falsified

| broken | result |
|---|---|
| fork copies eagerly again | 2 failures — the frame count and the COW count |
| only the child is made read-only | the parent's buffer comes back overwritten |
| `block_current` stops saving `fs_base` | the fault returns, in the same place |

## Milestone 70 — a keyboard and a mouse ✅ DONE

Milestone 67 gave this tree a display and said what was missing: *"no
input at all — no keyboard, no mouse. A display without input is half a
desktop."* The 32-bit tree has both drivers; neither had been ported.

The decoding is the small half. The interface is the argument, and it is
the one `/dev/fb0` already made: a driver only the kernel can read is
not a driver. So these are **`/dev/input/event0` and `event1`**, handing
back Linux's own `struct input_event` at Linux's own layout, because
evdev is what Wine sits on top of on Linux. `read(2)` returns whole
records and never a partial one.

The keyboard needs no setup — the BIOS leaves it streaming. The mouse
needs the auxiliary port enabled, IRQ12 turned on in the controller's
config byte, bit 5 cleared so the aux clock runs, and **the cascade line
unmasked**; a mouse enabled at both ends and silent is what any one of
those left out produces.

### Two things not to re-learn

**The mouse answers every command with `0xFA`, and `0xFA` has bit 3
set.** Re-syncing the packet stream on bit 3 alone therefore accepts an
ACK as a first byte and mis-frames everything after it. The test caught
this by disagreeing with a comment that claimed otherwise — the comment
was written first and was wrong.

**Interrupts had been off since the scheduler layer put them back that
way.** The first version of the watch loop sat in `hlt` forever and
received nothing at all, while every synthetic assertion passed.

### Which is exactly why inputtest.py exists

Every input assertion in `kmain64.c` feeds the decoder by hand. All of
them pass on a kernel whose IRQ1 is masked — verified, not assumed:
with the unmask deleted the kernel still reports **0 failures** across
all 275 assertions while `tools/inputtest.py` fails all nine of its
events. The kernel prints `irq1 0, irq12 0` beside them to say so.

`inputtest.py` sends real keystrokes and real mouse movement through the
QEMU monitor, so they cross the emulated 8042 and raise genuine
interrupts. Same argument as `fbtest.py` taking a screendump instead of
asking the kernel what it drew.

One expectation in it looked wrong and was not: a downward `mouse_move`
arrives as **positive** `REL_Y`. It is inverted twice — the monitor
speaks screen coordinates and QEMU negates into the PS/2 packet, then
the driver negates back — landing on evdev's convention, which is the
number Linux reports for the same gesture. The inversion itself is
asserted directly in the kernel instead, where a packet with `dy=+3` has
to come out as `REL_Y=-3`.

## Milestone 71 — towards a Wine prefix ⚠️ PARTLY DONE

**No prefix has been created.** What this milestone did is get Wine from
"could not load ntdll.so" to running its own ntdll and reserving its
address space, and it found two real kernel bugs on the way. The
distance covered is worth recording precisely, because the next session
starts exactly where this stopped.

### What a prefix costs, measured twice

Milestone 68 measured the prefix a Wine run *produces*: 1049 nodes,
901MB. This measured what creating one *requires*, which is a different
and much better-defined question.

Wine builds **602 PE DLLs** and 108 PE executables. `wineboot -u`, run
against this same tree with `WINEDEBUG=+loaddll`, loads **93 of them**.
That is 174MB in the build tree and **58MB stripped** — the difference
between an initrd this kernel can carry and one it cannot. The list is
`tools/wine_prefix_modules.txt` and `tools/stage_wine.sh` lays it out;
regenerate it the same way if Wine is updated.

The installation is off by default, because 64MB of initrd on every test
run is not a trade worth making:

```
make -f Makefile.amd64 WINE64_INSTALL=1 iso
```

### Where the unix libraries go, and how that was settled

Not in `lib/wine/x86_64-unix`, which is where an *installed* Wine keeps
them, but **beside the loader** — a Wine running from its build tree
reads `/proc/self/exe`, takes the directory, and appends the library
name. The wrong layout produces exactly one line of output:

```
wine: could not load ntdll.so: /usr/bin/ntdll.so: cannot open shared
object file: No such file or directory
```

which names the path it wanted. Worth remembering as a technique: Wine's
failures tend to name the thing they could not find.

### The bug that mattered: execve could not run a dynamic program

With ntdll.so found, Wine got further and died at `rip=0x6`. The syscall
trace said why, once `execve` was made to print its path:

```
execve "/usr/bin/i386-unix/wine-preloader"  = -ENOENT
execve "/usr/bin/i386-unix/wine"            = -ENOENT
execve "/usr/bin/wine-preloader"            = -ENOENT
execve "/usr/bin/wine"                      → fault at rip=6
```

Wine's loader re-execs itself, tries four candidates, and the fourth
exists. So `execve` succeeded and the program died immediately.

`do_execve` called `elf64_load` and entered at `e_entry`. Every program
it had ever run was static, so that was right. **A PIE that names an
interpreter asks to be placed at a bias and expects `ld.so` to be mapped
alongside it and entered instead**; jumping to its own `e_entry` lands
in unrelocated nonsense, which is what a single-digit RIP is.

It now reads `e_type` from the header — before the load, because the
bias has to be chosen first — loads the interpreter named in
`PT_INTERP`, passes its base as `AT_BASE`, and enters *ld.so*. Every
piece was already in the tree; layer 25 had been doing it by hand for
the one program that mattered, and `execve` had never been taught.

### The bug that only appeared at 6GB

`pmm64_owns` answered "is this a frame this allocator manages" with
`frame < frame_count`, and copy-on-write asks it about every page it
shares — the framebuffer is mapped into processes and must never be
refcounted or copied, because a private copy of VRAM is a window onto
nothing.

VRAM sits at `0xFD000000`. With 2GB the bitmap stops below it and the
question answers *no* by accident. With 6GB it stops above it and the
same question answers *yes*, so a forked process's framebuffer would
have been quietly copied away from the screen.

The allocation bitmap cannot answer it: it starts with everything used
and only clears what the memory map vouched for, so a hole in the map
and a frame in use are the same bit. There is now a separate record of
which frames are *memory*, set once from the memory map and never
cleared — reserving a region takes it out of the allocator, not out of
existence.

**This is the third time raising QEMU's RAM has found a real bug**
(Milestone 66's bitmap placement, this one, and the assertion that
caught it). `make -f Makefile.amd64 test QEMU_RAM=6G` is not optional.

### How far Wine actually gets now

Deep into `ntdll.so`'s own initialisation. It opens `/dev/null`, asks
for `membarrier`, and makes its address-space reservations —
`0x10000`, `0x7f000000`, `0x7ffffe000000`, all `MAP_FIXED |
MAP_NORESERVE` — which are ntdll's virtual memory layout going in. Then
it calls through a null pointer.

Two things that are *not* the cause, both checked:

- **It is not a missing syscall.** Three are unimplemented in the whole
  run: `prlimit64` (302), `rseq` (334) and `membarrier` (323). All
  three are absent on older Linux kernels too and both glibc and Wine
  handle that.
- **It is not the PE modules.** Wine never opens a single one — zero
  reads under `x86_64-windows` — so it dies before it looks for them.
  The 93 modules are staged and untouched.

So the next thing to find out is what in ntdll's init is being called
through a null pointer, and that is where the next session starts.

### Verified

15 host/guest differentials and 0 failures at **2G and 6G**, with the
Wine installation both present and absent.

### Not claimed

No prefix. No `wineboot` completing. No Windows program running under
Wine. `prefix = not created, system32 absent` is printed by the layer
itself on every run, and it is printed from a filesystem lookup rather
than from a variable, so it cannot drift away from the truth.

## Milestone 72 — the prefix, and three syscalls that lied ✅ DONE

**A Wine prefix now exists on this kernel.** `wineboot -u` creates
`/root/.wine`, `chdir`s into it, makes `dosdevices` and `drive_c`, and
links them together — the thing Milestone 71 said plainly had never
happened. It does not finish: it stops trying to launch the wineserver,
which is a different and later problem, recorded at the end.

Nothing here is a new feature. All four fixes are the same shape — a
syscall that reported success without doing what it said — and that is
the shape this tree keeps producing. What is worth recording is that
none of them faulted anywhere near where they were wrong.

### 1. `MAP_FIXED_NOREPLACE` was granted instead of refused

This is Milestone 71's "calls through a null pointer", and it was never
a null pointer.

Wine reserves its address space without a preloader by asking for three
ranges `PROT_NONE`, the last being
`0x7ffffe000000..0x7fffffff0000`. This kernel puts the user stack at
`0x7fffffff0000` and 128 pages below it — **inside that range**. On
Linux the call therefore fails, and `reserve_area` is written for that:
it halves the range and recurses, reserving what it can around the
obstacle.

The flag is `MAP_FIXED_NOREPLACE` (0x100000) and it **does not come with
`MAP_FIXED`** — it implies fixed placement on its own. So the request
missed the `MAP_FIXED` branch entirely and fell into the *hint* path,
which probed a single page, found it free, and mapped all 32MB. Then
`map_anon` did the thing it exists to do: a page already mapped in the
range is kept but **zeroed**, because an anonymous mapping must read as
zeros.

So Wine zeroed all 128 pages of its own stack. Nothing faulted. Every
saved return address and every saved register on that stack was now 0,
and execution carried on until the first `ret` popped a zero and jumped
to address 0. The fault was reported at `rip=0 cr2=0` with `rdi`/`rsi`
still holding the arguments of the mmap that had caused it, four calls
earlier.

`mmap` now honours the flag: the **whole** range must be free, and a
refusal returns `-EEXIST` having changed nothing. The plain-hint path
was checking one page for the same reason and now checks the range too.

### 2. An `mmap` above the user half was granted

Wine does not assume how big the address space is, it measures it
(`get_host_addr_space_limit`): mmap one page at `1<<63`, halve the
address until the kernel accepts it, and call twice the first address
that worked the limit.

This kernel accepted `0x8000000000000000` on the first try — a
non-canonical address in the kernel's half — so Wine concluded the user
address space ended at `0xffffffffffff0000` and sized its page
protection table from it:

```
pages_vprot_size = (limit >> 12 >> 20) + 1;
size = 2 * view_block_size + pages_vprot_size * sizeof(*pages_vprot);
```

which is **32GB**, where Linux asks for 2.2MB. That allocation failed
and `virtual_init` died on its own `assert`. The 32GB was a symptom;
the missing bounds check was the bug.

`USPACE64_LIMIT` is now `0x0000800000000000` and `mmap` refuses anything
that does not fit wholly below it, with the length checked as a
subtraction so a wrapping request is refused rather than wrapping into a
range that looks fine. The probe now stops at `0x400000000000` and
reports `0x7fffffff0000`, which is what Linux reports — asserted by
comparing the two.

### 3. `mprotect` was accepted and ignored

The old comment reasoned that every mapping is already readable and
writable, so the only thing left to implement was *removing*
permissions, and nothing needed that yet. It was wrong in the other
direction: pages get made read-only — by a loader's file mapping, or by
`fork` leaving them shared — and `mprotect` is how they are made
writable again. Returning 0 without touching a page table turns the
next store into an unexplained fault a long way from the call.

`uspace64_protect` now does it, and it is careful about one case: a
copy-on-write page must **not** simply have its write bit set, or a
parent and child that `fork` separated would share a writable frame.
The write bit stays clear and the request is recorded in
`PAGE64_COW_RW`, which is exactly the bit `break_cow` already consults
to tell a page it should copy from a page that is genuinely read-only.
That lives in `vmspace64_set_writable`, next to the COW code rather than
in `uspace64.c`.

What it still does not do is honour `PROT_NONE` or `PROT_EXEC`, and that
is a stated limit rather than an oversight. The only record this kernel
keeps of which ranges are taken is the page tables themselves, so
unmapping a `PROT_NONE` range would make Wine's own reservations read as
free and the next `MAP_FIXED_NOREPLACE` would be offered them. Reads
that ought to fault therefore succeed.

### 4. `getuid` was missing, and wineboot refused a directory it owned

```
wine: '/root' is not owned by you, refusing to create a configuration
directory there
```

from `setup_config_dir`, which does `if (!stat(config_dir, &st) &&
st.st_uid != getuid()) fatal_error(...)`. `do_stat` reports every file
as owned by uid 0. `getuid` was unimplemented, and it is one of the
syscalls Linux specifies as never failing, so glibc does not check it
and handed `-ENOSYS` back as a uid. `getuid`, `geteuid`, `getgid` and
`getegid` now return 0 and agree with what `stat` says.

### The test, and the falsification

`userland/vmem64.c` is a host/guest differential — 15 assertions, exit
101 — and it is deliberately not this tree's opinion about what should
happen: every expected answer is what Linux does, including the measured
address-space limit, which it computes with Wine's own probe copied out
of `virtual.c`. It asserts that a refused `MAP_FIXED_NOREPLACE` leaves
the memory it was refused byte-for-byte intact, which is the assertion
the stack corruption reduces to, and it checks a range overlapping by a
single page, which is the case a one-page probe gets wrong.

Each mechanism was removed in turn and the test failed on the assertion
for that mechanism and no other:

| removed | what failed |
| --- | --- |
| the `range_free` check | `MAP_FIXED_NOREPLACE over a mapped range is refused` |
| `USPACE64_LIMIT` | `a fixed mapping at 1<<63 is refused`, and `limit` diverged |
| real `mprotect` | `and the page really is read-only now` |

308 assertions, 16 differentials, 0 failures, green at 2G and 6G.

### Where Wine stops now

Much later, and for a reason that is a feature rather than a lie. Having
built the prefix it tries to start the **wineserver**:

```
clone3(...)                        = -ENOSYS      (glibc falls back)
clone(CLONE_VM|CLONE_VFORK|SIGCHLD) = 2
wait4(2, ...)                      = -ECHILD
exit_group(1)
```

That is `posix_spawn`'s vfork path. `clone` returns a tid but no process
the parent can wait for, so `wait4` says there is no such child. **vfork
— a child that shares its parent's address space until it `execve`s,
with the parent suspended — is the next thing to build**, and the
wineserver is what needs it.

Five syscalls are still unimplemented in a full run, and only the first
matters: `clone3` (435), `fcntl` (72), `prlimit64` (302), `rseq` (334),
`userfaultfd` (323).


## Where chrome.exe actually is from here

Worth stating plainly, because the milestones are accumulating and the
target is not obviously closer.

**The Win32 API surface is not the path.** `win32_64.c` implements seven
functions. Milestone 45 measured `chrome.dll` as importing **1316**
across 67 DLLs. Hand-writing that is not a plan, and it never was - the
plan is Wine, which Milestone 45 also measured as already implementing
all but eight of them.

**So the target is Wine, not Chrome**, and what Wine needs from a kernel
is the Linux side of this tree rather than the Windows side:

1. ~~**Threads** (`clone`)~~ - done in Milestone 55; thread exit and
   futex in 56 and 57.
2. ~~**Signals**, delivered~~ - done in Milestone 58, with Linux's exact
   `rt_sigframe` layout. Wine's exception dispatch is built on them.
3. ~~**A real filesystem**: Wine reads a prefix of thousands of files
   and writes to it.~~ - writable in Milestone 59, a real tree in 65,
   and sized against a measured prefix in 68. Not yet holding one.
4. ~~**File-backed `mmap`**, which is how Wine maps a PE at all.~~ -
   done in Milestone 60.
5. ~~**Wine itself reconfigured** `--enable-archs=x86_64`~~ - it
   configures cleanly (Milestone 55), it compiles, and since Milestone
   63 its loader answers `wine --version` -> wine-11.0, exit 0.

That list is now finished, which is worth saying plainly and worth not
over-reading: it was the list of what Wine needs *to start*, and Wine
does start. What stands between here and a Windows program is the next
list, and it is shorter than the one above but not smaller:

1. ~~**A Wine prefix, created on this kernel.**~~ - done in Milestone
   72. `wineboot -u` creates `/root/.wine`, `dosdevices` and `drive_c`
   and links them. Milestone 71's "calls through a null pointer" was an
   `mmap` that granted a `MAP_FIXED_NOREPLACE` it should have refused
   and zeroed Wine's own stack; the jump to 0 was a later `ret`.

   The prefix is created, not finished: wineboot then tries to start the
   **wineserver** and cannot, because `clone` with
   `CLONE_VM|CLONE_VFORK` returns a tid but no waitable child.
   **vfork is the next thing to build**, and it is what the whole
   Windows side now waits on - there is no prefix content, and no PE
   module is loaded, until a wineserver runs.
2. ~~**Copy-on-write `fork`.**~~ - done in Milestone 69. An 8MB process
   forks for 14 frames.
3. ~~**Keyboard and mouse.**~~ - done in Milestone 70, as
   `/dev/input/event0` and `event1` carrying Linux's `struct
   input_event`, and tested with real keystrokes through the QEMU
   monitor.
4. **A compositor, double buffering, write-combining.** `gfx.c`, `wm.c`
   and `console.c` have no 64-bit counterpart.
5. **The other 509 Wine DLLs.** 93 of the 602 are staged, which is what
   creating a prefix was measured to need - the rest is what *running
   things* needs, and that number is not yet measured.

Then, and only then, Chrome's own requirements: a GPU it has a flag to
do without, and a sandbox it has a flag to do without.

The honest distance: the Windows-side loader work (52, 53, 54) was real
and necessary, but the bulk has been Linux-side throughout, and it is
still measured in milestones rather than in sessions.

## The state of the tree, as found

Two things that belong to neither milestone, recorded because the next
session will hit both.

**`8bfc6d6` did not build.** `kernel/kheap.c` calls `ku32_to_dec` for its
allocation counters and does not include `kstring.h`, where it is
declared. On a compiler that treats an implicit declaration as an error —
which any current gcc does — `make` stops at `build/kheap.o`. It is a
one-line include and it is fixed, but the merge on the default branch was
in that state, so the counters added in `374896a`/`56a4a6e` were never
compiled by whoever merged them.

**`make test-posix` fails, and did before any of this.** It stops in
section 3a of the mmap test with:

```
FAIL: the binary produced no recognisable output on the host
```

The failure is on the **host** side of the differential, not on Novaris —
the Linux run of `userland/mmap_test.c` produces nothing to compare
against, so the comparison never reaches the guest. Verified against a
pristine export of `8bfc6d6` with only the `kheap.c` include added: same
failure, same place. It is environmental or a real host-side bug in that
program, and either way it is not a regression from Milestones 42 or 43.

`make`, `make test`, `make test-qemu` and `make test-wm` all pass.
`make test-desktop`, `test-wine`, `test-wine-gui` and `test-winsock`
require `WINE_BUILD` and are untested here.

## Later / open-ended

- ~~Networking (a NIC driver + a minimal TCP/IP stack)~~ — done in
  Milestones 38-41, up to and including AF_INET sockets a ring-3 process
  can call, streams and datagrams both. What is still missing above it:
  TLS, raw sockets, IPv6, and a libc with getaddrinfo in it.
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
       qemu-system-x86 build-essential gcc-multilib mingw-w64 flex bison
   make && make test && make test-qemu && make test-posix
   ```

   And for the Wine half, which needs a built Wine tree (`flex` and
   `bison` are Wine's, not Novaris's):

   ```bash
   git clone --depth 1 -b stable https://github.com/wine-mirror/wine
   cd wine && CC="gcc -m32" ./configure --enable-archs=i386 \
       --disable-tests --without-x --with-freetype --without-vulkan \
       --without-opengl && make -j4
   cd ../NovarisOS && make WINE_BUILD=../wine
   make test-wine test-wine-threads test-wine-gui
   ```

   `--with-freetype` is not optional if you want windows: without a font
   engine Wine's caption metrics are uninitialised stack, and the window
   frame is measured from them. `apt-get install libfreetype-dev:i386`
   first (`dpkg --add-architecture i386` if it is not enabled). See
   Milestone 37.

   `make` also builds Novaris's Wine display driver and installs it, by
   grafting `wine/winenovaris.drv/` into the Wine tree
   (`tools/build_wine_driver.sh`). That modifies the tree - one
   `WINE_CONFIG_MAKEFILE` line in `configure.ac`, one `sed` in
   `programs/explorer/desktop.c` - and both edits are idempotent, so a
   second `make` is an incremental build rather than a reconfigure.

   Since Milestone 35 there is one ISO and Wine is installed into it, so
   `WINE_BUILD` is a variable to `make` rather than an argument to a
   separate target. Set it once (`export WINE_BUILD=../wine`) and the
   ordinary `make` builds an OS with Wine in it.

   And, since Milestone 32, the disk. `make disk` makes an empty one and
   `make run-disk` boots with it attached; `make test-qemu-disk` is the
   write-reboot-read test. `make test-wine-prefix` makes an empty one of
   its own and lets Wine build its prefix on it - since Milestone 35 that
   needs no host-built prefix and no extra variables.

   `test-wine-prefix` is the long one: it boots a whole Wine on an
   emulated machine and a run takes minutes, most of it wineboot. It stops
   as soon as its assertions are all satisfied, so a passing run is much
   shorter than its fifteen-minute budget and only a failing one spends
   the lot.

   `mingw-w64` builds the Windows test programs in `userland/pe_test/`;
   `gcc-multilib` builds the host-side tests in `tests/`. Since Milestone
   10 the kernel mirrors its console to COM1, so verification no longer
   means OCR'ing framebuffer screenshots — `make test-qemu` boots the
   ISO, drives the shell through the QEMU monitor, and matches the serial
   transcript against expected output.
