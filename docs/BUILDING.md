# Building d2d.app

`build.sh` rebuilds d2d.app from source with one command. It works on
**Linux**, on **Windows through WSL**, and on **macOS**.

```
./build.sh check     # are the tools it needs installed?
./build.sh           # build everything: out/d2d.app
```

The first build takes about **25 minutes** on a recent machine and needs
about **6 GB** of disk: most of it is the cross compiler (binutils and GCC
for PeOS), built once. If the build stops, run `./build.sh` again: it goes
on where it was. `./build.sh clean` starts over (the downloads are kept).

## What you need

| | |
| --- | --- |
| Debian, Ubuntu, WSL | `sudo apt install build-essential curl git xz-utils cmake python3 m4 gpgv libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev` |
| Fedora | `sudo dnf install gcc gcc-c++ make curl git xz cmake python3 m4 gnupg2 gmp-devel mpfr-devel libmpc-devel zlib-devel` |
| macOS | [Homebrew](https://brew.sh), then `brew install cmake xz gmp mpfr libmpc gnupg` (and Xcode's command line tools: `xcode-select --install`) |

`./build.sh check` says what is missing and how to install it.

### Windows

Install WSL (in PowerShell as administrator: `wsl --install`, then restart),
open **Ubuntu**, install the packages above, and run `./build.sh` in this
folder. With the repository on a Windows drive (`/mnt/c/...`) the build
itself happens on WSL's Linux disk (`~/.cache/peos-d2d`): much faster.
The app still ends up in `out/d2d.app` here.

## What it does

1. **The cross compiler**: binutils 2.47 and GCC 16.2.0 (C and C++, with
   libstdc++) for the target `x86_64-peos`. The GNU releases are checked
   against their signatures; `toolchain/patch.py` and `toolchain/gcc-peos.h`
   add the target. libstdc++ is built over PeOS's C library, from the Starlight App Kit
   ([The PeOS SDK](#the-peos-sdk)).
2. **The game's libraries**, each at a fixed version, checked by SHA-256:
   zlib 1.3.1, bzip2 1.0.8, FFmpeg 9.0.2 (only what Bink videos need), SDL
   3.2.26's headers.
3. **The platform**: SDL3 (`platform/sdl3`: the window, the keyboard and
   mouse, over Starlight) and OpenAL (`platform/openal`: the sound, over
   PeOS's audio), written for PeOS.
4. **The game**: d2decomped and StormLib at fixed commits, with the patches
   in `port/patches`, built with CMake (`port/toolchain-peos.cmake`,
   `port/cmake/Platform/PeOS.cmake`).
5. **The app**: `out/d2d.app` with `app.bin` (the game), `app.ini` (what it
   is and what it may do on PeOS) and its texts and icon.

At the end it prints the SHA-256 of `app.bin`.

Where the work goes: `build/` in this folder (or `~/.cache/peos-d2d` under
WSL; `PEOS_D2D_WORK=/somewhere ./build.sh` chooses another place). `JOBS=4
./build.sh` limits the parallel jobs (the default is one per CPU).

## The PeOS SDK

What the build needs from PeOS comes from the
[Starlight App Kit](https://github.com/teddi201252/starlight-app-kit), the
kit for every PeOS app, fetched at the commit in `build.sh` (`KIT_COMMIT`):
its `sdk/` has the headers and the library of PeOS's base (`libpeos`), and
`sdk/compat/` what ports of C and C++ programs need (`libpeos-compat`, its
headers, the programs' entry `crt0.S`). Following a new PeOS release means
moving `KIT_COMMIT` to the kit's release for it.

`PEOS_APP_KIT=/path/to/starlight-app-kit ./build.sh` builds with a local
copy of the kit instead (then `./build.sh clean` first, so that the
compiler is built again over it).

## Trying the app

To try a d2d.app you built, copy the `d2d.app` folder into PeOS's
`/system/apps/` (or install it as PeStore does), attach a data disk
([DATA-DISK.md](DATA-DISK.md)) and open Diablo II from the app manager.

## The patches

`port/patches/stormlib/peos.patch`: a PeOS platform for StormLib: its file
layer on PeOS's own calls (`peos_fopen`, `peos_fread`, `peos_fwrite`,
`peos_fseek`, `peos_stat`, `peos_rename`).

`port/patches/d2d/peos.patch`:
- StormLib with `-fno-strict-aliasing` (its Huffman decoder breaks the rule;
  GCC 16 at `-O3` then decodes sounds wrong);
- `components/devctl`: the warn-once stub of Windows (no Unix sockets);
- `components/net`: on PeOS, TCP over PeOS's network (joining a LAN game);
- `components/userdir`: settings and saves in `/starlight/config/d2d`;
- `apps/`: on PeOS only d2d (no Qt launcher, d2proxy, packaging); no Sentry
  crash reports;
- the tests: PeOS's environment, no tests of sockets.
