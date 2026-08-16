/* dynhello64.c - the smallest program that makes ld.so run.
 *
 * Dynamically linked, which is the entire point. Wine's loader is a
 * dynamically linked PIE that needs /lib64/ld-linux-x86-64.so.2 and
 * libc.so.6, so nothing about Wine can be attempted until a kernel can
 * get the dynamic loader itself off the ground.
 *
 * This is that experiment reduced to its smallest form: if ld.so can
 * map libc, relocate it, and reach main, the same machinery is what
 * Wine's loader will use. If it cannot, whatever it asks for on the way
 * is the next thing to implement - which is why the kernel's [enosys]
 * tracer exists.
 */

#include <stdio.h>

int main(void)
{
    printf("a dynamically linked program reached main\n");
    return 67;
}
