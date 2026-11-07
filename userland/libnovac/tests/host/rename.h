/* HOST TESTS ONLY. Force-included into library and test translation units so
 * libNovaC's public symbols (memcpy, malloc, ...) are renamed and cannot
 * collide with the host libc linked into the test runner. Never used for the
 * NovarisOS target build. */
#ifndef NOVAC_HOST_RENAME_H
#define NOVAC_HOST_RENAME_H
#define memcpy novact_memcpy
#define memmove novact_memmove
#define memset novact_memset
#define memcmp novact_memcmp
#define memchr novact_memchr
#define strlen novact_strlen
#define strnlen novact_strnlen
#define strcmp novact_strcmp
#define strncmp novact_strncmp
#define strcpy novact_strcpy
#define strncpy novact_strncpy
#define strcat novact_strcat
#define strncat novact_strncat
#define strchr novact_strchr
#define strrchr novact_strrchr
#define strstr novact_strstr
#define strerror novact_strerror
#define isalnum novact_isalnum
#define isalpha novact_isalpha
#define isblank novact_isblank
#define iscntrl novact_iscntrl
#define isdigit novact_isdigit
#define isgraph novact_isgraph
#define islower novact_islower
#define isprint novact_isprint
#define ispunct novact_ispunct
#define isspace novact_isspace
#define isupper novact_isupper
#define isxdigit novact_isxdigit
#define tolower novact_tolower
#define toupper novact_toupper
#define malloc novact_malloc
#define calloc novact_calloc
#define realloc novact_realloc
#define free novact_free
#define abort novact_abort
#define exit novact_exit
#define _Exit novact__Exit
#define atexit novact_atexit
#define atoi novact_atoi
#define atol novact_atol
#define atoll novact_atoll
#define strtol novact_strtol
#define strtoll novact_strtoll
#define strtoul novact_strtoul
#define strtoull novact_strtoull
#define abs novact_abs
#define labs novact_labs
#define llabs novact_llabs
#define div novact_div
#define ldiv novact_ldiv
#define lldiv novact_lldiv
#define qsort novact_qsort
#define bsearch novact_bsearch
#define rand novact_rand
#define srand novact_srand
#define snprintf novact_snprintf
#define vsnprintf novact_vsnprintf
#define sprintf novact_sprintf
#define vsprintf novact_vsprintf
#define div_t novact_div_t
#define ldiv_t novact_ldiv_t
#define lldiv_t novact_lldiv_t
#endif
