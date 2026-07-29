#include "pit.h"
#include "io.h"
#include "idt.h"
#include "scheduler.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND        0x43

/* The PIT's oscillator runs at ~1.193182 MHz; the divisor we load tells
 * it how many of those oscillations to count before firing IRQ0. */
#define PIT_BASE_FREQUENCY 1193182u

static volatile uint32_t ticks = 0;

static void pit_handler(registers_t* regs) {
    ticks++;
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
