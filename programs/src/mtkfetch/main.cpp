/*
    * main.cpp
    * mtkfetch - System information display
    * Copyright (c) 2026 Daniel Hammer
*/

#include <montauk/syscall.h>
#include <montauk/string.h>
#include <montauk/heap.h>

// ==== ANSI color codes ====

#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define BWHT  "\033[1;37m"
#define BCYN  "\033[1;36m"
#define CYN   "\033[36m"
#define DGRY  "\033[90m"

// ==== String helpers ====

static char* app(char* d, const char* s) {
    while (*s) *d++ = *s++;
    *d = '\0';
    return d;
}

static char* app_int(char* d, uint64_t n) {
    if (n == 0) { *d++ = '0'; *d = '\0'; return d; }
    char tmp[20];
    int i = 0;
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) *d++ = tmp[j];
    *d = '\0';
    return d;
}

static void pad(int n) {
    for (int i = 0; i < n; i++) montauk::putchar(' ');
}

// ==== MT block letter ASCII art ====
//
// Each line has a known visible width (excluding ANSI codes).
// The print loop pads each line to a fixed column before printing info.

static constexpr int NUM_LINES = 19;
static constexpr int INFO_COL  = 26;

static const char* art[] = {
    /*  0 */ "",
    /*  1 */ "",
    /*  2 */ BCYN "##       ##  ##########" RST,
    /*  3 */ BCYN "###     ###  ##########" RST,
    /*  4 */ BCYN "####   ####      ##" RST,
    /*  5 */ BCYN "## ## ## ##      ##" RST,
    /*  6 */ BCYN "##  ###  ##      ##" RST,
    /*  7 */ BCYN "##   #   ##      ##" RST,
    /*  8 */ BCYN "##       ##      ##" RST,
    /*  9 */ BCYN "##       ##      ##" RST,
    /* 10 */ BCYN "##       ##      ##" RST,
    /* 11 */ CYN  "~~~~~~~~~~~~~~~~~~~~~~~" RST,
    /* 12 */ "",
    /* 13 */ "",
    /* 14 */ "",
    /* 15 */ "",
    /* 16 */ "",
    /* 17 */ "",
    /* 18 */ "",
};

static const int art_vw[] = {
     0,  0, 23, 23, 19, 19, 19, 19, 19, 19,
    19, 23,  0,  0,  0,  0,  0,  0,  0,
};

