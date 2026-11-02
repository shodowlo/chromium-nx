# Chromium NX

**The Chromium web browser as a homebrew app Nintendo Switch.**

The Linux kernel runs as a library (LKL), and on it runs Debian's
stock arm64 Chromium, unmodified, drawing with the console's GPU.
This is experimental software.

## Install

Copy `chromium-nx.nro` to `/switch/chromium-nx/` on the SD card.
Then create a forwarder using Sphaira (press the X button -> Install Forwarder -> set the address space to 39-bit -> Create Forwarder)

## Features

- Chromium (Chromium 156): tabs, address bar, a profile that
  lasts, JavaScript, HTTPS, sound, hardware H.264 video.
- A launcher: search bar, Chromium, YouTube for TVs, **My sites** (your own
  pages, each with full screen, controls and portrait), 
  GitHub, Downloads, Help, Chromium's settings, and Settings
  (keyboard and mouse keys, updates, 1080p on a TV, export / import the profile, reset).
- 1080p in the dock, 720p handheld. Portrait mode. Dark theme when the console's is.
- Touch with pinch to zoom, the on-screen keyboard, a file picker for
  uploads (the SD card), downloads to `/switch/chromium-nx/Downloads`.
- Updates itself from the GitHub releases.



## Status

Tested on a real console: Google, Wikipedia, GitHub and YouTube work, videos
play. About 60 frames a second on simple pages; YouTube's home page is up
about 30 s after start at the stock clock. It is faster with the CPU
overclocked. Chromium gets about 1.9 GB of memory.

Files in `/switch/chromium-nx/`: `profile.ext4` (cookies, history, logins),
`settings.ini`, `sites.ini`, `Downloads/`, `Backup/` (export), and
`chromium-nx.log`, the last run's log.

## How it works

```
chromium-nx.nro
├─ host_nx.c     Horizon side: memory, threads, screen, pad, sound, SD card, network
├─ LKL           Linux as a library: files (ext4 image), sockets, pipes, epoll
├─ gx            runs Linux programs on it, natively (arm64 on arm64)
│  ├─ gelf.c     loads ELF programs        gmem.c  the guest's memory
│  ├─ gsys.c     system calls              tramp.S every svc patched into a jump
│  ├─ wl.c       a Wayland compositor      glwin.c lays windows on the GPU screen
│  ├─ overlay.c  the + menu                cdp.c   Chromium's debugging pipe
│  └─ nat.c      NAT from LKL's network to Horizon's sockets
├─ launcher/     the home screen and updates (the same NRO, before the browser)
└─ guest         Debian's chromium and its libraries; libEGL / libGLESv2 /
                 libasound replaced by small ones that call the host's drivers
```

## Build

Two Docker containers share the project: one with Debian 12 and
`chromium:arm64` (Chromium's image, the PC test bench), one with devkitPro's
devkitA64 (the console). Each script says at its top how its container is
made.

```bash
bash gx/build-lkl.sh        # LKL for the PC bench
bash gx/build-pc.sh         # gx for PC, guest libraries, tests (NO_TESTS=1 to skip)
bash gx/build-chromium.sh   # Chromium's image
bash gx/build-nx.sh         # the NRO -> gx/dist-nx
```

The version is in `gx/version.h` (major.minor.patch); `scripts/release.py`
publishes a release (`--local <folder>` to try updates without publishing).
The browser's options (`-F` full screen, `-R` portrait, `-M` controls, ...)
are listed in `main()` of `gx/host_nx.c`, where they are read.

## Credits

- **mnnobre** ([mnnobre/lkl-switch](https://github.com/mnnobre/lkl-switch)),
  the original project: Linux as a library inside an NRO.
- [LKL](https://github.com/lkl/linux), [libnx](https://github.com/switchbrew/libnx)
  and devkitPro, [nx-hbloader](https://github.com/switchbrew/nx-hbloader),
  Mesa, and [Chromium](https://www.chromium.org/).

The Linux kernel (LKL) is GPL-2.0; the original lkl-switch code is MIT
according to its author. Chromium and its logo are the Chromium Authors'.
Chromium NX is not affiliated with Google or Nintendo.
