/* The program novac_target_test.c execs. Built with libNovaC alone, like
 * the test itself, so an exec that works proves libNovaC's start-up code
 * receives the argv and environment the new program was given. Exits with
 * 40 + argc so the parent can tell it really ran. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *mark = getenv("NOVAC_MARK");
    printf("novac child: argc=%d arg1=%s mark=%s\n", argc,
           argc > 1 ? argv[1] : "(none)", mark ? mark : "(none)");
    return 40 + argc;
}
