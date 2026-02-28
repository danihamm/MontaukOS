/*
    * svg.hpp
    * MontaukOS SVG icon parser and scanline rasterizer
    * Handles the Flat-Remix symbolic icon subset (path, circle, rect)
    * All math uses 16.16 fixed-point -- NO floating point.
    * Copyright (c) 2025 Daniel Hammer
*/

#pragma once
#include "gui/gui.hpp"
#include <montauk/syscall.h>

namespace gui {

// ---------------------------------------------------------------------------
// SVG icon result
// ---------------------------------------------------------------------------
struct SvgIcon {
    uint32_t* pixels;   // ARGB pixel data (heap-allocated)
    int width;
    int height;
};

// ---------------------------------------------------------------------------
// Edge used by the scanline rasterizer
// ---------------------------------------------------------------------------
struct SvgEdge {
    fixed_t x0, y0, x1, y1;
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int SVG_MAX_EDGES     = 8192;
static constexpr int SVG_MAX_PATH_LEN  = 8192;
static constexpr int SVG_MAX_FILE_SIZE = 32768;
static constexpr int SVG_BEZIER_STEPS  = 8;
static constexpr int SVG_MAX_GRADIENTS = 8;

// Gradient color table — stores first stop color for url(#id) resolution
struct SvgGradient {
    char id[32];
    Color color;   // first stop-color
};

struct SvgGradientTable {
    SvgGradient entries[SVG_MAX_GRADIENTS];
    int count;

    void clear() { count = 0; }

    void add(const char* id, Color c) {
        if (count >= SVG_MAX_GRADIENTS) return;
        int i = 0;
        while (id[i] && i < 31) { entries[count].id[i] = id[i]; i++; }
        entries[count].id[i] = '\0';
        entries[count].color = c;
        count++;
    }

