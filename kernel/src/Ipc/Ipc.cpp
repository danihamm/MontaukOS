#include "Ipc.hpp"

#include <Sched/Scheduler.hpp>
#include <Fs/Vfs.hpp>
#include <Net/Tcp.hpp>
#include <Net/Udp.hpp>
#include <Memory/PageFrameAllocator.hpp>
#include <Memory/HHDM.hpp>
#include <Memory/Paging.hpp>
#include <Libraries/Memory.hpp>
#include <CppLib/Spinlock.hpp>
#include <Timekeeping/ApicTimer.hpp>
#include <Terminal/Terminal.hpp>

namespace Ipc {

    static constexpr int MaxStreams = 64;
    static constexpr int MaxMailboxes = 64;
    static constexpr int MaxMailboxMessages = 64;
    static constexpr int MaxMailboxMessageBytes = 128;
    static constexpr int MaxFiles = 128;
    static constexpr int MaxSockets = 64;
    static constexpr uint32_t UdpRingSize = 4096;
    static constexpr int MaxSurfaces = 32;
    static constexpr int MaxSurfacePages = 8192;
    static constexpr int MaxProcessObjects = 512;
    static constexpr int MaxWaitsets = 32;
    static constexpr int MaxWaitsetEntries = 16;
    static constexpr int SocketTypeTcp = 1;
    static constexpr int SocketTypeUdp = 2;

    struct Object {
        HandleType type;
        bool active;
        uint32_t refs;
        kcp::Mutex lock;
    };

    struct Stream : Object {
        uint8_t* buffer;
        uint32_t capacity;
        uint32_t head;
        uint32_t tail;
        uint32_t count;
        uint32_t readerRefs;
        uint32_t writerRefs;
    };

    struct MailboxMessage {
        uint32_t type;
        uint16_t size;
        uint8_t  attachmentType;
        uint8_t  hasAttachment;
        uint32_t attachmentRights;
        Object*  attachmentObject;
        uint8_t  data[MaxMailboxMessageBytes];
    };

    struct Mailbox : Object {
        MailboxMessage messages[MaxMailboxMessages];
        uint32_t head;
        uint32_t tail;
        uint32_t count;
        uint32_t senderRefs;
        uint32_t receiverRefs;
    };

    struct File : Object {
        Fs::Vfs::BackendFile backend;
    };

    struct UdpDgramHeader {
        uint32_t srcIp;
        uint16_t srcPort;
        uint16_t dataLen;
    };

    struct Socket : Object {
        int socketType;
        Net::Tcp::Connection* tcpConn;
        uint16_t localPort;
        bool udpBound;
        uint8_t _pad[1];
        uint8_t udpRing[UdpRingSize];
        uint32_t udpHead;
        uint32_t udpTail;
        uint32_t udpCount;
        kcp::Spinlock socketLock;
    };

    struct Surface : Object {
        uint64_t physPages[MaxSurfacePages];
        uint32_t numPages;
        uint64_t sizeBytes;
    };

    struct ProcessObject : Object {
        int pid;
        bool exited;
    };

    struct WaitsetEntry {
        bool used;
        HandleType type;
        Object* object;
        uint32_t rights;
        uint32_t signals;
    };

    struct Waitset : Object {
        WaitsetEntry entries[MaxWaitsetEntries];
    };

    struct SurfaceMap {
        bool used;
        Surface* surface;
        uint64_t va;
        uint32_t numPages;
    };

    static inline uint32_t* SurfacePixelPtr(Surface* surface, uint64_t pixelIndex) {
        if (surface == nullptr) return nullptr;
        uint64_t byteOffset = pixelIndex * sizeof(uint32_t);
        uint32_t pageIndex = (uint32_t)(byteOffset / 0x1000ULL);
        uint32_t pageOffset = (uint32_t)(byteOffset % 0x1000ULL);
        if (pageIndex >= surface->numPages) return nullptr;
        return (uint32_t*)((uint8_t*)Memory::HHDM(surface->physPages[pageIndex]) + pageOffset);
    }

    static HandleEntry g_handleTables[Sched::MaxProcesses][MaxHandlesPerProcess] = {};
    static SurfaceMap g_surfaceMaps[Sched::MaxProcesses][MaxSurfaceMapsPerProcess] = {};

    static Stream g_streams[MaxStreams] = {};
    static Mailbox g_mailboxes[MaxMailboxes] = {};
    static File g_files[MaxFiles] = {};
    static Socket g_sockets[MaxSockets] = {};
    static Surface g_surfaces[MaxSurfaces] = {};
    static ProcessObject g_processObjects[MaxProcessObjects] = {};
    static Waitset g_waitsets[MaxWaitsets] = {};
    static ProcessObject* g_processObjectsBySlot[Sched::MaxProcesses] = {};

    static kcp::Mutex g_streamPoolLock;
    static kcp::Mutex g_mailboxPoolLock;
    static kcp::Mutex g_filePoolLock;
    static kcp::Mutex g_socketPoolLock;
    static kcp::Mutex g_surfacePoolLock;
    static kcp::Mutex g_processPoolLock;
    static kcp::Mutex g_waitsetPoolLock;
    static kcp::Mutex g_ephemeralPortLock;
    static uint16_t g_nextEphemeralPort = 49152;

    static void ReleaseRawObject(Object* object);

    static void InitObject(Object& object, HandleType type) {
        object.type = type;
        object.active = true;
        object.refs = 0;
    }

    static void RetainRawObject(Object* object) {
        if (object == nullptr) return;
        object->lock.Acquire();
        object->refs++;
        object->lock.Release();
    }

    static void DestroyStream(Stream* stream) {
        if (stream->buffer != nullptr) {
            int numPages = (int)((stream->capacity + 0xFFFu) / 0x1000u);
            Memory::g_pfa->Free(stream->buffer, numPages);
            stream->buffer = nullptr;
        }
        stream->capacity = 0;
        stream->head = 0;
        stream->tail = 0;
        stream->count = 0;
        stream->readerRefs = 0;
        stream->writerRefs = 0;
    }

    static void DestroyFile(File* file) {
        if (file == nullptr) return;
        Fs::Vfs::CloseBackendFile(file->backend);
        file->backend.driveNumber = -1;
        file->backend.localHandle = -1;
    }

    static void DestroyMailbox(Mailbox* mailbox) {
        if (mailbox == nullptr) return;
        for (uint32_t i = 0; i < MaxMailboxMessages; i++) {
            if (!mailbox->messages[i].hasAttachment || mailbox->messages[i].attachmentObject == nullptr) continue;
            ReleaseRawObject(mailbox->messages[i].attachmentObject);
            mailbox->messages[i].attachmentObject = nullptr;
            mailbox->messages[i].attachmentType = (uint8_t)HandleType::None;
            mailbox->messages[i].attachmentRights = 0;
            mailbox->messages[i].hasAttachment = 0;
        }
        mailbox->head = 0;
        mailbox->tail = 0;
        mailbox->count = 0;
        mailbox->senderRefs = 0;
        mailbox->receiverRefs = 0;
    }

    static void DestroySocket(Socket* socket) {
        if (socket == nullptr) return;

        socket->socketLock.Acquire();
        Net::Tcp::Connection* tcpConn = socket->tcpConn;
        bool udpBound = socket->udpBound;
        uint16_t localPort = socket->localPort;
        socket->tcpConn = nullptr;
        socket->udpBound = false;
        socket->localPort = 0;
        socket->udpHead = 0;
        socket->udpTail = 0;
        socket->udpCount = 0;
        socket->socketType = 0;
        socket->socketLock.Release();

        if (tcpConn != nullptr) {
            Net::Tcp::Close(tcpConn);
        }
        if (udpBound && localPort != 0) {
            Net::Udp::Unbind(localPort);
        }
    }

    static void DestroySurface(Surface* surface) {
        for (uint32_t i = 0; i < surface->numPages; i++) {
            if (surface->physPages[i] != 0) {
                Memory::g_pfa->Free((void*)Memory::HHDM(surface->physPages[i]));
                surface->physPages[i] = 0;
            }
        }
        surface->numPages = 0;
        surface->sizeBytes = 0;
    }

    static void DestroyWaitset(Waitset* waitset) {
        for (int i = 0; i < MaxWaitsetEntries; i++) {
            if (!waitset->entries[i].used) continue;
            ReleaseRawObject(waitset->entries[i].object);
            waitset->entries[i].used = false;
            waitset->entries[i].object = nullptr;
            waitset->entries[i].rights = 0;
            waitset->entries[i].signals = 0;
            waitset->entries[i].type = HandleType::None;
        }
    }

