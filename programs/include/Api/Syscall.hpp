/*
    * Syscall.hpp
    * ZenithOS syscall definitions for userspace programs
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <cstddef>

namespace Zenith {

    // Syscall numbers
    static constexpr uint64_t SYS_EXIT            = 0;
    static constexpr uint64_t SYS_YIELD           = 1;
    static constexpr uint64_t SYS_SLEEP_MS        = 2;
    static constexpr uint64_t SYS_GETPID          = 3;
    static constexpr uint64_t SYS_PRINT           = 4;
    static constexpr uint64_t SYS_PUTCHAR          = 5;
    static constexpr uint64_t SYS_OPEN            = 6;
    static constexpr uint64_t SYS_READ            = 7;
    static constexpr uint64_t SYS_GETSIZE         = 8;
    static constexpr uint64_t SYS_CLOSE           = 9;
    static constexpr uint64_t SYS_READDIR         = 10;
    static constexpr uint64_t SYS_ALLOC           = 11;
    static constexpr uint64_t SYS_FREE            = 12;
    static constexpr uint64_t SYS_GETTICKS        = 13;
    static constexpr uint64_t SYS_GETMILLISECONDS = 14;
    static constexpr uint64_t SYS_GETINFO         = 15;
    static constexpr uint64_t SYS_ISKEYAVAILABLE  = 16;
    static constexpr uint64_t SYS_GETKEY          = 17;
    static constexpr uint64_t SYS_GETCHAR         = 18;
    static constexpr uint64_t SYS_PING            = 19;
    static constexpr uint64_t SYS_SPAWN           = 20;
    static constexpr uint64_t SYS_FBINFO          = 21;
    static constexpr uint64_t SYS_FBMAP           = 22;
    static constexpr uint64_t SYS_WAITPID         = 23;
    static constexpr uint64_t SYS_TERMSIZE        = 24;
    static constexpr uint64_t SYS_GETARGS         = 25;
    static constexpr uint64_t SYS_RESET           = 26;
    static constexpr uint64_t SYS_SHUTDOWN        = 27;
    static constexpr uint64_t SYS_GETTIME         = 28;
    static constexpr uint64_t SYS_SOCKET          = 29;
    static constexpr uint64_t SYS_CONNECT         = 30;
    static constexpr uint64_t SYS_BIND            = 31;
    static constexpr uint64_t SYS_LISTEN          = 32;
    static constexpr uint64_t SYS_ACCEPT          = 33;
    static constexpr uint64_t SYS_SEND            = 34;
    static constexpr uint64_t SYS_RECV            = 35;
    static constexpr uint64_t SYS_CLOSESOCK       = 36;
    static constexpr uint64_t SYS_GETNETCFG      = 37;
    static constexpr uint64_t SYS_SETNETCFG      = 38;
    static constexpr uint64_t SYS_SENDTO         = 39;
    static constexpr uint64_t SYS_RECVFROM       = 40;
    static constexpr uint64_t SYS_FWRITE         = 41;
    static constexpr uint64_t SYS_FCREATE        = 42;
    static constexpr uint64_t SYS_TERMSCALE     = 43;
    static constexpr uint64_t SYS_RESOLVE        = 44;
    static constexpr uint64_t SYS_GETRANDOM     = 45;
    static constexpr uint64_t SYS_KLOG           = 46;
    static constexpr uint64_t SYS_MOUSESTATE     = 47;
    static constexpr uint64_t SYS_SETMOUSEBOUNDS = 48;
    static constexpr uint64_t SYS_SPAWN_REDIR    = 49;
    static constexpr uint64_t SYS_CHILDIO_READ   = 50;
    static constexpr uint64_t SYS_CHILDIO_WRITE  = 51;
    static constexpr uint64_t SYS_CHILDIO_WRITEKEY = 52;
    static constexpr uint64_t SYS_CHILDIO_SETTERMSZ = 53;

    static constexpr int SOCK_TCP = 1;
    static constexpr int SOCK_UDP = 2;

    struct NetCfg {
        uint32_t ipAddress;   // network byte order
        uint32_t subnetMask;  // network byte order
        uint32_t gateway;     // network byte order
        uint8_t  macAddress[6];
        uint8_t  _pad[2];
        uint32_t dnsServer;   // network byte order
    };

    struct DateTime {
        uint16_t Year;
        uint8_t Month;
        uint8_t Day;
        uint8_t Hour;
        uint8_t Minute;
        uint8_t Second;
    };

    struct FbInfo {
        uint64_t width;
        uint64_t height;
        uint64_t pitch;      // bytes per scanline
        uint64_t bpp;        // bits per pixel (32)
        uint64_t userAddr;   // filled by SYS_FBMAP (0 until mapped)
    };

    struct SysInfo {
        char osName[32];
        char osVersion[32];
        uint32_t    apiVersion;
        uint32_t    maxProcesses;
    };

    struct KeyEvent {
        uint8_t scancode;
        char    ascii;
        bool    pressed;
        bool    shift;
        bool    ctrl;
        bool    alt;
    };

    struct MouseState {
        int32_t  x;
        int32_t  y;
        int32_t  scrollDelta;
        uint8_t  buttons;
    };

}
