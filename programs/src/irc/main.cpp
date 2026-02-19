/*
    * main.cpp
    * IRC client for ZenithOS
    * Split-screen terminal UI with ANSI escape codes
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::slen;
using zenith::streq;
using zenith::starts_with;
using zenith::skip_spaces;
using zenith::strcpy;
using zenith::strncpy;
using zenith::memcpy;
using zenith::memmove;

// ---- Minimal snprintf (no libc available) ----

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState {
    char*  buf;
    int    pos;
    int    max;
};

static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24];
    int i = 0;
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
    PfState st;
    st.buf = buf;
    st.pos = 0;
    st.max = size > 0 ? size - 1 : 0;
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
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); }
            else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg);
            break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
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
    if (size > 0) {
        if (st.pos < size) st.buf[st.pos] = '\0';
        else st.buf[size - 1] = '\0';
    }
    return st.pos;
}

static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

// Case-insensitive comparison for IRC commands
static bool streqi(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

// ---- IP parsing (from shell) ----

static bool parse_ip(const char* s, uint32_t* out) {
    uint32_t octets[4];
    int idx = 0;
    uint32_t val = 0;
    bool hasDigit = false;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255) return false;
            hasDigit = true;
        } else if (c == '.' || c == '\0') {
            if (!hasDigit || idx >= 4) return false;
            octets[idx++] = val;
            val = 0; hasDigit = false;
            if (c == '\0') break;
        } else return false;
    }
    if (idx != 4) return false;
    *out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

static bool parse_uint16(const char* s, uint16_t* out) {
    uint32_t val = 0;
    if (*s == '\0') return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        val = val * 10 + (*s - '0');
        if (val > 65535) return false;
        s++;
    }
    *out = (uint16_t)val;
    return true;
}

// ---- Data structures ----

static constexpr int MAX_NICK_LEN     = 32;
static constexpr int MAX_CHANNEL_LEN  = 64;
static constexpr int MAX_LINE_LEN     = 256;
static constexpr int MAX_DISPLAY_LINES = 512;
static constexpr int IRC_MAX_MSG      = 512;

struct IrcState {
    int      fd;
    uint32_t serverIp;
    uint16_t serverPort;
    char     nick[MAX_NICK_LEN];
    char     channel[MAX_CHANNEL_LEN];
    bool     registered;
    bool     connected;
    bool     inChannel;
    int      nickRetries;
};

struct RecvBuffer {
    char buf[2048];
    int  len;
};

struct DisplayLine {
    char text[MAX_LINE_LEN];
    int  len;
};

struct MessageBuffer {
    DisplayLine lines[MAX_DISPLAY_LINES];
    int head;
    int count;
    int scrollOffset;
};

struct InputState {
    char buf[512];
    int  pos;
    int  len;
};

struct TermState {
    int cols;
    int rows;
    int msgAreaRows;
};

// ---- Terminal helpers ----

static int int_to_buf(char* buf, int n) {
    if (n == 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int i = 0;
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

// ---- Screen buffer for flicker-free rendering ----

static constexpr int SCREEN_BUF_SIZE = 32768;

struct ScreenBuf {
    char buf[SCREEN_BUF_SIZE];
    int  pos;
};

static ScreenBuf screen;

static void sb_reset() { screen.pos = 0; }

static void sb_putc(char c) {
    if (screen.pos < SCREEN_BUF_SIZE - 1) screen.buf[screen.pos++] = c;
}

static void sb_puts(const char* s) {
    while (*s && screen.pos < SCREEN_BUF_SIZE - 1) screen.buf[screen.pos++] = *s++;
}

static void sb_cursor_to(int row, int col) {
    sb_puts("\033[");
    char tmp[12]; int n = int_to_buf(tmp, row); for (int i = 0; i < n; i++) sb_putc(tmp[i]);
    sb_putc(';');
    n = int_to_buf(tmp, col); for (int i = 0; i < n; i++) sb_putc(tmp[i]);
    sb_putc('H');
}

static void sb_flush() {
    screen.buf[screen.pos] = '\0';
    zenith::print(screen.buf);
}

// ---- Globals ----

static IrcState     irc;
static RecvBuffer   recvBuf;
static MessageBuffer msgBuf;
static InputState   input;
static TermState    term;
static bool         running;
static bool         dirty;

// ---- Message buffer ----

static void msg_add(const char* text) {
    int idx = (msgBuf.head + msgBuf.count) % MAX_DISPLAY_LINES;
    if (msgBuf.count >= MAX_DISPLAY_LINES) {
        // Overwrite oldest
        msgBuf.head = (msgBuf.head + 1) % MAX_DISPLAY_LINES;
    } else {
        msgBuf.count++;
    }
    DisplayLine& line = msgBuf.lines[idx];
    int i = 0;
    while (text[i] && i < MAX_LINE_LEN - 1) {
        line.text[i] = text[i];
        i++;
    }
    line.text[i] = '\0';
    line.len = i;

    // Auto-scroll to bottom when new message arrives
    if (msgBuf.scrollOffset == 0 || msgBuf.count <= term.msgAreaRows) {
        msgBuf.scrollOffset = 0;
    }
    dirty = true;
}

static void msg_add_fmt(const char* fmt, ...) {
    char tmp[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    msg_add(tmp);
}

static void msg_clear() {
    msgBuf.head = 0;
    msgBuf.count = 0;
    msgBuf.scrollOffset = 0;
}

// ---- IRC send helpers ----

static void irc_send(const char* fmt, ...) {
    char buf[IRC_MAX_MSG];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    // Ensure \r\n termination
    if (len > IRC_MAX_MSG - 3) len = IRC_MAX_MSG - 3;
    buf[len] = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';
    zenith::send(irc.fd, buf, len + 2);
}

// ---- Sanitize incoming text (strip control chars) ----

static void sanitize(char* dst, const char* src, int maxLen) {
    int j = 0;
    for (int i = 0; src[i] && j < maxLen - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1B) {
            // Skip ESC sequences entirely
            continue;
        }
        if (c < 0x20 && c != ' ') continue;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

// ---- IRC prefix parsing ----

// Extract nick from ":nick!user@host" prefix
static void parse_prefix_nick(const char* prefix, char* nick, int maxLen) {
    int i = 0;
    if (prefix[0] == ':') prefix++;
    while (prefix[i] && prefix[i] != '!' && prefix[i] != '@' && prefix[i] != ' ' && i < maxLen - 1) {
        nick[i] = prefix[i];
        i++;
    }
    nick[i] = '\0';
}

// ---- IRC protocol handlers ----

static void irc_handle_ping(const char* params) {
    char buf[IRC_MAX_MSG];
    snprintf(buf, sizeof(buf), "PONG %s", params);
    irc_send("%s", buf);
}

static void irc_handle_privmsg(const char* prefix, const char* params) {
    char senderNick[MAX_NICK_LEN];
    parse_prefix_nick(prefix, senderNick, sizeof(senderNick));

    // Find the message text after " :"
    const char* text = params;
    // Skip target
    while (*text && *text != ' ') text++;
    text = skip_spaces(text);
    if (*text == ':') text++;

    char clean[MAX_LINE_LEN];
    sanitize(clean, text, sizeof(clean));

    // Check for CTCP ACTION (\001ACTION ... \001)
    if (starts_with(clean, "\001ACTION ")) {
        const char* action = clean + 8;
        // Strip trailing \001
        char actionBuf[MAX_LINE_LEN];
        strncpy(actionBuf, action, sizeof(actionBuf));
        int alen = slen(actionBuf);
        if (alen > 0 && actionBuf[alen - 1] == '\001') actionBuf[alen - 1] = '\0';
        msg_add_fmt("\033[35m* %s %s\033[0m", senderNick, actionBuf);
        return;
    }

    // Color own nick green, others cyan
    if (streq(senderNick, irc.nick)) {
        msg_add_fmt("\033[1;32m<%s>\033[0m %s", senderNick, clean);
    } else {
        msg_add_fmt("\033[1;36m<%s>\033[0m %s", senderNick, clean);
    }
}

static void irc_handle_notice(const char* prefix, const char* params) {
    char senderNick[MAX_NICK_LEN];
    if (prefix[0]) {
        parse_prefix_nick(prefix, senderNick, sizeof(senderNick));
    } else {
        strcpy(senderNick, "*");
    }

    const char* text = params;
    while (*text && *text != ' ') text++;
    text = skip_spaces(text);
    if (*text == ':') text++;

    char clean[MAX_LINE_LEN];
    sanitize(clean, text, sizeof(clean));
    msg_add_fmt("\033[1m-%s-\033[0m %s", senderNick, clean);
}

static void irc_handle_join(const char* prefix, const char* params) {
    char nick[MAX_NICK_LEN];
    parse_prefix_nick(prefix, nick, sizeof(nick));

    const char* chan = params;
    if (*chan == ':') chan++;

    if (streq(nick, irc.nick)) {
        strncpy(irc.channel, chan, sizeof(irc.channel));
        irc.inChannel = true;
        msg_add_fmt("\033[33m* Now talking in %s\033[0m", chan);
    } else {
        msg_add_fmt("\033[33m* %s has joined %s\033[0m", nick, chan);
    }
}

static void irc_handle_part(const char* prefix, const char* params) {
    char nick[MAX_NICK_LEN];
    parse_prefix_nick(prefix, nick, sizeof(nick));

    const char* chan = params;
    while (*chan && *chan != ' ') chan++;  // get channel text
    // Actually parse channel from params start
    chan = params;
    char chanBuf[MAX_CHANNEL_LEN];
    int i = 0;
    while (chan[i] && chan[i] != ' ' && i < MAX_CHANNEL_LEN - 1) {
        chanBuf[i] = chan[i]; i++;
    }
    chanBuf[i] = '\0';

    if (streq(nick, irc.nick)) {
        irc.inChannel = false;
        irc.channel[0] = '\0';
        msg_add_fmt("\033[33m* You have left %s\033[0m", chanBuf);
    } else {
        msg_add_fmt("\033[33m* %s has left %s\033[0m", nick, chanBuf);
    }
}

static void irc_handle_quit(const char* prefix, const char* params) {
    char nick[MAX_NICK_LEN];
    parse_prefix_nick(prefix, nick, sizeof(nick));

    const char* reason = params;
    if (*reason == ':') reason++;

    char clean[MAX_LINE_LEN];
    sanitize(clean, reason, sizeof(clean));
    if (clean[0]) {
        msg_add_fmt("\033[33m* %s has quit (%s)\033[0m", nick, clean);
    } else {
        msg_add_fmt("\033[33m* %s has quit\033[0m", nick);
    }
}

static void irc_handle_nick(const char* prefix, const char* params) {
    char oldNick[MAX_NICK_LEN];
    parse_prefix_nick(prefix, oldNick, sizeof(oldNick));

    const char* newNick = params;
    if (*newNick == ':') newNick++;

    if (streq(oldNick, irc.nick)) {
        strncpy(irc.nick, newNick, sizeof(irc.nick));
        msg_add_fmt("\033[33m* You are now known as %s\033[0m", newNick);
    } else {
        msg_add_fmt("\033[33m* %s is now known as %s\033[0m", oldNick, newNick);
    }
}

static void irc_handle_numeric(int num, const char* params) {
    // Extract trailing text (after the last " :")
    const char* text = params;
    const char* lastColon = nullptr;
    for (int i = 0; text[i]; i++) {
        if (text[i] == ':' && (i == 0 || text[i - 1] == ' ')) {
            lastColon = text + i + 1;
        }
    }

    switch (num) {
    case 1: // RPL_WELCOME
        irc.registered = true;
        if (lastColon) {
            char clean[MAX_LINE_LEN];
            sanitize(clean, lastColon, sizeof(clean));
            msg_add_fmt("\033[1m*** %s\033[0m", clean);
        }
        // Auto-join channel if specified
        if (irc.channel[0]) {
            irc_send("JOIN %s", irc.channel);
        }
        break;

    case 433: // ERR_NICKNAMEINUSE
        if (irc.nickRetries < 3) {
            int nlen = slen(irc.nick);
            if (nlen < MAX_NICK_LEN - 2) {
                irc.nick[nlen] = '_';
                irc.nick[nlen + 1] = '\0';
            }
            irc.nickRetries++;
            irc_send("NICK %s", irc.nick);
            msg_add_fmt("\033[33m* Nick in use, trying %s\033[0m", irc.nick);
        } else {
            msg_add("\033[31m*** Could not find available nickname\033[0m");
        }
        break;

    case 332: // RPL_TOPIC
    case 353: // RPL_NAMREPLY
    case 366: // RPL_ENDOFNAMES
    case 372: // RPL_MOTD
    case 375: // RPL_MOTDSTART
    case 376: // RPL_ENDOFMOTD
    default:
        if (lastColon) {
            char clean[MAX_LINE_LEN];
            sanitize(clean, lastColon, sizeof(clean));
            msg_add_fmt("\033[1m*** %s\033[0m", clean);
        }
        break;
    }
}

// ---- IRC line parser ----

static void irc_process_line(char* line) {
    // Strip trailing \r\n
    int len = slen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char prefix[256] = "";
    char* cmd = line;

    // Parse optional prefix
    if (line[0] == ':') {
        cmd = line + 1;
        int i = 0;
        while (cmd[i] && cmd[i] != ' ') i++;
        if (i > 0) {
            memcpy(prefix, cmd, i < 255 ? i : 255);
            prefix[i < 255 ? i : 255] = '\0';
        }
        cmd += i;
        while (*cmd == ' ') cmd++;
    }

    // Extract command word
    char command[32] = "";
    {
        int i = 0;
        while (cmd[i] && cmd[i] != ' ' && i < 31) {
            command[i] = cmd[i]; i++;
        }
        command[i] = '\0';
        cmd += i;
        while (*cmd == ' ') cmd++;
    }

    // cmd now points to params

    // Handle PING first (highest priority)
    if (streqi(command, "PING")) {
        irc_handle_ping(cmd);
        return;
    }

    // Check for numeric command
    bool isNumeric = true;
    for (int i = 0; command[i]; i++) {
        if (command[i] < '0' || command[i] > '9') { isNumeric = false; break; }
    }

    if (isNumeric && command[0]) {
        int num = 0;
        for (int i = 0; command[i]; i++) num = num * 10 + (command[i] - '0');
        irc_handle_numeric(num, cmd);
        return;
    }

    if (streqi(command, "PRIVMSG"))     irc_handle_privmsg(prefix, cmd);
    else if (streqi(command, "NOTICE")) irc_handle_notice(prefix, cmd);
    else if (streqi(command, "JOIN"))   irc_handle_join(prefix, cmd);
    else if (streqi(command, "PART"))   irc_handle_part(prefix, cmd);
    else if (streqi(command, "QUIT"))   irc_handle_quit(prefix, cmd);
    else if (streqi(command, "NICK"))   irc_handle_nick(prefix, cmd);
    else if (streqi(command, "PONG"))   { /* Ignore PONG replies */ }
    else if (streqi(command, "ERROR")) {
        char clean[MAX_LINE_LEN];
        sanitize(clean, cmd, sizeof(clean));
        msg_add_fmt("\033[31m*** Error: %s\033[0m", clean);
    }
}

