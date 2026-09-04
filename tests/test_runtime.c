/* Runtime tests — the semantic helpers that recompiled code lowers to.
 *
 * These are the instructions whose C translation is *not* obvious, which is
 * exactly the set where a wrong answer hides for months: division edge cases,
 * the unaligned load/store pairs, and the bitfield ops. All inputs are
 * synthetic; nothing here touches a game.
 */

#include "psprecomp/recomp_rt.h"
#include "psprecomp/clock.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define CHECK_EQ(got, want, label)                             \
    CHECK((got) == (want), "%s: got 0x%08X, want 0x%08X",       \
          (label), (unsigned)(got), (unsigned)(want))

static void test_zero_register(void) {
    /* $zero is hardwired. A recompiled store to it must be a no-op, not a
     * write — otherwise one bad decode corrupts every later read of $zero. */
    psp_cpu_reset();
    psp_set_reg(PSP_REG_ZERO, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_ZERO], 0u, "$zero stays zero");

    psp_set_reg(PSP_REG_A0, 0xDEADBEEF);
    CHECK_EQ(psp_cpu.r[PSP_REG_A0], 0xDEADBEEFu, "$a0 is writable");
}

static void test_clock_mode_report(void) {
    uint64_t guest = UINT64_MAX, wall = UINT64_MAX;
    psp_clock_realtime(0);
    psp_clock_reset();
    CHECK(psp_clock_realtime_stats(&guest, &wall) == 0,
          "virtual clock does not report a wall-time mapping");

    psp_clock_realtime(1);
    psp_clock_reset();
    CHECK(psp_clock_realtime_stats(&guest, &wall) == 1,
          "real-time clock reports its wall-time mapping");
    CHECK(guest == 0, "fresh real-time guest clock starts at zero");
    CHECK(wall < 1000000u, "fresh real-time wall clock has a sane origin");
    psp_clock_realtime(0);
}

static void test_division(void) {
    /* MIPS does not trap on divide-by-zero; HI/LO are architecturally
     * unpredictable. We define them so a recompiled game is deterministic and
     * can be diffed against the oracle rather than depending on host UB. */
    psp_div(100, 7);
    CHECK_EQ(psp_cpu.lo, 14u, "div quotient");
    CHECK_EQ(psp_cpu.hi, 2u,  "div remainder");

    psp_div(0xFFFFFF9Cu /* -100 */, 7);
    CHECK_EQ(psp_cpu.lo, (uint32_t)-14, "signed div quotient");
    CHECK_EQ(psp_cpu.hi, (uint32_t)-2,  "signed div remainder");

    psp_div(5, 0);
    CHECK_EQ(psp_cpu.hi, 5u, "div by zero leaves the dividend in hi");

    /* INT_MIN / -1 overflows in C — this must not trap or crash. */
    psp_div(0x80000000u, 0xFFFFFFFFu);
    CHECK_EQ(psp_cpu.lo, 0x80000000u, "INT_MIN / -1 quotient");
    CHECK_EQ(psp_cpu.hi, 0u,           "INT_MIN / -1 remainder");

    psp_divu(0xFFFFFFFFu, 2);
    CHECK_EQ(psp_cpu.lo, 0x7FFFFFFFu, "unsigned div treats the operand as unsigned");

    psp_divu(5, 0);
    CHECK_EQ(psp_cpu.lo, 0xFFFFFFFFu, "unsigned div by zero");
}

static void test_multiply(void) {
    psp_mult(0xFFFFFFFFu, 0xFFFFFFFFu);   /* -1 * -1 == 1 */
    CHECK_EQ(psp_cpu.lo, 1u, "signed mult lo");
    CHECK_EQ(psp_cpu.hi, 0u, "signed mult hi");

    psp_multu(0xFFFFFFFFu, 0xFFFFFFFFu);  /* 0xFFFFFFFE00000001 */
    CHECK_EQ(psp_cpu.lo, 0x00000001u, "unsigned mult lo");
    CHECK_EQ(psp_cpu.hi, 0xFFFFFFFEu, "unsigned mult hi");

    /* madd accumulates into the existing HI/LO pair. */
    psp_cpu.hi = 0; psp_cpu.lo = 10;
    psp_madd(3, 4);
    CHECK_EQ(psp_cpu.lo, 22u, "madd accumulates");

    psp_cpu.hi = 0; psp_cpu.lo = 100;
    psp_msub(3, 4);
    CHECK_EQ(psp_cpu.lo, 88u, "msub subtracts");
}

static void test_shifts(void) {
    /* C leaves shift-by->=32 undefined; MIPS masks the amount to 5 bits. */
    CHECK_EQ(psp_sll(1, 32), 1u,  "sll by 32 masks to 0");
    CHECK_EQ(psp_srl(2, 33), 1u,  "srl by 33 masks to 1");
    CHECK_EQ(psp_sra(0x80000000u, 31), 0xFFFFFFFFu, "sra sign-extends");
    CHECK_EQ(psp_srl(0x80000000u, 31), 0x00000001u, "srl does not sign-extend");

    CHECK_EQ(psp_rotr(0x12345678u, 8),  0x78123456u, "rotr by 8");
    CHECK_EQ(psp_rotr(0x12345678u, 0),  0x12345678u, "rotr by 0 is identity");
    CHECK_EQ(psp_rotr(0x12345678u, 32), 0x12345678u, "rotr by 32 is identity");
}

