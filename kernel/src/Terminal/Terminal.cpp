/*
    * Terminal.cpp
    * Terminal implementation
    * Copyright (c) 2025 Daniel Hammer
*/

#include "Terminal.hpp"

#define FLANTERM_IN_FLANTERM
#include "../Libraries/flanterm/src/flanterm_backends/fb.h"
#include "../Libraries/flanterm/src/flanterm.h"

#include "../Libraries/String.hpp"
#include "../Libraries/Memory.hpp"
#include <CppLib/CString.hpp>

namespace Kt {
    flanterm_context *ctx;
    std::size_t g_terminal_width = 0;

    // Kernel log depth counter and suppression flag
    uint32_t g_kernelLogDepth = 0;
    bool g_suppressKernelLog = false;

    // 64KB ring buffer for kernel log messages
    static constexpr uint64_t KLOG_BUF_SIZE = 65536;
    static char g_klogBuf[KLOG_BUF_SIZE];
    static uint64_t g_klogHead = 0;  // next write position
    static uint64_t g_klogCount = 0; // total chars stored (capped at KLOG_BUF_SIZE)

    // ANSI escape sequence filter state
    static bool g_ansiEscape = false;

    static void RingBufferAppend(char c) {
        // Strip ANSI escape sequences
        if (c == '\033') {
            g_ansiEscape = true;
            return;
        }
        if (g_ansiEscape) {
            if (c == 'm') {
                g_ansiEscape = false;
            }
            return;
        }

        g_klogBuf[g_klogHead] = c;
        g_klogHead = (g_klogHead + 1) % KLOG_BUF_SIZE;
        if (g_klogCount < KLOG_BUF_SIZE) {
            g_klogCount++;
        }
    }

    // Maximum grid cells allocated at init (scale 1,1). Used to validate
    // that a requested scale does not exceed the original buffer capacity.
    static std::size_t g_max_grid_cells = 0;

    // Custom plot_char that works for any font_scale_x/y >= 1.
    // This is the same algorithm as flanterm's plot_char_scaled_uncanvas
    // but lives outside fb.c so we can install it after rescaling.
    static void plot_char_universal(struct flanterm_context *_ctx,
                                    struct flanterm_fb_char *c,
                                    size_t x, size_t y) {
        struct flanterm_fb_context *fbctx = (struct flanterm_fb_context *)_ctx;

        if (x >= _ctx->cols || y >= _ctx->rows) {
            return;
        }

        uint32_t default_bg = fbctx->default_bg;
        uint32_t bg = c->bg == 0xffffffff ? default_bg : c->bg;
        uint32_t fg = c->fg == 0xffffffff ? fbctx->default_fg : c->fg;

        x = fbctx->offset_x + x * fbctx->glyph_width;
        y = fbctx->offset_y + y * fbctx->glyph_height;

        bool *glyph = &fbctx->font_bool[c->c * fbctx->font_height * fbctx->font_width];

        // Only ROTATE_0 is used in MontaukOS
        volatile uint32_t *dest = fbctx->framebuffer + x + y * (fbctx->pitch / 4);
        size_t stride = fbctx->pitch / 4;

        for (size_t gy = 0; gy < fbctx->glyph_height; gy++) {
            size_t fy = gy / fbctx->font_scale_y;
            volatile uint32_t *fb_line = dest;
            bool *glyph_pointer = glyph + (fy * fbctx->font_width);
            for (size_t fx = 0; fx < fbctx->font_width; fx++) {
                for (size_t i = 0; i < fbctx->font_scale_x; i++) {
                    *fb_line = *glyph_pointer ? fg : bg;
                    fb_line++;
                }
                glyph_pointer++;
            }
            dest += stride;
        }
    }

