/* ata.c - a polling ATA PIO driver for the two legacy IDE buses.
 *
 * Milestone 32. The first real storage device this kernel has ever
 * talked to, and the thing every remaining item on the Wine list runs
 * through: `wine.inf` builds a prefix out of hundreds of files, and a
 * filesystem that lives in a 40MB initrd read into RAM whole is not
 * somewhere to build one.
 *
 * The protocol is old enough to be simple. Select a drive, write the
 * sector number into four registers, write a command, then move 256
 * 16-bit words per sector through the data port while the controller
 * says it is ready. There is no DMA, no PCI enumeration and no interrupt
 * handler - see include/ata.h for why that is a choice rather than an
 * omission.
 *
 * The two things that actually bite when writing one of these:
 *
 *   - The 400ns settle. After selecting a drive, the status register does
 *     not become meaningful immediately, and reading it too early gets
 *     the *previous* drive's status. Every real driver reads the
 *     alternate status port four times to burn the time; so does this
 *     one, and the four reads are not superstition.
 *
 *   - The flush. A write command that returns is not a write that
 *     reached the platter (or, here, the host's file): the drive's cache
 *     holds it. FLUSH CACHE is what makes a write durable, and without it
 *     a disk image looks right in RAM and wrong after a reboot - which is
 *     precisely the property this milestone exists to provide.
 */

#include "ata.h"
#include "blockdev.h"
#include "io.h"
#include "kstring.h"
#include "console.h"
#include "pit.h"
#include "posix.h"

/* Register offsets from the bus's base I/O port. */
#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_DRIVE       6
#define ATA_REG_COMMAND     7
#define ATA_REG_STATUS      7

/* Status bits. */
#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_RDY   0x40
#define ATA_SR_BSY   0x80

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

/* How long to spin waiting for the controller. Polling has to have a
 * bound or a dead drive hangs the machine, and this is a real risk here:
 * the shell runs in the same thread as everything else. The count is
 * large enough that a slow emulated seek never trips it and small enough
 * that a drive which will never answer costs a fraction of a second. */
/* How long to wait for the drive, and it is deliberately two numbers.
 *
 * ATA_TIMEOUT used to be a spin count of four million, and a spin count is
 * a duration only if you know how long one `inb` takes - which on real
 * hardware is a bus cycle and on an emulated controller is a trap into the
 * emulator, two figures that differ by orders of magnitude. Four million
 * of them turned out to be a few seconds here and *still* not enough for
 * one specific operation: the CACHE FLUSH at the end of the very first
 * write of a boot, where the host has to allocate and fsync a sparse image
 * it has never written to. Every later flush was fast, so exactly one
 * write per boot failed - see the note in ata_write_run(), and Milestone
 * 35 for what that cost.
 *
 * So the wait is in *ticks* now, which is a real duration, with the spin
 * count kept as a backstop for the case the PIT cannot answer: a wait
 * that happens with interrupts disabled would otherwise never end. Five
 * seconds is generous for a flush and still far short of hanging. */
#define ATA_TIMEOUT_TICKS 500u        /* five seconds at the PIT's 100Hz */
#define ATA_SPIN_MAX      40000000u   /* the backstop, interrupts-off only */

/* True when `start` is more than ATA_TIMEOUT_TICKS ago. The PIT only moves
 * if interrupts are on; when they are not this never fires and the spin
 * cap is what ends the wait. */
static int ata_expired(uint32_t start, uint32_t spins) {
    if (spins > ATA_SPIN_MAX) return 1;
    if ((spins & 0xFFFFu) != 0) return 0;      /* only check now and then */
    return (pit_get_ticks() - start) > ATA_TIMEOUT_TICKS;
}

typedef struct {
    uint16_t io_base;    /* 0x1F0 (primary) or 0x170 (secondary) */
    uint16_t ctrl_base;  /* 0x3F6 / 0x376 */
    uint8_t  slave;      /* 0 = master, 1 = slave */
    char     model[41];
    blockdev_t dev;
} ata_drive_t;

static ata_drive_t drives[BLOCK_MAX_DEVICES];
static uint32_t ndrives = 0;

/* Reading the *alternate* status port has no side effects, unlike the
 * ordinary one, which clears a pending interrupt. Four reads is the
 * canonical way to spend 400ns without a calibrated delay loop. */
static void ata_settle(ata_drive_t* d) {
    for (int i = 0; i < 4; i++) (void)inb(d->ctrl_base);
}

