/*
    * L2cap.cpp
    * Bluetooth L2CAP implementation
    * Copyright (c) 2026 Daniel Hammer
*/

#include "L2cap.hpp"
#include "Hci.hpp"
#include "A2dp.hpp"
#include <Drivers/USB/Xhci.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>
#include <Libraries/Memory.hpp>
#include <Timekeeping/ApicTimer.hpp>

using namespace Kt;

namespace Drivers::USB::Bluetooth::L2cap {

    // =========================================================================
    // State
    // =========================================================================

    static uint16_t g_aclHandle = 0;
    static bool g_initialized = false;
    static uint8_t g_sigIdentifier = 1;

    // Channel table
    static ChannelInfo g_channels[MAX_CHANNELS] = {};
    static uint16_t g_nextCid = CID_DYNAMIC_START;

    // Signaling response tracking
    static volatile bool g_sigResponseReady = false;
    static uint8_t g_sigResponseBuf[64] = {};
    static uint32_t g_sigResponseLen = 0;

    // =========================================================================
    // Helpers
    // =========================================================================

    static uint16_t AllocCid() {
        return g_nextCid++;
    }

    static ChannelInfo* AllocChannel(uint16_t psm) {
        for (int i = 0; i < MAX_CHANNELS; i++) {
            if (!g_channels[i].Active) {
                g_channels[i].Active = true;
                g_channels[i].LocalCid = AllocCid();
                g_channels[i].RemoteCid = 0;
                g_channels[i].Psm = psm;
                g_channels[i].RemoteMtu = 672;  // Default L2CAP MTU
                g_channels[i].Configured = false;
                g_channels[i].LocalConfigDone = false;
                g_channels[i].RemoteConfigDone = false;
                return &g_channels[i];
            }
        }
        return nullptr;
    }

