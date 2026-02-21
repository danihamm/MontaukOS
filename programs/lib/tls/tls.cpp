/*
 * tls.cpp
 * Shared TLS helper library for ZenithOS
 * Extracted from fetch, wiki, wikipedia, and weather apps
 * Copyright (c) 2025-2026 Daniel Hammer
 */

#include <tls/tls.hpp>
#include <zenith/syscall.h>

extern "C" {
#include <string.h>
#include <stdlib.h>
}

// ============================================================================
// File-local: PEM/DER certificate parsing helpers
// ============================================================================

namespace {

struct DerAccum { unsigned char* data; size_t len, cap; };
struct DnAccum  { unsigned char* data; size_t len, cap; };

void der_append(void* ctx, const void* buf, size_t len) {
    DerAccum* a = (DerAccum*)ctx;
    if (a->len + len > a->cap) {
        size_t nc = a->cap * 2;
        if (nc < a->len + len) nc = a->len + len + 4096;
        unsigned char* nb = (unsigned char*)malloc(nc);
        if (!nb) return;
        if (a->data) { memcpy(nb, a->data, a->len); free(a->data); }
        a->data = nb; a->cap = nc;
    }
    memcpy(a->data + a->len, buf, len);
    a->len += len;
}

void dn_append(void* ctx, const void* buf, size_t len) {
    DnAccum* a = (DnAccum*)ctx;
    if (a->len + len > a->cap) {
        size_t nc = a->cap * 2;
        if (nc < a->len + len) nc = a->len + len + 256;
        unsigned char* nb = (unsigned char*)malloc(nc);
        if (!nb) return;
        if (a->data) { memcpy(nb, a->data, a->len); free(a->data); }
        a->data = nb; a->cap = nc;
    }
    memcpy(a->data + a->len, buf, len);
    a->len += len;
}

void ta_add(tls::TrustAnchors* tas, const br_x509_trust_anchor* ta) {
    if (tas->count >= tas->capacity) {
        size_t nc = tas->capacity == 0 ? 64 : tas->capacity * 2;
        br_x509_trust_anchor* na = (br_x509_trust_anchor*)malloc(nc * sizeof(*na));
        if (!na) return;
        if (tas->anchors) { memcpy(na, tas->anchors, tas->count * sizeof(*na)); free(tas->anchors); }
        tas->anchors = na; tas->capacity = nc;
    }
    tas->anchors[tas->count++] = *ta;
}

bool process_cert_der(tls::TrustAnchors* tas, const unsigned char* der, size_t der_len) {
    static br_x509_decoder_context dc;  // ~2KB+, keep off stack
    DnAccum dn = {nullptr, 0, 0};
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) { if (dn.data) free(dn.data); return false; }

    br_x509_trust_anchor ta;
    memset(&ta, 0, sizeof(ta));
    ta.dn.data = dn.data; ta.dn.len = dn.len; ta.flags = 0;
    if (br_x509_decoder_isCA(&dc)) ta.flags |= BR_X509_TA_CA;

