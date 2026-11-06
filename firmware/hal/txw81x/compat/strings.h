/* The C-SKY minilibc has no <strings.h>; the SDK's lib/common provides the
 * functions. */
#ifndef LC_COMPAT_STRINGS_H
#define LC_COMPAT_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

#endif
