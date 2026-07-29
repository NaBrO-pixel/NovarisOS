/* hello_win.c - an ordinary Windows console program.
 *
 * Nothing here is written for Novaris. It is compiled with stock
 * i686-w64-mingw32-gcc against the real mingw-w64 headers and CRT, which
 * means the resulting .exe imports msvcrt.dll and kernel32.dll exactly
 * the way any Windows program does, runs mingw's real startup code before
 * main(), and would run unchanged on Windows. That is the entire point:
 * if this executes on Novaris, the emulation layer is doing real work
 * rather than being handed a binary built to accommodate it.
 *
 * It exercises the parts of the C runtime a normal program leans on:
 * formatted output of every common conversion, the heap, string handling,
 * and returning an exit code through the CRT rather than calling
 * ExitProcess itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

int main(int argc, char** argv) {
    printf("Hello from a real Windows .exe running on Novaris!\n");
    printf("  compiled by mingw-w64, linked against msvcrt.dll\n\n");

    printf("integers:   %d %i %u %05d %-6d| %+d\n", 42, -7, 4000000000u, 42, 42, 42);
    printf("hex/octal:  %x %X %#x %o\n", 48879, 48879, 48879, 8);
    printf("strings:    [%s] [%10s] [%-10s] [%.3s]\n", "abc", "abc", "abc", "abcdef");
    printf("chars:      %c%c%c   percent: %%\n", 'a', 'b', 'c');
    printf("floats:     %f %.2f %e %g\n", 3.14159265358979, 2.5, 12345.6789, 0.0001);
    printf("64-bit:     %lld\n", 1234567890123LL);
    printf("pointer:    %p\n", (void*)main);

    /* The heap: malloc/free go through HeapAlloc's arena in the kernel. */
    char* buffer = (char*)malloc(64);
    if (!buffer) {
        printf("malloc failed\n");
        return 1;
    }
    strcpy(buffer, "malloc + strcpy + strlen");
    printf("\nheap:       \"%s\" (%d bytes)\n", buffer, (int)strlen(buffer));
    free(buffer);

    /* Some actual computation, so it's clear the CPU is really executing
     * this program's own code and not just its printf calls. */
    printf("fib(20):    %d\n", fib(20));

    printf("argc:       %d, argv[0]: %s\n", argc, argv[0] ? argv[0] : "(none)");
    printf("\nExiting with code 0.\n");
    return 0;
}
