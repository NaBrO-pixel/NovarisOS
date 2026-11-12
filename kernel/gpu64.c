/* gpu64.c - GPUs on the PCI bus (Milestone 112). See include/gpu64.h. */

#include "gpu64.h"
#include "gpu64_ids.h"
#include "pci.h"
#include "fb64.h"
#include "paging64.h"
#include "serial64.h"

static gpu64_t gpus[GPU64_MAX];
static int ngpus;

/* --- the tables ------------------------------------------------------------ */

const char* gpu64_intel_family(uint16_t device, int* gen) {
    for (unsigned i = 0; i < sizeof(gpu64_intel_ids) / sizeof(gpu64_intel_ids[0]); i++)
        if (gpu64_intel_ids[i].id == device) {
            const int p = gpu64_intel_ids[i].platform;
            if (gen) *gen = gpu64_intel_platforms[p].gen;
            return gpu64_intel_platforms[p].name;
        }
    return 0;
}

const char* gpu64_amd_family(uint16_t device, int* apu, int* old) {
    if (old) *old = 0;
    if (apu) *apu = 0;
    for (unsigned i = 0; i < sizeof(gpu64_amd_ids) / sizeof(gpu64_amd_ids[0]); i++)
        if (gpu64_amd_ids[i].id == device) {
            if (apu) *apu = gpu64_amd_ids[i].apu;
            return gpu64_amd_chips[gpu64_amd_ids[i].chip];
        }
    for (unsigned i = 0; i < sizeof(gpu64_amd_old_ids) / sizeof(gpu64_amd_old_ids[0]); i++)
        if (gpu64_amd_old_ids[i] == device) {
            if (old) *old = 1;
            return "pre-amdgpu";
        }
    return 0;
}

/* --- BARs -------------------------------------------------------------------
 *
 * The standard sizing dance - write all ones, read back which bits stuck,
 * put the address back - with memory and I/O decoding off while it
 * happens, as Linux does: a BAR that briefly reads as all ones is a BAR
 * that briefly claims every address, and the boot display is scanning
 * out of one of them. */
static void size_bars(gpu64_t* g) {
    uint16_t cmd = pci_config_read16(g->bus, g->slot, g->func, PCI_COMMAND);
    pci_config_write16(g->bus, g->slot, g->func, PCI_COMMAND,
                       cmd & (uint16_t)~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE));

    for (int i = 0; i < 6; i++) {
        uint8_t off = (uint8_t)(PCI_BAR0 + 4 * i);
        uint32_t orig = pci_config_read32(g->bus, g->slot, g->func, off), mask;
        gpu64_bar_t* b = &g->bar[i];

        b->base = b->size = 0;
        b->io = b->is64 = b->prefetch = 0;
        if (orig & 1) {
            pci_config_write32(g->bus, g->slot, g->func, off, 0xFFFFFFFFu);
            mask = pci_config_read32(g->bus, g->slot, g->func, off);
            pci_config_write32(g->bus, g->slot, g->func, off, orig);
            b->io = 1;
            b->base = orig & ~3u;
            mask &= 0xFFFCu;
            b->size = mask ? (uint64_t)((~mask + 1) & 0xFFFFu) : 0;
            continue;
        }
        b->prefetch = (orig >> 3) & 1;
        b->is64 = ((orig >> 1) & 3) == 2;
        pci_config_write32(g->bus, g->slot, g->func, off, 0xFFFFFFFFu);
        mask = pci_config_read32(g->bus, g->slot, g->func, off);
        pci_config_write32(g->bus, g->slot, g->func, off, orig);
        if (b->is64 && i < 5) {
            uint8_t hoff = (uint8_t)(off + 4);
            uint32_t ohi = pci_config_read32(g->bus, g->slot, g->func, hoff), mhi;
            pci_config_write32(g->bus, g->slot, g->func, hoff, 0xFFFFFFFFu);
            mhi = pci_config_read32(g->bus, g->slot, g->func, hoff);
            pci_config_write32(g->bus, g->slot, g->func, hoff, ohi);
            b->base = ((uint64_t)ohi << 32) | (orig & ~0xFu);
            {
                uint64_t m = ((uint64_t)mhi << 32) | (mask & ~0xFu);
                b->size = m ? (~m + 1) : 0;
            }
            i++;                                  /* the upper half is not a BAR */
            g->bar[i].base = g->bar[i].size = 0;
            g->bar[i].io = g->bar[i].is64 = g->bar[i].prefetch = 0;
        } else {
            b->base = orig & ~0xFu;
            mask &= ~0xFu;
            b->size = mask ? (uint64_t)(uint32_t)(~mask + 1) : 0;
        }
    }

    pci_config_write16(g->bus, g->slot, g->func, PCI_COMMAND, cmd);
}

