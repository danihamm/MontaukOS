/*
    * main.cpp
    * Manual page viewer for MontaukOS
    * Fullscreen pager with ANSI formatting and keyboard navigation
    * Copyright (c) 2025 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/heap.h>
#include <montauk/string.h>

using montauk::slen;
using montauk::starts_with;
using montauk::skip_spaces;

static void print_int(uint64_t n) {
    if (n == 0) {
        montauk::putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        montauk::putchar(buf[j]);
    }
}

// ---- Pager rendering ----

static constexpr int MAN_MAX_LINES = 2048;

static int int_to_buf(char* buf, int n) {
    if (n == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int i = 0;
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

static void cursor_to(int row, int col) {
    char seq[24] = "\033[";
    int p = 2;
    p += int_to_buf(seq + p, row);
    seq[p++] = ';';
    p += int_to_buf(seq + p, col);
    seq[p++] = 'H';
    seq[p] = '\0';
    montauk::print(seq);
}

struct ManLine {
    const char* text;
    int         len;
    bool        isSH;
    bool        isSS;
    bool        isBold;
    bool        isTH;
};

static void man_render(ManLine* lines, int totalLines, int scroll, int rows, int cols,
                       const char* name, int section) {
    int contentRows = rows - 1;

    for (int r = 0; r < contentRows; r++) {
        cursor_to(r + 1, 1);
        montauk::print("\033[2K");

        int idx = scroll + r;
        if (idx < 0 || idx >= totalLines) continue;

        ManLine& ln = lines[idx];
        if (ln.isTH) continue;

        if (ln.isSH || ln.isSS || ln.isBold) {
            montauk::print("\033[1m");
        }

        if (ln.isSS) {
            montauk::print("   ");
        }

        int maxW = cols;
        if (ln.isSS) maxW -= 3;
        int printLen = ln.len;
        if (printLen > maxW) printLen = maxW;
        for (int c = 0; c < printLen; c++) {
            montauk::putchar(ln.text[c]);
        }

        if (ln.isSH || ln.isSS || ln.isBold) {
            montauk::print("\033[0m");
        }
    }

    // Status bar
    cursor_to(rows, 1);
    montauk::print("\033[7m");
    montauk::print(" Manual page ");
    montauk::print(name);
    montauk::putchar('(');
    print_int((uint64_t)section);
    montauk::putchar(')');
    montauk::print(" line ");
    print_int((uint64_t)(scroll + 1));
    montauk::putchar('/');
    print_int((uint64_t)totalLines);

    int padCount = cols - 30 - slen(name);
    for (int i = 0; i < padCount; i++) montauk::putchar(' ');

    montauk::print("\033[0m");
}

// ---- Main ----

extern "C" void _start() {
    // Get arguments passed by the shell (e.g. "intro" or "2 syscalls")
    char argbuf[256];
    montauk::getargs(argbuf, sizeof(argbuf));

    const char* arg = skip_spaces(argbuf);
    if (*arg == '\0') {
        montauk::print("Usage: man <topic>\n");
        montauk::print("       man <section> <topic>\n");
        montauk::print("Try: man intro\n");
        return;
    }

    // Parse optional section number and topic name
    int section = 0;
    const char* topic = arg;

    if (arg[0] >= '1' && arg[0] <= '9' && arg[1] == ' ') {
        section = arg[0] - '0';
        topic = skip_spaces(arg + 2);
    }

    // Try to open man page file
    int handle = -1;
    int foundSection = 0;
    char path[128];

    if (section > 0) {
        const char* prefix = "0:/man/";
        int p = 0;
        while (prefix[p]) { path[p] = prefix[p]; p++; }
        int t = 0;
        while (topic[t] && p < 120) { path[p++] = topic[t++]; }
        path[p++] = '.';
        path[p++] = '0' + section;
        path[p] = '\0';

        handle = montauk::open(path);
        if (handle >= 0) foundSection = section;
    } else {
        for (int s = 1; s <= 9; s++) {
            const char* prefix = "0:/man/";
            int p = 0;
            while (prefix[p]) { path[p] = prefix[p]; p++; }
            int t = 0;
            while (topic[t] && p < 120) { path[p++] = topic[t++]; }
            path[p++] = '.';
            path[p++] = '0' + s;
            path[p] = '\0';

            handle = montauk::open(path);
            if (handle >= 0) {
                foundSection = s;
                break;
            }
        }
    }

    if (handle < 0) {
        montauk::print("No manual entry for ");
        montauk::print(topic);
        montauk::putchar('\n');
        return;
    }

    // Load entire file into heap
    uint64_t fileSize = montauk::getsize(handle);
    if (fileSize == 0) {
        montauk::close(handle);
        montauk::print("Empty manual page.\n");
        return;
    }

    char* fileData = (char*)montauk::malloc(fileSize + 1);
    if (fileData == nullptr) {
        montauk::close(handle);
        montauk::print("Out of memory.\n");
        return;
    }

    uint64_t offset = 0;
    while (offset < fileSize) {
        uint64_t chunk = fileSize - offset;
        if (chunk > 4096) chunk = 4096;
        int bytesRead = montauk::read(handle, (uint8_t*)fileData + offset, offset, chunk);
        if (bytesRead <= 0) break;
        offset += bytesRead;
    }
    fileData[offset] = '\0';
    montauk::close(handle);

    // Parse into lines
    ManLine* lines = (ManLine*)montauk::malloc(MAN_MAX_LINES * sizeof(ManLine));
    if (lines == nullptr) {
        montauk::mfree(fileData);
        montauk::print("Out of memory.\n");
        return;
    }

    int totalLines = 0;
    const char* p = fileData;
    while (*p && totalLines < MAN_MAX_LINES) {
        const char* lineStart = p;
        while (*p && *p != '\n') p++;
        int lineLen = (int)(p - lineStart);
        if (*p == '\n') p++;

        ManLine& ln = lines[totalLines];
        ln.isSH = false;
        ln.isSS = false;
        ln.isBold = false;
        ln.isTH = false;

        if (starts_with(lineStart, ".TH ")) {
            ln.isTH = true;
            ln.text = lineStart + 4;
            ln.len = lineLen - 4;
        } else if (starts_with(lineStart, ".SH ")) {
            ln.isSH = true;
            ln.text = lineStart + 4;
            ln.len = lineLen - 4;
        } else if (starts_with(lineStart, ".SS ")) {
            ln.isSS = true;
            ln.text = lineStart + 4;
            ln.len = lineLen - 4;
        } else if (starts_with(lineStart, ".B ")) {
            ln.isBold = true;
            ln.text = lineStart + 3;
            ln.len = lineLen - 3;
        } else if (starts_with(lineStart, ".BI ")) {
            ln.isBold = true;
            ln.text = lineStart + 4;
            ln.len = lineLen - 4;
        } else {
            ln.text = lineStart;
            ln.len = lineLen;
        }

        totalLines++;
    }

    if (totalLines == 0) {
        montauk::mfree(lines);
        montauk::mfree(fileData);
        montauk::print("Empty manual page.\n");
        return;
    }

    // Get terminal dimensions
    int cols = 80, rows = 25;
    montauk::termsize(&cols, &rows);

    // Enter alternate screen, hide cursor
    montauk::print("\033[?1049h");
    montauk::print("\033[?25l");

    int scroll = 0;
    int maxScroll = totalLines - (rows - 1);
    if (maxScroll < 0) maxScroll = 0;

    man_render(lines, totalLines, scroll, rows, cols, topic, foundSection);

    // Event loop — yield while waiting for key input
    bool running = true;
    while (running) {
        while (!montauk::is_key_available()) {
            montauk::yield();
        }

        Montauk::KeyEvent ev;
        montauk::getkey(&ev);
        if (!ev.pressed) continue;

        int contentRows = rows - 1;

        switch (ev.ascii) {
            case 'q':
                running = false;
                break;
            case 'j':
                if (scroll < maxScroll) scroll++;
                break;
            case 'k':
                if (scroll > 0) scroll--;
                break;
            case ' ':
                scroll += contentRows;
                if (scroll > maxScroll) scroll = maxScroll;
                break;
            case 'b':
                scroll -= contentRows;
                if (scroll < 0) scroll = 0;
                break;
            case 'g':
                scroll = 0;
                break;
            case 'G':
                scroll = maxScroll;
                break;
            default:
                // Handle scancodes for special keys
                switch (ev.scancode) {
                    case 0x48: // Up arrow
                        if (scroll > 0) scroll--;
                        break;
                    case 0x50: // Down arrow
                        if (scroll < maxScroll) scroll++;
                        break;
                    case 0x49: // Page Up
                        scroll -= contentRows;
                        if (scroll < 0) scroll = 0;
                        break;
                    case 0x51: // Page Down
                        scroll += contentRows;
                        if (scroll > maxScroll) scroll = maxScroll;
                        break;
                    case 0x47: // Home
                        scroll = 0;
                        break;
                    case 0x4F: // End
                        scroll = maxScroll;
                        break;
                }
                break;
        }

        if (running) {
            man_render(lines, totalLines, scroll, rows, cols, topic, foundSection);
        }
    }

    // Restore screen
    montauk::print("\033[?25h");
    montauk::print("\033[?1049l");

    montauk::mfree(lines);
    montauk::mfree(fileData);
}
