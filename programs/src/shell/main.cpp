/*
    * main.cpp
    * Interactive shell for ZenithOS
    * Copyright (c) 2025 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::slen;
using zenith::streq;
using zenith::starts_with;
using zenith::skip_spaces;

static void scopy(char* dst, const char* src, int maxLen) {
    int i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void scat(char* dst, const char* src, int maxLen) {
    int dLen = slen(dst);
    int i = 0;
    while (src[i] && dLen + i < maxLen - 1) {
        dst[dLen + i] = src[i];
        i++;
    }
    dst[dLen + i] = '\0';
}

// Current working directory (relative to 0:/).
// "" = root, "man" = 0:/man, "man/sub" = 0:/man/sub
static char cwd[128] = "";

// Build VFS directory path: "0:/" or "0:/<dir>"
static void build_dir_path(const char* dir, char* out, int outMax) {
    int i = 0;
    out[i++] = '0'; out[i++] = ':'; out[i++] = '/';
    if (dir && dir[0]) {
        int j = 0;
        while (dir[j] && i < outMax - 1) out[i++] = dir[j++];
    }
    out[i] = '\0';
}

// ---- Command history ----

static constexpr int HISTORY_MAX = 32;
static char history[HISTORY_MAX][256];
static int history_count = 0;
static int history_next = 0; // ring-buffer write index

static void history_add(const char* line) {
    if (line[0] == '\0') return;
    // Don't add duplicate of last entry
    if (history_count > 0) {
        int prev = (history_next + HISTORY_MAX - 1) % HISTORY_MAX;
        if (streq(history[prev], line)) return;
    }
    scopy(history[history_next], line, 256);
    history_next = (history_next + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
}

// Get history entry by index (0 = most recent, 1 = one before that, ...)
static const char* history_get(int idx) {
    if (idx < 0 || idx >= history_count) return nullptr;
    int pos = (history_next + HISTORY_MAX - 1 - idx) % HISTORY_MAX;
    return history[pos];
}

// ---- Prompt ----

static void prompt() {
    zenith::print("0:/");
    if (cwd[0]) zenith::print(cwd);
    zenith::print("> ");
}

// ---- Erase current input line on screen ----

static void erase_input(int len) {
    // Move cursor to start of input and overwrite with spaces
    for (int i = 0; i < len; i++) zenith::putchar('\b');
    for (int i = 0; i < len; i++) zenith::putchar(' ');
    for (int i = 0; i < len; i++) zenith::putchar('\b');
}

// ---- Replace visible line with new content ----

static void replace_line(char* line, int* pos, const char* newContent) {
    int oldLen = *pos;
    erase_input(oldLen);
    int newLen = slen(newContent);
    if (newLen > 255) newLen = 255;
    for (int i = 0; i < newLen; i++) {
        line[i] = newContent[i];
        zenith::putchar(newContent[i]);
    }
    line[newLen] = '\0';
    *pos = newLen;
}

// ---- Builtin: help ----

static void cmd_help() {
    zenith::print("Shell builtins:\n");
    zenith::print("  help          Show this help message\n");
    zenith::print("  ls [dir]      List files in directory\n");
    zenith::print("  cd [dir]      Change working directory\n");
    zenith::print("  exit          Exit the shell\n");
    zenith::print("\n");
    zenith::print("System commands:\n");
    zenith::print("  man <topic>   View manual pages\n");
    zenith::print("  cat <file>    Display file contents\n");
    zenith::print("  edit [file]   Text editor\n");
    zenith::print("  info          Show system information\n");
    zenith::print("  date          Show current date and time\n");
    zenith::print("  uptime        Show uptime\n");
    zenith::print("  clear         Clear the screen\n");
    zenith::print("  reset         Reboot the system\n");
    zenith::print("  shutdown      Shut down the system\n");
    zenith::print("\n");
    zenith::print("Network commands:\n");
    zenith::print("  ping <ip>     Send ICMP echo requests\n");
    zenith::print("  ifconfig      Show/set network configuration\n");
    zenith::print("  tcpconnect    Connect to a TCP server\n");
    zenith::print("  irc           IRC client\n");
    zenith::print("  dhcp          DHCP client\n");
    zenith::print("  fetch <url>   HTTP client\n");
    zenith::print("  httpd         HTTP server\n");
    zenith::print("\n");
    zenith::print("Games:\n");
    zenith::print("  doom          DOOM\n");
    zenith::print("\n");
    zenith::print("Any .elf on the ramdisk is executable.\n");
}

// ---- Builtin: ls ----

static void cmd_ls(const char* arg) {
    arg = skip_spaces(arg);

    // Build the target directory (relative path from root)
    char dir[128];
    if (*arg) {
        // ls <dir> -- combine cwd and arg
        if (cwd[0]) {
            int i = 0, j = 0;
            while (cwd[j] && i < 126) dir[i++] = cwd[j++];
            dir[i++] = '/';
            j = 0;
            while (arg[j] && i < 126) dir[i++] = arg[j++];
            dir[i] = '\0';
        } else {
            int i = 0;
            while (arg[i] && i < 126) { dir[i] = arg[i]; i++; }
            dir[i] = '\0';
        }
    } else {
        // ls with no arg -- use cwd
        int i = 0;
        while (cwd[i] && i < 126) { dir[i] = cwd[i]; i++; }
        dir[i] = '\0';
    }

    char path[128];
    build_dir_path(dir, path, sizeof(path));

    const char* entries[64];
    int count = zenith::readdir(path, entries, 64);
    if (count <= 0) {
        zenith::print("(empty)\n");
        return;
    }

    // Prefix to strip: "dir/" (if dir is non-empty)
    int prefixLen = 0;
    if (dir[0]) prefixLen = slen(dir) + 1;

    for (int i = 0; i < count; i++) {
        zenith::print("  ");
        if (prefixLen > 0 && starts_with(entries[i], dir)) {
            zenith::print(entries[i] + prefixLen);
        } else {
            zenith::print(entries[i]);
        }
        zenith::putchar('\n');
    }
}

// ---- Builtin: cd ----

static void cmd_cd(const char* arg) {
    arg = skip_spaces(arg);

    // Strip trailing slashes from argument (ls shows dirs as "www/", user may type that)
    static char argBuf[128];
    int aLen = 0;
    while (arg[aLen] && aLen < 127) { argBuf[aLen] = arg[aLen]; aLen++; }
    argBuf[aLen] = '\0';
    while (aLen > 0 && argBuf[aLen - 1] == '/') argBuf[--aLen] = '\0';
    arg = argBuf;

    // cd or cd / -> go to root
    if (*arg == '\0' || streq(arg, "/")) {
        cwd[0] = '\0';
        return;
    }

    // cd .. -> go up one level
    if (streq(arg, "..")) {
        int len = slen(cwd);
        int last = -1;
        for (int i = 0; i < len; i++) {
            if (cwd[i] == '/') last = i;
        }
        if (last >= 0) {
            cwd[last] = '\0';
        } else {
            cwd[0] = '\0';
        }
        return;
    }

    // Build target directory path
    char target[128];
    if (cwd[0]) {
        int i = 0, j = 0;
        while (cwd[j] && i < 126) target[i++] = cwd[j++];
        target[i++] = '/';
        j = 0;
        while (arg[j] && i < 126) target[i++] = arg[j++];
        target[i] = '\0';
    } else {
        int i = 0;
        while (arg[i] && i < 126) { target[i] = arg[i]; i++; }
        target[i] = '\0';
    }

    // Validate: try readdir on the target
    char path[128];
    build_dir_path(target, path, sizeof(path));
    const char* entries[1];
    int count = zenith::readdir(path, entries, 1);
    if (count < 0) {
        zenith::print("cd: no such directory: ");
        zenith::print(arg);
        zenith::putchar('\n');
        return;
    }

    // Set cwd
    int i = 0;
    while (target[i] && i < 126) { cwd[i] = target[i]; i++; }
    cwd[i] = '\0';
}

// ---- Builtin: man ----

static void cmd_man(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        zenith::print("Usage: man <topic>\n");
        zenith::print("       man <section> <topic>\n");
        zenith::print("Try: man intro\n");
        return;
    }

    int pid = zenith::spawn("0:/os/man.elf", arg);
    if (pid < 0) {
        zenith::print("Error: failed to start man viewer\n");
    } else {
        zenith::waitpid(pid);
    }
}

// ---- External command execution ----

// Try to spawn an ELF at the given path. Returns true on success.
static bool try_exec(const char* path, const char* args) {
    // Check if the file exists before asking the kernel to load it
    int h = zenith::open(path);
    if (h < 0) return false;
    zenith::close(h);

    int pid = zenith::spawn(path, args);
    if (pid < 0) return false;
    zenith::waitpid(pid);
    return true;
}

// Check if a string already has a VFS drive prefix (e.g. "0:/")
static bool has_drive_prefix(const char* s) {
    return s[0] >= '0' && s[0] <= '9' && s[1] == ':';
}

// Resolve arguments: expand relative file paths against CWD.
// Tokens that already have a drive prefix (e.g. "0:/foo") are left as-is.
// Everything before the first space-delimited token that looks like a path
// option (starts with '-') is also left as-is.
static void resolve_args(const char* args, char* out, int outMax) {
    if (!args || !args[0]) { out[0] = '\0'; return; }

    int o = 0;
    const char* p = args;

    while (*p && o < outMax - 1) {
        // Skip spaces, copy them through
        while (*p == ' ' && o < outMax - 1) { out[o++] = *p++; }
        if (!*p) break;

        // Extract the token
        const char* tokStart = p;
        int tokLen = 0;
        while (p[tokLen] && p[tokLen] != ' ') tokLen++;

        // Decide whether to resolve this token as a path.
        // Don't resolve if it already has a drive prefix, or starts with '-'
        bool resolve = cwd[0] && !has_drive_prefix(tokStart) && tokStart[0] != '-';

        if (resolve) {
            // Write "0:/<cwd>/<token>"
            if (o + 3 < outMax) { out[o++] = '0'; out[o++] = ':'; out[o++] = '/'; }
            int j = 0;
            while (cwd[j] && o < outMax - 1) out[o++] = cwd[j++];
            if (o < outMax - 1) out[o++] = '/';
            for (int k = 0; k < tokLen && o < outMax - 1; k++) out[o++] = tokStart[k];
        } else {
            for (int k = 0; k < tokLen && o < outMax - 1; k++) out[o++] = tokStart[k];
        }

        p = tokStart + tokLen;
    }
    out[o] = '\0';
}

static void exec_external(const char* cmd, const char* args) {
    char path[256];

    // Resolve arguments against CWD so external programs get full VFS paths
    char resolvedArgs[512];
    resolve_args(args, resolvedArgs, sizeof(resolvedArgs));
    const char* finalArgs = resolvedArgs[0] ? resolvedArgs : nullptr;

    // 1. Try 0:/os/<cmd>.elf
    scopy(path, "0:/os/", sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return;

    // 2. Try 0:/games/<cmd>.elf
    scopy(path, "0:/games/", sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return;

    // 3. Try 0:/<cwd>/<cmd>.elf (if cwd is set)
    if (cwd[0]) {
        scopy(path, "0:/", sizeof(path));
        scat(path, cwd, sizeof(path));
        scat(path, "/", sizeof(path));
        scat(path, cmd, sizeof(path));
        scat(path, ".elf", sizeof(path));
        if (try_exec(path, finalArgs)) return;
    }

    // 4. Try 0:/<cmd>.elf
    scopy(path, "0:/", sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return;

    // Not found
    zenith::print(cmd);
    zenith::print(": command not found\n");
}

// ---- Command dispatch ----

static void process_command(const char* line) {
    line = skip_spaces(line);
    if (*line == '\0') return;

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

    // Builtins
    if (streq(cmd, "help")) {
        cmd_help();
    } else if (streq(cmd, "ls")) {
        cmd_ls(args ? args : "");
    } else if (streq(cmd, "cd")) {
        cmd_cd(args ? args : "");
    } else if (streq(cmd, "man")) {
        cmd_man(args ? args : "");
    } else if (streq(cmd, "exit")) {
        zenith::print("Goodbye.\n");
        zenith::exit(0);
    } else {
        // External command -- pass full argument string
        exec_external(cmd, args);
    }
}

// ---- Arrow key scancodes ----

static constexpr uint8_t SC_UP    = 0x48;
static constexpr uint8_t SC_DOWN  = 0x50;
static constexpr uint8_t SC_LEFT  = 0x4B;
static constexpr uint8_t SC_RIGHT = 0x4D;

// ---- Entry point ----

extern "C" void _start() {
    zenith::print("\n");
    zenith::print("  ZenithOS\n");
    zenith::print("  Copyright (c) 2025-2026 Daniel Hammer\n");
    zenith::print("\n");

    zenith::print("  Type 'help' for available commands.\n");
    zenith::print("\n");

    char line[256];
    int pos = 0;
    int hist_nav = -1; // -1 = not navigating history

    prompt();

    while (true) {
        if (!zenith::is_key_available()) {
            zenith::yield();
            continue;
        }

        Zenith::KeyEvent ev;
        zenith::getkey(&ev);

        if (!ev.pressed) continue;

        // Arrow keys: ascii == 0, check scancode
        if (ev.ascii == 0) {
            if (ev.scancode == SC_UP) {
                // Navigate to older history entry
                int next = hist_nav + 1;
                const char* entry = history_get(next);
                if (entry) {
                    hist_nav = next;
                    replace_line(line, &pos, entry);
                }
            } else if (ev.scancode == SC_DOWN) {
                // Navigate to newer history entry
                if (hist_nav > 0) {
                    hist_nav--;
                    const char* entry = history_get(hist_nav);
                    if (entry) {
                        replace_line(line, &pos, entry);
                    }
                } else if (hist_nav == 0) {
                    // Back to empty line
                    hist_nav = -1;
                    erase_input(pos);
                    pos = 0;
                    line[0] = '\0';
                }
            }
            // Left/Right arrows: ignore for now (no cursor movement within line)
            continue;
        }

        if (ev.ascii == '\n') {
            zenith::putchar('\n');
            line[pos] = '\0';
            history_add(line);
            process_command(line);
            pos = 0;
            hist_nav = -1;
            prompt();
        } else if (ev.ascii == '\b') {
            if (pos > 0) {
                pos--;
                zenith::putchar('\b');
                zenith::putchar(' ');
                zenith::putchar('\b');
            }
        } else if (ev.ascii >= ' ' && pos < 255) {
            line[pos++] = ev.ascii;
            zenith::putchar(ev.ascii);
        }
    }
}
