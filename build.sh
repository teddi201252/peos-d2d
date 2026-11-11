#!/usr/bin/env bash
# Rebuild d2d.app, the Diablo II app of PeOS, from source.
#
#   ./build.sh            everything; the app ends up in out/d2d.app
#   ./build.sh check      only check that the tools it needs are installed
#   ./build.sh store      after a build: the bundle and its texts for PeStore
#   ./build.sh clean      start over (keeps the downloads)
#
# Linux (also WSL on Windows) and macOS. The first build takes a while
# (about 25 minutes on a recent machine: the cross compiler first, then the
# game); a build that stops resumes where it was. See docs/BUILDING.md.
#
# What it does: builds the x86_64-peos cross compiler (binutils, GCC with
# libstdc++) over the PeOS SDK of the Starlight App Kit (libpeos and
# libpeos-compat, fetched at a fixed commit), then zlib, bzip2, SDL3 and OpenAL
# for PeOS and a Bink-only FFmpeg, then fetches d2decomped and StormLib at
# fixed commits, applies the patches in port/patches and builds d2d.
# Every download is checked (GNU signatures, SHA-256, git commits).
set -euo pipefail
REPO="$(cd "$(dirname "$0")" && pwd)"
OS="$(uname -s)"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Where the work happens: build/ here, or with the repository on a Windows
# drive under WSL (/mnt/...) the Linux disk, much faster and case-sensitive
# as GCC's sources need. PEOS_D2D_WORK chooses another place.
if [ -n "${PEOS_D2D_WORK:-}" ]; then
    W="$PEOS_D2D_WORK"
