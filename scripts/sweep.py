#!/usr/bin/env python3
"""The corpus sweep (ROADMAP, phase 6): every module on every disc given,
unwrapped as the importer unwraps it, then decoded and discovered, with what
each needs that this toolkit does not yet have:

- words the decoder cannot name;
- how much of the code discovery reaches;
- constructors in the module's own tables that discovery missed;
- firmware calls the runtime does not answer, less those another module on
  the same disc exports.

    sweep.py [--app DIR | --allegrexrecomp PATH --pspdecrypt PATH]
             [--names-from DIR] [--out DIR] DISC_OR_DIR...

An ISO names one disc; a directory is searched for them. --app is an
extracted AppImage or AppDir (usr/bin/allegrexrecomp and usr/bin/pspdecrypt).
OUT (build/sweep by default) gets one JSON per disc and REPORT.md over all of
them. The report names the calls it can from C headers (--names-from; the
PSP SDK's in ~/pspdev by default)."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
EBOOT = "/PSP_GAME/SYSDIR/EBOOT.BIN"


def load_importer():
    spec = importlib.util.spec_from_file_location("import_game", REPO / "player/import_game.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(args, capture=False):
    p = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode:
        raise ValueError(f"{Path(str(args[0])).name} {args[1]} failed: {p.stderr.strip()[-300:]}")
    return p.stdout if capture else None


def disc_modules(listing):
    """The modules a disc holds: its executable, and every .prx outside the
    firmware update."""
    paths = [m[1] for m in re.finditer(r"^\s+\d+\s+lba=\d+\s+(/.*?)\s*$", listing, re.M)]
    return [p for p in paths if p == EBOOT or (p.lower().endswith(".prx") and "/UPDATE/" not in p.upper())]


def nid_names(include):
    """Names for NIDs. A NID is the first four bytes of the SHA-1 of its
    function's name, little-endian, so each function name the headers under
    `include` declare names the NID it hashes to."""
    names = {}
    for header in sorted(Path(include).rglob("*.h")) if include and Path(include).is_dir() else []:
        for name in re.findall(r"\b(sce[A-Za-z0-9_]+)\s*\(", header.read_text(errors="ignore")):
            names["0x%08X" % int.from_bytes(hashlib.sha1(name.encode()).digest()[:4], "little")] = name
    return names


def first(pattern, text, cast=str, default=None):
    m = re.search(pattern, text, re.M)
    return cast(m[1]) if m else default


def parse_cover(text):
    return dict(decoded_pct=first(r"^decoded:\s+\d+ \(([\d.]+)%\)", text, float),
                vfpu_unnamed=first(r"^vfpu:\s+(\d+)", text, int, 0),
                unknown=first(r"^unknown:\s+(\d+)", text, int, 0))


def parse_funcs(text):
    ctors = {}
    for kind, listed, found in re.findall(r"^ctors:\s+\.(cplinit|ctors) (\d+), (\d+) of them discovered", text, re.M):
        ctors[kind] = dict(listed=int(listed), discovered=int(found))
    return dict(
        functions=first(r"^functions:\s+(\d+)", text, int, 0),
        reached_pct=first(r"^reached:\s+\d+ bytes \(([\d.]+)%", text, float),
        imports=first(r"^unimplemented: \d+ of (\d+)", text, int, 0),
        unimplemented=[dict(lib=lib, nid=nid) for lib, nid in
                       re.findall(r"^\s+unimplemented (\S+)\s+(0x[0-9A-F]{8})$", text, re.M)],
        exports=re.findall(r"^\s+export (0x[0-9A-F]{8})$", text, re.M),
        ctors=ctors,
        no_sections="no section headers" in text,
        undiscovered_ctors=re.findall(r"^\s+undiscovered \.\S+ constructor (0x[0-9A-F]{8})$", text, re.M),
        vfpu_pct=first(r"^\s+vfpu\s+\d+ \(([\d.]+)%\)", text, float),
        invalid=first(r"^\s+invalid\s+(\d+)", text, int, 0))


def sweep_disc(iso, tools, out, importer):
    slug = re.sub(r"[^A-Za-z0-9]+", "-", iso.stem).strip("-").lower()
    work = out / slug
    work.mkdir(parents=True, exist_ok=True)
    disc = dict(disc=iso.name, modules=[])
    for i, path in enumerate(disc_modules(run([tools["allegrexrecomp"], "ls", iso], capture=True))):
        raw_dir = work / "raw" / str(i)
        raw_dir.mkdir(parents=True, exist_ok=True)
        entry = dict(path=path)
        disc["modules"].append(entry)
        try:
            run([tools["allegrexrecomp"], "extract", iso, path, raw_dir])
            raw = (raw_dir / path.lstrip("/").replace("/", "_")).read_bytes()
        except (ValueError, OSError) as e:
            entry["error"] = f"not extracted: {e}"
            continue
        entry["size"] = len(raw)
        image = importer.unwrap_module(run, raw, raw_dir, tools["pspdecrypt"])
        if image is None:
            entry["error"] = "not a module this toolkit reads (wrapped, encrypted or compressed past unwrapping)"
            continue
        elf = work / f"{i:02d}-{Path(path).name}.elf"
        elf.write_bytes(image)
        try:
            entry["cover"] = parse_cover(run([tools["allegrexrecomp"], "cover", elf], capture=True))
            entry["funcs"] = parse_funcs(run([tools["allegrexrecomp"], "funcs", elf, "--list"], capture=True))
        except ValueError as e:
            entry["error"] = str(e)
    # A call another module on the disc exports is that module's to answer.
    exported = {nid for m in disc["modules"] for nid in m.get("funcs", {}).get("exports", [])}
    for m in disc["modules"]:
        f = m.get("funcs")
        if f:
            f["unanswered"] = [c for c in f["unimplemented"] if c["nid"] not in exported]
    (out / f"{slug}.json").write_text(json.dumps(disc, indent=1) + "\n")
    return disc


def report(discs, names=None):
    lines = ["# Corpus sweep", "",
             "Per module: the share of words the decoder names; words it cannot (`unknown`); "
             "the VFPU share of discovered code; functions found and the share of `.text` they "
             "reach; firmware calls imported and those nothing answers (neither this runtime nor "
             "another module on the disc); and constructors in the module's tables that "
             "discovery found.", "",
             "| Disc | Module | Decoded | Unknown | VFPU | Functions | Reached | Calls | Unanswered | Constructors |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    needed = {}
    misses = []
    for d in discs:
        for m in d["modules"]:
            name = m["path"].removeprefix("/PSP_GAME/")
            if "error" in m and "funcs" not in m:
                lines.append(f"| {d['disc']} | {name} | {m['error']} |  |  |  |  |  |  |  |")
                continue
            c, f = m["cover"], m["funcs"]
            ctors = "no section headers" if f["no_sections"] else ", ".join(
                f"{k} {v['discovered']}/{v['listed']}" for k, v in f["ctors"].items() if v["listed"]) or "none"
            lines.append(f"| {d['disc']} | {name} | {c['decoded_pct']}% | {c['unknown']} | {f['vfpu_pct']}% | "
                         f"{f['functions']} | {f['reached_pct']}% | {f['imports']} | {len(f['unanswered'])} | {ctors} |")
            for call in f["unanswered"]:
                needed.setdefault((call["lib"], call["nid"]), set()).add(d["disc"])
            misses += [(d["disc"], name, a) for a in f["undiscovered_ctors"]]
    lines += ["", "## Firmware calls nothing answers", "",
              "By how many discs need each. A name is a function name from the headers that hashes "
              "to the NID; a call without one has no name there.", "",
              "| Library | NID | Name | Discs |", "|---|---|---|---|"]
    names = names or {}
    for (lib, nid), ds in sorted(needed.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        lines.append(f"| {lib} | {nid} | {names.get(nid, '')} | {len(ds)}: {', '.join(sorted(ds))} |")
    lines += ["", "## Constructors discovery missed", ""]
    lines += [f"- {disc}, {mod}: {addr}" for disc, mod, addr in misses] or ["None."]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("discs", nargs="+", type=Path)
    ap.add_argument("--app", type=Path)
    ap.add_argument("--allegrexrecomp", type=Path, default=REPO / "build/core/tools/allegrexrecomp/allegrexrecomp")
    ap.add_argument("--pspdecrypt", type=Path)
    ap.add_argument("--names-from", type=Path, default=Path.home() / "pspdev/psp/sdk/include")
    ap.add_argument("--out", type=Path, default=REPO / "build/sweep")
    a = ap.parse_args()
    tools = dict(allegrexrecomp=a.allegrexrecomp, pspdecrypt=a.pspdecrypt)
    if a.app:
        tools = dict(allegrexrecomp=a.app / "usr/bin/allegrexrecomp", pspdecrypt=a.app / "usr/bin/pspdecrypt")
    if not tools["pspdecrypt"]:
        ap.error("--pspdecrypt or --app is needed: discs hold encrypted modules")
    isos = []
    for p in a.discs:
        isos += sorted(q for q in p.rglob("*") if q.suffix.lower() == ".iso") if p.is_dir() else [p]
    a.out.mkdir(parents=True, exist_ok=True)
    importer = load_importer()
    discs = []
    for iso in isos:
        print(f"sweeping {iso.name}", flush=True)
        discs.append(sweep_disc(iso, tools, a.out, importer))
    (a.out / "REPORT.md").write_text(report(discs, nid_names(a.names_from)))
    print(f"report: {a.out / 'REPORT.md'}")


if __name__ == "__main__":
    main()
