/*
    * main.cpp
    * edit - Text editor for MontaukOS
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/heap.h>
#include <montauk/string.h>

using montauk::slen;
using montauk::memcpy;
using montauk::memmove;

// ---- Integer to string ----

static int itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[12];
    int i = 0;
    bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    while (val > 0) { tmp[i++] = '0' + val % 10; val /= 10; }
    int len = 0;
    if (neg) buf[len++] = '-';
    for (int j = i - 1; j >= 0; j--) buf[len++] = tmp[j];
    buf[len] = '\0';
    return len;
}

// ---- Terminal output helpers ----

static void print(const char* s) { montauk::print(s); }
static void putch(char c) { montauk::putchar(c); }

static void print_int(int v) {
    char buf[12];
    itoa(v, buf);
    print(buf);
}

// ANSI escape helpers
static void esc(const char* seq) {
    putch('\033');
    putch('[');
    print(seq);
}

static void cursor_to(int row, int col) {
    putch('\033'); putch('[');
    print_int(row); putch(';');
    print_int(col); putch('H');
}

static void clear_line() { esc("2K"); }
static void hide_cursor() { esc("?25l"); }
static void show_cursor() { esc("?25h"); }
static void enter_alt_screen() { esc("?1049h"); }
static void exit_alt_screen() { esc("?1049l"); }
static void reset_attrs() { esc("0m"); }
static void reverse_video() { esc("7m"); }
static void dim_text() { esc("2m"); }

// ---- Line buffer ----

struct Line {
    char* data;
    int len;
    int cap;
};

static constexpr int MAX_LINES = 10000;
static constexpr int INITIAL_LINE_CAP = 64;
static constexpr int TAB_WIDTH = 4;

static Line* lines = nullptr;
static int numLines = 0;

// Editor state
static int cursorRow = 0;     // cursor position in document (0-indexed)
static int cursorCol = 0;
static int topLine = 0;       // first visible line
static int leftCol = 0;       // horizontal scroll offset
static int screenRows = 24;
static int screenCols = 80;
static int editorRows = 0;    // screenRows - 2 (status + hint bars)
static int gutterWidth = 4;   // line number gutter width

static bool modified = false;
static bool running = true;
static bool fullRedraw = true;

static char filename[256] = "";
static bool hasFilename = false;

// Search state
static char searchQuery[128] = "";
static int searchLen = 0;

// Status message
static char statusMsg[128] = "";
static uint64_t statusMsgTime = 0;

// ---- Line operations ----

static void line_init(Line* ln) {
    ln->cap = INITIAL_LINE_CAP;
    ln->data = (char*)montauk::malloc(ln->cap);
    ln->len = 0;
    ln->data[0] = '\0';
}

static void line_ensure(Line* ln, int needed) {
    if (needed + 1 <= ln->cap) return;
    int newCap = ln->cap;
    while (newCap < needed + 1) newCap *= 2;
    ln->data = (char*)montauk::realloc(ln->data, newCap);
    ln->cap = newCap;
}

static void line_insert_char(Line* ln, int pos, char c) {
    if (pos < 0) pos = 0;
    if (pos > ln->len) pos = ln->len;
    line_ensure(ln, ln->len + 1);
    memmove(ln->data + pos + 1, ln->data + pos, ln->len - pos);
    ln->data[pos] = c;
    ln->len++;
    ln->data[ln->len] = '\0';
}

static void line_delete_char(Line* ln, int pos) {
    if (pos < 0 || pos >= ln->len) return;
    memmove(ln->data + pos, ln->data + pos + 1, ln->len - pos - 1);
    ln->len--;
    ln->data[ln->len] = '\0';
}

static void line_append(Line* ln, const char* s, int slen) {
    line_ensure(ln, ln->len + slen);
    memcpy(ln->data + ln->len, s, slen);
    ln->len += slen;
    ln->data[ln->len] = '\0';
}

// ---- Document operations ----

static void insert_line(int at) {
    if (numLines >= MAX_LINES) return;
    if (at < 0) at = 0;
    if (at > numLines) at = numLines;

    // Shift lines down
    for (int i = numLines; i > at; i--) {
        lines[i] = lines[i - 1];
    }
    line_init(&lines[at]);
    numLines++;
}

static void delete_line(int at) {
    if (at < 0 || at >= numLines) return;
    if (numLines <= 1) {
        // Don't delete the last line, just clear it
        lines[at].len = 0;
        lines[at].data[0] = '\0';
        return;
    }
    montauk::mfree(lines[at].data);
    for (int i = at; i < numLines - 1; i++) {
        lines[i] = lines[i + 1];
    }
    numLines--;
}

// ---- Compute gutter width from line count ----

static void update_gutter_width() {
    int digits = 1;
    int n = numLines;
    while (n >= 10) { digits++; n /= 10; }
    gutterWidth = digits + 2; // digits + space + separator
    if (gutterWidth < 4) gutterWidth = 4;
}

// ---- File I/O ----

static void set_status(const char* msg) {
    int i = 0;
    while (msg[i] && i < 126) { statusMsg[i] = msg[i]; i++; }
    statusMsg[i] = '\0';
    statusMsgTime = montauk::get_milliseconds();
}

// Build VFS path from filename
static void build_path(const char* fname, char* out, int outMax) {
    int i = 0;
    // Check if already has drive prefix
    bool hasPrefix = (fname[0] >= '0' && fname[0] <= '9' && fname[1] == ':');
    if (!hasPrefix) {
        out[i++] = '0'; out[i++] = ':'; out[i++] = '/';
    }
    int j = 0;
    while (fname[j] && i < outMax - 1) {
        out[i++] = fname[j++];
    }
    out[i] = '\0';
}

static void load_file(const char* fname) {
    char path[256];
    build_path(fname, path, sizeof(path));

    int handle = montauk::open(path);
    if (handle < 0) {
        // New file
        numLines = 1;
        line_init(&lines[0]);
        set_status("(New file)");
        return;
    }

    uint64_t size = montauk::getsize(handle);

    // Read entire file into a temp buffer
    uint8_t* buf = nullptr;
    if (size > 0) {
        buf = (uint8_t*)montauk::malloc(size + 1);
        uint64_t off = 0;
        while (off < size) {
            int r = montauk::read(handle, buf + off, off, size - off);
            if (r <= 0) break;
            off += r;
        }
        buf[size] = '\0';
    }
    montauk::close(handle);

    // Parse into lines
    numLines = 0;
    if (buf && size > 0) {
        uint64_t lineStart = 0;
        for (uint64_t i = 0; i <= size; i++) {
            if (i == size || buf[i] == '\n') {
                if (numLines >= MAX_LINES) break;
                line_init(&lines[numLines]);
                int lineLen = (int)(i - lineStart);
                if (lineLen > 0) {
                    line_append(&lines[numLines], (const char*)buf + lineStart, lineLen);
                }
                numLines++;
                lineStart = i + 1;
            }
        }
    }

    if (buf) montauk::mfree(buf);

    if (numLines == 0) {
        numLines = 1;
        line_init(&lines[0]);
    }

    set_status("File loaded");
}

static bool save_file() {
    if (!hasFilename) {
        set_status("No filename! Use Ctrl+S after setting a name");
        return false;
    }

    char path[256];
    build_path(filename, path, sizeof(path));

    // Calculate total size
    uint64_t totalSize = 0;
    for (int i = 0; i < numLines; i++) {
        totalSize += lines[i].len;
        if (i < numLines - 1) totalSize++; // newline
    }

    // Try to open existing file first
    int handle = montauk::open(path);
    if (handle < 0) {
        // Create new file
        handle = montauk::fcreate(path);
        if (handle < 0) {
            set_status("Error: could not create file");
            return false;
        }
    }

    // Build content buffer and write
    uint8_t* buf = (uint8_t*)montauk::malloc(totalSize + 1);
    uint64_t off = 0;
    for (int i = 0; i < numLines; i++) {
        if (lines[i].len > 0) {
            memcpy(buf + off, lines[i].data, lines[i].len);
            off += lines[i].len;
        }
        if (i < numLines - 1) {
            buf[off++] = '\n';
        }
    }

    int result = montauk::fwrite(handle, buf, 0, totalSize);
    montauk::close(handle);
    montauk::mfree(buf);

    if (result < 0) {
        set_status("Error: write failed");
        return false;
    }

    modified = false;
    set_status("File saved");
    return true;
}

// ---- Prompt for input in the hint bar area ----

static int prompt_input(const char* promptStr, char* out, int outMax) {
    // Draw prompt on the last row
    cursor_to(screenRows, 1);
    reverse_video();
    clear_line();
    print(promptStr);
    reset_attrs();
    show_cursor();

    int pos = 0;
    out[0] = '\0';

    while (true) {
        if (!montauk::is_key_available()) {
            montauk::yield();
            continue;
        }

        Montauk::KeyEvent ev;
        montauk::getkey(&ev);
        if (!ev.pressed) continue;

        if (ev.ascii == '\033' || (ev.ctrl && ev.ascii == 'q')) {
            // Cancel
            return -1;
        }

        if (ev.ascii == '\n') {
            out[pos] = '\0';
            return pos;
        }

        if (ev.ascii == '\b') {
            if (pos > 0) {
                pos--;
                putch('\b'); putch(' '); putch('\b');
            }
        } else if (ev.ascii >= ' ' && pos < outMax - 1) {
            out[pos++] = ev.ascii;
            out[pos] = '\0';
            putch(ev.ascii);
        }
    }
}

// ---- Rendering ----

static void draw_status_bar() {
    cursor_to(1, 1);
    reverse_video();
    clear_line();

    // Left side: "edit: filename [+]"
    print("  edit: ");
    if (hasFilename) {
        print(filename);
    } else {
        print("[No Name]");
    }
    if (modified) print(" [+]");

    // Right side: cursor position
    // Calculate right-side text
    char posBuf[32];
    int p = 0;
    posBuf[p++] = 'L';
    posBuf[p++] = 'n';
    posBuf[p++] = ' ';
    p += itoa(cursorRow + 1, posBuf + p);
    posBuf[p++] = ',';
    posBuf[p++] = ' ';
    posBuf[p++] = 'C';
    posBuf[p++] = 'o';
    posBuf[p++] = 'l';
    posBuf[p++] = ' ';
    p += itoa(cursorCol + 1, posBuf + p);
    posBuf[p++] = ' ';
    posBuf[p++] = ' ';
    posBuf[p] = '\0';

    // Pad to right-align
    // We need to figure out how many chars we've printed on the left
    int leftLen = 8 + (hasFilename ? slen(filename) : 9) + (modified ? 4 : 0);
    int rightLen = p;
    int padding = screenCols - leftLen - rightLen;
    for (int i = 0; i < padding; i++) putch(' ');
    print(posBuf);

    reset_attrs();
}

static void draw_hint_bar() {
    cursor_to(screenRows, 1);
    reverse_video();
    clear_line();

    // Show status message if recent (within 3 seconds)
    uint64_t now = montauk::get_milliseconds();
    if (statusMsg[0] && (now - statusMsgTime) < 3000) {
        print("  ");
        print(statusMsg);
    } else {
        print("  ^S Save  ^Q Quit  ^F Find  ^G Find Next");
    }

    // Pad rest of line
    reset_attrs();
}

static void draw_line(int screenRow, int docLine) {
    cursor_to(screenRow + 2, 1); // +2 because row 1 is status bar
    clear_line();

    if (docLine < numLines) {
        // Line number gutter
        dim_text();
        char numBuf[12];
        int numLen = itoa(docLine + 1, numBuf);
        // Right-align the number in the gutter
        int pad = gutterWidth - 2 - numLen; // -2 for trailing space+pipe
        for (int i = 0; i < pad; i++) putch(' ');
        print(numBuf);
        putch(' ');
        reset_attrs();

        // Line content
        Line* ln = &lines[docLine];
        int startCol = leftCol;
        int maxChars = screenCols - gutterWidth;

        for (int c = 0; c < maxChars && startCol + c < ln->len; c++) {
            putch(ln->data[startCol + c]);
        }
    } else {
        // Past end of file
        dim_text();
        putch('~');
        reset_attrs();
    }
}

static void render() {
    hide_cursor();

    update_gutter_width();
    draw_status_bar();

    if (fullRedraw) {
        for (int i = 0; i < editorRows; i++) {
            draw_line(i, topLine + i);
        }
        fullRedraw = false;
    }

    draw_hint_bar();

    // Position cursor
    int screenY = cursorRow - topLine + 2; // +2 for status bar
    int screenX = cursorCol - leftCol + gutterWidth;
    cursor_to(screenY, screenX);

    show_cursor();
}

// ---- Scrolling ----

static void scroll() {
    // Vertical scrolling
    if (cursorRow < topLine) {
        topLine = cursorRow;
        fullRedraw = true;
    }
    if (cursorRow >= topLine + editorRows) {
        topLine = cursorRow - editorRows + 1;
        fullRedraw = true;
    }

    // Horizontal scrolling
    int textCols = screenCols - gutterWidth;
    if (cursorCol < leftCol) {
        leftCol = cursorCol;
        fullRedraw = true;
    }
    if (cursorCol >= leftCol + textCols) {
        leftCol = cursorCol - textCols + 1;
        fullRedraw = true;
    }
}

// ---- Editing operations ----

static void insert_char(char c) {
    line_insert_char(&lines[cursorRow], cursorCol, c);
    cursorCol++;
    modified = true;
    fullRedraw = true;
}

static void insert_tab() {
    for (int i = 0; i < TAB_WIDTH; i++) {
        insert_char(' ');
    }
}

static void insert_newline() {
    Line* current = &lines[cursorRow];

    // Split current line at cursor position
    insert_line(cursorRow + 1);
    // Re-get pointer since insert_line shifted memory
    current = &lines[cursorRow];
    Line* newLine = &lines[cursorRow + 1];

    // Move text after cursor to new line
    if (cursorCol < current->len) {
        line_append(newLine, current->data + cursorCol, current->len - cursorCol);
        current->len = cursorCol;
        current->data[current->len] = '\0';
    }

    cursorRow++;
    cursorCol = 0;
    modified = true;
    fullRedraw = true;
}

static void delete_char_backspace() {
    if (cursorCol > 0) {
        line_delete_char(&lines[cursorRow], cursorCol - 1);
        cursorCol--;
        modified = true;
        fullRedraw = true;
    } else if (cursorRow > 0) {
        // Join with previous line
        int prevLen = lines[cursorRow - 1].len;
        line_append(&lines[cursorRow - 1], lines[cursorRow].data, lines[cursorRow].len);
        delete_line(cursorRow);
        cursorRow--;
        cursorCol = prevLen;
        modified = true;
        fullRedraw = true;
    }
}

static void delete_char_forward() {
    if (cursorCol < lines[cursorRow].len) {
        line_delete_char(&lines[cursorRow], cursorCol);
        modified = true;
        fullRedraw = true;
    } else if (cursorRow < numLines - 1) {
        // Join with next line
        line_append(&lines[cursorRow], lines[cursorRow + 1].data, lines[cursorRow + 1].len);
        delete_line(cursorRow + 1);
        modified = true;
        fullRedraw = true;
    }
}

// ---- Search ----

static void find_next(bool fromPrompt) {
    if (searchLen == 0) return;

    // Start search from cursor position + 1
    int startRow = cursorRow;
    int startCol = cursorCol + (fromPrompt ? 0 : 1);

    for (int i = 0; i < numLines; i++) {
        int row = (startRow + i) % numLines;
        int colStart = (i == 0) ? startCol : 0;

        Line* ln = &lines[row];
        for (int c = colStart; c <= ln->len - searchLen; c++) {
            bool match = true;
            for (int k = 0; k < searchLen; k++) {
                if (ln->data[c + k] != searchQuery[k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                cursorRow = row;
                cursorCol = c;
                fullRedraw = true;
                set_status("Found");
                return;
            }
        }
    }

    set_status("Not found");
}

static void do_search() {
    char query[128];
    int len = prompt_input("Search: ", query, sizeof(query));
    if (len < 0) {
        fullRedraw = true;
        return;
    }
    if (len == 0) {
        fullRedraw = true;
        return;
    }

    // Copy query
    for (int i = 0; i < len; i++) searchQuery[i] = query[i];
    searchQuery[len] = '\0';
    searchLen = len;

    find_next(true);
    fullRedraw = true;
}

// ---- Navigation scancodes ----

static constexpr uint8_t SC_UP     = 0x48;
static constexpr uint8_t SC_DOWN   = 0x50;
static constexpr uint8_t SC_LEFT   = 0x4B;
static constexpr uint8_t SC_RIGHT  = 0x4D;
static constexpr uint8_t SC_HOME   = 0x47;
static constexpr uint8_t SC_END    = 0x4F;
static constexpr uint8_t SC_PGUP   = 0x49;
static constexpr uint8_t SC_PGDN   = 0x51;
static constexpr uint8_t SC_DELETE = 0x53;

// ---- Input handling ----

static void handle_key(const Montauk::KeyEvent& ev) {
    if (!ev.pressed) return;

    // Ctrl key combinations
    if (ev.ctrl) {
        switch (ev.ascii) {
            case 'q':
                if (modified) {
                    set_status("Unsaved changes! Press Ctrl+Q again to quit");
                    // Set a flag so next Ctrl+Q quits
                    static bool warnedOnce = false;
                    if (warnedOnce) {
                        running = false;
                    }
                    warnedOnce = true;
                    return;
                }
                running = false;
                return;
            case 's':
                if (!hasFilename) {
                    char nameBuf[256];
                    int len = prompt_input("Save as: ", nameBuf, sizeof(nameBuf));
                    if (len > 0) {
                        for (int i = 0; i <= len; i++) filename[i] = nameBuf[i];
                        hasFilename = true;
                    }
                    fullRedraw = true;
                    if (!hasFilename) return;
                }
                save_file();
                fullRedraw = true;
                return;
            case 'f':
                do_search();
                return;
            case 'g':
                find_next(false);
                return;
            default:
                break;
        }
    }

    // Non-ASCII keys (scancode-based)
    if (ev.ascii == 0) {
        switch (ev.scancode) {
            case SC_UP:
                if (cursorRow > 0) {
                    cursorRow--;
                    if (cursorCol > lines[cursorRow].len)
                        cursorCol = lines[cursorRow].len;
                    fullRedraw = true;
                }
                return;
            case SC_DOWN:
                if (cursorRow < numLines - 1) {
                    cursorRow++;
                    if (cursorCol > lines[cursorRow].len)
                        cursorCol = lines[cursorRow].len;
                    fullRedraw = true;
                }
                return;
            case SC_LEFT:
                if (cursorCol > 0) {
                    cursorCol--;
                } else if (cursorRow > 0) {
                    cursorRow--;
                    cursorCol = lines[cursorRow].len;
                }
                fullRedraw = true;
                return;
            case SC_RIGHT:
                if (cursorCol < lines[cursorRow].len) {
                    cursorCol++;
                } else if (cursorRow < numLines - 1) {
                    cursorRow++;
                    cursorCol = 0;
                }
                fullRedraw = true;
                return;
            case SC_HOME:
                cursorCol = 0;
                fullRedraw = true;
                return;
            case SC_END:
                cursorCol = lines[cursorRow].len;
                fullRedraw = true;
                return;
            case SC_PGUP:
                cursorRow -= editorRows;
                if (cursorRow < 0) cursorRow = 0;
                if (cursorCol > lines[cursorRow].len)
                    cursorCol = lines[cursorRow].len;
                fullRedraw = true;
                return;
            case SC_PGDN:
                cursorRow += editorRows;
                if (cursorRow >= numLines) cursorRow = numLines - 1;
                if (cursorCol > lines[cursorRow].len)
                    cursorCol = lines[cursorRow].len;
                fullRedraw = true;
                return;
            case SC_DELETE:
                delete_char_forward();
                return;
            default:
                return;
        }
    }

    // Regular keys
    switch (ev.ascii) {
        case '\n':
            insert_newline();
            break;
        case '\b':
            delete_char_backspace();
            break;
        case '\t':
            insert_tab();
            break;
        default:
            if (ev.ascii >= ' ') {
                insert_char(ev.ascii);
            }
            break;
    }
}

// ---- Entry point ----

extern "C" void _start() {
    // Allocate line buffer
    lines = (Line*)montauk::malloc(sizeof(Line) * MAX_LINES);

    // Get terminal size
    montauk::termsize(&screenCols, &screenRows);
    editorRows = screenRows - 2;

    // Parse arguments
    char args[256];
    int argLen = montauk::getargs(args, sizeof(args));

    if (argLen > 0 && args[0] != '\0') {
        // Copy filename
        int i = 0;
        while (args[i] && i < 255) { filename[i] = args[i]; i++; }
        filename[i] = '\0';
        hasFilename = true;
        load_file(filename);
    } else {
        // New empty buffer
        numLines = 1;
        line_init(&lines[0]);
    }

    // Enter alternate screen
    enter_alt_screen();
    fullRedraw = true;

    // Main loop
    while (running) {
        scroll();
        render();

        // Wait for input
        while (!montauk::is_key_available()) {
            montauk::yield();
        }

        Montauk::KeyEvent ev;
        montauk::getkey(&ev);
        handle_key(ev);
    }

    // Exit alternate screen
    exit_alt_screen();
    show_cursor();
    reset_attrs();

    montauk::exit(0);
}