// ---- TCP recv with fragment assembly ----

static void recv_process() {
    char tmp[512];
    int r = zenith::recv(irc.fd, tmp, sizeof(tmp));
    if (r < 0) {
        irc.connected = false;
        msg_add("\033[31m*** Connection lost\033[0m");
        return;
    }
    if (r == 0) return;

    // Append to recv buffer
    int space = (int)sizeof(recvBuf.buf) - recvBuf.len;
    if (r > space) r = space;
    memcpy(recvBuf.buf + recvBuf.len, tmp, r);
    recvBuf.len += r;

    // Scan for complete lines (\r\n)
    int start = 0;
    for (int i = 0; i < recvBuf.len - 1; i++) {
        if (recvBuf.buf[i] == '\r' && recvBuf.buf[i + 1] == '\n') {
            // Extract line [start..i)
            int lineLen = i - start;
            char lineStr[IRC_MAX_MSG];
            if (lineLen > IRC_MAX_MSG - 1) lineLen = IRC_MAX_MSG - 1;
            memcpy(lineStr, recvBuf.buf + start, lineLen);
            lineStr[lineLen] = '\0';
            irc_process_line(lineStr);
            start = i + 2;
            i++; // skip \n
        }
    }

    // Move unprocessed remainder to front
    if (start > 0) {
        int remaining = recvBuf.len - start;
        if (remaining > 0) {
            memmove(recvBuf.buf, recvBuf.buf + start, remaining);
        }
        recvBuf.len = remaining;
    }

    // Prevent overflow if no \r\n found in a full buffer
    if (recvBuf.len >= (int)sizeof(recvBuf.buf) - 1) {
        recvBuf.len = 0;
    }
}

