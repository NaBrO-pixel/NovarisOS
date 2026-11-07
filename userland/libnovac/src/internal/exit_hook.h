#ifndef NOVAC_INTERNAL_EXIT_HOOK_H
#define NOVAC_INTERNAL_EXIT_HOOK_H
/* Lets stdio register "flush every open stream" to run inside exit(),
 * without exit.c depending on stdio. */
void novac__set_exit_flush(void (*fn)(void));
#endif
