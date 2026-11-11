# PeOS for CMake: static ELF programs, libraries in /usr/lib of the
# sysroot. PeOS is not POSIX (no __unix__), but its ports' build files ask
# UNIX for "not Windows, not Apple": UnixPaths sets it.
include(Platform/UnixPaths)
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