static void test_bitops(void) {
    CHECK_EQ(psp_clz(0x00000000u), 32u, "clz of zero");
    CHECK_EQ(psp_clz(0x80000000u), 0u,  "clz of the top bit");
    CHECK_EQ(psp_clz(0x00000001u), 31u, "clz of the bottom bit");
    CHECK_EQ(psp_clo(0xFFFFFFFFu), 32u, "clo of all-ones");
    CHECK_EQ(psp_clo(0xF0000000u), 4u,  "clo of a nibble");

    CHECK_EQ(psp_ext(0x12345678u, 4, 8),  0x67u,       "ext middle bits");
    CHECK_EQ(psp_ext(0x12345678u, 0, 32), 0x12345678u, "ext full word");
    CHECK_EQ(psp_ins(0xFFFFFFFFu, 0x0u, 8, 8), 0xFFFF00FFu, "ins clears a byte");
    CHECK_EQ(psp_ins(0x00000000u, 0xABu, 8, 8), 0x0000AB00u, "ins sets a byte");

    CHECK_EQ(psp_seb(0x000000FFu), 0xFFFFFFFFu, "seb sign-extends 0xFF");
    CHECK_EQ(psp_seb(0x0000007Fu), 0x0000007Fu, "seb leaves 0x7F positive");
    CHECK_EQ(psp_seh(0x0000FFFFu), 0xFFFFFFFFu, "seh sign-extends 0xFFFF");

    CHECK_EQ(psp_wsbh(0x12345678u), 0x34127856u, "wsbh swaps within halfwords");
    CHECK_EQ(psp_wsbw(0x12345678u), 0x78563412u, "wsbw reverses the word");

    CHECK_EQ(psp_bitrev(0x00000001u), 0x80000000u, "bitrev of bit 0");
    CHECK_EQ(psp_bitrev(0x80000000u), 0x00000001u, "bitrev of bit 31");
    CHECK_EQ(psp_bitrev(psp_bitrev(0x12345678u)), 0x12345678u, "bitrev is its own inverse");

    CHECK_EQ(psp_max(5, (uint32_t)-3), 5u,           "max is signed");
    CHECK_EQ(psp_min(5, (uint32_t)-3), (uint32_t)-3, "min is signed");
    CHECK_EQ(psp_slt((uint32_t)-1, 1), 1u, "slt is signed");
    CHECK_EQ(psp_sltu((uint32_t)-1, 1), 0u, "sltu is unsigned");
}

