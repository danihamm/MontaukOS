/*
    * toml.h
    * TOML config file parser
    * Supports: strings, integers, booleans, tables, arrays
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <montauk/heap.h>
#include <montauk/string.h>
#include <cstdint>

namespace montauk {
namespace toml {

    enum class Type { String, Int, Bool, Array, Table };

    struct Value;

    // Dynamic array of Value pointers (for arrays and table entries)
    struct ValueList {
        Value** items;
        int     count;
        int     cap;

        void init() { items = nullptr; count = 0; cap = 0; }

        void push(Value* v) {
            if (count >= cap) {
                int newCap = cap ? cap * 2 : 8;
                auto** buf = (Value**)montauk::malloc(newCap * sizeof(Value*));
                if (items) {
                    montauk::memcpy(buf, items, count * sizeof(Value*));
                    montauk::mfree(items);
                }
                items = buf;
                cap = newCap;
            }
            items[count++] = v;
        }

        void destroy();   // forward — defined after Value
    };

    struct Value {
        Type  type;
        char* key;        // owned, dotted path (e.g. "server.port"); null for array elements

        union {
            char*     str;       // Type::String  — owned
            int64_t   ival;      // Type::Int
            bool      bval;     // Type::Bool
        };
        ValueList array;         // Type::Array elements or Type::Table entries

        static Value* make_string(const char* k, const char* s, int slen) {
            auto* v = (Value*)montauk::malloc(sizeof(Value));
            v->type = Type::String;
            v->key = dup(k);
            v->str = dupn(s, slen);
            v->array.init();
            return v;
        }

        static Value* make_int(const char* k, int64_t n) {
            auto* v = (Value*)montauk::malloc(sizeof(Value));
            v->type = Type::Int;
            v->key = dup(k);
            v->ival = n;
            v->array.init();
            return v;
        }

        static Value* make_bool(const char* k, bool b) {
            auto* v = (Value*)montauk::malloc(sizeof(Value));
            v->type = Type::Bool;
            v->key = dup(k);
            v->bval = b;
            v->array.init();
            return v;
        }

        static Value* make_array(const char* k) {
            auto* v = (Value*)montauk::malloc(sizeof(Value));
            v->type = Type::Array;
            v->key = dup(k);
            v->ival = 0;
            v->array.init();
            return v;
        }

        static Value* make_table(const char* k) {
            auto* v = (Value*)montauk::malloc(sizeof(Value));
            v->type = Type::Table;
            v->key = dup(k);
            v->ival = 0;
            v->array.init();
            return v;
        }

        void destroy() {
            if (key) montauk::mfree(key);
            if (type == Type::String && str) montauk::mfree(str);
            array.destroy();
            montauk::mfree(this);
        }

        // helpers
        static char* dup(const char* s) {
            if (!s) return nullptr;
            int n = montauk::slen(s);
            return dupn(s, n);
        }
        static char* dupn(const char* s, int n) {
            char* d = (char*)montauk::malloc(n + 1);
            montauk::memcpy(d, s, n);
            d[n] = '\0';
            return d;
        }
    };

    inline void ValueList::destroy() {
        for (int i = 0; i < count; i++) items[i]->destroy();
        if (items) montauk::mfree(items);
        items = nullptr;
        count = 0;
        cap = 0;
    }

    // ---- Document ----

    struct Doc {
        ValueList entries;    // flat list of all top-level and nested values

        void init() { entries.init(); }

        void destroy() {
            entries.destroy();
        }

        // Lookup by dotted key path (e.g. "server.port")
        Value* get(const char* key) const {
            for (int i = 0; i < entries.count; i++) {
                if (entries.items[i]->key && montauk::streq(entries.items[i]->key, key))
                    return entries.items[i];
            }
            return nullptr;
        }

        // Typed accessors with defaults
        const char* get_string(const char* key, const char* def = "") const {
            auto* v = get(key);
            return (v && v->type == Type::String) ? v->str : def;
        }

        int64_t get_int(const char* key, int64_t def = 0) const {
            auto* v = get(key);
            return (v && v->type == Type::Int) ? v->ival : def;
        }

        bool get_bool(const char* key, bool def = false) const {
            auto* v = get(key);
            return (v && v->type == Type::Bool) ? v->bval : def;
        }

        Value* get_array(const char* key) const {
            auto* v = get(key);
            return (v && v->type == Type::Array) ? v : nullptr;
        }

        Value* get_table(const char* key) const {
            auto* v = get(key);
            return (v && v->type == Type::Table) ? v : nullptr;
        }
    };

    // ---- Parser ----

    namespace detail {

        struct Parser {
            const char* src;
            int         pos;
            int         len;
            char        table_prefix[256];  // current [table] prefix

            char peek()  const { return pos < len ? src[pos] : '\0'; }
            char next()         { return pos < len ? src[pos++] : '\0'; }
            bool at_end() const { return pos >= len; }

            void skip_ws() {
                while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
            }

            void skip_line() {
                while (pos < len && src[pos] != '\n') pos++;
                if (pos < len) pos++;   // consume newline
            }

            void skip_ws_and_newlines() {
                while (pos < len) {
                    char c = src[pos];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                        pos++;
                    else if (c == '#')
                        skip_line();
                    else
                        break;
                }
            }

            // Build dotted key: prefix.key
            void build_key(char* out, int outSz, const char* key, int keyLen) {
                int p = 0;
                if (table_prefix[0]) {
                    int plen = montauk::slen(table_prefix);
                    for (int i = 0; i < plen && p < outSz - 2; i++) out[p++] = table_prefix[i];
                    out[p++] = '.';
                }
                for (int i = 0; i < keyLen && p < outSz - 1; i++) out[p++] = key[i];
                out[p] = '\0';
            }

            // Parse a bare key or quoted key, returns length
            int parse_key(char* buf, int bufSz) {
                skip_ws();
                int n = 0;
                if (peek() == '"') {
                    next(); // consume opening quote
                    while (!at_end() && peek() != '"' && n < bufSz - 1)
                        buf[n++] = next();
                    if (peek() == '"') next();
                } else {
                    while (!at_end()) {
                        char c = peek();
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-') {
                            buf[n++] = next();
                            if (n >= bufSz - 1) break;
                        } else {
                            break;
                        }
                    }
                }
                buf[n] = '\0';
                return n;
            }

            // Parse a dotted key (e.g. server.host), returns full length written to buf
            int parse_dotted_key(char* buf, int bufSz) {
                int total = 0;
                // Parse first segment
                char seg[128];
                int segLen = parse_key(seg, sizeof(seg));
                for (int i = 0; i < segLen && total < bufSz - 1; i++)
                    buf[total++] = seg[i];

                // Parse additional dotted segments
                while (peek() == '.') {
                    next(); // consume dot
                    if (total < bufSz - 1) buf[total++] = '.';
                    segLen = parse_key(seg, sizeof(seg));
                    for (int i = 0; i < segLen && total < bufSz - 1; i++)
                        buf[total++] = seg[i];
                }
                buf[total] = '\0';
                return total;
            }

            int64_t parse_integer() {
                bool neg = false;
                if (peek() == '-') { neg = true; next(); }
                else if (peek() == '+') { next(); }

                // Hex, octal, binary
                if (peek() == '0' && pos + 1 < len) {
                    char c2 = src[pos + 1];
                    if (c2 == 'x' || c2 == 'X') {
                        pos += 2;
                        int64_t val = 0;
                        while (!at_end()) {
                            char c = peek();
                            if (c == '_') { next(); continue; }
                            int d = -1;
                            if (c >= '0' && c <= '9') d = c - '0';
                            else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
                            else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
                            if (d < 0) break;
                            val = val * 16 + d;
                            next();
                        }
                        return neg ? -val : val;
                    }
                    if (c2 == 'o') {
                        pos += 2;
                        int64_t val = 0;
                        while (!at_end()) {
                            char c = peek();
                            if (c == '_') { next(); continue; }
                            if (c < '0' || c > '7') break;
                            val = val * 8 + (c - '0');
                            next();
                        }
                        return neg ? -val : val;
                    }
                    if (c2 == 'b') {
                        pos += 2;
                        int64_t val = 0;
                        while (!at_end()) {
                            char c = peek();
                            if (c == '_') { next(); continue; }
                            if (c != '0' && c != '1') break;
                            val = val * 2 + (c - '0');
                            next();
                        }
                        return neg ? -val : val;
                    }
                }

                int64_t val = 0;
                while (!at_end()) {
                    char c = peek();
                    if (c == '_') { next(); continue; }
                    if (c < '0' || c > '9') break;
                    val = val * 10 + (c - '0');
                    next();
                }
                return neg ? -val : val;
            }

            // Parse string value (opening quote already detected but not consumed)
            // Returns heap-allocated string
            char* parse_string_value(int* outLen) {
                char quote = next(); // consume opening " or '
                bool literal = (quote == '\'');

                // Multi-line? (""" or ''')
                bool multi = false;
                if (pos + 1 < len && src[pos] == quote && src[pos + 1] == quote) {
                    pos += 2;
                    multi = true;
                    // Skip first newline after opening delimiter
                    if (peek() == '\r') next();
                    if (peek() == '\n') next();
                }

                // Collect into temp buffer
                char buf[4096];
                int n = 0;

                while (!at_end() && n < (int)sizeof(buf) - 1) {
                    if (multi) {
                        // Check for closing triple-quote
                        if (pos + 2 < len && src[pos] == quote &&
                            src[pos+1] == quote && src[pos+2] == quote) {
                            pos += 3;
                            break;
                        }
                    } else {
                        if (peek() == quote) { next(); break; }
                        if (peek() == '\n') break; // unterminated single-line
                    }

                    if (!literal && peek() == '\\') {
                        next(); // consume backslash
                        char esc = next();
                        switch (esc) {
                            case 'n':  buf[n++] = '\n'; break;
                            case 't':  buf[n++] = '\t'; break;
                            case 'r':  buf[n++] = '\r'; break;
                            case '\\': buf[n++] = '\\'; break;
                            case '"':  buf[n++] = '"';  break;
                            case '\r':
                                if (peek() == '\n') next();
                                skip_ws();
                                break;
                            case '\n':
                                skip_ws();
                                break;
                            default:   buf[n++] = esc;  break;
                        }
                    } else {
                        buf[n++] = next();
                    }
                }

                *outLen = n;
                return Value::dupn(buf, n);
            }

            Value* parse_value(const char* fullKey) {
                skip_ws();
                char c = peek();

                // String
                if (c == '"' || c == '\'') {
                    int slen = 0;
                    char* s = parse_string_value(&slen);
                    auto* v = Value::make_string(fullKey, s, slen);
                    montauk::mfree(s);
                    return v;
                }

                // Boolean
                if (montauk::starts_with(src + pos, "true")) {
                    pos += 4;
                    return Value::make_bool(fullKey, true);
                }
                if (montauk::starts_with(src + pos, "false")) {
                    pos += 5;
                    return Value::make_bool(fullKey, false);
                }

                // Array
                if (c == '[') {
                    next(); // consume [
                    auto* arr = Value::make_array(fullKey);
                    skip_ws_and_newlines();
                    while (!at_end() && peek() != ']') {
                        auto* elem = parse_value(nullptr);
                        if (elem) arr->array.push(elem);
                        skip_ws_and_newlines();
                        if (peek() == ',') next();
                        skip_ws_and_newlines();
                    }
                    if (peek() == ']') next();
                    return arr;
                }

                // Inline table
                if (c == '{') {
                    next(); // consume {
                    auto* tbl = Value::make_table(fullKey);
                    skip_ws_and_newlines();
                    while (!at_end() && peek() != '}') {
                        char key[128];
                        int keyLen = parse_dotted_key(key, sizeof(key));
                        if (keyLen == 0) { skip_line(); continue; }
                        skip_ws();
                        if (peek() == '=') next();
                        skip_ws();

                        // Build full dotted key for inline table entry
                        char entryKey[256];
                        int p = 0;
                        if (fullKey) {
                            int flen = montauk::slen(fullKey);
                            for (int i = 0; i < flen && p < 254; i++) entryKey[p++] = fullKey[i];
                            entryKey[p++] = '.';
                        }
                        for (int i = 0; i < keyLen && p < 255; i++) entryKey[p++] = key[i];
                        entryKey[p] = '\0';

                        auto* val = parse_value(entryKey);
                        if (val) tbl->array.push(val);
                        skip_ws();
                        if (peek() == ',') next();
                        skip_ws();
                    }
                    if (peek() == '}') next();
                    return tbl;
                }

                // Integer (includes negative)
                if ((c >= '0' && c <= '9') || c == '+' || c == '-') {
                    int64_t n = parse_integer();
                    return Value::make_int(fullKey, n);
                }

                // Unknown — skip the rest of the line
                skip_line();
                return nullptr;
            }

            void parse(Doc* doc) {
                table_prefix[0] = '\0';

                while (!at_end()) {
                    skip_ws_and_newlines();
                    if (at_end()) break;

                    char c = peek();

                    // Table header: [name] or [name.sub]
                    if (c == '[') {
                        next();
                        bool array_table = false;
                        if (peek() == '[') { next(); array_table = true; }

                        skip_ws();
                        int plen = parse_dotted_key(table_prefix, sizeof(table_prefix));
                        skip_ws();
                        if (peek() == ']') next();
                        if (array_table && peek() == ']') next();

                        // Register the table itself
                        auto* tbl = Value::make_table(table_prefix);
                        doc->entries.push(tbl);

                        skip_line();
                        continue;
                    }

                    // Key = value
                    char key[128];
                    int keyLen = parse_dotted_key(key, sizeof(key));
                    if (keyLen == 0) { skip_line(); continue; }

                    skip_ws();
                    if (peek() != '=') { skip_line(); continue; }
                    next(); // consume =
                    skip_ws();

                    char fullKey[256];
                    build_key(fullKey, sizeof(fullKey), key, keyLen);

                    Value* val = parse_value(fullKey);
                    if (val) {
                        doc->entries.push(val);

                        // For inline tables, also flatten entries into doc
                        if (val->type == Type::Table) {
                            for (int i = 0; i < val->array.count; i++) {
                                // Clone entry into doc's flat list so dotted lookup works
                                auto* e = val->array.items[i];
                                Value* clone = nullptr;
                                if (e->type == Type::String)
                                    clone = Value::make_string(e->key, e->str, montauk::slen(e->str));
                                else if (e->type == Type::Int)
                                    clone = Value::make_int(e->key, e->ival);
                                else if (e->type == Type::Bool)
                                    clone = Value::make_bool(e->key, e->bval);
                                if (clone) doc->entries.push(clone);
                            }
                        }
                    }

                    // Consume rest of line (trailing comment, etc.)
                    skip_ws();
                    if (peek() == '#') skip_line();
                    else if (peek() == '\r' || peek() == '\n') {
                        if (peek() == '\r') next();
                        if (peek() == '\n') next();
                    }
                }
            }
        };

    } // namespace detail

    // ---- Public API ----

    // Parse a TOML string into a Doc. Caller must call doc.destroy() when done.
    inline Doc parse(const char* text) {
        Doc doc;
        doc.init();

        detail::Parser p;
        p.src = text;
        p.pos = 0;
        p.len = montauk::slen(text);
        p.table_prefix[0] = '\0';
        p.parse(&doc);

        return doc;
    }

    // Parse from a file descriptor (reads entire file into buffer first).
    // Requires montauk::syscall read/fstat or similar — omitted here for portability.
    // Users can read a file into a buffer and call parse() directly.

} // namespace toml
} // namespace montauk
