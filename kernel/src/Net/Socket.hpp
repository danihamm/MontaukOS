/*
    * Socket.hpp
    * Socket descriptor table for userspace TCP access
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <Net/Tcp.hpp>

namespace Net::Socket {

    static constexpr int SOCK_TCP = 1;
    static constexpr int SOCK_UDP = 2;
    static constexpr int MAX_SOCKETS = 64;

    struct SocketEntry {
        bool     Active;
        int      Type;
        int      OwnerPid;
        Tcp::Connection* TcpConn;
        uint16_t LocalPort;
    };

    void Initialize();

    // Create a socket of the given type. Returns fd or -1.
    int Create(int type, int pid);

    // Connect socket fd to remote ip:port. Returns 0 or -1.
    int Connect(int fd, uint32_t ip, uint16_t port, int pid);

    // Bind socket fd to a local port. Returns 0 or -1.
    int Bind(int fd, uint16_t port, int pid);

    // Start listening on a bound socket. Returns 0 or -1.
    int Listen(int fd, int pid);

    // Accept an incoming connection. Returns new fd or -1.
    int Accept(int fd, int pid);

    // Send data on a connected socket. Returns bytes sent or -1.
    int Send(int fd, const uint8_t* data, uint32_t len, int pid);

    // Receive data from a connected socket. Returns bytes received, 0 on close, or -1.
    int Recv(int fd, uint8_t* buf, uint32_t maxLen, int pid);

    // Close a socket.
    void Close(int fd, int pid);

    // Close all sockets owned by a process (called on process exit).
    void CleanupProcess(int pid);

}
