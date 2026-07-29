/* crash.c - a Windows program that deliberately misbehaves.
 *
 * Running arbitrary .exe files means running buggy ones. Before
 * Milestone 10 a fault in ring 3 reached the kernel's panic screen and
 * halted the machine, which is the wrong response to "the program you
 * typed `run` on has a null pointer in it".
 *
 * This program writes through a null pointer on purpose. The expected
 * result is a Windows-shaped unhandled-exception report naming the fault
 * and the address, and a prompt afterwards - not a panic, and not a
 * reboot. Everything printed before the fault should still have appeared,
 * proving the program really did execute up to that point. */

#include <stdio.h>

int main(void) {
    printf("About to dereference a null pointer on purpose.\n");
    printf("Novaris should report an access violation and return to the shell.\n");
    fflush(stdout);

    volatile int* nowhere = (volatile int*)0;
    *nowhere = 1;

    printf("This line must never be reached.\n");
    return 0;
}
