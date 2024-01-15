#define _GNU_SOURCE

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <pthread.h>
#include "dlog.h"

enum daemon_log_flags daemon_log_use = DAEMON_LOG_AUTO | DAEMON_LOG_STDERR;
const char *daemon_log_ident = NULL;

unsigned int def_prio =
        LOG_MASK(LOG_EMERG) | LOG_MASK(LOG_ALERT) | LOG_MASK(LOG_CRIT) | LOG_MASK(LOG_ERR) | LOG_MASK(LOG_WARNING) \
 | LOG_MASK(LOG_NOTICE) | LOG_MASK(LOG_INFO) | LOG_MASK(LOG_DEBUG);

const char *prio_names[] = {"[emerg]",
                            "[alert]",
                            "[crit ]",
                            "[error]",
                            "[warn ]",
                            "[notice]",
                            "[info] ",
                            "[debug]"
};
const char *blue = "";
const char *red = "";
const char *yellow = "";
const char *magenta = "";
const char *color_end = "";

const char **prio_names_colors[] = {&red,
                                    &red,
                                    &red,
                                    &red,
                                    &yellow,
                                    &yellow,
                                    &blue,
                                    &magenta
};

unsigned int daemon_get_prio(void) {
    return (def_prio);
}

const char *daemon_prio_name(unsigned int priority) {
    if (priority < sizeof(prio_names) / sizeof(prio_names[0])) {
        return (prio_names[priority]);
    } else {
        return ("[unk] ");
    }
}

const char *daemon_prio_color(unsigned int priority) {
    if (priority < sizeof(prio_names_colors) / sizeof(prio_names_colors[0])) {
        return (*prio_names_colors[priority]);
    } else {
        return ("");
    }
}

unsigned int daemon_log_upto(unsigned int prio) {
    unsigned int save_prio = def_prio;
    def_prio = LOG_UPTO(prio);
    return save_prio;
}

static unsigned long get_tid(void) {
    static __thread unsigned long _tid = 0;
    if (!_tid) {
        _tid = syscall(SYS_gettid);
    }
    return (_tid);
}

void daemon_logv(int prio, const char *template, va_list arglist) {
    int saved_errno;

    if ((LOG_MASK(prio) & def_prio) == 0) return;

    saved_errno = errno;
    va_list arglist1, arglist2, arglist3;
    va_copy(arglist1, arglist);
    va_copy(arglist2, arglist);
    va_copy(arglist3, arglist);
    if (daemon_log_use & DAEMON_LOG_SYSLOG) {
        char buffer[256] = {};
        openlog(daemon_log_ident ? daemon_log_ident : "UNKNOWN", 0, /*LOG_DAEMON*/ LOG_LOCAL1);
        vsnprintf(buffer, sizeof(buffer), template, arglist1);
        buffer[sizeof(buffer) - 1] = 0;

        char *ps = buffer, *pb = buffer;
        while (*pb) {
            if (*pb == '\n') {
                *pb = 0;
                syslog(prio | LOG_DAEMON, "%s[%05ld]%s", daemon_prio_name(prio), get_tid(), ps);
                ps = pb + 1;
            } else {
                if ((*pb == '\r') || (*pb == '\t')) *pb = ' ';
            }
            pb++;
        }
        if (pb != ps) {
            syslog(prio | LOG_DAEMON, "%s[%05ld]%s", daemon_prio_name(prio), get_tid(), ps);
        }
    }
    va_end(arglist1);
    if ((daemon_log_use & DAEMON_LOG_STDERR) || (daemon_log_use & DAEMON_LOG_STDOUT)) {
        time_t curtime = time(NULL);
        struct tm *now = localtime(&curtime);
        struct timeval hp_now;
        char buffer[512] = {};
        char time_buffer[21] = {};

        gettimeofday(&hp_now, NULL);
        strftime(time_buffer, 20, "%T", now);

        int ll = snprintf(buffer, sizeof(buffer) - 1, "%s.%04d %s%s%s [%05lu] ", time_buffer,
                          (int) hp_now.tv_usec / 100, daemon_prio_color(prio), daemon_prio_name(prio), color_end,
                          get_tid());
        buffer[sizeof(buffer) - 1] = 0;

        int l = sizeof(buffer) - ll - 1;

        strncat(buffer, template, l);
        buffer[sizeof(buffer) - 1] = 0;

        if (daemon_log_use & DAEMON_LOG_STDERR) {
            vfprintf(stderr, buffer, arglist2);
            fprintf(stderr, "\n");
        }

        if (daemon_log_use & DAEMON_LOG_STDOUT) {
            vfprintf(stdout, buffer, arglist3);
            fprintf(stdout, "\n");
        }
    }
    va_end(arglist2);
    va_end(arglist3);

    errno = saved_errno;
}

