#ifndef FPU_H
#define FPU_H

#include <stdint.h>

/* fpu.h - x87/SSE state, Milestone 21.
 *
 * Novaris ignored the FPU for twenty milestones and got away with it: the
 * kernel never used it, and one program at a time meant nobody's state
 * could be clobbered by anybody else's. Two things ended that at once.
 *
 * First, SSE has to be *enabled* at all. A real glibc-linked binary's
 * string functions are SSE2 (`__strrchr_sse2_bsf` and friends), and with
 * CR4.OSFXSR clear every one of them raises an invalid-opcode fault.
 * That is exactly how far a real glibc program got before this existed.
 *
 * Second, once it is enabled, the XMM registers become live state that
 * preemption can corrupt - and Novaris has had preemptive threads since
 * Milestone 16. Two threads inside glibc's memcpy would quietly destroy
 * each other's registers. So enabling SSE and saving its state are one
 * job, not two. */

/* Turns on x87 and SSE: CR0.MP set, CR0.EM clear (no emulation), and
 * CR4.OSFXSR | CR4.OSXMMEXCPT so FXSAVE/FXRSTOR and the SSE instructions
 * are legal. Call once at boot, before anything might execute SSE. */
void fpu_init(void);

/* One task's x87+SSE state. FXSAVE writes 512 bytes and requires 16-byte
 * alignment - both are architectural, not a choice. */
#define FPU_STATE_SIZE 512
typedef struct {
    uint8_t data[FPU_STATE_SIZE];
} __attribute__((aligned(16))) fpu_state_t;

/* Initialises a state block to what a freshly-started thread should see:
 * a clean x87 control word and zeroed XMM registers. A thread must not
 * inherit whatever the last one left in there. */
void fpu_state_init(fpu_state_t* state);

void fpu_save(fpu_state_t* state);
void fpu_restore(const fpu_state_t* state);

#endif
