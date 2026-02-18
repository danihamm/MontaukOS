/*
    * Time.hpp
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <CppLib/CString.hpp>

namespace Timekeeping {
    /*
        Structure representing the attributes of a time zone; properties and time zone configuration will eventually
        be loaded from disk/ramdisk.
    */
    struct TimeZone {
        /* Time zone naming scheme: 
            i.e. "Central European Time" / "CET"
        */
        CString TZLongName;
        CString TZShortName;
        /* Hour offset from UTC */
        int8_t HourOffset;
        /* Minute offset from UTC */
        int8_t MinuteOffset;
        /* Is daylight saving time */
        bool IsDST;
    };
    
    const CString Months[] = {
        nullptr,
        "January",
        "February",
        "March",
        "April",
        "May",
        "June",
        "July",
        "August",
        "September",
        "October",
        "November",
        "December"
    };

    struct DateTime {
        uint16_t Year;
        uint8_t Month;
        uint8_t Day;
        uint8_t Hour;
        uint8_t Minute;
        uint8_t Second;
    };

    void Init(uint16_t Year, uint8_t Month, uint8_t Day, uint8_t Hour, uint8_t Minute, uint8_t Second);
    int64_t GetUnixTimestamp();
    DateTime GetDateTime();
};
