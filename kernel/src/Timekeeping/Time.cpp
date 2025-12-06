/*
    * Time.cpp
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Time.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

void Timekeeping::Init(uint16_t Year, uint8_t Month, uint8_t Day, uint8_t Hour, uint8_t Minute, uint8_t Second) {
    /* Hardcode CET for now */
    TimeZone CET = {
        "Central European Time",
        "CET",
        1, /* UTC+1 */
        0,
        false
    };

    Kt::KernelLogStream(INFO, "Timekeeping Service") << "Setting time zone to " << CET.TZLongName << " (" << CET.TZShortName << ")";

    Minute = Minute + CET.MinuteOffset;
    Hour = Hour + CET.HourOffset;
    if (Minute >= 60) {
        Minute -= 60;
        Hour += 1;
    }
    if (Hour >= 24) {
        Hour -= 24;
        Day += 1;
        /* Note: No month/day overflow handling yet */
    }

    kcp::cstringstream minuteStream;
    if (Minute < 10) {
        minuteStream << "0";
    }
    minuteStream << Minute;
    CString minuteStr = minuteStream.c_str();

    kcp::cstringstream secondStream;
    if (Second < 10) {
        secondStream << "0";
    }
    secondStream << Second;
    CString secondStr = secondStream.c_str();

    kcp::cstringstream panelStr;
    panelStr
        << " "
        << Day << " "
        << Months[Month] << " "
        << Year << ", "
        << Hour << ":"

        << minuteStr << ":"
        << secondStr
        << " (" << CET.TZLongName << ")";


    CString dateString = panelStr.c_str();

    UpdatePanelBar(dateString);
}