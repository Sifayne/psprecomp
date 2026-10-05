# The SDL2 host layer; see the root CMakeLists.txt for why it is optional.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PSPRECOMP_SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf)

add_library(psprecomp_host STATIC save_dialog.c)

# SDL is linked privately because its compile flags define _GNU_SOURCE, which
# changes what the C library declares for whatever else is compiled with them
# (The 3rd Birthday's boot saw SIGSTKSZ change). Its include directories are
# public: save_dialog.h includes <SDL2/SDL.h>.
target_link_libraries(psprecomp_host
    PUBLIC  psprecomp psprecomp_settings
    PRIVATE PkgConfig::PSPRECOMP_SDL2)
target_include_directories(psprecomp_host PUBLIC ${PSPRECOMP_SDL2_INCLUDE_DIRS})
message(STATUS "psprecomp_host: SDL2 host layer (save dialog)")
