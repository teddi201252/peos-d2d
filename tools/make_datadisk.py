#!/usr/bin/env python3
"""Make the Diablo II data disk for d2d on PeOS.

Takes the folder of your own Diablo II + Lord of Destruction installation
(version 1.14d) and writes a disk image with one FAT32 partition holding
the game files d2d needs. Attach the image to the virtual machine that
runs PeOS (QEMU, VirtualBox, UTM...), or write it to a USB stick.

Works on Windows, macOS and Linux with nothing but Python 3.8 or later:
no administrator rights, no extra programs.

    python3 make_datadisk.py "C:\\Program Files (x86)\\Diablo II"
    python3 make_datadisk.py ~/Games/Diablo2 -o d2data.img

Nothing is sent anywhere and nothing in your installation is changed: the
files are only read and copied into the image.
"""
import argparse
import datetime
import math
import os
import pathlib
import struct
import sys
import time

SECTOR = 512
PART_START = 2048            # the partition starts at 1 MiB, as every modern tool does
RESERVED = 32                # reserved sectors of the FAT32 volume
NUM_FATS = 2
MIN_CLUSTERS = 65525         # fewer, and the volume is FAT16 by definition
SPARE = 32 * 1024 * 1024     # free space left on the disk (32 MiB)
LABEL = b"D2DATA     "

# What d2d reads (components/install/install.cpp of d2decomped).
REQUIRED = ["d2data.mpq", "d2exp.mpq", "d2char.mpq", "d2sfx.mpq", "d2speech.mpq",
            "d2music.mpq", "d2video.mpq", "d2xtalk.mpq", "d2xmusic.mpq", "d2xvideo.mpq"]
# 1.14d: its tables are in patch_d2.mpq; or the official patch installer next to the MPQs.
PATCH = ["patch_d2.mpq", "LODPatch_114d.exe"]
# Optional: Game.exe lets d2d join a LAN game hosted by the original game.
OPTIONAL = ["Game.exe"]


def find_files(folder: pathlib.Path):
    """The game files in folder (any letter case), by their lower-case name."""
    found = {}
    for entry in folder.iterdir():
        if entry.is_file():
            found[entry.name.lower()] = entry
    picked, missing = [], []
    for name in REQUIRED:
        if name in found:
            picked.append(found[name])
        else:
            missing.append(name)
    patch = [found[n.lower()] for n in PATCH if n.lower() in found]
    if patch:
        picked.append(patch[0])
    for name in OPTIONAL:
        if name.lower() in found:
            picked.append(found[name.lower()])
    return picked, missing, bool(patch)


# --- FAT32 -----------------------------------------------------------------------

