#define _POSIX_SOURCE
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include "dlog.h"
#include "dsignal.h"
#include "dfork.h"

#include "dexec.h"

#define MAX_ARGS 100

int daemon_execv(const char * dir, int * ret, const char * prog, va_list ap) {
    pid_t pid;
    int p[2];
    unsigned n = 0;
    static char buf[256];
    int sigfd, r;
    fd_set fds;

    assert(daemon_signal_fd() >= 0);

    if (pipe(p) < 0) {
        daemon_log(LOG_ERR, "pipe() failed: %s", strerror(errno));
        return -1;
    }

    if ((pid = fork()) < 0) {
        daemon_log(LOG_ERR, "fork() failed: %s", strerror(errno));
        return -1;

    } else if (pid == 0) {
        char * args[MAX_ARGS];
        int i;

        if (p[1] != 1)
            dup2(p[1], 1);

        if (p[1] != 2)
            dup2(p[1], 2);

        if (p[0] > 2)
            close(p[0]);

        if (p[1] > 2)
            close(p[1]);

        close(0);
        if (open("/dev/null", O_RDONLY) != 0) {
            daemon_log(LOG_ERR, "Unable to open /dev/null as STDIN");
            _exit(EXIT_FAILURE);
        }

        daemon_close_all(-1);

        umask(0022); /* Set up a sane umask */

        if (dir && chdir(dir) < 0) {
            daemon_log(LOG_WARNING, "Failed to change to directory '%s'", dir);
            if (chdir("/") < 0) {
                daemon_log(LOG_ERR, "chdir to / failed (%d) %s", errno, strerror(errno));
            }
        }

        for (i = 0; i < MAX_ARGS - 1; i++)
            if (!(args[i] = va_arg(ap, char *)))
                break;
        args[i] = NULL;

        execv(prog, args);

        daemon_log(LOG_ERR, "execv(%s) failed: %s", prog, strerror(errno));

        _exit(EXIT_FAILURE);
    }

    close(p[1]);

    FD_ZERO(&fds);
    FD_SET(p[0], &fds);
    sigfd = daemon_signal_fd();
    FD_SET(sigfd, &fds);

    n = 0;

    for (;;) {
        fd_set qfds = fds;

        if (select(FD_SETSIZE, &qfds, NULL, NULL, NULL) < 0) {

            if (errno == EINTR)
                continue;

            daemon_log(LOG_ERR, "select() failed: %s", strerror(errno));
            return -1;
        }

        if (FD_ISSET(p[0], &qfds)) {
            char c;

            if (read(p[0], &c, 1) != 1)
                break;

            buf[n] = c;

            if (c == '\n' || n >= sizeof(buf) - 2) {
                if (c != '\n') n++;
                buf[n] = 0;

                if (buf[0])
                    daemon_log(LOG_INFO, "client: %s", buf);

                n = 0;
            } else
                n++;
        }

        if (FD_ISSET(sigfd, &qfds)) {
            int sig;

            if ((sig = daemon_signal_next()) < 0) {
                daemon_log(LOG_ERR, "daemon_signal_next(): %s", strerror(errno));
                break;
            }

            if (sig != SIGCHLD) {
                daemon_log(LOG_WARNING, "Killing child.");
                kill(pid, SIGTERM);
            }
        }
    }

    if (n > 0) {
        buf[n] = 0;
        daemon_log(LOG_WARNING, "client: %s", buf);
    }

    close(p[0]);

    for (;;) {
        if (waitpid(pid, &r, 0) < 0) {

            if (errno == EINTR)
                continue;

            daemon_log(LOG_ERR, "waitpid(): %s", strerror(errno));
            return -1;
        } else {
            if (!WIFEXITED(r))
                return -1;

            if (ret)
                *ret = WEXITSTATUS(r);

            return 0;
        }
    }
}

int daemon_exec(const char * dir,  int * ret, const char * prog, ...) {
    va_list ap;
    int r;

    va_start(ap, prog);
    r = daemon_execv(dir, ret, prog, ap);
    va_end(ap);

    return r;
}


pid_t daemon_execv1(const char * dir, const char * prog, va_list ap) {
    pid_t pid;

    assert(daemon_signal_fd() >= 0);


    if ((pid = fork()) < 0) {
        daemon_log(LOG_ERR, "fork() failed: %s", strerror(errno));
        return -1;

    } else if (pid == 0) {
        char * args[MAX_ARGS];
        int i;

        umask(0022); /* Set up a sane umask */

        if (dir && chdir(dir) < 0) {
            daemon_log(LOG_WARNING, "Failed to change to directory '%s'", dir);
            if (chdir("/") < 0) {
                daemon_log(LOG_ERR, "chdir to / failed (%d) %s", errno, strerror(errno));
            }
        }

        for (i = 0; i < MAX_ARGS - 1; i++)
            if (!(args[i] = va_arg(ap, char *)))
                break;
        args[i] = NULL;

        execv(prog, args);

        daemon_log(LOG_ERR, "execv(%s) failed: %s", prog, strerror(errno));

        _exit(EXIT_FAILURE);
    }

    daemon_log(LOG_INFO, "pid=%d", pid);
    return(pid);

}

