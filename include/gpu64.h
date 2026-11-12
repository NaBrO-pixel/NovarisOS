#ifndef GPU64_H
#define GPU64_H

#include <stdint.h>

/* GPUs on the PCI bus (Milestone 112).
 *
 * Finding a GPU and saying what it is comes before driving one: which
 * vendor, which platform or ASIC, which generation, where its registers
 * and its memory are, and whether it is the one the firmware left the
 * boot framebuffer on. Intel's platforms and AMD's ASICs come from
 * tables generated out of Linux's MIT-licensed ID lists
 * (include/gpu64_ids.h, tools/gen_gpu_ids.py).
 */

#define GPU64_MAX 8

enum {
    GPU64_OTHER,
    GPU64_INTEL,       /* gen is the graphics generation */
    GPU64_AMD,         /* an amdgpu ASIC, or one amdgpu finds by IP discovery */
    GPU64_ATI_OLD,     /* an ATI/AMD part older than amdgpu, radeon's or before */
    GPU64_VIRTIO,      /* virtio-gpu */
    GPU64_QEMU_STD,    /* QEMU's standard VGA or bochs-display */
};

typedef struct {
    uint64_t base, size;
    uint8_t  io, is64, prefetch;
} gpu64_bar_t;

typedef struct {
    uint8_t     bus, slot, func;
    uint16_t    vendor, device;
    uint8_t     revision, subclass;
    int         kind;
    const char* vendor_name;
    const char* family;      /* "TGL", "SIENNA_CICHLID", ... or a description */
    int         gen;         /* Intel only */
    int         apu;         /* AMD only: the GPU of a processor */
    int         boot;        /* the boot framebuffer is in one of its BARs */
    gpu64_bar_t bar[6];
} gpu64_t;

/* Scans the bus. Returns how many display controllers there are. */
int  gpu64_scan(void);
int  gpu64_count(void);
const gpu64_t* gpu64_at(int i);
const gpu64_t* gpu64_find_kind(int kind);

/* The tables on their own, for the test layer: the family an ID is, or
 * 0 if the table does not have it. */
const char* gpu64_intel_family(uint16_t device, int* gen);
const char* gpu64_amd_family(uint16_t device, int* apu, int* old);

/* Maps `bytes` of a memory BAR from `offset`, uncached; a kernel virtual
 * address, or 0. */
volatile uint32_t* gpu64_map(const gpu64_t* g, int bar, uint64_t offset, uint64_t bytes);

/* --- Intel's display engine, read only ------------------------------------
 *
 * What the firmware set up on an Intel GPU: which pipe is lit, how big,
 * where its primary plane scans out of and with what stride. Reading
 * these registers is safe on any generation that has them; nothing is
 * written. The decoding is separate from the reading so that it can be
 * checked against a register file the test layer makes up - this build
 * machine has no Intel GPU, and QEMU cannot emulate one. */
typedef uint32_t (*gpu64_read_fn)(void* ctx, uint32_t offset);

typedef struct {
    int      found;
    int      pipe;            /* 0 = A */
    uint32_t width, height;
    uint32_t stride_bytes;
    uint32_t surface;         /* a GGTT address, not a physical one */
    int      linear;
    int      xrgb8888;
    uint32_t plane_ctl;
} gpu64_intel_plane_t;

void gpu64_intel_decode(int gen, int vlv_chv, gpu64_read_fn read, void* ctx,
                        gpu64_intel_plane_t* out);

void gpu64_report(void);

#endif
