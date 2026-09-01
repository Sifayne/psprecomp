# psprecomp — a static-recompilation toolkit for PlayStation Portable games.
#
# This file builds two things:
#   1. the `psprecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory map, semantic helpers)
#   2. `allegrexrecomp` — the tool that turns a PSP module into C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psprecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `psprecomp` target.
#
# The core has no external dependencies on purpose. SDL2 and friends only ever
# appear behind an opt-in option, so a fresh clone builds with nothing installed.

cmake_minimum_required(VERSION 3.16)
project(psprecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # The generated C is millions of lines; MSVC's default object format
    # cannot hold that many sections.
    add_compile_options(/bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

add_library(psprecomp STATIC
    src/cpu.c
    src/mem.c
    src/dispatch.c
    src/ctors.c
    src/render.c
    src/vfpu.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/waitq.c
    src/hle/kernlock.c
    src/hle/sched.c
    src/hle/clock.c
    src/hle/mpeg.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
    src/hle/umd.c
)

target_include_directories(psprecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vfpu.c calls sinf/cosf/powf/logf/asinf. MSVC keeps those in the CRT, but on
# Unix they live in libm and need an explicit link. PUBLIC so that consumers —
# the test binaries, and any game host linking this runtime — inherit it too.
# The scheduler gives each guest thread a host thread; see src/hle/sched.c for
# why saved registers are not enough.
find_package(Threads REQUIRED)
target_link_libraries(psprecomp PUBLIC Threads::Threads)

# --- optional: H.264 decoding for sceMpeg -------------------------------------
#
# openh264 is BSD, so it can be linked into an MIT project without changing what
# anything downstream may do with it. That is why it is here and FFmpeg is not:
# a distro FFmpeg is usually built --enable-gpl, and linking that would relicense
# every consumer of this library.
#
# Optional on purpose. Without it sceMpeg keeps its bookkeeping-only behaviour
# and refuses playback, which is what it did before this existed.
find_path(OPENH264_INCLUDE_DIR wels/codec_api.h)
find_library(OPENH264_LIBRARY openh264)
if(OPENH264_INCLUDE_DIR AND OPENH264_LIBRARY)
    target_compile_definitions(psprecomp PRIVATE PSPRECOMP_HAVE_OPENH264=1)
    target_include_directories(psprecomp PRIVATE ${OPENH264_INCLUDE_DIR})
    target_link_libraries(psprecomp PUBLIC ${OPENH264_LIBRARY})
    message(STATUS "sceMpeg: H.264 decoding via openh264")
else()
    message(STATUS "sceMpeg: no openh264; playback stays refused")
endif()

if(NOT MSVC)
    find_library(PSPRECOMP_LIBM m)
    if(PSPRECOMP_LIBM)
        target_link_libraries(psprecomp PUBLIC ${PSPRECOMP_LIBM})
    endif()
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