extern "C" void _start() {
    // ==== Gather system information ====

    Montauk::SysInfo sysinfo;
    montauk::get_info(&sysinfo);

    const char* username = "user";
    char username_buf[32] = {};
    if (montauk::getuser(username_buf, sizeof(username_buf)) > 0 && username_buf[0]) {
        username = username_buf;
    }

    uint64_t ms = montauk::get_milliseconds();
    uint64_t total_secs = ms / 1000;
    uint64_t hours = total_secs / 3600;
    uint64_t mins = (total_secs % 3600) / 60;
    uint64_t secs = total_secs % 60;

    Montauk::MemStats mem;
    montauk::memstats(&mem);

    Montauk::FbInfo fb;
    montauk::fb_info(&fb);

    int tcols = 0, trows = 0;
    montauk::termsize(&tcols, &trows);

    Montauk::DevInfo devs[32];
    int ndev = montauk::devlist(devs, 32);
    const char* cpu_name = "Unknown";
    const char* gpu_name = "Unknown";
    bool found_cpu = false, found_gpu = false;
    for (int i = 0; i < ndev; i++) {
        if (devs[i].category == 0 && !found_cpu) {
            cpu_name = devs[i].name;
            found_cpu = true;
        }
        if (devs[i].category == 6 && !found_gpu) {
            gpu_name = devs[i].name;
            found_gpu = true;
        }
    }

    Montauk::ProcInfo procs[64];
    int nproc = montauk::proclist(procs, 64);
    int active = 0;
    for (int i = 0; i < nproc; i++) {
        if (procs[i].state == 1 || procs[i].state == 2) active++;
    }

    Montauk::NetCfg net;
    montauk::get_netcfg(&net);

    // ==== Build info lines ====

    char info[NUM_LINES][256];
    for (int i = 0; i < NUM_LINES; i++) info[i][0] = '\0';

    // Line 1: user@montauk
    {
        char* p = app(info[1], BCYN BOLD);
        p = app(p, username);
        p = app(p, "@montauk" RST);
    }

    // Line 2: separator
    {
        char* p = info[2];
        int len = montauk::slen(username) + 8;
        for (int i = 0; i < len; i++) *p++ = '-';
        *p = '\0';
    }

    // Line 3: OS
    {
        char* p = app(info[3], BCYN "OS" RST ": ");
        p = app(p, sysinfo.osName);
        p = app(p, " v");
        p = app(p, sysinfo.osVersion);
    }

    // Line 4: API version
    {
        char* p = app(info[4], BCYN "API" RST ": ");
        p = app_int(p, sysinfo.apiVersion);
    }

    // Line 5: Uptime
    {
        char* p = app(info[5], BCYN "Uptime" RST ": ");
        if (hours > 0) {
            p = app_int(p, hours);
            p = app(p, "h ");
        }
        p = app_int(p, mins);
        p = app(p, "m ");
        p = app_int(p, secs);
        p = app(p, "s");
    }

    // Line 6: CPU
    {
        char* p = app(info[6], BCYN "CPU" RST ": ");
        p = app(p, cpu_name);
    }

    // Line 7: GPU
    {
        char* p = app(info[7], BCYN "GPU" RST ": ");
        p = app(p, gpu_name);
    }

    // Line 8: Memory
    {
        char* p = app(info[8], BCYN "Memory" RST ": ");
        p = app_int(p, mem.usedBytes / 1048576);
        p = app(p, " MiB / ");
        p = app_int(p, mem.totalBytes / 1048576);
        p = app(p, " MiB");
    }

    // Line 9: Resolution
    {
        char* p = app(info[9], BCYN "Resolution" RST ": ");
        p = app_int(p, fb.width);
        *p++ = 'x';
        p = app_int(p, fb.height);
        *p = '\0';
    }

    // Line 10: Terminal
    {
        char* p = app(info[10], BCYN "Terminal" RST ": ");
        p = app_int(p, tcols);
        *p++ = 'x';
        p = app_int(p, trows);
        *p = '\0';
    }

    // Line 11: Processes
    {
        char* p = app(info[11], BCYN "Processes" RST ": ");
        p = app_int(p, active);
    }

    // Line 12: Network
    {
        char* p = app(info[12], BCYN "Network" RST ": ");
        uint32_t ip = net.ipAddress;
        if (ip) {
            p = app_int(p, ip & 0xFF);
            *p++ = '.';
            p = app_int(p, (ip >> 8) & 0xFF);
            *p++ = '.';
            p = app_int(p, (ip >> 16) & 0xFF);
            *p++ = '.';
            p = app_int(p, (ip >> 24) & 0xFF);
            *p = '\0';
        } else {
            p = app(p, "Not configured");
        }
    }

    // Line 13: Shell
    app(info[13], BCYN "Shell" RST ": MontaukOS Shell");

    // Line 15: Color palette (normal)
    {
        char* p = info[15];
        for (int c = 0; c < 8; c++) {
            p = app(p, "\033[4");
            *p++ = '0' + c;
            p = app(p, "m   ");
        }
        p = app(p, RST);
    }

    // Line 16: Color palette (bright)
    {
        char* p = info[16];
        for (int c = 0; c < 8; c++) {
            p = app(p, "\033[10");
            *p++ = '0' + c;
            p = app(p, "m   ");
        }
        p = app(p, RST);
    }

    // ==== Print output ====

    montauk::putchar('\n');
    for (int i = 0; i < NUM_LINES; i++) {
        montauk::print(art[i]);
        int p = INFO_COL - art_vw[i];
        if (p < 1) p = 1;
        pad(p);
        if (info[i][0]) montauk::print(info[i]);
        montauk::putchar('\n');
    }
    montauk::putchar('\n');

    montauk::exit(0);
}