    switch (pk->key_type) {
    case BR_KEYTYPE_RSA:
        ta.pkey.key_type = BR_KEYTYPE_RSA;
        ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta.pkey.key.rsa.n = (unsigned char*)malloc(pk->key.rsa.nlen);
        if (ta.pkey.key.rsa.n) memcpy(ta.pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
        ta.pkey.key.rsa.elen = pk->key.rsa.elen;
        ta.pkey.key.rsa.e = (unsigned char*)malloc(pk->key.rsa.elen);
        if (ta.pkey.key.rsa.e) memcpy(ta.pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
        break;
    case BR_KEYTYPE_EC:
        ta.pkey.key_type = BR_KEYTYPE_EC;
        ta.pkey.key.ec.curve = pk->key.ec.curve;
        ta.pkey.key.ec.qlen = pk->key.ec.qlen;
        ta.pkey.key.ec.q = (unsigned char*)malloc(pk->key.ec.qlen);
        if (ta.pkey.key.ec.q) memcpy(ta.pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
        break;
    default:
        if (dn.data) free(dn.data);
        return false;
    }
    ta_add(tas, &ta);
    return true;
}

} // anonymous namespace

// ============================================================================
// Exported functions
// ============================================================================

namespace tls {

TrustAnchors load_trust_anchors() {
    TrustAnchors tas = {nullptr, 0, 0};
    int fh = zenith::open("0:/etc/ca-certificates.crt");
    if (fh < 0) return tas;
    uint64_t fsize = zenith::getsize(fh);
    if (fsize == 0 || fsize > 512 * 1024) { zenith::close(fh); return tas; }

    unsigned char* pem = (unsigned char*)malloc(fsize + 1);
    if (!pem) { zenith::close(fh); return tas; }
    zenith::read(fh, pem, 0, fsize);
    zenith::close(fh);
    pem[fsize] = 0;

    static br_pem_decoder_context pc;  // keep off stack
    br_pem_decoder_init(&pc);
    DerAccum der = {nullptr, 0, 0};
    bool inCert = false;
    size_t offset = 0;

    while (offset < fsize) {
        size_t pushed = br_pem_decoder_push(&pc, pem + offset, fsize - offset);
        offset += pushed;
        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            inCert = (strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") == 0);
            br_pem_decoder_setdest(&pc, inCert ? der_append : nullptr, inCert ? &der : nullptr);
            if (inCert) der.len = 0;
        } else if (ev == BR_PEM_END_OBJ) {
            if (inCert && der.len > 0) process_cert_der(&tas, der.data, der.len);
            inCert = false;
        } else if (ev == BR_PEM_ERROR) {
            break;
        }
    }
    if (der.data) free(der.data);
    free(pem);
    return tas;
}

void get_bearssl_time(uint32_t* days, uint32_t* seconds) {
    Zenith::DateTime dt;
    zenith::gettime(&dt);
    int y = dt.Year, m = dt.Month, d = dt.Day;
    uint32_t total = 365u * (uint32_t)y
        + (uint32_t)(y/4) - (uint32_t)(y/100) + (uint32_t)(y/400);
    const int md[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    for (int mo = 1; mo < m && mo <= 12; mo++) total += md[mo];
    if (y%4==0 && (y%100!=0 || y%400==0) && m > 2) total++;
    total += d - 1;
    *days = total;
    *seconds = (uint32_t)(dt.Hour*3600 + dt.Minute*60 + dt.Second);
}

int tls_send_all(int fd, const unsigned char* data, size_t len) {
    size_t sent = 0;
    uint64_t deadline = zenith::get_milliseconds() + 15000;
    while (sent < len) {
        int r = zenith::send(fd, data + sent, (uint32_t)(len - sent));
        if (r > 0) { sent += r; deadline = zenith::get_milliseconds() + 15000; }
        else if (r < 0) return -1;
        else { if (zenith::get_milliseconds() >= deadline) return -1; zenith::sleep_ms(1); }
    }
    return (int)sent;
}

int tls_recv_some(int fd, unsigned char* buf, size_t maxlen) {
    uint64_t deadline = zenith::get_milliseconds() + 15000;
    while (true) {
        int r = zenith::recv(fd, buf, (uint32_t)maxlen);
        if (r > 0) return r;
        if (r < 0) return -1;
        if (zenith::get_milliseconds() >= deadline) return -1;
        zenith::sleep_ms(1);
    }
}

int tls_exchange(int fd, br_ssl_engine_context* eng,
                 const char* request, int reqLen,
                 char* respBuf, int respMax,
                 AbortCheckFn abort_check) {
    bool requestSent = false;
    int respLen = 0;
    uint64_t deadline = zenith::get_milliseconds() + 30000;

    while (true) {
        unsigned state = br_ssl_engine_current_state(eng);
        if (state & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            if (err != BR_ERR_OK && err != BR_ERR_IO && respLen == 0) return -1;
            return respLen;
        }
        if (abort_check && abort_check()) {
            br_ssl_engine_close(eng);
            return respLen > 0 ? respLen : -1;
        }
        if (state & BR_SSL_SENDREC) {
            size_t len; unsigned char* buf = br_ssl_engine_sendrec_buf(eng, &len);
            int sent = tls_send_all(fd, buf, len);
            if (sent < 0) { br_ssl_engine_close(eng); return respLen > 0 ? respLen : -1; }
            br_ssl_engine_sendrec_ack(eng, len);
            deadline = zenith::get_milliseconds() + 30000; continue;
        }
        if (state & BR_SSL_RECVAPP) {
            size_t len; unsigned char* buf = br_ssl_engine_recvapp_buf(eng, &len);
            size_t toCopy = len;
            if (respLen + (int)toCopy > respMax - 1) toCopy = respMax - 1 - respLen;
            if (toCopy > 0) { memcpy(respBuf + respLen, buf, toCopy); respLen += toCopy; }
            br_ssl_engine_recvapp_ack(eng, len);
            deadline = zenith::get_milliseconds() + 30000; continue;
        }
        if ((state & BR_SSL_SENDAPP) && !requestSent) {
            size_t len; unsigned char* buf = br_ssl_engine_sendapp_buf(eng, &len);
            size_t toWrite = (size_t)reqLen;
            if (toWrite > len) toWrite = len;
            memcpy(buf, request, toWrite);
            br_ssl_engine_sendapp_ack(eng, toWrite);
            br_ssl_engine_flush(eng, 0);
            requestSent = true;
            deadline = zenith::get_milliseconds() + 30000; continue;
        }
        if (state & BR_SSL_RECVREC) {
            size_t len; unsigned char* buf = br_ssl_engine_recvrec_buf(eng, &len);
            int got = tls_recv_some(fd, buf, len);
            if (got < 0) { br_ssl_engine_close(eng); return respLen > 0 ? respLen : -1; }
            br_ssl_engine_recvrec_ack(eng, got);
            deadline = zenith::get_milliseconds() + 30000; continue;
        }
        if (zenith::get_milliseconds() >= deadline) return respLen > 0 ? respLen : -1;
        zenith::sleep_ms(1);
    }
}

int https_fetch(const char* host, uint32_t ip, uint16_t port,
                const char* request, int reqLen,
                const TrustAnchors& tas,
                char* respBuf, int respMax,
                AbortCheckFn abort_check) {
    int fd = zenith::socket(Zenith::SOCK_TCP);
    if (fd < 0) return -1;
    if (zenith::connect(fd, ip, port) < 0) { zenith::closesocket(fd); return -1; }

    br_ssl_client_context* cc  = (br_ssl_client_context*)malloc(sizeof(*cc));
    br_x509_minimal_context* xc = (br_x509_minimal_context*)malloc(sizeof(*xc));
    void* iobuf = malloc(BR_SSL_BUFSIZE_BIDI);
    if (!cc || !xc || !iobuf) {
        free(cc); free(xc); free(iobuf);
        zenith::closesocket(fd); return -1;
    }

    br_ssl_client_init_full(cc, xc, tas.anchors, tas.count);
    uint32_t days, secs;
    get_bearssl_time(&days, &secs);
    br_x509_minimal_set_time(xc, days, secs);

    unsigned char seed[32];
    zenith::getrandom(seed, sizeof(seed));
    br_ssl_engine_set_buffer(&cc->eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    br_ssl_engine_inject_entropy(&cc->eng, seed, sizeof(seed));

    if (!br_ssl_client_reset(cc, host, 0)) {
        zenith::closesocket(fd); free(cc); free(xc); free(iobuf); return -1;
    }

    int respLen = tls_exchange(fd, &cc->eng, request, reqLen, respBuf, respMax, abort_check);
    zenith::closesocket(fd);
    free(cc); free(xc); free(iobuf);
    return respLen;
}

} // namespace tls
