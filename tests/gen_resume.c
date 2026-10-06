/* Emits the resume test's program (resume_program.h) with --resume, into the
 * directory given, for test_resume.c to compile: gen_resume <outdir>. */
#include "analyze.h"
#include "container.h"
#include "emit.h"
#include "resume_program.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: gen_resume <outdir>\n"); return 2; }
    uint8_t code[RP_WORDS * 4];
    for (int i = 0; i < RP_WORDS; i++)
        for (int b = 0; b < 4; b++) code[i * 4 + b] = (uint8_t)(RP_CODE[i] >> (8 * b));

    a_analysis an;
    memset(&an, 0, sizeof an);
    an.code = code;
    an.base = RP_BASE;
    an.size = (uint32_t)sizeof code;
    an.stub_addr = RP_STUB;
    an.stub_size = 8;
    const uint32_t seeds[1] = { RP_MAIN };
    if (a_discover(&an, seeds, 1, 1) != 0) { fprintf(stderr, "gen_resume: discovery failed\n"); return 1; }

    psp_import_entry imp = { RP_STUB, RP_NID, "ResumeTest" };
    emit_opts o = {0};
    o.outdir = argv[1];
    o.prefix = "t_resume";
    o.module = "resume test";
    o.imports = &imp;
    o.nimports = 1;
    o.resume = 1;
    const int rc = a_emit(&an, &o);
    a_analysis_free(&an);
    return rc ? 1 : 0;
}
