# Writing GUI Applications for MontaukOS

This guide covers how to build graphical applications for MontaukOS, from standalone Window Server clients to desktop-integrated apps.

## Table of Contents

- [App Types](#app-types)
- [Standalone Window Server Apps](#standalone-window-server-apps)
- [Desktop-Integrated Apps](#desktop-integrated-apps)
- [Drawing and Rendering](#drawing-and-rendering)
- [Text and Fonts](#text-and-fonts)
- [Input Handling](#input-handling)
- [Widgets](#widgets)
- [Colors and Theming](#colors-and-theming)
- [Memory Management](#memory-management)
- [Networking and HTTPS](#networking-and-https)
- [App Manifests](#app-manifests)
- [Build System](#build-system)

---

## App Types

MontaukOS supports two kinds of GUI applications:

| | Standalone Apps | Desktop Apps |
|---|---|---|
| **Location** | `programs/src/<appname>/` | `programs/src/desktop/apps/` |
| **Window** | Own process, shared-memory pixel buffer | Embedded in desktop compositor |
| **Event loop** | `win_poll()` syscall | Callback-driven (`on_draw`, `on_mouse`, `on_key`) |
| **Drawing** | Direct pixel buffer writes | `Canvas` abstraction |
| **Examples** | Spreadsheet, Music, Wikipedia | Calculator, File Manager, Terminal |

**Choose standalone** when you need a separate process (e.g., networking, heavy computation, isolation). **Choose desktop-integrated** for lightweight tools that benefit from tight compositor integration.

---

## Standalone Window Server Apps

Standalone apps are separate ELF binaries that communicate with the Window Server through syscalls. They get a shared-memory pixel buffer and manage their own event loop.

### Minimal Example

```cpp
#include <montauk/syscall.h>
#include <montauk/heap.h>
#include <montauk/string.h>
#include <gui/gui.hpp>
#include <gui/truetype.hpp>

static TrueTypeFont* g_font;
static int g_win_w, g_win_h;

static void render(uint32_t* pixels) {
    // Clear background
    for (int i = 0; i < g_win_w * g_win_h; i++)
        pixels[i] = Color::from_rgb(0xFF, 0xFF, 0xFF).to_pixel();

    // Draw text
    if (g_font)
        g_font->draw_to_buffer(pixels, g_win_w, g_win_h,
                               20, 30, "Hello, MontaukOS!",
                               Color::from_rgb(0x33, 0x33, 0x33), 18);
}

extern "C" void _start() {
    // Load font
    g_font = new TrueTypeFont();
    g_font->init("0:/fonts/Roboto-Medium.ttf");

    // Create window
    g_win_w = 400;
    g_win_h = 300;
    Montauk::WinCreateResult wres;
    montauk::win_create("My App", g_win_w, g_win_h, &wres);
    int win_id = wres.id;
    uint32_t* pixels = (uint32_t*)(uintptr_t)wres.pixelVa;

    // Initial render
    render(pixels);
    montauk::win_present(win_id);

    // Event loop
    while (true) {
        Montauk::WinEvent ev;
        int r = montauk::win_poll(win_id, &ev);

        if (r < 0) break;           // Window destroyed externally
        if (r == 0) {
            montauk::sleep_ms(16);   // ~60 FPS idle
            continue;
        }

        if (ev.type == 3) break;     // Close event

        if (ev.type == 2) {          // Resize
            g_win_w = ev.resize.w;
            g_win_h = ev.resize.h;
            pixels = (uint32_t*)(uintptr_t)montauk::win_resize(win_id, g_win_w, g_win_h);
        }

        if (ev.type == 0) {          // Keyboard
            // ev.key.ascii, ev.key.scancode, ev.key.pressed
        }

        if (ev.type == 1) {          // Mouse
            // ev.mouse.x, ev.mouse.y, ev.mouse.buttons
        }

        render(pixels);
        montauk::win_present(win_id);
    }

    montauk::win_destroy(win_id);
    montauk::exit(0);
}
```

### Window Lifecycle

```
win_create()    Create window, get pixel buffer pointer
    |
    v
win_present()   Push current pixel buffer to screen
    |
    v
win_poll()      Receive events (returns 0 if none, <0 if closed)
    |
    v
win_resize()    Handle resize, get new pixel buffer pointer
    |
    v
win_destroy()   Clean up window
```

### Event Types

Events are delivered via `win_poll()` into a `WinEvent` struct:

| `ev.type` | Event | Fields |
|---|---|---|
| 0 | Keyboard | `ev.key.scancode`, `ev.key.ascii`, `ev.key.pressed`, `ev.key.shift`, `ev.key.ctrl`, `ev.key.alt` |
| 1 | Mouse | `ev.mouse.x`, `ev.mouse.y`, `ev.mouse.buttons`, `ev.mouse.prev_buttons`, `ev.mouse.scroll` |
| 2 | Resize | `ev.resize.w`, `ev.resize.h` |
| 3 | Close | (none) |

### Drawing Helpers

Standalone apps typically define local pixel-drawing helpers since they work with raw `uint32_t*` buffers:

```cpp
static void px_fill(uint32_t* px, int bw, int bh,
                    int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h && row < bh; row++)
        for (int col = x; col < x + w && col < bw; col++)
            px[row * bw + col] = color;
}

static void px_hline(uint32_t* px, int bw, int bh,
                     int x, int y, int w, uint32_t color) {
    for (int col = x; col < x + w && col < bw; col++)
        if (y >= 0 && y < bh) px[y * bw + col] = color;
}

static void px_text(uint32_t* px, int bw, int bh,
                    int x, int y, const char* text, Color c, int size) {
    if (g_font)
        g_font->draw_to_buffer(px, bw, bh, x, y, text, c, size);
}
```

### Rounded Rectangles

```cpp
static void px_fill_rounded(uint32_t* px, int bw, int bh,
                            int x, int y, int w, int h, int r, uint32_t color) {
    for (int row = y; row < y + h && row < bh; row++) {
        for (int col = x; col < x + w && col < bw; col++) {
            int dx = 0, dy = 0;
            if (col < x + r && row < y + r)         { dx = x + r - col; dy = y + r - row; }
            else if (col >= x+w-r && row < y + r)    { dx = col - (x+w-r-1); dy = y + r - row; }
            else if (col < x + r && row >= y+h-r)    { dx = x + r - col; dy = row - (y+h-r-1); }
            else if (col >= x+w-r && row >= y+h-r)   { dx = col - (x+w-r-1); dy = row - (y+h-r-1); }
            if (dx * dx + dy * dy <= r * r)
                px[row * bw + col] = color;
            else if (dx == 0 && dy == 0)
                px[row * bw + col] = color;
        }
    }
}
```

---

## Desktop-Integrated Apps

Desktop apps are compiled into the desktop binary itself. They register callback functions that the compositor calls during its event loop.

### Creating a Desktop App

**Step 1: Define your app state**

```cpp
// In apps/app_myapp.cpp
struct MyAppState {
    int counter;
    char label[64];
};
```

**Step 2: Implement callbacks**

```cpp
static void myapp_on_draw(Window* win, Framebuffer& fb) {
    MyAppState* state = (MyAppState*)win->app_data;
    Canvas c(win);

    c.fill(colors::WINDOW_BG);

    // Draw toolbar
    c.fill_rect(0, 0, win->content_w, 36, Color::from_rgb(0xF5, 0xF5, 0xF5));
    c.hline(0, 36, win->content_w, colors::BORDER);

    // Draw content
    c.text(20, 60, state->label, colors::TEXT_COLOR);
}

static void myapp_on_mouse(Window* win, MouseEvent& ev) {
    MyAppState* state = (MyAppState*)win->app_data;
    if (ev.left_pressed()) {
        state->counter++;
        win->dirty = true;  // Request redraw
    }
}

static void myapp_on_key(Window* win, const Montauk::KeyEvent& key) {
    if (!key.pressed) return;
    MyAppState* state = (MyAppState*)win->app_data;
    // Handle keystrokes...
    win->dirty = true;
}

static void myapp_on_close(Window* win) {
    MyAppState* state = (MyAppState*)win->app_data;
    montauk::mfree(state);
}
```

**Step 3: Write the open function**

```cpp
void open_myapp(DesktopState* ds) {
    int idx = desktop_create_window(ds, "My App", 400, 300, 320, 400);
    if (idx < 0) return;
    Window* win = &ds->windows[idx];

    MyAppState* state = (MyAppState*)montauk::malloc(sizeof(MyAppState));
    montauk::memset(state, 0, sizeof(MyAppState));

    win->app_data = state;
    win->on_draw  = myapp_on_draw;
    win->on_mouse = myapp_on_mouse;
    win->on_key   = myapp_on_key;
    win->on_close = myapp_on_close;
    win->dirty    = true;
}
```

**Step 4: Register in the app menu**

Add an entry in `desktop_init()` (`main.cpp`) to the app menu so users can launch it. Include the `open_myapp` function in `apps_common.hpp` or similar.

### Callback Reference

| Callback | Signature | When Called |
|---|---|---|
| `on_draw` | `void(Window*, Framebuffer&)` | Every frame when `win->dirty` is true |
| `on_mouse` | `void(Window*, MouseEvent&)` | Mouse event within window content area |
| `on_key` | `void(Window*, const KeyEvent&)` | Keyboard event while window is focused |
| `on_close` | `void(Window*)` | Window close button clicked |
| `on_poll` | `void(Window*)` | Every frame, for background processing |

### The `dirty` Flag

Desktop apps use a `dirty` flag to control redraws. Set `win->dirty = true` after any state change that requires a visual update. The compositor skips `on_draw` for non-dirty windows.

---

## Drawing and Rendering

### Canvas API (Desktop Apps)

The `Canvas` wraps a window's pixel buffer with drawing primitives:

```cpp
Canvas c(win);  // Construct from Window*

// Fills
c.fill(Color c);                                          // Entire buffer
c.fill_rect(int x, int y, int w, int h, Color c);        // Rectangle
c.fill_rounded_rect(int x, int y, int w, int h, int r, Color c);  // Rounded rect

// Lines
c.hline(int x, int y, int len, Color c);                 // Horizontal
c.vline(int x, int y, int len, Color c);                 // Vertical
c.rect(int x, int y, int w, int h, Color c);             // Outline

// Text
c.text(int x, int y, const char* str, Color c);          // TrueType or bitmap
c.text_2x(int x, int y, const char* str, Color c);       // 2x scaled bitmap
c.text_mono(int x, int y, const char* str, Color c);     // Monospace

// UI elements
c.button(int x, int y, int w, int h, const char* label,
         Color bg, Color fg, int radius);                 // Styled button
c.icon(int x, int y, const SvgIcon& ic);                 // SVG icon

// Layout helpers
c.kv_line(int x, int* y, const char* line, Color c, int line_h);  // Key-value line
c.separator(int x_start, int x_end, int* y, Color c, int spacing); // Horizontal separator
```

### Framebuffer API (Low-Level)

For direct framebuffer access (used by the compositor itself and fullscreen apps):

```cpp
Framebuffer fb;
fb.put_pixel(x, y, color);
fb.put_pixel_alpha(x, y, color);     // With alpha blending
fb.fill_rect(x, y, w, h, color);
fb.fill_rect_alpha(x, y, w, h, color);
fb.blit(x, y, w, h, pixels);         // Copy pixel region
fb.blit_alpha(x, y, w, h, pixels);   // With alpha blending
fb.clear(color);
fb.flip();                            // Swap to hardware
```

### Drawing Primitives

From `gui/draw.hpp`, available for both Framebuffer-based rendering:

```cpp
draw_hline(fb, x, y, w, color);
draw_vline(fb, x, y, h, color);
draw_rect(fb, x, y, w, h, color);
fill_rounded_rect(fb, x, y, w, h, radius, color);
fill_circle(fb, cx, cy, r, color);
draw_circle(fb, cx, cy, r, color);
draw_line(fb, x0, y0, x1, y1, color);     // Bresenham's
draw_shadow(fb, x, y, w, h, offset, color);
```

---

## Text and Fonts

### TrueType Fonts (Preferred)

MontaukOS uses `stb_truetype` for font rendering. Fonts are loaded from the VFS:

```cpp
TrueTypeFont* font = new TrueTypeFont();
font->init("0:/fonts/Roboto-Medium.ttf");

// Render text to a pixel buffer
font->draw_to_buffer(pixels, buf_w, buf_h, x, y, "Hello", color, 18);

// Measure text width before drawing
int width = font->measure_text("Hello", 18);

// Get line height for layout
int line_h = font->get_line_height(18);
```

### System Fonts

The desktop initializes a set of global fonts:

```cpp
fonts::init();  // Call once at startup

// Available fonts
fonts::system_font   // Roboto-Medium.ttf    (UI text)
fonts::system_bold   // Roboto-Bold.ttf      (headings)
fonts::mono          // JetBrainsMono-Regular.ttf (code/terminal)
fonts::mono_bold     // JetBrainsMono-Bold.ttf

// Standard sizes
fonts::UI_SIZE       // 18  (body text)
fonts::TITLE_SIZE    // 18  (window titles)
fonts::LARGE_SIZE    // 28  (headings)
fonts::TERM_SIZE     // 18  (terminal)
```

### Glyph Caching

TrueType fonts cache rasterized glyphs per pixel size. Up to 4 size caches are maintained per font. Access the cache directly for advanced metrics:

```cpp
GlyphCache* cache = font->get_cache(18);
// cache->ascent, cache->descent — for line-height calculation
```

### Bitmap Font (Fallback)

An 8x8 bitmap font is always available for basic text rendering when TrueType is not loaded:

```cpp
draw_char(fb, x, y, 'A', color);
draw_text(fb, x, y, "Hello", color);
int w = text_width("Hello");
```

---

## Input Handling

### Keyboard

```cpp
// In standalone apps (via win_poll)
if (ev.type == 0) {
    Montauk::KeyEvent& key = ev.key;
    if (!key.pressed) { /* key release */ }

    if (key.ascii >= 0x20 && key.ascii < 0x7F) {
        // Printable character
    }

    // Special keys by scancode
    switch (key.scancode) {
        case 0x01: /* Escape */     break;
        case 0x0E: /* Backspace */  break;
        case 0x1C: /* Enter */      break;
        case 0x0F: /* Tab */        break;
        case 0x53: /* Delete */     break;
        case 0x48: /* Up */         break;
        case 0x50: /* Down */       break;
        case 0x4B: /* Left */       break;
        case 0x4D: /* Right */      break;
        case 0x47: /* Home */       break;
        case 0x4F: /* End */        break;
        case 0x49: /* Page Up */    break;
        case 0x51: /* Page Down */  break;
    }

    // Modifiers
    if (key.ctrl) { /* Ctrl held */ }
    if (key.shift) { /* Shift held */ }
    if (key.alt) { /* Alt held */ }
}
```

### Mouse

```cpp
// In standalone apps (via win_poll)
if (ev.type == 1) {
    int mx = ev.mouse.x;
    int my = ev.mouse.y;

    // Button state
    bool left_down  = ev.mouse.buttons & 0x01;
    bool right_down = ev.mouse.buttons & 0x02;

    // Detect clicks (press edge)
    bool left_pressed  = (ev.mouse.buttons & 0x01) && !(ev.mouse.prev_buttons & 0x01);
    bool left_released = !(ev.mouse.buttons & 0x01) && (ev.mouse.prev_buttons & 0x01);

    // Scroll wheel
    int scroll = ev.mouse.scroll;  // Positive = up, negative = down
}
```

In desktop apps, the `MouseEvent` struct provides convenience methods:

```cpp
void myapp_on_mouse(Window* win, MouseEvent& ev) {
    if (ev.left_pressed()) { /* click start */ }
    if (ev.left_released()) { /* click end */ }
    if (ev.left_held()) { /* dragging */ }
    if (ev.right_pressed()) { /* context menu */ }
    if (ev.scroll != 0) { /* scroll */ }
}
```

### Hit Testing

A common pattern for clickable UI regions:

```cpp
struct ButtonRect { int x, y, w, h; };

bool hit_test(ButtonRect& btn, int mx, int my) {
    return mx >= btn.x && mx < btn.x + btn.w &&
           my >= btn.y && my < btn.y + btn.h;
}
```

---

## Widgets

MontaukOS provides built-in widget types in `gui/widgets.hpp`:

### Button

```cpp
Button btn;
btn.init(x, y, width, height, "Click Me");
btn.bg = colors::ACCENT;
btn.fg = colors::WHITE;
btn.on_click = [](void* data) { /* handle click */ };
btn.userdata = my_state;

// In draw callback
btn.draw(fb);

// In mouse callback
btn.handle_mouse(ev);
```

### TextBox

```cpp
TextBox tb;
tb.init(x, y, width, height);

// In draw callback
tb.draw(fb);

// In mouse callback (sets focus)
tb.handle_mouse(ev);

// In key callback (text input)
tb.handle_key(key);

// Read value
const char* value = tb.text;
```

### Scrollbar

```cpp
Scrollbar sb;
sb.init(x, y, width, height);
sb.content_height = 2000;  // Total content height
sb.view_height = 400;      // Visible area height

// In draw callback
sb.draw(fb);

// In mouse callback
sb.handle_mouse(ev);

// Use scroll_offset for content positioning
int offset = sb.scroll_offset;
```

---

## Colors and Theming

### Color Construction

```cpp
Color c1 = Color::from_rgb(0xFF, 0x00, 0x00);     // Red
Color c2 = Color::from_rgba(0x00, 0x00, 0xFF, 0x80); // Semi-transparent blue
Color c3 = Color::from_hex(0x367BF0);               // From hex
uint32_t pixel = c3.to_pixel();                      // ARGB for pixel buffer
```

### System Colors

Defined in `gui/gui.hpp` under the `colors` namespace:

| Constant | Hex | Usage |
|---|---|---|
| `WINDOW_BG` | `#FFFFFF` | Window content background |
| `TEXT_COLOR` | `#333333` | Primary text |
| `ACCENT` | `#367BF0` | Links, selections, active elements |
| `BORDER` | `#D0D0D0` | Window/widget borders |
| `PANEL_BG` | `#2B3E50` | Taskbar/panel background |
| `PANEL_TEXT` | `#FFFFFF` | Panel text |
| `TITLEBAR_BG` | `#F5F5F5` | Window titlebar |
| `DESKTOP_BG` | `#E0E0E0` | Desktop background |
| `CLOSE_BTN` | `#FF5F57` | Close button (red) |
| `MAX_BTN` | `#28CA42` | Maximize button (green) |
| `MIN_BTN` | `#FFBD2E` | Minimize button (yellow) |
| `TERM_BG` | `#2D2D2D` | Terminal background |
| `TERM_FG` | `#CCCCCC` | Terminal text |
| `SCROLLBAR_BG` | | Scrollbar track |
| `SCROLLBAR_FG` | | Scrollbar thumb |

### Toolbar Convention

Standard toolbar pattern used across desktop apps:

```cpp
// 36px tall, light gray background, thin bottom border
c.fill_rect(0, 0, win->content_w, 36, Color::from_rgb(0xF5, 0xF5, 0xF5));
c.hline(0, 36, win->content_w, colors::BORDER);

// 24x24 icon buttons centered at y=6
c.icon(8, 6, my_icon);

// Content starts below toolbar
int content_y = 37;
```

---

## Memory Management

### Userspace Heap

Use `montauk::malloc`, `montauk::mfree`, and `montauk::realloc` for dynamic allocation:

```cpp
#include <montauk/heap.h>

MyState* state = (MyState*)montauk::malloc(sizeof(MyState));
montauk::memset(state, 0, sizeof(MyState));
// ... use state ...
montauk::mfree(state);

// Resize
char* buf = (char*)montauk::malloc(256);
buf = (char*)montauk::realloc(buf, 512);
```

The allocator uses size-class buckets (32 to 4096 bytes) with an overflow list for larger allocations.

### Kernel Page Allocation

`montauk::alloc` / `montauk::free` allocate kernel pages. Avoid for temporary buffers; use the heap instead.

### Important Notes

- **User stack is 32 KiB** (8 pages). Deep call chains (e.g., TrueType rendering) can approach this limit. Avoid large stack allocations.
- Use `inline` (not `static`) for shared globals in headers to avoid per-translation-unit copies and heap corruption.
- The libc needs `-fno-tree-loop-distribute-patterns` in CFLAGS to prevent GCC from converting `memcpy`/`memset` into calls to themselves.

---

## Networking and HTTPS

MontaukOS provides a shared TLS library (`tls/tls.hpp`) backed by BearSSL, and the MontaukAI dev environment adds a higher-level HTTP wrapper (`http/http.hpp`) on top. Build with `USE_TLS=1` to link TLS support.

### HTTP Wrapper (`http/http.hpp`)

Header-only library that handles DNS resolution, request building, TLS, response parsing, and cleanup. All functions return an `http::Response` struct.

#### Setup

```cpp
#include <http/http.hpp>

// Load CA certificates once at startup (required for HTTPS)
tls::TrustAnchors tas = tls::load_trust_anchors();
```

#### GET

```cpp
auto resp = http::get("api.example.com", "/v1/data", tas);
if (resp.status == 200) {
    // resp.body is a pointer to the response body
    // resp.body_len is its length
}
http::free_response(&resp);
```

#### POST

```cpp
const char* json = "{\"name\":\"MontaukOS\",\"version\":1}";
auto resp = http::post("api.example.com", "/v1/submit",
                       "application/json",
                       json, montauk::slen(json),
                       tas);
if (resp.status == 201) {
    // Created successfully
}
http::free_response(&resp);
```

#### Other Methods (PUT, PATCH, DELETE, ...)

```cpp
auto resp = http::request("PUT", "api.example.com", "/v1/item/42",
                          "application/json",
                          body, bodyLen, tas);
http::free_response(&resp);

// DELETE with no body
auto resp2 = http::request("DELETE", "api.example.com", "/v1/item/42",
                           nullptr, nullptr, 0, tas);
http::free_response(&resp2);
```

#### Plain HTTP (No TLS, Port 80)

```cpp
auto resp = http::get_plain("example.com", "/");
if (resp.status == 200) {
    // resp.body ...
}
http::free_response(&resp);
```

#### Reading Response Headers

```cpp
char content_type[128];
if (http::get_header(&resp, "Content-Type", content_type, sizeof(content_type))) {
    // content_type is e.g. "application/json; charset=utf-8"
}
```

#### Custom Headers

Pass extra headers as a string with `\r\n` terminators:

```cpp
auto resp = http::get("api.example.com", "/v1/data", tas,
                      32768,  // response buffer size
                      "Authorization: Bearer tok_abc123\r\n"
                      "Accept: application/json\r\n");
http::free_response(&resp);
```

#### Cancellable Requests

For GUI apps that need to stay responsive during network I/O:

```cpp
static bool g_quit = false;
static bool check_abort() { return g_quit; }

auto resp = http::get("api.example.com", "/v1/slow", tas,
                      32768, nullptr, check_abort);
http::free_response(&resp);
```

Set `g_quit = true` from your keyboard handler (e.g., on Escape) to cancel mid-request.

#### Response Struct Reference

```cpp
struct http::Response {
    int status;           // HTTP status code (200, 404, ...) or -1 on error
    const char* headers;  // Pointer to header block (within raw buffer)
    int headers_len;
    const char* body;     // Pointer to body (within raw buffer)
    int body_len;
    char* raw;            // Owned buffer — freed by free_response()
    int raw_len;
};
```

#### Function Signatures

```cpp
// HTTPS GET
http::Response http::get(const char* host, const char* path,
                         const tls::TrustAnchors& tas,
                         int resp_buf_size = 32768,
                         const char* extra_headers = nullptr,
                         tls::AbortCheckFn abort_check = nullptr);

// HTTPS POST
http::Response http::post(const char* host, const char* path,
                          const char* content_type,
                          const char* body, int body_len,
                          const tls::TrustAnchors& tas,
                          int resp_buf_size = 32768,
                          const char* extra_headers = nullptr,
                          tls::AbortCheckFn abort_check = nullptr);

// HTTPS with any method
http::Response http::request(const char* method,
                             const char* host, const char* path,
                             const char* content_type,
                             const char* body, int body_len,
                             const tls::TrustAnchors& tas,
                             int resp_buf_size = 32768,
                             const char* extra_headers = nullptr,
                             tls::AbortCheckFn abort_check = nullptr);

// Plain HTTP GET (port 80, no TLS)
http::Response http::get_plain(const char* host, const char* path,
                               int resp_buf_size = 32768,
                               const char* extra_headers = nullptr);

// Parse raw HTTP response buffer (used internally, available if needed)
int http::parse_response(char* buf, int len, http::Response* out);

// Case-insensitive header lookup
bool http::get_header(const http::Response* resp, const char* name,
                      char* out_val, int max_len);

// Free the response's raw buffer
void http::free_response(http::Response* resp);
```

### Low-Level TLS API (`tls/tls.hpp`)

If you need more control than `http::` provides (e.g., streaming responses, custom BearSSL setup), use the TLS layer directly:

```cpp
#include <tls/tls.hpp>

tls::TrustAnchors tas = tls::load_trust_anchors();

// Build raw HTTP request yourself
char req[512];
// ... "GET /stream HTTP/1.1\r\nHost: ...\r\n\r\n" ...

char buf[65536];
int n = tls::https_fetch("example.com", ip, 443, req, reqLen, tas, buf, sizeof(buf));
```

See `syscalls.md` for the full `tls::` API reference.

### Plain TCP/UDP

For non-TLS networking without the HTTP wrapper, use the raw socket syscalls directly (see `syscalls.md`):

```cpp
int sock = montauk::socket(Montauk::SOCK_TCP);  // or SOCK_UDP
montauk::connect(sock, ip, port);
montauk::send(sock, data, len);
int n = montauk::recv(sock, buf, maxLen);
montauk::closesocket(sock);
```

---

## App Manifests

External standalone apps can be discovered by the desktop through TOML manifest files placed in `0:/apps/`:

```toml
[app]
name = "My App"
binary = "myapp"
icon = "myapp_icon.svg"

[menu]
category = "Applications"
visible = true
```

**Categories:** `Applications`, `Internet`, `System`, `Games`

The desktop scans `0:/apps/` at startup and adds matching entries to the application menu. The `binary` field is the executable name (looked up in the system path).

---

## Build System

### Independent Development (MontaukAI)

The `MontaukAI/` directory provides a self-contained build environment. Edit the top of `Makefile` to configure your app:

```makefile
APP_NAME := myapp
SRCS     := src/main.cpp src/stb_truetype_impl.cpp src/cxxrt.cpp src/network.cpp
```

Build with optional feature flags:

```bash
make                           # GUI-only app
make USE_TLS=1                 # With HTTPS/TLS (links libtls + libbearssl)
make USE_JPEG=1                # With JPEG decoding (links libjpeg)
make USE_TLS=1 USE_JPEG=1     # Both
make install                   # Copy ELF to MontaukOS ramdisk
```

The sysroot contains all headers and pre-built libraries:

```
sysroot/
├── include/
│   ├── montauk/     syscall.h, heap.h, string.h, config.h, toml.h, user.h
│   ├── gui/         gui.hpp, canvas.hpp, truetype.hpp, widgets.hpp, svg.hpp, ...
│   ├── tls/         tls.hpp (HTTPS/TLS)
│   ├── Api/         Syscall.hpp (low-level syscall numbers)
│   ├── libc/        stdio.h, stdlib.h, string.h, ... (freestanding libc)
│   ├── bearssl*.h   BearSSL headers (for USE_TLS=1)
│   └── (freestanding C/C++ standard headers)
└── lib/
    ├── liblibc.a    C library (always linked)
    ├── libtls.a     TLS helper library
    ├── libbearssl.a BearSSL crypto
    └── libjpeg.a    JPEG decoding (stb_image)
```

Key build details:
- **Toolchain:** `x86_64-elf-g++` cross-compiler (falls back to system g++)
- **Standard:** C++20 (`-std=gnu++20`), freestanding, no exceptions/RTTI
- **SSE:** Enabled (`-msse -msse2`) for floating-point / TrueType rendering
- **Entry point:** `extern "C" void _start()` (not `main`)
- **Load address:** `0x400000` (set in `link.ld`)
- **Runtime support:** `src/cxxrt.cpp` provides `operator new/delete` via `montauk::malloc/mfree`
- **TrueType support:** `src/stb_truetype_impl.cpp` provides the stb_truetype implementation

### In-Tree Development

Standalone apps can also live in `MontaukOS/programs/src/<appname>/` with their own `Makefile`. Each app's Makefile sets `SRCS`, `CXXFLAGS`, `LDFLAGS`, and links against libraries in `programs/lib/`.

Desktop-integrated apps are compiled as part of the desktop binary — add your `.cpp` file to the desktop's source list in `programs/src/desktop/Makefile`.
