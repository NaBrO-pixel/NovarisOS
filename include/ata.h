#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* The ATA PIO driver - Milestone 32's half of "a real disk".
 *
 * PIO rather than DMA, and polling rather than interrupts. Both are
 * deliberate: a bus-mastering DMA driver needs the PCI configuration
 * space, a scatter/gather descriptor table and an IRQ handler that has to
 * cooperate with the scheduler, and none of that changes what the
 * filesystem above it can do. What this kernel needs from a disk is that
 * sector N can be read and written; PIO does that in about two hundred
 * lines, and a modern emulated IDE controller moves a megabyte in a few
 * milliseconds. When that becomes the bottleneck it will be measurable,
 * and that is the moment to write the DMA one.
 *
 * LBA28, so 128GB is the ceiling. The disks this project builds are
 * measured in tens of megabytes. */

/* Probes both legacy IDE buses and registers whatever answers as a block
 * device. Returns how many drives were found. Safe to call on a machine
 * with no disks at all: an absent bus floats its status register to 0xFF
 * or 0x00 and is skipped. */
uint32_t ata_init(void);

/* The model string IDENTIFY reported, for the shell's `df`. Empty for a
 * slot with no drive. */
const char* ata_model(uint32_t index);

#endif
