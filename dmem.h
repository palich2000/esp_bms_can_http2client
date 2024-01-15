#ifndef foodmemh
#define foodmemh
#include <stdlib.h>

void * xmalloc (size_t);
void * xrealloc(void *, size_t);
void xfree(void * ptr);
char * xstrdup(const char * s);
#ifndef FREE

#define FREE(x) \
    do { \
        if (x) {\
            xfree(x); \
            x=NULL; \
        } \
        break; \
    }  while(0) \

#endif // FREE

#endif // foodmemh

