# The data disk

d2d reads the game from your own Diablo II files. PeOS gets them on a FAT32
disk, which d2d finds in `/games/diablo2`.

## The files d2d needs

From your Diablo II folder (Diablo II + Lord of Destruction, version 1.14d):

| File | |
| --- | --- |
| `d2data.mpq`, `d2char.mpq`, `d2sfx.mpq`, `d2speech.mpq`, `d2music.mpq`, `d2video.mpq` | Diablo II |
| `d2exp.mpq`, `d2xtalk.mpq`, `d2xmusic.mpq`, `d2xvideo.mpq` | Lord of Destruction |
| `patch_d2.mpq` | the 1.14d patch (or `LODPatch_114d.exe`, Blizzard's patch installer, next to the others) |
| `Game.exe` | optional: lets d2d join a LAN game hosted by the original game |

Some installations do not copy `d2music.mpq`, `d2video.mpq` or
`d2xvideo.mpq` from the CDs: copy them from the discs into the folder first.

## Making the disk image

`tools/make_datadisk.py` needs only Python 3.8 or later. It writes a disk
image with one FAT32 partition (label `D2DATA`) holding those files.

```
python3 tools/make_datadisk.py "/path/to/Diablo II"
python3 tools/make_datadisk.py "/path/to/Diablo II" -o /somewhere/else/d2data.img
```

On Windows, `make-datadisk.bat` does the same with a double click. If a file
is missing it says which and stops; `--force` makes the disk anyway.

## Giving it to PeOS

The image is a raw disk. In PeOS it is the first SATA disk, `ahci0`, and
its partition is `ahci0p1`.

### QEMU

Add these to the command that starts PeOS:

```
-device ich9-ahci,id=ahci -drive id=d2,file=d2data.img,format=raw,if=none -device ide-hd,drive=d2,bus=ahci.0
```

If PeOS starts from its ISO and the virtual machine now tries to start from
the data disk (or from the network) instead, add `-global
ide-cd.bootindex=0`: the CD comes first again.

### VirtualBox

VirtualBox wants its own format; convert the image once:

```
VBoxManage convertfromraw d2data.img d2data.vdi --format VDI
```

Then in the virtual machine's **Settings > Storage**, on the **SATA**
controller, **Add Hard Disk** and pick `d2data.vdi`.

### UTM (macOS)

In the virtual machine's settings, **New Drive > Import**, pick
`d2data.img`, interface **SATA**.

### virt-manager / Proxmox

Add a disk from the existing image, bus **SATA**.

### A USB stick

On a real computer you can skip the image: format a USB stick (4 GB or
more) as **FAT32** and copy the files of the table above to it, at the top
level, not in a folder. In PeOS the stick is `usb` (the first USB disk).
From PeOS 0.16.0 an **exFAT** stick works too: it is what macOS's Disk
Utility and Windows choose for sticks over 32 GB, so a new stick usually
needs no formatting at all. PeOS 0.16.0 also mounts a stick by itself on
`/media1` (`/media2` for a second one), and d2d.app 1.1.0 looks for the game
there: at the top of the stick or in a folder right under it. Nothing to
type: plug it in and open Diablo II.

Writing `d2data.img` to a stick (Balena Etcher, Rufus in DD mode, `dd`)
works too.

## Mounting it in PeOS

In PeOS's terminal:

```
mkdir /games
mkdir /games/diablo2
mount ahci0p1 /games/diablo2 fat32
```

(`usb` for a USB stick). Then open Diablo II from the app manager.
`mount` says `mount: ok` when it worked. From PeOS 0.16.0 a USB stick needs
none of this (it is on `/media1` by itself, see above), and for a disk the
last word can be left out (`mount ahci0p1 /games/diablo2`): PeOS tells FAT32
from exFAT by itself.

## If something goes wrong

- **`make_datadisk.py` says files are missing**: is it the folder with
  `d2data.mpq`? Is Lord of Destruction installed (`d2exp.mpq`)? Is the game
  updated to 1.14d (`patch_d2.mpq`)?
- **`mount: failed`**: is the disk attached to the virtual machine, on a
  SATA controller? A USB stick: is it FAT32 (not NTFS; exFAT only from
  PeOS 0.16.0)? From PeOS 0.16.0 a stick is mounted by itself on `/media1`,
  and mounting it again elsewhere fails: there is no need to.
- **d2d says it cannot find the game data**: did you mount it on exactly
  `/games/diablo2`?
- **Python is not found on Windows**: install it from
  [python.org](https://www.python.org/downloads/) and tick
  *Add python.exe to PATH* in the installer.
