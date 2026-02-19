/*
    * Terminal.hpp
    * Terminal implementation
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include <cstdint>
#include <cstddef>

#include <Libraries/String.hpp>
#include <CppLib/Stream.hpp>

using namespace kcp;

namespace Kt
{
    constexpr const char *newline = "\r\n";

    namespace screen
    {
        constexpr const char *clear = "\033[2J";
        constexpr const char *cursor_reset = "\033[H";

        namespace colors {
            constexpr const char* black = "\u001b[30m";
            constexpr const char* red = "\u001b[31m";
            constexpr const char* green = "\u001b[32m";
            constexpr const char* blue = "\u001b[34m";
            constexpr const char* cyan = "\u001b[36m";
            constexpr const char* yellow = "\u001b[33m";
            constexpr const char* magenta = "\u001b[35m";

            constexpr const char* white = "\u001b[37m";
        };
    };
    
    void Initialize(std::uint32_t *framebuffer, std::size_t width, std::size_t height, std::size_t pitch,
                    std::uint8_t red_mask_size, std::uint8_t red_mask_shift,
                    std::uint8_t green_mask_size, std::uint8_t green_mask_shift,
                    std::uint8_t blue_mask_size, std::uint8_t blue_mask_shift);
    void Putchar(char c);
    void Print(const char *text);

    void UpdatePanelBar(const char* panelText);

    void Rescale(std::size_t font_scale_x, std::size_t font_scale_y);
    std::size_t GetFontScaleX();
    std::size_t GetFontScaleY();

    inline base base_custom(int custom)
    {
        return (base)custom;
    }

    class KernelOutStream
    {
    public:
        base streamBaseType = base::dec;

        // C++ streaming operator like cout
        friend KernelOutStream &operator<<(KernelOutStream &t, const char *string)
        {
            Print(string);
            return t;
        }

        // C++ streaming operator like cout
        friend KernelOutStream &operator<<(KernelOutStream &t, const char chr)
        {
            Putchar(chr);
            return t;
        }

        friend KernelOutStream &operator<<(KernelOutStream &t, int number)
        {
            Print(Lib::int2basestr(number, t.streamBaseType));

            return t;
        }

        friend KernelOutStream &operator<<(KernelOutStream &t, std::uint32_t number)
        {
            Print(Lib::uint2basestr(number, t.streamBaseType));

            return t;
        }

        friend KernelOutStream &operator<<(KernelOutStream &t, std::uint64_t number)
        {
            Print(Lib::u64_2_basestr(number, t.streamBaseType));

            return t;
        }

        friend KernelOutStream &operator<<(KernelOutStream &t, base newBase)
        {
            t.streamBaseType = newBase;

            return t;
        }
    };

    enum KernelLogLevel {
        INFO,
        WARNING,
        ERROR,
        DEBUG,
        OK
    };

    extern uint32_t g_kernelLogDepth;
    extern bool g_suppressKernelLog;

    void SuppressKernelLog();
    int64_t ReadKernelLog(char* buf, uint64_t size);

    class KernelLogStream {
        KernelOutStream localStream{};

        const char* componentName = "";

public:
        const char* LogLevelToStringWithColor(KernelLogLevel level) {
            switch (level) {
                case INFO:
                    return "\e[0;36mINFO\e[0m"; // Cyan
                case WARNING:
                    return "\e[0;33mWARNING\e[0m"; // Yellow
                case ERROR:
                    return "\e[0;31mERROR\e[0m"; // Red
                case DEBUG:
                    return "\e[0;35mDEBUG\e[0m"; // Magenta
                case OK:
                    return "\e[0;32mOK\e[0m"; // Green
                default:
                    return "UNKNOWN";
            }
        }

        KernelLogStream(KernelLogLevel level, const char* desiredComponentName) { /* level for potential future rich event logging */
            g_kernelLogDepth++;
            componentName = desiredComponentName;

            localStream << componentName << ": " << "[" << LogLevelToStringWithColor(level) << "] ";
        }

        ~KernelLogStream() {
            localStream << newline;
            g_kernelLogDepth--;
        }

        template<typename T>
        KernelLogStream &operator<<(T item) {
            localStream << item;

            return *this;
        }
    };

};

extern Kt::KernelOutStream kout;

namespace Kt
{
    class KernelErrorStream
    {
    public:
        template <typename T>
        friend KernelErrorStream &operator<<(KernelErrorStream &t, T value)
        {
            kout << "\e[0;31m" << value << "\e[0m";

            return t;
        }
    };
};

extern Kt::KernelErrorStream kerr;

// Forward-declare flanterm context for syscall access
struct flanterm_context;
namespace Kt {
    extern flanterm_context *ctx;
}