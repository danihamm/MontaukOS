/*
    * main.cpp
    * Init system for MontaukOS (PID 0)
    * Chains system services then launches the shell.
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>

// ---- Minimal snprintf ----

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState { char* buf; int pos; int max; };

static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24]; int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}

static int vsnprintf(char* buf, int size, const char* fmt, va_list ap) {
    PfState st; st.buf = buf; st.pos = 0; st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); } else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg); break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) { if (st.pos < size) st.buf[st.pos] = '\0'; else st.buf[size - 1] = '\0'; }
    return st.pos;
}

static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap); return ret;
}

// ---- ANSI color codes ----

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"

// ---- Logging ----

static void log_timestamp(char* buf, int size) {
    Montauk::DateTime dt;
    montauk::gettime(&dt);
    snprintf(buf, size, "%02d:%02d:%02d", dt.Hour, dt.Minute, dt.Second);
}

enum LogLevel { LOG_OK, LOG_INFO, LOG_WARN, LOG_ERR };

static void log(LogLevel level, const char* msg) {
    char line[512];
    char ts[16];
    log_timestamp(ts, sizeof(ts));

    const char* tag;
    const char* color;
    switch (level) {
    case LOG_OK:   tag = "  OK  "; color = C_GREEN;  break;
    case LOG_INFO: tag = " INFO "; color = C_CYAN;   break;
    case LOG_WARN: tag = " WARN "; color = C_YELLOW; break;
    case LOG_ERR:  tag = " FAIL "; color = C_RED;    break;
    }

    snprintf(line, sizeof(line),
        C_DIM "%s" C_RESET "  %s%s" C_RESET "  " C_BOLD "init" C_RESET "  %s\n",
        ts, color, tag, msg);
    montauk::print(line);
}

static void log_ok(const char* msg)   { log(LOG_OK, msg); }
static void log_info(const char* msg) { log(LOG_INFO, msg); }
static void log_warn(const char* msg) { log(LOG_WARN, msg); }
static void log_err(const char* msg)  { log(LOG_ERR, msg); }

// ---- Service runner ----

static bool run_service(const char* path, const char* name) {
    char msg[128];

    snprintf(msg, sizeof(msg), "Starting %s", name);
    log_info(msg);

    int pid = montauk::spawn(path);
    if (pid < 0) {
        snprintf(msg, sizeof(msg), "Failed to start %s", name);
        log_err(msg);
        return false;
    }

    montauk::waitpid(pid);

    snprintf(msg, sizeof(msg), "%s finished (pid %d)", name, pid);
    log_ok(msg);
    return true;
}

// ---- Main ----

extern "C" void _start() {

    log_info("The MontaukOS Operating System");

    // ---- Stage 1: Network configuration ----
    run_service("0:/os/dhcp.elf", "dhcp");

    // ---- Stage 2: Desktop environment (falls back to shell) ----
    if (!run_service("0:/os/desktop.elf", "desktop")) {
        log_warn("Desktop failed, falling back to shell");
        run_service("0:/os/shell.elf", "shell");
    }

    log_warn("All services exited");

    for (;;) {
        montauk::yield();
    }
}
