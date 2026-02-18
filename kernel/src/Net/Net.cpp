/*
    * Net.cpp
    * Network stack initialization
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Net.hpp"
#include <Net/Ethernet.hpp>
#include <Net/Arp.hpp>
#include <Net/Ipv4.hpp>
#include <Net/Icmp.hpp>
#include <Net/Udp.hpp>
#include <Net/Tcp.hpp>
#include <Net/Socket.hpp>
#include <Net/NetConfig.hpp>
#include <Drivers/Net/E1000.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net {

    void Initialize() {
        if (!Drivers::Net::E1000::IsInitialized()) {
            KernelLogStream(WARNING, "Net") << "E1000 not initialized, skipping network stack";
            return;
        }

        // Initialize layers bottom-up
        Ethernet::Initialize();
        Arp::Initialize();
        Ipv4::Initialize();
        Icmp::Initialize();
        Udp::Initialize();
        Tcp::Initialize();
        Socket::Initialize();

        // Hook E1000 RX to our Ethernet dispatcher
        Drivers::Net::E1000::SetRxCallback(Ethernet::OnFrameReceived);

        // Send a gratuitous ARP to announce ourselves on the network
        Arp::SendRequest(GetIpAddress());

        KernelLogStream(OK, "Net") << "Network stack initialized";
    }

}
