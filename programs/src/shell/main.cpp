/*
    * main.cpp
    * Interactive shell for ZenithOS
    * Copyright (c) 2025 Daniel Hammer
*/

#include <zenith/syscall.h>

static bool streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return false;
        str++; prefix++;
    }
    return true;
}

static const char* skip_spaces(const char* s) {
    while (*s == ' ') s++;
    return s;
}

static int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// Current working directory (relative to 0:/).
// "" = root, "man" = 0:/man, "man/sub" = 0:/man/sub
static char cwd[128] = "";

// Build full VFS path: "0:/" + cwd + "/" + name
static void resolve_path(const char* name, char* out, int outMax) {
    int i = 0;
    out[i++] = '0'; out[i++] = ':'; out[i++] = '/';
    if (cwd[0]) {
        int j = 0;
        while (cwd[j] && i < outMax - 2) out[i++] = cwd[j++];
        out[i++] = '/';
    }
    int j = 0;
    while (name[j] && i < outMax - 1) out[i++] = name[j++];
    out[i] = '\0';
}

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

static void print_int(uint64_t n) {
    if (n == 0) {
        zenith::putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        zenith::putchar(buf[j]);
    }
}

static void prompt() {
    zenith::print("0:/");
    if (cwd[0]) zenith::print(cwd);
    zenith::print("> ");
}

static void cmd_help() {
    zenith::print("Available commands:\n");
    zenith::print("  help          Show this help message\n");
    zenith::print("  info          Show system information\n");
    zenith::print("  man <topic>   View manual pages\n");
    zenith::print("  ls [dir]      List files in directory\n");
    zenith::print("  cd [dir]      Change working directory\n");
    zenith::print("  cat <file>    Display file contents\n");
    zenith::print("  run <file>    Spawn a new process from an ELF file\n");
    zenith::print("  ping <ip>     Send ICMP echo requests\n");
    zenith::print("  date          Show current date and time\n");
    zenith::print("  uptime        Show uptime in milliseconds\n");
    zenith::print("  clear         Clear the screen\n");
    zenith::print("  reset         Reboot the system\n");
    zenith::print("  shutdown      Shut down the system\n");
    zenith::print("  exit          Exit the shell\n");
}

static void cmd_info() {
    Zenith::SysInfo info;
    zenith::get_info(&info);
    zenith::print(info.osName);
    zenith::print(" v");
    zenith::print(info.osVersion);
    zenith::print("\n");
    zenith::print("Syscall API version: ");
    print_int(info.apiVersion);
    zenith::putchar('\n');
}

static void cmd_ls(const char* arg) {
    arg = skip_spaces(arg);

    // Build the target directory (relative path from root)
    char dir[128];
    if (*arg) {
        // ls <dir> — combine cwd and arg
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
        // ls with no arg — use cwd
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

static void cmd_cat(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        zenith::print("Usage: cat <filename>\n");
        return;
    }

    char path[128];
    resolve_path(arg, path, sizeof(path));

    int handle = zenith::open(path);
    if (handle < 0) {
        zenith::print("Error: cannot open '");
        zenith::print(arg);
        zenith::print("'\n");
        return;
    }

    uint64_t size = zenith::getsize(handle);
    if (size == 0) {
        zenith::close(handle);
        return;
    }

    // Read in chunks
    uint8_t buf[512];
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
        int bytesRead = zenith::read(handle, buf, offset, chunk);
        if (bytesRead <= 0) break;
        buf[bytesRead] = '\0';
        zenith::print((const char*)buf);
        offset += bytesRead;
    }

    zenith::close(handle);
    zenith::putchar('\n');
}

static void cmd_uptime() {
    uint64_t ms = zenith::get_milliseconds();
    uint64_t secs = ms / 1000;
    uint64_t mins = secs / 60;
    secs %= 60;
    ms %= 1000;

    zenith::print("Uptime: ");
    print_int(mins);
    zenith::print("m ");
    print_int(secs);
    zenith::print("s ");
    print_int(ms);
    zenith::print("ms\n");
}

static bool parse_ip(const char* s, uint32_t* out) {
    // Parse "a.b.c.d" into a uint32_t in network byte order (little-endian stored)
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
            val = 0;
            hasDigit = false;
            if (c == '\0') break;
        } else {
            return false;
        }
    }

    if (idx != 4) return false;
    *out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

static void print_ip(uint32_t ip) {
    print_int(ip & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 8) & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 16) & 0xFF);
    zenith::putchar('.');
    print_int((ip >> 24) & 0xFF);
}

static void cmd_ping(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        zenith::print("Usage: ping <ip address>\n");
        return;
    }

    uint32_t ip;
    if (!parse_ip(arg, &ip)) {
        zenith::print("Invalid IP address: ");
        zenith::print(arg);
        zenith::putchar('\n');
        return;
    }

    zenith::print("PING ");
    print_ip(ip);
    zenith::putchar('\n');

    for (int i = 0; i < 4; i++) {
        int32_t rtt = zenith::ping(ip, 3000);
        if (rtt < 0) {
            zenith::print("  Request timed out\n");
        } else {
            zenith::print("  Reply from ");
            print_ip(ip);
            zenith::print(": time=");
            print_int((uint64_t)rtt);
            zenith::print("ms\n");
        }
        if (i < 3) {
            zenith::sleep_ms(1000);
        }
    }
}

