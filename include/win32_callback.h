#ifndef WIN32_CALLBACK_H
#define WIN32_CALLBACK_H

#include <stdint.h>

/* Synchronous kernel -> ring-3 callbacks for the Win32 layer.
 *
 * Milestone 10 hit a wall that win32.c documents at some length: several
 * ordinary C-runtime functions take a *function pointer belonging to the
 * program* and are supposed to call it. qsort and bsearch call a
 * comparator; atexit calls its handlers at exit. The kernel implements
 * those APIs from inside an `int 0x81` handler, in ring 0, and had no way
 * to run ring-3 code from there and come back - so qsort, bsearch and
 * atexit were deliberately left unimplemented rather than stubbed into
 * silently wrong behavior.
 *
 * This is the missing piece. The kernel builds a call frame on the
 * program's own stack, iret's into ring 3 at the program's function with
 * a return address pointing at a two-byte ring-3 stub:
 *
 *     CD 82        int 0x82
 *
 * When the callback returns, it lands on that stub, traps back in, and
 * the handler unwinds the kernel back to the point where it entered ring
 * 3 - with the callback's eax as the return value. From the API
 * implementation's point of view win32_callback_call() is an ordinary
 * blocking function call, the same illusion process_run_user_mode()
 * creates for a whole program.
 *
 * Why this needs its own vector rather than reusing 0x81: 0x81 means
 * "the program is calling the kernel" and reads its arguments off the
 * ring-3 stack. 0x82 means the exact opposite - "the kernel's call into
 * the program has finished" - and its handler must not return through
 * iret at all. Sharing a dispatcher between the two would mean a flag
 * test on the hottest path in the emulation layer to get a worse result.
 *
 * The hard part is not the trap, it is the ring-0 stack. Every ring3
 * -> ring0 transition loads esp from the *same* fixed TSS.esp0, so while
 * a callback is running in ring 3, any trap it takes (its own int 0x82,
 * or an `int 0x81` from a comparator that calls printf, or a timer IRQ)
 * would push its frame straight on top of the still-live kernel frames of
 * the qsort call that started it. win32_callback_call() therefore lowers
 * TSS.esp0 below its own frame before entering ring 3 and restores it on
 * the way back, which is what makes nesting work at all. See ROADMAP.md
 * Milestone 13. */

/* Ring-3 -> ring-0 vector meaning "the callback you started has
 * returned". DPL=3 like 0x80 and 0x81, since ring 3 executes it. */
#define WIN32_CB_INT_VECTOR 0x82

/* A comparator takes 2 arguments; nothing the CRT calls back needs more
 * than this, and the limit keeps the frame builder bounded. */
#define WIN32_CB_MAX_ARGS  8

/* Callbacks nest: a qsort comparator is free to call qsort. Each level
 * costs one kernel stack region, so the depth is capped rather than
 * left to exhaust the stack and triple-fault. */
#define WIN32_CB_MAX_DEPTH 8

/* Emits the ring-3 return stub into the thunk arena. Called once from
 * win32_init(), after the arena exists and the export thunks are laid
 * out. Returns 0 if the arena is full. */
int win32_callback_init(void);

/* Calls `fn` in ring 3 with `argc` dword arguments, cdecl (the kernel
 * discards the frame afterwards, so nothing has to pop it), and blocks
 * until it returns.
 *
 * `user_stack` is where to build the frame: pass the ring-3 esp captured
 * at the top of the dispatch that led here (win32_call_t.useresp), *not*
 * a re-read of the trap frame - see the comment on that field. The frame
 * is built below it, so the caller's own arguments stay intact.
 *
 * Returns 1 with the callback's return value in *ret, or 0 without
 * calling anything if the callback machinery is unavailable (no stub, too
 * many arguments, nesting limit reached). Callers must handle 0 rather
 * than assume it can't happen. */
int win32_callback_call(uint32_t fn, const uint32_t* args, uint32_t argc,
                        uint32_t user_stack, uint32_t* ret);

/* How many callbacks are currently in flight. 0 outside one. */
uint32_t win32_callback_depth(void);

/* Drops any in-flight callback state and puts TSS.esp0 back where it
 * was. Called when a program ends - normally or by faulting - because
 * both paths unwind past win32_callback_call() without it returning. */
void win32_callback_reset(void);

#endif
