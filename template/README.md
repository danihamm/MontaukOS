# MontaukOS App Template

Self-contained development environment for building standalone MontaukOS GUI apps. Copy this entire directory anywhere and start building.

## Quick Start

```bash
cp -r template/ ~/dev/myapp/
cd ~/dev/myapp/
# Edit APP_NAME and SRCS in Makefile
make
make install   # copy ELF to MontaukOS ramdisk
```

## Build Commands

```bash
make                           # GUI-only app
make USE_TLS=1                 # With HTTPS/TLS (libtls + libbearssl)
make USE_JPEG=1                # With JPEG decoding (libjpeg)
make USE_TLS=1 USE_JPEG=1     # Both
make clean                     # Remove build artifacts
make install                   # Copy ELF to MontaukOS ramdisk
```

If the MontaukOS tree is not at `~/dev/MontaukOS/`, pass the path to the cross-compiler:

```bash
make MONTAUKOS=/path/to/MontaukOS
```

## Structure

```
myapp/
├── Makefile                     Edit APP_NAME and SRCS here
├── link.ld                      Linker script (load at 0x400000)
├── src/
│   ├── main.cpp                 Your app (template with window, rendering, input)
│   ├── stb_truetype_impl.cpp    TrueType font support (keep this)
│   └── cxxrt.cpp                C++ new/delete runtime (keep this)
├── sysroot/
│   ├── include/
│   │   ├── montauk/             syscall.h, heap.h, string.h, config.h, toml.h, user.h
│   │   ├── gui/                 gui.hpp, canvas.hpp, truetype.hpp, widgets.hpp, svg.hpp, ...
│   │   ├── http/                http.hpp (HTTP GET/POST/etc. wrapper)
│   │   ├── tls/                 tls.hpp (HTTPS/TLS, for USE_TLS=1)
│   │   ├── Api/                 Syscall.hpp (low-level syscall numbers)
│   │   ├── libc/                stdio.h, stdlib.h, string.h, ...
│   │   ├── bearssl*.h           BearSSL headers (for USE_TLS=1)
│   │   └── (freestanding C/C++ standard headers)
│   └── lib/
│       ├── liblibc.a            C library (always linked)
│       ├── libtls.a             TLS helper library
│       ├── libbearssl.a         BearSSL crypto
│       └── libjpeg.a            JPEG decoding (stb_image)
└── docs/
    ├── gui-apps.md              GUI programming guide
    └── syscalls.md              Syscall and API reference
```

## Key Points

- Entry point: `extern "C" void _start()` (not `main`)
- Memory: `montauk::malloc()` / `montauk::mfree()` (no standard libc)
- Window: `win_create()` / `win_poll()` / `win_present()` / `win_destroy()`
- Fonts: `TrueTypeFont::init("0:/fonts/Roboto-Medium.ttf")`
- HTTP: `#include <http/http.hpp>` with `USE_TLS=1` (see docs/)
- No exceptions, no RTTI, 32 KiB user stack