static void identify(gpu64_t* g) {
    int old = 0;

    g->kind = GPU64_OTHER;
    g->vendor_name = "unknown";
    g->family = "display controller";
    g->gen = 0;
    g->apu = 0;

    switch (g->vendor) {
    case 0x8086:
        g->vendor_name = "Intel";
        g->kind = GPU64_INTEL;
        if (!(g->family = gpu64_intel_family(g->device, &g->gen))) g->family = "unknown Intel graphics";
        break;
    case 0x1002:
        g->vendor_name = "AMD";
        if (g->device == 0x5046) { g->kind = GPU64_ATI_OLD; g->family = "Rage 128 Pro (PF)"; break; }
        if (g->device == 0x5159) { g->kind = GPU64_ATI_OLD; g->family = "Radeon 7000 (RV100 QY)"; break; }
        g->family = gpu64_amd_family(g->device, &g->apu, &old);
        if (g->family && !old) g->kind = GPU64_AMD;
        else if (g->family) g->kind = GPU64_ATI_OLD;
        else {
            /* amdgpu takes every AMD display controller it has no entry
             * for and asks the GPU itself, through its IP discovery
             * table: everything from RDNA 2 on is found that way. */
            g->kind = GPU64_AMD;
            g->family = "newer than the table: found by IP discovery";
        }
        break;
    case 0x1AF4:
        g->vendor_name = "virtio";
        if (g->device == 0x1050) { g->kind = GPU64_VIRTIO; g->family = "virtio-gpu"; }
        break;
    case 0x1234:
        g->vendor_name = "QEMU";
        if (g->device == 0x1111) { g->kind = GPU64_QEMU_STD; g->family = "standard VGA / bochs-display"; }
        break;
    case 0x10DE:
        g->vendor_name = "NVIDIA";
        g->family = "not supported";
        break;
    default:
        break;
    }
}

int gpu64_scan(void) {
    uint64_t fb = fb64_ready() ? fb64_phys() : 0;

    ngpus = 0;
    pci_init();
    for (uint32_t i = 0; i < pci_device_count() && ngpus < GPU64_MAX; i++) {
        const pci_device_t* d = pci_device_at(i);
        gpu64_t* g;

        if (d->class_code != 0x03) continue;               /* display controllers */
        g = &gpus[ngpus++];
        g->bus = d->bus;
        g->slot = d->slot;
        g->func = d->func;
        g->vendor = d->vendor;
        g->device = d->device;
        g->subclass = d->subclass;
        g->revision = pci_config_read8(d->bus, d->slot, d->func, PCI_REVISION);
        size_bars(g);
        identify(g);
        g->boot = 0;
        for (int b = 0; b < 6; b++)
            if (!g->bar[b].io && g->bar[b].size && fb >= g->bar[b].base &&
                fb < g->bar[b].base + g->bar[b].size)
                g->boot = 1;
    }
    return ngpus;
}

int gpu64_count(void) { return ngpus; }

const gpu64_t* gpu64_at(int i) { return i >= 0 && i < ngpus ? &gpus[i] : 0; }

const gpu64_t* gpu64_find_kind(int kind) {
    for (int i = 0; i < ngpus; i++) if (gpus[i].kind == kind) return &gpus[i];
    return 0;
}

/* --- mapping ------------------------------------------------------------------
 *
 * Above the framebuffer's window in the device-memory slot, a page at a
 * time and uncached: register blocks are small and seldom 2MB aligned. */
#define GPU64_MMIO_BASE (MMIO64_BASE + 0x40000000ULL)
static uint64_t next_virt = GPU64_MMIO_BASE;

volatile uint32_t* gpu64_map(const gpu64_t* g, int bar, uint64_t offset, uint64_t bytes) {
    uint64_t phys, first, last, virt;

    if (!g || bar < 0 || bar > 5 || g->bar[bar].io || !g->bar[bar].size) return 0;
    if (offset + bytes > g->bar[bar].size) return 0;
    phys = g->bar[bar].base + offset;
    first = phys & ~(PAGE64_SIZE - 1);
    last = (phys + bytes + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);
    virt = next_virt;
    for (uint64_t p = first; p < last; p += PAGE64_SIZE)
        if (paging64_map(virt + (p - first), p,
                         PAGE64_PRESENT | PAGE64_WRITE | PAGE64_PCD | PAGE64_PWT) != PAGING64_OK)
            return 0;
    next_virt += last - first + PAGE64_SIZE;           /* a guard page between */
    return (volatile uint32_t*)(virt + (phys - first));
}

/* --- Intel's display engine, read only ------------------------------------------
 *
 * Register offsets from Linux's MIT-licensed i915 headers
 * (skl_universal_plane_regs.h, i9xx_plane_regs.h, intel_display_regs.h):
 *
 *   PIPESRC      0x6001C + 0x1000 * pipe   width-1 in 31:16, height-1 in 15:0
 *   plane 1 ctl  0x70180 + 0x1000 * pipe   enable in bit 31
 *   stride       0x70188 + 0x1000 * pipe   bytes up to gen 8; 64-byte units
 *                                           (linear) from gen 9
 *   surface      0x7019C + 0x1000 * pipe   a graphics address
 *
 * The format field is bits 29:26 up to gen 8 (6 = BGRX 8888) and 27:24
 * on gen 9 and 10 (4 = XRGB 8888); gen 11 widened it to 27:23, the
 * same code shifted. Tiling, gen 9 on, is bits 12:10 (0 = linear).
 * Valleyview and Cherryview put the display block 0x180000 further in. */
