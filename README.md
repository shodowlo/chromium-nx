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
- One program, `chromium-nx.nro`, next to Chromium's image
  (`chromium-full.ext4`) in `/switch/chromium-nx` on the SD card.
- Opens on a launcher (`gx/launcher/`): a home screen with a clock, a
  search bar and a row of tiles: Chromium on its own start page, YouTube's
  TV page full screen (`-T`), Google, Chromium's settings, and the
  launcher's settings (the keyboard and mouse keys, updates, diagnostics,
  the disk and priority options, clearing the browsing data; kept in
  `settings.ini`). It writes the browser's command line (host_nx.c's,
  with what the settings add) to `launch.txt` and has the Homebrew loader
  load the same program again as the browser (`--browser`): the browser
  gets the whole process, as if started alone (1936 MB for the guest
  either way).
- Updates itself from the releases on GitHub: see [Updates](#updates).
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
  | + | The overlay: full screen, controls, keyboard, quit |

  The overlay (`gx/overlay.c`) is a panel over the page. Its Controls row
  says what the pad is for the page:

  - **Browser**: the table above.
  - **Keyboard & mouse**: the right stick is the mouse and every other
    input a key, a mouse button or the wheel (by default the left stick
    W A S D, ZR / ZL the buttons, L / R the wheel, A space, B shift, ...).
    The launcher's settings change them (`gx/keys.h`, passed to gx as
    `-K`). The page can lock the mouse (`requestPointerLock()`), as games
    that look around with it do: Minecraft Classic plays this way.
  - **Controller**: pages see a game controller through the Gamepad API,
    for games that support one. The kernel is given a uinput device that
    says it is an Xbox 360 pad, which Chromium finds as on any Linux
    (sysfs, joydev, and the udev records gx writes since there is no
    udev), and maps as a standard controller. Buttons go by position (the
    Switch's B is the Xbox's A). + held opens the overlay; a short press is
    Start.

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

### Updates

Each release on GitHub has the program, Chromium's image, a zip of both for
the SD card, and `manifest.txt`: the version, and each file's size and
SHA-256. When the launcher starts (unless turned off in its settings), it
reads the newest release's manifest (`gx/launcher/update.c`):

- A newer version: it offers to update. Only the files that differ from
  the card's are downloaded; the image (411 MB) is known by its SHA-256,
  worked out once and kept in `chromium-full.ext4.sha256`, so an update of
  the program alone is about 28 MB. Files are downloaded to `.part` (a
  download cut short goes on from there), checked, and only then take the
  old ones' place; the previous program is kept as `chromium-nx.nro.old`.
  The launcher then starts again as the new version. The profile, the
  settings and the downloads are not touched.
- Then the image: missing, or not the one of this version's release, the
  launcher says which one is needed (with the link to the release) and
  offers to download it. Without it, the Chromium tiles show that instead
  of starting a browser that cannot run.

Installing is copying `chromium-nx.nro` to `/switch/chromium-nx/`: the
launcher downloads the image the first time. Or extracting the release's zip
at the root of the card. A release is made with `scripts/release.py` (after
`build-nx.sh`; the version is `gx/version.h`'s), which needs `gh`. To try
the updates without publishing: `release.py --local <folder>` lays a release
out as GitHub does, to serve over HTTP to a build made with
`UPDATE_RELEASES=http://<pc>:<port>/` and `GX_VERSION=<older>`.

### The browser's command line

The launcher writes it to `launch.txt`, which the browser reads and deletes;
without one, the browser runs `programs.txt` if there is one (`DEV=1`'s menu),
else its own (`DEFAULT_COMMAND` in host_nx.c). Before the program, on its line:

| Option | Meaning |
|---|---|
| `-w` / `-b` | A Wayland display / the browser driven through its debugging pipe |
| `@image` / `=image` | The root image / the profile image, next to the NRO |
| `-c<n>` | Processors the guest is told about |
| `-T` | YouTube's TV page (youtube.com/tv): the browser says it is the Switch's own browser (YouTube sends others to the desktop site), starts full screen, and B is the page's Back key (Escape) |
| `-K<key>,...` | The keyboard and mouse mode's keys, in the order of `gx/keys.h` |
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
│    overlay.c  the panel + opens, laid over the page by glwin.c  │
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

`DEV=1 build-nx.sh` writes `programs.txt` with the test programs and
Chromium without the GPU: the program started as the browser with no
`launch.txt` (`chromium-nx.nro --browser`) shows it as a menu. The launcher
also starts the browser at once when it finds a `launch.txt` it did not
write, which is how a test starts a page of its own.

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