// ---- UI rendering (buffered, single flush) ----

static void ui_render() {
    sb_reset();

    // Hide cursor during redraw
    sb_puts("\033[?25l");

    // Status bar (row 1)
    sb_cursor_to(1, 1);
    sb_puts("\033[7m\033[2K");
    char status[256];
    if (irc.connected) {
        if (irc.inChannel) {
            snprintf(status, sizeof(status), " IRC | %s | %s | Connected ", irc.nick, irc.channel);
        } else {
            snprintf(status, sizeof(status), " IRC | %s | (no channel) | Connected ", irc.nick);
        }
    } else {
        snprintf(status, sizeof(status), " IRC | %s | Disconnected ", irc.nick);
    }
    sb_puts(status);
    int statusLen = slen(status);
    for (int i = statusLen; i < term.cols; i++) sb_putc(' ');
    sb_puts("\033[0m");

    // Message area (rows 2..N-2)
    int startLine;
    if (msgBuf.count <= term.msgAreaRows) {
        startLine = 0;
    } else {
        startLine = msgBuf.count - term.msgAreaRows - msgBuf.scrollOffset;
        if (startLine < 0) startLine = 0;
    }
    for (int r = 0; r < term.msgAreaRows; r++) {
        sb_cursor_to(r + 2, 1);
        sb_puts("\033[2K");
        int msgIdx = startLine + r;
        if (msgIdx < msgBuf.count) {
            int realIdx = (msgBuf.head + msgIdx) % MAX_DISPLAY_LINES;
            sb_puts(msgBuf.lines[realIdx].text);
        }
    }

    // Separator (row N-1)
    sb_cursor_to(term.rows - 1, 1);
    sb_puts("\033[2K\033[90m");
    for (int i = 0; i < term.cols; i++) sb_putc('-');
    sb_puts("\033[0m");

    // Input line (row N)
    sb_cursor_to(term.rows, 1);
    sb_puts("\033[2K\033[1m>\033[0m ");
    for (int i = 0; i < input.len; i++) sb_putc(input.buf[i]);

    // Position cursor at input insertion point
    sb_cursor_to(term.rows, input.pos + 3);

    // Single flush — entire screen in one print call
    sb_flush();
}

