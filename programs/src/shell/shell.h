/*
    * shell.h
    * Shared declarations for the MontaukOS interactive shell
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#pragma once

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/config.h>

using montauk::slen;
using montauk::streq;
using montauk::starts_with;
using montauk::skip_spaces;

// ---- Inline string helpers ----

inline void scopy(char* dst, const char* src, int maxLen) {
    int i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

inline void scat(char* dst, const char* src, int maxLen) {
    int dLen = slen(dst);
    int i = 0;
    while (src[i] && dLen + i < maxLen - 1) {
        dst[dLen + i] = src[i];
        i++;
    }
    dst[dLen + i] = '\0';
}

inline void int_to_str(int n, char* buf, int sz) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    bool neg = false;
    unsigned int u;
    if (n < 0) { neg = true; u = (unsigned)(-n); } else { u = (unsigned)n; }
    char tmp[12];
    int i = 0;
    while (u > 0) { tmp[i++] = '0' + u % 10; u /= 10; }
    int o = 0;
    if (neg && o < sz - 1) buf[o++] = '-';
    while (i > 0 && o < sz - 1) buf[o++] = tmp[--i];
    buf[o] = '\0';
}

// ---- Shared state (defined in main.cpp) ----

extern char cwd[128];
extern int current_drive;
extern int last_exit;
extern char session_user[32];
extern char session_home[64];

// ---- Inline path helpers ----

inline void build_drive_path(int drive, const char* dir, char* out, int outMax) {
    int i = 0;
    if (drive >= 10) { out[i++] = '0' + drive / 10; }
    out[i++] = '0' + drive % 10;
    out[i++] = ':'; out[i++] = '/';
    if (dir && dir[0]) {
        int j = 0;
        while (dir[j] && i < outMax - 1) out[i++] = dir[j++];
    }
    out[i] = '\0';
}

inline void build_dir_path(const char* dir, char* out, int outMax) {
    build_drive_path(current_drive, dir, out, outMax);
}

inline int parse_drive_prefix(const char* s) {
    if (s[0] < '0' || s[0] > '9') return -1;
    int n = s[0] - '0';
    if (s[1] >= '0' && s[1] <= '9') {
        n = n * 10 + (s[1] - '0');
        if (s[2] != ':') return -1;
    } else if (s[1] == ':') {
        // single digit drive
    } else {
        return -1;
    }
    return n;
}

inline int drive_prefix_len(const char* s) {
    if (s[1] >= '0' && s[1] <= '9') return 3;
    return 2;
}

inline bool has_drive_prefix(const char* s) {
    return parse_drive_prefix(s) >= 0;
}

// ---- Variables (vars.cpp) ----

void var_set(const char* name, const char* value);
void var_unset(const char* name);
const char* var_get(const char* name);
bool is_var_char(char c);
void expand_vars(const char* in, char* out, int outMax);
void expand_tilde(const char* in, char* out, int outMax);
void strip_comment(char* line);
int var_user_count();
const char* var_user_name(int idx);
const char* var_user_value(int idx);

// ---- Session (main.cpp) ----

void read_session();

// ---- Builtins (builtins.cpp) ----

void cmd_help();
void cmd_ls(const char* arg);
int cmd_cd(const char* arg);
int cmd_man(const char* arg);
bool switch_drive(int drive);

// ---- External execution (exec.cpp) ----

int exec_external(const char* cmd, const char* args);