void daemon_log(int prio, const char *template, ...) {
    va_list arglist;

    if ((LOG_MASK(prio) & def_prio) == 0) return;

    va_start(arglist, template);
    daemon_logv(prio, template, arglist);
    va_end(arglist);
}

char *daemon_ident_from_argv0(char *argv0) {
    char *p;

    if ((p = strrchr(argv0, '/')))
        return p + 1;

    return argv0;
}

unsigned int log_check_prio(unsigned int priority) {
    return ((LOG_MASK(priority) & def_prio) != 0);
}


static pthread_mutex_t _indent_mtx = PTHREAD_MUTEX_INITIALIZER;
static __thread int _indent = 0;

static int indent_get() {
    int a;
    pthread_mutex_lock(&_indent_mtx);
    a = _indent;
    pthread_mutex_unlock(&_indent_mtx);
    return (a);
}

void indent_set(int indent) {
    pthread_mutex_lock(&_indent_mtx);
    _indent = indent;
    pthread_mutex_unlock(&_indent_mtx);
}

static bool _daemon_trace_on = true;

void daemon_enter(const char *func_name, const char *template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;

    indent_set(indent_get() + 1);
    if (_daemon_trace_on) {
        static __thread char tmp[256];
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c>[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
}

void daemon_leave(const char *func_name, const char *template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;

    if (_daemon_trace_on) {
        static __thread char tmp[256];
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c<[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
    indent_set(indent_get() - 1);
}

void daemon_trace_switch(bool on) {
    _daemon_trace_on = on;
}

bool daemon_trace_switch_get() {
    return (_daemon_trace_on);
}

void daemon_trace_indent_reset_after_error() {
    indent_set(0);
}

void daemon_trace(const char *func_name, const char *template, ...) {
    static pthread_mutex_t tmp_mtx = PTHREAD_MUTEX_INITIALIZER;
    if (_daemon_trace_on) {
        static __thread char tmp[256];
        pthread_mutex_lock(&tmp_mtx);
        int ll = snprintf(tmp, sizeof(tmp) - 1, "%*c*[%s] ", indent_get(), ' ', func_name);
        tmp[sizeof(tmp) - 1] = 0;
        int l = sizeof(tmp) - ll - 1;
        strncat(tmp, template, l);
        tmp[sizeof(tmp) - 1] = 0;
        va_list arglist;
        va_start(arglist, template);
        daemon_logv(LOG_INFO, tmp, arglist);
        va_end(arglist);
        pthread_mutex_unlock(&tmp_mtx);
    }
}

void hex_dump(const unsigned char *buf, int len) {
    int i, j;
    char buffer[2048];
    char *p = buffer;
    for (i = 0; i < len; i = i + 16) {
        int k = 0;
        p = buffer;

        p += sprintf(p, "0x%04x ", i);

        for (j = 0; ((j < 16) && (i + j) < len); j++) {
            p += sprintf(p, "%02X ", buf[i + j]);
            if (j == 7) p += sprintf(p, " ");
            k = j;
        }

        for (j = k; j < 16; j++) {
            p += sprintf(p, "   ");
            if (j == 7) p += sprintf(p, " ");
        }

        p += sprintf(p, "   ");

        for (j = 0; ((j < 16) && (i + j) < len); j++) {
            if (buf[i + j] >= 0x20 && buf[i + j] <= 0x7F)
                p += sprintf(p, "%c", buf[i + j]);
            else
                p += sprintf(p, ".");

            if (j == 7) p += sprintf(p, " ");
        }
        daemon_log(LOG_ERR, "%s", buffer);;
    }
}

static bool is_member(const char *item, const char *list[]) {
    int i = 0;
    while (list[i] && 0 != strcmp(item, list[i])) {
        i++;
    }
    return list[i] != 0;
}

static void initialize_terminal(void) {
    const char *color_terms[] = {"linux", "xterm", "xterm-color", "xterm-256color",
                                 "Eterm", "rxvt", "rxvt-unicode", 0
    };
    const char *term = getenv("TERM");
    if (term == 0) {
        term = "dumb";
    }
    if (is_member(term, color_terms)) {
        blue = "\033[34m";
        red = "\033[31m";
        magenta = "\033[35m";
        color_end = "\033[0m";
        yellow = "\033[33m";
    }
}

static
void _dlog_module_init() __attribute__ ((constructor));

static
void _dlog_module_init() {
    initialize_terminal();
}