void gpu64_intel_decode(int gen, int vlv_chv, gpu64_read_fn read, void* ctx,
                        gpu64_intel_plane_t* out) {
    uint32_t base = vlv_chv ? 0x180000u : 0;

    out->found = 0;
    for (int pipe = 0; pipe < 3; pipe++) {
        uint32_t ctl = read(ctx, base + 0x70180u + 0x1000u * (uint32_t)pipe);
        uint32_t src, stride;
        if (!(ctl & 0x80000000u)) continue;
        src = read(ctx, base + 0x6001Cu + 0x1000u * (uint32_t)pipe);
        stride = read(ctx, base + 0x70188u + 0x1000u * (uint32_t)pipe);
        out->found = 1;
        out->pipe = pipe;
        out->plane_ctl = ctl;
        out->width = ((src >> 16) & 0xFFFFu) + 1;
        out->height = (src & 0xFFFFu) + 1;
        out->surface = read(ctx, base + 0x7019Cu + 0x1000u * (uint32_t)pipe) & ~0xFFFu;
        if (gen >= 9) {
            out->linear = ((ctl >> 10) & 7) == 0;
            out->stride_bytes = (stride & 0xFFFu) * 64;
            out->xrgb8888 = gen >= 11 ? ((ctl >> 23) & 0x1F) == 8
                                      : ((ctl >> 24) & 0xF) == 4;
        } else {
            out->linear = 1;
            out->stride_bytes = stride;
            out->xrgb8888 = ((ctl >> 26) & 0xF) == 6;
        }
        return;
    }
}

/* --- the report ------------------------------------------------------------------- */

static void say_hex(const char* label, uint64_t v) {
    serial64_puts(label);
    serial64_puthex(v);
}

static void say_id(const char* label, uint16_t v) {
    static const char digits[] = "0123456789abcdef";
    serial64_puts(label);
    for (int s = 12; s >= 0; s -= 4) serial64_putc(digits[(v >> s) & 0xF]);
}

static uint32_t mmio_read(void* ctx, uint32_t off) {
    return ((volatile uint32_t*)ctx)[off / 4];
}

void gpu64_report(void) {
    for (int i = 0; i < ngpus; i++) {
        const gpu64_t* g = &gpus[i];
        serial64_puts("NOVARIS64: [gpu] ");
        serial64_putdec(g->bus); serial64_putc(':');
        serial64_putdec(g->slot); serial64_putc('.');
        serial64_putdec(g->func); serial64_putc(' ');
        serial64_puts(g->vendor_name);
        say_id(" ", g->vendor);
        say_id(":", g->device);
        serial64_puts(" ");
        serial64_puts(g->family);
        if (g->kind == GPU64_INTEL) { serial64_puts(", gen "); serial64_putdec((uint64_t)g->gen); }
        if (g->apu) serial64_puts(", APU");
        if (g->boot) serial64_puts(", boot display");
        serial64_putc('\n');
        for (int b = 0; b < 6; b++) {
            if (!g->bar[b].size) continue;
            serial64_puts("NOVARIS64: [gpu]   BAR");
            serial64_putdec((uint64_t)b);
            serial64_puts(g->bar[b].io ? " io " : (g->bar[b].prefetch ? " mem prefetch " : " mem "));
            serial64_puthex(g->bar[b].base);
            serial64_puts(" size ");
            serial64_puthex(g->bar[b].size);
            serial64_putc('\n');
        }
        /* What the firmware lit on an Intel GPU, read and not touched. */
        if (g->kind == GPU64_INTEL && g->gen >= 4) {
            volatile uint32_t* mmio = gpu64_map(g, 0, 0, 0x200000);
            gpu64_intel_plane_t p;
            int vlv = g->family[0] == 'V' || g->family[0] == 'C';
            if (mmio) {
                gpu64_intel_decode(g->gen, vlv, mmio_read, (void*)mmio, &p);
                serial64_puts("NOVARIS64: [gpu]   display: ");
                if (!p.found) serial64_puts("no plane lit\n");
                else {
                    serial64_puts("pipe ");
                    serial64_putc((char)('A' + p.pipe));
                    serial64_puts(" ");
                    serial64_putdec(p.width); serial64_putc('x'); serial64_putdec(p.height);
                    serial64_puts(" stride ");
                    serial64_putdec(p.stride_bytes);
                    serial64_puts(p.linear ? " linear" : " tiled");
                    serial64_puts(p.xrgb8888 ? " XRGB8888" : " other format");
                    say_hex(" at GGTT ", p.surface);
                    serial64_putc('\n');
                }
            }
        }
    }
}
