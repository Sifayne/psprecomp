#!/usr/bin/env python3
"""Split an emitted <prefix>_funcs.c into N chunks at function boundaries, for the
OPT=1 path of scripts/04-emit-build.sh.

    scripts/emit-split.py game/generated/aclr_funcs.c build/host-opt/split 32

Each chunk gets the file's prelude (the #include). Bodies are static and
self-contained, every call goes through a public psp_func_* symbol, and there is
no file-static data, so the chunks compile independently and `ld -r` merges them
back into one object. The registration function at the end names every
psp_at_* thunk in the module and is one 59k-line function: it goes to its own
file with a generated header of thunk declarations, to be compiled at -O0 (at
-O2 GCC spends minutes on it for nothing that runs more than once)."""
import sys, re, os
src, outdir, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
# The chunks keep the input's emit prefix: <prefix>_funcs.c -> <prefix>_funcs_NN.c.
base = os.path.basename(src)
prefix = base[:-len('_funcs.c')] if base.endswith('_funcs.c') else 'recomp'
lines = open(src, errors='replace').read().split('\n')
starts = [i for i, l in enumerate(lines) if l.startswith('/* ----') and i + 1 < len(lines) and lines[i+1].startswith(' * psp_func_')]
prelude = lines[:starts[0]]
reg = next(i for i, l in enumerate(lines) if l.startswith('void psp_recomp_register('))
tail_start = reg
while tail_start > 0 and not lines[tail_start-1].startswith('/* ----'): tail_start -= 1
tail_start -= 1   # include the '/* ----' line itself
ats = sorted(set(re.findall(r'^void (psp_at_[0-9A-F]{8})\(void\)', '\n'.join(lines), re.M)))
with open(f'{outdir}/at_decls.h', 'w') as f:
    f.write('/* every interior-entry thunk, for the registration table */\n' + ''.join(f'void {a}(void);\n' for a in ats))
per = (len(starts) + n - 1) // n
for c in range(n):
    a = starts[c*per] if c*per < len(starts) else tail_start
    b = starts[(c+1)*per] if (c+1)*per < len(starts) else tail_start
    with open(f'{outdir}/{prefix}_funcs_{c:02d}.c', 'w') as f:
        f.write('\n'.join(prelude) + '\n' + '\n'.join(lines[a:b]) + '\n')
with open(f'{outdir}/{prefix}_funcs_reg.c', 'w') as f:
    f.write('\n'.join(prelude) + '\n#include "at_decls.h"\n' + '\n'.join(lines[tail_start:]) + '\n')
print(f'{len(starts)} functions, {len(lines)} lines -> {n} chunks of ~{per} functions + registration ({len(lines)-tail_start} lines, {len(ats)} thunks)')
