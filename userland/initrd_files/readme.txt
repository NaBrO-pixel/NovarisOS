Welcome to the Novaris initrd!

This file was loaded from a custom archive format (see
userland/mkinitrd.py) via a GRUB multiboot module, parsed by
kernel/ramfs.c, and is being read right now through the generic VFS
layer in kernel/vfs.c. Since Milestone 26 that filesystem is writable
and has real directories - try `mkdir /tmp/d`, `cp readme.txt /tmp/d/x`,
`ls /tmp/d`, `rm /tmp/d/x`.

Native Novaris programs:
  ls [dir]         - list a directory
  cat hello.txt    - print a file to the screen
  run helloc.bin   - load and execute a flat-binary ring-3 program
  run helloelf.elf - load and execute a *real ELF executable*, which
                     also exercises the mmap-equivalent syscall
                     (see ROADMAP.md's Milestone 8 notes)

Windows programs. Every .exe below except hellope.exe was compiled by
mingw-w64 against the real Windows headers and import libraries - none
of them is written for Novaris, and all of them would run unchanged on
Windows. See ROADMAP.md's Milestone 10.

  run hellowin.exe - an ordinary C program: printf in every form, the
                     heap, strings, recursion, an exit code
  run winapi.exe   - calls kernel32 and user32 directly and prints back
                     what each one returned
  run cppinit.exe  - C++, whose global constructor must run before main
  run lowbase.exe  - the same program as hellowin.exe, but linked at an
                     address Novaris won't hand out, so the loader has
                     to relocate it. The output should be identical
                     except for the printed pointer
  run guiapp.exe   - a GUI-subsystem program. Its message box works;
                     CreateWindowEx honestly fails, since there is no
                     window manager
  run crash.exe    - dereferences a null pointer on purpose. Should
                     report an access violation and return to the shell
  run hello64.exe  - a 64-bit binary, which a 32-bit kernel can never
                     run. Should say so specifically
  run hellope.exe  - the hand-written import-free PE from Milestone 8b

Inspecting them:
  peinfo hellowin.exe  - headers, and which imports resolve
  winapi               - the emulated DLLs and how much of each exists