pid_t daemon_exec1(const char * dir, const char * prog, ...) {
    va_list ap;
    pid_t r;

    va_start(ap, prog);
    r = daemon_execv1(dir, prog, ap);
    va_end(ap);

    return r;
}


#define CHILD_TO_PARENT         pipe_to_child[1]
#define PARENT_TO_CHILD         pipe_from_child[1]
#define CHILD_FROM_PARENT       pipe_from_child[0]
#define PARENT_FROM_CHILD       pipe_to_child[0]
#define CHILD_ERROR_TO_PARENT   pipe_error[1]
#define PARENT_ERROR_FROM_CHILD pipe_error[0]


int
daemon_execva2(const char * dir, const char * prog,
               int * read_fd, int * write_fd, int * error_fd, pid_t * pidp,
               va_list ap) {
    char * args[MAX_ARGS];
    int i;
    for (i = 0; i < MAX_ARGS - 1; i++) {
        if (!(args[i] = va_arg(ap, char *))) {
            syslog(LOG_INFO, "ARG[%d]=%s", i, args[i]);
            break;
        } else {
            syslog(LOG_INFO, "ARG[%d]=%s", i, args[i]);
        }
    }
    args[i] = NULL;
    return(daemon_execv2(dir, prog, read_fd, write_fd, error_fd, pidp, args));
}


int
daemon_execv2(const char * dir, const char * prog,
              int * read_fd, int * write_fd, int * error_fd, pid_t * pidp,
              char * const args[]) {
    pid_t pid;
    int pipe_from_child[2], pipe_to_child[2], pipe_error[2];

    pipe_from_child[0] = pipe_from_child[1] = -1;
    pipe_to_child[0] = pipe_to_child[1] = -1;
    pipe_error[0] = pipe_error[1] = -1;

    if (pipe(pipe_from_child) == -1) {
        daemon_log(LOG_ERR, "exec2: pipe() failed");
        goto err;
    }

    if (pipe(pipe_to_child) == -1) {
        daemon_log(LOG_ERR, "exec2: pipe() failed");
        goto err;
    }

    if (pipe(pipe_error) == -1) {
        daemon_log(LOG_ERR, "exec2: pipe() failed");
        goto err;
    }

    signal(SIGPIPE, SIG_IGN);

    daemon_log(LOG_ERR, "exec2: executing %s", prog);

    if ((pid = fork()) == 0) {

        close(PARENT_FROM_CHILD);
        close(PARENT_TO_CHILD);
        close(PARENT_ERROR_FROM_CHILD);
        close(0);
        if (dup(CHILD_FROM_PARENT) < 0 ) {
            daemon_log(LOG_ERR, "dup failed (%d) %s", errno, strerror(errno));
        }
        close(1);
        if (dup(CHILD_TO_PARENT) < 0) {
            daemon_log(LOG_ERR, "dup failed (%d) %s", errno, strerror(errno));
        }
        close(2);
        if (dup(CHILD_ERROR_TO_PARENT) < 0) {
            daemon_log(LOG_ERR, "dup failed (%d) %s", errno, strerror(errno));
        }


        umask(0022);

        if (dir && chdir(dir) < 0) {
            daemon_log(LOG_WARNING, "Failed to change to directory '%s'", dir);
            if (chdir("/") < 0) {
                daemon_log(LOG_ERR, "chdir to / failed (%d) %s", errno, strerror(errno));
            }
        }

        if (execv(prog, args) < 0) {
            daemon_log(LOG_ERR, "exec2: execv of %s failed! (%d) %s", prog, errno, strerror(errno));
        }

        _exit(1);
    } else if (pid == -1) {
        daemon_log(LOG_ERR, "exec2: fork failed! (%d) %s", errno, strerror(errno));
        goto err;
    }


    close(CHILD_TO_PARENT);
    close(CHILD_FROM_PARENT);
    close(CHILD_ERROR_TO_PARENT);

    if (write_fd != NULL)
        *write_fd = PARENT_TO_CHILD;
    else
        close(PARENT_TO_CHILD);

    if (read_fd != NULL)
        *read_fd = PARENT_FROM_CHILD;
    else
        close(PARENT_FROM_CHILD);

    if (error_fd != NULL)
        *error_fd = PARENT_ERROR_FROM_CHILD;
    else
        close(PARENT_ERROR_FROM_CHILD);

    if (pidp != NULL)
        *pidp = pid;

    return(0);

err:
    if (pipe_from_child[0] != -1)
        close(pipe_from_child[0]);
    if (pipe_from_child[1] != -1)
        close(pipe_from_child[1]);
    if (pipe_to_child[0] != -1)
        close(pipe_to_child[0]);
    if (pipe_to_child[1] != -1)
        close(pipe_to_child[1]);
    if (pipe_error[0] != -1)
        close(pipe_error[0]);
    if (pipe_error[1] != -1)
        close(pipe_error[1]);

    return(-1);
}

int daemon_exec2(const char * dir, const char * prog,
                 int * read_fd, int * write_fd, int * error_fd, pid_t * pidp, ...) {
    va_list ap;
    pid_t r;

    va_start(ap, pidp);
    r = daemon_execva2(dir, prog, read_fd, write_fd, error_fd, pidp, ap);
    va_end(ap);

    return r;
}
