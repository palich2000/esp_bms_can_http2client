#include <fcntl.h>

#include "dnonblock.h"

int daemon_nonblock(int fd, int b) {
    int a;
    if ((a = fcntl(fd, F_GETFL)) < 0)
        return -1;

    if (b)
        a |= O_NDELAY;
    else
        a &= ~O_NDELAY;

    if (fcntl(fd, F_SETFL, a) < 0)
        return -1;

    return 0;
}
