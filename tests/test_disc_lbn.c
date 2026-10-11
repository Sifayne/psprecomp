/* A disc file named by its extent, `disc0:/sce_lbn<sector>_size<bytes>`,
 * against the disc probe's image: ALPHA.BIN is eight sectors from sector 22,
 * and word j of sector s there is (s << 16) | j, so a read names where it
 * came from. */
#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "crypto/sha1.h"
#include <stdio.h>
#include <string.h>

enum { NAME = 0x08804000u, DATA = 0x08810000u };
static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %d: ", __LINE__); \
                           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t call(const char *api, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    psp_cpu.r[PSP_REG_A0] = a; psp_cpu.r[PSP_REG_A1] = b; psp_cpu.r[PSP_REG_A2] = c;
    psp_cpu.r[PSP_REG_A3] = d; psp_cpu.r[PSP_REG_T0] = e;
    psp_hle_call(psp_nid(api));
    return psp_cpu.r[PSP_REG_V0];
}
static int32_t open_path(const char *path) {
    psp_mem_write_block(NAME, path, (uint32_t)strlen(path) + 1);
    return (int32_t)call("sceIoOpen", NAME, 1 /* PSP_O_RDONLY */, 0, 0, 0);
}
static int32_t read_words(int32_t fd, uint32_t bytes) {
    return (int32_t)call("sceIoRead", (uint32_t)fd, DATA, bytes, 0, 0);
}
static uint32_t word(unsigned i) { return psp_read32(DATA + 4 * i); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (psp_mem_init() != 0) return 2;
    psp_hle_init(); psp_cpu_reset(); psp_sched_set_threading(0);
    psp_io_set_umd_image(argv[1]);

    int32_t fd = open_path("disc0:/sce_lbn0x16_size0x4000");
    CHECK(fd > 0, "hex extent opens, got %08X", (unsigned)fd);
    CHECK(read_words(fd, 16) == 16, "hex extent reads");
    CHECK(word(0) == 0x00160000u && word(3) == 0x00160003u,
          "hex extent starts at sector 22, got %08X %08X", word(0), word(3));
    CHECK(call("sceIoLseek32", (uint32_t)fd, 0, 2 /* SEEK_END */, 0, 0) == 0x4000u,
          "hex extent is as long as its size");
    call("sceIoClose", (uint32_t)fd, 0, 0, 0, 0);

    fd = open_path("disc0:/sce_lbn23_size8");
    CHECK(fd > 0, "decimal extent opens, got %08X", (unsigned)fd);
    CHECK(read_words(fd, 64) == 8, "a read stops at the extent's size");
    CHECK(word(0) == 0x00170000u && word(1) == 0x00170001u,
          "decimal extent starts at sector 23, got %08X %08X", word(0), word(1));
    call("sceIoClose", (uint32_t)fd, 0, 0, 0, 0);

    CHECK(open_path("disc0:/sce_lbn0x16") < 0, "an extent without a size does not open");
    CHECK(open_path("disc0:/sce_lbn0x16_size0x10.bin") < 0, "nothing may follow the size");
    CHECK(open_path("disc0:/sce_lbn_size0x10") < 0, "an extent needs a sector");

    /* The raw device counts in sectors, and 2^21 of them are 4 GiB: such a
     * count is the caller's, not a byte count that wraps. Past the image's
     * end the device answers the count it was asked for; inside it, the
     * sectors that are there. */
    fd = open_path("umd0:");
    CHECK(fd > 0, "the raw device opens, got %08X", (unsigned)fd);
    const uint32_t sectors = call("sceIoLseek32", (uint32_t)fd, 0, 2 /* SEEK_END */, 0, 0);
    call("sceIoLseek32", (uint32_t)fd, 100, 0 /* SEEK_SET */, 0, 0);
    uint32_t got = call("sceIoRead", (uint32_t)fd, DATA, 0x200000u, 0, 0);
    CHECK(got == 0x200000u, "past the end, 2^21 sectors answer 2^21, got %08X", got);
    got = call("sceIoLseek32", (uint32_t)fd, 0, 1 /* SEEK_CUR */, 0, 0);
    CHECK(got == 100u + 0x200000u, "and move the position past them, got %08X", got);
    call("sceIoLseek32", (uint32_t)fd, 22, 0 /* SEEK_SET */, 0, 0);
    got = call("sceIoRead", (uint32_t)fd, DATA, 0x200001u, 0, 0);
    CHECK(got == sectors - 22, "from sector 22, 2^21 + 1 sectors read the %u left, got %u",
          sectors - 22, got);
    CHECK(word(0) == 0x00160000u, "from sector 22, got %08X", word(0));
    call("sceIoClose", (uint32_t)fd, 0, 0, 0, 0);

    psp_mem_free();
    printf("disc extents: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
