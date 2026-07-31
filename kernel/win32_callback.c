/* win32_callback.c - the C side of kernel -> ring-3 callbacks.
 * See include/win32_callback.h for the design and ROADMAP.md Milestone 13
 * for how this came about. */

#include "win32_callback.h"
#include "win32.h"
#include "win32_internal.h"
#include "gdt.h"
#include "idt.h"

/* The kernel context win32_callback_enter() has to come back to. Laid
 * out to match the [ecx+0] / [ecx+4] offsets in win32_callback_asm.s. */
typedef struct {
    uint32_t esp;
    uint32_t ebp;
} win32_cb_frame_t;

extern uint32_t win32_callback_enter(uint32_t entry, uint32_t user_esp,
                                     win32_cb_frame_t* frame);
extern void win32_callback_leave(win32_cb_frame_t* frame, uint32_t retval)
    __attribute__((noreturn));

static win32_cb_frame_t frames[WIN32_CB_MAX_DEPTH];
/* TSS.esp0 as it was before each level lowered it - see lower_esp0(). */
static uint32_t saved_esp0[WIN32_CB_MAX_DEPTH];
static uint32_t depth = 0;

/* Address of the two-byte `int 0x82` stub in the thunk arena, which every
 * callback gets as its return address. */
static uint32_t return_stub = 0;

/* Bytes below the current kernel esp that the ring-3 callback's own traps
 * are allowed to start using. Only has to clear win32_callback_enter's
 * frame (a return address plus four saved registers, 20 bytes); 64 is
 * round, cheap, and leaves room if that prologue ever grows. */
#define CB_KSTACK_GUARD 64u

/* Bytes of the program's stack left untouched between its current esp and
 * the callback frame we build. Nothing live sits there - at the point of
 * an `int 0x81` the ring-3 esp points at the thunk's return address, and
 * the argument list is *above* it - but a gap keeps a callback frame from
 * abutting the caller's frame, which makes stack dumps far easier to read
 * when something does go wrong. */
#define CB_USTACK_GAP 64u

uint32_t win32_callback_depth(void) {
    return depth;
}

/* --- the trap ---------------------------------------------------------- */

/* int 0x82: the ring-3 callback has returned, and eax is its return
 * value. Unwinds the kernel to the win32_callback_call() that started it;
 * does not return through iret. */
static void callback_trap(registers_t* regs) {
    if ((regs->cs & 3) != 3) {
        /* The stub only ever executes in ring 3. A ring-0 int 0x82 would
         * mean the kernel jumped somewhere it shouldn't have; unwinding
         * on the strength of that would make things worse, not better. */
        w32_report("int 0x82 from ring 0 - ignored");
        regs->eax = 0;
        return;
    }
    if (depth == 0) {
        /* A program can execute `int 0x82` itself. There is nothing to
         * return to, so treat it as a no-op rather than unwinding into a
         * frame that was never saved. */
        w32_report("int 0x82 with no callback in flight - ignored");
        regs->eax = 0;
        return;
    }

    uint32_t d = --depth;
    /* Put esp0 back before switching stacks: from here on, traps belong
     * to the level we are returning *to*. */
    gdt_set_kernel_stack(saved_esp0[d]);
    win32_callback_leave(&frames[d], regs->eax);
}

/* --- setup ------------------------------------------------------------- */

int win32_callback_init(void) {
    static const uint8_t stub[] = { 0xCD, WIN32_CB_INT_VECTOR };

    return_stub = w32_emit_ring3_code(stub, sizeof(stub));
    if (!return_stub) return 0;

    register_interrupt_handler(WIN32_CB_INT_VECTOR, callback_trap);
    return 1;
}

void win32_callback_reset(void) {
    /* A program that faults, or calls ExitProcess, unwinds straight past
     * every win32_callback_call() frame on the stack without any of them
     * returning. Level 0's saved value is the esp0 the kernel runs on
     * normally, so restoring that one puts everything back. */
    if (depth > 0) gdt_set_kernel_stack(saved_esp0[0]);
    depth = 0;
}

/* --- the call ---------------------------------------------------------- */

int win32_callback_call(uint32_t fn, const uint32_t* args, uint32_t argc,
                        uint32_t user_stack, uint32_t* ret) {
    if (!fn || !return_stub || !user_stack) return 0;
    if (argc > WIN32_CB_MAX_ARGS) return 0;
    if (depth >= WIN32_CB_MAX_DEPTH) {
        w32_report("callback nesting limit reached - callback not made");
        return 0;
    }

    /* Build the ring-3 frame below the program's current esp: arguments
     * pushed right-to-left (cdecl), then the return address. 16-byte
     * alignment because mingw-compiled code is free to use SSE on
     * anything it finds on the stack. */
    uint32_t sp = (user_stack - CB_USTACK_GAP) & ~15u;
    for (uint32_t i = argc; i > 0; i--) {
        sp -= 4;
        *(uint32_t*)sp = args[i - 1];
    }
    sp -= 4;
    *(uint32_t*)sp = return_stub;

    uint32_t d = depth++;

    /* The load-bearing line. While the callback runs in ring 3, every
     * trap it takes - its own int 0x82, an int 0x81 from a comparator
     * that calls printf, the timer IRQ - re-enters ring 0 with esp
     * loaded from TSS.esp0. Leaving that at its normal value would push
     * those frames straight over the still-live kernel frames of the API
     * call we are standing in. Lowering it below our own frame gives the
     * nested trap fresh stack, and the saved value goes back on return. */
    uint32_t kesp;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(kesp));
    saved_esp0[d] = gdt_get_kernel_stack();
    gdt_set_kernel_stack((kesp - CB_KSTACK_GUARD) & ~15u);

    uint32_t r = win32_callback_enter(fn, sp, &frames[d]);

    /* callback_trap() has already restored esp0 and decremented depth by
     * the time control arrives back here. */
    if (ret) *ret = r;
    return 1;
}