elif [ "$OS" = Linux ] && [[ "$REPO" == /mnt/* ]]; then
    W="$HOME/.cache/peos-d2d"
else
    W="$REPO/build"
fi
OUT="$REPO/out"
TARGET=x86_64-peos
export PEOS_PREFIX="$W/toolchain"
export PEOS_SYSROOT="$W/sysroot"
export PATH="$PEOS_PREFIX/bin:$PATH"

# --- versions (every one fixed and checked) -----------------------------------------

# The PeOS SDK: the Starlight App Kit's sdk/ (PeOS 0.15.0, SDK 1.14.0).
# PEOS_APP_KIT=/path/to/starlight-app-kit uses a local copy instead.
KIT_REPO=https://github.com/teddi201252/starlight-app-kit
KIT_COMMIT=abb9955d7a9f92fde6f6d5bb9f6836c4b2717fb1   # release 0.15.0
BINUTILS=binutils-2.47
GCC=gcc-16.2.0
D2D_REPO=https://github.com/psi29a/d2decomped
D2D_COMMIT=5fbd7115e4cae7b34bd7f59b3523b2d384b190e8
STORMLIB_REPO=https://github.com/ladislav-zezula/StormLib
STORMLIB_COMMIT=86f9b99ffe4d3417dad16d00541cf6f2e3d7bf79   # v9.30, what d2d asks for
ZLIB=zlib-1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
BZIP2=bzip2-1.0.8
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
SDL3=SDL3-3.2.26   # only its headers (zlib license); the code is platform/sdl3
SDL3_SHA256=dad488474a51a0b01d547cd2834893d6299328d2e30f479a3564088b5476bae2
FFMPEG=ffmpeg-9.0.2   # the one d2d's CMake asks for, Bink only
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\nbuild.sh: %s\n' "$*" >&2; exit 1; }

# A stage done once is not done again (./build.sh clean starts over). Its
# output goes to $W/logs/<stage>.log (compilers are chatty); when it fails,
# the end of the log is shown. (Run in the background and waited for, so
# that set -e still stops the stage at its first error.)
run_logged() {   # name function
    mkdir -p "$W/logs"
    ( "$2" ) > "$W/logs/$1.log" 2>&1 &
    if ! wait $!; then
        echo
        tail -n 25 "$W/logs/$1.log"
        die "$1 failed; the whole log: $W/logs/$1.log"
    fi
}

stage() {   # name function what
    if [ -f "$W/.done-$1" ]; then
        echo "    $1: done before"
        return
    fi
    say "$3"
    run_logged "$1" "$2"
    touch "$W/.done-$1"
}

# --- the tools this needs -------------------------------------------------------------

check() {
    local missing=()
    for t in curl git tar xz make cmake python3 perl m4 gcc g++; do
        command -v "$t" >/dev/null || missing+=("$t")
    done
    command -v gpgv >/dev/null || command -v docker >/dev/null || missing+=("gpgv")
    if [ "$OS" = Darwin ]; then
        command -v brew >/dev/null || missing+=("Homebrew (https://brew.sh)")
        if command -v brew >/dev/null; then
            for p in gmp mpfr libmpc; do
                [ -d "$(brew --prefix)/opt/$p" ] || missing+=("$p")
            done
        fi
    else
        echo '#include <gmp.h>
#include <mpfr.h>
#include <mpc.h>' | gcc -E - >/dev/null 2>&1 || missing+=("GMP/MPFR/MPC headers")
        echo '#include <zlib.h>' | gcc -E - >/dev/null 2>&1 || missing+=("zlib headers")
    fi
    if [ ${#missing[@]} -gt 0 ]; then
        echo "Missing: ${missing[*]}"
        echo
        if [ "$OS" = Darwin ]; then
            echo "Install them with Homebrew:"
            echo "    brew install cmake xz gmp mpfr libmpc gnupg"
        else
            echo "On Debian or Ubuntu (WSL too):"
            echo "    sudo apt install build-essential curl git xz-utils cmake python3 m4 gpgv \\"
            echo "        libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev"
            echo "On Fedora:"
            echo "    sudo dnf install gcc gcc-c++ make curl git xz cmake python3 m4 gnupg2 \\"
            echo "        gmp-devel mpfr-devel libmpc-devel zlib-devel"
        fi
        exit 1
    fi
    echo "All the tools are there ($OS, $JOBS jobs, work in $W)."
}

# --- downloads --------------------------------------------------------------------------

sha256_check() {
    if command -v shasum >/dev/null; then shasum -a 256 -c -; else sha256sum -c -; fi
}

fetch_sha() {   # url sha256 [saved name]
    local f="${3:-$(basename "$1")}"
    if [ ! -f "$W/dl/$f" ]; then
        curl -sSLf "$1" -o "$W/dl/$f.part" || die "cannot download $1"
        mv "$W/dl/$f.part" "$W/dl/$f"
    fi
    echo "$2  $W/dl/$f" | sha256_check >/dev/null || { rm -f "$W/dl/$f"; die "$f: wrong SHA-256 (removed; run again)"; }
}

# A GNU release, checked against its signature and GNU's keyring.
fetch_gnu() {   # url
    local f; f="$(basename "$1")"
    (cd "$W/dl" && { [ -f "$f" ] || curl -sSLfO "$1"; } && { [ -f "$f.sig" ] || curl -sSLfO "$1.sig"; } &&
        { [ -f gnu-keyring.gpg ] || curl -sSLfO https://ftp.gnu.org/gnu/gnu-keyring.gpg; }) || die "cannot download $1"
    if command -v gpgv >/dev/null; then
        (cd "$W/dl" && gpgv --keyring ./gnu-keyring.gpg "$f.sig" "$f" 2>/dev/null) || die "$f: bad signature"
    else
        docker run --rm -v "$W/dl":/d -w /d debian:trixie bash -c "apt-get update >/dev/null 2>&1;
            apt-get install -y gpgv >/dev/null 2>&1; gpgv --keyring ./gnu-keyring.gpg $f.sig $f" || die "$f: bad signature"
    fi
}

checkout() {   # repo commit dir patches: the commit, clean, with the patches
    [ -d "$3/.git" ] || git clone -q "$1" "$3"
    git -C "$3" cat-file -e "$2^{commit}" 2>/dev/null || git -C "$3" fetch -q origin "$2"
    git -C "$3" -c advice.detachedHead=false checkout -q --detach "$2"
    git -C "$3" reset -q --hard "$2"
    git -C "$3" clean -qfd
    for p in "$4"/*.patch; do
        [ -e "$p" ] || continue
        git -C "$3" apply "$p"
    done
}

# --- the cross compiler -------------------------------------------------------------------

toolchain_sources() {
    fetch_gnu "https://ftpmirror.gnu.org/gnu/binutils/$BINUTILS.tar.xz"
    fetch_gnu "https://ftpmirror.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz"
    rm -rf "$W/src" && mkdir -p "$W/src"
    tar -xf "$W/dl/$BINUTILS.tar.xz" -C "$W/src"
    tar -xf "$W/dl/$GCC.tar.xz" -C "$W/src"
    python3 "$REPO/toolchain/patch.py" "$W/src/$BINUTILS" "$W/src/$GCC"
    # libstdc++'s configure regenerated with the target added: autoconf 2.69,
    # the one GCC uses (installed, or GNU's built here)
    if command -v autoconf2.69 >/dev/null; then
        (cd "$W/src/$GCC/libstdc++-v3" && autoconf2.69)
    else
        if [ ! -x "$W/autoconf/bin/autoconf" ]; then
            fetch_gnu https://ftpmirror.gnu.org/gnu/autoconf/autoconf-2.69.tar.xz
            rm -rf "$W/autoconf-2.69" && tar -xf "$W/dl/autoconf-2.69.tar.xz" -C "$W"
            (cd "$W/autoconf-2.69" && ./configure --prefix="$W/autoconf" >/dev/null && make >/dev/null &&
                make install >/dev/null)
        fi
        (cd "$W/src/$GCC/libstdc++-v3" && "$W/autoconf/bin/autoconf")
    fi
}

binutils() {
    rm -rf "$W/b-binutils" && mkdir -p "$W/b-binutils" && cd "$W/b-binutils"
    "$W/src/$BINUTILS/configure" --target=$TARGET --prefix="$PEOS_PREFIX" --with-sysroot="$PEOS_SYSROOT" \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --disable-sim >/dev/null
    make -j"$JOBS" >/dev/null && make install >/dev/null
}

# Where the Starlight App Kit is: fetched under $W, or a local copy.
kit_dir() {
    if [ -n "${PEOS_APP_KIT:-}" ]; then echo "$PEOS_APP_KIT"; else echo "$W/starlight-app-kit"; fi
}

# The PeOS SDK, from the Starlight App Kit: the base (libpeos) and, over it,
# what ports need (libpeos-compat), laid out as the toolchain's sysroot.
sysroot() {
    local kit; kit="$(kit_dir)"
    if [ -z "${PEOS_APP_KIT:-}" ]; then
        checkout "$KIT_REPO" "$KIT_COMMIT" "$kit" /nonexistent
    fi
    local sdk="$kit/sdk"
    [ -f "$sdk/compat/lib/libpeos-compat.a" ] || die "$sdk has no compat/ (the Starlight App Kit 0.15.0 or later)"
    rm -rf "$PEOS_SYSROOT" && mkdir -p "$PEOS_SYSROOT/usr/include/peos-base" "$PEOS_SYSROOT/usr/lib"
    cp -R "$sdk/include/." "$PEOS_SYSROOT/usr/include/"
    cp -R "$sdk/include/." "$PEOS_SYSROOT/usr/include/peos-base/"   # compat's own headers include them so
    cp -R "$sdk/compat/include/." "$PEOS_SYSROOT/usr/include/"      # compat over the base where both have one
    cp "$sdk/lib/libpeos.a" "$sdk/compat/lib/libpeos-compat.a" "$PEOS_SYSROOT/usr/lib/"
    # maths, threads, clocks are all in PeOS's C library; the names ports
    # pass (g++ adds -lm) are empty archives
    for l in m pthread rt dl; do
        rm -f "$PEOS_SYSROOT/usr/lib/lib$l.a" && "$PEOS_PREFIX/bin/$TARGET-ar" rc "$PEOS_SYSROOT/usr/lib/lib$l.a"
    done
}

gcc_stage() {
    local gmp=()
    if [ "$OS" = Darwin ]; then
        local b; b="$(brew --prefix)"
        gmp=(--with-gmp="$b" --with-mpfr="$b" --with-mpc="$b")
    fi
    rm -rf "$W/b-gcc" && mkdir -p "$W/b-gcc" && cd "$W/b-gcc"
    "$W/src/$GCC/configure" --target=$TARGET --prefix="$PEOS_PREFIX" --with-sysroot="$PEOS_SYSROOT" \
        --with-native-system-header-dir=/usr/include \
        --enable-languages=c,c++ --disable-nls --disable-multilib --disable-shared --disable-libssp \
        --disable-libquadmath --disable-libgomp --disable-libatomic --disable-libsanitizer --disable-libvtv \
        --disable-libitm --disable-bootstrap --enable-threads=posix --enable-initfini-array \
        --enable-tls --enable-libstdcxx-time=yes --disable-wchar_t \
        "${gmp[@]}" --without-isl --with-system-zlib >/dev/null
    make -j"$JOBS" all-gcc all-target-libgcc >/dev/null
    make install-gcc install-target-libgcc >/dev/null
}

libstdcxx() {
    # the programs' entry (libpeos-compat's crt0), with the new compiler:
    # libstdc++'s configure links test programs
    "$PEOS_PREFIX/bin/$TARGET-gcc" -c "$(kit_dir)/sdk/compat/crt0.S" -o "$PEOS_SYSROOT/usr/lib/crt0.o"
    cd "$W/b-gcc"
    make -j"$JOBS" all-target-libstdc++-v3 >/dev/null
    make install-target-libstdc++-v3 >/dev/null
}

# --- the game's libraries ----------------------------------------------------------------------

sources() {
    fetch_sha "https://zlib.net/fossils/$ZLIB.tar.gz" "$ZLIB_SHA256"
    fetch_sha "https://sourceware.org/pub/bzip2/$BZIP2.tar.gz" "$BZIP2_SHA256"
    fetch_sha "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3#SDL3-}/$SDL3.tar.gz" "$SDL3_SHA256"
    fetch_sha "https://deb.debian.org/debian/pool/main/f/ffmpeg/ffmpeg_${FFMPEG#ffmpeg-}.orig.tar.xz" \
        "$FFMPEG_SHA256" "$FFMPEG.tar.xz"
    checkout "$D2D_REPO" "$D2D_COMMIT" "$W/d2d" "$REPO/port/patches/d2d"
    checkout "$STORMLIB_REPO" "$STORMLIB_COMMIT" "$W/stormlib" "$REPO/port/patches/stormlib"
}

deps() {
    rm -rf "$W/deps" && mkdir -p "$W/deps"
    tar -xzf "$W/dl/$ZLIB.tar.gz" -C "$W/deps"
    tar -xzf "$W/dl/$BZIP2.tar.gz" -C "$W/deps"
    (cd "$W/deps/$ZLIB" && CHOST=$TARGET CC=$TARGET-gcc AR=$TARGET-ar RANLIB=$TARGET-ranlib CFLAGS="-O2" \
        ./configure --static --prefix=/usr >/dev/null && make -j"$JOBS" libz.a >/dev/null)
    cp "$W/deps/$ZLIB/libz.a" "$PEOS_SYSROOT/usr/lib/"
    cp "$W/deps/$ZLIB/zlib.h" "$W/deps/$ZLIB/zconf.h" "$PEOS_SYSROOT/usr/include/"
    (cd "$W/deps/$BZIP2" && make -j"$JOBS" CC=$TARGET-gcc AR=$TARGET-ar RANLIB=$TARGET-ranlib \
        CFLAGS="-O2 -D_FILE_OFFSET_BITS=64" libbz2.a >/dev/null)
    cp "$W/deps/$BZIP2/libbz2.a" "$PEOS_SYSROOT/usr/lib/"
    cp "$W/deps/$BZIP2/bzlib.h" "$PEOS_SYSROOT/usr/include/"
}

# SDL3 and OpenAL written over PeOS (platform/), SDL's own headers, and an
# FFmpeg with only what Bink videos need.
platform() {
    local cc="$TARGET-gcc -std=gnu11 -O2 -Wall -Wextra -Werror"
    mkdir -p "$W/platform"
    rm -rf "$W/deps/$SDL3" && tar -xzf "$W/dl/$SDL3.tar.gz" -C "$W/deps" "$SDL3/include" "$SDL3/LICENSE.txt"
    rm -rf "$PEOS_SYSROOT/usr/include/SDL3" && cp -R "$W/deps/$SDL3/include/SDL3" "$PEOS_SYSROOT/usr/include/"
    $cc -I"$PEOS_SYSROOT/usr/include" -c "$REPO/platform/sdl3/sdl3_peos.c" -o "$W/platform/sdl3_peos.o"
    rm -f "$PEOS_SYSROOT/usr/lib/libSDL3.a" && $TARGET-ar rcs "$PEOS_SYSROOT/usr/lib/libSDL3.a" "$W/platform/sdl3_peos.o"
    mkdir -p "$PEOS_SYSROOT/usr/lib/cmake/SDL3" && cp "$REPO/platform/sdl3/cmake/"*.cmake "$PEOS_SYSROOT/usr/lib/cmake/SDL3/"
    rm -rf "$PEOS_SYSROOT/usr/include/AL" && cp -R "$REPO/platform/openal/include/AL" "$PEOS_SYSROOT/usr/include/"
    $cc -I"$PEOS_SYSROOT/usr/include" -c "$REPO/platform/openal/al_peos.c" -o "$W/platform/al_peos.o"
    rm -f "$PEOS_SYSROOT/usr/lib/libopenal.a" && $TARGET-ar rcs "$PEOS_SYSROOT/usr/lib/libopenal.a" "$W/platform/al_peos.o"
    rm -rf "$W/deps/$FFMPEG" "$W/ffmpeg-build" "$W/ffmpeg-peos" && tar -xJf "$W/dl/$FFMPEG.tar.xz" -C "$W/deps"
    mkdir -p "$W/ffmpeg-build"
    (cd "$W/ffmpeg-build" && "$W/deps/$FFMPEG/configure" --prefix="$W/ffmpeg-peos" \
        --enable-cross-compile --target-os=none --arch=x86_64 --cross-prefix=$TARGET- \
        --cc=$TARGET-gcc --cxx=$TARGET-g++ --pkg-config=false \
        --disable-everything --disable-autodetect --disable-programs --disable-doc \
        --disable-network --disable-avdevice --disable-avfilter --disable-faan --disable-iamf \
        --disable-swscale-alpha --disable-pthreads --disable-runtime-cpudetect --disable-debug \
        --enable-static --disable-shared --enable-pic --disable-x86asm \
        --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct >/dev/null &&
        make -j"$JOBS" >/dev/null && make install >/dev/null)
}

# --- the game ---------------------------------------------------------------------------------

game() {
    cmake -S "$W/d2d" -B "$W/game" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$REPO/port/toolchain-peos.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DD2D_WERROR=OFF -DD2D_BUILD_APPS=ON -DD2D_BUILD_TOOLS=OFF -DD2D_BUILD_TESTS=OFF \
        -DD2D_FFMPEG_PREFIX="$W/ffmpeg-peos" \
        -DOPENAL_INCLUDE_DIR="$PEOS_SYSROOT/usr/include" -DOPENAL_LIBRARY="$PEOS_SYSROOT/usr/lib/libopenal.a" \
        -DD2D_BUNDLED_STORMLIB=ON -DFETCHCONTENT_SOURCE_DIR_STORMLIB="$W/stormlib" \
        -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
    cmake --build "$W/game" -j"$JOBS"
}

# d2d.app for Starlight: the game, bundle/app.ini and its texts, d2d's icon (128 px)
app() {
    local out="$OUT/d2d.app"
    rm -rf "$out" && mkdir -p "$out/resources"
    $TARGET-strip -o "$out/app.bin" "$W/game/apps/d2d/d2d"
    cp "$REPO/port/bundle/app.ini" "$out/app.ini"
    cp -R "$REPO/port/bundle/resources/." "$out/resources/"
    if command -v sips >/dev/null; then
        sips -z 128 128 "$W/d2d/assets/icon/d2d.png" --out "$out/resources/icon.png" >/dev/null
    else
        python3 "$REPO/tools/png_resize.py" "$W/d2d/assets/icon/d2d.png" "$out/resources/icon.png" 128 128
    fi
}

all() {
    check
    mkdir -p "$W/dl" "$OUT"
    stage toolchain-sources toolchain_sources "binutils and GCC: download, check, patch"
    stage binutils binutils "binutils for PeOS"
    stage sysroot sysroot "the PeOS SDK as the sysroot"
    stage gcc gcc_stage "GCC for PeOS (a few minutes)"
    stage libstdcxx libstdcxx "libstdc++ for PeOS"
    stage sources sources "d2decomped, StormLib, zlib, bzip2, SDL3, FFmpeg: download and check"
    stage deps deps "zlib and bzip2"
    stage platform platform "SDL3 and OpenAL for PeOS, FFmpeg for the films"
    say "d2d (a few minutes)"
    run_logged game game
    say "d2d.app"
    run_logged app app
    local sum
    sum="$( (shasum -a 256 "$OUT/d2d.app/app.bin" 2>/dev/null || sha256sum "$OUT/d2d.app/app.bin") | cut -d' ' -f1)"
    echo
    echo "Done: $OUT/d2d.app"
    echo "  app.bin SHA-256 $sum"
    echo "  PeOS SDK $(cat "$(kit_dir)/sdk/VERSION") (Starlight App Kit), d2decomped ${D2D_COMMIT:0:7}"
}

# For PeStore: out/d2d.app.tar (the bundle as appget and PeStore take it) and
# out/store/ (bundle/app.ini and resources/: what the Starlight App Kit's
# scripts/publish_app.py reads for the catalog: version, category, texts).
store() {
    [ -f "$OUT/d2d.app/app.bin" ] || die "no $OUT/d2d.app yet: run ./build.sh first"
    [ -f "$(kit_dir)/scripts/pack_app.py" ] || checkout "$KIT_REPO" "$KIT_COMMIT" "$(kit_dir)" /nonexistent
    python3 "$(kit_dir)/scripts/pack_app.py" "$OUT/d2d.app" "$OUT/d2d.app.tar"
    rm -rf "$OUT/store" && mkdir -p "$OUT/store/bundle"
    cp "$OUT/d2d.app/app.ini" "$OUT/store/bundle/app.ini"
    cp -R "$OUT/d2d.app/resources" "$OUT/store/resources"
    if compgen -G "$REPO/port/store/*.png" >/dev/null; then
        mkdir -p "$OUT/store/store" && cp "$REPO/port/store/"*.png "$OUT/store/store/"
    fi
    echo "Ready for PeStore: $OUT/d2d.app.tar and $OUT/store"
    echo "Publish it with the Starlight App Kit (the store's key and catalog are the publisher's):"
    echo "    python3 scripts/publish_app.py --app-id d2d --app-dir \"$OUT/store\" \\"
    echo "        --tar \"$OUT/d2d.app.tar\" --store <store folder> --key <store key>"
}

case "${1:-all}" in
    all) all ;;
    check) check ;;
    store) store ;;
    clean) rm -rf "$W/src" "$W/b-binutils" "$W/b-gcc" "$W/toolchain" "$W/sysroot" "$W/deps" "$W/platform" \
               "$W/ffmpeg-build" "$W/ffmpeg-peos" "$W/game" "$W"/.done-* "$OUT"
           echo "Cleaned (the downloads in $W/dl are kept)." ;;
    *) echo "usage: $0 [all|check|store|clean]"; exit 2 ;;
esac
