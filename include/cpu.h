#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* CPUID, for the About window and Activity Monitor. "About This Mac"
 * names the processor it's running on; so does this, and on a hobby OS
 * that string is genuinely useful - it's how you tell a QEMU TCG guest
 * from KVM from real silicon at a glance. */

/* e.g. "GenuineIntel", "AuthenticAMD". Always 12 characters. */
const char* cpu_vendor(void);

/* The 48-character brand string from leaves 0x80000002-4, trimmed. Falls
 * back to a family/model description if the CPU doesn't support it. */
const char* cpu_brand(void);

/* Comma-separated feature names from the CPUID feature bits, e.g.
 * "FPU, TSC, PAE, APIC, SSE, SSE2". Written into `out`. */
void cpu_feature_list(char* out, uint32_t size);

#endif
