# Tests are self-contained: no game data, no ROMs, no disc images. Every input
# is either a hand-written instruction encoding or a synthetic buffer built in
# the test itself.

add_executable(test_decode test_decode.c)
target_link_libraries(test_decode PRIVATE allegrex_core)
add_test(NAME decode COMMAND test_decode)

add_executable(test_settings test_settings.c)
target_link_libraries(test_settings PRIVATE psprecomp_settings)
# Its checks are asserts; Release must not compile them out.
if(MSVC)
    target_compile_options(test_settings PRIVATE /UNDEBUG)
else()
    target_compile_options(test_settings PRIVATE -UNDEBUG)
endif()
add_test(NAME settings COMMAND test_settings)

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE psprecomp)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_container test_container.c)
target_link_libraries(test_container PRIVATE allegrex_core)
add_test(NAME container COMMAND test_container)

# Published standard vectors only (FIPS-197, SP 800-38A, RFC 4493) — no PSP
# key material, no game data.
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE allegrex_core)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_kirk test_kirk.c)
target_link_libraries(test_kirk PRIVATE allegrex_core)
add_test(NAME kirk COMMAND test_kirk)

add_executable(test_emit test_emit.c)
target_link_libraries(test_emit PRIVATE allegrex_core)
add_test(NAME emit COMMAND test_emit)

add_executable(test_hle test_hle.c)
target_link_libraries(test_hle PRIVATE psprecomp allegrex_core)
add_test(NAME hle COMMAND test_hle)

add_executable(test_directory_sdk test_directory_sdk.c)
target_link_libraries(test_directory_sdk PRIVATE psprecomp allegrex_core)
add_test(NAME directory_sdk COMMAND test_directory_sdk)

add_executable(test_interrupt test_interrupt.c)
target_link_libraries(test_interrupt PRIVATE psprecomp)
add_test(NAME interrupt COMMAND test_interrupt
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/intr/observed.txt)
set_tests_properties(interrupt PROPERTIES TIMEOUT 10)

add_executable(test_ge_callback test_ge_callback.c)
target_link_libraries(test_ge_callback PRIVATE psprecomp allegrex_core)
add_test(NAME ge_callback COMMAND test_ge_callback
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/ge/observed.txt)
set_tests_properties(ge_callback PROPERTIES TIMEOUT 10)

add_executable(test_ge_sync test_ge_sync.c)
target_link_libraries(test_ge_sync PRIVATE psprecomp)
add_test(NAME ge_sync COMMAND test_ge_sync)
add_test(NAME ge_sync_capture COMMAND test_ge_sync sync-capture.gcap)

add_executable(test_texture_upload test_texture_upload.c)
target_link_libraries(test_texture_upload PRIVATE psprecomp)
add_test(NAME texture_upload COMMAND test_texture_upload)

add_executable(test_thread_lifecycle test_thread_lifecycle.c)
target_link_libraries(test_thread_lifecycle PRIVATE psprecomp allegrex_core)
add_test(NAME thread_lifecycle COMMAND test_thread_lifecycle)
set_tests_properties(thread_lifecycle PROPERTIES TIMEOUT 10)

add_executable(test_kernel_observed test_kernel_observed.c)
target_link_libraries(test_kernel_observed PRIVATE psprecomp allegrex_core)
add_test(NAME kernel_observed COMMAND test_kernel_observed
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/kernel/observed.txt)
set_tests_properties(kernel_observed PROPERTIES TIMEOUT 10)

# sceMpeg follows mpegprobe (fw 6.60) where it measured: the handle is the
# buffer, the ring's word after the struct is the caller's gp, InitAu leaves
# the timestamps and sets size 0, and the stream-size query checks the size
# word, not the magic. provenance/mpeg/observed.txt, recorded under an
# emulator, differs in exactly those 176 checks, so its checker is built for
# reference but not run.
add_executable(test_ctrl_observed test_ctrl_observed.c)
target_link_libraries(test_ctrl_observed PRIVATE psprecomp allegrex_core)
add_test(NAME ctrl_observed COMMAND test_ctrl_observed
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/ctrl/observed.txt)
set_tests_properties(ctrl_observed PROPERTIES TIMEOUT 10)

add_executable(test_mpeg_observed test_mpeg_observed.c)
target_link_libraries(test_mpeg_observed PRIVATE psprecomp allegrex_core)

add_executable(test_vfpu test_vfpu.c)
target_link_libraries(test_vfpu PRIVATE psprecomp)
add_test(NAME vfpu COMMAND test_vfpu)

add_executable(test_ctors test_ctors.c)
target_link_libraries(test_ctors PRIVATE psprecomp)
add_test(NAME ctors COMMAND test_ctors)

# The handoff token. Guest thread bodies are plain C functions registered in
# the dispatch table, so no module and no game data is involved.
add_executable(test_sched test_sched.c)
target_link_libraries(test_sched PRIVATE psprecomp)
add_test(NAME sched COMMAND test_sched)

add_executable(test_raster test_raster.c)
target_link_libraries(test_raster PRIVATE psprecomp)
add_test(NAME raster COMMAND test_raster)

