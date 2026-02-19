/*
    * main.cpp
    * DHCP client for ZenithOS
    * Obtains network configuration automatically via DHCP (RFC 2131)
    * Copyright (c) 2025-2026 Daniel Hammer
*/

#include <zenith/syscall.h>
#include <zenith/string.h>

using zenith::memcpy;
using zenith::memset;

// ---- Minimal snprintf (no libc available) ----

using va_list = __builtin_va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

struct PfState {
    char*  buf;
    int    pos;
    int    max;
};

static void pf_putc(PfState* st, char c) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void pf_putnum(PfState* st, unsigned long val, int base, int width, char pad, int neg) {
    char tmp[24];
    int i = 0;
    const char* digits = "0123456789abcdef";
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val > 0) { tmp[i++] = digits[val % base]; val /= base; } }
    int total = (neg ? 1 : 0) + i;
    if (neg && pad == '0') pf_putc(st, '-');
    for (int w = total; w < width; w++) pf_putc(st, pad);
    if (neg && pad != '0') pf_putc(st, '-');
    while (i > 0) pf_putc(st, tmp[--i]);
}

static int vsnprintf(char* buf, int size, const char* fmt, va_list ap) {
    PfState st;
    st.buf = buf;
    st.pos = 0;
    st.max = size > 0 ? size - 1 : 0;
    while (*fmt) {
        if (*fmt != '%') { pf_putc(&st, *fmt++); continue; }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            long val = va_arg(ap, int);
            int neg = 0; unsigned long uval;
            if (val < 0) { neg = 1; uval = (unsigned long)(-val); }
            else uval = (unsigned long)val;
            pf_putnum(&st, uval, 10, width, pad, neg);
            break;
        }
        case 'u': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 10, width, pad, 0); break; }
        case 'x': { unsigned val = va_arg(ap, unsigned); pf_putnum(&st, val, 16, width, pad, 0); break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            for (int w = slen; w < width; w++) pf_putc(&st, ' ');
            for (int j = 0; j < slen; j++) pf_putc(&st, s[j]);
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); pf_putc(&st, c); break; }
        case '%': pf_putc(&st, '%'); break;
        default: pf_putc(&st, '%'); pf_putc(&st, *fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (size > 0) {
        if (st.pos < size) st.buf[st.pos] = '\0';
        else st.buf[size - 1] = '\0';
    }
    return st.pos;
}

static int snprintf(char* buf, int size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}



// ---- DHCP constants ----

static constexpr uint8_t  BOOTREQUEST = 1;
static constexpr uint8_t  BOOTREPLY   = 2;
static constexpr uint8_t  HTYPE_ETH   = 1;
static constexpr uint8_t  HLEN_ETH    = 6;

static constexpr uint16_t DHCP_SERVER_PORT = 67;
static constexpr uint16_t DHCP_CLIENT_PORT = 68;

static constexpr uint32_t DHCP_MAGIC = 0x63825363; // network byte order

static constexpr uint16_t BROADCAST_FLAG = 0x8000;

// DHCP message types
static constexpr uint8_t DHCPDISCOVER = 1;
static constexpr uint8_t DHCPOFFER    = 2;
static constexpr uint8_t DHCPREQUEST  = 3;
static constexpr uint8_t DHCPACK      = 5;
static constexpr uint8_t DHCPNAK      = 6;

// DHCP option codes
static constexpr uint8_t OPT_SUBNET        = 1;
static constexpr uint8_t OPT_ROUTER        = 3;
static constexpr uint8_t OPT_DNS           = 6;
static constexpr uint8_t OPT_REQUESTED_IP  = 50;
static constexpr uint8_t OPT_LEASE_TIME    = 51;
static constexpr uint8_t OPT_MSG_TYPE      = 53;
static constexpr uint8_t OPT_SERVER_ID     = 54;
static constexpr uint8_t OPT_PARAM_LIST    = 55;
static constexpr uint8_t OPT_END           = 255;

// ---- DHCP packet structure ----

struct DhcpPacket {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} __attribute__((packed));

// ---- Byte order helpers (DHCP uses network byte order) ----

static uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static uint32_t ntohl(uint32_t v) { return htonl(v); }

// ---- IP formatting ----

