/*
    * syntax_ansi.hpp
    * ANSI terminal syntax highlighting for C and Lua
    * Used by the MontaukOS CLI text editor
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once

#include <stdint.h>

// ============================================================================
// Token types
// ============================================================================

enum SynLanguage : uint8_t {
    SYN_LANG_NONE,
    SYN_LANG_C,
    SYN_LANG_LUA,
};

enum SynToken : uint8_t {
    SYN_NORMAL,
    SYN_KEYWORD,
    SYN_TYPE,
    SYN_PREPROCESSOR,
    SYN_STRING,
    SYN_CHAR,
    SYN_COMMENT,
    SYN_NUMBER,
    SYN_OPERATOR,
};

struct SynState {
    int in_block_comment;    // 0 or 1 (bool stored as int for freestanding)
    SynToken long_token;
    int long_bracket_eqs;
};

// ============================================================================
// Helpers
// ============================================================================

inline bool syn_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool syn_is_alnum(char c) {
    return syn_is_alpha(c) || (c >= '0' && c <= '9');
}

inline bool syn_is_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool syn_is_hex(char c) {
    return syn_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline void syn_set_token(SynToken* out, int out_len, int idx, SynToken tok) {
    if (out && idx >= 0 && idx < out_len)
        out[idx] = tok;
}

inline void syn_fill_tokens(SynToken* out, int out_len, int start, int end, SynToken tok) {
    if (!out) return;
    if (start < 0) start = 0;
    if (end > out_len) end = out_len;
    for (int i = start; i < end; i++)
        out[i] = tok;
}

inline bool syn_streq(const char* buf, int len, const char* kw) {
    int i = 0;
    while (i < len && kw[i]) {
        if (buf[i] != kw[i]) return false;
        i++;
    }
    return i == len && kw[i] == '\0';
}

// ============================================================================
// Keyword / type classification
// ============================================================================

inline SynToken syn_classify_c_word(const char* buf, int len) {
    static const char* keywords[] = {
        "auto", "break", "case", "const", "continue", "default", "do",
        "else", "enum", "extern", "for", "goto", "if", "inline",
        "register", "restrict", "return", "sizeof", "static", "struct",
        "switch", "typedef", "union", "volatile", "while",
        "NULL", "true", "false", "nullptr",
    };
    static const char* types[] = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "bool", "_Bool",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
        "FILE",
    };
    for (int i = 0; i < (int)(sizeof(keywords) / sizeof(keywords[0])); i++) {
        if (syn_streq(buf, len, keywords[i])) return SYN_KEYWORD;
    }
    for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++) {
        if (syn_streq(buf, len, types[i])) return SYN_TYPE;
    }
    return SYN_NORMAL;
}

inline SynToken syn_classify_lua_word(const char* buf, int len) {
    static const char* keywords[] = {
        "and", "break", "do", "else", "elseif", "end", "false",
        "for", "function", "goto", "if", "in", "local", "nil",
        "not", "or", "repeat", "return", "then", "true",
        "until", "while",
    };
    static const char* builtins[] = {
        "_ENV", "_G", "_VERSION",
        "assert", "collectgarbage", "coroutine", "debug", "dofile",
        "error", "getmetatable", "io", "ipairs", "load", "loadfile",
        "math", "next", "os", "package", "pairs", "pcall",
        "print", "rawequal", "rawget", "rawlen", "rawset", "require",
        "select", "setmetatable", "string", "table", "tonumber",
        "tostring", "type", "utf8", "warn", "xpcall",
    };
    for (int i = 0; i < (int)(sizeof(keywords) / sizeof(keywords[0])); i++) {
        if (syn_streq(buf, len, keywords[i])) return SYN_KEYWORD;
    }
    for (int i = 0; i < (int)(sizeof(builtins) / sizeof(builtins[0])); i++) {
        if (syn_streq(buf, len, builtins[i])) return SYN_TYPE;
    }
    return SYN_NORMAL;
}

inline SynToken syn_classify_word(SynLanguage lang, const char* buf, int len) {
    switch (lang) {
    case SYN_LANG_C:   return syn_classify_c_word(buf, len);
    case SYN_LANG_LUA: return syn_classify_lua_word(buf, len);
    default:           return SYN_NORMAL;
    }
}

inline SynState syn_make_state() {
    SynState state = {};
    state.in_block_comment = 0;
    state.long_token = SYN_NORMAL;
    state.long_bracket_eqs = -1;
    return state;
}

// ============================================================================
// Lua long bracket matching
// ============================================================================

inline bool syn_match_lua_long_bracket_open(const char* line, int len, int i,
                                            int& eqs, int& span) {
    if (i >= len || line[i] != '[') return false;
    int j = i + 1;
    while (j < len && line[j] == '=') j++;
    if (j < len && line[j] == '[') {
        eqs = j - i - 1;
        span = j - i + 1;
        return true;
    }
    return false;
}

inline bool syn_match_lua_long_bracket_close(const char* line, int len, int i,
                                             int eqs, int& span) {
    if (i >= len || line[i] != ']') return false;
    int j = i + 1;
    for (int k = 0; k < eqs; k++) {
        if (j >= len || line[j] != '=') return false;
        j++;
    }
    if (j < len && line[j] == ']') {
        span = j - i + 1;
        return true;
    }
    return false;
}

// ============================================================================
// Number consumption
// ============================================================================

inline void syn_consume_number(const char* line, int len, int& i,
                               SynToken* out, int out_len, int c_style_suffixes) {
    int start = i;
    if (line[i] == '0' && i + 1 < len && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
        i += 2;
        while (i < len && syn_is_hex(line[i])) i++;
        if (i < len && line[i] == '.' && !(i + 1 < len && line[i + 1] == '.')) {
            i++;
            while (i < len && syn_is_hex(line[i])) i++;
        }
        if (i < len && (line[i] == 'p' || line[i] == 'P')) {
            int exp = i + 1;
            if (exp < len && (line[exp] == '+' || line[exp] == '-')) exp++;
            if (exp < len && syn_is_digit(line[exp])) {
                i = exp + 1;
                while (i < len && syn_is_digit(line[i])) i++;
            }
        }
    } else {
        if (line[i] == '.') i++;
        while (i < len && syn_is_digit(line[i])) i++;
        if (i < len && line[i] == '.' && !(i + 1 < len && line[i + 1] == '.')) {
            i++;
            while (i < len && syn_is_digit(line[i])) i++;
        }
        if (i < len && (line[i] == 'e' || line[i] == 'E')) {
            int exp = i + 1;
            if (exp < len && (line[exp] == '+' || line[exp] == '-')) exp++;
            if (exp < len && syn_is_digit(line[exp])) {
                i = exp + 1;
                while (i < len && syn_is_digit(line[i])) i++;
            }
        }
    }
    if (c_style_suffixes) {
        while (i < len && (line[i] == 'u' || line[i] == 'U' ||
                           line[i] == 'l' || line[i] == 'L' ||
                           line[i] == 'f' || line[i] == 'F'))
            i++;
    }
    syn_fill_tokens(out, out_len, start, i, SYN_NUMBER);
}

// ============================================================================
// C highlighting
// ============================================================================

inline void syn_highlight_line_c(const char* line, int len, SynToken* out, int out_len,
                                 SynState& state) {
    int i = 0;
    while (i < len) {
        if (state.in_block_comment) {
            while (i < len) {
                if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                    syn_set_token(out, out_len, i, SYN_COMMENT);
                    syn_set_token(out, out_len, i + 1, SYN_COMMENT);
                    i += 2;
                    state.in_block_comment = 0;
                    break;
                }
                syn_set_token(out, out_len, i, SYN_COMMENT);
                i++;
            }
            continue;
        }
        char c = line[i];
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            syn_fill_tokens(out, out_len, i, len, SYN_COMMENT);
            break;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            state.in_block_comment = 1;
            syn_set_token(out, out_len, i, SYN_COMMENT);
            syn_set_token(out, out_len, i + 1, SYN_COMMENT);
            i += 2;
            continue;
        }
        if (c == '#') {
            int is_pp = 1;
            for (int j = 0; j < i; j++) {
                if (line[j] != ' ' && line[j] != '\t') { is_pp = 0; break; }
            }
            if (is_pp) {
                while (i < len) {
                    if (i + 1 < len && line[i] == '/' && line[i + 1] == '/') {
                        syn_fill_tokens(out, out_len, i, len, SYN_COMMENT);
                        break;
                    }
                    if (i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
                        state.in_block_comment = 1;
                        syn_set_token(out, out_len, i, SYN_COMMENT);
                        syn_set_token(out, out_len, i + 1, SYN_COMMENT);
                        i += 2;
                        while (i < len) {
                            if (i + 1 < len && line[i] == '*' && line[i + 1] == '/') {
                                syn_set_token(out, out_len, i, SYN_COMMENT);
                                syn_set_token(out, out_len, i + 1, SYN_COMMENT);
                                i += 2;
                                state.in_block_comment = 0;
                                break;
                            }
                            syn_set_token(out, out_len, i, SYN_COMMENT);
                            i++;
                        }
                        continue;
                    }
                    syn_set_token(out, out_len, i, SYN_PREPROCESSOR);
                    i++;
                }
                continue;
            }
        }
        if (c == '"') {
            syn_set_token(out, out_len, i, SYN_STRING);
            i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    syn_set_token(out, out_len, i, SYN_STRING);
                    syn_set_token(out, out_len, i + 1, SYN_STRING);
                    i += 2;
                    continue;
                }
                if (line[i] == '"') {
                    syn_set_token(out, out_len, i, SYN_STRING);
                    i++;
                    break;
                }
                syn_set_token(out, out_len, i, SYN_STRING);
                i++;
            }
            continue;
        }
        if (c == '\'') {
            syn_set_token(out, out_len, i, SYN_CHAR);
            i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    syn_set_token(out, out_len, i, SYN_CHAR);
                    syn_set_token(out, out_len, i + 1, SYN_CHAR);
                    i += 2;
                    continue;
                }
                if (line[i] == '\'') {
                    syn_set_token(out, out_len, i, SYN_CHAR);
                    i++;
                    break;
                }
                syn_set_token(out, out_len, i, SYN_CHAR);
                i++;
            }
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < len && syn_is_digit(line[i + 1]))) {
            syn_consume_number(line, len, i, out, out_len, 1);
            continue;
        }
        if (syn_is_alpha(c)) {
            int start = i;
            while (i < len && syn_is_alnum(line[i])) i++;
            SynToken tok = syn_classify_word(SYN_LANG_C, line + start, i - start);
            syn_fill_tokens(out, out_len, start, i, tok);
            continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '=' || c == '!' || c == '<' || c == '>' || c == '&' ||
            c == '|' || c == '^' || c == '~' || c == '?' || c == ':') {
            syn_set_token(out, out_len, i, SYN_OPERATOR);
            i++;
            continue;
        }
        syn_set_token(out, out_len, i, SYN_NORMAL);
        i++;
    }
}

// ============================================================================
// Lua highlighting
// ============================================================================

inline void syn_highlight_line_lua(const char* line, int len, SynToken* out, int out_len,
                                   SynState& state) {
    int i = 0;
    while (i < len) {
        if (state.long_token != SYN_NORMAL) {
            int span = 0;
            if (syn_match_lua_long_bracket_close(line, len, i, state.long_bracket_eqs, span)) {
                syn_fill_tokens(out, out_len, i, i + span, state.long_token);
                i += span;
                state.long_token = SYN_NORMAL;
                state.long_bracket_eqs = -1;
                continue;
            }
            syn_set_token(out, out_len, i, state.long_token);
            i++;
            continue;
        }
        char c = line[i];
        if (c == '-' && i + 1 < len && line[i + 1] == '-') {
            int eqs = 0;
            int span = 0;
            if (i + 2 < len && syn_match_lua_long_bracket_open(line, len, i + 2, eqs, span)) {
                syn_fill_tokens(out, out_len, i, i + 2 + span, SYN_COMMENT);
                i += 2 + span;
                state.long_token = SYN_COMMENT;
                state.long_bracket_eqs = eqs;
                continue;
            }
            syn_fill_tokens(out, out_len, i, len, SYN_COMMENT);
            break;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            syn_set_token(out, out_len, i, SYN_STRING);
            i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    syn_set_token(out, out_len, i, SYN_STRING);
                    syn_set_token(out, out_len, i + 1, SYN_STRING);
                    i += 2;
                    continue;
                }
                syn_set_token(out, out_len, i, SYN_STRING);
                if (line[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        int eqs = 0;
        int span = 0;
        if (syn_match_lua_long_bracket_open(line, len, i, eqs, span)) {
            syn_fill_tokens(out, out_len, i, i + span, SYN_STRING);
            i += span;
            state.long_token = SYN_STRING;
            state.long_bracket_eqs = eqs;
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < len && syn_is_digit(line[i + 1]))) {
            syn_consume_number(line, len, i, out, out_len, 0);
            continue;
        }
        if (syn_is_alpha(c)) {
            int start = i;
            while (i < len && syn_is_alnum(line[i])) i++;
            SynToken tok = syn_classify_word(SYN_LANG_LUA, line + start, i - start);
            syn_fill_tokens(out, out_len, start, i, tok);
            continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '^' || c == '#' || c == '=' || c == '<' || c == '>' ||
            c == '~' || c == ':' || c == '.' || c == ',' || c == ';' ||
            c == '(' || c == ')' || c == '{' || c == '}' ||
            c == '[' || c == ']') {
            syn_set_token(out, out_len, i, SYN_OPERATOR);
            i++;
            continue;
        }
        syn_set_token(out, out_len, i, SYN_NORMAL);
        i++;
    }
}

// ============================================================================
// Per-line highlighter
// ============================================================================

inline void syn_highlight_line(const char* line, int len, SynToken* out, int out_len,
                               SynLanguage lang, SynState& state) {
    if (!line || len <= 0) return;
    switch (lang) {
    case SYN_LANG_C:
        syn_highlight_line_c(line, len, out, out_len, state);
        break;
    case SYN_LANG_LUA:
        syn_highlight_line_lua(line, len, out, out_len, state);
        break;
    default:
        syn_fill_tokens(out, out_len, 0, len, SYN_NORMAL);
        break;
    }
}

// ============================================================================
// Language detection
// ============================================================================

inline bool syn_path_ends_with(const char* path, const char* suffix) {
    if (!path || !suffix) return false;
    int path_len = 0;
    while (path[path_len]) path_len++;
    int suffix_len = 0;
    while (suffix[suffix_len]) suffix_len++;
    if (path_len < suffix_len) return false;
    for (int i = 0; i < suffix_len; i++) {
        if (path[path_len - suffix_len + i] != suffix[i])
            return false;
    }
    return true;
}

inline SynLanguage syn_detect_language(const char* filepath) {
    if (!filepath || filepath[0] == '\0') return SYN_LANG_NONE;
    if (syn_path_ends_with(filepath, ".c") || syn_path_ends_with(filepath, ".h"))
        return SYN_LANG_C;
    if (syn_path_ends_with(filepath, ".lua"))
        return SYN_LANG_LUA;
    return SYN_LANG_NONE;
}