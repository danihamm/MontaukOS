/*
    * Time.cpp
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Time.hpp"
#include "ApicTimer.hpp"
#include <Terminal/Terminal.hpp>
#include <CppLib/Stream.hpp>

using namespace Kt;

static int64_t g_bootEpoch = 0;

static bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int DaysInMonth(int month, int year) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) return 29;
    return days[month];
}

static int64_t DateToEpoch(int year, int month, int day, int hour, int minute, int second) {
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += IsLeapYear(y) ? 366 : 365;
    }
    for (int m = 1; m < month; m++) {
        days += DaysInMonth(m, year);
    }
    days += day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static Timekeeping::DateTime EpochToDate(int64_t epoch) {
    Timekeeping::DateTime dt = {};
    int64_t rem = epoch;
    int year = 1970;
    while (true) {
        int64_t daysInYear = IsLeapYear(year) ? 366 : 365;
        if (rem < daysInYear * 86400) break;
        rem -= daysInYear * 86400;
        year++;
    }
    dt.Year = (uint16_t)year;

    int dayOfYear = (int)(rem / 86400);
    rem -= (int64_t)dayOfYear * 86400;

    int month = 1;
    while (month <= 12) {
        int dim = DaysInMonth(month, year);
        if (dayOfYear < dim) break;
        dayOfYear -= dim;
        month++;
    }
    dt.Month = (uint8_t)month;
    dt.Day = (uint8_t)(dayOfYear + 1);

    dt.Hour = (uint8_t)(rem / 3600);
    rem %= 3600;
    dt.Minute = (uint8_t)(rem / 60);
    dt.Second = (uint8_t)(rem % 60);
    return dt;
}

void Timekeeping::Init(uint16_t Year, uint8_t Month, uint8_t Day, uint8_t Hour, uint8_t Minute, uint8_t Second) {
    g_bootEpoch = DateToEpoch(Year, Month, Day, Hour, Minute, Second);
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

int64_t Timekeeping::GetUnixTimestamp() {
    return g_bootEpoch + (int64_t)(Timekeeping::GetMilliseconds() / 1000);
}

Timekeeping::DateTime Timekeeping::GetDateTime() {
    return EpochToDate(GetUnixTimestamp());
}