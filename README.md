# Chromium NX

**A Chromium web browser running as a homebrew app inside Horizon OS, on a
hacked Nintendo Switch.**

Chromium NX is an ordinary NRO started from
the Homebrew Menu. Inside it, the Linux kernel runs as a library (LKL), and on
top of that kernel runs the stock arm64 Chromium binary from Debian,
unmodified, drawing with the console's GPU.

This is experimental software.

## What it does

- Runs Debian's `chromium` 156, the whole browser (tabs, address bar, a
  profile that lasts), as shipped, without recompiling it.
- Opens on a launcher (`chromium-nx.nro`, from `gx/launcher/`): a home
  screen with a clock, a search bar and a row of sites (Chromium itself
  opens on its own start page), and the settings (diagnostics, the disk and
  priority options, clearing the browsing data; kept in `settings.ini`). It writes the browser's command
  line to `launch.txt` and has the Homebrew loader load the browser,
  `gx.nro`, once it has ended: the browser gets the whole process, as if
  started alone (1936 MB for the guest either way). `gx.nro` started by
  itself still reads `programs.txt`; holding L shows its menu.
- Renders with the GPU: ANGLE inside Chromium talks to the console's own
  EGL/GLES driver
- Runs JavaScript
- Touch screen, on-screen keyboard, and the pad:

  | Input | Action |
  |---|---|
  | Touch | Tap, drag, scroll; touching a text field opens the keyboard |
  | D-pad | Moves the focus from link to link |
  | A | Follows the focused link, or opens the keyboard on a text field |
  | B / X | Back / reload |
  | Y | Opens the keyboard on the address bar |
  | L / R | Backspace / Enter |
  | Sticks | Scroll |
  | + | Quit |

- HTTPS networking through the console's Wi-Fi.
- Sound, through the console's audio output.

## Status

Confirmed on a real console:

