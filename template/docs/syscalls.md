# MontaukOS Syscall Reference

All syscalls are available through `#include <montauk/syscall.h>` in the `montauk` namespace. The syscall layer provides `syscall0()` through `syscall6()` primitives using AMD64 calling conventions. All raw syscalls return `int64_t`.

## Table of Contents

- [Process Management](#process-management)
- [File System](#file-system)
- [Memory](#memory)
- [Console I/O](#console-io)
- [Keyboard and Mouse](#keyboard-and-mouse)
- [Window Server](#window-server)
- [Framebuffer](#framebuffer)
- [Networking](#networking)
- [Audio](#audio)
- [Bluetooth](#bluetooth)
- [Timekeeping](#timekeeping)
- [System Information](#system-information)
- [Storage and Disks](#storage-and-disks)
- [Terminal](#terminal)
- [Process I/O Redirection](#process-io-redirection)
- [Configuration](#configuration)
- [User Management](#user-management)
- [Utility Libraries](#utility-libraries)

---

## Process Management

```cpp
void     exit(int code);                              // Terminate process (noreturn)
void     yield();                                     // Yield CPU to scheduler
void     sleep_ms(uint64_t ms);                       // Sleep for milliseconds
int      getpid();                                    // Get current process ID
int      spawn(const char* path, const char* args);   // Spawn child process (-1 on error)
int      waitpid(int pid);                            // Wait for process to exit
int      kill(int pid);                               // Kill a process
int      proclist(ProcInfo* buf, int max);             // List all processes (returns count)
```

**`ProcInfo` struct:**
```cpp
struct ProcInfo {
    int pid;
    char name[32];
    // ... additional fields
};
```

### Arguments

```cpp
int getargs(char* buf, uint64_t maxLen);  // Get command-line arguments passed to this process
```

---

## File System

```cpp
int      open(const char* path);                                      // Open file (returns fd, negative on error)
int      close(int handle);                                           // Close file handle
int      read(int handle, uint8_t* buf, uint64_t off, uint64_t size); // Read bytes at offset (returns bytes read)
int      fwrite(int handle, const uint8_t* buf, uint64_t off, uint64_t size); // Write bytes at offset
uint64_t getsize(int handle);                                         // Get file size
int      readdir(const char* path, const char** names, int max);      // List directory entries (returns count)
int      fcreate(const char* path);                                   // Create file (returns fd)
int      fdelete(const char* path);                                   // Delete file (0 on success)
int      fmkdir(const char* path);                                    // Create directory
int      drivelist(int* outDrives, int max);                          // Enumerate mounted drives (returns count)
```

### Path Format

Paths use the format `<drive>:/<path>`, e.g. `0:/fonts/Roboto-Medium.ttf`. Drive 0 is the boot drive (ramdisk).

### Example: Reading a File

```cpp
int fd = montauk::open("0:/config/settings.toml");
if (fd < 0) { /* error */ }

uint64_t size = montauk::getsize(fd);
uint8_t* buf = (uint8_t*)montauk::malloc(size + 1);
montauk::read(fd, buf, 0, size);
buf[size] = 0;  // null-terminate
montauk::close(fd);

// ... use buf ...
montauk::mfree(buf);
```

---

## Memory

```cpp
void* alloc(uint64_t size);   // Allocate kernel pages (avoid for temp buffers)
void  free(void* ptr);        // Free kernel pages
```

For general-purpose allocation, prefer the userspace heap (`montauk/heap.h`):

```cpp
void* malloc(uint64_t size);
void  mfree(void* ptr);
void* realloc(void* ptr, uint64_t size);
```

---

## Console I/O

```cpp
void print(const char* text);   // Write string to kernel console
void putchar(char c);           // Write single character
```

These write to the kernel's text-mode console, not to a GUI window.

---

## Keyboard and Mouse

### Keyboard (Direct, Non-Windowed)

```cpp
bool is_key_available();            // Poll for pending keystroke
void getkey(Montauk::KeyEvent* out); // Get next keystroke (blocks)
char getchar();                      // Get single ASCII character (blocks)
```

**`KeyEvent` struct:**
```cpp
struct KeyEvent {
    bool pressed;         // true = key down, false = key up
    uint8_t scancode;     // PS/2 scancode
    char ascii;           // ASCII value (0 if non-printable)
    bool shift, ctrl, alt;
};
```

**Common Scancodes:**

| Scancode | Key |
|---|---|
| `0x01` | Escape |
| `0x0E` | Backspace |
| `0x0F` | Tab |
| `0x1C` | Enter |
| `0x39` | Space |
| `0x53` | Delete |
| `0x48` | Up |
| `0x50` | Down |
| `0x4B` | Left |
| `0x4D` | Right |
| `0x47` | Home |
| `0x4F` | End |
| `0x49` | Page Up |
| `0x51` | Page Down |

### Mouse (Direct, Non-Windowed)

```cpp
void mouse_state(Montauk::MouseState* out);     // Get current mouse state
void set_mouse_bounds(int32_t maxX, int32_t maxY); // Set mouse boundary
```

**`MouseState` struct:**
```cpp
struct MouseState {
    int32_t x, y;
    uint8_t buttons;   // 0x01=Left, 0x02=Right, 0x04=Middle
};
```

For GUI apps, prefer the windowed event system (`win_poll`) over direct mouse/keyboard syscalls.

---

## Window Server

These syscalls are for standalone GUI apps that run as separate processes. The desktop compositor manages window frames; your app renders into a shared pixel buffer.

```cpp
int      win_create(const char* title, int w, int h, WinCreateResult* result);
void     win_destroy(int id);
uint64_t win_present(int id);                          // Mark window dirty, present to screen
int      win_poll(int id, WinEvent* event);            // Poll events (0=none, 1=event, <0=closed)
uint64_t win_resize(int id, int w, int h);             // Resize, returns new pixel buffer address
uint64_t win_map(int id);                              // Get pixel buffer address
int      win_enumerate(WinInfo* info, int max);        // List all windows
int      win_sendevent(int id, const WinEvent* event); // Send event to a window
int      win_setscale(int scale);                      // Set UI scale factor
int      win_getscale();                               // Get UI scale factor
int      win_setcursor(int id, int cursor);            // Set cursor style
```

**`WinCreateResult` struct:**
```cpp
struct WinCreateResult {
    int id;            // Window ID for subsequent calls
    uint64_t pixelVa;  // Virtual address of shared pixel buffer (uint32_t* ARGB)
};
```

**`WinEvent` struct:**
```cpp
struct WinEvent {
    int type;   // 0=Key, 1=Mouse, 2=Resize, 3=Close
    union {
        struct { uint8_t scancode; char ascii; bool pressed, shift, ctrl, alt; } key;
        struct { int x, y; uint8_t buttons, prev_buttons; int32_t scroll; } mouse;
        struct { int w, h; } resize;
    };
};
```

### Event Loop Pattern

```cpp
while (true) {
    Montauk::WinEvent ev;
    int r = montauk::win_poll(win_id, &ev);
    if (r < 0) break;               // Window closed
    if (r == 0) {
        montauk::sleep_ms(16);       // No events, idle at ~60fps
        continue;
    }
    switch (ev.type) {
        case 0: handle_key(ev.key);     break;
        case 1: handle_mouse(ev.mouse); break;
        case 2: handle_resize(ev.resize); break;
        case 3: goto done;              // Close
    }
    render(pixels);
    montauk::win_present(win_id);
}
done:
montauk::win_destroy(win_id);
```

---

## Framebuffer

For direct framebuffer access (fullscreen apps, not typical for windowed apps):

```cpp
void  fb_info(FbInfo* info);   // Get dimensions and pitch
void* fb_map();                // Map framebuffer to user address space
```

**`FbInfo` struct:**
```cpp
struct FbInfo {
    uint32_t width, height;
    uint32_t pitch;    // Bytes per row
};
```

---

## Networking

### DNS and Connectivity

```cpp
int32_t  ping(uint32_t ip, uint32_t timeoutMs);   // Ping, returns RTT in ms (-1 on timeout)
uint32_t resolve(const char* hostname);             // DNS lookup, returns IPv4 (0 on failure)
void     get_netcfg(NetCfg* out);                  // Get network config
int      set_netcfg(const NetCfg* cfg);            // Set network config
```

### Sockets

```cpp
int socket(int type);                              // Create socket (0=TCP, 1=UDP)
int connect(int fd, uint32_t ip, uint16_t port);   // TCP connect
int bind(int fd, uint16_t port);                   // Bind to port
int listen(int fd);                                // Start listening
int accept(int fd);                                // Accept connection (returns new fd)
int send(int fd, const void* data, uint32_t len);  // Send data (returns bytes sent)
int recv(int fd, void* buf, uint32_t maxLen);      // Receive data (returns bytes read)
int closesocket(int fd);                           // Close socket
```

### UDP

```cpp
int sendto(int fd, const void* data, uint32_t len,
           uint32_t destIp, uint16_t destPort);
int recvfrom(int fd, void* buf, uint32_t maxLen,
             uint32_t* srcIp, uint16_t* srcPort);
```

### Example: Plain HTTP GET (Port 80)

```cpp
uint32_t ip = montauk::resolve("example.com");
int sock = montauk::socket(0);  // TCP
montauk::connect(sock, ip, 80);

const char* req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
montauk::send(sock, req, montauk::slen(req));

char buf[4096];
int n = montauk::recv(sock, buf, sizeof(buf) - 1);
buf[n] = 0;

montauk::closesocket(sock);
```

### TLS / HTTPS Library

Header: `tls/tls.hpp` — requires linking `libtls.a` and `libbearssl.a` (build with `USE_TLS=1`).

Provides a high-level `tls::https_fetch()` that handles socket creation, BearSSL TLS 1.2 setup, handshake, request/response exchange, and cleanup in a single call.

```cpp
#include <tls/tls.hpp>

// Load CA certificates (from 0:/etc/ca-certificates.crt)
tls::TrustAnchors tas = tls::load_trust_anchors();

// Build HTTP request
const char* host = "en.wikipedia.org";
uint32_t ip = montauk::resolve(host);

char req[256];
// ... build "GET /path HTTP/1.1\r\nHost: ...\r\n\r\n" into req ...
int reqLen = montauk::slen(req);

// Fetch (handles TLS handshake, send, receive, teardown)
char resp[32768];
int n = tls::https_fetch(host, ip, 443, req, reqLen, tas, resp, sizeof(resp));
if (n > 0) {
    resp[n] = 0;
    // ... parse HTTP response ...
}
```

**API reference:**

```cpp
namespace tls {
    // Load PEM CA bundle from 0:/etc/ca-certificates.crt
    TrustAnchors load_trust_anchors();

    // High-level: DNS → socket → TLS → exchange → cleanup
    int https_fetch(const char* host, uint32_t ip, uint16_t port,
                    const char* request, int reqLen,
                    const TrustAnchors& tas,
                    char* respBuf, int respMax,
                    AbortCheckFn abort_check = nullptr);

    // Lower-level: run TLS exchange on an existing socket + BearSSL engine
    int tls_exchange(int fd, br_ssl_engine_context* eng,
                     const char* request, int reqLen,
                     char* respBuf, int respMax,
                     AbortCheckFn abort_check = nullptr);

    // Helpers for manual BearSSL usage
    int  tls_send_all(int fd, const unsigned char* data, size_t len);
    int  tls_recv_some(int fd, unsigned char* buf, size_t maxlen);
    void get_bearssl_time(uint32_t* days, uint32_t* seconds);
}
```

The optional `AbortCheckFn` callback (e.g., `bool check_quit()`) lets terminal/GUI apps cancel a fetch in progress (return `true` to abort).

### HTTP Wrapper (`http/http.hpp`)

The MontaukAI dev environment includes a higher-level HTTP wrapper built on top of `tls::https_fetch()`. It handles DNS, request building, TLS, and response parsing automatically. See the "Networking and HTTPS" section in `gui-apps.md` for full documentation and examples.

```cpp
#include <http/http.hpp>

tls::TrustAnchors tas = tls::load_trust_anchors();

auto resp = http::get("api.example.com", "/v1/data", tas);
if (resp.status == 200) { /* resp.body, resp.body_len */ }
http::free_response(&resp);

auto resp2 = http::post("api.example.com", "/v1/submit",
                        "application/json", json, jsonLen, tas);
http::free_response(&resp2);
```

---

## Audio

```cpp
int audio_open(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample);
int audio_close(int handle);
int audio_write(int handle, const void* data, uint32_t size);  // Write PCM samples
int audio_ctl(int handle, int cmd, int value);                 // Generic control
```

### Convenience Wrappers

```cpp
int audio_set_volume(int handle, int percent);  // 0-100
int audio_get_volume(int handle);
int audio_pause(int handle);
int audio_resume(int handle);
int audio_get_pos(int handle);                  // Playback position
int audio_get_output(int handle);               // Current output device
int audio_bt_status(int handle);                // Bluetooth audio status
```

---

## Bluetooth

```cpp
int bt_scan(BtScanResult* buf, int maxCount, uint32_t timeoutMs);  // Scan for devices
int bt_connect(const uint8_t* bdAddr);                              // Connect
int bt_disconnect(const uint8_t* bdAddr);                           // Disconnect
int bt_list(BtDevInfo* buf, int maxCount);                          // List connected devices
int bt_info(BtAdapterInfo* buf);                                    // Adapter info
```

---

## Timekeeping

```cpp
uint64_t get_ticks();                      // CPU tick counter
uint64_t get_milliseconds();               // Milliseconds since boot
void     gettime(Montauk::DateTime* out);  // Wall-clock time
```

**`DateTime` struct:**
```cpp
struct DateTime {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    uint8_t weekday;
};
```

---

## System Information

```cpp
void get_info(SysInfo* info);       // CPU, memory, system info
void memstats(MemStats* out);       // Memory usage statistics
int64_t getrandom(void* buf, uint32_t len);  // Cryptographic random bytes
void reset();                        // System reset (noreturn)
void shutdown();                     // System shutdown (noreturn)
```

---

## Storage and Disks

```cpp
int partlist(PartInfo* buf, int max);                                         // List GPT partitions
int disk_read(int blockDev, uint64_t lba, uint32_t sectorCount, void* buf);   // Raw sector read
int disk_write(int blockDev, uint64_t lba, uint32_t sectorCount, const void* buf); // Raw sector write
int gpt_init(int blockDev);                                                   // Initialize GPT
int gpt_add(const GptAddParams* params);                                      // Add partition
int fs_mount(int partIndex, int driveNum);                                    // Mount filesystem
int fs_format(const FsFormatParams* params);                                  // Format filesystem
int diskinfo(DiskInfo* buf, int port);                                        // Get disk info
int devlist(DevInfo* buf, int max);                                           // List block devices
```

---

## Terminal

For terminal-mode (non-GUI) applications:

```cpp
void termsize(int* cols, int* rows);               // Get terminal dimensions
void termscale(int scale_x, int scale_y);           // Set text scaling
void get_termscale(int* scale_x, int* scale_y);     // Get text scaling
```

---

## Process I/O Redirection

For launching child processes with redirected I/O (used by the terminal emulator):

```cpp
int  spawn_redir(const char* path, const char* args);               // Spawn with I/O pipes
int  childio_read(int childPid, char* buf, int maxLen);             // Read child stdout
int  childio_write(int childPid, const char* data, int len);        // Write to child stdin
int  childio_writekey(int childPid, const Montauk::KeyEvent* key);  // Send keystroke to child
int  childio_settermsz(int childPid, int cols, int rows);           // Set child terminal size
```

---

## Configuration

Header: `montauk/config.h`

TOML-based configuration stored in `0:/config/`. Provides per-system and per-user config files.

```cpp
toml::Doc config::load(const char* name);                                      // Load 0:/config/<name>.toml
int       config::save(const char* name, toml::Doc* doc);                      // Save config (0 = success)
toml::Doc config::load_user(const char* username, const char* name);           // Per-user config
int       config::save_user(const char* username, const char* name, toml::Doc* doc);

void config::set_string(toml::Doc* doc, const char* key, const char* val);
void config::set_int(toml::Doc* doc, const char* key, int64_t val);
void config::set_bool(toml::Doc* doc, const char* key, bool val);
bool config::unset(toml::Doc* doc, const char* key);
```

### Example

```cpp
auto doc = montauk::config::load("session");
const char* user = doc.get_string("session.username", "guest");

montauk::config::set_string(&doc, "session.theme", "dark");
montauk::config::save("session", &doc);
```

---

## User Management

Header: `montauk/user.h`

User authentication and session management with SHA-256 password hashing.

```cpp
bool user::authenticate(const char* username, const char* password);
bool user::create_user(const char* username, const char* display_name,
                       const char* password, const char* role);
bool user::delete_user(const char* username);
bool user::change_password(const char* username, const char* new_password);

void user::set_session(const char* username);         // Log in
void user::clear_session();                           // Log out
bool user::get_session_username(char* buf, int sz);   // Get current user
bool user::get_home_dir(char* buf, int sz);           // Get user's home directory
```

---

## Utility Libraries

### String Functions (`montauk/string.h`)

```cpp
int  slen(const char* s);
bool streq(const char* a, const char* b);
bool starts_with(const char* str, const char* prefix);
void memcpy(void* dst, const void* src, uint64_t n);
void memmove(void* dst, const void* src, uint64_t n);
void memset(void* dst, int val, uint64_t n);
void strcpy(char* dst, const char* src);
void strncpy(char* dst, const char* src, int max);
```

### Heap (`montauk/heap.h`)

```cpp
void* malloc(uint64_t size);
void  mfree(void* ptr);
void* realloc(void* ptr, uint64_t size);
```
