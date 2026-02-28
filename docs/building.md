# Building MontaukOS

## Build Dependencies

### Toolchain

| Dependency | Purpose |
|---|---|
| `gcc` / `g++` | C/C++ compiler (C++20, GNU11) |
| `nasm` | x86_64 assembly |
| `make` | Build system |
| `git` | Fetching dependencies (Limine, freestanding headers, cc-runtime) |
| `curl` or `wget` | Downloading `limine.h` header |

### ISO/Disk Image Creation

| Dependency | Purpose |
|---|---|
| `xorriso` | Creating bootable ISO images |
| `mtools` | Creating FAT-formatted HDD images (`mformat`, `mcopy`, `mmd`) |
| `gdisk` | GPT partitioning for HDD images (`sgdisk`) |

### Running in QEMU

| Dependency | Purpose |
|---|---|
| `qemu-system-x86-64` | x86_64 emulation |
| `ovmf` | UEFI firmware for QEMU |

For other architectures, install the corresponding QEMU system emulator (`qemu-system-aarch64`, `qemu-system-riscv64`, etc.).

## Ubuntu / Debian

Install all build dependencies with:

```bash
sudo apt install build-essential nasm git curl xorriso mtools gdisk qemu-system-x86 ovmf
```

`build-essential` provides `gcc`, `g++`, and `make`.

## Building

1. Clone the repository:
   ```bash
   git clone https://github.com/danihamm/MontaukOS.git
   cd MontaukOS
   ```

2. Build the ISO:
   ```bash
   make
   ```
   This will automatically fetch dependencies (Limine bootloader, freestanding headers, cc-runtime) and produce an ISO image.

3. Run in QEMU:
   ```bash
   make run
   ```

### Other Targets

| Target | Description |
|---|---|
| `make all-hdd` | Build a raw HDD image instead of ISO |
| `make run-hdd` | Run the HDD image in QEMU |
| `make run-bios` | Run in QEMU with legacy BIOS (x86_64 only) |
| `make clean` | Remove build artifacts |
| `make distclean` | Remove everything including downloaded dependencies |

### Cross-Architecture Builds

To build for a different architecture, pass `ARCH=`:

```bash
make ARCH=aarch64
make ARCH=riscv64
make ARCH=loongarch64
```
