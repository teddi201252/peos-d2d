# CMake for PeOS: the x86_64-peos cross compiler and the PeOS sysroot that
# build.sh makes. Static programs only; the platform's rules are in
# cmake/Platform/PeOS.cmake. build.sh sets PEOS_PREFIX (the toolchain) and
# PEOS_SYSROOT in the environment.
set(CMAKE_SYSTEM_NAME PeOS)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

if(NOT DEFINED ENV{PEOS_PREFIX} OR NOT DEFINED ENV{PEOS_SYSROOT})
    message(FATAL_ERROR "PEOS_PREFIX and PEOS_SYSROOT are not set: build with ./build.sh")
endif()
set(_peos_bin "$ENV{PEOS_PREFIX}/bin")
set(CMAKE_C_COMPILER "${_peos_bin}/x86_64-peos-gcc")
set(CMAKE_CXX_COMPILER "${_peos_bin}/x86_64-peos-g++")
set(CMAKE_AR "${_peos_bin}/x86_64-peos-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_peos_bin}/x86_64-peos-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${_peos_bin}/x86_64-peos-strip" CACHE FILEPATH "")
set(CMAKE_SYSROOT "$ENV{PEOS_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
