#include "pit.h"
#include "io.h"
#include "idt.h"
#include "scheduler.h"
#include "serial.h"   /* pit_dump_samples */

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND        0x43

/* The PIT's oscillator runs at ~1.193182 MHz; the divisor we load tells
 * it how many of those oscillations to count before firing IRQ0. */
#define PIT_BASE_FREQUENCY 1193182u

static volatile uint32_t ticks = 0;

/* Where the CPU actually is, sampled a hundred times a second.
 *
 * A falling syscall *rate* has two readings and the rate cannot tell
 * them apart: either each call got slower (the kernel degrading) or the
 * program started doing more of its own work between calls (the kernel
 * is fine). Asserting the first without checking is how kmalloc came to
 * be blamed for something it cannot be doing - it is called fewer than
 * twenty thousand times in a startup that makes eleven million syscalls.
 *
 * The interrupted CS says which it is, exactly and with no bookkeeping:
 * its low two bits are the privilege level of whatever the timer just
 * interrupted. Ring 3 is the program's own work; ring 0 is this kernel's.
 * A hundred votes a second over five minutes is thirty thousand samples,
 * which is far more than enough to say which side of the boundary a
 * missing four minutes is on. */
uint32_t pit_ticks_user, pit_ticks_kernel;

/* And *where*, not just which side: the interrupted eip, with the
 * address space and privilege level packed into one word (a page
 * directory is 4KB-aligned, so its low bits are free). F12 prints them -
 * pit_dump_samples() - and the host resolves kernel addresses with nm.
 *
 * Sampled here rather than from outside because outside is biased. QEMU
 * answers a monitor `info registers` when the CPU leaves translated code,
 * which it does at every int $0x80 and every invlpg, so a profile taken
 * that way put 99.6% of user time on one instruction - the syscall gate -
 * and inflated anything that flushes a TLB entry. The timer interrupt
 * arrives whatever the code is doing. 32768 samples is five and a half
 * minutes at 100Hz; the ring keeps the most recent. */
#define PIT_SAMPLES 32768u
static uint32_t sample_eip[PIT_SAMPLES], sample_space[PIT_SAMPLES];
/* eax too: at a syscall gate it is the call being made. */
static uint32_t sample_eax[PIT_SAMPLES];
static uint32_t sample_count;

static void pit_handler(registers_t* regs) {
    ticks++;
    if ((regs->cs & 3u) == 3u) pit_ticks_user++;
    else pit_ticks_kernel++;
    {
        uint32_t cr3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        uint32_t i = sample_count++ % PIT_SAMPLES;
        sample_eip[i] = regs->eip;
        sample_space[i] = (cr3 & ~0xFFFu) | (regs->cs & 3u);
        sample_eax[i] = regs->eax;
    }
    /* Milestone 9: gives the preemptive scheduler its clock. A no-op
     * whenever scheduler_run_until_idle() isn't actively multitasking,
     * so this doesn't change anything about ticks/uptime/sleep for
     * everything that predates it. */
    scheduler_tick(regs);
}

void pit_install(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;

    /* 0x36 = channel 0, access mode lobyte/hibyte, mode 3 (square wave),
     * binary (not BCD) counting. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    register_interrupt_handler(32, pit_handler); /* IRQ0 -> remapped vector 32 */
}

uint32_t pit_get_ticks(void) {
    return ticks;
}

void pit_sleep_ticks(uint32_t n) {
    uint32_t target = ticks + n;
    while (ticks < target) {
        __asm__ __volatile__("hlt");
    }
}

/* Every sample still in the ring, oldest first, as "P eip space eax" -
 * see pit_handler(). Called on F12, from the keyboard interrupt. */
void pit_dump_samples(void) {
    static const char hex[] = "0123456789abcdef";
    uint32_t n = sample_count < PIT_SAMPLES ? sample_count : PIT_SAMPLES;
    uint32_t first = sample_count - n;
    char line[48];
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (first + k) % PIT_SAMPLES;
        uint32_t v[3] = { sample_eip[i], sample_space[i], sample_eax[i] };
        int o = 0;
        line[o++] = 'P'; line[o++] = ' ';
        for (int w = 0; w < 3; w++) {
            for (int d = 7; d >= 0; d--) line[o++] = hex[(v[w] >> (4 * d)) & 0xF];
            line[o++] = (w == 2) ? '\n' : ' ';
        }
        line[o] = 0;
        serial_writestring(line);
    }
}
