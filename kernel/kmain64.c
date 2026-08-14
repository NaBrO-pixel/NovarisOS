/* kmain64.c - the 64-bit kernel's entry point during bring-up.
 *
 * This is deliberately not kernel.c. Porting 40,000 lines of kernel to a
 * new architecture in one step produces a mountain of compiler errors and
 * no way to tell which of them matter; the way this is done instead is to
 * get the machine into long mode first, prove it, and then move subsystems
 * across one at a time against a foundation that is known to work.
 *
 * So the only job here is evidence. "It booted and printed something" is
 * not evidence of 64-bit: a 32-bit kernel prints too, and a bootstrap that
 * silently failed to switch would still reach C code in compatibility
 * mode. What is checked below is the architectural state itself - EFER.LMA
 * is the bit the CPU sets when long mode actually becomes active, and it
 * cannot be set by anything this code did on its own.
 *
 * Self-contained on purpose: it talks to COM1 directly rather than calling
 * serial.c, so that this file compiling proves the toolchain and the
 * bootstrap and nothing else. */

#include <stdint.h>
#include "io.h"

#define COM1 0x3F8

static void s_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) { }
    if (c == '\n') {
        outb(COM1, '\r');
        while (!(inb(COM1 + 5) & 0x20)) { }
    }
    outb(COM1, (uint8_t)c);
}

static void s_puts(const char* s) {
    while (*s) s_putc(*s++);
}

static void s_puthex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    int i;
    buf[16] = '\0';
    for (i = 15; i >= 0; i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    s_puts("0x");
    s_puts(buf);
}

static void s_putdec(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0) { s_putc('0'); return; }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    s_puts(&buf[i]);
}

static void serial_setup(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0F);
}

static uint64_t read_cr0(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v;
}
static uint64_t read_cr3(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v;
}
static uint64_t read_cr4(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v;
}
static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static uint16_t read_cs(void) {
    uint16_t v; __asm__ __volatile__("mov %%cs, %0" : "=r"(v)); return v;
}

static void check(const char* what, int ok) {
    s_puts(ok ? "NOVARIS64: ok    " : "NOVARIS64: FAIL  ");
    s_puts(what);
    s_putc('\n');
}

/* Declared by linker64.ld, so the addresses printed are the linker's own
 * view rather than anything this file could have got wrong. */
extern char _kernel_virtual_start[];
extern char _kernel_virtual_end[];

void kernel_main(uint32_t magic, void* mbi) {
    uint64_t cr0   = read_cr0();
    uint64_t cr4   = read_cr4();
    uint64_t efer  = read_msr(0xC0000080);
    uint64_t rip;

    __asm__ __volatile__("lea (%%rip), %0" : "=r"(rip));

    serial_setup();

    s_puts("\n");
    s_puts("NOVARIS64: ---- long mode bring-up ----\n");

    /* EFER.LMA (bit 10) is set by the CPU itself, at the moment paging is
     * enabled with LME already on. It is the one bit here that constitutes
     * proof rather than assertion. */
    check("EFER.LMA  - long mode is active", (efer & (1ULL << 10)) != 0);
    check("EFER.LME  - long mode is enabled", (efer & (1ULL << 8)) != 0);
    check("CR0.PG    - paging is on",         (cr0 & (1ULL << 31)) != 0);
    check("CR4.PAE   - PAE is on",            (cr4 & (1ULL << 5)) != 0);
    check("CR4.OSFXSR- SSE is usable",        (cr4 & (1ULL << 9)) != 0);

    /* A 32-bit build cannot satisfy this one: it has no 64-bit pointer. */
    check("pointers are 8 bytes",  sizeof(void*) == 8);
    check("long is 8 bytes (LP64)", sizeof(long) == 8);

    /* And the code really is running from the higher half, not from the
     * identity window it started in. */
    check("RIP is in the higher half", rip >= 0xFFFFFFFF80000000ULL);

    s_puts("NOVARIS64: cs      = "); s_puthex(read_cs()); s_putc('\n');
    s_puts("NOVARIS64: rip     = "); s_puthex(rip); s_putc('\n');
    s_puts("NOVARIS64: cr3     = "); s_puthex(read_cr3()); s_putc('\n');
    s_puts("NOVARIS64: efer    = "); s_puthex(efer); s_putc('\n');
    s_puts("NOVARIS64: kernel  = "); s_puthex((uint64_t)_kernel_virtual_start);
    s_puts(" .. ");                  s_puthex((uint64_t)_kernel_virtual_end);
    s_putc('\n');
    s_puts("NOVARIS64: size    = ");
    s_putdec((uint64_t)(_kernel_virtual_end - _kernel_virtual_start));
    s_puts(" bytes\n");

    /* The multiboot handoff has to survive the mode switch too - if the
     * bootstrap had clobbered eax or ebx this is where it would show. */
    s_puts("NOVARIS64: magic   = "); s_puthex(magic); s_putc('\n');
    s_puts("NOVARIS64: mbi     = "); s_puthex((uint64_t)mbi); s_putc('\n');
    check("multiboot magic is 0x2BADB002", magic == 0x2BADB002u);
    check("multiboot info pointer is non-NULL", mbi != 0);

    s_puts("NOVARIS64: ---- bring-up complete ----\n");

    for (;;) __asm__ __volatile__("hlt");
}
