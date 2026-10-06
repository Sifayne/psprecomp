#!/usr/bin/env python3
"""Summarize park censuses (PSPRECOMP_PARK_CENSUS, src/hle/census.h).

    tools/park_census.py [--label NAME] LOG...

Each log is a run's stderr. The summary is what docs/PLAYER-LAYER.md §5 asks
the census to decide: which kinds of wait the threads are parked in at save
points, how many of them need a start/finish split, and what is refused.
"""
import argparse
import collections
import re
import sys

ROW = re.compile(r"^census-row\t")
POLL = re.compile(r"^census: poll (\d+) .*? on (0x[0-9A-F]+) \"([^\"]*)\" after (\S+), resumes at (0x[0-9A-F]+)(.*)$")
SAFE_NEST = re.compile(r"^census:   safe point is under a (.+) (0x[0-9A-F]+)$")
MEDIA = re.compile(r"^census: movie contexts (\d+) \((\d+) holding stream data\), savedata dialog status (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", action="append", default=[])
    ap.add_argument("logs", nargs="+")
    args = ap.parse_args()
    labels = args.label + [None] * (len(args.logs) - len(args.label))

    waits = collections.OrderedDict()   # (kind, call, what) -> {threads, seen, verdict, ...}
    refusals = collections.Counter()
    safe = collections.Counter()
    outcome = collections.Counter()      # per census: what decides it
    censuses = 0
    for path, label in zip(args.logs, labels):
        current = None
        with open(path, errors="replace") as f:
            lines = f.readlines() + ["census: poll 0 (end) on 0x0 \"\" after -, resumes at 0x0\n"]
        for line in lines:
            m = POLL.match(line)
            if m:
                if current is not None:
                    outcome[current] += 1
                if "(end)" in line:
                    break
                censuses += 1
                current = "under host frames" if m.group(6) else "resumable, given split waits"
                safe[(m.group(3), m.group(4))] += 1
                safe_thread, safe_call = m.group(3), m.group(4)
                continue
            m = SAFE_NEST.match(line)
            if m and current is not None:
                refusals[(f"{safe_thread} (the safe point)", safe_call, f"{m.group(1)} {m.group(2)}")] += 1
                continue
            m = MEDIA.match(line)
            if m and current is not None:
                # Host frames decide first; then an open dialog (status 1-3;
                # 4 is finished, a status word); then a movie with stream data.
                if current == "under host frames":
                    pass
                elif 1 <= int(m.group(3)) <= 3:
                    current = "savedata dialog open"
                elif int(m.group(2)):
                    current = "movie playing"
                continue
            if not ROW.match(line):
                continue
            f_ = line.rstrip("\n").split("\t")
            _, uid, name, state, kind, call, what, site, nest, frames, verdict = f_[:11]
            if verdict == "refused-host-frames":
                refusals[(name, call, frames)] += 1
                current = "under host frames"
            key = (kind, call, what if what != call else "-")
            w = waits.setdefault(key, {"threads": set(), "seen": 0, "verdict": verdict,
                                       "sites": set(), "where": set()})
            w["threads"].add(name or uid)
            w["seen"] += 1
            w["sites"].add(site)
            if label:
                w["where"].add(label)

    print(f"{censuses} censuses: " + "; ".join(f"{n} {what}" for what, n in outcome.most_common()))
    print("\nsafe point (thread, after):")
    for (thread, call), n in safe.most_common():
        print(f"  {n:4d}  {thread}  after {call}")
    print("\nparked threads, by wait:")
    print(f"  {'seen':>5} {'thr':>3}  {'verdict':<22} wait")
    for (kind, call, what), w in sorted(waits.items(), key=lambda kv: -kv[1]["seen"]):
        desc = f"{kind} in {call}" if call != "-" else kind
        if what not in ("-", call):
            desc += f' on "{what}"'
        where = f"  [{', '.join(sorted(w['where']))}]" if w["where"] else ""
        print(f"  {w['seen']:5d} {len(w['threads']):3d}  {w['verdict']:<22} {desc}"
              f"  ({len(w['sites'])} return site(s)){where}")
    splits = sorted({call for (kind, call, _), w in waits.items() if w["verdict"] == "wait-in-call"})
    print(f"\nfirmware calls a wait sits in ({len(splits)}): {', '.join(splits) or '-'}")
    if refusals:
        print("\nrefused, host frames between guest frames:")
        for (name, call, frames), n in refusals.most_common():
            print(f"  {n:4d}  {name} in {call}: {frames}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
