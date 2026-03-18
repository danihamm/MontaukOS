# The Montauk Operating System
MontaukOS is a hobbyist operating system written in modern C++. It runs on bare metal and supports various applications, including DOOM, a Wikipedia client, and standard desktop utilities.

![MontaukOS screenshot](images/MontaukOS-2.png)

## Features
* Modern preemptive multitasking kernel
* Multi-user userspace with desktop environment and command line
* PCI-e support and drivers for Intel GPU and e100e Ethernet for graphics and networking on real hardware
* ACPI support with AML interpreter, including S3 sleep and ACPI shutdown
* Support for USB including input devices (keyboard/mouse), along with PS/2 input support
* Support for Intel High Definition (HDA) audio devices
* Userspace and kernel audio support
* Support for (some) Intel Bluetooth devices, userspace Bluetooth management app
* VFS using numbered drive identifiers with ramdisk support
* Support for UEFI Runtime Services, including power management calls (shutdown/reboot)
* Customizable desktop environment with 12+ graphical apps, including a terminal emulator, file manager, Wikipedia client, weather app, DOOM, and more
* Modern icon pack (Flat Remix) used in desktop environment
* Support for TrueType font, JPEG image, and SVG icon rendering
* Networking including TCP/IP stack, UDP, DNS, DHCP and TLS via BearSSL
* Command-line IRC client
* Live viewable kernel log from GUI
* Mandelbrot set renderer
* PDF viewer app with full support for text (inc. baked-in TrueType fonts) and some graphics
* Userspace Music app with support for MP3 and WAV files
* 2D game engine and demo game with fantasy tileset

## Ideology
The goal of the MontaukOS project is to create a modern and unique operating system that runs on both emulators and on real hardware. The kernel and included userspace applications are written in modern C++.

## History and methodology
Development started in early 2025, with the first published GitHub commit being on Feb 27, 2025. In early 2026, Claude Opus 4.6 began being used to accelerate development of the kernel and userspace.