    static void ReleaseRawObject(Object* object) {
        if (object == nullptr) return;

        bool destroy = false;
        HandleType type = HandleType::None;

        object->lock.Acquire();
        if (object->refs > 0) {
            object->refs--;
        }
        if (object->refs == 0) {
            destroy = true;
            type = object->type;
            object->active = false;
        }
        object->lock.Release();

        if (!destroy) return;

        switch (type) {
            case HandleType::Stream:
                DestroyStream((Stream*)object);
                break;
            case HandleType::Mailbox:
                DestroyMailbox((Mailbox*)object);
                break;
            case HandleType::File:
                DestroyFile((File*)object);
                break;
            case HandleType::Socket:
                DestroySocket((Socket*)object);
                break;
            case HandleType::Surface:
                DestroySurface((Surface*)object);
                break;
            case HandleType::Process:
                break;
            case HandleType::Waitset:
                DestroyWaitset((Waitset*)object);
                break;
            default:
                break;
        }
    }

    static uint32_t CurrentSignalsForSnapshot(HandleType type, Object* object, uint32_t rights);
    static uint32_t CurrentSocketSignals(Socket* socket, uint32_t rights);
    static bool WaitsetCheckReady(Waitset* waitset, WaitsetReady* outReady);

    static uint16_t AllocEphemeralPort() {
        g_ephemeralPortLock.Acquire();
        uint16_t port = g_nextEphemeralPort++;
        if (g_nextEphemeralPort == 0) g_nextEphemeralPort = 49152;
        g_ephemeralPortLock.Release();
        return port;
    }

    static void UdpSocketDispatcher(uint32_t srcIp, uint16_t srcPort,
                                    uint16_t dstPort,
                                    const uint8_t* data, uint16_t length) {
        for (int i = 0; i < MaxSockets; i++) {
            if (!g_sockets[i].active || g_sockets[i].socketType != SocketTypeUdp) continue;

            g_sockets[i].socketLock.Acquire();
            if (!g_sockets[i].udpBound || g_sockets[i].localPort != dstPort) {
                g_sockets[i].socketLock.Release();
                continue;
            }

            uint32_t needed = sizeof(UdpDgramHeader) + length;
            if (g_sockets[i].udpCount + needed > UdpRingSize) {
                g_sockets[i].socketLock.Release();
                return;
            }

            UdpDgramHeader hdr = {srcIp, srcPort, length};
            const uint8_t* hdrBytes = (const uint8_t*)&hdr;
            for (uint32_t j = 0; j < sizeof(UdpDgramHeader); j++) {
                g_sockets[i].udpRing[g_sockets[i].udpTail] = hdrBytes[j];
                g_sockets[i].udpTail = (g_sockets[i].udpTail + 1) % UdpRingSize;
            }
            for (uint16_t j = 0; j < length; j++) {
                g_sockets[i].udpRing[g_sockets[i].udpTail] = data[j];
                g_sockets[i].udpTail = (g_sockets[i].udpTail + 1) % UdpRingSize;
            }
            g_sockets[i].udpCount += needed;
            g_sockets[i].socketLock.Release();

            NotifyObjectChanged((Object*)&g_sockets[i]);
            return;
        }
    }

    int CurrentSlot() {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return -1;
        auto* slot0 = Sched::GetProcessSlot(0);
        return (int)(proc - slot0);
    }

    int SlotForPid(int pid) {
        for (int i = 0; i < Sched::MaxProcesses; i++) {
            auto* proc = Sched::GetProcessSlot(i);
            if (proc == nullptr) continue;
            auto state = proc->state;
            if (state == Sched::ProcessState::Free) continue;
            if (proc->pid == pid) return i;
        }
        return -1;
    }

    static void* AllocContiguousPages(int numPages) {
        if (numPages <= 0) return nullptr;
        void* first = Memory::g_pfa->AllocateZeroed();
        if (first == nullptr) return nullptr;
        if (numPages == 1) return first;
        void* span = Memory::g_pfa->ReallocConsecutive(first, numPages);
        if (span == nullptr) {
            Memory::g_pfa->Free(first);
            return nullptr;
        }
        return span;
    }

    static void RetainForHandle(Object* object, HandleType type, uint32_t rights) {
        if (object == nullptr) return;

        switch (type) {
            case HandleType::Stream: {
                auto* stream = (Stream*)object;
                stream->lock.Acquire();
                stream->refs++;
                if (rights & RightRead) stream->readerRefs++;
                if (rights & RightWrite) stream->writerRefs++;
                stream->lock.Release();
                break;
            }
            case HandleType::Mailbox: {
                auto* mailbox = (Mailbox*)object;
                mailbox->lock.Acquire();
                mailbox->refs++;
                if (rights & RightSend) mailbox->senderRefs++;
                if (rights & RightRecv) mailbox->receiverRefs++;
                mailbox->lock.Release();
                break;
            }
            default:
                RetainRawObject(object);
                break;
        }
    }

    static void ReleaseForHandle(Object* object, HandleType type, uint32_t rights) {
        if (object == nullptr) return;

        bool notify = false;
        bool destroy = false;

        switch (type) {
            case HandleType::Stream: {
                auto* stream = (Stream*)object;
                stream->lock.Acquire();
                if (rights & RightRead && stream->readerRefs > 0) {
                    stream->readerRefs--;
                    notify = true;
                }
                if (rights & RightWrite && stream->writerRefs > 0) {
                    stream->writerRefs--;
                    notify = true;
                }
                if (stream->refs > 0) stream->refs--;
                if (stream->refs == 0) {
                    destroy = true;
                    stream->active = false;
                }
                stream->lock.Release();

                if (notify) NotifyObjectChanged((Object*)stream);
                if (destroy) DestroyStream(stream);
                break;
            }
            case HandleType::Mailbox: {
                auto* mailbox = (Mailbox*)object;
                mailbox->lock.Acquire();
                if (rights & RightSend && mailbox->senderRefs > 0) {
                    mailbox->senderRefs--;
                    notify = true;
                }
                if (rights & RightRecv && mailbox->receiverRefs > 0) {
                    mailbox->receiverRefs--;
                    notify = true;
                }
                if (mailbox->refs > 0) mailbox->refs--;
                if (mailbox->refs == 0) {
                    destroy = true;
                    mailbox->active = false;
                }
                mailbox->lock.Release();

                if (notify) NotifyObjectChanged((Object*)mailbox);
                if (destroy) DestroyMailbox(mailbox);
                break;
            }
            default:
                ReleaseRawObject(object);
                break;
        }
    }

    int InstallHandleForSlot(int slot, Object* object, HandleType type, uint32_t rights) {
        if (slot < 0 || slot >= Sched::MaxProcesses || object == nullptr) return -1;

        for (int i = 0; i < MaxHandlesPerProcess; i++) {
            if (g_handleTables[slot][i].used) continue;
            g_handleTables[slot][i].used = true;
            g_handleTables[slot][i].rights = rights;
            g_handleTables[slot][i].type = type;
            g_handleTables[slot][i].object = object;
            RetainForHandle(object, type, rights);
            return i;
        }
        return -1;
    }

    bool SnapshotHandleForSlot(int slot, int handle, HandleType& type, Object*& object, uint32_t& rights) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return false;
        if (handle < 0 || handle >= MaxHandlesPerProcess) return false;

        const HandleEntry& entry = g_handleTables[slot][handle];
        if (!entry.used || entry.object == nullptr) return false;

