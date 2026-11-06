/* The C-SKY minilibc's <string.h> plus what it lacks: strtok_r (defined in
 * hal_txw81x.c). */
#ifndef LC_COMPAT_STRING_H
#define LC_COMPAT_STRING_H

#include_next <string.h>

char *strtok_r(char *str, const char *delim, char **saveptr);

#endif
