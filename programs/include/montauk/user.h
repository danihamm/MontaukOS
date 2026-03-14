/*
    * user.h
    * User database, password hashing, and authentication for MontaukOS
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>
#include <montauk/config.h>
#include <bearssl_hash.h>

namespace montauk {
namespace user {

    static constexpr int MAX_USERS = 16;

    struct UserInfo {
        char username[32];
        char display_name[64];
        char password_hash[65];  // 64 hex chars + null
        char salt[33];           // 32 hex chars + null
        char role[16];
    };

    // ---- String helpers ----

    inline void str_cat(char* dst, const char* src, int max) {
        int len = montauk::slen(dst);
        int i = 0;
        while (src[i] && len < max - 1) {
            dst[len++] = src[i++];
        }
        dst[len] = '\0';
    }

    inline void build_user_key(char* out, int sz, const char* username, const char* field) {
        int p = 0;
        const char* prefix = "users.";
        while (*prefix && p < sz - 1) out[p++] = *prefix++;
        while (*username && p < sz - 1) out[p++] = *username++;
        if (p < sz - 1) out[p++] = '.';
        while (*field && p < sz - 1) out[p++] = *field++;
        out[p] = '\0';
    }

    // ---- Path helpers ----

    inline void home_dir(const char* username, char* out, int sz) {
        int p = 0;
        const char* prefix = "0:/users/";
        while (*prefix && p < sz - 1) out[p++] = *prefix++;
        while (*username && p < sz - 1) out[p++] = *username++;
        out[p] = '\0';
    }

    inline void config_dir(const char* username, char* out, int sz) {
        home_dir(username, out, sz);
        str_cat(out, "/config", sz);
    }

    // ---- Hex conversion helpers ----

    inline void bytes_to_hex(const uint8_t* bytes, int len, char* out) {
        const char* digits = "0123456789abcdef";
        for (int i = 0; i < len; i++) {
            out[i * 2]     = digits[(bytes[i] >> 4) & 0x0F];
            out[i * 2 + 1] = digits[bytes[i] & 0x0F];
        }
        out[len * 2] = '\0';
    }

    inline int hex_char_val(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    }

    inline int hex_to_bytes(const char* hex, uint8_t* out, int maxBytes) {
        int i = 0;
        while (hex[i * 2] && hex[i * 2 + 1] && i < maxBytes) {
            int hi = hex_char_val(hex[i * 2]);
            int lo = hex_char_val(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return i;
            out[i] = (uint8_t)((hi << 4) | lo);
            i++;
        }
        return i;
    }

    // ---- Password hashing (SHA-256) ----

    inline void hash_password(const char* password, const char* salt_hex, char* out_hex64) {
        uint8_t salt_bytes[16];
        int salt_len = hex_to_bytes(salt_hex, salt_bytes, 16);
        int pass_len = montauk::slen(password);

        br_sha256_context ctx;
        br_sha256_init(&ctx);
        br_sha256_update(&ctx, salt_bytes, salt_len);
        br_sha256_update(&ctx, password, pass_len);

        uint8_t hash[32];
        br_sha256_out(&ctx, hash);
        bytes_to_hex(hash, 32, out_hex64);
    }

    inline bool verify_password(const char* password, const char* salt_hex, const char* stored_hex64) {
        char computed[65];
        hash_password(password, salt_hex, computed);

        int diff = 0;
        for (int i = 0; i < 64; i++) {
            diff |= computed[i] ^ stored_hex64[i];
        }
        return diff == 0;
    }

    // ---- User database I/O ----

    inline int load_users(UserInfo* buf, int max) {
        auto doc = config::load("users");
        int count = 0;

        for (int i = 0; i < doc.entries.count && count < max; i++) {
            auto* v = doc.entries.items[i];
            if (!v->key) continue;

            if (!montauk::starts_with(v->key, "users.")) continue;
            const char* after = v->key + 6;

            int dot = -1;
            for (int j = 0; after[j]; j++) {
                if (after[j] == '.') { dot = j; break; }
            }
            if (dot <= 0) continue;

            if (!montauk::streq(after + dot + 1, "display_name")) continue;

            UserInfo* u = &buf[count];
            montauk::memset(u, 0, sizeof(UserInfo));
            int ulen = dot < 31 ? dot : 31;
            montauk::memcpy(u->username, after, ulen);
            u->username[ulen] = '\0';

            if (v->type == montauk::toml::Type::String && v->str)
                montauk::strncpy(u->display_name, v->str, sizeof(u->display_name));

            char prefix[64];
            int plen = 0;
            const char* pfx = "users.";
            while (*pfx) prefix[plen++] = *pfx++;
            for (int j = 0; j < ulen; j++) prefix[plen++] = u->username[j];
            prefix[plen] = '\0';

            char key[96];

            montauk::strcpy(key, prefix);
            str_cat(key, ".password_hash", 96);
            const char* ph = doc.get_string(key, "");
            montauk::strncpy(u->password_hash, ph, sizeof(u->password_hash));

            montauk::strcpy(key, prefix);
            str_cat(key, ".salt", 96);
            const char* s = doc.get_string(key, "");
            montauk::strncpy(u->salt, s, sizeof(u->salt));

            montauk::strcpy(key, prefix);
            str_cat(key, ".role", 96);
            const char* r = doc.get_string(key, "user");
            montauk::strncpy(u->role, r, sizeof(u->role));

            count++;
        }

        doc.destroy();
        return count;
    }

    inline int save_users(const UserInfo* buf, int count) {
        montauk::toml::Doc doc;
        doc.init();

        for (int i = 0; i < count; i++) {
            const UserInfo* u = &buf[i];
            char key[96];

            build_user_key(key, 96, u->username, "display_name");
            config::set_string(&doc, key, u->display_name);

            build_user_key(key, 96, u->username, "password_hash");
            config::set_string(&doc, key, u->password_hash);

            build_user_key(key, 96, u->username, "salt");
            config::set_string(&doc, key, u->salt);

            build_user_key(key, 96, u->username, "role");
            config::set_string(&doc, key, u->role);
        }

        int ret = config::save("users", &doc);
        doc.destroy();
        return ret;
    }

    // ---- Authentication ----

    inline bool authenticate(const char* username, const char* password) {
        UserInfo users[MAX_USERS];
        int count = load_users(users, MAX_USERS);

        for (int i = 0; i < count; i++) {
            if (montauk::streq(users[i].username, username)) {
                return verify_password(password, users[i].salt, users[i].password_hash);
            }
        }
        return false;
    }

    // ---- User management ----

    inline bool create_user(const char* username, const char* display_name,
                            const char* password, const char* role) {
        UserInfo users[MAX_USERS];
        int count = load_users(users, MAX_USERS);

        for (int i = 0; i < count; i++) {
            if (montauk::streq(users[i].username, username)) return false;
        }
        if (count >= MAX_USERS) return false;

        UserInfo* u = &users[count];
        montauk::memset(u, 0, sizeof(UserInfo));
        montauk::strncpy(u->username, username, 31);
        montauk::strncpy(u->display_name, display_name, 63);
        montauk::strncpy(u->role, role, 15);

        uint8_t salt_bytes[16];
        montauk::getrandom(salt_bytes, 16);
        bytes_to_hex(salt_bytes, 16, u->salt);

        hash_password(password, u->salt, u->password_hash);

        count++;
        save_users(users, count);

        // Create home directory structure
        char dir[128];
        home_dir(username, dir, sizeof(dir));
        montauk::fmkdir(dir);

        char subdir[128];
        montauk::strcpy(subdir, dir);
        str_cat(subdir, "/config", 128);
        montauk::fmkdir(subdir);

        return true;
    }

    inline bool delete_user(const char* username) {
        UserInfo users[MAX_USERS];
        int count = load_users(users, MAX_USERS);

        int idx = -1;
        for (int i = 0; i < count; i++) {
            if (montauk::streq(users[i].username, username)) { idx = i; break; }
        }
        if (idx < 0) return false;

        for (int i = idx; i < count - 1; i++) {
            users[i] = users[i + 1];
        }
        count--;

        save_users(users, count);
        return true;
    }

    inline bool change_password(const char* username, const char* new_password) {
        UserInfo users[MAX_USERS];
        int count = load_users(users, MAX_USERS);

        for (int i = 0; i < count; i++) {
            if (montauk::streq(users[i].username, username)) {
                uint8_t salt_bytes[16];
                montauk::getrandom(salt_bytes, 16);
                bytes_to_hex(salt_bytes, 16, users[i].salt);

                hash_password(new_password, users[i].salt, users[i].password_hash);

                save_users(users, count);
                return true;
            }
        }
        return false;
    }

    // ---- Session (current logged-in user) ----

    // Write the current session after successful login.
    // Stores username in 0:/config/session.toml so any app can query it.
    inline void set_session(const char* username) {
        montauk::toml::Doc doc;
        doc.init();
        config::set_string(&doc, "session.username", username);
        config::save("session", &doc);
        doc.destroy();
    }

    // Clear the session (on logout).
    inline void clear_session() {
        montauk::fdelete("0:/config/session.toml");
    }

    // Read the current username into buf. Returns true if a session exists.
    inline bool get_session_username(char* buf, int bufSz) {
        auto doc = config::load("session");
        const char* name = doc.get_string("session.username", "");
        bool found = (name[0] != '\0');
        if (found) {
            montauk::strncpy(buf, name, bufSz);
        }
        doc.destroy();
        return found;
    }

    // Get the current user's home directory. Returns true if a session exists.
    inline bool get_home_dir(char* buf, int bufSz) {
        char username[32];
        if (!get_session_username(username, sizeof(username))) return false;
        home_dir(username, buf, bufSz);
        return true;
    }

} // namespace user
} // namespace montauk
