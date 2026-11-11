# SDL3 on PeOS (userland/ports/sdl3): SDL 3.2's headers, PeOS's libSDL3.a
# (sdl3_peos.c over Starlight), static. Installed in the ports' sysroot.
get_filename_component(_sdl3_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 STATIC IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_LOCATION "${_sdl3_prefix}/lib/libSDL3.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_sdl3_prefix}/include")
endif()
if(NOT TARGET SDL3::SDL3-static)
    add_library(SDL3::SDL3-static ALIAS SDL3::SDL3)
endif()
set(SDL3_FOUND TRUE)
