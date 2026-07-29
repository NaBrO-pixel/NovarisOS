#include "libc.h"

/* No libc string functions yet - write a tiny local one. */
static void copy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

int main(void) {
    write("Hello from a real ELF executable (not a flat binary)!\n");

    char* buf = (char*)mmap_anon(128);
    if (!buf) {
        write("mmap_anon failed!\n");
        return 1;
    }
    copy(buf, "...and this string just round-tripped through mmap_anon().\n");
    write(buf);

    return 0;
}
