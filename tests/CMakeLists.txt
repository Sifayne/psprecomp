# Tests are self-contained: no game data, no ROMs, no disc images. Every input
# is either a hand-written instruction encoding or a synthetic buffer built in
# the test itself.

add_executable(test_decode test_decode.c)
target_link_libraries(test_decode PRIVATE allegrex_core)
add_test(NAME decode COMMAND test_decode)

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
