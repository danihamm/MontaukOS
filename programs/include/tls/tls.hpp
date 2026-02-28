/*
 * tls.hpp
 * Shared TLS helper library for MontaukOS
 * Trust anchor loading, BearSSL time, TLS I/O, HTTPS fetch
 * Copyright (c) 2026 Daniel Hammer
 */

#pragma once

extern "C" {
#include <bearssl.h>
}

#include <stddef.h>
#include <stdint.h>

namespace tls {

struct TrustAnchors {
    br_x509_trust_anchor* anchors;
    size_t count;
    size_t capacity;
};

TrustAnchors load_trust_anchors();
void get_bearssl_time(uint32_t* days, uint32_t* seconds);
int tls_send_all(int fd, const unsigned char* data, size_t len);
int tls_recv_some(int fd, unsigned char* buf, size_t maxlen);

// Optional abort callback (for Ctrl+Q in terminal apps). nullptr = no abort.
using AbortCheckFn = bool (*)();

int tls_exchange(int fd, br_ssl_engine_context* eng,
                 const char* request, int reqLen,
                 char* respBuf, int respMax,
                 AbortCheckFn abort_check = nullptr);

// High-level: socket -> TLS setup -> exchange -> cleanup, all in one call.
int https_fetch(const char* host, uint32_t ip, uint16_t port,
                const char* request, int reqLen,
                const TrustAnchors& tas,
                char* respBuf, int respMax,
                AbortCheckFn abort_check = nullptr);

} // namespace tls
