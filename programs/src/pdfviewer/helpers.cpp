/*
 * helpers.cpp
 * Pixel drawing and string helpers
 * Copyright (c) 2026 Daniel Hammer
 */

#include "pdfviewer.h"

void px_fill(uint32_t* px, int bw, int bh,
             int x, int y, int w, int h, Color c) {
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        for (int col = x0; col < x1; col++)
            px[row * bw + col] = v;
}

void px_hline(uint32_t* px, int bw, int bh,
              int x, int y, int w, Color c) {
    if (y < 0 || y >= bh) return;
    uint32_t v = c.to_pixel();
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > bw ? bw : x + w;
    for (int col = x0; col < x1; col++)
        px[y * bw + col] = v;
}

void px_vline(uint32_t* px, int bw, int bh,
              int x, int y, int h, Color c) {
    if (x < 0 || x >= bw) return;
    uint32_t v = c.to_pixel();
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > bh ? bh : y + h;
    for (int row = y0; row < y1; row++)
        px[row * bw + x] = v;
}

void px_rect(uint32_t* px, int bw, int bh,
             int x, int y, int w, int h, Color c) {
    px_hline(px, bw, bh, x, y, w, c);
    px_hline(px, bw, bh, x, y + h - 1, w, c);
    px_vline(px, bw, bh, x, y, h, c);
    px_vline(px, bw, bh, x + w - 1, y, h, c);
}

void px_fill_rounded(uint32_t* px, int bw, int bh,
                     int x, int y, int w, int h, int r, Color c) {
    uint32_t v = c.to_pixel();
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        int inset = 0;
        if (row < r) {
            int dy = r - 1 - row;
            if (r == 3) {
                if (dy >= 2) inset = 2;
                else if (dy >= 1) inset = 1;
            } else {
                for (int i = r; i > 0; i--) {
                    int dx = r - i;
                    if (dx * dx + dy * dy < r * r) { inset = i; break; }
                }
            }
        } else if (row >= h - r) {
            int dy = row - (h - r);
            if (r == 3) {
                if (dy >= 2) inset = 2;
                else if (dy >= 1) inset = 1;
            } else {
                for (int i = r; i > 0; i--) {
                    int dx = r - i;
                    if (dx * dx + dy * dy < r * r) { inset = i; break; }
                }
            }
        }
        int x0 = x + inset;
        int x1 = x + w - inset;
        if (x0 < 0) x0 = 0;
        if (x1 > bw) x1 = bw;
        for (int col = x0; col < x1; col++)
            px[py * bw + col] = v;
    }
}

int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void str_cpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