    // Send L2CAP signaling command
    static void SendSignal(uint8_t code, uint8_t identifier,
                           const uint8_t* payload, uint16_t payloadLen) {
        // L2CAP header + Signal header + payload
        uint16_t sigLen = sizeof(SignalHeader) + payloadLen;
        uint16_t totalPayload = sizeof(L2capHeader) + sigLen;

        uint8_t buf[128] = {};
        auto* l2hdr = (L2capHeader*)buf;
        l2hdr->Length = sigLen;
        l2hdr->ChannelId = CID_SIGNALING;

        auto* sig = (SignalHeader*)(buf + sizeof(L2capHeader));
        sig->Code = code;
        sig->Identifier = identifier;
        sig->Length = payloadLen;

        if (payload && payloadLen > 0) {
            memcpy(buf + sizeof(L2capHeader) + sizeof(SignalHeader), payload, payloadLen);
        }

        Hci::SendAcl(g_aclHandle, Hci::ACL_PB_FIRST_FLUSH,
                      buf, totalPayload);
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    void Initialize(uint16_t aclHandle) {
        g_aclHandle = aclHandle;
        g_initialized = true;
        g_sigIdentifier = 1;
        g_nextCid = CID_DYNAMIC_START;

        for (int i = 0; i < MAX_CHANNELS; i++) {
            g_channels[i].Active = false;
        }

        KernelLogStream(OK, "BT-L2CAP") << "Initialized for ACL handle " << (uint64_t)aclHandle;
    }

    // =========================================================================
    // ProcessPacket
    // =========================================================================

    void ProcessPacket(uint16_t aclHandle, const uint8_t* data, uint16_t len) {
        if (len < sizeof(L2capHeader)) return;

        auto* l2hdr = (const L2capHeader*)data;
        uint16_t l2len = l2hdr->Length;
        uint16_t cid = l2hdr->ChannelId;
        const uint8_t* payload = data + sizeof(L2capHeader);

        if (l2len + sizeof(L2capHeader) > len) return;

        if (cid == CID_SIGNALING) {
            // L2CAP signaling channel
            if (l2len < sizeof(SignalHeader)) return;

            auto* sig = (const SignalHeader*)payload;
            const uint8_t* sigPayload = payload + sizeof(SignalHeader);
            uint16_t sigPayloadLen = sig->Length;

            switch (sig->Code) {
                case SIG_CONN_REQ: {
                    if (sigPayloadLen >= 4) {
                        uint16_t psm = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);
                        uint16_t srcCid = (uint16_t)sigPayload[2] | ((uint16_t)sigPayload[3] << 8);

                        KernelLogStream(INFO, "BT-L2CAP") << "Connection Request: PSM="
                            << base::hex << (uint64_t)psm << " srcCID=" << (uint64_t)srcCid;

                        // Accept connections for AVDTP
                        if (psm == PSM_AVDTP || psm == PSM_SDP) {
                            auto* ch = AllocChannel(psm);
                            if (ch) {
                                ch->RemoteCid = srcCid;

                                // Send Connection Response (success)
                                uint8_t rsp[8] = {};
                                rsp[0] = (uint8_t)(ch->LocalCid & 0xFF);
                                rsp[1] = (uint8_t)(ch->LocalCid >> 8);
                                rsp[2] = (uint8_t)(srcCid & 0xFF);
                                rsp[3] = (uint8_t)(srcCid >> 8);
                                rsp[4] = 0; rsp[5] = 0;  // Result: success
                                rsp[6] = 0; rsp[7] = 0;  // Status: no info
                                SendSignal(SIG_CONN_RSP, sig->Identifier, rsp, 8);
                            }
                        } else {
                            // Reject: PSM not supported
                            uint8_t rsp[8] = {};
                            rsp[0] = 0; rsp[1] = 0;  // Dest CID = 0
                            rsp[2] = (uint8_t)(srcCid & 0xFF);
                            rsp[3] = (uint8_t)(srcCid >> 8);
                            rsp[4] = 0x02; rsp[5] = 0;  // Result: PSM not supported
                            rsp[6] = 0; rsp[7] = 0;
                            SendSignal(SIG_CONN_RSP, sig->Identifier, rsp, 8);
                        }
                    }
                    break;
                }

                case SIG_CONN_RSP: {
                    if (sigPayloadLen >= 8) {
                        uint16_t dstCid = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);
                        uint16_t srcCid = (uint16_t)sigPayload[2] | ((uint16_t)sigPayload[3] << 8);
                        uint16_t result = (uint16_t)sigPayload[4] | ((uint16_t)sigPayload[5] << 8);

                        KernelLogStream(INFO, "BT-L2CAP") << "Connection Response: dstCID="
                            << base::hex << (uint64_t)dstCid << " result=" << (uint64_t)result;

                        if (result == CONN_SUCCESS) {
                            // Find our channel by srcCid (which is our local CID)
                            for (int i = 0; i < MAX_CHANNELS; i++) {
                                if (g_channels[i].Active && g_channels[i].LocalCid == srcCid) {
                                    g_channels[i].RemoteCid = dstCid;

                                    // Send Configuration Request
                                    uint8_t cfgReq[4] = {};
                                    cfgReq[0] = (uint8_t)(dstCid & 0xFF);
                                    cfgReq[1] = (uint8_t)(dstCid >> 8);
                                    cfgReq[2] = 0; cfgReq[3] = 0;  // Flags
                                    SendSignal(SIG_CONFIG_REQ, g_sigIdentifier++, cfgReq, 4);
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }

                case SIG_CONFIG_REQ: {
                    if (sigPayloadLen >= 4) {
                        uint16_t dstCid = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);

                        // Find channel
                        for (int i = 0; i < MAX_CHANNELS; i++) {
                            if (g_channels[i].Active && g_channels[i].LocalCid == dstCid) {
                                g_channels[i].RemoteConfigDone = true;

                                // Parse MTU option if present
                                uint16_t cfgOffset = 4;  // Skip dstCid + flags
                                while (cfgOffset + 2 <= sigPayloadLen) {
                                    uint8_t optType = sigPayload[cfgOffset];
                                    uint8_t optLen = sigPayload[cfgOffset + 1];
                                    if (optType == 0x01 && optLen == 2 && cfgOffset + 4 <= sigPayloadLen) {
                                        g_channels[i].RemoteMtu = (uint16_t)sigPayload[cfgOffset + 2]
                                                                | ((uint16_t)sigPayload[cfgOffset + 3] << 8);
                                    }
                                    cfgOffset += 2 + optLen;
                                }

                                // Send Config Response (success)
                                uint8_t rsp[6] = {};
                                rsp[0] = (uint8_t)(g_channels[i].RemoteCid & 0xFF);
                                rsp[1] = (uint8_t)(g_channels[i].RemoteCid >> 8);
                                rsp[2] = 0; rsp[3] = 0;  // Flags
                                rsp[4] = 0; rsp[5] = 0;  // Result: success
                                SendSignal(SIG_CONFIG_RSP, sig->Identifier, rsp, 6);

                                if (g_channels[i].LocalConfigDone && g_channels[i].RemoteConfigDone) {
                                    g_channels[i].Configured = true;
                                    KernelLogStream(OK, "BT-L2CAP") << "Channel "
                                        << (uint64_t)g_channels[i].LocalCid << " configured";

                                    // Notify A2DP if this is an AVDTP channel
                                    if (g_channels[i].Psm == PSM_AVDTP) {
                                        A2dp::OnChannelReady(g_channels[i].LocalCid);
                                    }
                                }
                                break;
                            }
                        }
                    }
                    break;
                }

                case SIG_CONFIG_RSP: {
                    if (sigPayloadLen >= 6) {
                        uint16_t srcCid = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);
                        uint16_t result = (uint16_t)sigPayload[4] | ((uint16_t)sigPayload[5] << 8);

                        if (result == CFG_SUCCESS) {
                            for (int i = 0; i < MAX_CHANNELS; i++) {
                                if (g_channels[i].Active && g_channels[i].RemoteCid == srcCid) {
                                    g_channels[i].LocalConfigDone = true;
                                    if (g_channels[i].LocalConfigDone && g_channels[i].RemoteConfigDone) {
                                        g_channels[i].Configured = true;
                                        KernelLogStream(OK, "BT-L2CAP") << "Channel "
                                            << (uint64_t)g_channels[i].LocalCid << " configured";

                                        if (g_channels[i].Psm == PSM_AVDTP) {
                                            A2dp::OnChannelReady(g_channels[i].LocalCid);
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }

                case SIG_DISCONN_REQ: {
                    if (sigPayloadLen >= 4) {
                        uint16_t dstCid = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);
                        uint16_t srcCid = (uint16_t)sigPayload[2] | ((uint16_t)sigPayload[3] << 8);

                        for (int i = 0; i < MAX_CHANNELS; i++) {
                            if (g_channels[i].Active && g_channels[i].LocalCid == dstCid) {
                                g_channels[i].Active = false;
                                break;
                            }
                        }

                        // Send Disconnect Response
                        uint8_t rsp[4] = {};
                        rsp[0] = (uint8_t)(dstCid & 0xFF);
                        rsp[1] = (uint8_t)(dstCid >> 8);
                        rsp[2] = (uint8_t)(srcCid & 0xFF);
                        rsp[3] = (uint8_t)(srcCid >> 8);
                        SendSignal(SIG_DISCONN_RSP, sig->Identifier, rsp, 4);
                    }
                    break;
                }

                case SIG_INFO_REQ: {
                    if (sigPayloadLen >= 2) {
                        uint16_t infoType = (uint16_t)sigPayload[0] | ((uint16_t)sigPayload[1] << 8);

                        if (infoType == 0x0002) {
                            // Extended features mask
                            uint8_t rsp[8] = {};
                            rsp[0] = 0x02; rsp[1] = 0x00;  // InfoType
                            rsp[2] = 0x00; rsp[3] = 0x00;  // Result: success
                            rsp[4] = 0x00; rsp[5] = 0x00;  // Features: none
                            rsp[6] = 0x00; rsp[7] = 0x00;
                            SendSignal(SIG_INFO_RSP, sig->Identifier, rsp, 8);
                        } else {
                            // Not supported
                            uint8_t rsp[4] = {};
                            rsp[0] = (uint8_t)(infoType & 0xFF);
                            rsp[1] = (uint8_t)(infoType >> 8);
                            rsp[2] = 0x01; rsp[3] = 0x00;  // Result: not supported
                            SendSignal(SIG_INFO_RSP, sig->Identifier, rsp, 4);
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        } else {
            // Data on a dynamic channel
            for (int i = 0; i < MAX_CHANNELS; i++) {
                if (g_channels[i].Active && g_channels[i].LocalCid == cid) {
                    if (g_channels[i].Psm == PSM_AVDTP) {
                        A2dp::ProcessAvdtp(payload, l2len);
                    }
                    break;
                }
            }
        }
    }

    // =========================================================================
    // Connect
    // =========================================================================

    uint16_t Connect(uint16_t psm) {
        if (!g_initialized) return 0;

        auto* ch = AllocChannel(psm);
        if (!ch) return 0;

        // Send Connection Request
        uint8_t req[4] = {};
        req[0] = (uint8_t)(psm & 0xFF);
        req[1] = (uint8_t)(psm >> 8);
        req[2] = (uint8_t)(ch->LocalCid & 0xFF);
        req[3] = (uint8_t)(ch->LocalCid >> 8);
        SendSignal(SIG_CONN_REQ, g_sigIdentifier++, req, 4);

        return ch->LocalCid;
    }

    // =========================================================================
    // WaitConfigured
    // =========================================================================

    bool WaitConfigured(uint16_t localCid, uint32_t timeoutMs) {
        uint64_t start = Timekeeping::GetMilliseconds();

        while (Timekeeping::GetMilliseconds() - start < timeoutMs) {
            Xhci::PollEvents();

            auto* ch = GetChannel(localCid);
            if (ch && ch->Configured) return true;

            for (int j = 0; j < 100; j++) {
                asm volatile("" ::: "memory");
            }
        }
        return false;
    }

    // =========================================================================
    // SendData
    // =========================================================================

    bool SendData(uint16_t localCid, const uint8_t* data, uint16_t len) {
        if (!g_initialized) return false;

        auto* ch = GetChannel(localCid);
        if (!ch || !ch->Configured) return false;

        // Build L2CAP packet
        uint16_t totalLen = sizeof(L2capHeader) + len;
        uint8_t buf[1024] = {};
        if (totalLen > sizeof(buf)) return false;

        auto* l2hdr = (L2capHeader*)buf;
        l2hdr->Length = len;
        l2hdr->ChannelId = ch->RemoteCid;

        if (data && len > 0) {
            memcpy(buf + sizeof(L2capHeader), data, len);
        }

        return Hci::SendAcl(g_aclHandle, Hci::ACL_PB_FIRST_FLUSH, buf, totalLen);
    }

    // =========================================================================
    // Channel queries
    // =========================================================================

    ChannelInfo* GetChannel(uint16_t localCid) {
        for (int i = 0; i < MAX_CHANNELS; i++) {
            if (g_channels[i].Active && g_channels[i].LocalCid == localCid) {
                return &g_channels[i];
            }
        }
        return nullptr;
    }

    ChannelInfo* FindChannelByPsm(uint16_t psm) {
        for (int i = 0; i < MAX_CHANNELS; i++) {
            if (g_channels[i].Active && g_channels[i].Psm == psm) {
                return &g_channels[i];
            }
        }
        return nullptr;
    }

    uint16_t GetAclHandle() {
        return g_aclHandle;
    }

}
