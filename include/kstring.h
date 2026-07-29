#ifndef KSTRING_H
#define KSTRING_H

#include <stdint.h>
#include <stddef.h>

/* The string/memory primitives the Win32 layer needs everywhere. Earlier
 * files each hand-rolled the two or three helpers they happened to want
 * (see the `streq`/`starts_with` pair at the top of kernel/shell.c);
 * kernel/win32*.c needs enough of them - and needs to *export* several of
 * them to emulated programs as msvcrt's strlen/strcmp/memcpy/... - that
 * one shared implementation is worth having.
 *
 * Deliberately not named memcpy/strlen/etc: -ffreestanding still lets GCC
 * synthesize calls to those four names, and having our own definitions
 * collide with the compiler's assumptions about them is a subtle trap. */

void  kmemcpy(void* dst, const void* src, uint32_t n);
void  kmemmove(void* dst, const void* src, uint32_t n);
void  kmemset(void* dst, uint8_t value, uint32_t n);
int   kmemcmp(const void* a, const void* b, uint32_t n);
void* kmemchr(const void* s, uint8_t c, uint32_t n);

uint32_t kstrlen(const char* s);
uint32_t kstrnlen(const char* s, uint32_t max);
int   kstrcmp(const char* a, const char* b);
int   kstrncmp(const char* a, const char* b, uint32_t n);
/* ASCII case-insensitive - Win32 module and export lookups are
 * case-insensitive, and import tables spell DLL names every which way
 * ("KERNEL32.dll", "kernel32.DLL", ...). */
int   kstricmp(const char* a, const char* b);
char* kstrcpy(char* dst, const char* src);
/* Bounded copy that always NUL-terminates (unlike strncpy). Returns dst. */
char* kstrlcpy(char* dst, const char* src, uint32_t size);
char* kstrcat(char* dst, const char* src);
char* kstrchr(const char* s, char c);
char* kstrrchr(const char* s, char c);
char* kstrstr(const char* haystack, const char* needle);

/* Suffix test, case-insensitive. Used to spot ".exe"/".dll" names. */
int   kstr_ends_with_ci(const char* s, const char* suffix);

/* Number -> text. Both return the number of characters written (excluding
 * the NUL). `width` pads with leading zeros when > 0. */
uint32_t ku32_to_dec(uint32_t value, char* out);
uint32_t ku32_to_hex(uint32_t value, char* out, int uppercase, int width);

/* Text -> number. Stops at the first character that isn't a digit. */
int32_t  kstr_to_int(const char* s);

/* 64-bit division, done by hand.
 *
 * GCC turns `uint64_t / uint32_t` on i386 into a call to libgcc's
 * __udivmoddi4, and a freestanding kernel doesn't link libgcc - so
 * printing a 64-bit value or running the bignum in win32_dtoa.c would
 * fail at link time. The x86 DIV instruction already does exactly what's
 * needed (it divides the 64-bit EDX:EAX by a 32-bit operand), so these
 * two wrap it directly rather than pulling in a support library. */

/* Divides value by divisor; stores the remainder if `remainder` is
 * non-null. */
uint64_t kdiv64(uint64_t value, uint32_t divisor, uint32_t* remainder);

/* One DIV step: divides the 64-bit quantity (hi:lo) by divisor and
 * returns the 32-bit quotient. `hi` must be less than `divisor` - which
 * is what makes the quotient fit in 32 bits, and is naturally true when
 * chaining these across the limbs of a bignum, since each step's
 * remainder feeds the next step's `hi`. */
uint32_t kdiv_hi_lo(uint32_t hi, uint32_t lo, uint32_t divisor,
                    uint32_t* remainder);

#endif
