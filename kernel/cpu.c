/* cpu.c - CPUID lookups for the About and Activity Monitor windows. */

#include "cpu.h"
#include "kstring.h"

static int probed;
static char vendor[13];
static char brand[64];

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c,
                         uint32_t* d) {
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(0));
}

static void store4(char* out, uint32_t value) {
    out[0] = (char)(value & 0xFF);
    out[1] = (char)((value >> 8) & 0xFF);
    out[2] = (char)((value >> 16) & 0xFF);
    out[3] = (char)((value >> 24) & 0xFF);
}

static void probe(void) {
    if (probed) return;
    probed = 1;

    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    store4(&vendor[0], b);
    store4(&vendor[4], d);
    store4(&vendor[8], c);
    vendor[12] = '\0';

    /* The brand string only exists if the CPU says leaf 0x80000004 does. */
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        char raw[49];
        for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            uint32_t off = (leaf - 0x80000002u) * 16u;
            cpuid(leaf, &a, &b, &c, &d);
            store4(&raw[off], a);
            store4(&raw[off + 4], b);
            store4(&raw[off + 8], c);
            store4(&raw[off + 12], d);
        }
        raw[48] = '\0';
        const char* start = raw;
        while (*start == ' ') start++;  /* the string is right-aligned in its field */
        kstrlcpy(brand, start, sizeof(brand));
    } else {
        cpuid(1, &a, &b, &c, &d);
        char num[12];
        kstrcpy(brand, "x86 family ");
        ku32_to_dec((a >> 8) & 0xF, num);
        kstrcat(brand, num);
        kstrcat(brand, " model ");
        ku32_to_dec((a >> 4) & 0xF, num);
        kstrcat(brand, num);
    }
}

const char* cpu_vendor(void) {
    probe();
    return vendor;
}

const char* cpu_brand(void) {
    probe();
    return brand;
}

void cpu_feature_list(char* out, uint32_t size) {
    static const struct { uint8_t bit; const char* name; } edx_features[] = {
        { 0, "FPU" }, { 3, "PSE" }, { 4, "TSC" }, { 5, "MSR" }, { 6, "PAE" },
        { 8, "CX8" }, { 9, "APIC" }, { 13, "PGE" }, { 15, "CMOV" },
        { 23, "MMX" }, { 25, "SSE" }, { 26, "SSE2" },
    };
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);

    out[0] = '\0';
    for (uint32_t i = 0; i < sizeof(edx_features) / sizeof(edx_features[0]); i++) {
        if (!(d & (1u << edx_features[i].bit))) continue;
        if (out[0] && kstrlen(out) + 2 < size) kstrcat(out, ", ");
        if (kstrlen(out) + kstrlen(edx_features[i].name) + 1 < size) {
            kstrcat(out, edx_features[i].name);
        }
    }
    if (c & 1u) {  /* ECX bit 0 */
        if (kstrlen(out) + 8 < size) kstrcat(out, ", SSE3");
    }
}
