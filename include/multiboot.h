#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Subset of the Multiboot 1 info structure - just what we use so far.
 * Full spec: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
 * Packed (and every field kept, even unused ones like drives_ and vbe_)
 * so the framebuffer_* fields land at the byte offset the bootloader
 * actually writes them to. */
typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;   /* KB of memory below 1MB */
    uint32_t mem_upper;   /* KB of memory above 1MB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6]; /* palette or RGB field-position/size info - unused here */
} __attribute__((packed)) multiboot_info_t;

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEMORY      (1 << 0)
#define MULTIBOOT_INFO_MODS        (1 << 3)
#define MULTIBOOT_INFO_MMAP        (1 << 6)
#define MULTIBOOT_INFO_FRAMEBUFFER (1 << 12)
#define MULTIBOOT_MEMORY_AVAILABLE 1
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB 1

/* One entry in the module list (mods_addr points to mods_count of
 * these) - GRUB's `module` directive in grub.cfg loads a file into
 * memory before the kernel and describes it with one of these. We use
 * this to ship the initrd. */
typedef struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;   /* pointer to a C string: the module's command line */
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

/* One variable-length entry in the Multiboot memory map (mmap_addr points
 * to a packed list of these; advance by entry->size + 4 bytes, since the
 * `size` field doesn't count itself). Lets the PMM know exactly which
 * physical regions are real usable RAM vs reserved/MMIO. */
typedef struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif
