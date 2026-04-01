#pragma once

#include <cstdint>

namespace Net::Tcp { struct Connection; }

namespace Ipc {

    static constexpr int MaxHandlesPerProcess = 128;
    static constexpr int MaxSurfaceMapsPerProcess = 64;
    static constexpr uint32_t DefaultStreamCapacity = 4096;

    enum class HandleType : uint8_t {
        None = 0,
        Stream,
        Mailbox,
        File,
        Socket,
        Surface,
        Process,
        Waitset
    };

    enum Rights : uint32_t {
        RightWait   = 1u << 0,
        RightRead   = 1u << 1,
        RightWrite  = 1u << 2,
        RightSend   = 1u << 3,
        RightRecv   = 1u << 4,
        RightMap    = 1u << 5,
        RightManage = 1u << 6,
        RightDup    = 1u << 7
    };

    enum Signals : uint32_t {
        SignalNone       = 0,
        SignalReadable   = 1u << 0,
        SignalWritable   = 1u << 1,
        SignalPeerClosed = 1u << 2,
        SignalExited     = 1u << 3,
        SignalReady      = 1u << 4
    };

    struct Object;
    struct Stream;
    struct Mailbox;
    struct File;
    struct Socket;
    struct Surface;
    struct ProcessObject;
    struct Waitset;

    struct HandleEntry {
        bool used;
        uint32_t rights;
        HandleType type;
        Object* object;
    };

    struct WaitsetReady {
        int32_t index;
        uint32_t signals;
    };

    void Initialize();

    int CurrentSlot();
    int SlotForPid(int pid);

    int InstallHandleForSlot(int slot, Object* object, HandleType type, uint32_t rights);
    int CloseHandleForSlot(int slot, int handle);
    int CloseHandle(int handle);
    int DupHandle(int handle);

    bool SnapshotHandleForSlot(int slot, int handle, HandleType& type, Object*& object, uint32_t& rights);
    uint32_t GetHandleSignalsForSlot(int slot, int handle);

    Stream* CreateStream(uint32_t capacity = DefaultStreamCapacity);
    int CreateStreamHandlePairForSlot(int slot, uint32_t capacity, int& outReadHandle, int& outWriteHandle);
    int CreateStreamHandlePair(uint32_t capacity, int& outReadHandle, int& outWriteHandle);
    void RetainStream(Stream* stream, bool readSide, bool writeSide);
    void ReleaseStream(Stream* stream, bool readSide, bool writeSide);
    int StreamRead(Stream* stream, uint8_t* out, int maxLen, bool nonBlocking);
    int StreamWrite(Stream* stream, const uint8_t* data, int len, bool nonBlocking);
    int StreamReadHandle(int handle, uint8_t* out, int maxLen);
    int StreamWriteHandle(int handle, const uint8_t* data, int len);
    bool StreamHasData(Stream* stream);

    Mailbox* CreateMailbox();
    int CreateMailboxHandlePairForSlot(int slot, int& outSendHandle, int& outRecvHandle);
    int CreateMailboxHandlePair(int& outSendHandle, int& outRecvHandle);
    void RetainMailbox(Mailbox* mailbox, bool sender, bool receiver);
    void ReleaseMailbox(Mailbox* mailbox, bool sender, bool receiver);
    int MailboxSend(Mailbox* mailbox, uint32_t msgType, const void* data, uint16_t len);
    int MailboxRecv(Mailbox* mailbox, uint32_t* msgType, void* data, uint16_t* inOutLen, bool nonBlocking);
    int MailboxSendHandle(int handle, uint32_t msgType, const void* data, uint16_t len, int attachHandle);
    int MailboxRecvHandle(int handle, uint32_t* msgType, void* data, uint16_t* inOutLen, int* outAttachHandle);
    bool MailboxHasMessage(Mailbox* mailbox);

    int OpenFileHandleForSlot(int slot, const char* path, bool create);
    int OpenFileHandle(const char* path);
    int CreateFileHandle(const char* path);
    int FileReadHandle(int handle, uint8_t* buffer, uint64_t offset, uint64_t size);
    int FileWriteHandle(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size);
    uint64_t FileGetSizeHandle(int handle);

    int CreateSocketHandleForSlot(int slot, int type);
    int CreateSocketHandle(int type);
    int SocketConnectHandle(int handle, uint32_t ip, uint16_t port);
    int SocketBindHandle(int handle, uint16_t port);
    int SocketListenHandle(int handle);
    int SocketAcceptHandle(int handle);
    int SocketSendHandle(int handle, const uint8_t* data, uint32_t len);
    int SocketRecvHandle(int handle, uint8_t* buffer, uint32_t maxLen);
    int SocketSendToHandle(int handle, const uint8_t* data, uint32_t len, uint32_t destIp, uint16_t destPort);
    int SocketRecvFromHandle(int handle, uint8_t* buffer, uint32_t maxLen, uint32_t* srcIp, uint16_t* srcPort);
    void NotifyTcpConnectionChanged(Net::Tcp::Connection* connection);

    Surface* CreateSurface(uint64_t byteSize);
    int CreateSurfaceHandle(uint64_t byteSize);
    void RetainSurface(Surface* surface);
    void ReleaseSurface(Surface* surface);
    uint64_t GetSurfaceSize(const Surface* surface);
    int ResizeSurface(Surface* surface, uint64_t newSize);
    int CopySurface(Surface* dst, Surface* src);
    int CopySurfacePreserve(Surface* dst, int dstWidth, int dstHeight,
                            Surface* src, int srcWidth, int srcHeight,
                            uint32_t fillPixel);
    uint64_t MapSurfaceHandle(int handle);
    int ResizeSurfaceHandle(int handle, uint64_t newSize);
    int MapSurfaceForPid(Surface* surface, int pid, uint64_t pml4Phys, uint64_t& heapNext, uint64_t& outVa);
    int UnmapSurfaceForPid(Surface* surface, int pid, uint64_t pml4Phys);

    ProcessObject* GetProcessObject(int pid);
    int OpenProcessHandle(int pid);
    void ProcessStartedInSlot(int slot, int pid);
    void ProcessExitedInSlot(int slot, int pid);
    bool ProcessHasExited(ProcessObject* process);

    uint32_t WaitOnHandle(int handle, uint32_t wantedSignals, uint64_t timeoutMs);

    int CreateWaitsetHandleForSlot(int slot);
    int CreateWaitsetHandleForCurrent();
    int WaitsetAddHandleForSlot(int slot, int waitsetHandle, int targetHandle, uint32_t signals);
    int WaitsetAddHandle(int waitsetHandle, int targetHandle, uint32_t signals);
    int WaitsetRemoveIndexForSlot(int slot, int waitsetHandle, int index);
    int WaitsetRemoveIndex(int waitsetHandle, int index);
    int WaitsetWaitHandle(int waitsetHandle, WaitsetReady* outReady, uint64_t timeoutMs);

    void NotifyObjectChanged(Object* object);
    void CleanupProcessSlot(int slot, int pid, uint64_t pml4Phys);

}
