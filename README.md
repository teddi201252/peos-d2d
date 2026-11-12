# Diablo II on PeOS (d2d)

This is the port of [d2decomped](https://github.com/psi29a/d2decomped) to
[PeOS](https://github.com/teddi201252): the code that turns d2decomped, a
reimplementation of Diablo II + Lord of Destruction, into **d2d.app**, the
Diablo II app you install from **PeStore**.

d2d plays the game with **your own** copy of Diablo II. Nothing of
Blizzard's comes with it: you give it your game files on a small disk, and
this repository has the tool that makes that disk for you.

There are two things you can do here:

| I want to... | Go to |
| --- | --- |
| **Play**: make the disk with my game files | [Play Diablo II on PeOS](#play-diablo-ii-on-peos) |
| **Rebuild** d2d.app from source | [docs/BUILDING.md](docs/BUILDING.md) |

## Play Diablo II on PeOS

You need:
- PeOS with **d2d** installed from PeStore;
- **Diablo II** with the **Lord of Destruction** expansion, installed on
  your computer and updated to **version 1.14d** (the last classic patch).
  *Diablo II: Resurrected* does not work: its files are different;
- **Python 3** (free: [python.org](https://www.python.org/downloads/); on
  macOS and Linux it is usually there already).

### 1. Make the data disk

[Download this repository](../../archive/refs/heads/main.zip) and unzip it,
or clone it. Then:

- **Windows**: double-click `make-datadisk.bat`, drag your Diablo II folder
  into the window, press Enter.
- **macOS and Linux**: in a terminal, in this folder:
  ```
  python3 tools/make_datadisk.py "/path/to/Diablo II"
  ```

It checks that every file d2d needs is there, then writes `d2data.img`
(about 2.5 GB). Your installation is only read, never changed.

Don't know where Diablo II is? Usually `C:\Program Files (x86)\Diablo II` on
Windows, or `/Applications/Diablo II` on macOS. The right folder is the one
with `d2data.mpq` in it.

### 2. Give the disk to PeOS

- **PeOS in a virtual machine** (QEMU, VirtualBox, UTM): add `d2data.img`
  as a second disk. [docs/DATA-DISK.md](docs/DATA-DISK.md) shows how for
  each one.
- **PeOS on a real computer**: you do not even need the image. Format a USB
  stick as **FAT32** (from PeOS 0.16.0 also **exFAT**, and then Diablo II
  finds the files on it by itself) and copy the game files onto it (the list is in
  [docs/DATA-DISK.md](docs/DATA-DISK.md#a-usb-stick)).

### 3. Play

In PeOS, open the terminal (the `>_` icon in the dock) and type:

```
mkdir /games
mkdir /games/diablo2
mount ahci0p1 /games/diablo2 fat32
```

(on a USB stick the last line is `mount usb /games/diablo2 fat32`).

**From PeOS 0.16.0 (d2d.app 1.1.0) no commands are needed for a stick**:
PeOS mounts a USB stick by itself on `/media1` (FAT32 or **exFAT**, what
macOS and Windows choose for large sticks), and Diablo II looks for the game
files there, at the top of the stick or in a folder of it. Plug it in, open
Diablo II.
Then open **Diablo II** from the app manager. Your characters and settings
are saved in PeOS (`/starlight/config/d2d`), not on the data disk.

Something wrong? See [docs/DATA-DISK.md](docs/DATA-DISK.md#if-something-goes-wrong).

## What is in here

| Folder | What |
| --- | --- |
| `tools/make_datadisk.py` | makes the data disk (Windows, macOS, Linux; Python only) |
| `build.sh` | rebuilds d2d.app, everything included ([docs/BUILDING.md](docs/BUILDING.md)) |
| `port/` | PeOS's patches to d2decomped and StormLib, CMake for PeOS, the app's bundle |
| `platform/` | SDL3 and OpenAL written over PeOS, which the game uses for its window, input and sound |
| `toolchain/` | what teaches binutils and GCC the target `x86_64-peos` |

The PeOS SDK the game is built against (PeOS's C library and its headers)
comes from the [Starlight App Kit](https://github.com/teddi201252/starlight-app-kit),
the kit for every PeOS app: `build.sh` fetches it at a fixed commit.

## License

d2d.app is d2decomped, which is free software under the **GNU General Public
License, version 3 or later**; so is this port: see [LICENSE](LICENSE). This
repository is the source of the d2d.app on PeStore, as the GPL requires:
`build.sh` rebuilds it from these files and the fixed versions it downloads.
The other projects it uses and their licenses are in [NOTICE.md](NOTICE.md).

Diablo II is a trademark of Blizzard Entertainment. This project is not
affiliated with or endorsed by Blizzard, and contains none of its files.