- Google, Wikipedia and YouTube load and are usable; YouTube videos play
  (H.264 decoded by the console's hardware decoder).
- GPU rendering and the JIT: about 60 pictures a second on simple
  animations, 55 when scrolling a long page, 25 animating 400 elements (at
  the stock 1 GHz; 38 to 41 at 1.58 GHz).
- At the stock clock, YouTube's home page is up about 32 s after the app
  starts (Chromium itself takes most of it), and a video plays at 30
  pictures a second once the page has finished loading around it; in the
  half minute before, its scripts take the processors and the video
  stutters.
- Touch, scrolling, text entry with the system keyboard, d-pad focus.
- Memory: the guest (Chromium) has about 1.9 GB, the console's own side
  (GPU driver, video decoder, thread stacks) 448 MB, Linux 192 MB. A heavy
  page such as Minecraft Classic takes most of it; a page that is left is
  freed (no back/forward cache). When something dies anyway, the system's
  error dialog says so and the app closes, rather than the last picture
  staying on screen.

The browser's profile (cookies, history, logins, cache) is kept in
`/switch/chromium-nx/profile.ext4`, made the first time from an empty image
inside the program. What it downloads goes to `/switch/chromium-nx/Downloads`
on the SD card, as plain files: the kernel writes them through hostfs
(User-Mode Linux's filesystem of the host's files, built into LKL:
`gx/lkl/hostfs/`) and `gx/hostfs_nx.c`, which makes up on the card what it is
not (symbolic links, removing an open file, renaming over one). The profile
was a folder of the card for a while; it is an image again because every
access to a folder holds the kernel for as long as the card takes, and the
kernel's memory ran out.

The log of the last run is `/switch/chromium-nx/chromium-nx.log`.

### Options in programs.txt

Before the program, on its line:

| Option | Meaning |
|---|---|
| `-w` / `-b` | A Wayland display / the browser driven through its debugging pipe |
| `@image` / `=image` | The root image / the profile image, next to the NRO |
| `-c<n>` | Processors the guest is told about |
| `-A<l>,<t>,<r>,<b>` | Where the address bar is |
| `-p` | Diagnostics in the log: where the processor goes (a sampling profiler), what a syscall costs, failed requests and media player errors |
| `-xb`, `-xh` | Experiments: no kernel priority for syscalls, a tickless kernel |
| `-xp`, `-xd` | Experiments: Chromium's thread priorities ignored, the disk read under the kernel's processor (as LKL does) |
| `-t` | Traces every syscall |

## How it works

```
┌─ chromium-nx.nro ──────────────────────────────────────────────┐
│  host_nx.c   the Horizon side: memory, threads, screen, pad,    │
│              sound, the SD card as a disk, the network          │
│  ┌─ LKL: Linux as a library (no MMU) ────────────────────────┐  │
│  │  files (ext4 image, tmpfs), sockets, pipes, epoll, ...    │  │
│  └───────────────────────────────────────────────────────────┘  │
│  gx: runs Linux programs on that kernel                         │
│    gelf.c   loads dynamically linked ELF programs               │
│    gmem.c   the guest's address space (mmap, mprotect, ...)     │
│    gsys.c   system calls: most go to LKL, memory and threads    │
│             are done here                                       │
│    tramp.S  every `svc` of the guest is patched into a jump     │
│             here                                                │
│    wl.c     a Wayland compositor: Chromium's window and inputs  │
│    cdp.c    Chromium's debugging pipe, for what a window does   │
│             not say (is the focus on a text field?)             │
│    nat.c    a user-mode NAT between LKL's network and Horizon's │
│             sockets                                             │
│  ┌─ guest ───────────────────────────────────────────────────┐  │
│  │  chromium-shell (Debian arm64), glibc and its libraries   │  │
│  │  libEGL / libGLESv2 / libasound: small replacements that  │  │
│  │  call the host's drivers                                  │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

The guest runs natively: it is arm64 code on an arm64 processor, with no
emulation. What the kernel of a real Linux would do for it is split between
LKL (files, network) and gx (memory, threads, time).

A few things Horizon does differently had to be worked around:

- **No memory that is writable and executable.** A page of V8's code area is
  one or the other; the fault of a write to an executable page, or of a jump
  to a writable one, flips it.
- **Shared memory is scarce** (80 objects for the whole console) and must be
  mapped whole, so shared files are pooled into a few large pieces.
- **The kernel keeps a record per run of pages mapped alike**, and stops
  serving a process past about ten thousand of them, so guest memory is
  committed in large units.
- **Reading the virtual counter traps**; it is emulated in the exception
  handler.
- **Memory can be missing for a moment.** When code memory next to it is
  mapped again, a thread may fault on a page that is there an instant later;
  the exception handler waits and lets it try again.
- **newlib's malloc refuses 2 GB or more**, and says yes to sizes it does
  not have: the guest's memory is sized by counting what the heap has
  left, not by asking for it. When it runs out, the units the guest made
  inaccessible and left all zero are given back first.
- **The kernel's one processor is handed between threads** through
  semaphores: syscalls of LKL run at the priority of the kernel's threads,
  and LKL's tick is not stopped when it is idle (`nohz=off`), which made
  every hand-over wake its timer thread. The disk does not hold it: LKL's
  virtio block device is replaced by one that reads the SD card on threads
  of its own and raises the interrupt when done (`gx/lkl/`), where LKL's
  held every other thread's syscalls for the length of each read.
- **Only one priority is time-sliced** (0x3B, the guests'). What Chromium
  asks of its threads' priorities (display, audio) puts them one or two
  levels above it; the renderer's main thread, which runs the pages'
  scripts, stays where it can be time-sliced.
- **A guest thread is known by its TLS pointer**, which a new thread may
  inherit from one that just ended (glibc reuses stacks): an ending thread
  leaves the table before its joiner is told.

## Build

Everything builds in two Docker containers that share a volume: `lkl-gx`
(Debian 12 with arm64 packages, for the PC test bench and Chromium's root
image) and `lkl-gx-nx` (devkitPro's devkitA64, for the console). Each script
says at its top how its container is made. `gx/docker/Dockerfile` describes the
first one but has not been rebuilt from scratch recently.

```bash
docker exec lkl-gx    bash /project/gx/build-lkl.sh        # LKL for the PC bench
docker exec lkl-gx    bash /project/gx/build-pc.sh         # gx for PC, guest libraries, tests
docker exec lkl-gx    bash /project/gx/build-chromium.sh   # Chromium's root images
docker exec lkl-gx-nx bash /project/gx/build-nx.sh         # the NRO -> gx/dist-nx
docker exec lkl-gx-nx bash /project/gx/build-hbl.sh        # the loader -> gx/dist-hbl
```

The root image wants Debian's arm64 `chromium` installed in `lkl-gx`
(`apt-get install chromium:arm64`). With `NO_TESTS=1`, `build-pc.sh` and
`build-chromium.sh` only make what the console needs and run nothing under
qemu. LKL itself is the unmodified `lkl/linux` tree.

`DEV=1 build-nx.sh` adds the test programs and Chromium without the GPU to
`programs.txt`: the app then shows a menu of what to run instead of starting
the browser.

## Testing on a PC

The same code builds for an arm64 Linux host and runs under `qemu-aarch64`,
with the same restrictions the console imposes (no writable and executable
memory, pooled shared memory). `build-pc.sh` runs the small tests;
`build-chromium.sh wayland [url]` runs Chromium with a scripted user and
writes the pictures it draws to files. Most of the work was done this way
before touching hardware.

## Layout

- `gx/` — everything described above.
- `nro/`, `scripts/`, `pc-test/`, `DEVLOG.md` — the original lkl-switch
  project this one grew from: its NRO that boots LKL and runs busybox. gx
  reuses its `nro/source/nx_host.c` (LKL's host operations on libnx).

## Credits

- **mnnobre** ([mnnobre/lkl-switch](https://github.com/mnnobre/lkl-switch))
  — the original project: Linux as a library inside an NRO, and the first
  executor of Linux binaries on it.
- [LKL](https://github.com/lkl/linux), [libnx](https://github.com/switchbrew/libnx)
  and devkitPro, [nx-hbloader](https://github.com/switchbrew/nx-hbloader),
  Mesa, and of course [Chromium](https://www.chromium.org/).

The Linux kernel (LKL) is GPL-2.0. The original lkl-switch code is MIT
according to its author. Chromium and its logo are the Chromium Authors'.
Chromium NX is not affiliated with Google or Nintendo.
