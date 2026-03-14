/*
    * main.cpp
    * Entry point, input loop, command dispatch, and chaining
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "shell.h"

// ---- Shared state definitions ----

char cwd[128] = "";
int current_drive = 0;
int last_exit = 0;
char session_user[32] = "";
char session_home[64] = "";

// ---- Session info (read once at startup) ----

void read_session() {
    auto doc = montauk::config::load("session");
    const char* name = doc.get_string("session.username", "");
    if (name[0]) {
        scopy(session_user, name, sizeof(session_user));
        scopy(session_home, "0:/users/", sizeof(session_home));
        scat(session_home, session_user, sizeof(session_home));
    }
    doc.destroy();
}

// ---- Command history ----

static constexpr int HISTORY_MAX = 32;
static char history[HISTORY_MAX][256];
static int history_count = 0;
static int history_next = 0;

static void history_add(const char* line) {
    if (line[0] == '\0') return;
    if (history_count > 0) {
        int prev = (history_next + HISTORY_MAX - 1) % HISTORY_MAX;
        if (streq(history[prev], line)) return;
    }
    scopy(history[history_next], line, 256);
    history_next = (history_next + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
}

static const char* history_get(int idx) {
    if (idx < 0 || idx >= history_count) return nullptr;
    int pos = (history_next + HISTORY_MAX - 1 - idx) % HISTORY_MAX;
    return history[pos];
}

// ---- Prompt ----

static void prompt() {
    char drv[4];
    if (current_drive >= 10) {
        drv[0] = '0' + current_drive / 10;
        drv[1] = '0' + current_drive % 10;
        drv[2] = '\0';
    } else {
        drv[0] = '0' + current_drive;
        drv[1] = '\0';
    }
    montauk::print(drv);
    montauk::print(":/");
    if (cwd[0]) montauk::print(cwd);
    montauk::print("> ");
}

// ---- Line editing ----

static void erase_input(int len) {
    for (int i = 0; i < len; i++) montauk::putchar('\b');
    for (int i = 0; i < len; i++) montauk::putchar(' ');
    for (int i = 0; i < len; i++) montauk::putchar('\b');
}

static void replace_line(char* line, int* pos, const char* newContent) {
    int oldLen = *pos;
    erase_input(oldLen);
    int newLen = slen(newContent);
    if (newLen > 255) newLen = 255;
    for (int i = 0; i < newLen; i++) {
        line[i] = newContent[i];
        montauk::putchar(newContent[i]);
    }
    line[newLen] = '\0';
    *pos = newLen;
}

// ---- Command dispatch (single command, already expanded) ----

static int process_command(const char* line) {
    line = skip_spaces(line);
    if (*line == '\0') return 0;

    // Variable assignment: NAME=VALUE
    {
        int eq = -1;
        bool valid = (line[0] >= 'A' && line[0] <= 'Z') ||
                     (line[0] >= 'a' && line[0] <= 'z') || line[0] == '_';
        if (valid) {
            for (int i = 1; line[i]; i++) {
                if (line[i] == '=') { eq = i; break; }
                if (!is_var_char(line[i])) break;
            }
        }
        if (eq > 0) {
            char name[32];
            int nlen = eq < 31 ? eq : 31;
            for (int i = 0; i < nlen; i++) name[i] = line[i];
            name[nlen] = '\0';
            const char* val = line + eq + 1;
            int vlen = slen(val);
            char vbuf[128];
            if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                              (val[0] == '\'' && val[vlen-1] == '\''))) {
                int j = 0;
                for (int i = 1; i < vlen - 1 && j < 127; i++) vbuf[j++] = val[i];
                vbuf[j] = '\0';
                var_set(name, vbuf);
            } else {
                var_set(name, val);
            }
            return 0;
        }
    }

    // Parse command name and arguments
    char cmd[128];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < 127) {
        cmd[i] = line[i];
        i++;
    }
    cmd[i] = '\0';

    const char* args = nullptr;
    if (line[i] == ' ') {
        args = skip_spaces(line + i);
        if (*args == '\0') args = nullptr;
    }

    // Bare drive switch: "1:", "2:", etc.
    if (has_drive_prefix(cmd) && cmd[drive_prefix_len(cmd)] == '\0' && args == nullptr) {
        int drive = parse_drive_prefix(cmd);
        if (!switch_drive(drive)) {
            montauk::print("No such drive: ");
            montauk::print(cmd);
            montauk::print("/\n");
            return 1;
        }
        return 0;
    }

    // Builtins
    if (streq(cmd, "help")) { cmd_help(); return 0; }
    if (streq(cmd, "ls")) { cmd_ls(args ? args : ""); return 0; }
    if (streq(cmd, "cd")) { return cmd_cd(args ? args : ""); }
    if (streq(cmd, "man")) { return cmd_man(args ? args : ""); }
    if (streq(cmd, "true")) { return 0; }
    if (streq(cmd, "false")) { return 1; }

    if (streq(cmd, "pwd")) {
        char path[128];
        build_dir_path(cwd, path, sizeof(path));
        montauk::print(path);
        montauk::putchar('\n');
        return 0;
    }

    if (streq(cmd, "echo")) {
        if (!args) {
            montauk::putchar('\n');
            return 0;
        }
        bool no_newline = false;
        if (starts_with(args, "-n ")) {
            no_newline = true;
            args = skip_spaces(args + 3);
        } else if (streq(args, "-n")) {
            return 0;
        }
        montauk::print(args);
        if (!no_newline) montauk::putchar('\n');
        return 0;
    }

    if (streq(cmd, "set")) {
        if (!args) {
            // List all variables
            if (session_user[0]) {
                montauk::print("USER=");
                montauk::print(session_user);
                montauk::putchar('\n');
            }
            if (session_home[0]) {
                montauk::print("HOME=");
                montauk::print(session_home);
                montauk::putchar('\n');
            }
            char path[128];
            build_dir_path(cwd, path, sizeof(path));
            montauk::print("PWD=");
            montauk::print(path);
            montauk::putchar('\n');
            int vc = var_user_count();
            for (int j = 0; j < vc; j++) {
                montauk::print(var_user_name(j));
                montauk::putchar('=');
                montauk::print(var_user_value(j));
                montauk::putchar('\n');
            }
            return 0;
        }
        // set VAR=value
        int eq = -1;
        for (int j = 0; args[j]; j++) {
            if (args[j] == '=') { eq = j; break; }
        }
        if (eq > 0) {
            char name[32];
            int nlen = eq < 31 ? eq : 31;
            for (int j = 0; j < nlen; j++) name[j] = args[j];
            name[nlen] = '\0';
            var_set(name, args + eq + 1);
            return 0;
        }
        // set VAR (show value)
        const char* val = var_get(args);
        if (val) {
            montauk::print(args);
            montauk::putchar('=');
            montauk::print(val);
            montauk::putchar('\n');
        } else {
            montauk::print(args);
            montauk::print(": not set\n");
        }
        return 0;
    }

    if (streq(cmd, "unset")) {
        if (!args) {
            montauk::print("Usage: unset <variable>\n");
            return 1;
        }
        var_unset(args);
        return 0;
    }

    if (streq(cmd, "exit")) {
        montauk::print("Goodbye.\n");
        montauk::exit(last_exit);
    }

    // External command
    return exec_external(cmd, args);
}

// ---- Command line execution with chaining ----

static void execute_line(const char* raw) {
    // Step 1: expand tilde
    char texp[512];
    expand_tilde(raw, texp, sizeof(texp));

    // Step 2: expand variables
    char expanded[512];
    expand_vars(texp, expanded, sizeof(expanded));

    // Step 3: strip comments
    strip_comment(expanded);

    // Step 4: split on ;, &&, || and execute with chaining logic
    const char* p = expanded;
    int prev = 0;
    enum { OP_NONE, OP_SEMI, OP_AND, OP_OR } pending = OP_NONE;

    while (true) {
        p = skip_spaces(p);
        if (!*p) break;

        // Extract next command segment
        char seg[256];
        int si = 0;
        int op = OP_NONE;
        bool in_sq = false, in_dq = false;

        while (*p && si < 255) {
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; seg[si++] = *p++; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; seg[si++] = *p++; continue; }
            if (!in_sq && !in_dq) {
                if (*p == ';') { op = OP_SEMI; p++; break; }
                if (*p == '&' && p[1] == '&') { op = OP_AND; p += 2; break; }
                if (*p == '|' && p[1] == '|') { op = OP_OR; p += 2; break; }
            }
            seg[si++] = *p++;
        }
        seg[si] = '\0';

        // Trim trailing spaces
        while (si > 0 && seg[si - 1] == ' ') seg[--si] = '\0';

        // Decide whether to run based on pending operator
        bool run = true;
        if (pending == OP_AND && prev != 0) run = false;
        if (pending == OP_OR && prev == 0) run = false;

        if (run && seg[0]) {
            prev = process_command(seg);
        }

        pending = (decltype(pending))op;
        if (op == OP_NONE) break;
    }

    last_exit = prev;
}

// ---- Arrow key scancodes ----

static constexpr uint8_t SC_UP    = 0x48;
static constexpr uint8_t SC_DOWN  = 0x50;
static constexpr uint8_t SC_LEFT  = 0x4B;
static constexpr uint8_t SC_RIGHT = 0x4D;

// ---- Entry point ----

extern "C" void _start() {
    read_session();

    montauk::print("\n");
    montauk::print("  MontaukOS\n");
    montauk::print("  Copyright (c) 2025-2026 Daniel Hammer\n");
    montauk::print("\n");

    if (session_user[0]) {
        montauk::print("  Logged in as ");
        montauk::print(session_user);
        montauk::putchar('\n');
        montauk::print("\n");
    }

    montauk::print("  Type 'help' for available commands.\n");
    montauk::print("\n");

    char line[256];
    int pos = 0;
    int hist_nav = -1;

    prompt();

    while (true) {
        if (!montauk::is_key_available()) {
            montauk::yield();
            continue;
        }

        Montauk::KeyEvent ev;
        montauk::getkey(&ev);

        if (!ev.pressed) continue;

        // Arrow keys: ascii == 0, check scancode
        if (ev.ascii == 0) {
            if (ev.scancode == SC_UP) {
                int next = hist_nav + 1;
                const char* entry = history_get(next);
                if (entry) {
                    hist_nav = next;
                    replace_line(line, &pos, entry);
                }
            } else if (ev.scancode == SC_DOWN) {
                if (hist_nav > 0) {
                    hist_nav--;
                    const char* entry = history_get(hist_nav);
                    if (entry) {
                        replace_line(line, &pos, entry);
                    }
                } else if (hist_nav == 0) {
                    hist_nav = -1;
                    erase_input(pos);
                    pos = 0;
                    line[0] = '\0';
                }
            }
            continue;
        }

        if (ev.ascii == '\n') {
            montauk::putchar('\n');
            line[pos] = '\0';
            history_add(line);
            execute_line(line);
            pos = 0;
            hist_nav = -1;
            prompt();
        } else if (ev.ascii == '\b') {
            if (pos > 0) {
                pos--;
                montauk::putchar('\b');
                montauk::putchar(' ');
                montauk::putchar('\b');
            }
        } else if (ev.ascii >= ' ' && pos < 255) {
            line[pos++] = ev.ascii;
            montauk::putchar(ev.ascii);
        }
    }
}