        type = entry.type;
        object = entry.object;
        rights = entry.rights;
        return true;
    }

    int CloseHandleForSlot(int slot, int handle) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return -1;
        if (handle < 0 || handle >= MaxHandlesPerProcess) return -1;
        if (!g_handleTables[slot][handle].used) return -1;

        HandleEntry entry = g_handleTables[slot][handle];
        g_handleTables[slot][handle].used = false;
        g_handleTables[slot][handle].rights = 0;
        g_handleTables[slot][handle].type = HandleType::None;
        g_handleTables[slot][handle].object = nullptr;

        ReleaseForHandle(entry.object, entry.type, entry.rights);
        return 0;
    }

    int CloseHandle(int handle) {
        return CloseHandleForSlot(CurrentSlot(), handle);
    }

    int DupHandle(int handle) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        int slot = CurrentSlot();
        if (!SnapshotHandleForSlot(slot, handle, type, object, rights)) return -1;
        if ((rights & RightDup) == 0) return -1;
        return InstallHandleForSlot(slot, object, type, rights);
    }

    Stream* CreateStream(uint32_t capacity) {
        if (capacity == 0) capacity = DefaultStreamCapacity;

        int numPages = (int)((capacity + 0xFFFu) / 0x1000u);
        void* buffer = AllocContiguousPages(numPages);
        if (buffer == nullptr) return nullptr;

        g_streamPoolLock.Acquire();
        for (int i = 0; i < MaxStreams; i++) {
            if (g_streams[i].active) continue;
            InitObject(g_streams[i], HandleType::Stream);
            g_streams[i].buffer = (uint8_t*)buffer;
            g_streams[i].capacity = (uint32_t)numPages * 0x1000u;
            g_streams[i].head = 0;
            g_streams[i].tail = 0;
            g_streams[i].count = 0;
            g_streams[i].readerRefs = 0;
            g_streams[i].writerRefs = 0;
            g_streamPoolLock.Release();
            return &g_streams[i];
        }
        g_streamPoolLock.Release();

        Memory::g_pfa->Free(buffer, numPages);
        return nullptr;
    }

    int CreateStreamHandlePairForSlot(int slot, uint32_t capacity, int& outReadHandle, int& outWriteHandle) {
        outReadHandle = -1;
        outWriteHandle = -1;

        Stream* stream = CreateStream(capacity);
        if (stream == nullptr) return -1;

        int readHandle = InstallHandleForSlot(slot, (Object*)stream, HandleType::Stream,
                                              RightRead | RightWait | RightDup);
        if (readHandle < 0) {
            DestroyStream(stream);
            stream->active = false;
            return -1;
        }

        int writeHandle = InstallHandleForSlot(slot, (Object*)stream, HandleType::Stream,
                                               RightWrite | RightWait | RightDup);
        if (writeHandle < 0) {
            CloseHandleForSlot(slot, readHandle);
            return -1;
        }

        outReadHandle = readHandle;
        outWriteHandle = writeHandle;
        return 0;
    }

    int CreateStreamHandlePair(uint32_t capacity, int& outReadHandle, int& outWriteHandle) {
        return CreateStreamHandlePairForSlot(CurrentSlot(), capacity, outReadHandle, outWriteHandle);
    }

    void RetainStream(Stream* stream, bool readSide, bool writeSide) {
        if (stream == nullptr) return;
        stream->lock.Acquire();
        stream->refs++;
        if (readSide) stream->readerRefs++;
        if (writeSide) stream->writerRefs++;
        stream->lock.Release();
    }

    void ReleaseStream(Stream* stream, bool readSide, bool writeSide) {
        if (stream == nullptr) return;
        ReleaseForHandle((Object*)stream, HandleType::Stream,
                         (readSide ? (uint32_t)RightRead : 0u) |
                         (writeSide ? (uint32_t)RightWrite : 0u));
    }

    int StreamRead(Stream* stream, uint8_t* out, int maxLen, bool /*nonBlocking*/) {
        if (stream == nullptr || out == nullptr || maxLen <= 0) return -1;

        stream->lock.Acquire();
        if (stream->count == 0) {
            bool closed = stream->writerRefs == 0;
            stream->lock.Release();
            return closed ? -1 : 0;
        }

        int count = 0;
        while (count < maxLen && stream->count > 0) {
            out[count++] = stream->buffer[stream->tail];
            stream->tail = (stream->tail + 1) % stream->capacity;
            stream->count--;
        }
        stream->lock.Release();

        NotifyObjectChanged((Object*)stream);
        return count;
    }

    int StreamWrite(Stream* stream, const uint8_t* data, int len, bool /*nonBlocking*/) {
        if (stream == nullptr || data == nullptr || len <= 0) return -1;

        stream->lock.Acquire();
        if (stream->readerRefs == 0) {
            stream->lock.Release();
            return -1;
        }

        int written = 0;
        while (written < len && stream->count < stream->capacity) {
            stream->buffer[stream->head] = data[written++];
            stream->head = (stream->head + 1) % stream->capacity;
            stream->count++;
        }
        stream->lock.Release();

        if (written > 0) NotifyObjectChanged((Object*)stream);
        return written;
    }

    int StreamReadHandle(int handle, uint8_t* out, int maxLen) {
        if (out == nullptr) return -1;

        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return -1;
        if (type != HandleType::Stream || (rights & RightRead) == 0) return -1;
        return StreamRead((Stream*)object, out, maxLen, true);
    }

    int StreamWriteHandle(int handle, const uint8_t* data, int len) {
        if (data == nullptr) return -1;

        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return -1;
        if (type != HandleType::Stream || (rights & RightWrite) == 0) return -1;
        return StreamWrite((Stream*)object, data, len, true);
    }

    bool StreamHasData(Stream* stream) {
        if (stream == nullptr) return false;
        stream->lock.Acquire();
        bool hasData = stream->count > 0;
        stream->lock.Release();
        return hasData;
    }

    Mailbox* CreateMailbox() {
        g_mailboxPoolLock.Acquire();
        for (int i = 0; i < MaxMailboxes; i++) {
            if (g_mailboxes[i].active) continue;
            InitObject(g_mailboxes[i], HandleType::Mailbox);
            g_mailboxes[i].head = 0;
            g_mailboxes[i].tail = 0;
            g_mailboxes[i].count = 0;
            g_mailboxes[i].senderRefs = 0;
            g_mailboxes[i].receiverRefs = 0;
            for (int j = 0; j < MaxMailboxMessages; j++) {
                g_mailboxes[i].messages[j].type = 0;
                g_mailboxes[i].messages[j].size = 0;
                g_mailboxes[i].messages[j].attachmentType = (uint8_t)HandleType::None;
                g_mailboxes[i].messages[j].hasAttachment = 0;
                g_mailboxes[i].messages[j].attachmentRights = 0;
                g_mailboxes[i].messages[j].attachmentObject = nullptr;
            }
            g_mailboxPoolLock.Release();
            return &g_mailboxes[i];
        }
        g_mailboxPoolLock.Release();
        return nullptr;
    }

    int CreateMailboxHandlePairForSlot(int slot, int& outSendHandle, int& outRecvHandle) {
        outSendHandle = -1;
        outRecvHandle = -1;

        Mailbox* mailbox = CreateMailbox();
        if (mailbox == nullptr) return -1;

        int sendHandle = InstallHandleForSlot(slot, (Object*)mailbox, HandleType::Mailbox,
                                              RightSend | RightWait | RightDup);
        if (sendHandle < 0) {
            DestroyMailbox(mailbox);
            mailbox->active = false;
            return -1;
        }

        int recvHandle = InstallHandleForSlot(slot, (Object*)mailbox, HandleType::Mailbox,
                                              RightRecv | RightWait | RightDup);
        if (recvHandle < 0) {
            CloseHandleForSlot(slot, sendHandle);
            return -1;
        }

        outSendHandle = sendHandle;
        outRecvHandle = recvHandle;
        return 0;
    }

    int CreateMailboxHandlePair(int& outSendHandle, int& outRecvHandle) {
        return CreateMailboxHandlePairForSlot(CurrentSlot(), outSendHandle, outRecvHandle);
    }

    void RetainMailbox(Mailbox* mailbox, bool sender, bool receiver) {
        if (mailbox == nullptr) return;
        mailbox->lock.Acquire();
        mailbox->refs++;
        if (sender) mailbox->senderRefs++;
        if (receiver) mailbox->receiverRefs++;
        mailbox->lock.Release();
    }

    void ReleaseMailbox(Mailbox* mailbox, bool sender, bool receiver) {
        if (mailbox == nullptr) return;
        ReleaseForHandle((Object*)mailbox, HandleType::Mailbox,
                         (sender ? (uint32_t)RightSend : 0u) |
                         (receiver ? (uint32_t)RightRecv : 0u));
    }

    static int MailboxSendInternal(Mailbox* mailbox, int senderSlot, uint32_t msgType,
                                   const void* data, uint16_t len, int attachHandle) {
        if (mailbox == nullptr) return -1;
        if (len > MaxMailboxMessageBytes) return -1;

        HandleType attachmentType = HandleType::None;
        Object* attachmentObject = nullptr;
        uint32_t attachmentRights = 0;
        if (attachHandle >= 0) {
            if (senderSlot < 0) return -1;
            if (!SnapshotHandleForSlot(senderSlot, attachHandle, attachmentType, attachmentObject, attachmentRights)) {
                return -1;
            }
            if ((attachmentRights & RightDup) == 0 || attachmentObject == nullptr) return -1;
            RetainRawObject(attachmentObject);
        }

        mailbox->lock.Acquire();
        if (mailbox->receiverRefs == 0) {
            mailbox->lock.Release();
            if (attachmentObject != nullptr) ReleaseRawObject(attachmentObject);
            return -1;
        }
        if (mailbox->count >= MaxMailboxMessages) {
            mailbox->lock.Release();
            if (attachmentObject != nullptr) ReleaseRawObject(attachmentObject);
            return 0;
        }

        MailboxMessage& msg = mailbox->messages[mailbox->head];
        msg.type = msgType;
        msg.size = len;
        msg.attachmentType = (uint8_t)attachmentType;
        msg.hasAttachment = attachmentObject != nullptr ? 1 : 0;
        msg.attachmentRights = attachmentRights;
        msg.attachmentObject = attachmentObject;
        if (len > 0 && data != nullptr) {
            memcpy(msg.data, data, len);
        }
        mailbox->head = (mailbox->head + 1) % MaxMailboxMessages;
        mailbox->count++;
        mailbox->lock.Release();

        NotifyObjectChanged((Object*)mailbox);
        return len;
    }

    int MailboxSend(Mailbox* mailbox, uint32_t msgType, const void* data, uint16_t len) {
        return MailboxSendInternal(mailbox, -1, msgType, data, len, -1);
    }

    static int MailboxRecvInternal(Mailbox* mailbox, int receiverSlot, uint32_t* msgType,
                                   void* data, uint16_t* inOutLen, int* outAttachHandle,
                                   bool /*nonBlocking*/) {
        if (mailbox == nullptr || inOutLen == nullptr) return -1;

        mailbox->lock.Acquire();
        if (mailbox->count == 0) {
            bool closed = mailbox->senderRefs == 0;
            mailbox->lock.Release();
            return closed ? -1 : 0;
        }

        MailboxMessage& msg = mailbox->messages[mailbox->tail];
        int attachedHandle = -1;
        if (msg.hasAttachment) {
            if (receiverSlot < 0 || outAttachHandle == nullptr) {
                mailbox->lock.Release();
                return -1;
            }

            attachedHandle = InstallHandleForSlot(receiverSlot, msg.attachmentObject,
                                                  (HandleType)msg.attachmentType,
                                                  msg.attachmentRights);
            if (attachedHandle < 0) {
                mailbox->lock.Release();
                return -1;
            }
        }

        uint16_t copyLen = msg.size;
        if (copyLen > *inOutLen) copyLen = *inOutLen;
        if (copyLen > 0 && data != nullptr) {
            memcpy(data, msg.data, copyLen);
        }
        if (msgType != nullptr) *msgType = msg.type;
        *inOutLen = copyLen;
        if (outAttachHandle != nullptr) *outAttachHandle = attachedHandle;

        mailbox->tail = (mailbox->tail + 1) % MaxMailboxMessages;
        mailbox->count--;
        Object* attachedObject = msg.attachmentObject;
        bool hadAttachment = msg.hasAttachment != 0;
        msg.attachmentType = (uint8_t)HandleType::None;
        msg.hasAttachment = 0;
        msg.attachmentRights = 0;
        msg.attachmentObject = nullptr;
        mailbox->lock.Release();

        if (hadAttachment && attachedObject != nullptr) {
            ReleaseRawObject(attachedObject);
        }
        NotifyObjectChanged((Object*)mailbox);
        return (int)copyLen;
    }

    int MailboxRecv(Mailbox* mailbox, uint32_t* msgType, void* data, uint16_t* inOutLen, bool nonBlocking) {
        return MailboxRecvInternal(mailbox, -1, msgType, data, inOutLen, nullptr, nonBlocking);
    }

    int MailboxSendHandle(int handle, uint32_t msgType, const void* data, uint16_t len, int attachHandle) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        int slot = CurrentSlot();
        if (!SnapshotHandleForSlot(slot, handle, type, object, rights)) return -1;
        if (type != HandleType::Mailbox || (rights & RightSend) == 0) return -1;
        return MailboxSendInternal((Mailbox*)object, slot, msgType, data, len, attachHandle);
    }

    int MailboxRecvHandle(int handle, uint32_t* msgType, void* data, uint16_t* inOutLen, int* outAttachHandle) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        int slot = CurrentSlot();
        if (!SnapshotHandleForSlot(slot, handle, type, object, rights)) return -1;
        if (type != HandleType::Mailbox || (rights & RightRecv) == 0) return -1;
        return MailboxRecvInternal((Mailbox*)object, slot, msgType, data, inOutLen, outAttachHandle, true);
    }

    bool MailboxHasMessage(Mailbox* mailbox) {
        if (mailbox == nullptr) return false;
        mailbox->lock.Acquire();
        bool hasMsg = mailbox->count > 0;
        mailbox->lock.Release();
        return hasMsg;
    }

    int OpenFileHandleForSlot(int slot, const char* path, bool create) {
        if (slot < 0 || slot >= Sched::MaxProcesses || path == nullptr) return -1;

        Fs::Vfs::BackendFile backend = {-1, -1};
        int result = create ? Fs::Vfs::CreateBackendFile(path, backend)
                            : Fs::Vfs::OpenBackendFile(path, backend);
        if (result < 0) return -1;

        g_filePoolLock.Acquire();
        for (int i = 0; i < MaxFiles; i++) {
            if (g_files[i].active) continue;
            InitObject(g_files[i], HandleType::File);
            g_files[i].backend = backend;
            g_filePoolLock.Release();

            uint32_t rights = RightRead | RightWait | RightDup;
            if (Fs::Vfs::BackendFileCanWrite(g_files[i].backend)) {
                rights |= RightWrite;
            }

            int handle = InstallHandleForSlot(slot, (Object*)&g_files[i], HandleType::File, rights);
            if (handle >= 0) return handle;

            DestroyFile(&g_files[i]);
            g_files[i].active = false;
            return -1;
        }
        g_filePoolLock.Release();

        Fs::Vfs::CloseBackendFile(backend);
        return -1;
    }

    int OpenFileHandle(const char* path) {
        return OpenFileHandleForSlot(CurrentSlot(), path, false);
    }

    int CreateFileHandle(const char* path) {
        return OpenFileHandleForSlot(CurrentSlot(), path, true);
    }

    int FileReadHandle(int handle, uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (buffer == nullptr) return -1;

        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return -1;
        if (type != HandleType::File || (rights & RightRead) == 0) return -1;
        return Fs::Vfs::ReadBackendFile(((File*)object)->backend, buffer, offset, size);
    }

    int FileWriteHandle(int handle, const uint8_t* buffer, uint64_t offset, uint64_t size) {
        if (buffer == nullptr) return -1;

        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return -1;
        if (type != HandleType::File || (rights & RightWrite) == 0) return -1;
        return Fs::Vfs::WriteBackendFile(((File*)object)->backend, buffer, offset, size);
    }

    uint64_t FileGetSizeHandle(int handle) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return 0;
        if (type != HandleType::File || (rights & RightRead) == 0) return 0;
        return Fs::Vfs::GetBackendFileSize(((File*)object)->backend);
    }

    static Socket* AllocateSocketObject(int type) {
        g_socketPoolLock.Acquire();
        for (int i = 0; i < MaxSockets; i++) {
            if (g_sockets[i].active) continue;
            InitObject(g_sockets[i], HandleType::Socket);
            g_sockets[i].socketType = type;
            g_sockets[i].tcpConn = nullptr;
            g_sockets[i].localPort = 0;
            g_sockets[i].udpBound = false;
            g_sockets[i].udpHead = 0;
            g_sockets[i].udpTail = 0;
            g_sockets[i].udpCount = 0;
            g_socketPoolLock.Release();
            return &g_sockets[i];
        }
        g_socketPoolLock.Release();
        return nullptr;
    }

    static bool SnapshotSocketHandle(int handle, Socket*& outSocket, uint32_t& outRights) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, outRights)) return false;
        if (type != HandleType::Socket || object == nullptr) return false;
        outSocket = (Socket*)object;
        return true;
    }

    int CreateSocketHandleForSlot(int slot, int type) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return -1;
        if (type != SocketTypeTcp && type != SocketTypeUdp) return -1;

        Socket* socket = AllocateSocketObject(type);
        if (socket == nullptr) return -1;

        int handle = InstallHandleForSlot(slot, (Object*)socket, HandleType::Socket,
                                          RightRead | RightWrite | RightWait | RightManage | RightDup);
        if (handle >= 0) return handle;

        DestroySocket(socket);
        socket->active = false;
        return -1;
    }

    int CreateSocketHandle(int type) {
        return CreateSocketHandleForSlot(CurrentSlot(), type);
    }

    int SocketConnectHandle(int handle, uint32_t ip, uint16_t port) {
        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightManage) == 0 || socket->socketType != SocketTypeTcp) return -1;

        socket->socketLock.Acquire();
        if (socket->tcpConn != nullptr) {
            socket->socketLock.Release();
            return -1;
        }
        uint16_t localPort = AllocEphemeralPort();
        socket->localPort = localPort;
        socket->socketLock.Release();

        Net::Tcp::Connection* conn = Net::Tcp::Connect(ip, port, localPort);
        if (conn == nullptr) {
            socket->socketLock.Acquire();
            if (socket->tcpConn == nullptr) socket->localPort = 0;
            socket->socketLock.Release();
            NotifyObjectChanged((Object*)socket);
            return -1;
        }

        socket->socketLock.Acquire();
        if (socket->tcpConn != nullptr) {
            socket->socketLock.Release();
            Net::Tcp::Close(conn);
            return -1;
        }
        socket->tcpConn = conn;
        socket->socketLock.Release();

        NotifyObjectChanged((Object*)socket);
        return 0;
    }

    int SocketBindHandle(int handle, uint16_t port) {
        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightManage) == 0) return -1;

        if (socket->socketType == SocketTypeTcp) {
            socket->socketLock.Acquire();
            if (socket->tcpConn != nullptr) {
                socket->socketLock.Release();
                return -1;
            }
            socket->localPort = port;
            socket->socketLock.Release();
            NotifyObjectChanged((Object*)socket);
            return 0;
        }

        if (socket->socketType != SocketTypeUdp) return -1;

        socket->socketLock.Acquire();
        bool alreadyBound = socket->udpBound;
        uint16_t oldPort = socket->localPort;
        socket->socketLock.Release();

        if (alreadyBound && oldPort == port) {
            return 0;
        }

        if (alreadyBound && oldPort != 0 && oldPort != port) {
            Net::Udp::Unbind(oldPort);
        }

        if (!Net::Udp::Bind(port, UdpSocketDispatcher)) {
            if (alreadyBound && oldPort != 0 && oldPort != port) {
                Net::Udp::Bind(oldPort, UdpSocketDispatcher);
            }
            return -1;
        }

        socket->socketLock.Acquire();
        socket->localPort = port;
        socket->udpBound = true;
        socket->socketLock.Release();
        NotifyObjectChanged((Object*)socket);
        return 0;
    }

    int SocketListenHandle(int handle) {
        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightManage) == 0 || socket->socketType != SocketTypeTcp) return -1;

        socket->socketLock.Acquire();
        uint16_t localPort = socket->localPort;
        bool busy = socket->tcpConn != nullptr;
        socket->socketLock.Release();
        if (busy || localPort == 0) return -1;

        Net::Tcp::Connection* listener = Net::Tcp::Listen(localPort);
        if (listener == nullptr) return -1;

        socket->socketLock.Acquire();
        if (socket->tcpConn != nullptr) {
            socket->socketLock.Release();
            Net::Tcp::Close(listener);
            return -1;
        }
        socket->tcpConn = listener;
        socket->socketLock.Release();
        NotifyObjectChanged((Object*)socket);
        return 0;
    }

    int SocketAcceptHandle(int handle) {
        int slot = CurrentSlot();
        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightManage) == 0 || socket->socketType != SocketTypeTcp) return -1;

        socket->socketLock.Acquire();
        Net::Tcp::Connection* listener = socket->tcpConn;
        uint16_t localPort = socket->localPort;
        socket->socketLock.Release();
        if (listener == nullptr || Net::Tcp::GetState(listener) != Net::Tcp::State::Listen) return -1;

        Net::Tcp::Connection* clientConn = Net::Tcp::Accept(listener);
        if (clientConn == nullptr) return -1;

        Socket* accepted = AllocateSocketObject(SocketTypeTcp);
        if (accepted == nullptr) {
            Net::Tcp::Close(clientConn);
            return -1;
        }

        accepted->socketLock.Acquire();
        accepted->tcpConn = clientConn;
        accepted->localPort = localPort;
        accepted->socketLock.Release();

        int acceptedHandle = InstallHandleForSlot(slot, (Object*)accepted, HandleType::Socket,
                                                  RightRead | RightWrite | RightWait | RightManage | RightDup);
        if (acceptedHandle < 0) {
            DestroySocket(accepted);
            accepted->active = false;
            return -1;
        }

        NotifyObjectChanged((Object*)socket);
        return acceptedHandle;
    }

    int SocketSendHandle(int handle, const uint8_t* data, uint32_t len) {
        if (data == nullptr) return -1;

        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightWrite) == 0 || socket->socketType != SocketTypeTcp) return -1;

        socket->socketLock.Acquire();
        Net::Tcp::Connection* conn = socket->tcpConn;
        socket->socketLock.Release();
        if (conn == nullptr) return -1;
        return Net::Tcp::Send(conn, data, (uint16_t)len);
    }

    int SocketRecvHandle(int handle, uint8_t* buffer, uint32_t maxLen) {
        if (buffer == nullptr) return -1;

        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightRead) == 0 || socket->socketType != SocketTypeTcp) return -1;

        socket->socketLock.Acquire();
        Net::Tcp::Connection* conn = socket->tcpConn;
        socket->socketLock.Release();
        if (conn == nullptr) return -1;

        int result = Net::Tcp::ReceiveNonBlocking(conn, buffer, (uint16_t)maxLen);
        if (result != 0) NotifyObjectChanged((Object*)socket);
        return result;
    }

    int SocketSendToHandle(int handle, const uint8_t* data, uint32_t len, uint32_t destIp, uint16_t destPort) {
        if (data == nullptr) return -1;

        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightWrite) == 0 || socket->socketType != SocketTypeUdp) return -1;

        socket->socketLock.Acquire();
        uint16_t localPort = socket->localPort;
        bool udpBound = socket->udpBound;
        socket->socketLock.Release();

        if (!udpBound || localPort == 0) {
            localPort = AllocEphemeralPort();
            if (!Net::Udp::Bind(localPort, UdpSocketDispatcher)) return -1;

            socket->socketLock.Acquire();
            socket->localPort = localPort;
            socket->udpBound = true;
            socket->socketLock.Release();
            NotifyObjectChanged((Object*)socket);
        }

        if (!Net::Udp::Send(destIp, localPort, destPort, data, (uint16_t)len)) {
            return -1;
        }
        return (int)len;
    }

    int SocketRecvFromHandle(int handle, uint8_t* buffer, uint32_t maxLen, uint32_t* srcIp, uint16_t* srcPort) {
        if (buffer == nullptr) return -1;

        Socket* socket = nullptr;
        uint32_t rights = 0;
        if (!SnapshotSocketHandle(handle, socket, rights)) return -1;
        if ((rights & RightRead) == 0 || socket->socketType != SocketTypeUdp) return -1;

        socket->socketLock.Acquire();
        if (socket->udpCount < sizeof(UdpDgramHeader)) {
            socket->socketLock.Release();
            return -1;
        }

        UdpDgramHeader hdr = {};
        uint8_t* hdrBytes = (uint8_t*)&hdr;
        for (uint32_t j = 0; j < sizeof(UdpDgramHeader); j++) {
            hdrBytes[j] = socket->udpRing[socket->udpHead];
            socket->udpHead = (socket->udpHead + 1) % UdpRingSize;
        }
        socket->udpCount -= sizeof(UdpDgramHeader);

        uint16_t copyLen = hdr.dataLen;
        if (copyLen > maxLen) copyLen = (uint16_t)maxLen;
        for (uint16_t j = 0; j < copyLen; j++) {
            buffer[j] = socket->udpRing[socket->udpHead];
            socket->udpHead = (socket->udpHead + 1) % UdpRingSize;
        }
        for (uint16_t j = copyLen; j < hdr.dataLen; j++) {
            socket->udpHead = (socket->udpHead + 1) % UdpRingSize;
        }
        socket->udpCount -= hdr.dataLen;
        socket->socketLock.Release();

        if (srcIp != nullptr) *srcIp = hdr.srcIp;
        if (srcPort != nullptr) *srcPort = hdr.srcPort;

        NotifyObjectChanged((Object*)socket);
        return (int)copyLen;
    }

    void NotifyTcpConnectionChanged(Net::Tcp::Connection* connection) {
        if (connection == nullptr) return;

        for (int i = 0; i < MaxSockets; i++) {
            if (!g_sockets[i].active || g_sockets[i].socketType != SocketTypeTcp) continue;

            g_sockets[i].socketLock.Acquire();
            bool matches = g_sockets[i].tcpConn == connection;
            g_sockets[i].socketLock.Release();
            if (matches) {
                NotifyObjectChanged((Object*)&g_sockets[i]);
            }
        }
    }

    Surface* CreateSurface(uint64_t byteSize) {
        if (byteSize == 0) byteSize = 0x1000;
        uint32_t numPages = (uint32_t)((byteSize + 0xFFFu) / 0x1000u);
        if (numPages == 0 || numPages > MaxSurfacePages) return nullptr;

        g_surfacePoolLock.Acquire();
        int slot = -1;
        for (int i = 0; i < MaxSurfaces; i++) {
            if (!g_surfaces[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            g_surfacePoolLock.Release();
            return nullptr;
        }

        InitObject(g_surfaces[slot], HandleType::Surface);
        g_surfaces[slot].numPages = numPages;
        g_surfaces[slot].sizeBytes = (uint64_t)numPages * 0x1000ULL;
        for (uint32_t i = 0; i < MaxSurfacePages; i++) g_surfaces[slot].physPages[i] = 0;
        g_surfacePoolLock.Release();

        for (uint32_t i = 0; i < numPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) {
                DestroySurface(&g_surfaces[slot]);
                g_surfaces[slot].active = false;
                return nullptr;
            }
            g_surfaces[slot].physPages[i] = Memory::SubHHDM((uint64_t)page);
        }
        return &g_surfaces[slot];
    }

    int CreateSurfaceHandle(uint64_t byteSize) {
        Surface* surface = CreateSurface(byteSize);
        if (surface == nullptr) return -1;

        int handle = InstallHandleForSlot(CurrentSlot(), (Object*)surface, HandleType::Surface,
                                          RightMap | RightManage | RightWait | RightDup);
        if (handle >= 0) return handle;

        DestroySurface(surface);
        surface->active = false;
        return -1;
    }

    void RetainSurface(Surface* surface) {
        RetainRawObject((Object*)surface);
    }

    void ReleaseSurface(Surface* surface) {
        ReleaseRawObject((Object*)surface);
    }

    uint64_t GetSurfaceSize(const Surface* surface) {
        if (surface == nullptr) return 0;
        return surface->sizeBytes;
    }

    int ResizeSurface(Surface* surface, uint64_t newSize) {
        if (surface == nullptr) return -1;
        if (newSize == 0) newSize = 0x1000;

        uint32_t newPages = (uint32_t)((newSize + 0xFFFu) / 0x1000u);
        if (newPages == 0 || newPages > MaxSurfacePages) return -1;

        surface->lock.Acquire();
        for (uint32_t i = 0; i < surface->numPages; i++) {
            if (surface->physPages[i] != 0) {
                Memory::g_pfa->Free((void*)Memory::HHDM(surface->physPages[i]));
                surface->physPages[i] = 0;
            }
        }
        surface->numPages = newPages;
        surface->sizeBytes = (uint64_t)newPages * 0x1000ULL;
        surface->lock.Release();

        for (uint32_t i = 0; i < newPages; i++) {
            void* page = Memory::g_pfa->AllocateZeroed();
            if (page == nullptr) return -1;
            surface->physPages[i] = Memory::SubHHDM((uint64_t)page);
        }
        NotifyObjectChanged((Object*)surface);
        return 0;
    }

    uint64_t MapSurfaceHandle(int handle) {
        auto* proc = Sched::GetCurrentProcessPtr();
        if (proc == nullptr) return 0;

        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return 0;
        if (type != HandleType::Surface || (rights & RightMap) == 0) return 0;

        uint64_t va = 0;
        if (MapSurfaceForPid((Surface*)object, proc->pid, proc->pml4Phys, proc->heapNext, va) < 0) {
            return 0;
        }
        return va;
    }

    int ResizeSurfaceHandle(int handle, uint64_t newSize) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(CurrentSlot(), handle, type, object, rights)) return -1;
        if (type != HandleType::Surface || (rights & RightManage) == 0) return -1;
        return ResizeSurface((Surface*)object, newSize);
    }

    int CopySurface(Surface* dst, Surface* src) {
        if (dst == nullptr || src == nullptr) return -1;
        if (dst->numPages != src->numPages) return -1;

        for (uint32_t i = 0; i < src->numPages; i++) {
            void* srcPage = (void*)Memory::HHDM(src->physPages[i]);
            void* dstPage = (void*)Memory::HHDM(dst->physPages[i]);
            memcpy(dstPage, srcPage, 0x1000);
        }
        NotifyObjectChanged((Object*)dst);
        return 0;
    }

    int CopySurfacePreserve(Surface* dst, int dstWidth, int dstHeight,
                            Surface* src, int srcWidth, int srcHeight,
                            uint32_t fillPixel) {
        if (dst == nullptr || src == nullptr) return -1;
        if (dstWidth <= 0 || dstHeight <= 0 || srcWidth <= 0 || srcHeight <= 0) return -1;

        uint64_t dstPixels = (uint64_t)dstWidth * (uint64_t)dstHeight;
        uint64_t srcPixels = (uint64_t)srcWidth * (uint64_t)srcHeight;
        if (dstPixels * sizeof(uint32_t) > dst->sizeBytes) return -1;
        if (srcPixels * sizeof(uint32_t) > src->sizeBytes) return -1;

        for (uint64_t i = 0; i < dstPixels; i++) {
            uint32_t* p = SurfacePixelPtr(dst, i);
            if (p != nullptr) *p = fillPixel;
        }

        int copyWidth = (dstWidth < srcWidth) ? dstWidth : srcWidth;
        int copyHeight = (dstHeight < srcHeight) ? dstHeight : srcHeight;
        for (int y = 0; y < copyHeight; y++) {
            uint64_t dstRow = (uint64_t)y * (uint64_t)dstWidth;
            uint64_t srcRow = (uint64_t)y * (uint64_t)srcWidth;
            for (int x = 0; x < copyWidth; x++) {
                uint32_t* srcPixel = SurfacePixelPtr(src, srcRow + (uint64_t)x);
                uint32_t* dstPixel = SurfacePixelPtr(dst, dstRow + (uint64_t)x);
                if (srcPixel != nullptr && dstPixel != nullptr) {
                    *dstPixel = *srcPixel;
                }
            }
        }

        NotifyObjectChanged((Object*)dst);
        return 0;
    }

    int MapSurfaceForPid(Surface* surface, int pid, uint64_t pml4Phys, uint64_t& heapNext, uint64_t& outVa) {
        if (surface == nullptr) return -1;

        int slot = SlotForPid(pid);
        if (slot < 0) return -1;

        for (int i = 0; i < MaxSurfaceMapsPerProcess; i++) {
            if (g_surfaceMaps[slot][i].used && g_surfaceMaps[slot][i].surface == surface) {
                outVa = g_surfaceMaps[slot][i].va;
                return 0;
            }
        }

        int mapIdx = -1;
        for (int i = 0; i < MaxSurfaceMapsPerProcess; i++) {
            if (!g_surfaceMaps[slot][i].used) {
                mapIdx = i;
                break;
            }
        }
        if (mapIdx < 0) return -1;

        uint64_t baseVa = heapNext;
        for (uint32_t i = 0; i < surface->numPages; i++) {
            if (!Memory::VMM::Paging::MapUserIn(pml4Phys, surface->physPages[i],
                                                baseVa + (uint64_t)i * 0x1000ULL)) {
                for (uint32_t j = 0; j < i; j++) {
                    Memory::VMM::Paging::UnmapUserIn(pml4Phys, baseVa + (uint64_t)j * 0x1000ULL);
                }
                return -1;
            }
        }

        g_surfaceMaps[slot][mapIdx].used = true;
        g_surfaceMaps[slot][mapIdx].surface = surface;
        g_surfaceMaps[slot][mapIdx].va = baseVa;
        g_surfaceMaps[slot][mapIdx].numPages = surface->numPages;
        RetainRawObject((Object*)surface);

        heapNext += (uint64_t)surface->numPages * 0x1000ULL;
        outVa = baseVa;
        return 0;
    }

    int UnmapSurfaceForPid(Surface* surface, int pid, uint64_t pml4Phys) {
        if (surface == nullptr) return -1;

        int slot = SlotForPid(pid);
        if (slot < 0) return -1;

        int unmapped = 0;
        for (int i = 0; i < MaxSurfaceMapsPerProcess; i++) {
            if (!g_surfaceMaps[slot][i].used || g_surfaceMaps[slot][i].surface != surface) continue;
            for (uint32_t p = 0; p < g_surfaceMaps[slot][i].numPages; p++) {
                Memory::VMM::Paging::UnmapUserIn(pml4Phys, g_surfaceMaps[slot][i].va + (uint64_t)p * 0x1000ULL);
            }
            g_surfaceMaps[slot][i].used = false;
            g_surfaceMaps[slot][i].surface = nullptr;
            g_surfaceMaps[slot][i].va = 0;
            g_surfaceMaps[slot][i].numPages = 0;
            ReleaseRawObject((Object*)surface);
            unmapped++;
        }

        return unmapped > 0 ? 0 : -1;
    }

    ProcessObject* GetProcessObject(int pid) {
        g_processPoolLock.Acquire();
        for (int i = 0; i < MaxProcessObjects; i++) {
            if (!g_processObjects[i].active) continue;
            if (g_processObjects[i].pid == pid) {
                g_processPoolLock.Release();
                return &g_processObjects[i];
            }
        }
        g_processPoolLock.Release();
        return nullptr;
    }

    int OpenProcessHandle(int pid) {
        ProcessObject* process = GetProcessObject(pid);
        if (process == nullptr) return -1;
        return InstallHandleForSlot(CurrentSlot(), (Object*)process, HandleType::Process,
                                    RightWait | RightDup);
    }

    void ProcessStartedInSlot(int slot, int pid) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return;

        g_processPoolLock.Acquire();
        for (int i = 0; i < MaxProcessObjects; i++) {
            if (g_processObjects[i].active) continue;
            InitObject(g_processObjects[i], HandleType::Process);
            g_processObjects[i].pid = pid;
            g_processObjects[i].exited = false;
            g_processObjectsBySlot[slot] = &g_processObjects[i];
            g_processObjects[i].refs = 1; // liveness reference
            g_processPoolLock.Release();
            return;
        }
        g_processPoolLock.Release();
        Kt::KernelLogStream(Kt::ERROR, "IPC") << "Out of process objects for PID " << (uint64_t)pid;
    }

    void ProcessExitedInSlot(int slot, int /*pid*/) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return;

        ProcessObject* object = g_processObjectsBySlot[slot];
        if (object == nullptr) return;

        object->lock.Acquire();
        object->exited = true;
        object->lock.Release();

        NotifyObjectChanged((Object*)object);
        ReleaseRawObject((Object*)object);
        g_processObjectsBySlot[slot] = nullptr;
    }

    bool ProcessHasExited(ProcessObject* process) {
        if (process == nullptr) return true;
        process->lock.Acquire();
        bool exited = process->exited;
        process->lock.Release();
        return exited;
    }

    static uint32_t CurrentSocketSignals(Socket* socket, uint32_t rights) {
        if (socket == nullptr) return SignalNone;

        if (socket->socketType == SocketTypeUdp) {
            uint32_t signals = SignalReady;
            socket->socketLock.Acquire();
            if ((rights & RightRead) && socket->udpCount >= sizeof(UdpDgramHeader)) {
                signals |= SignalReadable;
            }
            if (rights & RightWrite) {
                signals |= SignalWritable;
            }
            socket->socketLock.Release();
            return signals;
        }

        if (socket->socketType != SocketTypeTcp) return SignalNone;

        socket->socketLock.Acquire();
        Net::Tcp::Connection* conn = socket->tcpConn;
        socket->socketLock.Release();
        if (conn == nullptr) return SignalNone;

        uint32_t signals = SignalNone;
        Net::Tcp::State state = Net::Tcp::GetState(conn);
        if (state == Net::Tcp::State::Listen) {
            if ((rights & RightRead) && Net::Tcp::HasPendingAccept(conn)) {
                signals |= SignalReadable | SignalReady;
            }
            return signals;
        }

        if ((rights & RightRead) && Net::Tcp::HasReceiveData(conn)) {
            signals |= SignalReadable;
        }
        if ((rights & RightWrite) && Net::Tcp::CanSend(conn)) {
            signals |= SignalWritable;
        }
        if ((rights & (RightRead | RightWrite)) && Net::Tcp::IsClosedForIo(conn)) {
            signals |= SignalPeerClosed;
        }
        if (signals != SignalNone) signals |= SignalReady;
        return signals;
    }

    static uint32_t CurrentSignalsForSnapshot(HandleType type, Object* object, uint32_t rights) {
        if (object == nullptr) return SignalNone;

        switch (type) {
            case HandleType::Stream: {
                auto* stream = (Stream*)object;
                uint32_t signals = SignalNone;
                stream->lock.Acquire();
                if ((rights & RightRead) && stream->count > 0) signals |= SignalReadable;
                if ((rights & RightWrite) && stream->count < stream->capacity && stream->readerRefs > 0) signals |= SignalWritable;
                if ((rights & RightRead) && stream->writerRefs == 0) signals |= SignalPeerClosed;
                if ((rights & RightWrite) && stream->readerRefs == 0) signals |= SignalPeerClosed;
                stream->lock.Release();
                return signals;
            }
            case HandleType::Mailbox: {
                auto* mailbox = (Mailbox*)object;
                uint32_t signals = SignalNone;
                mailbox->lock.Acquire();
                if ((rights & RightRecv) && mailbox->count > 0) signals |= SignalReadable;
                if ((rights & RightSend) && mailbox->count < MaxMailboxMessages && mailbox->receiverRefs > 0) signals |= SignalWritable;
                if ((rights & RightRecv) && mailbox->senderRefs == 0) signals |= SignalPeerClosed;
                if ((rights & RightSend) && mailbox->receiverRefs == 0) signals |= SignalPeerClosed;
                mailbox->lock.Release();
                return signals;
            }
            case HandleType::File: {
                uint32_t signals = SignalReady;
                if (rights & RightRead) signals |= SignalReadable;
                if (rights & RightWrite) signals |= SignalWritable;
                return signals;
            }
            case HandleType::Socket:
                return CurrentSocketSignals((Socket*)object, rights);
            case HandleType::Surface:
                return (rights & RightMap) ? SignalReady : SignalNone;
            case HandleType::Process: {
                auto* process = (ProcessObject*)object;
                return ProcessHasExited(process) ? SignalExited : SignalNone;
            }
            case HandleType::Waitset: {
                WaitsetReady ready{};
                return WaitsetCheckReady((Waitset*)object, &ready) ? SignalReady : SignalNone;
            }
            default:
                return SignalNone;
        }
    }

    uint32_t GetHandleSignalsForSlot(int slot, int handle) {
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(slot, handle, type, object, rights)) return SignalNone;
        return CurrentSignalsForSnapshot(type, object, rights);
    }

    static bool WaitsetCheckReady(Waitset* waitset, WaitsetReady* outReady) {
        if (waitset == nullptr) return false;
        for (int i = 0; i < MaxWaitsetEntries; i++) {
            if (!waitset->entries[i].used || waitset->entries[i].object == nullptr) continue;
            uint32_t current = CurrentSignalsForSnapshot(waitset->entries[i].type,
                                                         waitset->entries[i].object,
                                                         waitset->entries[i].rights);
            uint32_t readySignals = current & waitset->entries[i].signals;
            if (readySignals == 0) continue;
            if (outReady != nullptr) {
                outReady->index = i;
                outReady->signals = readySignals;
            }
            return true;
        }
        return false;
    }

    void NotifyObjectChanged(Object* object) {
        if (object == nullptr) return;

        Sched::WakeObjectWaiters(object);

        for (int i = 0; i < MaxWaitsets; i++) {
            if (!g_waitsets[i].active) continue;
            WaitsetReady ready{};
            if (WaitsetCheckReady(&g_waitsets[i], &ready)) {
                Sched::WakeObjectWaiters(&g_waitsets[i]);
            }
        }
    }

    uint32_t WaitOnHandle(int handle, uint32_t wantedSignals, uint64_t timeoutMs) {
        int slot = CurrentSlot();
        HandleType type = HandleType::None;
        Object* object = nullptr;
        uint32_t rights = 0;
        if (!SnapshotHandleForSlot(slot, handle, type, object, rights)) return (uint32_t)-1;
        if ((rights & RightWait) == 0) return (uint32_t)-1;

        uint64_t start = Timekeeping::GetMilliseconds();
        for (;;) {
            uint32_t current = CurrentSignalsForSnapshot(type, object, rights);
            uint32_t ready = current & wantedSignals;
            if (ready != 0) return ready;

            if (timeoutMs == 0) return 0;
            if (timeoutMs != ~0ULL) {
                uint64_t elapsed = Timekeeping::GetMilliseconds() - start;
                if (elapsed >= timeoutMs) return 0;
                Sched::BlockOnObject(object, timeoutMs - elapsed);
            } else {
                Sched::BlockOnObject(object, 0);
            }
        }
    }

    int CreateWaitsetHandleForSlot(int slot) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return -1;

        g_waitsetPoolLock.Acquire();
        for (int i = 0; i < MaxWaitsets; i++) {
            if (g_waitsets[i].active) continue;
            InitObject(g_waitsets[i], HandleType::Waitset);
            for (int j = 0; j < MaxWaitsetEntries; j++) {
                g_waitsets[i].entries[j].used = false;
                g_waitsets[i].entries[j].type = HandleType::None;
                g_waitsets[i].entries[j].object = nullptr;
                g_waitsets[i].entries[j].rights = 0;
                g_waitsets[i].entries[j].signals = 0;
            }
            g_waitsetPoolLock.Release();
            return InstallHandleForSlot(slot, (Object*)&g_waitsets[i], HandleType::Waitset,
                                        RightWait | RightManage | RightDup);
        }
        g_waitsetPoolLock.Release();
        return -1;
    }

    int CreateWaitsetHandleForCurrent() {
        return CreateWaitsetHandleForSlot(CurrentSlot());
    }

    int WaitsetAddHandleForSlot(int slot, int waitsetHandle, int targetHandle, uint32_t signals) {
        HandleType waitsetType = HandleType::None;
        Object* waitsetObject = nullptr;
        uint32_t waitsetRights = 0;
        if (!SnapshotHandleForSlot(slot, waitsetHandle, waitsetType, waitsetObject, waitsetRights)) return -1;
        if (waitsetType != HandleType::Waitset || (waitsetRights & RightManage) == 0) return -1;

        HandleType targetType = HandleType::None;
        Object* targetObject = nullptr;
        uint32_t targetRights = 0;
        if (!SnapshotHandleForSlot(slot, targetHandle, targetType, targetObject, targetRights)) return -1;
        if ((targetRights & RightWait) == 0) return -1;
        if (targetType == HandleType::Waitset) return -1;

        auto* waitset = (Waitset*)waitsetObject;
        waitset->lock.Acquire();
        for (int i = 0; i < MaxWaitsetEntries; i++) {
            if (waitset->entries[i].used) continue;
            waitset->entries[i].used = true;
            waitset->entries[i].type = targetType;
            waitset->entries[i].object = targetObject;
            waitset->entries[i].rights = targetRights;
            waitset->entries[i].signals = signals;
            RetainRawObject(targetObject);
            waitset->lock.Release();
            NotifyObjectChanged(waitsetObject);
            return i;
        }
        waitset->lock.Release();
        return -1;
    }

    int WaitsetAddHandle(int waitsetHandle, int targetHandle, uint32_t signals) {
        return WaitsetAddHandleForSlot(CurrentSlot(), waitsetHandle, targetHandle, signals);
    }

    int WaitsetRemoveIndexForSlot(int slot, int waitsetHandle, int index) {
        HandleType waitsetType = HandleType::None;
        Object* waitsetObject = nullptr;
        uint32_t waitsetRights = 0;
        if (!SnapshotHandleForSlot(slot, waitsetHandle, waitsetType, waitsetObject, waitsetRights)) return -1;
        if (waitsetType != HandleType::Waitset || (waitsetRights & RightManage) == 0) return -1;
        if (index < 0 || index >= MaxWaitsetEntries) return -1;

        auto* waitset = (Waitset*)waitsetObject;
        waitset->lock.Acquire();
        if (!waitset->entries[index].used) {
            waitset->lock.Release();
            return -1;
        }

        Object* targetObject = waitset->entries[index].object;
        waitset->entries[index].used = false;
        waitset->entries[index].type = HandleType::None;
        waitset->entries[index].object = nullptr;
        waitset->entries[index].rights = 0;
        waitset->entries[index].signals = 0;
        waitset->lock.Release();

        ReleaseRawObject(targetObject);
        return 0;
    }

    int WaitsetRemoveIndex(int waitsetHandle, int index) {
        return WaitsetRemoveIndexForSlot(CurrentSlot(), waitsetHandle, index);
    }

    int WaitsetWaitHandle(int waitsetHandle, WaitsetReady* outReady, uint64_t timeoutMs) {
        int slot = CurrentSlot();

        HandleType waitsetType = HandleType::None;
        Object* waitsetObject = nullptr;
        uint32_t waitsetRights = 0;
        if (!SnapshotHandleForSlot(slot, waitsetHandle, waitsetType, waitsetObject, waitsetRights)) return -1;
        if (waitsetType != HandleType::Waitset || (waitsetRights & RightWait) == 0) return -1;

        uint64_t start = Timekeeping::GetMilliseconds();
        for (;;) {
            WaitsetReady ready = {-1, 0};
            if (WaitsetCheckReady((Waitset*)waitsetObject, &ready)) {
                if (outReady != nullptr) *outReady = ready;
                return 1;
            }

            if (timeoutMs == 0) return 0;
            if (timeoutMs != ~0ULL) {
                uint64_t elapsed = Timekeeping::GetMilliseconds() - start;
                if (elapsed >= timeoutMs) return 0;
                Sched::BlockOnObject(waitsetObject, timeoutMs - elapsed);
            } else {
                Sched::BlockOnObject(waitsetObject, 0);
            }
        }
    }

    void CleanupProcessSlot(int slot, int /*pid*/, uint64_t pml4Phys) {
        if (slot < 0 || slot >= Sched::MaxProcesses) return;

        for (int h = 0; h < MaxHandlesPerProcess; h++) {
            if (g_handleTables[slot][h].used) {
                CloseHandleForSlot(slot, h);
            }
        }

        for (int i = 0; i < MaxSurfaceMapsPerProcess; i++) {
            if (!g_surfaceMaps[slot][i].used || g_surfaceMaps[slot][i].surface == nullptr) continue;
            for (uint32_t p = 0; p < g_surfaceMaps[slot][i].numPages; p++) {
                Memory::VMM::Paging::UnmapUserIn(pml4Phys, g_surfaceMaps[slot][i].va + (uint64_t)p * 0x1000ULL);
            }
            Object* surfaceObject = (Object*)g_surfaceMaps[slot][i].surface;
            g_surfaceMaps[slot][i].used = false;
            g_surfaceMaps[slot][i].surface = nullptr;
            g_surfaceMaps[slot][i].va = 0;
            g_surfaceMaps[slot][i].numPages = 0;
            ReleaseRawObject(surfaceObject);
        }
    }

    void Initialize() {
        for (int i = 0; i < Sched::MaxProcesses; i++) {
            g_processObjectsBySlot[i] = nullptr;
        }
        Kt::KernelLogStream(Kt::OK, "IPC") << "Initialized ("
            << (uint64_t)MaxHandlesPerProcess << " handles/process, "
            << (uint64_t)MaxStreams << " streams, "
            << (uint64_t)MaxMailboxes << " mailboxes, "
            << (uint64_t)MaxFiles << " files, "
            << (uint64_t)MaxSockets << " sockets, "
            << (uint64_t)MaxSurfaces << " surfaces)";
    }

}
