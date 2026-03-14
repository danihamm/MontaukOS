/*
    * config.h
    * Config file manager for MontaukOS programs
    * Loads, modifies, and saves TOML config files from 0:/config/
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <montauk/toml.h>
#include <montauk/syscall.h>

namespace montauk {
namespace config {

    static constexpr const char* CONFIG_DIR = "0:/config";

    // ---- Serializer (Doc -> TOML text) ----

    namespace detail {

        struct Writer {
            char*  buf;
            int    len;
            int    cap;

            void init() {
                cap = 1024;
                buf = (char*)montauk::malloc(cap);
                len = 0;
                buf[0] = '\0';
            }

            void destroy() {
                if (buf) montauk::mfree(buf);
                buf = nullptr;
                len = 0;
                cap = 0;
            }

            void grow(int need) {
                if (len + need < cap) return;
                int newCap = cap;
                while (newCap <= len + need) newCap *= 2;
                char* nb = (char*)montauk::malloc(newCap);
                montauk::memcpy(nb, buf, len);
                montauk::mfree(buf);
                buf = nb;
                cap = newCap;
            }

            void put(char c) {
                grow(2);
                buf[len++] = c;
                buf[len] = '\0';
            }

            void puts(const char* s) {
                int n = montauk::slen(s);
                grow(n + 1);
                montauk::memcpy(buf + len, s, n);
                len += n;
                buf[len] = '\0';
            }

            void put_int(int64_t v) {
                if (v < 0) { put('-'); v = -v; }
                if (v == 0) { put('0'); return; }
                char tmp[24];
                int n = 0;
                while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
                for (int i = n - 1; i >= 0; i--) put(tmp[i]);
            }

            // Write a TOML-escaped string value
            void put_string(const char* s) {
                put('"');
                while (*s) {
                    switch (*s) {
                        case '"':  puts("\\\""); break;
                        case '\\': puts("\\\\"); break;
                        case '\n': puts("\\n");  break;
                        case '\t': puts("\\t");  break;
                        case '\r': puts("\\r");  break;
                        default:   put(*s);      break;
                    }
                    s++;
                }
                put('"');
            }

            void write_value(toml::Value* v) {
                switch (v->type) {
                    case toml::Type::String:
                        put_string(v->str);
                        break;
                    case toml::Type::Int:
                        put_int(v->ival);
                        break;
                    case toml::Type::Bool:
                        puts(v->bval ? "true" : "false");
                        break;
                    case toml::Type::Array: {
                        put('[');
                        for (int i = 0; i < v->array.count; i++) {
                            if (i > 0) puts(", ");
                            write_value(v->array.items[i]);
                        }
                        put(']');
                        break;
                    }
                    case toml::Type::Table:
                        // Inline tables are not re-serialized here;
                        // their entries are flattened in the doc
                        break;
                }
            }
        };

        // Extract the table prefix from a dotted key (everything before last dot)
        // Returns length of prefix, or 0 if no dot
        inline int key_table(const char* key, char* out, int outSz) {
            int lastDot = -1;
            int n = montauk::slen(key);
            for (int i = 0; i < n; i++) {
                if (key[i] == '.') lastDot = i;
            }
            if (lastDot <= 0) { out[0] = '\0'; return 0; }
            int cp = lastDot < outSz - 1 ? lastDot : outSz - 1;
            montauk::memcpy(out, key, cp);
            out[cp] = '\0';
            return cp;
        }

        // Extract the bare key (after last dot)
        inline const char* key_bare(const char* key) {
            const char* last = key;
            while (*key) {
                if (*key == '.') last = key + 1;
                key++;
            }
            return last;
        }

    } // namespace detail

    // Serialize a Doc back to TOML text. Caller must montauk::mfree() the result.
    inline char* serialize(toml::Doc* doc) {
        detail::Writer w;
        w.init();

        char currentTable[256] = {};

        for (int i = 0; i < doc->entries.count; i++) {
            auto* v = doc->entries.items[i];
            if (!v->key) continue;

            // Skip Table entries that only serve as section markers —
            // we emit [table] headers based on the keys of actual values
            if (v->type == toml::Type::Table && v->array.count == 0)
                continue;

            // Determine table prefix for this key
            char table[256];
            detail::key_table(v->key, table, sizeof(table));

            // Emit [table] header if the section changed
            if (!montauk::streq(table, currentTable)) {
                montauk::strcpy(currentTable, table);
                if (table[0]) {
                    if (w.len > 0) w.put('\n');
                    w.put('[');
                    w.puts(table);
                    w.puts("]\n");
                }
            }

            // Emit key = value
            w.puts(detail::key_bare(v->key));
            w.puts(" = ");
            w.write_value(v);
            w.put('\n');
        }

        return w.buf;
    }

    // ---- File operations ----

    // Ensure the config directory exists
    inline void ensure_dir() {
        montauk::fmkdir(CONFIG_DIR);
    }

    // Build full path: "0:/config/<name>.toml"
    inline void build_path(char* out, int outSz, const char* name) {
        int p = 0;
        const char* dir = CONFIG_DIR;
        while (*dir && p < outSz - 2) out[p++] = *dir++;
        out[p++] = '/';
        while (*name && p < outSz - 6) out[p++] = *name++;
        // Append ".toml"
        const char* ext = ".toml";
        while (*ext && p < outSz - 1) out[p++] = *ext++;
        out[p] = '\0';
    }

    // Load a config file by name (without extension).
    // Returns an initialized Doc (empty if file doesn't exist).
    inline toml::Doc load(const char* name) {
        char path[128];
        build_path(path, sizeof(path), name);

        int handle = montauk::open(path);
        if (handle < 0) {
            toml::Doc doc;
            doc.init();
            return doc;
        }

        uint64_t size = montauk::getsize(handle);
        if (size == 0) {
            montauk::close(handle);
            toml::Doc doc;
            doc.init();
            return doc;
        }

        char* text = (char*)montauk::malloc(size + 1);
        montauk::read(handle, (uint8_t*)text, 0, size);
        montauk::close(handle);
        text[size] = '\0';

        toml::Doc doc = toml::parse(text);
        montauk::mfree(text);
        return doc;
    }

    // Save a Doc to disk as a TOML file.
    // Creates the file if it doesn't exist.
    // Returns 0 on success, negative on error.
    inline int save(const char* name, toml::Doc* doc) {
        ensure_dir();

        char path[128];
        build_path(path, sizeof(path), name);

        char* text = serialize(doc);
        int textLen = montauk::slen(text);

        // Delete and recreate to ensure file is exactly the right size
        // (no ftruncate syscall available, so this avoids stale trailing data)
        montauk::fdelete(path);
        int handle = montauk::fcreate(path);
        if (handle < 0) {
            montauk::mfree(text);
            return -1;
        }

        int ret = montauk::fwrite(handle, (const uint8_t*)text, 0, textLen);
        montauk::close(handle);
        montauk::mfree(text);
        return ret < 0 ? ret : 0;
    }

    // ---- Per-user config ----

    // Build path: "0:/users/<username>/config/<name>.toml"
    inline void build_user_path(char* out, int outSz, const char* username, const char* name) {
        int p = 0;
        const char* prefix = "0:/users/";
        while (*prefix && p < outSz - 2) out[p++] = *prefix++;
        while (*username && p < outSz - 2) out[p++] = *username++;
        const char* mid = "/config/";
        while (*mid && p < outSz - 2) out[p++] = *mid++;
        while (*name && p < outSz - 6) out[p++] = *name++;
        const char* ext = ".toml";
        while (*ext && p < outSz - 1) out[p++] = *ext++;
        out[p] = '\0';
    }

    // Ensure per-user config directory exists
    inline void ensure_user_dir(const char* username) {
        char dir[128];
        int p = 0;
        const char* prefix = "0:/users/";
        while (*prefix && p < 126) dir[p++] = *prefix++;
        while (*username && p < 126) dir[p++] = *username++;
        dir[p] = '\0';
        montauk::fmkdir(dir);

        const char* suffix = "/config";
        while (*suffix && p < 126) dir[p++] = *suffix++;
        dir[p] = '\0';
        montauk::fmkdir(dir);
    }

    // Load a per-user config file
    inline toml::Doc load_user(const char* username, const char* name) {
        char path[192];
        build_user_path(path, sizeof(path), username, name);

        int handle = montauk::open(path);
        if (handle < 0) {
            toml::Doc doc;
            doc.init();
            return doc;
        }

        uint64_t size = montauk::getsize(handle);
        if (size == 0) {
            montauk::close(handle);
            toml::Doc doc;
            doc.init();
            return doc;
        }

        char* text = (char*)montauk::malloc(size + 1);
        montauk::read(handle, (uint8_t*)text, 0, size);
        montauk::close(handle);
        text[size] = '\0';

        toml::Doc doc = toml::parse(text);
        montauk::mfree(text);
        return doc;
    }

    // Save a per-user config file
    inline int save_user(const char* username, const char* name, toml::Doc* doc) {
        ensure_user_dir(username);

        char path[192];
        build_user_path(path, sizeof(path), username, name);

        char* text = serialize(doc);
        int textLen = montauk::slen(text);

        montauk::fdelete(path);
        int handle = montauk::fcreate(path);
        if (handle < 0) {
            montauk::mfree(text);
            return -1;
        }

        int ret = montauk::fwrite(handle, (const uint8_t*)text, 0, textLen);
        montauk::close(handle);
        montauk::mfree(text);
        return ret < 0 ? ret : 0;
    }

    // Delete a config file. Returns 0 on success.
    inline int remove(const char* name) {
        char path[128];
        build_path(path, sizeof(path), name);
        return montauk::fdelete(path);
    }

    // ---- In-place modification helpers ----

    // Free any dynamically allocated data owned by a value (without freeing key)
    namespace detail {
        inline void free_value_data(toml::Value* v) {
            if (v->type == toml::Type::String && v->str)
                montauk::mfree(v->str);
            else if (v->type == toml::Type::Array || v->type == toml::Type::Table) {
                for (int i = 0; i < v->array.count; i++) {
                    if (v->array.items[i]) v->array.items[i]->destroy();
                }
                if (v->array.items) montauk::mfree(v->array.items);
                v->array.items = nullptr;
                v->array.count = 0;
                v->array.cap = 0;
            }
        }
    }

    // Set or overwrite a string value in the doc.
    inline void set_string(toml::Doc* doc, const char* key, const char* val) {
        auto* existing = doc->get(key);
        if (existing) {
            detail::free_value_data(existing);
            existing->type = toml::Type::String;
            existing->str = toml::Value::dup(val);
        } else {
            doc->entries.push(toml::Value::make_string(key, val, montauk::slen(val)));
        }
    }

    // Set or overwrite an integer value in the doc.
    inline void set_int(toml::Doc* doc, const char* key, int64_t val) {
        auto* existing = doc->get(key);
        if (existing) {
            detail::free_value_data(existing);
            existing->type = toml::Type::Int;
            existing->ival = val;
        } else {
            doc->entries.push(toml::Value::make_int(key, val));
        }
    }

    // Set or overwrite a boolean value in the doc.
    inline void set_bool(toml::Doc* doc, const char* key, bool val) {
        auto* existing = doc->get(key);
        if (existing) {
            detail::free_value_data(existing);
            existing->type = toml::Type::Bool;
            existing->bval = val;
        } else {
            doc->entries.push(toml::Value::make_bool(key, val));
        }
    }

    // Remove a key from the doc.
    inline bool unset(toml::Doc* doc, const char* key) {
        for (int i = 0; i < doc->entries.count; i++) {
            auto* v = doc->entries.items[i];
            if (v->key && montauk::streq(v->key, key)) {
                v->destroy();
                // Shift remaining entries down
                for (int j = i; j < doc->entries.count - 1; j++)
                    doc->entries.items[j] = doc->entries.items[j + 1];
                doc->entries.count--;
                return true;
            }
        }
        return false;
    }

} // namespace config
} // namespace montauk
