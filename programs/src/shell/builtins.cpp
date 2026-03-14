/*
    * builtins.cpp
    * Shell builtin commands: help, ls, cd, man
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "shell.h"

// ---- help ----

void cmd_help() {
    montauk::print("Shell builtins:\n");
    montauk::print("  help            Show this help message\n");
    montauk::print("  ls [dir]        List files in directory\n");
    montauk::print("  cd [dir]        Change working directory\n");
    montauk::print("  pwd             Print working directory\n");
    montauk::print("  echo [-n] ...   Print arguments\n");
    montauk::print("  set [VAR=val]   Show or set shell variables\n");
    montauk::print("  unset VAR       Remove a shell variable\n");
    montauk::print("  true / false    Return success / failure\n");
    montauk::print("  N:              Switch to drive N (e.g. 1:)\n");
    montauk::print("  exit            Exit the shell\n");
    montauk::print("\n");
    montauk::print("Syntax:\n");
    montauk::print("  VAR=value       Set a shell variable\n");
    montauk::print("  $VAR ${VAR}     Variable expansion\n");
    montauk::print("  ~               Expands to home directory\n");
    montauk::print("  cmd1 ; cmd2     Run commands sequentially\n");
    montauk::print("  cmd1 && cmd2    Run cmd2 if cmd1 succeeds\n");
    montauk::print("  cmd1 || cmd2    Run cmd2 if cmd1 fails\n");
    montauk::print("  # comment       Comment (ignored)\n");
    montauk::print("\n");
    montauk::print("Built-in variables:\n");
    montauk::print("  $USER  $HOME  $PWD  $?\n");
    montauk::print("\n");
    montauk::print("System commands:\n");
    montauk::print("  man <topic>     View manual pages\n");
    montauk::print("  cat <file>      Display file contents\n");
    montauk::print("  edit [file]     Text editor\n");
    montauk::print("  whoami          Print current username\n");
    montauk::print("  info            Show system information\n");
    montauk::print("  date            Show current date and time\n");
    montauk::print("  uptime          Show uptime\n");
    montauk::print("  clear           Clear the screen\n");
    montauk::print("  fontscale [n]   Set terminal font scale (1-8)\n");
    montauk::print("  reset           Reboot the system\n");
    montauk::print("  shutdown        Shut down the system\n");
    montauk::print("\n");
    montauk::print("Network commands:\n");
    montauk::print("  ping <ip>       Send ICMP echo requests\n");
    montauk::print("  nslookup        DNS lookup\n");
    montauk::print("  ifconfig        Show/set network configuration\n");
    montauk::print("  tcpconnect      Connect to a TCP server\n");
    montauk::print("  irc             IRC client\n");
    montauk::print("  dhcp            DHCP client\n");
    montauk::print("  fetch <url>     HTTP client\n");
    montauk::print("  httpd           HTTP server\n");
    montauk::print("\n");
    montauk::print("Games:\n");
    montauk::print("  doom            DOOM\n");
    montauk::print("\n");
    montauk::print("Any .elf on the ramdisk is executable.\n");
}

// ---- ls ----

void cmd_ls(const char* arg) {
    arg = skip_spaces(arg);

    char dir[128];
    int drive = current_drive;
    if (*arg) {
        if (has_drive_prefix(arg)) {
            drive = parse_drive_prefix(arg);
            int plen = drive_prefix_len(arg);
            scopy(dir, arg + plen + 1, sizeof(dir));
        } else if (arg[0] == '/') {
            scopy(dir, arg + 1, sizeof(dir));
        } else if (cwd[0]) {
            scopy(dir, cwd, sizeof(dir));
            scat(dir, "/", sizeof(dir));
            scat(dir, arg, sizeof(dir));
        } else {
            scopy(dir, arg, sizeof(dir));
        }
    } else {
        scopy(dir, cwd, sizeof(dir));
    }

    char path[128];
    build_drive_path(drive, dir, path, sizeof(path));

    const char* entries[64];
    int count = montauk::readdir(path, entries, 64);
    if (count <= 0) {
        montauk::print("(empty)\n");
        return;
    }

    int prefixLen = 0;
    if (dir[0]) prefixLen = slen(dir) + 1;

    for (int i = 0; i < count; i++) {
        montauk::print("  ");
        if (prefixLen > 0 && starts_with(entries[i], dir)) {
            montauk::print(entries[i] + prefixLen);
        } else {
            montauk::print(entries[i]);
        }
        montauk::putchar('\n');
    }
}

// ---- cd ----

bool switch_drive(int drive) {
    char path[8];
    build_drive_path(drive, "", path, sizeof(path));
    const char* entries[1];
    if (montauk::readdir(path, entries, 1) < 0) return false;
    current_drive = drive;
    cwd[0] = '\0';
    return true;
}

int cmd_cd(const char* arg) {
    arg = skip_spaces(arg);

    // cd with no argument -> go to home directory (or root if no session)
    if (*arg == '\0') {
        if (session_home[0] && has_drive_prefix(session_home)) {
            int drive = parse_drive_prefix(session_home);
            int plen = drive_prefix_len(session_home);
            const char* rel = session_home + plen;
            if (*rel == '/') rel++;
            current_drive = drive;
            if (*rel) scopy(cwd, rel, sizeof(cwd));
            else cwd[0] = '\0';
        } else {
            cwd[0] = '\0';
        }
        return 0;
    }

    // cd / -> go to root
    if (streq(arg, "/")) {
        cwd[0] = '\0';
        return 0;
    }

    // Strip trailing slashes from argument
    static char argBuf[128];
    int aLen = 0;
    while (arg[aLen] && aLen < 127) { argBuf[aLen] = arg[aLen]; aLen++; }
    argBuf[aLen] = '\0';
    while (aLen > 0 && argBuf[aLen - 1] == '/') argBuf[--aLen] = '\0';
    arg = argBuf;

    if (*arg == '\0') {
        cwd[0] = '\0';
        return 0;
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
        return 0;
    }

    // cd /path -> absolute path from root
    if (arg[0] == '/') {
        arg++;
        if (*arg == '\0') { cwd[0] = '\0'; return 0; }
        char path[128];
        build_dir_path(arg, path, sizeof(path));
        const char* entries[1];
        if (montauk::readdir(path, entries, 1) < 0) {
            montauk::print("cd: no such directory: ");
            montauk::print(arg);
            montauk::putchar('\n');
            return 1;
        }
        scopy(cwd, arg, sizeof(cwd));
        return 0;
    }

    // cd N:/ or cd N:/path -> switch drive
    if (has_drive_prefix(arg)) {
        int drive = parse_drive_prefix(arg);
        int plen = drive_prefix_len(arg);
        const char* rel = arg + plen;
        if (*rel == '/') rel++;

        char rootPath[8];
        build_drive_path(drive, "", rootPath, sizeof(rootPath));
        const char* rootEntries[1];
        if (montauk::readdir(rootPath, rootEntries, 1) < 0) {
            montauk::print("cd: no such drive: ");
            montauk::print(arg);
            montauk::putchar('\n');
            return 1;
        }

        if (*rel != '\0') {
            char path[128];
            build_drive_path(drive, rel, path, sizeof(path));
            const char* entries[1];
            if (montauk::readdir(path, entries, 1) < 0) {
                montauk::print("cd: no such directory: ");
                montauk::print(arg);
                montauk::putchar('\n');
                return 1;
            }
            current_drive = drive;
            scopy(cwd, rel, sizeof(cwd));
        } else {
            current_drive = drive;
            cwd[0] = '\0';
        }
        return 0;
    }

    // Relative path
    char target[128];
    if (cwd[0]) {
        scopy(target, cwd, sizeof(target));
        scat(target, "/", sizeof(target));
        scat(target, arg, sizeof(target));
    } else {
        scopy(target, arg, sizeof(target));
    }

    char path[128];
    build_dir_path(target, path, sizeof(path));
    const char* entries[1];
    int count = montauk::readdir(path, entries, 1);
    if (count < 0) {
        montauk::print("cd: no such directory: ");
        montauk::print(arg);
        montauk::putchar('\n');
        return 1;
    }

    scopy(cwd, target, sizeof(cwd));
    return 0;
}

// ---- man ----

int cmd_man(const char* arg) {
    arg = skip_spaces(arg);
    if (*arg == '\0') {
        montauk::print("Usage: man <topic>\n");
        montauk::print("       man <section> <topic>\n");
        montauk::print("Try: man intro\n");
        return 1;
    }

    int pid = montauk::spawn("0:/os/man.elf", arg);
    if (pid < 0) {
        montauk::print("Error: failed to start man viewer\n");
        return 1;
    }
    montauk::waitpid(pid);
    return 0;
}
