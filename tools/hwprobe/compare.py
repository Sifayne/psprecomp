#!/usr/bin/env python3
"""Compare a probe's hardware results with psprecomp's.

    compare.py <hardware dir> <psprecomp dir> [--lines N]

Both directories hold one probe's output: <name>.txt plus any .bin/.raw
files. The hardware one is the probe's folder copied off the memory stick;
the psprecomp one is ./ms/PSP/GAME/<name>/ after

    allegrexrecomp interp <name>.prx --dispatch --budget 4000000000 --drain 200

The log is compared step by step (a step starts at a "[n] ..." line), so one
difference does not shift every line after it. A log holds every run of the
probe, one after another; only the last run in each is compared, unless
--run picks another (1 is the first, -1 the last). A probe that switched the
PSP off is started again and skips that step, so its last run is the whole
result. Binary files are compared by
content: .bin as 32-bit words (reporting the input word too when
vfpu_inputs.bin is present and as long), .raw as pixels of a 480-wide frame. Standard
library only."""
import os, re, struct, sys

STEP = re.compile(r"^\[(\d+)\] (.*)")
RUN = re.compile(r"^==== \S+ \d+, firmware")
# The closing "==== <name> done ====" too: it belongs to whichever step is
# last, so a newer version that appends steps would otherwise show the old
# last step as different.
HEADER = re.compile(r"^(==== .* firmware|==== \S+ done ====$|log: |an earlier run stopped in )")


def last_run(lines, run):
    """The lines of one run: the last by default."""
    starts = [i for i, l in enumerate(lines) if RUN.match(l)] or [0]
    starts.append(len(lines))
    k = run - 1 if run > 0 else len(starts) - 1 + run
    k = max(0, min(k, len(starts) - 2))
    return lines[starts[k]:starts[k + 1]]


def steps(path, run=-1):
    """Map step title -> list of lines under it, in file order."""
    out, cur, order = {}, "(before the first step)", []
    out[cur] = []
    order.append(cur)
    for line in last_run(open(path, errors="replace").read().splitlines(), run):
        if HEADER.match(line):
            continue
        m = STEP.match(line)
        if m:
            cur = m.group(2)
            while cur in out:
                cur += "'"
            out[cur] = []
            order.append(cur)
        else:
            out[cur].append(line)
    for lines in out.values():          # the blank line before "done"
        while lines and not lines[-1]:
            lines.pop()
    return out, order


def compare_logs(hw, pc, nlines, run):
    a, order = steps(hw, run)
    b, _ = steps(pc)
    same = differ = missing = 0
    for title in order:
        if title not in b:
            missing += 1
            print(f"MISSING in psprecomp: [{title}]")
            continue
        if a[title] == b[title]:
            same += 1
            continue
        differ += 1
        print(f"DIFFERS: [{title}]")
        shown = 0
        for i in range(max(len(a[title]), len(b[title]))):
            x = a[title][i] if i < len(a[title]) else "<none>"
            y = b[title][i] if i < len(b[title]) else "<none>"
            if x != y:
                print(f"   hw: {x}\n   pc: {y}")
                shown += 1
                if shown >= nlines:
                    print("   ...")
                    break
    print(f"\nsteps: {same} same, {differ} differ, {missing} missing in psprecomp")


def compare_words(name, hw, pc, inputs):
    x = open(hw, "rb").read()
    y = open(pc, "rb").read() if os.path.exists(pc) else b""
    if not y:
        print(f"{name}: missing in psprecomp")
        return
    if inputs and len(x) != len(inputs) * 4:
        inputs = None   # a file with inputs of its own (vfpuprobe's core dumps)
    n = min(len(x), len(y)) // 4
    wa = struct.unpack(f"<{n}I", x[:n * 4])
    wb = struct.unpack(f"<{n}I", y[:n * 4])
    bad = [i for i in range(n) if wa[i] != wb[i]]
    note = "" if len(x) == len(y) else f" (sizes {len(x)} vs {len(y)})"
    print(f"{name}: {len(bad)} of {n} words differ{note}")
    for i in bad[:8]:
        src = f" input {inputs[i]:08X}" if inputs and i < len(inputs) else ""
        print(f"   [{i}]{src}: hw {wa[i]:08X} pc {wb[i]:08X}")


def compare_frame(name, hw, pc):
    x = open(hw, "rb").read()
    y = open(pc, "rb").read() if os.path.exists(pc) else b""
    if not y:
        print(f"{name}: missing in psprecomp")
        return
    bpp = 4 if len(x) == 480 * 272 * 4 else 2
    n = min(len(x), len(y)) // bpp
    bad, box = 0, [480, 272, -1, -1]
    for i in range(n):
        if x[i * bpp:(i + 1) * bpp] != y[i * bpp:(i + 1) * bpp]:
            bad += 1
            px, py = i % 480, i // 480
            box = [min(box[0], px), min(box[1], py), max(box[2], px), max(box[3], py)]
    where = f", within x {box[0]}..{box[2]} y {box[1]}..{box[3]}" if bad else ""
    print(f"{name}: {bad} of {n} pixels differ{where}")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("hardware")
    ap.add_argument("psprecomp")
    ap.add_argument("--lines", type=int, default=6, help="differing lines shown per step")
    ap.add_argument("--run", type=int, default=-1,
                    help="which run of the hardware log: 1 is the first, -1 (default) the last")
    a = ap.parse_args()
    hw, pc = a.hardware, a.psprecomp
    for f in sorted(os.listdir(hw)):
        if f.endswith(".txt") and os.path.exists(os.path.join(pc, f)):
            print(f"==== {f}")
            compare_logs(os.path.join(hw, f), os.path.join(pc, f), a.lines, a.run)
    inputs = None
    ip = os.path.join(hw, "vfpu_inputs.bin")
    if os.path.exists(ip):
        d = open(ip, "rb").read()
        inputs = struct.unpack(f"<{len(d) // 4}I", d)
    for f in sorted(os.listdir(hw)):
        if f.endswith(".bin") and f != "vfpu_inputs.bin":
            compare_words(f, os.path.join(hw, f), os.path.join(pc, f), inputs)
        elif f.endswith(".raw"):
            compare_frame(f, os.path.join(hw, f), os.path.join(pc, f))


if __name__ == "__main__":
    main()