def short_names(names):
    """An 8.3 name for each long name (NAME~1.EXT), all different."""
    used, out = set(), []
    for name in names:
        stem, _, ext = name.rpartition(".") if "." in name else (name, "", "")
        clean = lambda s: "".join(c for c in s.upper() if c.isascii() and (c.isalnum() or c in "_-"))
        stem, ext = clean(stem) or "FILE", clean(ext)[:3]
        n = 1
        while True:
            tail = "~%d" % n
            s = (stem[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
            if s not in used:
                break
            n += 1
        used.add(s)
        out.append(s.encode("ascii"))
    return out


def lfn_checksum(short11: bytes) -> int:
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entries(name: str, short11: bytes):
    """The long-name entries for name, in the order they are written."""
    units = list(name.encode("utf-16-le"))
    chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    count = math.ceil(len(chars) / 13)
    chars += [0x0000] + [0xFFFF] * (count * 13 - len(chars) - 1) if len(chars) % 13 else []
    chk = lfn_checksum(short11)
    entries = []
    for i in range(count):
        part = chars[i * 13:(i + 1) * 13]
        seq = (i + 1) | (0x40 if i == count - 1 else 0)
        e = bytearray(32)
        e[0] = seq
        for k in range(5):
            struct.pack_into("<H", e, 1 + 2 * k, part[k])
        e[11], e[12], e[13] = 0x0F, 0, chk
        for k in range(6):
            struct.pack_into("<H", e, 14 + 2 * k, part[5 + k])
        for k in range(2):
            struct.pack_into("<H", e, 28 + 2 * k, part[11 + k])
        entries.append(bytes(e))
    return list(reversed(entries))


def dos_time(ts: float):
    t = datetime.datetime.fromtimestamp(ts)
    if t.year < 1980:
        t = datetime.datetime(1980, 1, 1)
    date = ((t.year - 1980) << 9) | (t.month << 5) | t.day
    tm = (t.hour << 11) | (t.minute << 5) | (t.second // 2)
    return date, tm


def short_entry(short11: bytes, attr: int, cluster: int, size: int, ts: float) -> bytes:
    date, tm = dos_time(ts)
    return struct.pack("<11sBBBHHHHHHHI", short11, attr, 0, 0, tm, date, date,
                       cluster >> 16, tm, date, cluster & 0xFFFF, size)


def plan(files):
    """Cluster size, cluster count and FAT size for these files."""
    sizes = [f.stat().st_size for f in files]
    dir_bytes = 32 * (1 + sum(1 + math.ceil(len(f.name) / 13) for f in files))
    for spc in (64, 32, 16, 8, 4, 2, 1):
        cb = spc * SECTOR
        used = math.ceil(dir_bytes / cb) + sum(math.ceil(s / cb) for s in sizes)
        clusters = used + math.ceil(SPARE / cb)
        if clusters >= MIN_CLUSTERS or spc == 1:
            clusters = max(clusters, MIN_CLUSTERS + 16)
            fat_sectors = math.ceil((clusters + 2) * 4 / SECTOR)
            return spc, clusters, fat_sectors, math.ceil(dir_bytes / cb)


def write_image(files, out: pathlib.Path, progress) -> int:
    spc, clusters, fat_sectors, dir_clusters = plan(files)
    cb = spc * SECTOR
    vol_sectors = RESERVED + NUM_FATS * fat_sectors + clusters * spc
    total = (PART_START + vol_sectors) * SECTOR
    vol = PART_START * SECTOR
    fat_off = vol + RESERVED * SECTOR
    data_off = fat_off + NUM_FATS * fat_sectors * SECTOR

    def cluster_off(c):
        return data_off + (c - 2) * cb

    # clusters: the root directory first, then each file, one after the other
    names = [f.name for f in files]
    shorts = short_names(names)
    fat = [0x0FFFFFF8, 0x0FFFFFFF]
    nxt = 2

    def chain(count):
        nonlocal nxt
        if count == 0:
            return 0
        first = nxt
        for i in range(count):
            fat.append(nxt + 1 if i < count - 1 else 0x0FFFFFFF)
            nxt += 1
        return first

    root = chain(dir_clusters)
    starts = [chain(math.ceil(f.stat().st_size / cb)) for f in files]
    used = nxt - 2

    with open(out, "wb") as img:
        img.truncate(total)

        # MBR: one partition, FAT32 with LBA (0x0C)
        mbr = bytearray(SECTOR)
        entry = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", PART_START, vol_sectors)
        mbr[446:462] = entry
        mbr[510:512] = b"\x55\xaa"
        img.seek(0)
        img.write(mbr)

        # the volume's boot sector
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x58\x90"
        bs[3:11] = b"PEOSD2D "
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, RESERVED, NUM_FATS, 0, 0, 0xF8, 0, 63, 255,
                         PART_START, vol_sectors)
        struct.pack_into("<IHHIHH", bs, 36, fat_sectors, 0, 0, root, 1, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, int(time.time()) & 0xFFFFFFFF)
        bs[71:82] = LABEL
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xaa"
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, clusters - used, nxt)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        for base in (0, 6):
            img.seek(vol + base * SECTOR)
            img.write(bs)
            img.write(fsinfo)
            img.seek(vol + (base + 2) * SECTOR + 508)
            img.write(struct.pack("<I", 0xAA550000))

        # the FATs (the rest of each is zero: free)
        fat_bytes = struct.pack("<%dI" % len(fat), *fat)
        for i in range(NUM_FATS):
            img.seek(fat_off + i * fat_sectors * SECTOR)
            img.write(fat_bytes)

        # the root directory: the label, then each file's long and short entries
        d = bytearray(short_entry(LABEL, 0x08, 0, 0, time.time()))
        for f, s, c in zip(files, shorts, starts):
            for e in lfn_entries(f.name, s):
                d += e
            d += short_entry(s, 0x20, c, f.stat().st_size, f.stat().st_mtime)
        img.seek(cluster_off(root))
        img.write(d)

        # the files
        done, grand = 0, sum(f.stat().st_size for f in files)
        for f, c in zip(files, starts):
            if c == 0:
                continue
            img.seek(cluster_off(c))
            with open(f, "rb") as src:
                while True:
                    chunk = src.read(8 * 1024 * 1024)
                    if not chunk:
                        break
                    img.write(chunk)
                    done += len(chunk)
                    progress(done, grand, f.name)
    return total


def main() -> int:
    ap = argparse.ArgumentParser(description="Make the Diablo II data disk for d2d on PeOS (a FAT32 disk image).")
    ap.add_argument("folder", nargs="?", help="the folder of your Diablo II installation (with d2data.mpq)")
    ap.add_argument("-o", "--output", default="d2data.img", help="the image to write (default: d2data.img)")
    ap.add_argument("--force", action="store_true", help="make the disk even if game files are missing")
    args = ap.parse_args()

    if not args.folder:   # started with a double click: ask
        print("d2d data disk for PeOS")
        print("Drag your Diablo II folder here (or type its path), then press Enter:")
        args.folder = input("> ").strip().strip('"').strip("'")
    folder = pathlib.Path(args.folder).expanduser()
    if not folder.is_dir():
        print(f"Folder not found: {folder}")
        return 1
    files, missing, has_patch = find_files(folder)
    print(f"Diablo II folder: {folder}")
    for f in files:
        print(f"  found  {f.name:22s} {f.stat().st_size / 1048576:8.1f} MiB")
    problems = []
    if missing:
        problems.append("missing: " + ", ".join(missing))
    if not has_patch:
        problems.append("no patch_d2.mpq or LODPatch_114d.exe: d2d needs version 1.14d")
    if problems:
        print()
        for p in problems:
            print("  ! " + p)
        print("\n  d2d needs Diablo II with Lord of Destruction, version 1.14d.")
        print("  Are the expansion's files installed? Is this the right folder?")
        if not args.force:
            print("  (use --force to make the disk anyway)")
            return 1

    out = pathlib.Path(args.output).expanduser()
    if out.resolve().parent == folder.resolve():
        print("Write the image somewhere else than the game folder.")
        return 1
    last = [0.0]

    def progress(done, grand, name):
        now = time.time()
        if now - last[0] > 0.5 or done == grand:
            last[0] = now
            pct = 100 * done // grand if grand else 100
            sys.stdout.write(f"\r  writing {pct:3d}%  {name:24s}")
            sys.stdout.flush()

    print(f"\nWriting {out} ...")
    try:
        size = write_image(files, out, progress)
    except OSError as e:
        print(f"\nCould not write {out}: {e}")
        return 1
    print(f"\n\nDone: {out} ({size / 1048576:.0f} MiB, FAT32, label D2DATA).")
    print("Next: attach it to the PeOS virtual machine as a disk (or write it to a USB stick),")
    print("then in PeOS's terminal:")
    print("    mkdir /games")
    print("    mkdir /games/diablo2")
    print("    mount ahci0p1 /games/diablo2 fat32      (a USB stick: usb-msdp1)")
    print("and open Diablo II from the app manager.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
