/* Function discovery. See analyze.h. */

#include "analyze.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a growable uint32 list ---------------------------------------------- */

typedef struct { uint32_t *v; int n, cap; } u32list;

static int u32_push(u32list *l, uint32_t x) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        uint32_t *v = (uint32_t *)realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = x;
    return 0;
}

static int u32_contains(const u32list *l, uint32_t x) {
    for (int i = 0; i < l->n; i++) if (l->v[i] == x) return 1;
    return 0;
}

/* ---- per-word bookkeeping ------------------------------------------------ */

#define SEEN_CODE  0x01   /* decoded as an instruction */
#define SEEN_ENTRY 0x02   /* known function entry */

static uint32_t word_index(const a_analysis *an, uint32_t addr) {
    return (addr - an->base) >> 2;
}

int a_in_range(const a_analysis *an, uint32_t addr) {
    return addr >= an->base && addr < an->base + an->size && (addr & 3) == 0;
}

static int is_import_stub(const a_analysis *an, uint32_t addr) {
    return an->stub_size && addr >= an->stub_addr &&
           addr < an->stub_addr + an->stub_size;
}

static uint32_t fetch(const a_analysis *an, uint32_t addr) {
    const uint8_t *p = an->code + (addr - an->base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- the walk ------------------------------------------------------------ */

typedef struct {
    a_analysis *an;
    uint8_t    *seen;
    /* Every address known to be a function entry, marked before any walking
     * starts. Knowing the full entry set up front is what lets a walk tell a
     * tail call from a local jump, and lets it stop when it runs off the end
     * of one function into the start of the next. */
    uint8_t    *entry_map;
    /* Ownership of each word, and the entry of the function currently being
     * traced. Recorded as the walk proceeds so the emitter knows exactly which
     * instructions belong to which function. */
    uint32_t   *owner;
    uint32_t    cur_owner;
    u32list    *func_queue;
    u32list    *imports;
    u32list    *indirects;
    /* Pairs of function entries that a branch showed to be one function.
     *
     * A branch into a block another walk already claimed means both walks are
     * inside the same original function -- whichever ran first simply took the
     * block. Recorded here as (this function, the claimant) and reconciled
     * once all walking is done. */
    u32list    *merges;
} walk_ctx;

static int is_known_entry(const walk_ctx *c, uint32_t addr) {
    if (!a_in_range(c->an, addr)) return 0;
    return c->entry_map[word_index(c->an, addr)] != A_ENTRY_NONE;
}

/* An entry the module demonstrably enters, as opposed to one a pointer merely
 * suggests. See A_ENTRY_SOFT in analyze.h for why the difference matters.
 *
 * Only the merge sites ask this. Walk boundaries still stop at *any* entry --
 * splitting on a soft one and letting the merge fold it back is what keeps
 * ownership non-overlapping, and the merge already records a folded entry in
 * split_entry so the emitter keeps a label and a dispatch thunk for it. */
static int is_hard_entry(const walk_ctx *c, uint32_t addr) {
    if (!a_in_range(c->an, addr)) return 0;
    return c->entry_map[word_index(c->an, addr)] == A_ENTRY_HARD;
}

/* Record a call target. Import thunks are boundaries, not functions to walk
 * into: they get patched at load time and their contents in the file are
 * meaningless. Everything else is a function entry. */
static void note_call(walk_ctx *c, uint32_t target) {
    if (is_import_stub(c->an, target)) {
        if (!u32_contains(c->imports, target)) u32_push(c->imports, target);
        return;
    }
    if (!a_in_range(c->an, target)) return;
    u32_push(c->func_queue, target);
}

/* Walk one function from `entry`, filling in `out`. Returns 0 if anything was
 * decoded, -1 if the entry was unusable. */
static int trace_function(walk_ctx *c, uint32_t entry, a_func *out) {
    a_analysis *an = c->an;
    if (!a_in_range(an, entry)) return -1;

    u32list blocks = { 0 };
    if (u32_push(&blocks, entry) != 0) return -1;

    uint32_t end = entry;
    uint32_t lo  = entry;
    uint32_t insns = 0;
    unsigned has_return = 0, has_indirect = 0, has_vfpu = 0;

    /* Instructions visited by *this* function, so a block shared with an
     * earlier function does not get walked twice here but is still counted
     * once globally. */
    while (blocks.n) {
        uint32_t a = blocks.v[--blocks.n];

        while (a_in_range(an, a)) {
            uint32_t idx = word_index(an, a);
            if (c->seen[idx] & SEEN_CODE) {
                /* Already walked. If somebody *else* walked it, the two walks
                 * are inside one original function and this is the same shared
                 * block the branch and backward-jump paths below merge -- it
                 * just arrived by falling through rather than by a transfer,
                 * so neither of them ever saw it.
                 *
                 * Whether the split is visible here or at the entry test below
                 * is pure walk order: if the seed was traced first it claimed
                 * these words and we stop here, and if it was not we stop
                 * there. Both are the same situation and both have to record
                 * it, or a shared epilogue stays split depending on which
                 * order the queue happened to produce.
                 *
                 * A hard entry is left alone: falling into one is an ordinary
                 * function ending without a visible return, which is exactly
                 * what the split is for. */
                if (c->owner[idx] != A_NO_OWNER && c->owner[idx] != c->cur_owner &&
                    !is_hard_entry(c, a)) {
                    u32_push(c->merges, c->cur_owner);
                    u32_push(c->merges, c->owner[idx]);
                }
                break;
            }

            /* Arriving at a different function's entry means we walked off the
             * end of this one — a tail call, or a function that ends without a
             * visible return. Stop; that address is walked as its own function.
             * Without this a single trace swallows every function that follows
             * it in address order. */
            if (a != entry && is_known_entry(c, a)) {
                /* ...unless the only evidence for that entry is a pointer.
                 *
                 * Falling through into an address is the plainest possible
                 * proof that it is not a function: execution reaches it from
                 * the instruction above, with the caller's frame already set
                 * up. A stored pointer that happens to land here is a guess,
                 * and the guess is now contradicted.
                 *
                 * This is the same shared-block situation the branch and
                 * backward-jump paths below handle, but neither of them sees
                 * it -- both fire on a *transfer* into a claimed block, and a
                 * fall-through is neither. So it was recorded nowhere and the
                 * split stood.
                 *
                 * That split is what a shared epilogue looks like afterwards:
                 * the continuation holds `lw $ra` / `jr $ra` / `addiu $sp` but
                 * not the prologue that reserved the frame, so it releases
                 * stack it never took and the $sp invariant cannot hold for
                 * it. Merging is what puts the two halves back together;
                 * merge_shared records the folded entry in split_entry, so it
                 * keeps its label and stays reachable through dispatch for the
                 * pointer that pointed here in the first place. */
                if (!is_hard_entry(c, a)) {
                    u32_push(c->merges, c->cur_owner);
                    u32_push(c->merges, a);
                }
                break;
            }

            a_insn in;
            a_decode(fetch(an, a), a, &in);
            c->seen[idx] |= SEEN_CODE;
            c->owner[idx] = c->cur_owner;
            insns++;
            an->insns++;
            if (a + 4 > end) end = a + 4;
            if (a < lo) lo = a;

            if (in.op == A_INVALID) {
                /* Almost always data reached by a bad path. Stop this trace
                 * rather than manufacturing instructions out of it. */
                an->invalid++;
                break;
            }
            if (in.op == A_VFPU_UNKNOWN) { an->vfpu++; has_vfpu = 1; }

            /* Every branch and jump has a delay slot that executes before
             * control transfers, so it is always part of this function and is
             * always visited — including on paths that leave here. */
            if (in.has_delay_slot) {
                uint32_t d = a + 4;
                if (a_in_range(an, d)) {
                    uint32_t didx = word_index(an, d);
                    if (!(c->seen[didx] & SEEN_CODE)) {
                        a_insn din;
                        a_decode(fetch(an, d), d, &din);
                        c->seen[didx] |= SEEN_CODE;
                        c->owner[didx] = c->cur_owner;
                        insns++;
                        an->insns++;
                        if (din.op == A_VFPU_UNKNOWN) { an->vfpu++; has_vfpu = 1; }
                        if (din.op == A_INVALID) an->invalid++;
                    }
                    if (d + 4 > end) end = d + 4;
                    if (d < lo) lo = d;
                }
            }

            /* A branch or tail-jump into the import thunks is a call into
             * firmware by another name. It cannot be followed — the thunks are
             * patched at load time and their file contents are meaningless —
             * but it must be *recorded*, because the emitter decides what is an
             * import purely by address range. If the walk does not record it,
             * the generated code references psp_import_<addr> and the imports
             * file never defines it: a link error, and one that only shows up
             * on a module whose imports are not all reached by `jal`.
             *
             * The stub region sits immediately after `.text`, so it is outside
             * the analysed range and the branch/jump paths below skip it
             * silently. Hence the explicit check here. */
            if (in.has_target && !in.is_indirect && is_import_stub(an, in.target)) {
                if (!u32_contains(c->imports, in.target))
                    u32_push(c->imports, in.target);
            }

            if (in.is_call) {
                /* jal / jalr / the *al REGIMM forms. Direct calls give us a
                 * new function; indirect ones we cannot resolve statically. */
                if (in.has_target && !in.is_indirect) note_call(c, in.target);
                else if (in.is_indirect) has_indirect = 1;
                a += 8;                     /* past the delay slot; calls return */
                continue;
            }

            if (in.is_return) { has_return = 1; break; }

            if (in.is_indirect) {
                /* `jr $rN` — a computed jump, usually a switch table. We
                 * cannot follow it without resolving the table, so record the
                 * site and stop. These are the concrete targets for jump-table
                 * analysis; they are counted rather than silently dropped. */
                has_indirect = 1;
                if (!u32_contains(c->indirects, a)) u32_push(c->indirects, a);
                break;
            }

            if (in.is_branch) {
                /* Conditional: the target is another block of this function,
                 * and execution also falls through past the delay slot. */
                if (in.has_target && a_in_range(an, in.target)) {
                    uint32_t ti = word_index(an, in.target);
                    if ((c->seen[ti] & SEEN_CODE) && c->entry_map[ti] != A_ENTRY_HARD) {
                        /* Already claimed by an earlier walk, and not an entry.
                         * Two functions therefore share this block, which is a
                         * walk-order artefact rather than a property of the
                         * code -- whichever was traced first took it.
                         *
                         * These are one function, so they are merged into one.
                         * Promoting the target to an entry instead makes it
                         * reachable, but at a price that is only visible at run
                         * time: the branch becomes a C *call*, and when the two
                         * blocks form a loop -- which is exactly what a branch
                         * between them usually means -- every iteration of a
                         * loop the guest runs in constant stack costs the host
                         * a frame, until the stack is gone. */
                        u32_push(c->merges, c->cur_owner);
                        u32_push(c->merges, c->owner[ti]);
                    } else {
                        /* Refused because the target is a known entry. If the
                         * only evidence for that entry is a pointer that
                         * happened to decode, this is a loop being cut in half
                         * on a guess -- counted so the cost is visible before
                         * anything is changed about it. */
                        if ((c->seen[ti] & SEEN_CODE) && c->owner[ti] != c->cur_owner) {
                            an->nsuppressed++;
                            if (c->entry_map[ti] == A_ENTRY_SOFT) an->nsuppressed_soft++;
                        }
                        u32_push(&blocks, in.target);
                    }
                }
                a += 8;
                continue;
            }

            if (in.is_jump) {
                /* Unconditional `j`. MIPS compilers emit `b` — which is
                 * `beq $zero, $zero` and decodes as a branch — for local
                 * unconditional flow inside a function. A `j` is therefore
                 * almost always either a tail call or a loop back-edge, and
                 * the direction tells them apart:
                 *
                 *   backward -> a loop back-edge, a block of this function
                 *   forward  -> a tail call, the entry of another function
                 *
                 * Observed in this module as two-instruction thunks:
                 *     j 0x00091E38 ; addiu $a0, $zero, 2
                 * Treating that forward `j` as local flow pulls the callee
                 * into the thunk and, transitively, swallows most of .text
                 * into one 121 KB "function".
                 *
                 * Over-splitting is the safe direction to err: a wrongly split
                 * function is still correct code reached through the dispatch
                 * table, whereas a wrongly merged one has bogus boundaries. */
                if (in.has_target && a_in_range(an, in.target)) {
                    uint32_t bi = word_index(an, in.target);
                    if (in.target <= a && a != entry && !is_hard_entry(c, in.target)) {
                        /* Backward `j` -- a loop back-edge into this function's
                         * own blocks, unless another walk already claimed the
                         * target. Then the two functions share a block, exactly
                         * as for a conditional branch, and for the same reason:
                         * walk order, not a property of the code.
                         *
                         * The emitter cannot express a jump into another C
                         * function's middle. It degrades to a dispatch, which
                         * misses because the address was never made an entry --
                         * observed as `j 0x0000E588` inside the allocator,
                         * emitted as a dispatch to an undiscovered address
                         * and missing at run time.
                         *
                         * The branch path above has always promoted these. The
                         * jump path did not, so a whole class of shared block
                         * stayed unreachable. */
                        if ((c->seen[bi] & SEEN_CODE) && c->entry_map[bi] != A_ENTRY_HARD) {
                            u32_push(c->merges, c->cur_owner);
                            u32_push(c->merges, c->owner[bi]);
                        } else {
                            u32_push(&blocks, in.target);
                        }
                    } else {
                        /* A backward `j` lands here only when the target is
                         * already a known entry -- the same refusal as the
                         * branch path, so it is counted the same way. A forward
                         * one is a tail call and is not a shared block at all. */
                        if (in.target <= a && a != entry &&
                            (c->seen[bi] & SEEN_CODE) && c->owner[bi] != c->cur_owner) {
                            an->nsuppressed++;
                            if (c->entry_map[bi] == A_ENTRY_SOFT) an->nsuppressed_soft++;
                        }
                        uint32_t ti = word_index(an, in.target);
                        if (!c->entry_map[ti]) {
                            /* A *forward* `j` is a tail call, and its target is
                             * genuinely another function's entry: hard.
                             *
                             * A *backward* one only reaches here because this
                             * walk began on the jump itself -- the `a != entry`
                             * test above failed -- which happens when a block
                             * was split out and its first instruction jumps
                             * back into the function it came from. That is not
                             * a tail call and the target is not a function, so
                             * calling it hard makes it veto its own merge for
                             * good: the target keeps the epilogue while the
                             * prologue stays behind in the original, and the
                             * two can never be put back together.
                             *
                             * Soft still makes it an entry -- walked, labelled,
                             * dispatchable -- it just stops it from blocking
                             * the merge. Observed at 0x002B60C0, whose only
                             * reference in the whole module is the backward
                             * `j` at 0x002B60CC. */
                            c->entry_map[ti] = (in.target <= a) ? A_ENTRY_SOFT
                                                                : A_ENTRY_HARD;
                            u32_push(c->func_queue, in.target);
                        }
                    }
                }
                break;
            }

            a += 4;
        }
    }

    free(blocks.v);

    if (!insns) return -1;

    memset(out, 0, sizeof *out);
    out->addr = entry;
    out->start = lo;
    out->end = end;
    out->insns = insns;
    out->has_return = has_return;
    out->has_indirect = has_indirect;
    out->has_vfpu = has_vfpu;
    return 0;
}

/* ---- jump tables --------------------------------------------------------- */

/* MIPS compiles a `switch` into a bounds check, a scaled index, a table load
 * and an indirect jump:
 *
 *     sltiu $at, $key, N          bounds check -- N is the entry count
 *     beqz  $at, default
 *     sll   $t0, $key, 2          scale to a word index
 *     lui   $t1, %hi(table)
 *     addu  $t0, $t0, $t1
 *     lw    $t0, %lo(table)($t0)  load the target
 *     jr    $t0
 *
 * Recovering it means walking backward from the `jr` for the `lw` that fed it,
 * then for the lui/addiu pair that formed the address, then for the sltiu that
 * bounded it. The instructions may be interleaved with unrelated ones, so the
 * search is by register rather than by position.
 *
 * The result is cheap to *verify*, which is what makes this safe: every entry
 * must land inside .text, be instruction-aligned, and decode as a valid
 * instruction. A misidentified table fails those and is rejected wholesale
 * rather than seeding garbage.
 */

static uint32_t image_read32(const a_analysis *an, uint32_t addr) {
    if (!an->image) return 0;
    if (addr < an->image_base || addr + 4 > an->image_base + an->image_size) return 0;
    const uint8_t *p = an->image + (addr - an->image_base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* How far back to look. A compiler keeps the whole idiom close together; a
 * wider window mostly finds unrelated instructions that happen to match. */
#define JT_WINDOW 24
#define JT_MAX_ENTRIES 512

static int resolve_jump_table(a_analysis *an, uint32_t site, u32list *out) {
    if (!an->image) return 0;

    a_insn jr;
    a_decode(fetch(an, site), site, &jr);
    const uint8_t target_reg = jr.rs;

    /* Walk back for `lw target_reg, lo(base)`. */
    uint32_t lw_addr = 0;
    a_insn lw;
    memset(&lw, 0, sizeof lw);
    for (int i = 1; i <= JT_WINDOW; i++) {
        uint32_t a = site - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_LW && in.rt == target_reg) { lw = in; lw_addr = a; break; }
        /* If something else wrote the register first, this is not the idiom. */
        if (in.op == A_ADDIU && in.rt == target_reg) return 0;
    }
    if (!lw_addr) return 0;

    /* The lw's base is `index + tablebase`, formed by an `addu`. One operand
     * is the scaled index (from `sll`), the other is the table address. The
     * address itself is built as lui+addiu -- NOT as the lw's displacement,
     * which is typically zero:
     *
     *     lui   $t3, %hi(table)
     *     sll   $t0, $key, 2
     *     addiu $t2, $t3, %lo(table)
     *     addu  $t0, $t0, $t2
     *     lw    $rN, 0($t0)
     *
     * An earlier attempt looked for the low half in the lw displacement, which
     * is the other common form, and so matched nothing here. */
    uint8_t cand[2];
    int ncand = 0;
    for (int i = 1; i <= JT_WINDOW && ncand == 0; i++) {
        uint32_t a = lw_addr - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_ADDU && in.rd == lw.rs) {
            cand[0] = in.rs;
            cand[1] = in.rt;
            ncand = 2;
        }
    }
    if (!ncand) return 0;

    /* Whichever operand traces back to an lui+addiu pair is the table base;
     * the other is the index. */
    uint32_t table = 0;
    int found = 0;
    for (int k = 0; k < 2 && !found; k++) {
        uint8_t reg = cand[k];
        for (int i = 1; i <= JT_WINDOW && !found; i++) {
            uint32_t a = lw_addr - (uint32_t)i * 4;
            if (!a_in_range(an, a)) break;
            a_insn ai;
            a_decode(fetch(an, a), a, &ai);
            if (ai.op != A_ADDIU || ai.rt != reg) continue;

            /* Found the low half; now the lui that set its source. */
            for (int j = 1; j <= JT_WINDOW; j++) {
                uint32_t b = a - (uint32_t)j * 4;
                if (!a_in_range(an, b)) break;
                a_insn li;
                a_decode(fetch(an, b), b, &li);
                if (li.op == A_LUI && li.rt == ai.rs) {
                    table = ((uint32_t)li.imm << 16) + (uint32_t)ai.imm
                          + (uint32_t)lw.imm;
                    found = 1;
                    break;
                }
            }
        }
    }
    if (!found) return 0;

    /* Bound the entry count with the sltiu, if we can find it. Without one,
     * fall back to reading until an entry stops looking like code -- which the
     * per-entry validation below makes safe. */
    uint32_t limit = JT_MAX_ENTRIES;
    for (int i = 1; i <= JT_WINDOW * 2; i++) {
        uint32_t a = site - (uint32_t)i * 4;
        if (!a_in_range(an, a)) break;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.op == A_SLTIU && in.imm > 0 && (uint32_t)in.imm <= JT_MAX_ENTRIES) {
            limit = (uint32_t)in.imm;
            break;
        }
    }

    /* Read and validate. Every entry must be code; the first that is not ends
     * the table. A table that yields nothing valid is rejected entirely. */
    int taken = 0;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t entry = image_read32(an, table + i * 4);
        if (!a_in_range(an, entry)) break;

        a_insn probe;
        if (!a_decode(fetch(an, entry), entry, &probe) || probe.op == A_INVALID) break;

        u32_push(out, entry);
        taken++;
    }

    /* One entry is not evidence of a table -- a single plausible word turns up
     * by chance. Two consecutive valid code pointers is a much stronger
     * signal. */
    if (taken < 2) {
        out->n -= taken;
        return 0;
    }
    return taken;
}

