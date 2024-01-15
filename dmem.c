#include <stdlib.h>
#include <execinfo.h>
#include <strings.h>
#include <string.h>
#include <search.h>

#include "dmem.h"
#include "dlog.h"

char * xstrdup(const char * s) {
    if (!s)
        return (char *) 0;
    size_t len = strlen (s) + 1;
    char * result = (char *) xmalloc (len);
    if (result == (char *) 0)
        return (char *) 0;
    return (char *) memcpy (result, s, len);
}

void * xmalloc (size_t n) {
    if (!n) return(NULL);
    void * p = malloc (n);
    if (p) bzero(p, n);
    return p;
}

void * xrealloc(void * ptr, size_t n) {
    void * p = realloc (ptr, n);
    return(p);
}

void xfree(void * ptr) {
    if (ptr) {
        free(ptr);
    }
}
