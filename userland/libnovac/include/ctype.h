#ifndef NOVAC_CTYPE_H
#define NOVAC_CTYPE_H
#include <novac/defs.h>

NOVAC_BEGIN_DECLS
/* "C" locale only (ASCII). Values outside 0..127 (including EOF) are
 * classified as nothing and converted to themselves. */
int isalnum(int c);  int isalpha(int c);  int isblank(int c);
int iscntrl(int c);  int isdigit(int c);  int isgraph(int c);
int islower(int c);  int isprint(int c);  int ispunct(int c);
int isspace(int c);  int isupper(int c);  int isxdigit(int c);
int tolower(int c);  int toupper(int c);
NOVAC_END_DECLS
#endif
