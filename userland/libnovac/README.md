# libNovaC

NovarisOS's own C library: a small, freestanding C11 library for static
x86-64 programs that run on NovarisOS. Programs linked against it need no
host libc at all - libNovaC brings its own start-up code (`crt0.o`) and
talks to the kernel through raw system calls.

NovarisOS implements the Linux x86-64 system-call ABI, so a libNovaC
program is also an ordinary static Linux executable. That is how it is
tested: the same binary runs on the build host and inside NovarisOS, and
the two transcripts have to match.

Version 0.2.

## What it provides

| Header | Contents |
| --- | --- |
| `<stdio.h>` | `FILE` streams with buffering, `printf` family, `fgets`, `fseek`, `perror`, `remove`, `rename` |
| `<stdlib.h>` | `malloc`/`calloc`/`realloc`/`free`, `exit`/`atexit`/`abort`, `getenv`/`setenv`/`unsetenv`, `strto*`, `qsort`, `bsearch`, `rand` |
| `<string.h>` | the `mem*` and `str*` functions, `strerror` |
| `<ctype.h>` | the classification and case functions (C locale) |
| `<unistd.h>` | file and process calls: `read`, `write`, `lseek`, `pipe`, `dup2`, `fork`, `execve`/`execv`/`execvp`, `getcwd`, `sleep`, ... |
| `<fcntl.h>`, `<sys/stat.h>`, `<dirent.h>` | `open`, `stat`/`fstat`/`lstat`, `mkdir`, `opendir`/`readdir` |
| `<sys/wait.h>` | `wait`, `waitpid` and the status macros |
| `<signal.h>` | `signal`, `sigaction` (with `SA_SIGINFO`), `raise`, `kill`, `sigprocmask` and the `sigset_t` operations |
| `<time.h>` | `time`, `clock_gettime`, `nanosleep`, `gmtime`, `timegm`/`mktime` (UTC) |
| `<errno.h>`, `<limits.h>`, `<assert.h>`, `<sys/types.h>` | as expected; errno values are Linux's |

## Layout

- `src/` - the library. The portable core (`string`, `ctype`, `stdlib`,
  formatting) reaches the kernel only through the three hooks in
  `src/internal/sys.h` (map, unmap, exit); `src/sys/novaris/sys.c`
  implements them. Everything else that talks to the kernel goes through
  `src/internal/syscall.h`, the one place a `-errno` becomes `-1` and
  `errno`.
- `src/arch/x86_64/` - `_start`, the `syscall` instruction, and the syscall
  numbers, which `make check-sysnum` compares with the kernel's
  `include/syscall64.h`.
- `tests/host/` - unit tests of the portable core, run on the host under
  ASan and UBSan with symbols renamed so they cannot clash with the host
  libc.
- `tests/target/` - the target test, a static program that uses only
  libNovaC, and `novac_child`, the program it execs.

## Building and testing

```bash
make host-test      # portable-core unit tests on the host
make lib            # build/target/libNovaC.a and crt0.o
make target-test    # the target test, run on this (Linux x86-64) host
make check-sysnum   # syscall numbers against ../../include/syscall64.h
make example        # compile-check examples/demo.c
```

To link a program against it:

```bash
gcc -std=c11 -ffreestanding -fno-builtin -nostdinc -isystem include \
    -isystem "$(gcc -print-file-name=include)" -c prog.c
ld -m elf_x86_64 -static -nostdlib -e _start -o prog \
    build/target/crt0.o prog.o build/target/libNovaC.a
```

## In NovarisOS

`Makefile.amd64` builds libNovaC with this Makefile (into
`build64/libnovac`), checks its syscall numbers first, embeds the target
test in the kernel and stages `novac_child` at `/tmp/novac_child` in the
initrd. The bring-up runs the target test as the "libNovaC" layer, with
the timer on, and expects exit status 131. `make -f Makefile.amd64 test`
then runs the same binary on the host and requires every line it prints
there to appear in the guest's transcript too.

## Changes in 0.2

Written against an earlier kernel, 0.1 had no way to use what NovarisOS
can now do. 0.2 adds:

- **Signals** - `<signal.h>`, with a `rt_sigreturn` trampoline as the
  handlers' return address (`SA_RESTORER`), so a handler that returns
  resumes the interrupted code.
- **`abort()` by `SIGABRT`.** 0.1 exited with status 134, so a parent saw
  a normal exit. `abort()` now unblocks and raises `SIGABRT`; if a handler
  catches it and returns, the default action is restored and it is raised
  again. The exit status remains only as a fallback, and is all the host
  unit tests (which have no signals) see.
- **`execve`, `execv` and `execvp`**, the last searching `PATH` (default
  `/bin:/usr/bin`).
- **`setenv` and `unsetenv`.** The environment is copied into memory of
  its own the first time it changes.
- `ESRCH`, `ETXTBSY`, `EDEADLK`, `ENOLCK`, and `PATH_MAX`.

Running the target test inside NovarisOS for the first time found four
places where the kernel differed from Linux, all fixed in the kernel:
`mkdir` on an existing path succeeded instead of failing with `EEXIST`;
`open` ignored `O_DIRECTORY` (and `O_EXCL`); and the `siginfo` of a
signal sent with `kill` or `tgkill` said `SEGV_MAPERR` instead of
`SI_USER`/`SI_TKILL` and carried no `si_pid`.
