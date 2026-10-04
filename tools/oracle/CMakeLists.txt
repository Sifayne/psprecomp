# oracle_diff is built by a game, with that game's generated C: it calls the
# generated psp_recomp_register and brings its own main(), psp_syscall and
# psp_unimplemented. Here it is only compiled, so that a runtime change that
# breaks it fails this repository's build rather than a game's. POSIX only
# (sigsetjmp, setitimer, sigaltstack).
add_library(psprecomp_oracle_check OBJECT oracle_diff.c)
target_link_libraries(psprecomp_oracle_check PRIVATE allegrex_core)