// ---- User command processing ----

static void handle_user_input() {
    input.buf[input.len] = '\0';
    const char* text = input.buf;

    if (text[0] != '/') {
        // Plain message to current channel
        if (!irc.inChannel) {
            msg_add("\033[31m*** Not in a channel. Use /join #channel\033[0m");
            return;
        }
        irc_send("PRIVMSG %s :%s", irc.channel, text);
        // Echo own message
        msg_add_fmt("\033[1;32m<%s>\033[0m %s", irc.nick, text);
        return;
    }

    // Parse command
    const char* cmd = text + 1;

    if (starts_with(cmd, "join ") || starts_with(cmd, "JOIN ")) {
        const char* chan = skip_spaces(cmd + 5);
        if (*chan == '\0') {
            msg_add("\033[31m*** Usage: /join #channel\033[0m");
            return;
        }
        strncpy(irc.channel, chan, sizeof(irc.channel));
        irc_send("JOIN %s", chan);
    }
    else if (starts_with(cmd, "part") && (cmd[4] == '\0' || cmd[4] == ' ')) {
        if (!irc.inChannel) {
            msg_add("\033[31m*** Not in a channel\033[0m");
            return;
        }
        const char* reason = cmd[4] ? skip_spaces(cmd + 5) : "";
        if (*reason) {
            irc_send("PART %s :%s", irc.channel, reason);
        } else {
            irc_send("PART %s", irc.channel);
        }
    }
    else if (starts_with(cmd, "msg ") || starts_with(cmd, "MSG ")) {
        const char* rest = skip_spaces(cmd + 4);
        char target[MAX_NICK_LEN];
        int i = 0;
        while (rest[i] && rest[i] != ' ' && i < MAX_NICK_LEN - 1) {
            target[i] = rest[i]; i++;
        }
        target[i] = '\0';
        const char* msg = skip_spaces(rest + i);
        if (!target[0] || !*msg) {
            msg_add("\033[31m*** Usage: /msg nick message\033[0m");
            return;
        }
        irc_send("PRIVMSG %s :%s", target, msg);
        msg_add_fmt("\033[1;35m-> %s:\033[0m %s", target, msg);
    }
    else if (starts_with(cmd, "nick ") || starts_with(cmd, "NICK ")) {
        const char* newNick = skip_spaces(cmd + 5);
        if (!*newNick) {
            msg_add("\033[31m*** Usage: /nick newnick\033[0m");
            return;
        }
        irc_send("NICK %s", newNick);
        strncpy(irc.nick, newNick, sizeof(irc.nick));
    }
    else if (starts_with(cmd, "quit") && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char* reason = cmd[4] ? skip_spaces(cmd + 5) : "";
        if (*reason) {
            irc_send("QUIT :%s", reason);
        } else {
            irc_send("QUIT :Leaving");
        }
        irc.connected = false;
        running = false;
    }
    else if (starts_with(cmd, "me ") || starts_with(cmd, "ME ")) {
        if (!irc.inChannel) {
            msg_add("\033[31m*** Not in a channel\033[0m");
            return;
        }
        const char* action = skip_spaces(cmd + 3);
        irc_send("PRIVMSG %s :\001ACTION %s\001", irc.channel, action);
        msg_add_fmt("\033[35m* %s %s\033[0m", irc.nick, action);
    }
    else if (starts_with(cmd, "raw ") || starts_with(cmd, "RAW ")) {
        const char* raw = cmd + 4;
        irc_send("%s", raw);
        msg_add_fmt("\033[90m>> %s\033[0m", raw);
    }
    else if (streqi(cmd, "help") || starts_with(cmd, "help ")) {
        msg_add("\033[1m--- Help ---\033[0m");
        msg_add("  /join #channel  - Join a channel");
        msg_add("  /part [reason]  - Leave current channel");
        msg_add("  /msg nick text  - Send private message");
        msg_add("  /nick newnick   - Change nickname");
        msg_add("  /quit [reason]  - Disconnect and exit");
        msg_add("  /me action      - Send action");
        msg_add("  /raw text       - Send raw IRC line");
        msg_add("  /clear          - Clear message area");
        msg_add("  /help           - Show this help");
        msg_add("  Ctrl+Q          - Quit");
        msg_add("  PgUp/PgDn       - Scroll messages");
    }
    else if (streqi(cmd, "clear")) {
        msg_clear();
    }
    else {
        char cmdWord[32];
        int i = 0;
        while (cmd[i] && cmd[i] != ' ' && i < 31) { cmdWord[i] = cmd[i]; i++; }
        cmdWord[i] = '\0';
        msg_add_fmt("\033[31m*** Unknown command: /%s (try /help)\033[0m", cmdWord);
    }
}