static void test_memory(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    const uint32_t tracked = PSP_VRAM_BASE + 0x1000u;
    const uint32_t nearby  = tracked + 0x80u;
    const uint32_t remote  = tracked + 0x1000u;
    const uint64_t initial_serial = psp_mem_write_serial();
    CHECK(initial_serial != 0, "memory reset has a cache-visible serial");
    CHECK(psp_mem_range_generation(tracked, 4) == 0,
          "fresh range has generation zero");

    psp_write32(tracked, 0xAABBCCDDu);
    const uint64_t first_generation = psp_mem_range_generation(tracked, 4);
    CHECK(first_generation > initial_serial, "scalar store marks its range");
    CHECK(psp_mem_range_generation(nearby, 4) == first_generation,
          "nearby bytes conservatively share a dirty granule");
    CHECK(psp_mem_range_generation(remote, 4) == 0,
          "a remote range stays clean");
    CHECK(psp_mem_range_generation(0x44001000u, 4) == first_generation,
          "cache mirror shares the write generation");

    const uint8_t block[4] = { 1, 2, 3, 4 };
    CHECK(psp_mem_write_block(remote, block, sizeof block) == 0,
          "block write succeeds");
    CHECK(psp_mem_range_generation(remote, sizeof block) > first_generation,
          "block write marks its destination");

    const uint32_t edge = tracked + 0xFFu;
    psp_write16(edge, 0x7788u);
    const uint64_t edge_generation = psp_mem_range_generation(edge, 2);
    CHECK(edge_generation == psp_mem_range_generation(edge, 1) &&
          edge_generation == psp_mem_range_generation(edge + 1, 1),
          "a write crossing a granule marks both sides");

    const uint64_t before_raw = psp_mem_write_serial();
    void *raw = psp_mem_ptr(remote + 0x1000u, 16);
    CHECK(raw != NULL, "raw write destination maps");
    if (raw) {
        memset(raw, 0x5A, 16);
        CHECK(psp_mem_write_serial() == before_raw,
              "raw pointer write is invisible until explicitly marked");
        psp_mem_mark_write(remote + 0x1000u, 16);
        CHECK(psp_mem_write_serial() > before_raw,
              "explicit mark advances the write serial");
    }

    const uint64_t before_read = psp_mem_write_serial();
    (void)psp_read32(tracked);
    CHECK(psp_mem_write_serial() == before_read,
          "reads do not change the write serial");
    CHECK(psp_mem_range_generation(0x01000000u, 4) == 0,
          "unmapped ranges have no generation");

    CHECK(psp_mem_map_module(0x00100000u, 0x1000u) == 0,
          "module region maps for generation test");
    psp_write8(0x00100100u, 0x42u);
    CHECK(psp_mem_range_generation(0x00100100u, 1) != 0,
          "module image writes carry generations too");

    psp_write32(0x08800000u, 0x12345678u);
    CHECK_EQ(psp_read32(0x08800000u), 0x12345678u, "round-trip a word");
    CHECK_EQ(psp_read8 (0x08800000u), 0x78u, "little-endian byte 0");
    CHECK_EQ(psp_read8 (0x08800003u), 0x12u, "little-endian byte 3");
    CHECK_EQ(psp_read16(0x08800000u), 0x5678u, "little-endian halfword");

    /* The three cache-behaviour mirrors must alias the same storage. */
    CHECK_EQ(psp_read32(0x48800000u), 0x12345678u, "uncached mirror aliases RAM");
    CHECK_EQ(psp_read32(0x88800000u), 0x12345678u, "kernel mirror aliases RAM");

    /* Unmapped access is counted, not fatal — the counter is how a recompiled
     * game tells you the analysis missed something. */
    uint64_t before = psp_mem_bad_access;
    CHECK_EQ(psp_read32(0x00000000u), 0u, "unmapped read returns zero");
    CHECK(psp_mem_bad_access == before + 1, "unmapped read is counted");

    /* An access straddling the end of a region must be rejected outright
     * rather than reading past the allocation. */
    before = psp_mem_bad_access;
    (void)psp_read32(PSP_VRAM_BASE + PSP_VRAM_SIZE - 2);
    CHECK(psp_mem_bad_access == before + 1, "straddling read is rejected");

    const uint64_t before_reset = psp_mem_write_serial();
    psp_mem_free();
    CHECK(psp_mem_init() == 0, "memory reinitialises");
    CHECK(psp_mem_write_serial() > before_reset,
          "memory reset invalidates surviving external caches");
    CHECK(psp_mem_range_generation(tracked, 4) == 0,
          "memory reset restores pristine range generations");
    psp_mem_free();
}

static void test_unaligned(void) {
    CHECK(psp_mem_init() == 0, "memory init");

    /* Lay down a known byte pattern: 00 11 22 33 44 55 66 77. */
    for (uint32_t i = 0; i < 8; i++)
        psp_write8(0x08800000u + i, (uint8_t)(i * 0x11));

    /* The LE compiler idiom for an unaligned 32-bit load at address A is
     *     lwr rt, A       lwl rt, A+3
     * Bytes 1..4 are 11 22 33 44, so the loaded word must be 0x44332211. */
    uint32_t rt = 0xCCCCCCCCu;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0x44332211u, "unaligned load via lwr+lwl");

    /* The store idiom, mirrored. Writing 0xAABBCCDD at address 1 must leave
     * bytes 1..4 as DD CC BB AA and must not disturb bytes 0, 5, 6, 7. */
    psp_swr(0xAABBCCDDu, 0x08800001u);
    psp_swl(0xAABBCCDDu, 0x08800004u);
    CHECK_EQ(psp_read8(0x08800000u), 0x00u, "swl/swr left byte 0 alone");
    CHECK_EQ(psp_read8(0x08800001u), 0xDDu, "unaligned store byte 1");
    CHECK_EQ(psp_read8(0x08800002u), 0xCCu, "unaligned store byte 2");
    CHECK_EQ(psp_read8(0x08800003u), 0xBBu, "unaligned store byte 3");
    CHECK_EQ(psp_read8(0x08800004u), 0xAAu, "unaligned store byte 4");
    CHECK_EQ(psp_read8(0x08800005u), 0x55u, "swl/swr left byte 5 alone");

    /* Round-trip: read back what we just stored. */
    rt = 0;
    rt = psp_lwr(rt, 0x08800001u);
    rt = psp_lwl(rt, 0x08800004u);
    CHECK_EQ(rt, 0xAABBCCDDu, "unaligned store then load round-trips");

    /* An aligned address must behave exactly like a plain lw. */
    psp_write32(0x08800010u, 0xFEEDFACEu);
    rt = 0;
    rt = psp_lwr(rt, 0x08800010u);
    rt = psp_lwl(rt, 0x08800013u);
    CHECK_EQ(rt, 0xFEEDFACEu, "the unaligned pair degrades to lw when aligned");

    psp_mem_free();
}

int main(void) {
    test_zero_register();
    test_clock_mode_report();
    test_division();
    test_multiply();
    test_shifts();
    test_bitops();
    test_memory();
    test_unaligned();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all runtime checks passed\n");
    return 0;
}