    void Rescale(std::size_t scale_x, std::size_t scale_y) {
        if (scale_x == 0) scale_x = 1;
        if (scale_y == 0) scale_y = 1;

        struct flanterm_fb_context *fbctx = (struct flanterm_fb_context *)ctx;

        // Calculate new dimensions
        size_t new_glyph_w = fbctx->font_width * scale_x;
        size_t new_glyph_h = fbctx->font_height * scale_y;
        size_t new_cols = fbctx->width / new_glyph_w;
        size_t new_rows = fbctx->height / new_glyph_h;

        if (new_cols == 0 || new_rows == 0) return;

        // Ensure the new grid fits within original buffer allocation
        if (new_cols * new_rows > g_max_grid_cells) return;

        // Update scale and glyph dimensions
        fbctx->font_scale_x = scale_x;
        fbctx->font_scale_y = scale_y;
        fbctx->glyph_width = new_glyph_w;
        fbctx->glyph_height = new_glyph_h;

        // Update terminal grid dimensions
        ctx->cols = new_cols;
        ctx->rows = new_rows;

        // Center the text area
        fbctx->offset_x = (fbctx->width % new_glyph_w) / 2;
        fbctx->offset_y = (fbctx->height % new_glyph_h) / 2;

        // Install our universal plot_char
        fbctx->plot_char = plot_char_universal;

        // Reinitialize grid data (reuse existing buffers)
        for (size_t i = 0; i < new_rows * new_cols; i++) {
            fbctx->grid[i].c = ' ';
            fbctx->grid[i].fg = fbctx->text_fg;
            fbctx->grid[i].bg = fbctx->text_bg;
        }

        fbctx->queue_i = 0;
        memset(fbctx->queue, 0, new_rows * new_cols * sizeof(struct flanterm_fb_queue_item));
        memset(fbctx->map, 0, new_rows * new_cols * sizeof(struct flanterm_fb_queue_item *));

        // Clear the framebuffer
        for (size_t y = 0; y < fbctx->height; y++) {
            volatile uint32_t *row = fbctx->framebuffer + y * (fbctx->pitch / 4);
            for (size_t x = 0; x < fbctx->width; x++) {
                row[x] = fbctx->default_bg;
            }
        }

        // Reset terminal state and refresh
        flanterm_context_reinit(ctx);
        flanterm_full_refresh(ctx);
    }

    std::size_t GetFontScaleX() {
        struct flanterm_fb_context *fbctx = (struct flanterm_fb_context *)ctx;
        return fbctx->font_scale_x;
    }

    std::size_t GetFontScaleY() {
        struct flanterm_fb_context *fbctx = (struct flanterm_fb_context *)ctx;
        return fbctx->font_scale_y;
    }

    void UpdatePanelBar(CString panelText) {
        kout << "\033[s";
        kout << "\033[H";

        int panelWidth = g_terminal_width / 9;

        kout << "\033[44m" << "\033[97m";
        kout << panelText;
        for (int i = static_cast<int>(Lib::strlen(panelText)); i < panelWidth; ++i)
            kout << " ";
        kout << "\033[0m";
        kout << "\033[u";
    }

    void Initialize(std::uint32_t *framebuffer, std::size_t width, std::size_t height, std::size_t pitch,
        std::uint8_t red_mask_size, std::uint8_t red_mask_shift,
        std::uint8_t green_mask_size, std::uint8_t green_mask_shift,
        std::uint8_t blue_mask_size, std::uint8_t blue_mask_shift
        )
    {
        ctx = flanterm_fb_init(
            NULL,
            NULL,
            framebuffer,
            width, height, pitch,
            red_mask_size, red_mask_shift,
            green_mask_size, green_mask_shift,
            blue_mask_size, blue_mask_shift,
            NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, 0, 0, 1,
            1, 1,
            0,
            FLANTERM_FB_ROTATE_0
        );

        g_terminal_width = width;

        // Store max grid cells for rescale buffer bounds checking
        g_max_grid_cells = ctx->cols * ctx->rows;

        // Install our universal plot_char so rescaling works at any scale
        struct flanterm_fb_context *fbctx = (struct flanterm_fb_context *)ctx;
        fbctx->plot_char = plot_char_universal;

        UpdatePanelBar("Initializing...");
        kout << "\n\n\n";
    }

    void Putchar(char c) {
        if (g_kernelLogDepth > 0) {
            if (c == '\n') {
                RingBufferAppend('\r');
                RingBufferAppend('\n');
            } else {
                RingBufferAppend(c);
            }

            if (g_suppressKernelLog) {
                return;
            }
        }

        if (c == '\n') {
            flanterm_write(ctx, "\r\n", 2);
            return;
        }
        flanterm_write(ctx, &c, 1);
    }

    void Print(const char *text) {
        for (size_t i = 0; text[i] != '\0'; i++) {
            Putchar(text[i]);
        }
    }

    void SuppressKernelLog() {
        g_suppressKernelLog = true;
    }

    int64_t ReadKernelLog(char* buf, uint64_t size) {
        if (buf == nullptr || size == 0) return 0;

        uint64_t toRead = g_klogCount;
        if (toRead > size) toRead = size;

        // Start position: oldest character in the ring buffer
        uint64_t start;
        if (g_klogCount < KLOG_BUF_SIZE) {
            start = 0;
        } else {
            start = g_klogHead; // head points to the oldest entry when full
        }

        // Copy from oldest to newest, skipping entries if buffer too small
        uint64_t skipCount = g_klogCount - toRead;
        uint64_t readPos = (start + skipCount) % KLOG_BUF_SIZE;

        for (uint64_t i = 0; i < toRead; i++) {
            buf[i] = g_klogBuf[readPos];
            readPos = (readPos + 1) % KLOG_BUF_SIZE;
        }

        return (int64_t)toRead;
    }

};
