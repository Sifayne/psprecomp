# The player's programs, built inside the package builder by build.py.
#
# The app's own: the runtime (libruntime.a); the player archive every game
# links on the player's machine (libplayer.a: the shared host, its menu and
# Dear ImGui, the settings mechanism and the recompiler's loader); the
# recompiler; the launcher, which holds no pack and loads the ones added
# (src/host/launcher_none.c); and the shared host's checks. Then each pack
# named in PACK_IDS, PACK_ROOTS and PACK_CMAKES: its cmake file (pack.json
# build.cmake) builds the pack's checks, in a folder of its own whose
# programs land in pack-<id>/. It may use the variables and targets set
# here: TOOLKIT, PACK, RECOMP, PLAYER_SETTINGS, PLAYER_HOST, PLAYER_UI,
# PLAYER_LAUNCHER, the targets runtime and PkgConfig::SDL. Last Raven's
# packaging/linux/CMakeLists.txt until stage 2 of docs/PLAYER-LAYER.md split
# it; stage 10 made the app one for every pack.
cmake_minimum_required(VERSION 3.16)
project(psprecomp_player C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Release)
foreach(var TOOLKIT DEPS FFMPEG)
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

# The shared host. Dear ImGui (third_party/imgui) and the settings pages
# draw the menu and the launcher alike; ui.cpp and ImGui are C++ without
# exceptions, RTTI or thread-safe statics, which needs nothing of the C++
# runtime, so a game's C link on the player's machine takes them as they are.
set(PLAYER_SETTINGS "${TOOLKIT}/src/host/settings.c")
set(PLAYER_IMGUI "${TOOLKIT}/third_party/imgui")
add_library(player_imgui OBJECT "${TOOLKIT}/src/host/pages.c" "${TOOLKIT}/src/host/ui.cpp"
    "${PLAYER_IMGUI}/imgui.cpp" "${PLAYER_IMGUI}/imgui_draw.cpp" "${PLAYER_IMGUI}/imgui_tables.cpp"
    "${PLAYER_IMGUI}/imgui_widgets.cpp" "${PLAYER_IMGUI}/backends/imgui_impl_sdl2.cpp"
    "${PLAYER_IMGUI}/backends/imgui_impl_sdlrenderer2.cpp")
target_include_directories(player_imgui PRIVATE "${PLAYER_IMGUI}")
target_compile_definitions(player_imgui PRIVATE HAVE_SDL2)
target_compile_options(player_imgui PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fno-threadsafe-statics>")
set_target_properties(player_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(player_imgui PRIVATE runtime PkgConfig::SDL)
set(PLAYER_UI "${TOOLKIT}/src/host/overlay.c" $<TARGET_OBJECTS:player_imgui>)
set(PLAYER_HOST "${TOOLKIT}/src/host/present.c" "${TOOLKIT}/src/host/input.c" "${TOOLKIT}/src/host/save_dialog.c"
    "${TOOLKIT}/src/host/render_gl.c" ${PLAYER_UI} "${PLAYER_SETTINGS}")
set(PLAYER_LAUNCHER "${TOOLKIT}/src/host/launcher.c")

# What every game links on the player's machine, with its pack's host
# objects (compile_game.py). Position independent: the on-device link may
# make a position-independent executable.
add_library(player STATIC ${PLAYER_HOST} "${RECOMP}/loader.c" "${RECOMP}/container.c" "${RECOMP}/decode.c")
target_include_directories(player PRIVATE "${RECOMP}")
target_compile_definitions(player PRIVATE HAVE_SDL2)
set_target_properties(player PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(player PRIVATE runtime PkgConfig::SDL)

# The launcher: no pack linked in; it loads each added pack's launcher.so,
# whose references to the settings mechanism resolve against it.
add_executable(launcher ${PLAYER_LAUNCHER} "${TOOLKIT}/src/host/launcher_none.c"
    $<TARGET_OBJECTS:player_imgui> "${PLAYER_SETTINGS}")
target_compile_definitions(launcher PRIVATE HAVE_SDL2)
target_compile_options(launcher PRIVATE -Wall -Wextra -Werror)
target_link_libraries(launcher PRIVATE runtime PkgConfig::SDL ${CMAKE_DL_LIBS})
set_target_properties(launcher PROPERTIES ENABLE_EXPORTS ON)

# The shared host's own checks. Assertions are the fixtures' checks; Release
# must not compile them out.
add_executable(present-tests "${TOOLKIT}/tests/test_present.c" "${TOOLKIT}/src/host/save_dialog.c" "${PLAYER_SETTINGS}")
target_compile_definitions(present-tests PRIVATE HAVE_SDL2)
target_compile_options(present-tests PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(present-tests PRIVATE runtime PkgConfig::SDL)
# The launcher with two packs of its own and one added (a launcher.so in
# packs/gamma/built, as the importer builds one); it includes launcher.c.
add_library(test_pack_plugin MODULE "${TOOLKIT}/tests/test_pack_plugin.c" "${TOOLKIT}/src/host/pack_api.c")
target_include_directories(test_pack_plugin PRIVATE "${TOOLKIT}/include")
set_target_properties(test_pack_plugin PROPERTIES PREFIX "" OUTPUT_NAME launcher
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packs/gamma/built")
add_executable(player-launcher-tests "${TOOLKIT}/tests/test_launcher.c" $<TARGET_OBJECTS:player_imgui> "${PLAYER_SETTINGS}")
target_compile_definitions(player-launcher-tests PRIVATE HAVE_SDL2)
target_compile_options(player-launcher-tests PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(player-launcher-tests PRIVATE runtime PkgConfig::SDL ${CMAKE_DL_LIBS})
set_target_properties(player-launcher-tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(player-launcher-tests test_pack_plugin)
add_executable(savedata-tests "${TOOLKIT}/tests/test_savedata.c")
target_compile_options(savedata-tests PRIVATE -UNDEBUG)
target_link_libraries(savedata-tests PRIVATE runtime)

# Each pack's checks, in a folder of its own.
list(LENGTH PACK_IDS pack_count)
if(pack_count GREATER 0)
    math(EXPR last "${pack_count} - 1")
    foreach(i RANGE ${last})
        list(GET PACK_IDS ${i} id)
        list(GET PACK_ROOTS ${i} PACK)
        list(GET PACK_CMAKES ${i} cmake)
        set(wrapper "${CMAKE_BINARY_DIR}/pack-sources/${id}")
        file(MAKE_DIRECTORY "${wrapper}")
        file(WRITE "${wrapper}/CMakeLists.txt"
            "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_BINARY_DIR}/pack-${id}\")\ninclude(\"${cmake}\")\n")
        add_subdirectory("${wrapper}" "${CMAKE_BINARY_DIR}/pack-build/${id}")
    endforeach()
endif()
