#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
/* Bit 9 is one of the three the CPU leaves to the OS, and Milestone 30
 * spends it on the one fact a page table otherwise cannot record: this
 * frame is mapped here but is not this address space's to free.
 *
 * A MAP_SHARED file mapping hands user space the very frames the file's
 * bytes live in, so tearing the process down must unmap them and stop -
 * freeing them would hand the file's contents back to the physical
 * allocator while the file still exists. Every teardown path checks it:
 * munmap, process exit, execve, and the destruction of an address space.
 * fork checks it too, and *shares* rather than copies, which is what
 * makes a shared mapping shared across a fork. */
#define PAGE_SHARED  0x200

/* Bit 10, and the other fact a page table cannot otherwise record: this
 * address is *spoken for* but has no memory behind it.
 *
 * PROT_NONE means "reserve the address range and give me nothing", and
 * Novaris used to answer it by allocating a frame per page and marking it
 * unreadable - which is the same observable behaviour and a completely
 * different cost. Wine's preloader asks for 2GB of it before it does
 * anything else, so on a 512MB machine the reservation succeeded, ate
 * every frame there was, and the loader ground to a halt on the next
 * allocation.
 *
 * A reserved entry is *not present*, so an access to it faults exactly as
 * PROT_NONE should. What the bit adds is the ability to tell "reserved"
 * from "free" afterwards - which is what mprotect needs when the program
 * comes back and asks for the memory it reserved. */
#define PAGE_RESERVED 0x400

/* Bit 11, the last one the CPU leaves to the OS, and the one Milestone 34
 * needed: this page is a *private* view of memory that belongs to
 * somebody else - a file's storage - and a write to it has to be given a
 * copy first.
 *
 * That is what MAP_PRIVATE on a file actually means. Novaris used to
 * answer it by allocating pages and reading the bytes in, which is a
 * snapshot: correct about this process's stores staying here, wrong about
 * everything else, because until a page is written the mapping is the
 * file's own memory and a writer elsewhere shows through it. Wine takes
 * that deliberately - it maps a read-only view of a section with
 * MAP_PRIVATE and says so in a comment - and win32u's shared session
 * block is exactly such a view. See ROADMAP.md Milestone 34.
 *
 * A COW page carries PAGE_SHARED too, because until it is written the
 * frame really is the file's and no teardown path may free it. */
#define PAGE_COW      0x800

/* And the fact a COW page still has to record: whether the mapping it
 * belongs to was made writable. Both kinds have PAGE_RW clear, because
 * both have to fault on a write - one to be given its copy, the other
 * because writing to a read-only mapping is an access violation and
 * Novaris says so with SIGSEGV.
 *
 * Bit 10 again, which is free here: PAGE_RESERVED only ever means
 * anything on an entry that is *not present*, and a COW page is present.
 * The CPU consults bit 0 first, so the two readings never overlap. */
#define PAGE_COW_RW   0x400

/* Where the scratch page used to copy an address space lives. In the
 * kernel half deliberately: it was in the user half until Milestone 30,
 * where it was a page a process teardown would have freed if a copy had
 * ever been interrupted between mapping and unmapping it. kernel.c
 * reserves its page table before the kernel address space is frozen, so
 * every address space has it. */
#define PAGING_SCRATCH_VA 0xE0000000u

/* Where the initrd is mapped. In the kernel half since Milestone 30,
 * because GRUB puts modules just above the kernel image - across
 * 0x400000, which is where a Windows executable wants to load, and the
 * first thing real Wine asked this kernel for. The initrd only ever
 * needed to be reachable, not to be at its physical address. */
#define INITRD_VIRTUAL_BASE 0xE1000000u
#define INITRD_VIRTUAL_MAX  (192u * 1024u * 1024u)

/* Where DMA buffers are mapped, just past the initrd's window.
 *
 * Milestone 38. A network card writes received frames into memory itself,
 * so its buffer has to be a physically contiguous run whose physical
 * address the card is told. Everything else this kernel allocates is
 * virtual and scattered - kmalloc hands back addresses in the heap whose
 * physical frames are wherever the PMM had them - which is exactly what a
 * device cannot use.
 *
 * So: pmm_alloc_contiguous() for the physical run, and this window to
 * look at it from. 1MB, which is thirty times what the one driver here
 * asks for. */
#define DMA_VIRTUAL_BASE 0xEE000000u
#define DMA_VIRTUAL_MAX  (1u * 1024u * 1024u)

