"""Fit the scalar random-state transition using only the probe's observations.

No reference implementation is an input. One-bit interventions identify the
bit-linear component and affine coefficients. Mixed inputs select a carry
model; the remaining mixed inputs are withheld for validation.
"""
import itertools
import json
import pathlib
import sys

MASK = (1 << 32) - 1
rows = [[int(word, 16) for word in line.split()]
        for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
rows = [row for row in rows if row[0] != 9]
assert all(len(row) == 19 for row in rows)


def unpack(registers):
    # The one-bit interventions show the low halfwords paired 0/4..3/7.
    words = [(registers[i] & 65535) | ((registers[i+4] & 65535) << 16)
             for i in range(4)]
    # Intervening in bits 16..19 contributes at successive four-bit positions.
    words.append(sum(((value >> 16) & 15) << (4*i)
                     for i, value in enumerate(registers)))
    return words


basis_rows = {row[1]: row for row in rows if row[0] == 5}
mixed = [row for row in rows if row[0] == 6]
thresholds = [row for row in rows if row[0] == 8]
zero = next(row for row in rows if row[:2] == [0, 0])
offset = unpack(zero[11:])[0]
multiplier = (unpack(basis_rows[0][11:])[0] - offset) & MASK
linear_basis = [unpack(basis_rows[32+bit if bit<16 else 160+bit-16][11:])[1]
                for bit in range(32)]


def linear(value):
    result = 0
    for bit in range(32):
        if value & (1 << bit):
            result ^= linear_basis[bit]
    return result


w_offset = unpack(zero[11:])[3]
w_coefficients = [(unpack(basis_rows[index][11:])[3] - w_offset) & MASK
                  for index in (64, 96, 16)]
carry_candidates = []
for shifts in itertools.product(range(3), repeat=3):
    for coefficients in itertools.product(range(3), repeat=3):
        if any(c == 0 and s != 0 for c, s in zip(coefficients, shifts)):
            continue
        if all(sum(c * (v >> s) for c, v, s in
                   zip(coefficients, unpack(row[2:10])[2:], shifts)) >> 32
               == unpack(row[11:])[4] for row in thresholds + mixed[:128]):
            carry_candidates.append((coefficients, shifts))
assert len(carry_candidates) == 1, carry_candidates
carry_coefficients, carry_shifts = carry_candidates[0]

output_candidates = []
for coefficients in itertools.product((-1, 0, 1), repeat=4):
    if all(sum(c * v for c, v in zip(coefficients, unpack(row[11:])[:4])) & MASK
           == row[10] for row in mixed[:128]):
        output_candidates.append(coefficients)
assert len(output_candidates) == 1, output_candidates
output_coefficients = output_candidates[0]


def transition(registers):
    a, b, c, d, e = unpack(registers)
    carry = sum(k * (v >> s) for k, v, s in
                zip(carry_coefficients, (c,d,e), carry_shifts)) >> 32
    advanced = ((a*multiplier + offset) & MASK, linear(b), d,
                (w_offset + sum(k*v for k,v in zip(w_coefficients,(c,d,e)))) & MASK,
                carry)
    value = sum(k*v for k,v in zip(output_coefficients,advanced)) & MASK
    return value, advanced


for row in rows:
    value, state = transition(row[2:10])
    assert value == row[10] and tuple(state) == tuple(unpack(row[11:])), row[:2]
    assert all((register >> 20) == 0x3f8 for register in row[11:]), row[:2]

# Find each seed-controlled bit using varied seed experiments, separately from
# the transition fitting. Constants and duplicated seed bits are both allowed.
seed_rows = [row for row in rows if row[0] == 7]
seed_mapping = []
for register in range(8):
    mapping = []
    for bit in range(32):
        wanted = [(row[2+register] >> bit) & 1 for row in seed_rows]
        if len(set(wanted)) == 1:
            mapping.append(-1 if wanted[0] == 0 else -2)
        else:
            candidates = [source for source in range(32)
                          if all(((row[1] >> source) & 1) == value
                                 for row,value in zip(seed_rows,wanted))]
            assert len(candidates) == 1, (register, bit, candidates)
            mapping.append(candidates[0])
    seed_mapping.append(mapping)

result = dict(multiplier=multiplier, offset=offset, linear_basis=linear_basis,
              w_coefficients=w_coefficients, w_offset=w_offset,
              carry_coefficients=carry_coefficients, carry_shifts=carry_shifts,
              output_coefficients=output_coefficients, seed_mapping=seed_mapping,
              training_mixed_inputs=128, held_out_mixed_inputs=len(mixed)-128,
              validated_observations=len(rows))
print(json.dumps(result, indent=2))
