/*
    * Dns.cpp
    * DNS resolver (kernel-level, RFC 1035)
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include "Dns.hpp"
#include <Net/Udp.hpp>
#include <Net/ByteOrder.hpp>
#include <Net/NetConfig.hpp>
#include <Libraries/Memory.hpp>
#include <Libraries/String.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Sched/Scheduler.hpp>
#include <Terminal/Terminal.hpp>

namespace Net::Dns {

    // ---- DNS packet constants ----

    static constexpr uint16_t DNS_PORT       = 53;
    static constexpr uint16_t DNS_FLAGS_RD   = 0x0100; // Recursion Desired
    static constexpr uint16_t DNS_QTYPE_A    = 1;
    static constexpr uint16_t DNS_QCLASS_IN  = 1;

    // ---- Simple cache ----

    static constexpr int CACHE_SIZE = 8;

    struct CacheEntry {
        char     hostname[128];
        uint32_t ip;
        uint32_t ttl;         // TTL in seconds
        uint64_t timestamp;   // ms when cached
        bool     valid;
    };

    static CacheEntry g_cache[CACHE_SIZE] = {};

    static bool streq(const char* a, const char* b) {
        while (*a && *b) {
            if (*a != *b) return false;
            a++; b++;
        }
        return *a == *b;
    }

    static uint32_t CacheLookup(const char* hostname) {
        uint64_t now = Timekeeping::GetMilliseconds();
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (!g_cache[i].valid) continue;
            if (!streq(g_cache[i].hostname, hostname)) continue;
            // Check TTL
            uint64_t elapsed = (now - g_cache[i].timestamp) / 1000;
            if (elapsed < g_cache[i].ttl) {
                return g_cache[i].ip;
            }
            // Expired
            g_cache[i].valid = false;
            return 0;
        }
        return 0;
    }

    static void CacheStore(const char* hostname, uint32_t ip, uint32_t ttl) {
        if (ttl == 0) ttl = 60; // Minimum 60s TTL

        // Find free or oldest slot
        int slot = 0;
        uint64_t oldestTime = ~0ULL;
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (!g_cache[i].valid) { slot = i; break; }
            if (g_cache[i].timestamp < oldestTime) {
                oldestTime = g_cache[i].timestamp;
                slot = i;
            }
        }

        CacheEntry& e = g_cache[slot];
        // Copy hostname
        int len = 0;
        while (hostname[len] && len < 126) { e.hostname[len] = hostname[len]; len++; }
        e.hostname[len] = '\0';
        e.ip = ip;
        e.ttl = ttl;
        e.timestamp = Timekeeping::GetMilliseconds();
        e.valid = true;
    }

    // ---- DNS query building ----

    // Encode a hostname as DNS labels: "example.com" -> "\x07example\x03com\x00"
    // Returns number of bytes written, or 0 on error.
    static int EncodeName(const char* hostname, uint8_t* out, int maxLen) {
        int outPos = 0;
        const char* p = hostname;

        while (*p) {
            // Find the next dot or end
            const char* dot = p;
            while (*dot && *dot != '.') dot++;
            int labelLen = (int)(dot - p);

            if (labelLen == 0 || labelLen > 63) return 0;
            if (outPos + 1 + labelLen >= maxLen) return 0;

            out[outPos++] = (uint8_t)labelLen;
            for (int i = 0; i < labelLen; i++) {
                out[outPos++] = (uint8_t)p[i];
            }

            p = dot;
            if (*p == '.') p++;
        }

        if (outPos >= maxLen) return 0;
        out[outPos++] = 0; // Root label terminator
        return outPos;
    }

    // Build a DNS query packet. Returns total packet length, or 0 on error.
    static int BuildQuery(uint16_t id, const char* hostname, uint8_t* packet, int maxLen) {
        if (maxLen < 12) return 0;

        // Header (12 bytes)
        packet[0]  = (uint8_t)(id >> 8);
        packet[1]  = (uint8_t)(id & 0xFF);
        packet[2]  = (uint8_t)(DNS_FLAGS_RD >> 8);   // Flags high: RD=1
        packet[3]  = (uint8_t)(DNS_FLAGS_RD & 0xFF);  // Flags low
        packet[4]  = 0; packet[5]  = 1;  // QDCOUNT = 1
        packet[6]  = 0; packet[7]  = 0;  // ANCOUNT = 0
        packet[8]  = 0; packet[9]  = 0;  // NSCOUNT = 0
        packet[10] = 0; packet[11] = 0;  // ARCOUNT = 0

        // Question section
        int nameLen = EncodeName(hostname, packet + 12, maxLen - 12 - 4);
        if (nameLen == 0) return 0;

        int pos = 12 + nameLen;
        if (pos + 4 > maxLen) return 0;

        // QTYPE = A (1)
        packet[pos++] = 0;
        packet[pos++] = DNS_QTYPE_A;
        // QCLASS = IN (1)
        packet[pos++] = 0;
        packet[pos++] = DNS_QCLASS_IN;

        return pos;
    }

    // ---- DNS response parsing ----

    // Skip over a DNS name in the packet (handles compression pointers).
    // Returns the new offset, or -1 on error.
    static int SkipName(const uint8_t* packet, int packetLen, int offset) {
        int maxJumps = 32; // prevent infinite loops
        bool jumped = false;
        int returnOffset = -1;

        while (offset < packetLen && maxJumps > 0) {
            uint8_t len = packet[offset];

            if (len == 0) {
                // End of name
                offset++;
                return jumped ? returnOffset : offset;
            }

            if ((len & 0xC0) == 0xC0) {
                // Compression pointer
                if (offset + 1 >= packetLen) return -1;
                if (!jumped) returnOffset = offset + 2;
                int target = ((len & 0x3F) << 8) | packet[offset + 1];
                if (target >= packetLen) return -1;  // Pointer beyond packet bounds
                offset = target;
                jumped = true;
                maxJumps--;
                continue;
            }

            // Regular label
            offset += 1 + len;
            maxJumps--;
        }

        return -1;
    }

    struct DnsAnswer {
        uint32_t ip;
        uint32_t ttl;
        bool     found;
    };

    // Parse a DNS response and extract the first A record.
    static DnsAnswer ParseResponse(uint16_t expectedId, const uint8_t* packet, int packetLen) {
        DnsAnswer result = {0, 0, false};

        if (packetLen < 12) return result;

        // Check ID
        uint16_t id = ((uint16_t)packet[0] << 8) | packet[1];
        if (id != expectedId) return result;

        // Check QR bit (must be response)
        if (!(packet[2] & 0x80)) return result;

        // Check RCODE (must be 0 = no error)
        uint8_t rcode = packet[3] & 0x0F;
        if (rcode != 0) return result;

        uint16_t qdcount = ((uint16_t)packet[4] << 8) | packet[5];
        uint16_t ancount = ((uint16_t)packet[6] << 8) | packet[7];

        // Skip question section
        int offset = 12;
        for (uint16_t i = 0; i < qdcount; i++) {
            offset = SkipName(packet, packetLen, offset);
            if (offset < 0) return result;
            offset += 4; // QTYPE + QCLASS
            if (offset > packetLen) return result;
        }

        // Parse answers
        for (uint16_t i = 0; i < ancount; i++) {
            offset = SkipName(packet, packetLen, offset);
            if (offset < 0 || offset + 10 > packetLen) return result;

            uint16_t atype  = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
            // uint16_t aclass = ((uint16_t)packet[offset + 2] << 8) | packet[offset + 3];
            uint32_t attl   = ((uint32_t)packet[offset + 4] << 24) |
                              ((uint32_t)packet[offset + 5] << 16) |
                              ((uint32_t)packet[offset + 6] << 8)  |
                              ((uint32_t)packet[offset + 7]);
            uint16_t rdlen  = ((uint16_t)packet[offset + 8] << 8) | packet[offset + 9];
            offset += 10;

            if (offset + rdlen > packetLen) return result;

            if (atype == DNS_QTYPE_A && rdlen == 4) {
                // A record: 4-byte IPv4 address (already in network byte order)
                result.ip = ((uint32_t)packet[offset])
                          | ((uint32_t)packet[offset + 1] << 8)
                          | ((uint32_t)packet[offset + 2] << 16)
                          | ((uint32_t)packet[offset + 3] << 24);
                result.ttl = attl;
                result.found = true;
                return result;
            }

            offset += rdlen;
        }

        return result;
    }

    // ---- Resolve state (shared with UDP callback) ----

    static volatile bool g_gotResponse = false;
    static volatile uint16_t g_currentId = 0;
    static uint8_t g_responseBuffer[512];
    static volatile int g_responseLen = 0;

    static void DnsRecvCallback(uint32_t srcIp, uint16_t srcPort,
                                 uint16_t dstPort,
                                 const uint8_t* data, uint16_t length) {
        (void)srcIp;
        (void)srcPort;
        (void)dstPort;

        if (g_gotResponse) return; // Already got a response
        if (length > sizeof(g_responseBuffer)) length = sizeof(g_responseBuffer);

        memcpy(g_responseBuffer, data, length);
        g_responseLen = length;
        g_gotResponse = true;
    }

    // ---- Simple PRNG for transaction IDs ----

    static uint16_t g_nextId = 0x4E53; // "NS"

    static uint16_t NextId() {
        g_nextId = g_nextId * 25173 + 13849;
        return g_nextId;
    }

    // ---- Check if string is already an IP address ----

    static bool IsIpAddress(const char* s) {
        int dotCount = 0;
        bool hasDigit = false;
        for (int i = 0; s[i]; i++) {
            if (s[i] >= '0' && s[i] <= '9') {
                hasDigit = true;
            } else if (s[i] == '.') {
                if (!hasDigit) return false;
                dotCount++;
                hasDigit = false;
            } else {
                return false;
            }
        }
        return hasDigit && dotCount == 3;
    }

    // ---- Public API ----

    uint32_t Resolve(const char* hostname, uint32_t timeoutMs) {
        if (hostname == nullptr || hostname[0] == '\0') return 0;

        // Don't try to resolve IP addresses
        if (IsIpAddress(hostname)) return 0;

        // Check cache first
        uint32_t cached = CacheLookup(hostname);
        if (cached != 0) return cached;

        // Check DNS server is configured
        uint32_t dnsServer = Net::GetDnsServer();
        if (dnsServer == 0) return 0;

        // Pick a local port for receiving the response (ephemeral range)
        uint16_t localPort = 10000 + (NextId() % 50000);
        uint16_t txId = NextId();

        // Build DNS query
        uint8_t queryPacket[512];
        int queryLen = BuildQuery(txId, hostname, queryPacket, sizeof(queryPacket));
        if (queryLen == 0) return 0;

        // Reset response state
        g_gotResponse = false;
        g_responseLen = 0;
        g_currentId = txId;

        // Bind our receive port
        if (!Net::Udp::Bind(localPort, DnsRecvCallback)) {
            // Port might be in use, try another
            localPort = 10000 + (NextId() % 50000);
            if (!Net::Udp::Bind(localPort, DnsRecvCallback)) {
                return 0;
            }
        }

        // Send the query to DNS server port 53
        bool sent = Net::Udp::Send(dnsServer, localPort, DNS_PORT, queryPacket, (uint16_t)queryLen);
        if (!sent) {
            Net::Udp::Unbind(localPort);
            return 0;
        }

        // Wait for response with timeout
        uint64_t start = Timekeeping::GetMilliseconds();
        while (!g_gotResponse) {
            if (Timekeeping::GetMilliseconds() - start >= timeoutMs) {
                Net::Udp::Unbind(localPort);
                return 0;
            }
            Sched::Schedule();
        }

        // Unbind the port
        Net::Udp::Unbind(localPort);

        // Parse the response
        DnsAnswer answer = ParseResponse(txId, g_responseBuffer, g_responseLen);
        if (!answer.found) return 0;

        // Cache the result
        CacheStore(hostname, answer.ip, answer.ttl);

        return answer.ip;
    }

}