static void format_ip(char* buf, int size, uint32_t ip) {
    const uint8_t* b = (const uint8_t*)&ip;
    snprintf(buf, size, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

static void format_mac(char* buf, int size, const uint8_t* mac) {
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- Build DHCP packets ----

static uint32_t g_xid = 0x5A454E49; // "ZENI"

static void build_base(DhcpPacket* pkt, const uint8_t* mac) {
    memset(pkt, 0, sizeof(DhcpPacket));
    pkt->op    = BOOTREQUEST;
    pkt->htype = HTYPE_ETH;
    pkt->hlen  = HLEN_ETH;
    pkt->xid   = g_xid;
    pkt->flags = htons(BROADCAST_FLAG);
    memcpy(pkt->chaddr, mac, 6);

    // Magic cookie
    pkt->options[0] = 0x63;
    pkt->options[1] = 0x82;
    pkt->options[2] = 0x53;
    pkt->options[3] = 0x63;
}

static int build_discover(DhcpPacket* pkt, const uint8_t* mac) {
    build_base(pkt, mac);

    int off = 4; // after magic cookie

    // Option 53: DHCP Message Type = DISCOVER
    pkt->options[off++] = OPT_MSG_TYPE;
    pkt->options[off++] = 1;
    pkt->options[off++] = DHCPDISCOVER;

    // Option 55: Parameter Request List
    pkt->options[off++] = OPT_PARAM_LIST;
    pkt->options[off++] = 4;
    pkt->options[off++] = OPT_SUBNET;
    pkt->options[off++] = OPT_ROUTER;
    pkt->options[off++] = OPT_DNS;
    pkt->options[off++] = OPT_LEASE_TIME;

    pkt->options[off++] = OPT_END;

    return (int)sizeof(DhcpPacket) - 312 + off;
}

static int build_request(DhcpPacket* pkt, const uint8_t* mac,
                          uint32_t requestedIp, uint32_t serverId) {
    build_base(pkt, mac);

    int off = 4;

    // Option 53: DHCP Message Type = REQUEST
    pkt->options[off++] = OPT_MSG_TYPE;
    pkt->options[off++] = 1;
    pkt->options[off++] = DHCPREQUEST;

    // Option 50: Requested IP Address
    pkt->options[off++] = OPT_REQUESTED_IP;
    pkt->options[off++] = 4;
    memcpy(&pkt->options[off], &requestedIp, 4);
    off += 4;

    // Option 54: Server Identifier
    pkt->options[off++] = OPT_SERVER_ID;
    pkt->options[off++] = 4;
    memcpy(&pkt->options[off], &serverId, 4);
    off += 4;

    // Option 55: Parameter Request List
    pkt->options[off++] = OPT_PARAM_LIST;
    pkt->options[off++] = 4;
    pkt->options[off++] = OPT_SUBNET;
    pkt->options[off++] = OPT_ROUTER;
    pkt->options[off++] = OPT_DNS;
    pkt->options[off++] = OPT_LEASE_TIME;

    pkt->options[off++] = OPT_END;

    return (int)sizeof(DhcpPacket) - 312 + off;
}

// ---- Parse DHCP options ----

struct DhcpOffer {
    uint32_t offeredIp;
    uint32_t serverId;
    uint32_t subnetMask;
    uint32_t router;
    uint32_t dns;
    uint32_t leaseTime;
    uint8_t  msgType;
    bool     valid;
};

static void parse_options(const DhcpPacket* pkt, DhcpOffer* offer) {
    offer->offeredIp  = pkt->yiaddr;
    offer->serverId   = 0;
    offer->subnetMask = 0;
    offer->router     = 0;
    offer->dns        = 0;
    offer->leaseTime  = 0;
    offer->msgType    = 0;
    offer->valid      = false;

    // Verify magic cookie
    if (pkt->options[0] != 0x63 || pkt->options[1] != 0x82 ||
        pkt->options[2] != 0x53 || pkt->options[3] != 0x63) {
        return;
    }

    int off = 4;
    while (off < 312) {
        uint8_t code = pkt->options[off++];
        if (code == OPT_END) break;
        if (code == 0) continue; // pad

        if (off >= 312) break;
        uint8_t len = pkt->options[off++];
        if (off + len > 312) break;

        switch (code) {
        case OPT_MSG_TYPE:
            if (len >= 1) offer->msgType = pkt->options[off];
            break;
        case OPT_SUBNET:
            if (len >= 4) memcpy(&offer->subnetMask, &pkt->options[off], 4);
            break;
        case OPT_ROUTER:
            if (len >= 4) memcpy(&offer->router, &pkt->options[off], 4);
            break;
        case OPT_DNS:
            if (len >= 4) memcpy(&offer->dns, &pkt->options[off], 4);
            break;
        case OPT_SERVER_ID:
            if (len >= 4) memcpy(&offer->serverId, &pkt->options[off], 4);
            break;
        case OPT_LEASE_TIME:
            if (len >= 4) {
                uint32_t raw;
                memcpy(&raw, &pkt->options[off], 4);
                offer->leaseTime = ntohl(raw);
            }
            break;
        }

        off += len;
    }

    offer->valid = (offer->msgType != 0);
}

// ---- Broadcast destination ----

static constexpr uint32_t BROADCAST_IP = 0xFFFFFFFF;

// ---- Main ----

extern "C" void _start() {
    char msg[256];

    zenith::print("ZenithOS DHCP Client\n");

    // 1. Get MAC address
    Zenith::NetCfg origCfg;
    zenith::get_netcfg(&origCfg);

    char macStr[32];
    format_mac(macStr, sizeof(macStr), origCfg.macAddress);
    snprintf(msg, sizeof(msg), "MAC address: %s\n", macStr);
    zenith::print(msg);

    // 2. Set IP to 0.0.0.0 to allow broadcast send/receive
    Zenith::NetCfg zeroCfg;
    zeroCfg.ipAddress  = 0;
    zeroCfg.subnetMask = 0;
    zeroCfg.gateway    = 0;
    zenith::set_netcfg(&zeroCfg);

    // 3. Create UDP socket and bind to port 68
    int fd = zenith::socket(Zenith::SOCK_UDP);
    if (fd < 0) {
        zenith::print("Error: failed to create UDP socket\n");
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    if (zenith::bind(fd, DHCP_CLIENT_PORT) < 0) {
        zenith::print("Error: failed to bind to port 68\n");
        zenith::closesocket(fd);
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    // 4. Send DISCOVER
    DhcpPacket pkt;
    int pktLen = build_discover(&pkt, origCfg.macAddress);

    zenith::print("Sending DHCPDISCOVER...\n");
    if (zenith::sendto(fd, (const void*)&pkt, pktLen, BROADCAST_IP, DHCP_SERVER_PORT) < 0) {
        zenith::print("Error: failed to send DISCOVER\n");
        zenith::closesocket(fd);
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    // 5. Wait for OFFER
    DhcpPacket resp;
    DhcpOffer offer;
    uint64_t startMs = zenith::get_milliseconds();
    bool gotOffer = false;

    zenith::print("Waiting for DHCPOFFER...\n");
    while (zenith::get_milliseconds() - startMs < 10000) {
        uint32_t srcIp;
        uint16_t srcPort;
        int r = zenith::recvfrom(fd, (void*)&resp, sizeof(resp), &srcIp, &srcPort);
        if (r > 0) {
            if (resp.op == BOOTREPLY && resp.xid == g_xid) {
                parse_options(&resp, &offer);
                if (offer.valid && offer.msgType == DHCPOFFER) {
                    gotOffer = true;
                    break;
                }
            }
        }
        zenith::yield();
    }

    if (!gotOffer) {
        zenith::print("Error: no DHCPOFFER received (timeout)\n");
        zenith::closesocket(fd);
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    char ipStr[32];
    format_ip(ipStr, sizeof(ipStr), offer.offeredIp);
    snprintf(msg, sizeof(msg), "Received OFFER: %s\n", ipStr);
    zenith::print(msg);

    // 6. Send REQUEST
    pktLen = build_request(&pkt, origCfg.macAddress, offer.offeredIp, offer.serverId);

    zenith::print("Sending DHCPREQUEST...\n");
    if (zenith::sendto(fd, (const void*)&pkt, pktLen, BROADCAST_IP, DHCP_SERVER_PORT) < 0) {
        zenith::print("Error: failed to send REQUEST\n");
        zenith::closesocket(fd);
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    // 7. Wait for ACK
    bool gotAck = false;
    startMs = zenith::get_milliseconds();

    zenith::print("Waiting for DHCPACK...\n");
    while (zenith::get_milliseconds() - startMs < 10000) {
        uint32_t srcIp;
        uint16_t srcPort;
        int r = zenith::recvfrom(fd, (void*)&resp, sizeof(resp), &srcIp, &srcPort);
        if (r > 0) {
            if (resp.op == BOOTREPLY && resp.xid == g_xid) {
                parse_options(&resp, &offer);
                if (offer.valid && offer.msgType == DHCPACK) {
                    gotAck = true;
                    break;
                }
                if (offer.valid && offer.msgType == DHCPNAK) {
                    zenith::print("Error: received DHCPNAK from server\n");
                    zenith::closesocket(fd);
                    zenith::set_netcfg(&origCfg);
                    zenith::exit(1);
                }
            }
        }
        zenith::yield();
    }

    zenith::closesocket(fd);

    if (!gotAck) {
        zenith::print("Error: no DHCPACK received (timeout)\n");
        zenith::set_netcfg(&origCfg);
        zenith::exit(1);
    }

    // 8. Apply configuration
    Zenith::NetCfg newCfg;
    newCfg.ipAddress  = offer.offeredIp;
    newCfg.subnetMask = offer.subnetMask;
    newCfg.gateway    = offer.router;
    zenith::set_netcfg(&newCfg);

    // 9. Print results
    zenith::print("\nDHCP configuration applied:\n");

    format_ip(ipStr, sizeof(ipStr), offer.offeredIp);
    snprintf(msg, sizeof(msg), "  IP Address:  %s\n", ipStr);
    zenith::print(msg);

    format_ip(ipStr, sizeof(ipStr), offer.subnetMask);
    snprintf(msg, sizeof(msg), "  Subnet Mask: %s\n", ipStr);
    zenith::print(msg);

    format_ip(ipStr, sizeof(ipStr), offer.router);
    snprintf(msg, sizeof(msg), "  Gateway:     %s\n", ipStr);
    zenith::print(msg);

    if (offer.dns != 0) {
        format_ip(ipStr, sizeof(ipStr), offer.dns);
        snprintf(msg, sizeof(msg), "  DNS Server:  %s\n", ipStr);
        zenith::print(msg);
    }

    if (offer.leaseTime != 0) {
        snprintf(msg, sizeof(msg), "  Lease Time:  %u seconds\n", offer.leaseTime);
        zenith::print(msg);
    }

    zenith::exit(0);
}