static int cmp_func(const void *a, const void *b) {
    uint32_t x = ((const a_func *)a)->addr, y = ((const a_func *)b)->addr;
    return (x > y) - (x < y);
}


/* ---- reconciling functions that share a block ---------------------------- */

/* Index of the function whose entry is exactly `addr`, or -1. `funcs` sorted. */
static int func_index(const a_func *funcs, int n, uint32_t addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (funcs[mid].addr == addr) return mid;
        if (funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int uf_find(int *parent, int i) {
    while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
    return i;
}

/* Always keep the lower index -- and so the lower address -- as the
 * representative, which makes the result independent of the order the merges
 * were discovered in. */
static void uf_union(int *parent, int a, int b) {
    a = uf_find(parent, a);
    b = uf_find(parent, b);
    if (a == b) return;
    if (a < b) parent[b] = a; else parent[a] = b;
}

/* Collapse each set of functions that share blocks into one.
 *
 * Ownership is rewritten to the representative, the a_func records are folded
 * together, and the entries that stop being functions are recorded so the
 * emitter still gives them a label -- something may call one, and it has to
 * remain reachable through dispatch even though it is no longer a C function
 * of its own.
 *
 * Returns the new function count. */
static int merge_shared(a_analysis *an, a_func *funcs, int nfuncs,
                        const u32list *merges, uint8_t *split_entry) {
    if (nfuncs <= 0) return nfuncs;

    int *parent = (int *)malloc((size_t)nfuncs * sizeof *parent);
    if (!parent) return nfuncs;
    for (int i = 0; i < nfuncs; i++) parent[i] = i;

    for (int i = 0; i + 1 < merges->n; i += 2) {
        const int a = func_index(funcs, nfuncs, merges->v[i]);
        const int b = func_index(funcs, nfuncs, merges->v[i + 1]);
        if (a >= 0 && b >= 0) uf_union(parent, a, b);
    }

    /* Fold the members into their representative. */
    for (int i = 0; i < nfuncs; i++) {
        const int r = uf_find(parent, i);
        if (r == i) continue;
        a_func *rep = &funcs[r], *m = &funcs[i];
        if (m->start < rep->start) rep->start = m->start;
        if (m->end   > rep->end)   rep->end   = m->end;
        rep->insns        += m->insns;
        rep->has_return   |= m->has_return;
        rep->has_indirect |= m->has_indirect;
        rep->has_vfpu     |= m->has_vfpu;
        if (a_in_range(an, m->addr))
            split_entry[word_index(an, m->addr)] = 1;
    }

    /* Rewrite ownership to the representative. */
    for (uint32_t w = 0; w < an->nwords; w++) {
        if (an->owner[w] == A_NO_OWNER) continue;
        const int i = func_index(funcs, nfuncs, an->owner[w]);
        if (i < 0) continue;
        an->owner[w] = funcs[uf_find(parent, i)].addr;
    }

    /* Compact, keeping only the representatives. Order is preserved, so the
     * result is still sorted by address. */
    int out = 0;
    for (int i = 0; i < nfuncs; i++)
        if (uf_find(parent, i) == i) funcs[out++] = funcs[i];

    free(parent);
    return out;
}

/* ---- code nothing reaches ------------------------------------------------ */

/* Does `in` leave unconditionally, other than by a call? `b` is assembled as
 * `beq $zero, $zero`, which the decoder reports as the conditional branch it
 * is encoded as. */
static int leaves(const a_insn *in) {
    if (in->is_call) return 0;
    if (in->is_jump) return 1;                     /* j, jr */
    return in->op == A_BEQ && in->rs == 0 && in->rt == 0;
}

/* A word that may begin code: recognised, and not a VFPU encoding nothing
 * names, which is where data lands most often. */
static int decodes(const a_analysis *an, uint32_t a, a_insn *in) {
    return a_decode(fetch(an, a), a, in) && in->op != A_INVALID && in->op != A_VFPU_UNKNOWN;
}

/* From `a`, a run of instructions ending in an unconditional transfer and its
 * slot, every word decoding, unclaimed, and no transfer naming an address
 * outside the module. Returns the address past the slot, or 0 for no such
 * run before `limit`.
 *
 * A run ending in `b` also needs the word after its slot unclaimed. The walk
 * reads `b` as the conditional branch it is encoded as and carries on past
 * it, and carrying on into a claimed function merges the two -- which is all
 * that seeding The 3rd Birthday's 104 unreachable `b .` stubs, each just
 * before a function, achieved. */
static uint32_t gap_block(const a_analysis *an, const uint32_t *owner,
                          uint32_t a, uint32_t limit) {
    for (uint32_t p = a; p + 4 < limit && a_in_range(an, p + 4); p += 4) {
        a_insn in;
        if (owner[word_index(an, p)] != A_NO_OWNER || !decodes(an, p, &in)) return 0;
        if (in.has_target && !in.is_indirect && !a_in_range(an, in.target)) return 0;
        if (!leaves(&in)) continue;
        a_insn slot;
        if (owner[word_index(an, p + 4)] != A_NO_OWNER || !decodes(an, p + 4, &slot) ||
            slot.has_delay_slot)
            return 0;
        if (!in.is_jump && (!a_in_range(an, p + 8) || owner[word_index(an, p + 8)] != A_NO_OWNER))
            return 0;
        return p + 8;
    }
    return 0;
}

/* Soft entries for the code in the scan range that no walk claimed; see
 * sweep_gaps in analyze.h. Within an unclaimed run, each block after the
 * first starts where the one before it left, past any nops that pad to an
 * alignment. The first word that fails ends the run: what follows it is
 * data, or code a better-founded entry will reach. Returns how many. */
static int sweep_gaps(const a_analysis *an, const uint32_t *owner, uint8_t *entry_map) {
    const uint32_t lo = an->scan_size ? an->scan_base : an->base;
    const uint32_t hi = lo + (an->scan_size ? an->scan_size : an->size);
    int added = 0;
    uint32_t a = lo;
    while (a < hi) {
        if (!a_in_range(an, a) || is_import_stub(an, a) ||
            owner[word_index(an, a)] != A_NO_OWNER || fetch(an, a) == 0) {
            a += 4;
            continue;
        }
        const uint32_t next = entry_map[word_index(an, a)] ? 0 : gap_block(an, owner, a, hi);
        if (!next) {
            while (a < hi && a_in_range(an, a) && owner[word_index(an, a)] == A_NO_OWNER) a += 4;
            continue;
        }
        entry_map[word_index(an, a)] = A_ENTRY_SOFT;
        added++;
        a = next;
    }
    return added;
}

int a_discover(a_analysis *an, const uint32_t *seeds, int nseeds, int nhard) {
    an->funcs = NULL; an->nfuncs = 0;
    an->imports = NULL; an->nimports = 0;
    an->indirects = NULL; an->nindirects = 0;
    an->insns = an->vfpu = an->invalid = 0;
    an->ntables = an->ntable_targets = 0;
    an->bytes_reached = 0;
    an->nsuppressed = an->nsuppressed_soft = 0;
    an->nswept = 0;
    /* The import stubs sit at the end of .text, so their extent is the best
     * available executable bound when no section header gives one. */
    if (!an->text_size && an->stub_addr && an->stub_size)
        an->text_size = an->stub_addr + an->stub_size - an->base;

    const uint32_t nwords = an->size >> 2;
    uint8_t *seen = (uint8_t *)calloc(nwords ? nwords : 1, 1);
    if (!seen) return -1;

    uint8_t *entry_map = (uint8_t *)calloc(nwords ? nwords : 1, 1);
    if (!entry_map) { free(seen); return -1; }

    uint32_t *owner = (uint32_t *)malloc((nwords ? nwords : 1) * sizeof *owner);
    if (!owner) { free(seen); free(entry_map); return -1; }
    for (uint32_t i = 0; i < nwords; i++) owner[i] = A_NO_OWNER;

    u32list queue = { 0 }, imports = { 0 }, indirects = { 0 }, merges = { 0 };
    u32list jr_sites = { 0 };   /* the `jr` sites of the final walk */
    walk_ctx ctx = { an, seen, entry_map, owner, A_NO_OWNER,
                     &queue, &imports, &indirects, &merges };

    /* Pass 1: establish the complete set of function entries before walking
     * anything. Both the tail-call test and the ran-off-the-end test need to
     * ask "is this somebody's entry?", and neither can answer correctly if
     * entries are still being discovered as the walk proceeds. */
    for (int i = 0; i < nseeds; i++) {
        if (!a_in_range(an, seeds[i])) continue;
        const uint32_t wi = word_index(an, seeds[i]);
        /* Hard wins wherever the two lists overlap: an address that is both an
         * export and a stored pointer is an entry on the export's evidence. */
        const uint8_t kind = (i < nhard) ? A_ENTRY_HARD : A_ENTRY_SOFT;
        if (entry_map[wi] < kind) entry_map[wi] = kind;
        /* Pushed unconditionally; a repeat is a no-op once walked. */
        u32_push(&queue, seeds[i]);
    }

    /* Harvest `jal` targets by linear scan. See the note in analyze.h for why
     * this is required rather than a nicety on PSP. Duplicates are harmless —
     * the entry map makes the second sighting a no-op — so no membership test
     * is done here, which keeps this O(n) rather than O(n^2). */
    if (an->scan_calls) {
        /* Restricted to the scan range -- see the note in analyze.h. Falls back
         * to the whole extent if the caller did not set one. */
        uint32_t sbase = an->scan_size ? an->scan_base : an->base;
        uint32_t ssize = an->scan_size ? an->scan_size : an->size;
        for (uint32_t off = 0; off + 4 <= ssize; off += 4) {
            uint32_t a = sbase + off;
            if (!a_in_range(an, a)) continue;
            a_insn in;
            a_decode(fetch(an, a), a, &in);
            if (in.op != A_JAL || !in.has_target) continue;
            if (!a_in_range(an, in.target) || is_import_stub(an, in.target)) continue;

            uint32_t ti = word_index(an, in.target);
            if (entry_map[ti] == A_ENTRY_HARD) continue;
            /* A `jal` here is the code calling the address, which is the
             * strongest evidence there is -- it upgrades a soft seed rather
             * than being skipped by it. Already queued if it was soft. */
            const int was_soft = (entry_map[ti] == A_ENTRY_SOFT);
            entry_map[ti] = A_ENTRY_HARD;
            if (!was_soft) u32_push(&queue, in.target);
        }
    }

    a_func *funcs = NULL;
    int nfuncs = 0, cap = 0;

    /* Two rounds. The first walks everything reachable by control flow; the
     * second walks whatever the jump tables reveal, which can only be resolved
     * once the `jr` sites have been found. Tables discovered in round two are
     * not chased further -- one level covers the switch statements a compiler
     * actually emits, and unbounded rounds would need a fixpoint check for
     * very little gain. */
    /* Iterate to a fixpoint. Each re-walk can expose cross-function branches that
 * the previous walk order hid, so a fixed two rounds leaves some unpromoted --
 * which shows up as a dispatch miss at run time. The cap is a termination
 * guarantee, not an expected limit: entries only ever accumulate, so this
 * converges in a handful of rounds. */
    for (int round = 0; round < 8; round++) {
    if (round > 0) {
        /* Merges no longer drive another round: they change ownership, not the
         * entry set, so re-walking would produce exactly the same split. Only
         * jump tables, and then the gap sweep, can still reveal new entries. */
        int added = 0;
        if (an->image || indirects.n) {
            u32list targets = { 0 };
            for (int i = 0; i < indirects.n; i++) {
                int n = resolve_jump_table(an, indirects.v[i], &targets);
                if (n > 0) { an->ntables++; an->ntable_targets += n; }
            }
            /* Keep the sites. They are cleared just below and the round that
             * finds nothing new exits before any walk refills them, so by the time
             * ownership is final `indirects` is empty -- and the table targets have
             * to be reconciled against ownership, which only exists then. */
            jr_sites.n = 0;
            for (int i = 0; i < indirects.n; i++) u32_push(&jr_sites, indirects.v[i]);
            indirects.n = 0;
            for (int i = 0; i < targets.n; i++) {
                uint32_t t = targets.v[i];
                if (!a_in_range(an, t)) continue;
                uint32_t ti = word_index(an, t);
                if (entry_map[ti] != A_ENTRY_NONE) continue;
                /* Soft, not hard. A resolved table target is a real destination of
                 * a `jr`, but a switch case is a *block inside* a function, not a
                 * function -- it has no prologue, and reached as an entry it is
                 * exactly the split that leaves an epilogue stranded.
                 *
                 * Making it soft does not make it unreachable: merge_shared records
                 * every folded entry in split_entry, so the emitter still gives it
                 * a label and a dispatch thunk, which is what the `jr` needs. It
                 * only stops the address from *vetoing* a merge. */
                entry_map[ti] = A_ENTRY_SOFT;
                added++;
            }
            free(targets.v);
        }
        /* Only once the tables are exhausted: a table target is better
         * evidence than a shape, and claims its words first. */
        if (!added && an->sweep_gaps) {
            added = sweep_gaps(an, owner, entry_map);
            an->nswept += added;
        }
        if (!added) break;

        /* Re-walk everything from a clean slate rather than walking only the
         * new entries.
         *
         * A promoted address was, by definition, already claimed in round one
         * -- that is why it showed up as a cross-function target. Walking it
         * again in isolation does nothing: the very first instruction is
         * already marked as code, so the trace stops immediately and produces
         * an empty function. The owning function also still contains the code,
         * so simply stealing it would leave that function with a hole.
         *
         * Discarding the round-one result and re-walking with the complete
         * entry set produces correct, non-overlapping boundaries in one pass,
         * because every split point is now known before any walking starts --
         * which is the same reason the entry map is built up front to begin
         * with. One extra walk is cheap; reconciling overlapping ownership
         * afterwards is not. */
        memset(seen, 0, nwords);
        for (uint32_t i = 0; i < nwords; i++) owner[i] = A_NO_OWNER;
        /* Ownership is about to be rebuilt, so merges naming the old owners
         * mean nothing. Only the final walk's are applied. */
        merges.n = 0;
        an->insns = an->vfpu = an->invalid = 0;
        nfuncs = 0;

        for (uint32_t i = 0; i < nwords; i++)
            if (entry_map[i]) u32_push(&queue, an->base + i * 4);
        if (!queue.n) break;
    }

    /* The queue grows as calls are found; this terminates because every
     * function entry is marked and never walked twice, and there are finitely
     * many words. */
    while (queue.n) {
        uint32_t entry = queue.v[--queue.n];
        if (!a_in_range(an, entry)) continue;

        uint32_t idx = word_index(an, entry);
        if (seen[idx] & SEEN_ENTRY) continue;
        seen[idx] |= SEEN_ENTRY;

        a_func f;
        ctx.cur_owner = entry;
        if (trace_function(&ctx, entry, &f) != 0) continue;

        if (nfuncs == cap) {
            int ncap = cap ? cap * 2 : 512;
            a_func *nf = (a_func *)realloc(funcs, (size_t)ncap * sizeof *nf);
            if (!nf) break;
            funcs = nf;
            cap = ncap;
        }
        funcs[nfuncs++] = f;
    }
    }   /* rounds */

    for (uint32_t i = 0; i < nwords; i++)
        if (seen[i] & SEEN_CODE) an->bytes_reached += 4;

    qsort(funcs, (size_t)nfuncs, sizeof *funcs, cmp_func);

    an->owner = owner;
    an->nwords = nwords;

    /* A switch case belongs to the function whose `jr` selects it.
     *
     * Nothing else can establish that. The walk stops dead at a computed jump,
     * so the owning function never reaches its own cases by control flow, and
     * the cases are only discovered later by resolving the table -- at which
     * point they are walked as functions in their own right and no branch,
     * jump or fall-through ever connects them back. Every other merge here is
     * driven by the walk noticing a collision; this one has to be stated.
     *
     * Left unstated, a `jr` through a table calls its own switch case, and the
     * case runs the function's epilogue on a frame its caller allocated: two
     * host frames per switch, and a stack-balance report against a prologue
     * that ran in a different invocation. That is the whole of 0x002B608C,
     * whose case at 0x002B60B8 holds the epilogue at 0x002B60C0.
     *
     * Only soft targets. A table that dispatches to genuine functions -- an
     * array of handlers rather than a switch -- has `jal` targets in it, and
     * those are hard and left alone. */
    for (int i = 0; i < jr_sites.n; i++) {
        const uint32_t site = jr_sites.v[i];
        if (!a_in_range(an, site)) continue;
        const uint32_t site_owner = owner[word_index(an, site)];
        if (site_owner == A_NO_OWNER) continue;

        u32list t = { 0 };
        if (resolve_jump_table(an, site, &t) > 0) {
            for (int k = 0; k < t.n; k++) {
                if (!a_in_range(an, t.v[k])) continue;
                const uint32_t ti = word_index(an, t.v[k]);
                if (entry_map[ti] == A_ENTRY_HARD) continue;
                if (owner[ti] == A_NO_OWNER || owner[ti] == site_owner) continue;
                u32_push(&merges, site_owner);
                u32_push(&merges, owner[ti]);
            }
        }
        free(t.v);
    }

    /* Ownership has to be final before the emitter sees it, and merging is
     * what makes a branch between two shared blocks an ordinary `goto` rather
     * than a call that never returns. Ownership is published first because the
     * merge rewrites it in place. */
    an->split_entry = (uint8_t *)calloc(nwords ? nwords : 1, 1);
    if (an->split_entry) {
        const int before = nfuncs;
        nfuncs = merge_shared(an, funcs, nfuncs, &merges, an->split_entry);
        an->nmerged = before - nfuncs;
    }

    an->funcs = funcs;
    an->nfuncs = nfuncs;
    an->imports = imports.v;
    an->nimports = imports.n;
    an->indirects = indirects.v;
    an->nindirects = indirects.n;

    free(jr_sites.v);
    free(merges.v);
    free(queue.v);
    free(seen);
    free(entry_map);
    return 0;
}

int a_scan_data_pointers(const a_analysis *an,
                         const uint8_t *region, uint32_t region_len,
                         uint32_t *out, int max) {
    int found = 0;

    for (uint32_t off = 0; off + 4 <= region_len; off += 4) {
        uint32_t v = (uint32_t)region[off] | ((uint32_t)region[off + 1] << 8) |
                     ((uint32_t)region[off + 2] << 16) | ((uint32_t)region[off + 3] << 24);

        if (!a_in_range(an, v)) continue;        /* inside the code extent */

        /* Point at something that decodes. A pointer into the middle of a data
         * table would usually fail this; a genuine function entry never does. */
        a_insn in;
        if (!a_decode(fetch(an, v), v, &in) || in.op == A_INVALID) continue;

        if (found < max) out[found] = v;
        found++;
    }
    return found;
}

void a_analysis_free(a_analysis *an) {
    free(an->funcs);
    free(an->imports);
    free(an->indirects);
    free(an->owner);
    free(an->split_entry);
    an->funcs = NULL;
    an->imports = NULL;
    an->indirects = NULL;
    an->owner = NULL;
    an->nfuncs = an->nimports = an->nindirects = 0;
}