// ---- Entry point ----

extern "C" void _start() {
    // Parse arguments: <server_ip> <port> <nickname> [#channel]
    char argbuf[256];
    zenith::getargs(argbuf, sizeof(argbuf));
    const char* arg = skip_spaces(argbuf);

    if (*arg == '\0') {
        zenith::print("Usage: irc.elf <server_ip> <port> <nickname> [#channel]\n");
        zenith::print("Example: run irc.elf 10.0.68.1 6667 ZenithUser #general\n");
        return;
    }

    // Parse IP
    char ipStr[32];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 31) { ipStr[i] = arg[i]; i++; }
    ipStr[i] = '\0';
    arg = skip_spaces(arg + i);

    if (!parse_ip(ipStr, &irc.serverIp)) {
        zenith::print("Invalid IP address: ");
        zenith::print(ipStr);
        zenith::putchar('\n');
        return;
    }

    // Parse port
    char portStr[16];
    i = 0;
    while (arg[i] && arg[i] != ' ' && i < 15) { portStr[i] = arg[i]; i++; }
    portStr[i] = '\0';
    arg = skip_spaces(arg + i);

    if (!parse_uint16(portStr, &irc.serverPort)) {
        zenith::print("Invalid port: ");
        zenith::print(portStr);
        zenith::putchar('\n');
        return;
    }

    // Parse nickname
    i = 0;
    while (arg[i] && arg[i] != ' ' && i < MAX_NICK_LEN - 1) { irc.nick[i] = arg[i]; i++; }
    irc.nick[i] = '\0';
    arg = skip_spaces(arg + i);

    if (!irc.nick[0]) {
        zenith::print("Missing nickname\n");
        return;
    }

    // Parse optional channel
    i = 0;
    while (arg[i] && arg[i] != ' ' && i < MAX_CHANNEL_LEN - 1) { irc.channel[i] = arg[i]; i++; }
    irc.channel[i] = '\0';

    // Initialize state
    irc.fd = -1;
    irc.registered = false;
    irc.connected = false;
    irc.inChannel = false;
    irc.nickRetries = 0;
    recvBuf.len = 0;
    msgBuf.head = 0;
    msgBuf.count = 0;
    msgBuf.scrollOffset = 0;
    input.pos = 0;
    input.len = 0;
    running = true;

    // Get terminal size
    zenith::termsize(&term.cols, &term.rows);
    term.msgAreaRows = term.rows - 3;
    if (term.msgAreaRows < 1) term.msgAreaRows = 1;

    // Enter alternate screen buffer, hide cursor
    zenith::print("\033[?1049h");
    zenith::print("\033[?25l");

    // Initial draw
    msg_add("\033[1m*** ZenithOS IRC Client\033[0m");
    msg_add_fmt("*** Connecting to %s:%d as %s...", ipStr, (int)irc.serverPort, irc.nick);
    ui_render();

    // Create socket and connect
    irc.fd = zenith::socket(Zenith::SOCK_TCP);
    if (irc.fd < 0) {
        msg_add("\033[31m*** Failed to create socket\033[0m");
        ui_render();
        // Wait for keypress then exit
        while (!zenith::is_key_available()) zenith::yield();
        goto cleanup;
    }

    if (zenith::connect(irc.fd, irc.serverIp, irc.serverPort) < 0) {
        msg_add("\033[31m*** Connection failed\033[0m");
        ui_render();
        zenith::closesocket(irc.fd);
        irc.fd = -1;
        while (!zenith::is_key_available()) zenith::yield();
        goto cleanup;
    }

    irc.connected = true;
    msg_add("\033[32m*** Connected!\033[0m");

    // Send IRC registration
    irc_send("NICK %s", irc.nick);
    irc_send("USER %s 0 * :%s", irc.nick, irc.nick);

    ui_render();

    // ---- Main loop ----
    while (running && irc.connected) {
        dirty = false;

        // Process incoming data
        int prevCount = msgBuf.count;
        recv_process();
        if (msgBuf.count != prevCount) dirty = true;

        // Poll keyboard
        if (zenith::is_key_available()) {
            Zenith::KeyEvent ev;
            zenith::getkey(&ev);

            if (!ev.pressed) {
                if (dirty) ui_render();
                continue;
            }

            dirty = true;

            // Ctrl+Q to quit
            if (ev.ctrl && (ev.ascii == 'q' || ev.ascii == 'Q')) {
                if (irc.connected) {
                    irc_send("QUIT :Leaving");
                }
                running = false;
                continue;
            }

            // Handle special keys by scancode
            switch (ev.scancode) {
                case 0x49: // Page Up
                    if (msgBuf.scrollOffset < msgBuf.count - term.msgAreaRows) {
                        msgBuf.scrollOffset += term.msgAreaRows / 2;
                        int maxScroll = msgBuf.count - term.msgAreaRows;
                        if (maxScroll < 0) maxScroll = 0;
                        if (msgBuf.scrollOffset > maxScroll) msgBuf.scrollOffset = maxScroll;
                    }
                    break;
                case 0x51: // Page Down
                    msgBuf.scrollOffset -= term.msgAreaRows / 2;
                    if (msgBuf.scrollOffset < 0) msgBuf.scrollOffset = 0;
                    break;
                case 0x47: // Home (scroll to top)
                    if (msgBuf.count > term.msgAreaRows) {
                        msgBuf.scrollOffset = msgBuf.count - term.msgAreaRows;
                    }
                    break;
                case 0x4F: // End (scroll to bottom)
                    msgBuf.scrollOffset = 0;
                    break;
                default:
                    if (ev.ascii == '\n') {
                        if (input.len > 0) {
                            input.buf[input.len] = '\0';
                            handle_user_input();
                            input.pos = 0;
                            input.len = 0;
                        }
                    } else if (ev.ascii == '\b') {
                        if (input.pos > 0) {
                            for (int j = input.pos - 1; j < input.len - 1; j++) {
                                input.buf[j] = input.buf[j + 1];
                            }
                            input.pos--;
                            input.len--;
                        }
                    } else if (ev.ascii >= ' ' && ev.ascii <= '~') {
                        if (input.len < 510) {
                            for (int j = input.len; j > input.pos; j--) {
                                input.buf[j] = input.buf[j - 1];
                            }
                            input.buf[input.pos] = ev.ascii;
                            input.pos++;
                            input.len++;
                        }
                    } else {
                        dirty = false; // Unhandled key, no redraw needed
                    }
                    break;
            }
        } else {
            if (!dirty) {
                zenith::yield();
                continue;
            }
        }

        if (dirty) ui_render();
    }

    if (irc.fd >= 0) {
        zenith::closesocket(irc.fd);
    }

cleanup:
    // Restore terminal
    zenith::print("\033[?25h");
    zenith::print("\033[?1049l");
}
