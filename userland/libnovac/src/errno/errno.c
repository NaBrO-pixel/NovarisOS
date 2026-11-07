#include <errno.h>

/* Single process-wide variable. Not thread-safe; revisit when NovarisOS
 * provides thread-local storage. */
static int novac_errno_value;

int *__novac_errno_location(void) { return &novac_errno_value; }
