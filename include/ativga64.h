#ifndef ATIVGA64_H
#define ATIVGA64_H

#include <stdint.h>

/* ATI Rage 128 / Radeon display (Milestone 112). See kernel/ativga64.c. */

typedef struct {
    int      enabled;
    uint32_t width, height, bpp;
    uint32_t pitch;          /* bytes */
    uint32_t offset;         /* where in video memory the screen starts */
} ativga64_mode_t;

/* Finds the card among gpu64_scan()'s and maps its registers. 1 if so. */
int      ativga64_probe(void);
uint64_t ativga64_vram_size(void);
uint64_t ativga64_vram_phys(void);
int      ativga64_get_mode(ativga64_mode_t* m);
/* 32 bits a pixel; `pitch_pixels` a multiple of 8. */
int      ativga64_set_mode(uint32_t width, uint32_t height, uint32_t pitch_pixels, uint32_t offset);
int      ativga64_flip(uint32_t offset);

#endif
