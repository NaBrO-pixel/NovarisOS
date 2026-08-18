/* fbdraw64.c - an ordinary Linux fbdev client.
 *
 * Nothing in this file knows what Novaris is. It opens /dev/fb0, asks
 * the driver what the screen is with the two FBIOGET ioctls every fbdev
 * program uses, maps it, draws, and unmaps - which is exactly what a
 * display driver above the kernel does, Wine's included.
 *
 * It is written against <linux/fb.h>'s structures without including it,
 * so that the field offsets it checks are stated here rather than
 * inherited. If the kernel fills the wrong offsets, this notices; a
 * program that just called the ioctl and drew would not.
 *
 * On Linux it needs a real framebuffer and permission to open it, which
 * a desktop session normally does not give - so it reports "no
 * framebuffer here" and exits 77 rather than failing. That keeps it a
 * program whose behaviour on the host is defined, which is the point of
 * every userland program in this tree.
 *
 * Exit codes:
 *    0  drew and verified
 *   77  no /dev/fb0 to draw on (the ordinary result on a Linux desktop)
 *   71  the driver answered, but with geometry that cannot be right
 *   72  the pixels did not read back
 */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

/* The two structures, by size only - every field this program wants is
 * read at its documented offset, so a mistake in the kernel's layout
 * shows up as a wrong number rather than being papered over by the
 * host's headers agreeing with the host's kernel. */
static unsigned char vinfo[160];
static unsigned char finfo[80];

static uint32_t u32(const unsigned char* p, int off) {
    uint32_t v;
    memcpy(&v, p + off, sizeof v);
    return v;
}

static void say(const char* s) { write(1, s, strlen(s)); }

int main(void) {
    int fd;
    uint32_t xres, yres, bpp, line_length, red_off, green_off, blue_off;
    uint32_t span;
    volatile uint32_t* fb;

    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        say("fbdraw: no /dev/fb0 here\n");
        return 77;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, finfo) < 0) {
        say("fbdraw: the driver would not describe itself\n");
        close(fd);
        return 71;
    }

    xres        = u32(vinfo, 0);
    yres        = u32(vinfo, 4);
    bpp         = u32(vinfo, 24);
    red_off     = u32(vinfo, 32);
    green_off   = u32(vinfo, 44);
    blue_off    = u32(vinfo, 56);
    line_length = u32(finfo, 48);

    /* The geometry has to be self-consistent before any of it is
     * trusted. A scanline shorter than a row of pixels is the specific
     * failure that produces a sheared picture, and it is worth catching
     * here rather than looking at. */
    if (xres == 0 || yres == 0 || bpp != 32 ||
        line_length < xres * (bpp / 8)) {
        say("fbdraw: the geometry cannot be right\n");
        close(fd);
        return 71;
    }
    if (red_off != 16 || green_off != 8 || blue_off != 0) {
        say("fbdraw: unexpected pixel layout\n");
        close(fd);
        return 71;
    }
    /* The band below is at fixed coordinates so that a host-side
     * screenshot check can be a pixel compare. Checked rather than
     * assumed: on a smaller screen those coordinates would be a write
     * past the end of the mapping. */
    if (xres < 1024 || yres < 768) {
        say("fbdraw: the screen is smaller than this program draws\n");
        close(fd);
        return 78;
    }

    span = line_length * yres;
    fb = mmap(0, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == (void*)-1) {
        say("fbdraw: could not map the screen\n");
        close(fd);
        return 72;
    }

    /* A band across the bottom of the screen, in a colour nothing else
     * draws. Below the kernel's test pattern so the two do not overlap
     * and each can be checked on its own. */
    {
        uint32_t stride = line_length / 4;
        uint32_t y, x;
        for (y = 620; y < 700 && y < yres; y++)
            for (x = 100; x < 900 && x < xres; x++)
                fb[(uint64_t)y * stride + x] = 0x00FF00FF;   /* magenta */

        /* Read back through the same mapping. On a shared mapping of a
         * device this is a round trip to the hardware, which is the
         * thing worth asserting: a private copy would pass it too, and
         * then nothing would be on the screen - so the host-side
         * screenshot check is what separates those two, not this. */
        if (fb[(uint64_t)650 * stride + 500] != 0x00FF00FF) {
            say("fbdraw: what was written did not read back\n");
            munmap((void*)fb, span);
            close(fd);
            return 72;
        }
    }

    munmap((void*)fb, span);
    close(fd);
    say("fbdraw: drew a magenta band on the mapped framebuffer\n");
    return 0;
}
