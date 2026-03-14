/*
    * vars.cpp
    * Shell variable storage, expansion, and tilde/comment processing
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "shell.h"

// ---- Storage ----

static constexpr int MAX_VARS = 32;

struct ShellVar {
    char name[32];
    char value[128];
};

static ShellVar vars[MAX_VARS];
static int var_count = 0;

// ---- Core operations ----

void var_set(const char* name, const char* value) {
    for (int i = 0; i < var_count; i++) {
        if (streq(vars[i].name, name)) {
            scopy(vars[i].value, value, sizeof(vars[i].value));
            return;
        }
    }
    if (var_count < MAX_VARS) {
        scopy(vars[var_count].name, name, sizeof(vars[var_count].name));
        scopy(vars[var_count].value, value, sizeof(vars[var_count].value));
        var_count++;
    }
}

void var_unset(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (streq(vars[i].name, name)) {
            for (int j = i; j < var_count - 1; j++) vars[j] = vars[j + 1];
            var_count--;
            return;
        }
    }
}

// Get variable value. Built-in dynamic vars ($?, $PWD) are synthesized on
// demand. Returns nullptr if not found. The returned pointer for dynamic
// vars points to a static buffer overwritten on the next call.
const char* var_get(const char* name) {
    // User-defined vars take priority
    for (int i = 0; i < var_count; i++) {
        if (streq(vars[i].name, name)) return vars[i].value;
    }
    // Synthesize built-in vars
    static char synth[128];
    if (streq(name, "?")) {
        int_to_str(last_exit, synth, sizeof(synth));
        return synth;
    }
    if (streq(name, "USER")) return session_user[0] ? session_user : nullptr;
    if (streq(name, "HOME")) return session_home[0] ? session_home : nullptr;
    if (streq(name, "PWD")) {
        build_dir_path(cwd, synth, sizeof(synth));
        return synth;
    }
    return nullptr;
}

// ---- Helpers used by the set builtin (main.cpp) ----

// The set builtin needs to iterate user-defined vars and show built-in
// vars. We expose a small iteration API rather than the raw array.

int var_user_count() { return var_count; }

const char* var_user_name(int idx) {
    return (idx >= 0 && idx < var_count) ? vars[idx].name : nullptr;
}

const char* var_user_value(int idx) {
    return (idx >= 0 && idx < var_count) ? vars[idx].value : nullptr;
}

// ---- Character classification ----

bool is_var_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// ---- Variable expansion ----

void expand_vars(const char* in, char* out, int outMax) {
    int o = 0;
    while (*in && o < outMax - 1) {
        if (*in == '\\' && in[1] == '$') {
            if (o < outMax - 1) out[o++] = '$';
            in += 2;
            continue;
        }
        if (*in == '$') {
            in++;
            char name[32];
            int n = 0;
            if (*in == '{') {
                in++;
                while (*in && *in != '}' && n < 31) name[n++] = *in++;
                if (*in == '}') in++;
            } else if (*in == '?') {
                name[n++] = '?';
                in++;
            } else {
                while (is_var_char(*in) && n < 31) name[n++] = *in++;
            }
            name[n] = '\0';
            if (n > 0) {
                const char* val = var_get(name);
                if (val) {
                    while (*val && o < outMax - 1) out[o++] = *val++;
                }
            } else {
                if (o < outMax - 1) out[o++] = '$';
            }
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

// ---- Tilde expansion ----

void expand_tilde(const char* in, char* out, int outMax) {
    int o = 0;
    bool at_start = true;
    while (*in && o < outMax - 1) {
        if (at_start && *in == '~' && session_home[0]) {
            if (in[1] == '\0' || in[1] == '/' || in[1] == ' ') {
                const char* h = session_home;
                while (*h && o < outMax - 1) out[o++] = *h++;
                in++;
            } else {
                out[o++] = *in++;
            }
        } else {
            at_start = (*in == ' ');
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

// ---- Comment stripping ----

void strip_comment(char* line) {
    bool in_sq = false, in_dq = false;
    for (int i = 0; line[i]; i++) {
        if (line[i] == '\'' && !in_dq) in_sq = !in_sq;
        else if (line[i] == '"' && !in_sq) in_dq = !in_dq;
        else if (line[i] == '#' && !in_sq && !in_dq) {
            line[i] = '\0';
            return;
        }
    }
}
