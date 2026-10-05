# The player's programs, built inside the package builder by build.py.
#
# This file builds what every pack shares: the runtime, the recompiler and
# the shared host's own checks. The pack's cmake file (pack.json build.cmake,
# included below) builds the rest, and must define:
#   launcher, settings-tool   the two programs the app ships
#   host-<slug>               per title: the host objects its game links
#                             on the player's machine (libhost-<slug>.a)
# plus whatever its build.checks run. It may use the variables and targets
# set here: TOOLKIT, PACK, RECOMP, PLAYER_SETTINGS, PLAYER_HOST,
# PLAYER_LAUNCHER, the targets runtime and PkgConfig::SDL. Its launcher is
# PLAYER_LAUNCHER with the pack's settings schema and its psp_launcher_info
# (include/psprecomp/host/launcher.h). Last Raven's
# packaging/linux/CMakeLists.txt until stage 2 of docs/PLAYER-LAYER.md split it.
cmake_minimum_required(VERSION 3.16)
project(psprecomp_player C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Release)
foreach(var TOOLKIT PACK PACK_CMAKE PACK_SLUGS DEPS FFMPEG)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "build.py passes ${var}")
    endif()
endforeach()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf)
file(GLOB RUNTIME_SOURCES "${TOOLKIT}/src/*.c" "${TOOLKIT}/src/hle/*.c")
add_library(runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(runtime PUBLIC "${TOOLKIT}/include" "${DEPS}/include" "${FFMPEG}/include")
target_compile_definitions(runtime PRIVATE PSPRECOMP_HAVE_OPENH264=1 PSPRECOMP_HAVE_FFMPEG=1)
target_link_libraries(runtime PUBLIC "${DEPS}/lib/libopenh264.so" "${FFMPEG}/lib/libavcodec.so" "${FFMPEG}/lib/libavutil.so" m pthread)

set(RECOMP "${TOOLKIT}/tools/allegrexrecomp")
file(GLOB RECOMP_SOURCES "${RECOMP}/*.c" "${RECOMP}/crypto/*.c")
add_executable(allegrexrecomp ${RECOMP_SOURCES})
target_include_directories(allegrexrecomp PRIVATE "${RECOMP}")
target_link_libraries(allegrexrecomp PRIVATE runtime)

# The shared host, compiled into the pack's programs by path.
set(PLAYER_SETTINGS "${TOOLKIT}/src/host/settings.c")
set(PLAYER_HOST "${TOOLKIT}/src/host/present.c" "${TOOLKIT}/src/host/save_dialog.c" "${PLAYER_SETTINGS}")
set(PLAYER_LAUNCHER "${TOOLKIT}/src/host/launcher.c")

# Its own checks. Assertions are the fixtures' checks; Release must not
# compile them out.
add_executable(present-tests "${TOOLKIT}/tests/test_present.c" "${TOOLKIT}/src/host/save_dialog.c" "${PLAYER_SETTINGS}")
target_compile_definitions(present-tests PRIVATE HAVE_SDL2)
target_compile_options(present-tests PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(present-tests PRIVATE runtime PkgConfig::SDL)
# The launcher on a schema of its own; the pack's launcher-tests drive it
# with the pack's.
add_executable(player-launcher-tests "${TOOLKIT}/tests/test_launcher.c" "${PLAYER_SETTINGS}")
target_compile_options(player-launcher-tests PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(player-launcher-tests PRIVATE runtime PkgConfig::SDL)
add_executable(savedata-tests "${TOOLKIT}/tests/test_savedata.c")
target_compile_options(savedata-tests PRIVATE -UNDEBUG)
target_link_libraries(savedata-tests PRIVATE runtime)

include("${PACK_CMAKE}")
foreach(target launcher settings-tool)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "the pack's cmake file must define ${target}")
    endif()
endforeach()
foreach(slug ${PACK_SLUGS})
    if(NOT TARGET host-${slug})
        message(FATAL_ERROR "the pack's cmake file must define host-${slug}")
    endif()
endforeach()
