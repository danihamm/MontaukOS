/*
    * exec.cpp
    * External command search and execution
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "shell.h"

// ---- Try to spawn an ELF at the given path ----

static bool try_exec(const char* path, const char* args) {
    int h = montauk::open(path);
    if (h < 0) return false;
    montauk::close(h);

    int pid = montauk::spawn(path, args);
    if (pid < 0) return false;
    montauk::waitpid(pid);
    return true;
}

// ---- Resolve arguments: expand relative file paths against CWD ----

static void resolve_args(const char* args, char* out, int outMax) {
    if (!args || !args[0]) { out[0] = '\0'; return; }

    int o = 0;
    const char* p = args;

    while (*p && o < outMax - 1) {
        while (*p == ' ' && o < outMax - 1) { out[o++] = *p++; }
        if (!*p) break;

        const char* tokStart = p;
        int tokLen = 0;
        while (p[tokLen] && p[tokLen] != ' ') tokLen++;

        bool resolve = (cwd[0] || current_drive != 0) && !has_drive_prefix(tokStart) && tokStart[0] != '-';

        if (resolve) {
            char candidate[256];
            int r = 0;
            if (current_drive >= 10) candidate[r++] = '0' + current_drive / 10;
            candidate[r++] = '0' + current_drive % 10;
            candidate[r++] = ':'; candidate[r++] = '/';
            int j = 0;
            while (cwd[j] && r < 255) candidate[r++] = cwd[j++];
            if (cwd[0] && r < 255) candidate[r++] = '/';
            for (int k = 0; k < tokLen && r < 255; k++) candidate[r++] = tokStart[k];
            candidate[r] = '\0';

            int h = montauk::open(candidate);
            if (h >= 0) {
                montauk::close(h);
                for (int k = 0; k < r && o < outMax - 1; k++) out[o++] = candidate[k];
            } else {
                bool looksLikeFile = false;
                for (int k = 0; k < tokLen; k++) {
                    if (tokStart[k] == '.') { looksLikeFile = true; break; }
                }
                if (looksLikeFile) {
                    for (int k = 0; k < r && o < outMax - 1; k++) out[o++] = candidate[k];
                } else {
                    for (int k = 0; k < tokLen && o < outMax - 1; k++) out[o++] = tokStart[k];
                }
            }
        } else {
            for (int k = 0; k < tokLen && o < outMax - 1; k++) out[o++] = tokStart[k];
        }

        p = tokStart + tokLen;
    }
    out[o] = '\0';
}

// ---- Search and execute an external command ----

int exec_external(const char* cmd, const char* args) {
    char path[256];

    char resolvedArgs[512];
    resolve_args(args, resolvedArgs, sizeof(resolvedArgs));
    const char* finalArgs = resolvedArgs[0] ? resolvedArgs : nullptr;

    // 1. Try 0:/os/<cmd>.elf
    scopy(path, "0:/os/", sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return 0;

    // 2. Try 0:/games/<cmd>.elf
    scopy(path, "0:/games/", sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return 0;

    // 3. Try N:/<cwd>/<cmd>.elf on current drive
    if (cwd[0]) {
        build_drive_path(current_drive, "", path, sizeof(path));
        scat(path, cwd, sizeof(path));
        scat(path, "/", sizeof(path));
        scat(path, cmd, sizeof(path));
        scat(path, ".elf", sizeof(path));
        if (try_exec(path, finalArgs)) return 0;
    }

    // 4. Try N:/<cmd>.elf on current drive
    build_drive_path(current_drive, "", path, sizeof(path));
    scat(path, cmd, sizeof(path));
    scat(path, ".elf", sizeof(path));
    if (try_exec(path, finalArgs)) return 0;

    // 5. If on a non-zero drive, also try 0:/<cmd>.elf
    if (current_drive != 0) {
        scopy(path, "0:/", sizeof(path));
        scat(path, cmd, sizeof(path));
        scat(path, ".elf", sizeof(path));
        if (try_exec(path, finalArgs)) return 0;
    }

    montauk::print(cmd);
    montauk::print(": command not found\n");
    return 127;
}
