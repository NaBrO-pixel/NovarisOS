/* cpp_init.cpp - a C++ program with global constructors.
 *
 * This exists to exercise one specific thing: _initterm. A C++ binary's
 * global constructors are function pointers the compiler puts in the
 * .CRT section, and the C runtime calls them by walking that array before
 * main() runs. That walk cannot happen inside the kernel - the pointers
 * are ring-3 code, and the kernel is mid-interrupt when the call arrives
 * - so kernel/win32_msvcrt.c implements _initterm as a hand-assembled
 * ring-3 code blob instead of an emulated call. If the line printed by
 * the constructor below appears *before* the one printed by main(), that
 * mechanism works. */

#include <stdio.h>

struct Banner {
    Banner() {
        printf("[ctor]  a C++ global constructor ran before main()\n");
        value = 1234;
    }
    ~Banner() {
        /* Global destructors run through atexit, which the C++ runtime
         * registers them with. Through Milestone 12 Novaris accepted the
         * registration and never called anything back, so this line did
         * not appear; Milestone 13's ring-3 callback mechanism made
         * atexit real, and it does now. Nothing in this file changed to
         * make that happen - which is the point of checking it here. */
        printf("[dtor]  global destructor ran at exit\n");
    }
    int value;
};

static Banner banner;

int main() {
    printf("[main]  main() running; the constructor set value = %d\n",
           banner.value);
    return 0;
}