# The interpreter oracle. Hand-written encodings only; the delay-slot cases are
# the ones that matter, since that is where interp.c stops sharing code with
# the emitter.
add_executable(test_interp test_interp.c)
target_link_libraries(test_interp PRIVATE allegrex_core)
add_test(NAME interp COMMAND test_interp)

add_executable(test_ctrl_replay test_ctrl_replay.c)
target_link_libraries(test_ctrl_replay PRIVATE psprecomp)
add_test(NAME ctrl_replay COMMAND test_ctrl_replay)

add_executable(test_view_log test_view_log.c)
target_link_libraries(test_view_log PRIVATE psprecomp)
add_test(NAME view_log COMMAND test_view_log view-log-test.txt)

add_executable(test_ramsnap test_ramsnap.c)
target_link_libraries(test_ramsnap PRIVATE psprecomp)
add_test(NAME ramsnap COMMAND test_ramsnap ramsnap-test)

add_executable(test_watch_arm test_watch_arm.c)
target_link_libraries(test_watch_arm PRIVATE psprecomp)
add_test(NAME watch_arm COMMAND test_watch_arm)

add_executable(test_ge_capture test_ge_capture.c)
target_link_libraries(test_ge_capture PRIVATE psprecomp)
add_test(NAME ge_capture_polls COMMAND test_ge_capture polls capture-polls)
add_test(NAME ge_capture_legacy COMMAND test_ge_capture legacy capture-legacy.gcap)
foreach(value "2,2" "5,2" "2," "-1" "2,no" "4294967296")
    string(MAKE_C_IDENTIFIER "${value}" id)
    add_test(NAME ge_capture_invalid_${id}
             COMMAND test_ge_capture invalid capture-invalid-${id} "${value}")
endforeach()

add_executable(test_savedata test_savedata.c)
target_link_libraries(test_savedata PRIVATE psprecomp)
if(MSVC)
    target_compile_options(test_savedata PRIVATE /UNDEBUG)
else()
    target_compile_options(test_savedata PRIVATE -UNDEBUG)
endif()
add_test(NAME savedata COMMAND test_savedata)

# The random unit follows the hardware fit in src/vfpu.c (vfpuprobe, fw 6.60).
# provenance/vrnd-observed.txt was recorded under an emulator and disagrees
# with it on the carry and on vector lane order, so the checker is built for
# reference but not run.
# Savedata's result codes follow saveprobe (fw 6.60) in src/hle/utility.c.
# provenance/savedata/observed.txt was recorded under an emulator and
# disagrees with the hardware in 19 of its checks, so the checker is built
# for reference but not run.
add_executable(test_savedata_observed test_savedata_observed.c)
target_link_libraries(test_savedata_observed PRIVATE psprecomp)

add_executable(test_mpeg_au test_mpeg_au.c)
target_link_libraries(test_mpeg_au PRIVATE psprecomp)
add_test(NAME mpeg_au COMMAND test_mpeg_au)
set_tests_properties(mpeg_au PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_vrnd_observed test_vrnd_observed.c)
target_link_libraries(test_vrnd_observed PRIVATE psprecomp)

add_executable(test_vertex_observed test_vertex_observed.c)
target_link_libraries(test_vertex_observed PRIVATE psprecomp)
add_test(NAME vertex_observed COMMAND test_vertex_observed
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/vertices/observed.txt)

add_executable(test_pixels_observed test_pixels_observed.c)
target_link_libraries(test_pixels_observed PRIVATE psprecomp)
add_test(NAME pixels_observed COMMAND test_pixels_observed
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/pixels/observed.txt)

add_executable(test_disc_observed test_disc_observed.c)
target_link_libraries(test_disc_observed PRIVATE psprecomp allegrex_core)
add_test(NAME disc_observed COMMAND test_disc_observed
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/disc/observed.txt
         ${CMAKE_CURRENT_SOURCE_DIR}/provenance/disc/probe.iso)

# The SDL2 host layer, when it is built. The dummy video driver keeps it off
# the screen; with no system font the controller half skips.
if(PSPRECOMP_HOST)
    add_executable(test_save_dialog test_save_dialog.c)
    target_link_libraries(test_save_dialog PRIVATE psprecomp_host)
    add_test(NAME save_dialog COMMAND test_save_dialog)
    set_tests_properties(save_dialog PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Includes present.c for its internals, so it links the host layer only
    # for the save dialog. The quit shortcuts alone, then the real SDL loop
    # closed by the keyboard and by a virtual controller.
    add_executable(test_present test_present.c)
    target_link_libraries(test_present PRIVATE psprecomp_host)
    if(MSVC)
        target_compile_options(test_present PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_present PRIVATE -UNDEBUG)
    endif()
    add_test(NAME present COMMAND test_present)
    add_test(NAME present_keyboard COMMAND test_present keyboard)
    add_test(NAME present_controller COMMAND test_present controller)
    set_tests_properties(present present_keyboard present_controller PROPERTIES
        TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
