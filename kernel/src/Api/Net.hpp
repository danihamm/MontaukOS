/*
    * Net.hpp
    * Networking syscalls: SYS_PING, SYS_SOCKET, SYS_CONNECT, SYS_BIND,
    * SYS_LISTEN, SYS_ACCEPT, SYS_SEND, SYS_RECV, SYS_CLOSESOCK,
    * SYS_SENDTO, SYS_RECVFROM, SYS_GETNETCFG, SYS_SETNETCFG, SYS_RESOLVE
    * Copyright (c) 2026 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Sched/Scheduler.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Net/Icmp.hpp>
#include <Net/Dns.hpp>
#include <Net/Socket.hpp>
#include <Net/NetConfig.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Drivers/Net/E1000E.hpp>

#include "Syscall.hpp"

namespace Montauk {

    static uint16_t g_pingSeq = 0;
    static constexpr uint16_t PING_ID = 0x2E01; // "ZE"

    static int32_t Sys_Ping(uint32_t ipAddr, uint32_t timeoutMs) {
        uint16_t seq = g_pingSeq++;

        Net::Icmp::ResetReply();
        Net::Icmp::SendEchoRequest(ipAddr, PING_ID, seq);

        uint64_t start = Timekeeping::GetMilliseconds();
        while (!Net::Icmp::HasReply(PING_ID, seq)) {
            if (Timekeeping::GetMilliseconds() - start >= timeoutMs) {
                return -1;
            }
            Sched::Schedule();
        }

        return (int32_t)(Timekeeping::GetMilliseconds() - start);
    }

    // ---- Socket syscalls ----

    static int Sys_Socket(int type) {
        return Net::Socket::Create(type, Sched::GetCurrentPid());
    }

    static int Sys_Connect(int fd, uint32_t ip, uint16_t port) {
        return Net::Socket::Connect(fd, ip, port, Sched::GetCurrentPid());
    }

    static int Sys_Bind(int fd, uint16_t port) {
        return Net::Socket::Bind(fd, port, Sched::GetCurrentPid());
    }

    static int Sys_Listen(int fd) {
        return Net::Socket::Listen(fd, Sched::GetCurrentPid());
    }

    static int Sys_Accept(int fd) {
        return Net::Socket::Accept(fd, Sched::GetCurrentPid());
    }

    static int Sys_Send(int fd, const uint8_t* data, uint32_t len) {
        return Net::Socket::Send(fd, data, len, Sched::GetCurrentPid());
    }

    static int Sys_Recv(int fd, uint8_t* buf, uint32_t maxLen) {
        return Net::Socket::Recv(fd, buf, maxLen, Sched::GetCurrentPid());
    }

    static void Sys_CloseSock(int fd) {
        Net::Socket::Close(fd, Sched::GetCurrentPid());
    }

    static int Sys_SendTo(int fd, const uint8_t* data, uint32_t len,
                          uint32_t destIp, uint16_t destPort) {
        return Net::Socket::SendTo(fd, data, len, destIp, destPort, Sched::GetCurrentPid());
    }

    static int Sys_RecvFrom(int fd, uint8_t* buf, uint32_t maxLen,
                            uint32_t* srcIp, uint16_t* srcPort) {
        return Net::Socket::RecvFrom(fd, buf, maxLen, srcIp, srcPort, Sched::GetCurrentPid());
    }

    static void Sys_GetNetCfg(NetCfg* out) {
        if (out == nullptr) return;
        out->ipAddress  = Net::GetIpAddress();
        out->subnetMask = Net::GetSubnetMask();
        out->gateway    = Net::GetGateway();

        const uint8_t* mac = nullptr;
        if (Drivers::Net::E1000::IsInitialized()) {
            mac = Drivers::Net::E1000::GetMacAddress();
        } else if (Drivers::Net::E1000E::IsInitialized()) {
            mac = Drivers::Net::E1000E::GetMacAddress();
        }
        if (mac) {
            for (int i = 0; i < 6; i++) out->macAddress[i] = mac[i];
        } else {
            for (int i = 0; i < 6; i++) out->macAddress[i] = 0;
        }
        out->_pad[0] = 0;
        out->_pad[1] = 0;
        out->dnsServer = Net::GetDnsServer();
    }

    static int Sys_SetNetCfg(const NetCfg* in) {
        if (in == nullptr) return -1;
        Net::SetIpAddress(in->ipAddress);
        Net::SetSubnetMask(in->subnetMask);
        Net::SetGateway(in->gateway);
        Net::SetDnsServer(in->dnsServer);
        return 0;
    }

    // ---- DNS resolve ----

    static int64_t Sys_Resolve(const char* hostname) {
        uint32_t ip = Net::Dns::Resolve(hostname);
        return (int64_t)ip;
    }
};