    // Look up gradient by id. Returns true if found.
    bool lookup(const char* id, Color* out) const {
        for (int i = 0; i < count; i++) {
            const char* a = entries[i].id;
            const char* b = id;
            bool match = true;
            while (*a && *b) {
                if (*a != *b) { match = false; break; }
                a++; b++;
            }
            if (match && *a == '\0' && *b == '\0') {
                *out = entries[i].color;
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// CSS class color table  (.ClassName { color: #rrggbb; })
// Used to resolve fill:currentColor via the element's class= attribute.
// ---------------------------------------------------------------------------
static constexpr int SVG_MAX_CSS_CLASSES = 8;

struct SvgCssClass {
    char name[48];
    Color color;
};

struct SvgCssTable {
    SvgCssClass entries[SVG_MAX_CSS_CLASSES];
    int count;

    void clear() { count = 0; }

    void add(const char* name, Color c) {
        if (count >= SVG_MAX_CSS_CLASSES) return;
        int i = 0;
        while (name[i] && i < 47) { entries[count].name[i] = name[i]; i++; }
        entries[count].name[i] = '\0';
        entries[count].color = c;
        count++;
    }

    // Look up class by name (first token if multiple classes are listed).
    // Returns true if found.
    bool lookup(const char* cls, Color* out) const {
        for (int i = 0; i < count; i++) {
            const char* a = entries[i].name;
            const char* b = cls;
            bool match = true;
            while (*a && *b) {
                if (*a != *b) { match = false; break; }
                a++; b++;
            }
            if (match && *a == '\0' && (*b == '\0' || *b == ' ')) {
                *out = entries[i].color;
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Fixed-point number parser (NO floating point)
// Parses strings like "3.25", "-0.5", ".1115", "16"
// Returns the number of characters consumed.
// ---------------------------------------------------------------------------
inline int svg_parse_fixed(const char* s, fixed_t* out) {
    const char* p = s;
    bool neg = false;

    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }

    // Integer part
    int32_t integer = 0;
    while (*p >= '0' && *p <= '9') {
        integer = integer * 10 + (*p - '0');
        ++p;
    }

    // Fractional part
    int32_t frac = 0;
    int32_t frac_div = 1;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (frac_div < 100000) {  // prevent overflow
                frac = frac * 10 + (*p - '0');
                frac_div *= 10;
            }
            ++p;
        }
    }

    fixed_t val = int_to_fixed(integer);
    if (frac_div > 1) {
        val += (int32_t)(((int64_t)frac << 16) / frac_div);
    }

    if (neg) val = -val;
    *out = val;
    return (int)(p - s);
}

// ---------------------------------------------------------------------------
// String helpers (no stdlib)
// ---------------------------------------------------------------------------
inline bool svg_char_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool svg_char_is_sep(char c) {
    return svg_char_is_ws(c) || c == ',';
}

inline bool svg_char_is_num_start(char c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

inline bool svg_char_is_cmd(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline int svg_strlen(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}

inline bool svg_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

inline void svg_memset(void* dst, uint8_t val, int n) {
    auto* d = (uint8_t*)dst;
    for (int i = 0; i < n; ++i) d[i] = val;
}

inline void svg_memcpy(void* dst, const void* src, int n) {
    auto* d = (uint8_t*)dst;
    auto* s = (const uint8_t*)src;
    for (int i = 0; i < n; ++i) d[i] = s[i];
}

// ---------------------------------------------------------------------------
// Mini XML attribute extraction
// ---------------------------------------------------------------------------

// Find the next occurrence of needle in haystack (haystack has length hLen).
// Returns pointer to start of match, or nullptr.
inline const char* svg_strstr(const char* haystack, int hLen, const char* needle) {
    int nLen = svg_strlen(needle);
    if (nLen == 0 || nLen > hLen) return nullptr;
    for (int i = 0; i <= hLen - nLen; ++i) {
        if (svg_strncmp(haystack + i, needle, nLen))
            return haystack + i;
    }
    return nullptr;
}

// Check if a character can be part of an XML attribute name
inline bool svg_char_is_attrname(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

// Extract the value of an XML attribute: attr="value"
// The attr string should include a leading space (e.g., " cx") to distinguish
// from substrings of other attribute names.
// Writes into buf (up to maxLen-1 chars), returns length or -1 if not found.
inline int svg_get_attr(const char* tag, int tagLen, const char* attr, char* buf, int maxLen) {
    int attrLen = svg_strlen(attr);
    const char* search_start = tag;
    int search_len = tagLen;

    while (search_len > 0) {
        const char* p = svg_strstr(search_start, search_len, attr);
        if (!p) return -1;

        // Ensure this is the exact attribute name, not a prefix of another
        // (e.g., " r" should not match " rx" or " ry")
        const char* after_name = p + attrLen;
        if (after_name < tag + tagLen && svg_char_is_attrname(*after_name)) {
            // This is a prefix of a longer attribute name, skip and search again
            search_start = after_name;
            search_len = tagLen - (int)(search_start - tag);
            continue;
        }

        p = after_name;
        // skip whitespace around '='
        while (p < tag + tagLen && svg_char_is_ws(*p)) ++p;
        if (p >= tag + tagLen || *p != '=') return -1;
        ++p;
        while (p < tag + tagLen && svg_char_is_ws(*p)) ++p;
        if (p >= tag + tagLen) return -1;
        char quote = *p;
        if (quote != '"' && quote != '\'') return -1;
        ++p;
        int len = 0;
        while (p < tag + tagLen && *p != quote && len < maxLen - 1) {
            buf[len++] = *p++;
        }
        buf[len] = '\0';
        return len;
    }
    return -1;
}

// Parse an integer from a string (no sign, simple decimal).
inline int svg_parse_int(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    return v;
}

// Parse a hex color like "#5c616c" or "#fff" into a Color.
inline Color svg_parse_hex_color(const char* s) {
    if (*s == '#') ++s;
    auto hexval = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
    };
    // Count hex digits
    int len = 0;
    for (const char* p = s; *p; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
            ++len;
        else break;
    }
    if (len == 3) {
        // 3-digit shorthand: #rgb → #rrggbb
        uint8_t r = hexval(s[0]); r = (r << 4) | r;
        uint8_t g = hexval(s[1]); g = (g << 4) | g;
        uint8_t b = hexval(s[2]); b = (b << 4) | b;
        return Color::from_rgb(r, g, b);
    }
    uint8_t r = (hexval(s[0]) << 4) | hexval(s[1]);
    uint8_t g = (hexval(s[2]) << 4) | hexval(s[3]);
    uint8_t b = (hexval(s[4]) << 4) | hexval(s[5]);
    return Color::from_rgb(r, g, b);
}

// ---------------------------------------------------------------------------
// Edge list builder
// ---------------------------------------------------------------------------
struct SvgEdgeList {
    SvgEdge* edges;
    int count;
    int capacity;

    void init(int cap) {
        edges = (SvgEdge*)montauk::alloc(cap * sizeof(SvgEdge));
        count = 0;
        capacity = cap;
    }

    void clear() { count = 0; }

    void add(fixed_t x0, fixed_t y0, fixed_t x1, fixed_t y1) {
        if (count >= capacity) return;
        // skip horizontal edges (they don't contribute to scanline crossings)
        if (y0 == y1) return;
        edges[count++] = {x0, y0, x1, y1};
    }
};

// ---------------------------------------------------------------------------
// Bezier flattening (fixed-point)
// ---------------------------------------------------------------------------

// Cubic bezier: add line segments approximating B(t) for t in [0,1]
inline void svg_flatten_cubic(SvgEdgeList& el,
                              fixed_t x0, fixed_t y0,
                              fixed_t x1, fixed_t y1,
                              fixed_t x2, fixed_t y2,
                              fixed_t x3, fixed_t y3) {
    constexpr int N = SVG_BEZIER_STEPS;
    fixed_t px = x0, py = y0;
    for (int i = 1; i <= N; ++i) {
        // t = i/N in 16.16: (i << 16) / N
        fixed_t t = (int32_t)(((int64_t)i << 16) / N);
        fixed_t omt = int_to_fixed(1) - t;  // 1 - t

        // (1-t)^2 and t^2
        fixed_t omt2 = fixed_mul(omt, omt);
        fixed_t t2 = fixed_mul(t, t);

        // (1-t)^3 and t^3
        fixed_t omt3 = fixed_mul(omt2, omt);
        fixed_t t3 = fixed_mul(t2, t);

        // 3*(1-t)^2*t and 3*(1-t)*t^2
        fixed_t c1 = fixed_mul(omt2, t) * 3;
        fixed_t c2 = fixed_mul(omt, t2) * 3;

        fixed_t nx = fixed_mul(omt3, x0) + fixed_mul(c1, x1) + fixed_mul(c2, x2) + fixed_mul(t3, x3);
        fixed_t ny = fixed_mul(omt3, y0) + fixed_mul(c1, y1) + fixed_mul(c2, y2) + fixed_mul(t3, y3);

        el.add(px, py, nx, ny);
        px = nx;
        py = ny;
    }
}

// Quadratic bezier: add line segments approximating B(t) for t in [0,1]
inline void svg_flatten_quad(SvgEdgeList& el,
                             fixed_t x0, fixed_t y0,
                             fixed_t x1, fixed_t y1,
                             fixed_t x2, fixed_t y2) {
    constexpr int N = SVG_BEZIER_STEPS;
    fixed_t px = x0, py = y0;
    for (int i = 1; i <= N; ++i) {
        fixed_t t = (int32_t)(((int64_t)i << 16) / N);
        fixed_t omt = int_to_fixed(1) - t;

        // (1-t)^2*P0 + 2*(1-t)*t*P1 + t^2*P2
        fixed_t omt2 = fixed_mul(omt, omt);
        fixed_t t2 = fixed_mul(t, t);
        fixed_t c1 = fixed_mul(omt, t) * 2;

        fixed_t nx = fixed_mul(omt2, x0) + fixed_mul(c1, x1) + fixed_mul(t2, x2);
        fixed_t ny = fixed_mul(omt2, y0) + fixed_mul(c1, y1) + fixed_mul(t2, y2);

        el.add(px, py, nx, ny);
        px = nx;
        py = ny;
    }
}

// ---------------------------------------------------------------------------
// Circle to edges: approximate a circle as N line segments
// ---------------------------------------------------------------------------
inline void svg_circle_edges(SvgEdgeList& el, fixed_t cx, fixed_t cy, fixed_t r) {
    // Approximate circle with 16 segments using a precomputed sin/cos table
    // for angles 0, 22.5, 45, ... 337.5 degrees.
    // sin/cos in 16.16 fixed-point for 16 evenly-spaced angles:
    static const fixed_t cos16[16] = {
         65536,  60547,  46341,  25080,      0, -25080, -46341, -60547,
        -65536, -60547, -46341, -25080,      0,  25080,  46341,  60547
    };
    static const fixed_t sin16[16] = {
             0,  25080,  46341,  60547,  65536,  60547,  46341,  25080,
             0, -25080, -46341, -60547, -65536, -60547, -46341, -25080
    };

    fixed_t px = cx + fixed_mul(r, cos16[0]);
    fixed_t py = cy + fixed_mul(r, sin16[0]);
    for (int i = 1; i <= 16; ++i) {
        int idx = i & 15;
        fixed_t nx = cx + fixed_mul(r, cos16[idx]);
        fixed_t ny = cy + fixed_mul(r, sin16[idx]);
        el.add(px, py, nx, ny);
        px = nx;
        py = ny;
    }
}

// ---------------------------------------------------------------------------
// Rounded rect to edges
// ---------------------------------------------------------------------------
inline void svg_rect_edges(SvgEdgeList& el, fixed_t x, fixed_t y, fixed_t w, fixed_t h,
                           fixed_t rx, fixed_t ry) {
    if (rx <= 0 && ry <= 0) {
        // Simple rectangle: 4 edges
        fixed_t x2 = x + w;
        fixed_t y2 = y + h;
        el.add(x,  y,  x2, y);   // top
        el.add(x2, y,  x2, y2);  // right
        el.add(x2, y2, x,  y2);  // bottom
        el.add(x,  y2, x,  y);   // left
        return;
    }

    // Clamp radii
    fixed_t half_w = w >> 1;
    fixed_t half_h = h >> 1;
    if (rx > half_w) rx = half_w;
    if (ry > half_h) ry = half_h;

    // Quarter-circle corner with 4 segments per corner.
    // cos/sin for 0, 22.5, 45, 67.5, 90 degrees (5 points, 4 segments):
    static const fixed_t qcos[5] = { 65536, 60547, 46341, 25080, 0 };
    static const fixed_t qsin[5] = { 0, 25080, 46341, 60547, 65536 };

    // Corners: top-right, bottom-right, bottom-left, top-left
    // Each corner has a center and a quadrant direction for cos/sin application
    struct Corner { fixed_t cx, cy; int sx, sy; };
    Corner corners[4] = {
        { x + w - rx, y + ry,       1, -1 },  // top-right
        { x + w - rx, y + h - ry,   1,  1 },  // bottom-right
        { x + rx,     y + h - ry,  -1,  1 },  // bottom-left
        { x + rx,     y + ry,      -1, -1 },  // top-left
    };

    for (int c = 0; c < 4; ++c) {
        Corner& cn = corners[c];
        fixed_t px = cn.cx + fixed_mul(rx, qcos[0]) * cn.sx;
        fixed_t py = cn.cy + fixed_mul(ry, qsin[0]) * cn.sy;
        for (int i = 1; i <= 4; ++i) {
            fixed_t nx = cn.cx + fixed_mul(rx, qcos[i]) * cn.sx;
            fixed_t ny = cn.cy + fixed_mul(ry, qsin[i]) * cn.sy;
            el.add(px, py, nx, ny);
            px = nx;
            py = ny;
        }
    }

    // Straight edges between corners
    // Top edge: top-left corner end -> top-right corner start
    el.add(x + rx,     y,         x + w - rx, y);
    // Right edge: top-right corner end -> bottom-right corner start
    el.add(x + w,      y + ry,    x + w,      y + h - ry);
    // Bottom edge: bottom-right corner end -> bottom-left corner start
    el.add(x + w - rx, y + h,     x + rx,     y + h);
    // Left edge: bottom-left corner end -> top-left corner start
    el.add(x,          y + h - ry, x,          y + ry);
}

// ---------------------------------------------------------------------------
// Path command parser: tokenize the 'd' attribute
// ---------------------------------------------------------------------------
struct SvgPathParser {
    const char* data;
    int len;
    int pos;

    void init(const char* d, int l) { data = d; len = l; pos = 0; }

    void skip_separators() {
        while (pos < len && svg_char_is_sep(data[pos])) ++pos;
    }

    bool has_more() const { return pos < len; }

    // Peek at what's next: command letter, number, or end
    bool next_is_number() {
        skip_separators();
        if (pos >= len) return false;
        return svg_char_is_num_start(data[pos]);
    }

    char read_command() {
        skip_separators();
        if (pos >= len) return '\0';
        if (svg_char_is_cmd(data[pos]) && data[pos] != 'e' && data[pos] != 'E') {
            return data[pos++];
        }
        return '\0';
    }

    fixed_t read_number() {
        skip_separators();
        if (pos >= len) return 0;
        fixed_t val = 0;
        int consumed = svg_parse_fixed(data + pos, &val);
        pos += consumed;
        return val;
    }
};

// ---------------------------------------------------------------------------
// Process an SVG path 'd' attribute into edges
// ---------------------------------------------------------------------------
inline void svg_path_to_edges(SvgEdgeList& el, const char* d, int dLen,
                              fixed_t scale_x, fixed_t scale_y,
                              fixed_t off_x, fixed_t off_y) {
    SvgPathParser pp;
    pp.init(d, dLen);

    fixed_t cur_x = 0, cur_y = 0;     // current point
    fixed_t start_x = 0, start_y = 0; // subpath start
    fixed_t last_cx = 0, last_cy = 0; // last control point (for S/T)
    char last_cmd = '\0';

    auto scale_pt = [&](fixed_t x, fixed_t y, fixed_t* ox, fixed_t* oy) {
        *ox = fixed_mul(x - off_x, scale_x);
        *oy = fixed_mul(y - off_y, scale_y);
    };

    while (pp.has_more()) {
        char cmd = '\0';

        // Try reading a command letter
        pp.skip_separators();
        if (pp.pos < pp.len && svg_char_is_cmd(pp.data[pp.pos]) &&
            pp.data[pp.pos] != 'e' && pp.data[pp.pos] != 'E') {
            cmd = pp.data[pp.pos++];
        } else if (pp.next_is_number()) {
            // Implicit repeat of last command
            // After M, implicit repeat is L; after m, implicit repeat is l
            if (last_cmd == 'M') cmd = 'L';
            else if (last_cmd == 'm') cmd = 'l';
            else cmd = last_cmd;
        } else {
            // Skip unknown character
            if (pp.pos < pp.len) pp.pos++;
            continue;
        }

        if (cmd == '\0') break;

        switch (cmd) {
        case 'M': {
            fixed_t x = pp.read_number();
            fixed_t y = pp.read_number();
            cur_x = x; cur_y = y;
            start_x = x; start_y = y;
            last_cmd = 'M';
            break;
        }
        case 'm': {
            fixed_t dx = pp.read_number();
            fixed_t dy = pp.read_number();
            cur_x += dx; cur_y += dy;
            start_x = cur_x; start_y = cur_y;
            last_cmd = 'm';
            break;
        }
        case 'L': {
            fixed_t x = pp.read_number();
            fixed_t y = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x, y, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_x = x; cur_y = y;
            last_cmd = 'L';
            break;
        }
        case 'l': {
            fixed_t dx = pp.read_number();
            fixed_t dy = pp.read_number();
            fixed_t nx = cur_x + dx, ny = cur_y + dy;
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(nx, ny, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_x = nx; cur_y = ny;
            last_cmd = 'l';
            break;
        }
        case 'H': {
            fixed_t x = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x, cur_y, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_x = x;
            last_cmd = 'H';
            break;
        }
        case 'h': {
            fixed_t dx = pp.read_number();
            fixed_t nx = cur_x + dx;
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(nx, cur_y, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_x = nx;
            last_cmd = 'h';
            break;
        }
        case 'V': {
            fixed_t y = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(cur_x, y, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_y = y;
            last_cmd = 'V';
            break;
        }
        case 'v': {
            fixed_t dy = pp.read_number();
            fixed_t ny = cur_y + dy;
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(cur_x, ny, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_y = ny;
            last_cmd = 'v';
            break;
        }
        case 'C': {
            fixed_t x1 = pp.read_number(), y1 = pp.read_number();
            fixed_t x2 = pp.read_number(), y2 = pp.read_number();
            fixed_t x3 = pp.read_number(), y3 = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x1, y1, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            scale_pt(x3, y3, &sx3, &sy3);
            svg_flatten_cubic(el, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3);
            last_cx = x2; last_cy = y2;
            cur_x = x3; cur_y = y3;
            last_cmd = 'C';
            break;
        }
        case 'c': {
            fixed_t dx1 = pp.read_number(), dy1 = pp.read_number();
            fixed_t dx2 = pp.read_number(), dy2 = pp.read_number();
            fixed_t dx3 = pp.read_number(), dy3 = pp.read_number();
            fixed_t x1 = cur_x + dx1, y1 = cur_y + dy1;
            fixed_t x2 = cur_x + dx2, y2 = cur_y + dy2;
            fixed_t x3 = cur_x + dx3, y3 = cur_y + dy3;
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x1, y1, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            scale_pt(x3, y3, &sx3, &sy3);
            svg_flatten_cubic(el, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3);
            last_cx = x2; last_cy = y2;
            cur_x = x3; cur_y = y3;
            last_cmd = 'c';
            break;
        }
        case 'S': {
            // Smooth cubic: reflect last control point
            fixed_t rcx = cur_x * 2 - last_cx;
            fixed_t rcy = cur_y * 2 - last_cy;
            if (last_cmd != 'C' && last_cmd != 'c' && last_cmd != 'S' && last_cmd != 's') {
                rcx = cur_x; rcy = cur_y;
            }
            fixed_t x2 = pp.read_number(), y2 = pp.read_number();
            fixed_t x3 = pp.read_number(), y3 = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(rcx, rcy, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            scale_pt(x3, y3, &sx3, &sy3);
            svg_flatten_cubic(el, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3);
            last_cx = x2; last_cy = y2;
            cur_x = x3; cur_y = y3;
            last_cmd = 'S';
            break;
        }
        case 's': {
            fixed_t rcx = cur_x * 2 - last_cx;
            fixed_t rcy = cur_y * 2 - last_cy;
            if (last_cmd != 'C' && last_cmd != 'c' && last_cmd != 'S' && last_cmd != 's') {
                rcx = cur_x; rcy = cur_y;
            }
            fixed_t dx2 = pp.read_number(), dy2 = pp.read_number();
            fixed_t dx3 = pp.read_number(), dy3 = pp.read_number();
            fixed_t x2 = cur_x + dx2, y2 = cur_y + dy2;
            fixed_t x3 = cur_x + dx3, y3 = cur_y + dy3;
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(rcx, rcy, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            scale_pt(x3, y3, &sx3, &sy3);
            svg_flatten_cubic(el, sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3);
            last_cx = x2; last_cy = y2;
            cur_x = x3; cur_y = y3;
            last_cmd = 's';
            break;
        }
        case 'Q': {
            fixed_t x1 = pp.read_number(), y1 = pp.read_number();
            fixed_t x2 = pp.read_number(), y2 = pp.read_number();
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x1, y1, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            svg_flatten_quad(el, sx0, sy0, sx1, sy1, sx2, sy2);
            last_cx = x1; last_cy = y1;
            cur_x = x2; cur_y = y2;
            last_cmd = 'Q';
            break;
        }
        case 'q': {
            fixed_t dx1 = pp.read_number(), dy1 = pp.read_number();
            fixed_t dx2 = pp.read_number(), dy2 = pp.read_number();
            fixed_t x1 = cur_x + dx1, y1 = cur_y + dy1;
            fixed_t x2 = cur_x + dx2, y2 = cur_y + dy2;
            fixed_t sx0, sy0, sx1, sy1, sx2, sy2;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x1, y1, &sx1, &sy1);
            scale_pt(x2, y2, &sx2, &sy2);
            svg_flatten_quad(el, sx0, sy0, sx1, sy1, sx2, sy2);
            last_cx = x1; last_cy = y1;
            cur_x = x2; cur_y = y2;
            last_cmd = 'q';
            break;
        }
        case 'A': case 'a': {
            // Arc command: consume parameters but approximate as a line
            // (arcs are rare in these icons)
            fixed_t rx = pp.read_number();
            fixed_t ry = pp.read_number();
            pp.read_number(); // x-rotation
            pp.read_number(); // large-arc-flag
            pp.read_number(); // sweep-flag
            fixed_t x = pp.read_number();
            fixed_t y = pp.read_number();
            if (cmd == 'a') { x += cur_x; y += cur_y; }
            fixed_t sx0, sy0, sx1, sy1;
            scale_pt(cur_x, cur_y, &sx0, &sy0);
            scale_pt(x, y, &sx1, &sy1);
            el.add(sx0, sy0, sx1, sy1);
            cur_x = x; cur_y = y;
            last_cmd = cmd;
            (void)rx; (void)ry;
            break;
        }
        case 'Z': case 'z': {
            if (cur_x != start_x || cur_y != start_y) {
                fixed_t sx0, sy0, sx1, sy1;
                scale_pt(cur_x, cur_y, &sx0, &sy0);
                scale_pt(start_x, start_y, &sx1, &sy1);
                el.add(sx0, sy0, sx1, sy1);
            }
            cur_x = start_x; cur_y = start_y;
            last_cmd = 'Z';
            break;
        }
        default:
            // Unknown command, skip
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Scanline rasterizer (even-odd fill rule)
// ---------------------------------------------------------------------------
inline void svg_rasterize(const SvgEdgeList& el, uint32_t* pixels, int w, int h, uint32_t fill) {
    // Temporary array for x-intersections on each scanline
    // Allocate enough for all edges (each edge can intersect at most once per scanline)
    int maxIsect = el.count + 16;
    fixed_t* isect = (fixed_t*)montauk::alloc(maxIsect * sizeof(fixed_t));

    for (int y = 0; y < h; ++y) {
        // Scanline center in fixed-point
        fixed_t scanY = int_to_fixed(y) + (1 << 15); // y + 0.5

        int isectCount = 0;

        // Find intersections with all edges
        for (int i = 0; i < el.count; ++i) {
            const SvgEdge& e = el.edges[i];
            fixed_t ey0 = e.y0, ey1 = e.y1;

            // Ensure ey0 <= ey1 for the range check
            fixed_t emin = ey0 < ey1 ? ey0 : ey1;
            fixed_t emax = ey0 > ey1 ? ey0 : ey1;

            // Does this edge cross scanY?
            if (scanY < emin || scanY >= emax) continue;

            // Compute x at intersection: x = x0 + (scanY - y0) * (x1 - x0) / (y1 - y0)
            fixed_t dy = ey1 - ey0;
            if (dy == 0) continue; // horizontal, skip
            fixed_t dx = e.x1 - e.x0;
            fixed_t t_num = scanY - ey0;
            // x_intersect = x0 + dx * t_num / dy
            fixed_t x_int = e.x0 + (int32_t)(((int64_t)dx * t_num) / dy);

            if (isectCount < maxIsect)
                isect[isectCount++] = x_int;
        }

        // Sort intersections (simple insertion sort -- usually very few)
        for (int i = 1; i < isectCount; ++i) {
            fixed_t key = isect[i];
            int j = i - 1;
            while (j >= 0 && isect[j] > key) {
                isect[j + 1] = isect[j];
                --j;
            }
            isect[j + 1] = key;
        }

        // Fill between pairs (even-odd rule)
        for (int i = 0; i + 1 < isectCount; i += 2) {
            int x0 = fixed_to_int(isect[i]);
            int x1 = fixed_to_int(isect[i + 1]);

            // Clamp to pixel bounds
            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;

            for (int x = x0; x < x1; ++x) {
                pixels[y * w + x] = fill;
            }
        }
    }

    montauk::free(isect);
}

// ---------------------------------------------------------------------------
// Parse CSS class color mappings out of a <style> block.
// Handles:  .ClassName { color: #rrggbb; }
// ---------------------------------------------------------------------------
inline void svg_parse_css_table(const char* style_data, int style_len, SvgCssTable* css) {
    const char* p   = style_data;
    const char* end = style_data + style_len;
    while (p < end && css->count < SVG_MAX_CSS_CLASSES) {
        // Find next '.' that starts a class selector
        while (p < end && *p != '.') ++p;
        if (p >= end) break;
        ++p; // skip '.'

        // Read class name (stops at whitespace or '{')
        char name[48]; int ni = 0;
        while (p < end && *p != ' ' && *p != '\t' && *p != '{' && ni < 47)
            name[ni++] = *p++;
        name[ni] = '\0';

        // Find opening brace
        while (p < end && *p != '{') ++p;
        if (p >= end) break;
        const char* brace_open = p++;

        // Find closing brace
        const char* brace_close = brace_open;
        while (brace_close < end && *brace_close != '}') ++brace_close;

        // Search for "color:" inside the braces (not "background-color:" etc.)
        int block_len = (int)(brace_close - brace_open);
        const char* cp = svg_strstr(brace_open, block_len, "color:");
        if (cp) {
            // Make sure it's not prefixed (e.g. "background-color:")
            if (cp > brace_open && *(cp - 1) != '-') {
                cp += 6;
                while (cp < brace_close && (*cp == ' ' || *cp == '\t')) ++cp;
                if (cp < brace_close && *cp == '#') {
                    Color c = svg_parse_hex_color(cp);
                    css->add(name, c);
                }
            }
        }
        p = brace_close + 1;
    }
}

// ---------------------------------------------------------------------------
// Resolve a fill value that might be url(#id) against gradient table
// Returns: 1 = resolved, 0 = not a url() ref, -1 = fill="none"
// ---------------------------------------------------------------------------
inline int svg_resolve_fill_value(const char* val, Color* out_color, const SvgGradientTable* grads) {
    if (svg_strncmp(val, "none", 4)) return -1;
    if (*val == '#') { *out_color = svg_parse_hex_color(val); return 1; }
    if (grads && svg_strncmp(val, "url(#", 5)) {
        // Extract id from url(#id)
        const char* id_start = val + 5;
        char id_buf[32];
        int i = 0;
        while (id_start[i] && id_start[i] != ')' && i < 31) {
            id_buf[i] = id_start[i];
            i++;
        }
        id_buf[i] = '\0';
        if (grads->lookup(id_buf, out_color)) return 1;
    }
    return 0; // currentColor or unresolved
}

// ---------------------------------------------------------------------------
// Per-element fill color extraction
// Returns: 1 = color found in out_color, 0 = use default, -1 = fill="none"
// When fill:currentColor is encountered and css != nullptr, the element's
// class= attribute is looked up to resolve the actual color.
// ---------------------------------------------------------------------------
inline int svg_get_element_fill(const char* elem, int elemLen, Color* out_color,
                                const SvgGradientTable* grads = nullptr,
                                const SvgCssTable* css = nullptr) {
    char buf[128];

    // Helper: try to resolve currentColor via the element's class= attribute.
    // ColorScheme-Text is the "foreground" color — callers control it via fill_color,
    // so we skip it here and let the fallback fill_color apply.
    // Accent classes (Highlight, NeutralText, etc.) use their CSS-defined colors.
    auto resolve_current_color = [&]() -> int {
        if (!css || css->count == 0) return 0; // fall back to fill_color
        char cls[64];
        int cLen = svg_get_attr(elem, elemLen, " class", cls, sizeof(cls));
        if (cLen <= 0) return 0;
        if (svg_strncmp(cls, "ColorScheme-Text", 16)) return 0; // use fill_color
        Color c;
        if (css->lookup(cls, &c)) { *out_color = c; return 1; }
        return 0;
    };

    // Check style="..." first (higher CSS priority)
    int sLen = svg_get_attr(elem, elemLen, " style", buf, sizeof(buf));
    if (sLen > 0) {
        // Search for "fill:" that isn't part of "fill-rule:" or "fill-opacity:"
        const char* fp = svg_strstr(buf, sLen, "fill:");
        if (fp) {
            // Verify this is standalone "fill:" not "-fill:" or similar
            if (fp > buf && *(fp - 1) != ';' && *(fp - 1) != ' ' && *(fp - 1) != '\t') {
                // Part of another property name, skip
            } else {
                fp += 5;
                while (*fp == ' ') ++fp;
                // Check for currentColor before calling resolve_fill_value
                if (svg_strncmp(fp, "currentColor", 12))
                    return resolve_current_color();
                return svg_resolve_fill_value(fp, out_color, grads);
            }
        }
    }

    // Check fill="..." attribute
    int fLen = svg_get_attr(elem, elemLen, " fill", buf, sizeof(buf));
    if (fLen > 0) {
        if (svg_strncmp(buf, "currentColor", 12))
            return resolve_current_color();
        return svg_resolve_fill_value(buf, out_color, grads);
    }

    return 0; // no fill specified
}

// ---------------------------------------------------------------------------
// Per-element opacity (0-255)
// ---------------------------------------------------------------------------
inline int svg_get_element_opacity(const char* elem, int elemLen) {
    char buf[32];
    if (svg_get_attr(elem, elemLen, " opacity", buf, sizeof(buf)) > 0) {
        fixed_t val;
        svg_parse_fixed(buf, &val);
        int alpha = (int)(((int64_t)val * 255) >> 16);
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
        return alpha;
    }
    return 255;
}

// ---------------------------------------------------------------------------
// Check if element references a filter (shadow/blur layers)
// ---------------------------------------------------------------------------
inline bool svg_element_has_filter(const char* elem, int elemLen) {
    char buf[64];
    return svg_get_attr(elem, elemLen, " filter", buf, sizeof(buf)) > 0;
}

// ---------------------------------------------------------------------------
// Scanline rasterizer with alpha blending (for multi-color SVGs)
// ---------------------------------------------------------------------------
inline void svg_rasterize_blend(const SvgEdgeList& el, uint32_t* pixels, int w, int h,
                                uint32_t fill, int alpha) {
    if (el.count == 0) return;

    int maxIsect = el.count + 16;
    fixed_t* isect = (fixed_t*)montauk::alloc(maxIsect * sizeof(fixed_t));

    uint32_t fr = (fill >> 16) & 0xFF;
    uint32_t fg = (fill >> 8) & 0xFF;
    uint32_t fb = fill & 0xFF;

    for (int y = 0; y < h; ++y) {
        fixed_t scanY = int_to_fixed(y) + (1 << 15);
        int isectCount = 0;

        for (int i = 0; i < el.count; ++i) {
            const SvgEdge& e = el.edges[i];
            fixed_t ey0 = e.y0, ey1 = e.y1;
            fixed_t emin = ey0 < ey1 ? ey0 : ey1;
            fixed_t emax = ey0 > ey1 ? ey0 : ey1;
            if (scanY < emin || scanY >= emax) continue;
            fixed_t dy = ey1 - ey0;
            if (dy == 0) continue;
            fixed_t dx = e.x1 - e.x0;
            fixed_t t_num = scanY - ey0;
            fixed_t x_int = e.x0 + (int32_t)(((int64_t)dx * t_num) / dy);
            if (isectCount < maxIsect)
                isect[isectCount++] = x_int;
        }

        for (int i = 1; i < isectCount; ++i) {
            fixed_t key = isect[i];
            int j = i - 1;
            while (j >= 0 && isect[j] > key) {
                isect[j + 1] = isect[j];
                --j;
            }
            isect[j + 1] = key;
        }

        for (int i = 0; i + 1 < isectCount; i += 2) {
            int x0 = fixed_to_int(isect[i]);
            int x1 = fixed_to_int(isect[i + 1]);
            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;

            if (alpha >= 255) {
                for (int x = x0; x < x1; ++x)
                    pixels[y * w + x] = fill;
            } else {
                uint32_t sa = (uint32_t)alpha;
                uint32_t inv_sa = 255 - sa;
                for (int x = x0; x < x1; ++x) {
                    uint32_t dst = pixels[y * w + x];
                    uint32_t da = (dst >> 24) & 0xFF;
                    uint32_t dr = (dst >> 16) & 0xFF;
                    uint32_t dg = (dst >> 8) & 0xFF;
                    uint32_t db = dst & 0xFF;
                    uint32_t out_a = sa + (da * inv_sa + 127) / 255;
                    uint32_t rr = (fr * sa + dr * inv_sa + 128) / 255;
                    uint32_t gg = (fg * sa + dg * inv_sa + 128) / 255;
                    uint32_t bb = (fb * sa + db * inv_sa + 128) / 255;
                    pixels[y * w + x] = (out_a << 24) | (rr << 16) | (gg << 8) | bb;
                }
            }
        }
    }

    montauk::free(isect);
}

// ---------------------------------------------------------------------------
// SVG document parser: extract paths, circles, rects and rasterize
// ---------------------------------------------------------------------------
inline SvgIcon svg_render(const char* svg_data, int svg_len, int target_w, int target_h, Color fill_color) {
    SvgIcon icon;
    icon.width = target_w;
    icon.height = target_h;
    icon.pixels = (uint32_t*)montauk::alloc(target_w * target_h * sizeof(uint32_t));
    // Clear to transparent
    svg_memset(icon.pixels, 0, target_w * target_h * sizeof(uint32_t));

    // Parse SVG dimensions: width and height
    int svg_w = 16, svg_h = 16;
    fixed_t vb_x = 0, vb_y = 0, vb_w = 0, vb_h = 0;
    bool has_viewbox = false;

    // Find <svg tag
    const char* svg_tag = svg_strstr(svg_data, svg_len, "<svg");
    if (svg_tag) {
        // Find the end of the <svg ...> tag
        int tag_offset = (int)(svg_tag - svg_data);
        int tag_end = tag_offset;
        while (tag_end < svg_len && svg_data[tag_end] != '>') ++tag_end;
        int tag_len = tag_end - tag_offset + 1;

        char attr_buf[64];

        // width
        if (svg_get_attr(svg_tag, tag_len, " width", attr_buf, sizeof(attr_buf)) > 0) {
            svg_w = svg_parse_int(attr_buf);
        }
        // height
        if (svg_get_attr(svg_tag, tag_len, " height", attr_buf, sizeof(attr_buf)) > 0) {
            svg_h = svg_parse_int(attr_buf);
        }
        // viewBox
        if (svg_get_attr(svg_tag, tag_len, " viewBox", attr_buf, sizeof(attr_buf)) > 0) {
            has_viewbox = true;
            const char* vp = attr_buf;
            int c;
            c = svg_parse_fixed(vp, &vb_x); vp += c; while (*vp && svg_char_is_sep(*vp)) ++vp;
            c = svg_parse_fixed(vp, &vb_y); vp += c; while (*vp && svg_char_is_sep(*vp)) ++vp;
            c = svg_parse_fixed(vp, &vb_w); vp += c; while (*vp && svg_char_is_sep(*vp)) ++vp;
            svg_parse_fixed(vp, &vb_h);
        }
    }

    if (!has_viewbox) {
        vb_x = 0;
        vb_y = 0;
        vb_w = int_to_fixed(svg_w);
        vb_h = int_to_fixed(svg_h);
    }

    // Compute scale: pixel = (svg_coord - vb_origin) * target_size / vb_size
    fixed_t scale_x = vb_w > 0 ? fixed_div(int_to_fixed(target_w), vb_w) : int_to_fixed(1);
    fixed_t scale_y = vb_h > 0 ? fixed_div(int_to_fixed(target_h), vb_h) : int_to_fixed(1);

    // Pre-parse gradient definitions from <defs> for url(#id) fill resolution
    SvgGradientTable grads;
    grads.clear();
    {
        const char* dp = svg_data;
        const char* dend = svg_data + svg_len;
        while (dp < dend) {
            const char* gp = svg_strstr(dp, (int)(dend - dp), "<linearGradient");
            if (!gp) {
                gp = svg_strstr(dp, (int)(dend - dp), "<radialGradient");
            }
            if (!gp) break;

            // Find end of gradient block (</linearGradient> or </radialGradient>)
            const char* gend = svg_strstr(gp, (int)(dend - gp), "</linearGradient>");
            if (!gend) gend = svg_strstr(gp, (int)(dend - gp), "</radialGradient>");
            if (!gend) gend = svg_strstr(gp, (int)(dend - gp), "/>");
            if (!gend) break;

            // Extract gradient id
            int gtag_end = 0;
            while (gp + gtag_end < dend && gp[gtag_end] != '>') ++gtag_end;
            char grad_id[32];
            int id_len = svg_get_attr(gp, gtag_end + 1, " id", grad_id, sizeof(grad_id));

            if (id_len > 0) {
                // Find first <stop and extract stop-color
                const char* stop = svg_strstr(gp, (int)(gend - gp), "<stop");
                if (stop) {
                    int stop_end = 0;
                    while (stop + stop_end < dend && stop[stop_end] != '>') ++stop_end;
                    char sc_buf[32];
                    if (svg_get_attr(stop, stop_end + 1, " stop-color", sc_buf, sizeof(sc_buf)) > 0) {
                        if (sc_buf[0] == '#') {
                            grads.add(grad_id, svg_parse_hex_color(sc_buf));
                        }
                    }
                }
            }
            dp = gend + 1;
        }
    }

    // Pre-parse CSS class color mappings from <style> block
    SvgCssTable css;
    css.clear();
    {
        const char* sp = svg_strstr(svg_data, svg_len, "<style");
        if (sp) {
            // Advance past the opening tag to the content
            while (sp < svg_data + svg_len && *sp != '>') ++sp;
            if (sp < svg_data + svg_len) ++sp; // skip '>'
            const char* se = svg_strstr(sp, (int)(svg_data + svg_len - sp), "</style>");
            if (se)
                svg_parse_css_table(sp, (int)(se - sp), &css);
        }
    }

    // Shared edge list (cleared per element for multi-color support)
    SvgEdgeList el;
    el.init(SVG_MAX_EDGES);

    // Scan for <path, <circle, <rect elements — rasterize each individually
    // Skip elements inside <defs> blocks (they define reusable items, not rendered directly)
    const char* p = svg_data;
    const char* end = svg_data + svg_len;

    while (p < end) {
        // Find next '<'
        while (p < end && *p != '<') ++p;
        if (p >= end) break;

        int remaining = (int)(end - p);

        // Skip <defs>...</defs> blocks entirely
        if (remaining > 5 && svg_strncmp(p, "<defs", 5) && (svg_char_is_ws(p[5]) || p[5] == '>')) {
            const char* defs_end = svg_strstr(p, remaining, "</defs>");
            if (defs_end) {
                p = defs_end + 7; // skip past </defs>
            } else {
                p += 5;
            }
            continue;
        }

        // Check for <path
        if (remaining > 5 && svg_strncmp(p, "<path", 5) && (svg_char_is_ws(p[5]) || p[5] == '/')) {
            const char* elem_start = p;
            const char* elem_end = p;
            while (elem_end < end && *elem_end != '>') ++elem_end;
            if (elem_end < end) ++elem_end;
            int elem_len = (int)(elem_end - elem_start);

            // Skip filter-referenced elements (shadow/blur layers)
            if (svg_element_has_filter(elem_start, elem_len)) {
                p = elem_end;
                continue;
            }

            // Determine fill color for this element
            Color elem_color = fill_color;
            int fillResult = svg_get_element_fill(elem_start, elem_len, &elem_color, &grads, &css);
            if (fillResult == -1) { p = elem_end; continue; } // fill="none"

            int alpha = svg_get_element_opacity(elem_start, elem_len);

            // Extract and rasterize path
            char d_buf[SVG_MAX_PATH_LEN];
            int d_len = svg_get_attr(elem_start, elem_len, " d", d_buf, SVG_MAX_PATH_LEN);
            if (d_len > 0) {
                el.clear();
                svg_path_to_edges(el, d_buf, d_len, scale_x, scale_y, vb_x, vb_y);
                if (el.count > 0)
                    svg_rasterize_blend(el, icon.pixels, target_w, target_h, elem_color.to_pixel(), alpha);
            }

            p = elem_end;
            continue;
        }

        // Check for <circle
        if (remaining > 7 && svg_strncmp(p, "<circle", 7) && (svg_char_is_ws(p[7]) || p[7] == '/')) {
            const char* elem_start = p;
            const char* elem_end = p;
            while (elem_end < end && *elem_end != '>') ++elem_end;
            if (elem_end < end) ++elem_end;
            int elem_len = (int)(elem_end - elem_start);

            if (svg_element_has_filter(elem_start, elem_len)) {
                p = elem_end;
                continue;
            }

            Color elem_color = fill_color;
            int fillResult = svg_get_element_fill(elem_start, elem_len, &elem_color, &grads, &css);
            if (fillResult == -1) { p = elem_end; continue; }

            int alpha = svg_get_element_opacity(elem_start, elem_len);

            char attr_buf[32];
            fixed_t cx = 0, cy = 0, r = 0;
            if (svg_get_attr(elem_start, elem_len, " cx", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &cx);
            if (svg_get_attr(elem_start, elem_len, " cy", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &cy);
            if (svg_get_attr(elem_start, elem_len, " r", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &r);

            fixed_t scx = fixed_mul(cx - vb_x, scale_x);
            fixed_t scy = fixed_mul(cy - vb_y, scale_y);
            fixed_t srx = fixed_mul(r, scale_x);
            fixed_t sry = fixed_mul(r, scale_y);
            fixed_t sr = (srx + sry) >> 1;

            el.clear();
            svg_circle_edges(el, scx, scy, sr);
            if (el.count > 0)
                svg_rasterize_blend(el, icon.pixels, target_w, target_h, elem_color.to_pixel(), alpha);

            p = elem_end;
            continue;
        }

        // Check for <rect
        if (remaining > 5 && svg_strncmp(p, "<rect", 5) && (svg_char_is_ws(p[5]) || p[5] == '/')) {
            const char* elem_start = p;
            const char* elem_end = p;
            while (elem_end < end && *elem_end != '>') ++elem_end;
            if (elem_end < end) ++elem_end;
            int elem_len = (int)(elem_end - elem_start);

            if (svg_element_has_filter(elem_start, elem_len)) {
                p = elem_end;
                continue;
            }

            Color elem_color = fill_color;
            int fillResult = svg_get_element_fill(elem_start, elem_len, &elem_color, &grads, &css);
            if (fillResult == -1) { p = elem_end; continue; }

            int alpha = svg_get_element_opacity(elem_start, elem_len);

            char attr_buf[32];
            fixed_t rx_val = 0, ry_val = 0, rw = 0, rh = 0, rrx = 0, rry = 0;

            if (svg_get_attr(elem_start, elem_len, " x", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &rx_val);
            if (svg_get_attr(elem_start, elem_len, " y", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &ry_val);
            if (svg_get_attr(elem_start, elem_len, " width", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &rw);
            if (svg_get_attr(elem_start, elem_len, " height", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &rh);
            if (svg_get_attr(elem_start, elem_len, " rx", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &rrx);
            if (svg_get_attr(elem_start, elem_len, " ry", attr_buf, sizeof(attr_buf)) > 0)
                svg_parse_fixed(attr_buf, &rry);

            fixed_t sx = fixed_mul(rx_val - vb_x, scale_x);
            fixed_t sy = fixed_mul(ry_val - vb_y, scale_y);
            fixed_t sw = fixed_mul(rw, scale_x);
            fixed_t sh = fixed_mul(rh, scale_y);
            fixed_t srx = fixed_mul(rrx, scale_x);
            fixed_t sry = fixed_mul(rry, scale_y);

            el.clear();
            svg_rect_edges(el, sx, sy, sw, sh, srx, sry);
            if (el.count > 0)
                svg_rasterize_blend(el, icon.pixels, target_w, target_h, elem_color.to_pixel(), alpha);

            p = elem_end;
            continue;
        }

        ++p;
    }

    montauk::free(el.edges);
    return icon;
}

// ---------------------------------------------------------------------------
// Load SVG from VFS and render
// ---------------------------------------------------------------------------
inline SvgIcon svg_load(const char* vfs_path, int target_w, int target_h, Color fill_color) {
    int fd = montauk::open(vfs_path);
    if (fd < 0) {
        return {nullptr, 0, 0};
    }

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > SVG_MAX_FILE_SIZE) {
        montauk::close(fd);
        return {nullptr, 0, 0};
    }

    char* buf = (char*)montauk::alloc(size + 1);
    montauk::read(fd, (uint8_t*)buf, 0, size);
    montauk::close(fd);
    buf[size] = '\0';

    // 4x supersampling: render at 4x resolution, then downsample with box filter
    static constexpr int SS = 4;
    int hi_w = target_w * SS;
    int hi_h = target_h * SS;

    SvgIcon hi = svg_render(buf, (int)size, hi_w, hi_h, fill_color);
    montauk::free(buf);

    if (!hi.pixels) return {nullptr, 0, 0};

    // Allocate final icon at target resolution
    uint32_t* out = (uint32_t*)montauk::alloc(target_w * target_h * 4);
    for (int i = 0; i < target_w * target_h; i++) out[i] = 0;

    // Downsample: average each SSxSS block using premultiplied alpha
    for (int dy = 0; dy < target_h; dy++) {
        for (int dx = 0; dx < target_w; dx++) {
            uint32_t sum_a = 0, sum_pr = 0, sum_pg = 0, sum_pb = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    uint32_t px = hi.pixels[(dy * SS + sy) * hi_w + (dx * SS + sx)];
                    uint32_t a = (px >> 24) & 0xFF;
                    uint32_t r = (px >> 16) & 0xFF;
                    uint32_t g = (px >>  8) & 0xFF;
                    uint32_t b =  px        & 0xFF;
                    // Premultiply before averaging (rasterizer outputs straight alpha)
                    sum_a  += a;
                    sum_pr += r * a;
                    sum_pg += g * a;
                    sum_pb += b * a;
                }
            }
            uint32_t avg_a = sum_a / (SS * SS);

            // Un-premultiply for final straight-alpha output
            uint32_t avg_r = 0, avg_g = 0, avg_b = 0;
            if (sum_a > 0) {
                avg_r = sum_pr / sum_a;
                avg_g = sum_pg / sum_a;
                avg_b = sum_pb / sum_a;
                if (avg_r > 255) avg_r = 255;
                if (avg_g > 255) avg_g = 255;
                if (avg_b > 255) avg_b = 255;
            }

            out[dy * target_w + dx] = (avg_a << 24) | (avg_r << 16) | (avg_g << 8) | avg_b;
        }
    }

    montauk::free(hi.pixels);
    return {out, target_w, target_h};
}

// ---------------------------------------------------------------------------
// Free icon pixel data
// ---------------------------------------------------------------------------
inline void svg_free(SvgIcon& icon) {
    if (icon.pixels) montauk::free(icon.pixels);
    icon.pixels = nullptr;
    icon.width = 0;
    icon.height = 0;
}

} // namespace gui
