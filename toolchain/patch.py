#!/usr/bin/env python3
"""Teaches binutils 2.47 and GCC 16.2.0 the target x86_64-peos (the d2decomped
port, phase 2; docs/d2decomped-port-plan.md). Edits the unpacked sources in
place, once (a second run finds them done and says so).

    python3 tools/peos-toolchain/patch.py <binutils dir> <gcc dir>
"""
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent


def edit(path: pathlib.Path, old: str, new: str, done_mark: str) -> None:
    s = path.read_text()
    if done_mark in s:
        return
    if s.count(old) != 1:
        sys.exit(f"patch: {path}: the text to change is not there once")
    path.write_text(s.replace(old, new))


def binutils(b: pathlib.Path) -> None:
    edit(b / "config.sub", "\t| redox* \\\n", "\t| redox* \\\n\t| peos* \\\n", "| peos*")
    edit(b / "bfd/config.bfd",
         "  x86_64-*-elf* | x86_64-*-fuchsia | x86_64-*-genode* | x86_64-*-gnu*)",
         "  x86_64-*-elf* | x86_64-*-fuchsia | x86_64-*-genode* | x86_64-*-gnu* | x86_64-*-peos*)",
         "x86_64-*-peos*")
    edit(b / "gas/configure.tgt", "  i386-*-redox*)\t\t\tfmt=elf ;;\n",
         "  i386-*-redox*)\t\t\tfmt=elf ;;\n  i386-*-peos*)\t\t\tfmt=elf ;;\n", "i386-*-peos*")
    edit(b / "ld/configure.tgt", "x86_64-*-redox*)\ttarg_emul=elf_x86_64\n",
         "x86_64-*-peos*)\t\ttarg_emul=elf_x86_64\n\t\t\t;;\nx86_64-*-redox*)\ttarg_emul=elf_x86_64\n",
         "x86_64-*-peos*")


def gcc(g: pathlib.Path) -> None:
    edit(g / "config.sub", "| fuchsia* | redox* | bme* \\\n", "| fuchsia* | redox* | peos* | bme* \\\n", "| peos* |")
    edit(g / "gcc/config.gcc", "*-*-fuchsia*)\n  native_system_header_dir=/include\n",
         "*-*-peos*)\n  gas=yes\n  gnu_ld=yes\n  default_use_cxa_atexit=yes\n  use_gcc_stdint=wrap\n"
         "  case ${enable_threads} in\n    \"\" | yes | posix) thread_file='posix' ;;\n  esac\n  ;;\n"
         "*-*-fuchsia*)\n  native_system_header_dir=/include\n",
         "*-*-peos*)")
    edit(g / "gcc/config.gcc", "x86_64-*-elf*)\n\ttm_file=",
         "x86_64-*-peos*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h "
         "i386/x86-64.h peos.h\"\n\t;;\nx86_64-*-elf*)\n\ttm_file=",
         "x86_64-*-peos*)\n\ttm_file")
    shutil.copy(HERE / "gcc-peos.h", g / "gcc/config/peos.h")
    edit(g / "libgcc/config.host", "x86_64-*-elf* | x86_64-*-rtems*)\n",
         "x86_64-*-peos*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"\n"
         "\textra_parts=\"$extra_parts crtbegin.o crtend.o\"\n\t;;\nx86_64-*-elf* | x86_64-*-rtems*)\n",
         "x86_64-*-peos*)")
    edit(g / "libstdc++-v3/crossconfig.m4",
         "  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris*)\n",
         "  *-peos*)\n    GLIBCXX_CHECK_COMPILER_FEATURES\n    GLIBCXX_CHECK_LINKER_FEATURES\n"
         "    GLIBCXX_CHECK_MATH_SUPPORT\n    GLIBCXX_CHECK_STDLIB_SUPPORT\n"
         "    AC_DEFINE(_GLIBCXX_USE_RANDOM_TR1)\n    GCC_CHECK_TLS\n"
         "    AC_CHECK_FUNCS(aligned_alloc posix_memalign memalign)\n    AC_CHECK_FUNCS(timespec_get)\n"
         "    ;;\n  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris*)\n",
         "  *-peos*)")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    binutils(pathlib.Path(sys.argv[1]))
    gcc(pathlib.Path(sys.argv[2]))
    print("patch: binutils and gcc know x86_64-peos")
    return 0


if __name__ == "__main__":
    sys.exit(main())
