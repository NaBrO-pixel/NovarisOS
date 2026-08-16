#ifndef USPACE64_H
#define USPACE64_H

#include <stdint.h>
#include "vmspace64.h"
#include "elf64.h"

/* The parts of a Linux process that are not the code: its heap, its
 * anonymous mappings, and the stack the kernel hands it at entry.
 *
 * There is one of these at a time. A real kernel keeps this per process,
 * next to the address space; this is a single static because the 64-bit
 * tree runs one Linux program at a time and pretending otherwise would
 * be structure without a user. */

/* Where mmap hands out addresses from. Well clear of the program (ld puts
 * it at 0x400000) and of the initial stack near the top of the low half. */
#define USPACE64_MMAP_BASE 0x0000100000000000ULL

void uspace64_reset(vmspace64_t* space, uint64_t brk_start);

/* brk(0) reports the break; brk(addr) moves it and returns where it
 * ended up. Linux returns the *current* break on failure rather than an
 * error, which is what glibc's malloc checks for. */
uint64_t uspace64_brk(uint64_t addr);

/* Anonymous, private mappings only. Returns the address, or -ENOMEM. */
uint64_t uspace64_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                       uint64_t flags);
uint64_t uspace64_munmap(uint64_t addr, uint64_t length);

/* Narrows an existing mapping's permissions. Used by the file-mapping
 * path: the kernel has to write the file's bytes in, so the pages are
 * mapped writable and then closed down if the caller asked for
 * PROT_READ alone. */
void uspace64_protect(uint64_t addr, uint64_t length, uint64_t prot);

/* Builds argc/argv/envp/auxv at the top of the mapped stack and returns
 * the rsp the program should start on. `stack_top` is one past the last
 * mapped byte; the pages below it must already be mapped writable. */
/* `interp_base` is where the dynamic loader was mapped, and becomes
 * AT_BASE. Zero for a static binary. */
uint64_t uspace64_build_stack(vmspace64_t* space, uint64_t stack_top,
                              uint64_t stack_pages, const char* argv0,
                              const elf64_info_t* elf,
                              uint64_t interp_base);

uint64_t uspace64_pages_allocated(void);

#endif