static int ata_wait_not_busy(ata_drive_t* d) {
    uint32_t start = pit_get_ticks();
    for (uint32_t i = 1; !ata_expired(start, i); i++) {
        uint8_t st = inb(d->io_base + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -EIO;
}

/* Waits for the controller to be ready to move a sector. ERR and DF are
 * checked in the same loop, because a drive that has failed the command
 * will never raise DRQ and waiting for it is waiting for the timeout. */
static int ata_wait_drq(ata_drive_t* d) {
    uint32_t start = pit_get_ticks();
    for (uint32_t i = 1; !ata_expired(start, i); i++) {
        uint8_t st = inb(d->io_base + ATA_REG_STATUS);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -EIO;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
    }
    return -EIO;
}

/* Selects the drive and programs an LBA28 transfer. The top four bits of
 * the address ride in the drive-select register, which is the whole of
 * why LBA28 stops at 128GB. */
static int ata_prepare(ata_drive_t* d, uint32_t lba, uint8_t count) {
    if (ata_wait_not_busy(d) != 0) return -EIO;

    outb(d->io_base + ATA_REG_DRIVE,
         (uint8_t)(0xE0u | ((uint32_t)d->slave << 4) | ((lba >> 24) & 0x0Fu)));
    ata_settle(d);

    outb(d->io_base + ATA_REG_FEATURES, 0);
    outb(d->io_base + ATA_REG_SECCOUNT, count);
    outb(d->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(d->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(d->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
    return 0;
}

/* A count of 0 means 256 sectors to the hardware, which is why transfers
 * here are chopped into runs of at most 255: one fewer special case, and
 * nothing above this asks for a bigger run anyway. */
#define ATA_MAX_RUN 255u

static void ata_report(ata_drive_t* d, const char* what) {
    char b[16];
    terminal_writestring_color("[ata] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring(what);
    terminal_writestring(": status 0x");
    ku32_to_hex(inb(d->io_base + ATA_REG_STATUS), b, 0, 2);
    terminal_writestring(b);
    terminal_writestring(" error 0x");
    ku32_to_hex(inb(d->io_base + ATA_REG_ERROR), b, 0, 2);
    terminal_writestring(b);
    terminal_writestring("\n");
}

static int ata_read_run(ata_drive_t* d, uint32_t lba, uint8_t count,
                        uint8_t* buf) {
    if (ata_prepare(d, lba, count) != 0) return -EIO;
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    /* 400ns before the first look at the status register. See the note on
     * the same line in ata_write_run() - this one has never been observed
     * to matter and is here because it is the same rule. */
    ata_settle(d);

    uint32_t sectors = count ? count : 256u;
    for (uint32_t s = 0; s < sectors; s++) {
        /* DRQ is raised once per sector, not once per command. */
        if (ata_wait_drq(d) != 0) return -EIO;
        uint16_t* out = (uint16_t*)(buf + s * BLOCK_SECTOR_SIZE);
        for (uint32_t w = 0; w < BLOCK_SECTOR_SIZE / 2; w++) {
            out[w] = inw(d->io_base + ATA_REG_DATA);
        }
        ata_settle(d);
    }
    return 0;
}

static int ata_write_run(ata_drive_t* d, uint32_t lba, uint8_t count,
                         const uint8_t* buf) {
    if (ata_prepare(d, lba, count) != 0) return -EIO;
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    /* 400ns between writing the command register and reading the status
     * one, and this is not pedantry - it is Milestone 35's bug.
     *
     * The status register does not become the new command's until the
     * drive has begun it. Read it immediately and what comes back is the
     * *previous* command's result, and ata_wait_drq() fails on ERR the
     * moment it sees it. So the first write after a boot spent entirely
     * reading picked up a bit left over from the identify at probe time
     * and reported a write error - and the next write, whose predecessor
     * was a clean write, worked. Every write on the machine failed once
     * and then never again.
     *
     * What that looked like from three layers up: `mkdir /disk/.wine`
     * refused, and Wine saying "chdir to /disk/.wine : No such file or
     * directory" - the whole prefix, lost to a missing 400 nanoseconds.
     * It stayed hidden this long because the first write of a boot is
     * rarely the one anybody is watching. */
    ata_settle(d);

    uint32_t sectors = count ? count : 256u;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_wait_drq(d) != 0) {
            ata_report(d, "no data request while writing");
            return -EIO;
        }
        const uint16_t* in = (const uint16_t*)(buf + s * BLOCK_SECTOR_SIZE);
        for (uint32_t w = 0; w < BLOCK_SECTOR_SIZE / 2; w++) {
            outw(d->io_base + ATA_REG_DATA, in[w]);
            /* The specification requires a short gap between the words of
             * a PIO write and not between those of a read. Omitting it
             * works on emulated controllers and corrupts data on some
             * real ones; the cost is one I/O port read per word. */
            ata_settle(d);
        }
    }

    /* Durability. See the file header - this is not optional. */
    if (ata_wait_not_busy(d) != 0) { ata_report(d, "busy before flush"); return -EIO; }
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy(d) != 0) { ata_report(d, "busy after flush"); return -EIO; }
    if (inb(d->io_base + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) {
        ata_report(d, "flush reported an error");
        return -EIO;
    }
    return 0;
}

static int ata_dev_read(blockdev_t* dev, uint32_t lba, uint32_t count,
                        void* buf) {
    ata_drive_t* d = (ata_drive_t*)dev->impl;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t run = count > ATA_MAX_RUN ? ATA_MAX_RUN : count;
        if (ata_read_run(d, lba, (uint8_t)run, p) != 0) return -EIO;
        lba += run;
        count -= run;
        p += run * BLOCK_SECTOR_SIZE;
    }
    return 0;
}

static int ata_dev_write(blockdev_t* dev, uint32_t lba, uint32_t count,
                         const void* buf) {
    ata_drive_t* d = (ata_drive_t*)dev->impl;
    const uint8_t* p = (const uint8_t*)buf;
    while (count) {
        uint32_t run = count > ATA_MAX_RUN ? ATA_MAX_RUN : count;
        if (ata_write_run(d, lba, (uint8_t)run, p) != 0) return -EIO;
        lba += run;
        count -= run;
        p += run * BLOCK_SECTOR_SIZE;
    }
    return 0;
}

/* IDENTIFY. Also the presence test: a bus with nothing on it floats its
 * status register, and a status of 0x00 or 0xFF means "no drive" rather
 * than "a drive reporting something". */
static int ata_identify(ata_drive_t* d) {
    outb(d->io_base + ATA_REG_DRIVE,
         (uint8_t)(0xA0u | ((uint32_t)d->slave << 4)));
    ata_settle(d);

    outb(d->io_base + ATA_REG_SECCOUNT, 0);
    outb(d->io_base + ATA_REG_LBA0, 0);
    outb(d->io_base + ATA_REG_LBA1, 0);
    outb(d->io_base + ATA_REG_LBA2, 0);
    outb(d->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t st = inb(d->io_base + ATA_REG_STATUS);
    if (st == 0 || st == 0xFF) return 0;

    if (ata_wait_not_busy(d) != 0) return 0;

    /* A non-zero signature in the two cylinder registers means the device
     * speaks ATAPI (a CD-ROM, which is exactly what this kernel boots
     * from) and will not answer IDENTIFY. Skipping it here is why
     * attaching the ISO and the disk to the same machine works. */
    if (inb(d->io_base + ATA_REG_LBA1) || inb(d->io_base + ATA_REG_LBA2)) {
        return 0;
    }
    if (ata_wait_drq(d) != 0) return 0;

    uint16_t id[256];
    for (uint32_t w = 0; w < 256; w++) id[w] = inw(d->io_base + ATA_REG_DATA);

    /* Words 27..46 are the model, in the byte order of a machine that is
     * not this one - ATA strings are big-endian pairs. */
    for (uint32_t i = 0; i < 20; i++) {
        d->model[i * 2]     = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = '\0';

    /* Words 60-61: the LBA28 sector count. A drive that reports zero here
     * is either not an LBA drive or is lying, and either way there is
     * nothing this driver can address. */
    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (!sectors) return 0;

    d->dev.sectors = sectors;
    return 1;
}

static void name_drive(char* out, uint32_t index) {
    out[0] = 'a'; out[1] = 't'; out[2] = 'a';
    out[3] = (char)('0' + index);
    out[4] = '\0';
}

uint32_t ata_init(void) {
    static const uint16_t io_bases[2]   = { 0x1F0, 0x170 };
    static const uint16_t ctrl_bases[2] = { 0x3F6, 0x376 };

    ndrives = 0;
    for (uint32_t bus = 0; bus < 2; bus++) {
        for (uint32_t slave = 0; slave < 2; slave++) {
            if (ndrives >= BLOCK_MAX_DEVICES) break;
            ata_drive_t* d = &drives[ndrives];
            kmemset(d, 0, sizeof(*d));
            d->io_base = io_bases[bus];
            d->ctrl_base = ctrl_bases[bus];
            d->slave = (uint8_t)slave;

            if (!ata_identify(d)) continue;

            d->dev.read = ata_dev_read;
            d->dev.write = ata_dev_write;
            d->dev.impl = d;
            name_drive(d->dev.name, ndrives);
            blockdev_register(&d->dev);
            ndrives++;
        }
    }
    return ndrives;
}

const char* ata_model(uint32_t index) {
    if (index >= ndrives) return "";
    return drives[index].model;
}
