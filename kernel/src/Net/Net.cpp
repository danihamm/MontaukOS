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
#include <Drivers/Net/E1000E.hpp>
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

namespace Net {

    void Initialize() {
        if (!Drivers::Net::E1000::IsInitialized() && !Drivers::Net::E1000E::IsInitialized()) {
            KernelLogStream(WARNING, "Net") << "No NIC initialized, skipping network stack";
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

        // Hook the active NIC's RX to our Ethernet dispatcher
        if (Drivers::Net::E1000::IsInitialized()) {
            Drivers::Net::E1000::SetRxCallback(Ethernet::OnFrameReceived);
        } else {
            Drivers::Net::E1000E::SetRxCallback(Ethernet::OnFrameReceived);
        }

        // Send a gratuitous ARP to announce ourselves on the network
        Arp::SendRequest(GetIpAddress());

        KernelLogStream(OK, "Net") << "Network stack initialized";
    }

}
