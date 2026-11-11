# Third-party software

d2d.app is built from these projects. Their sources are downloaded by
`build.sh` at the versions below; none of them is copied here.

| Project | Version | License | Used for |
| --- | --- | --- | --- |
| [d2decomped](https://github.com/psi29a/d2decomped) | commit `5fbd711` | GPL-3.0-or-later | the game |
| [StormLib](https://github.com/ladislav-zezula/StormLib) | v9.30, commit `86f9b99` | MIT | reading the MPQ archives |
| [zlib](https://zlib.net) | 1.3.1 | zlib | compression |
| [bzip2](https://sourceware.org/bzip2/) | 1.0.8 | bzip2 (BSD-like) | compression |
| [SDL](https://libsdl.org) | 3.2.26, headers only | zlib | the API the game is written against |
| [FFmpeg](https://ffmpeg.org) | 9.0.2, Bink only | LGPL-2.1-or-later | the films |
| [GCC](https://gcc.gnu.org) | 16.2.0 | GPL-3.0 with the Runtime Library Exception | the compiler, libstdc++ and libgcc in the app |
| [binutils](https://www.gnu.org/software/binutils/) | 2.47 | GPL-3.0 | the assembler and linker |

`platform/sdl3/sdl3_peos.c` and `platform/openal/al_peos.c` are PeOS's own
implementations of the SDL3 and OpenAL APIs; `platform/openal/include/AL`
are PeOS's own headers with the OpenAL 1.1 specification's types and values.

The PeOS SDK (from the [Starlight App Kit](https://github.com/teddi201252/starlight-app-kit),
fetched by `build.sh`) is PeOS's: the headers and the built libraries of its
C library, which every PeOS program is linked with (the operating system's
own libraries).

Diablo II is a trademark of Blizzard Entertainment, Inc. This project is
not affiliated with or endorsed by Blizzard and contains none of its files;
the game data is the user's own.
