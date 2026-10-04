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
    src/os.c
    src/render.c
    src/vfpu.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/waitq.c
    src/hle/kernlock.c
    src/hle/kernobj.c
    src/hle/ktimer.c
    src/hle/interrupt.c
    src/hle/sched.c
    src/hle/clock.c
    src/hle/mpeg.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
    src/hle/net.c
    src/hle/ctrl_replay.c
    src/hle/umd.c
    src/hle/utility.c
    src/hle/atrac.c
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

# --- optional: ATRAC3 / ATRAC3+ decoding for sceAtrac3plus ---------------------
#
# FFmpeg's libavcodec has the only complete open ATRAC3+ decoder, and it is
# LGPL-2.1 -- fine to link dynamically from an MIT project. The caveat is the
# distro build: an FFmpeg configured with --enable-gpl (most are) makes the
# whole library GPL, and a binary linked against it inherits that. So it is
# optional, dynamic, and never required: without it sceAtrac3plus keeps the
# bookkeeping and every track ends at its first frame, which is what it did
# before this existed. Only avcodec and avutil; the float-to-PCM conversion
# is done in atrac.c rather than pulling in swresample.
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVUTIL_LIBRARY avutil)
if(FFMPEG_INCLUDE_DIR AND AVCODEC_LIBRARY AND AVUTIL_LIBRARY)
    target_compile_definitions(psprecomp PRIVATE PSPRECOMP_HAVE_FFMPEG=1)
    target_include_directories(psprecomp PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(psprecomp PUBLIC ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
    message(STATUS "sceAtrac3plus: ATRAC3/ATRAC3+ decoding via FFmpeg libavcodec")
else()
    message(STATUS "sceAtrac3plus: no libavcodec; tracks end at their first frame")
endif()

if(NOT MSVC)
    find_library(PSPRECOMP_LIBM m)
    if(PSPRECOMP_LIBM)
        target_link_libraries(psprecomp PUBLIC ${PSPRECOMP_LIBM})
    endif()
endif()

# --- optional: the SDL2 host layer --------------------------------------------
#
# Code a game's host needs and the runtime does not: so far the savedata
# dialog (src/host/save_dialog.c), drawn with SDL2_ttf and stb_image
# (third_party/stb, MIT). Off by default and required when asked for -- the
# runtime itself never touches SDL. A host that builds its own sources can
# compile src/host/*.c directly instead; they need SDL2, SDL2_ttf, include/
# and third_party/, nothing else from here.
option(PSPRECOMP_HOST "Build the SDL2 host layer (psprecomp_host)" OFF)
if(PSPRECOMP_HOST)
    add_subdirectory(src/host)
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
    if(UNIX)
        add_subdirectory(tools/oracle)
    endif()
endif()