static void cmd_run(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        zenith::print("Usage: run <filename>\n");
        return;
    }

    char path[128];
    resolve_path(arg, path, sizeof(path));

    int pid = zenith::spawn(path);
    if (pid < 0) {
        zenith::print("Error: failed to spawn '");
        zenith::print(arg);
        zenith::print("'\n");
    } else {
        zenith::waitpid(pid);
    }
}

static void cmd_cd(const char* arg) {
    arg = skip_spaces(arg);

    // cd or cd / → go to root
    if (*arg == '\0' || streq(arg, "/")) {
        cwd[0] = '\0';
        return;
    }

    // cd .. → go up one level
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

static void cmd_man(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        zenith::print("Usage: man <topic>\n");
        zenith::print("       man <section> <topic>\n");
        zenith::print("Try: man intro\n");
        return;
    }

    int pid = zenith::spawn("0:/man.elf", arg);
    if (pid < 0) {
        zenith::print("Error: failed to start man viewer\n");
    } else {
        zenith::waitpid(pid);
    }
}

static void print_int_padded(uint64_t n) {
    if (n < 10) zenith::putchar('0');
    print_int(n);
}

static const char* month_name(int m) {
    static const char* months[] = {
        "", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    if (m >= 1 && m <= 12) return months[m];
    return "?";
}

static void cmd_date() {
    Zenith::DateTime dt;
    zenith::gettime(&dt);

    print_int(dt.Day);
    zenith::putchar(' ');
    zenith::print(month_name(dt.Month));
    zenith::putchar(' ');
    print_int(dt.Year);
    zenith::print(", ");
    print_int(dt.Hour);
    zenith::putchar(':');
    print_int_padded(dt.Minute);
    zenith::putchar(':');
    print_int_padded(dt.Second);
    zenith::print(" UTC\n");
}

static void cmd_clear() {
    zenith::print("\033[2J");   // Clear entire screen
    zenith::print("\033[H");    // Move cursor to top-left
}

static void process_command(const char* line) {
    // Skip leading spaces
    line = skip_spaces(line);
    if (*line == '\0') return;

    if (streq(line, "help")) {
        cmd_help();
    } else if (streq(line, "info")) {
        cmd_info();
    } else if (starts_with(line, "ls ")) {
        cmd_ls(line + 3);
    } else if (streq(line, "ls")) {
        cmd_ls("");
    } else if (starts_with(line, "cd ")) {
        cmd_cd(line + 3);
    } else if (streq(line, "cd")) {
        cmd_cd("");
    } else if (starts_with(line, "man ")) {
        cmd_man(line + 4);
    } else if (streq(line, "man")) {
        cmd_man("");
    } else if (starts_with(line, "cat ")) {
        cmd_cat(line + 4);
    } else if (streq(line, "cat")) {
        cmd_cat("");
    } else if (starts_with(line, "run ")) {
        cmd_run(line + 4);
    } else if (streq(line, "run")) {
        cmd_run("");
    } else if (starts_with(line, "ping ")) {
        cmd_ping(line + 5);
    } else if (streq(line, "ping")) {
        cmd_ping("");
    } else if (streq(line, "date")) {
        cmd_date();
    } else if (streq(line, "uptime")) {
        cmd_uptime();
    } else if (streq(line, "clear")) {
        cmd_clear();
    } else if (streq(line, "reset")) {
        zenith::print("Rebooting...\n");
        zenith::reset();
    } else if (streq(line, "shutdown")) {
        zenith::print("Shutting down...\n");
        zenith::shutdown();
    } else if (streq(line, "exit")) {
        zenith::print("Goodbye.\n");
        zenith::exit(0);
    } else {
        zenith::print("Unknown command: ");
        zenith::print(line);
        zenith::print("\nType 'help' for available commands.\n");
    }
}

extern "C" void _start() {
    zenith::print("\n");
    zenith::print("  ZenithOS\n");
    zenith::print("  Copyright (c) 2025-2026 Daniel Hammer\n");
    zenith::print("\n");

    zenith::print("  Type 'help' for available commands.\n");
    zenith::print("\n");

    char line[256];
    int pos = 0;

    prompt();

    while (true) {
        char c = zenith::getchar();

        if (c == '\n') {
            zenith::putchar('\n');
            line[pos] = '\0';
            process_command(line);
            pos = 0;
            prompt();
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                zenith::putchar('\b');
                zenith::putchar(' ');
                zenith::putchar('\b');
            }
        } else if (c >= ' ' && pos < 255) {
            line[pos++] = c;
            zenith::putchar(c);
        }
    }
}