/* Reserves a physically contiguous run of `bytes` and maps it into the
 * DMA window. Writes the physical address through `phys_out` - which is
 * the whole point, since that is the number the device is given. Returns
 * the kernel virtual address, or 0. Uncached is not needed here: x86 is
 * cache-coherent with bus-master DMA. */
void* paging_alloc_dma(uint32_t bytes, uint32_t* phys_out);

/* Replaces boot.s's coarse 4MB-page bootstrap mapping with a real
 * 4KB-granularity page directory: identity-maps the first 4MB (so the
 * VGA buffer and low-memory structures keep working) and maps the
 * kernel's own physical image at its higher-half virtual addresses.
 * Must run after pmm_init(), since it allocates page-table frames
 * through the physical memory manager. */
void paging_init(uint32_t kernel_physical_start, uint32_t kernel_physical_end);

/* Maps a single 4KB page. virt_addr and phys_addr must both be
 * page-aligned. Allocates a new page table via the PMM if needed. */
void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/* Removes a mapping and invalidates the TLB entry for it. Does not free
 * the underlying physical frame - call pmm_free_frame() separately if
 * that's what you want. */
void paging_unmap_page(uint32_t virt_addr);

/* Raw page-table-entry access, added for the PE loader (kernel/pe.c).
 * A Windows .exe wants to be mapped at its own ImageBase - classically
 * 0x400000, which lands inside the first 4MB that paging_init()
 * identity-maps. Rather than refuse to honor ImageBase, the loader saves
 * whatever PTEs it is about to overwrite and puts them back when the
 * program exits, so borrowing that address range is invisible to the rest
 * of the kernel. paging_get_entry() returns 0 for an address whose page
 * table doesn't exist yet, which paging_set_entry() treats as "unmap". */
uint32_t paging_get_entry(uint32_t virt_addr);
void paging_set_entry(uint32_t virt_addr, uint32_t entry);

/* Virtual-address reservations. Some identity mappings genuinely cannot
 * be borrowed the way the previous paragraph describes, because the
 * kernel still dereferences them while a user program runs - the initrd
 * (a program that reads a file goes through it) and the framebuffer
 * (every terminal_putchar draws into it) are the two that matter.
 * kernel.c registers those, the Win32 layer registers its own arenas, and
 * the PE loader checks candidate load addresses against the list before
 * mapping anything. Regions are half-open [start, end). */
#define PAGING_MAX_RESERVATIONS 16
void paging_reserve_region(uint32_t start, uint32_t end, const char* name);

/* Returns the name of the first reserved region overlapping
 * [start, end), or 0 if the range is clear. */
const char* paging_region_conflict(uint32_t start, uint32_t end);

/* --- address spaces ------------------------------------------------------
 *
 * Milestone 14. Through Milestone 13 there was exactly one page directory
 * and every program ran in it, which is why the Win32 layer can read a
 * program's stack by casting a pointer. These functions add the other
 * half: as many directories as you like, each with a private user half
 * and a kernel half identical to every other one's.
 *
 * An address space is identified by the physical address of its page
 * directory - the value that goes in CR3 - rather than by a struct, so
 * there is nothing to allocate, free or keep in step, and
 * paging_current_address_space() can answer by reading the register.
 *
 * The order of operations at boot matters:
 *
 *   1. paging_init()                     - build the kernel directory
 *   2. ... map the heap, initrd, framebuffer, whatever else is kernel-wide
 *   3. paging_reserve_kernel_tables()    - for kernel ranges that grow later
 *   4. paging_finalize_kernel_space()    - freeze what "kernel" means
 *   5. paging_create_address_space()     - any number of times, afterwards
 */

/* Pre-creates the page tables covering [start, end) in the kernel
 * directory, so that later mappings into that range only fill in page
 * table entries and never add a directory entry. Call before
 * paging_finalize_kernel_space() for every kernel range that can grow
 * after boot - the kernel heap being the one that does. */
void paging_reserve_kernel_tables(uint32_t start, uint32_t end);

/* Freezes the current set of directory entries as the shared kernel set.
 * Every address space created afterwards gets exactly these, which is
 * what makes the kernel reachable at the same addresses in all of them -
 * including from an interrupt that arrives while a user address space is
 * loaded. Call once, at the end of boot-time kernel mapping. */
void paging_finalize_kernel_space(void);

/* Non-zero if the kernel tried to add a directory entry to its own half
 * after finalizing - i.e. a kernel mapping that address spaces created
 * before it would not have. Surfaced by the `vmtest` shell command rather
 * than being made fatal, since it is a design error to find and fix, not
 * a runtime condition to recover from. */
