/*
    * NetConfig.cpp
    * Network configuration (static IP, gateway, etc.)
    * Copyright (c) 2025 Daniel Hammer
*/

#include "NetConfig.hpp"

namespace Net {

    // Unconfigured until usermode DHCP sets them
    static uint32_t g_ipAddress  = 0;
    static uint32_t g_subnetMask = 0;
    static uint32_t g_gateway    = 0;
    static uint32_t g_dnsServer  = 0;

    uint32_t GetIpAddress() { return g_ipAddress; }
    void SetIpAddress(uint32_t ip) { g_ipAddress = ip; }

    uint32_t GetSubnetMask() { return g_subnetMask; }
    void SetSubnetMask(uint32_t mask) { g_subnetMask = mask; }

    uint32_t GetGateway() { return g_gateway; }
    void SetGateway(uint32_t gw) { g_gateway = gw; }

    uint32_t GetDnsServer() { return g_dnsServer; }
    void SetDnsServer(uint32_t dns) { g_dnsServer = dns; }

    bool IsLocalSubnet(uint32_t destIp) {
        return (destIp & g_subnetMask) == (g_ipAddress & g_subnetMask);
    }

    uint32_t GetNextHop(uint32_t destIp) {
        if (IsLocalSubnet(destIp)) {
            return destIp;
        }
        return g_gateway;
    }

}