int paging_kernel_pde_violated(void);

/* True if every page table covering [start, end) is private to the
 * current address space - i.e. mapping something there affects only this
 * process. A process's own mappings *must* satisfy this: a page table in
 * the shared kernel set is shared by every address space, so writing an
 * entry into one would hand the mapping to the kernel and to every other
 * process too. The shared set is 4MB-granular, so a range can fail this
 * while being nowhere near anything the kernel actually uses. */
int paging_range_is_private(uint32_t start, uint32_t end);

/* Index of the first shared directory entry at or after `from`, or 1024
 * if there is none. For reporting the layout; see the `vmtest` command. */
uint32_t paging_next_global_pde(uint32_t from);

/* Allocates a page directory whose kernel half is the frozen shared set
 * and whose user half is empty, with its own recursive mapping in place.
 * Returns its physical address, or 0 (out of memory, or called before
 * finalizing). */
uint32_t paging_create_address_space(void);

/* Frees an address space: every page table in its private half, every
 * frame those tables map, and the directory itself. Shared kernel tables
 * are left alone. Refuses to destroy the kernel's own directory, or the
 * one currently loaded in CR3. */
void paging_destroy_address_space(uint32_t pd_phys);

/* Loads a page directory into CR3. */
void paging_switch_address_space(uint32_t pd_phys);

/* The directory currently in CR3, and the kernel's own. */
uint32_t paging_current_address_space(void);
uint32_t paging_kernel_address_space(void);

/* paging_map_page() / paging_get_entry() against an address space that
 * is *not* the one currently loaded, reached through a second recursive
 * directory slot rather than by switching CR3 - see the comment on
 * FOREIGN_PD_INDEX in paging.c. Passing the current address space (or 0)
 * falls through to the ordinary versions. paging_map_page_in() returns 0
 * if it needed a page table and no physical memory was left. */
int      paging_map_page_in(uint32_t pd_phys, uint32_t virt_addr,
                            uint32_t phys_addr, uint32_t flags);
uint32_t paging_get_entry_in(uint32_t pd_phys, uint32_t virt_addr);

/* --- whole-address-space operations (Milestone 29) -----------------------
 *
 * fork and execve are both "do something to every page a process owns",
 * and both need to walk the private half of a page directory rather than
 * one address at a time. Keeping the walk here keeps the recursive-mapping
 * knowledge in one file: what a caller says is "copy this process" or
 * "empty this process", not "index 1022 is the foreign window".
 *
 * "Private" means a directory entry that is not part of the frozen kernel
 * set - so the identity-mapped low 4MB, the kernel image, the heap, the
 * framebuffer and the Win32 arenas are all excluded automatically, and no
 * list of them has to be maintained here. */

/* Copies every private mapping of the *current* address space into
 * `dest_pd`, allocating a fresh frame per page and copying the bytes -
 * fork without copy-on-write, which this kernel has no fault machinery
 * for. Returns 1, or 0 if physical memory ran out partway (the caller is
 * expected to destroy `dest_pd`, which reclaims what was allocated). */
int paging_copy_user_space(uint32_t dest_pd);

/* Unmaps and frees every private page of the current address space, and
 * the page tables holding them. What execve does before loading a new
 * image, and what a process exit does so its frames come back
 * immediately rather than at the end of the batch. Returns the number of
 * frames handed back to the PMM.
 *
 * The caller must not touch user memory afterwards - which for both of
 * its callers is the point rather than a caveat. */
uint32_t paging_release_user_pages(void);

/* Marks `virt_addr` reserved: an address that belongs to the process but
 * has no frame behind it. Allocates a page table if the range has none.
 * Returns 0 only if that allocation failed. See PAGE_RESERVED. */
int paging_reserve_page(uint32_t virt_addr);

/* Gives a COW page (see PAGE_COW) a private frame with the same bytes in
 * it, mapped writable, and drops the two bits that said it was somebody
 * else's. Returns 1 if it did that, 0 if the page was not a COW page or
 * there was no memory left - and 0 is the caller's cue to treat the fault
 * as a fault. Safe to call on an address that is not page-aligned. */
int paging_break_cow(uint32_t virt_addr);

/* The highest address a program may map. Everything above is the
 * kernel's, shared by every address space, and a fixed mapping that
 * reached into it would hand the kernel's own memory to a program - or,
 * worse, hand it to every other process too. */
#define USER_SPACE_END 0xC0000000u

#endif
