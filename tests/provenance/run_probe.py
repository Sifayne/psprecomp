#!/usr/bin/env python3
"""Build our freestanding probe and record an external executable's stdout.

The executable is deliberately an explicit argument. No reference source,
game data, network download, or reference library is used by this script.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(text):
    """Small deterministic selection; full output is kept by the caller."""
    selected = []
    for line in text.splitlines():
        row = [int(word, 16) for word in line.split()]
        kind, index = row[:2]
        if (kind < 6 or kind in (7, 10, 11)
                or kind == 6 and index % 32 == 0
                or kind == 8 and index % 7 == 0
                or kind == 9 and ((index & 4095) < 4
                    or 256 <= (index & 4095) < 260
                    or (index & 4095) in [1 << bit for bit in range(12)]
                    or (index & 4095) % 127 == 0)):
            selected.append(line)
    return "\n".join(selected) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="path to an external PPSSPPHeadless executable")
    parser.add_argument("output", type=Path, help="new output directory")
    parser.add_argument("--suite", choices=("vrnd", "umd", "intr", "ge", "vertices", "savedata", "kernel", "mpeg", "pixels", "disc"), default="vrnd")
    args = parser.parse_args()
    executable = Path(shutil.which(args.executable) or args.executable).resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    if args.suite != "vrnd":
        source /= args.suite
    flags = ["--target=mipsel-none-elf", "-march=mips2", "-mabi=32", "-mno-abicalls",
             "-fno-pic", "-msoft-float", "-G0", "-ffreestanding", "-fno-builtin", "-O2"]
    objects = []
    names = (["probe.c", "rng.S", "imports.S", "probe.ld"] if args.suite == "vrnd"
             else ["probe.c", "imports.S", "probe.ld"])
    if args.suite in ("intr", "ge"):
        names.insert(1, "control.S")
    if args.suite in ("kernel", "mpeg", "disc"):
        names.insert(1, "abi.S")
    for name in names[:-1]:
        obj = output / (Path(name).stem + ".o")
        subprocess.run(["clang", *flags, "-c", str(source / name), "-o", str(obj)], check=True)
        objects.append(str(obj))
    elf = output / "probe.elf"
    subprocess.run(["ld.lld", "-m", "elf32ltsmip", "-T", str(source / "probe.ld"),
                    *objects, "-o", str(elf)], check=True)
    command = [str(executable), "--root", str(output), "--timeout=30", "-l", str(elf)]
    if args.suite in ("vertices", "pixels"):
        command.insert(1, "--graphics=software")
    if args.suite == "disc":
        # The probe runs as a game booted from its own disc: make_iso.py puts
        # the ELF just built into a bootable ISO 9660 image of its own, and
        # the executable is given the image, not the ELF. (Loaded from the
        # host with the image mounted by -m, the same probe sees no umd0:.)
        image = output / "probe.iso"
        subprocess.run(["python3", str(source / "make_iso.py"), str(elf), "-o", str(image)], check=True)
        command[-1] = str(image)
    if args.suite == "savedata":
        # Hide the entire real home. The observed executable uses ~/.ppsspp
        # as ms0; --root is host0 and does not isolate savedata by itself.
        # No HOME override and no writes outside this new output directory.
        home = Path.home()
        stick = output / "memorystick"
        stick.mkdir()
        savedata = stick / "PSP" / "SAVEDATA"
        for name in ("DIRFILE", "READDENIED", "BLOCKED", "BROKEN"):
            slot = savedata / ("PRV260919" + name)
            slot.mkdir(parents=True)
            (slot / "PARAM.SFO").write_bytes(b"original malformed metadata stimulus")
            if name == "DIRFILE":
                (slot / "DATA.BIN").mkdir()
            else:
                (slot / "DATA.BIN").write_bytes(bytes(range(64)))
            if name == "READDENIED":
                (slot / "DATA.BIN").chmod(0)
            if name == "BLOCKED":
                slot.chmod(0o500)
        command = ["bwrap", "--ro-bind", "/", "/", "--tmpfs", str(home), "--tmpfs", "/tmp",
                   "--bind", str(stick), str(home / ".ppsspp"),
                   "--ro-bind", str(output), "/tmp/probe", "--chdir", "/tmp/probe",
                   "--die-with-parent", str(executable), "--root", "/tmp/probe",
                   "--timeout=30", "-l", "/tmp/probe/probe.elf"]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=45, check=False)
    (output / "diagnostics.txt").write_bytes(result.stdout)
    result.check_returncode()
    lines = [line.split("I stdout: ", 1)[1].strip()
             for line in result.stdout.decode(errors="replace").splitlines()
             if "I stdout: " in line]
    expected = {"vrnd": 54809, "umd": 31, "intr": 68}.get(args.suite)
    if args.suite == "mpeg":
        if not lines or not lines[-1].startswith("0000000c "):
            raise RuntimeError("incomplete MPEG experiment; see diagnostics.txt")
    elif args.suite == "kernel":
        if not lines or not lines[-1].startswith("00000006 "):
            raise RuntimeError("incomplete kernel experiment; see diagnostics.txt")
    elif args.suite == "savedata":
        if not lines or not lines[-1].startswith("00000003 "):
            raise RuntimeError("incomplete isolated savedata probe; see diagnostics.txt")
    elif args.suite == "vertices":
        if not lines or not lines[-1].startswith("00000002 "):
            raise RuntimeError("incomplete vertex experiment; see diagnostics.txt")
    elif args.suite in ("pixels", "disc"):
        if not lines or not lines[-1].startswith("00000009 "):
            raise RuntimeError(f"incomplete {args.suite} experiment; see diagnostics.txt")
    elif args.suite == "ge":
        if not 85 <= len(lines) <= 117 or not lines[-1].startswith("00000003 "):
            raise RuntimeError("incomplete GE version experiment; see diagnostics.txt")
    elif len(lines) != expected:
        raise RuntimeError(f"expected {expected} observations, received {len(lines)}; see diagnostics.txt")
    observations = "\n".join(lines) + "\n"
    (output / "observed.txt").write_text(observations)
    (output / "fixture.txt").write_text(fixture(observations) if args.suite == "vrnd" else observations)
    metadata = dict(suite=args.suite, command=command, executable_sha256=digest(executable),
                    runner_sha256=digest(Path(__file__).resolve()),
                    probe_sha256=digest(elf), observations_sha256=digest(output / "observed.txt"),
                    fixture_sha256=digest(output / "fixture.txt"),
                    sources={name: digest(source / name) for name in names +
                             (["cases.h", "cases.json", "make_cases.py"] if args.suite == "vertices" else
                              ["make_iso.py"] if args.suite == "disc" else [])},
                    **({"image_sha256": digest(output / "probe.iso")} if args.suite == "disc" else {}),
                    clang=subprocess.check_output(["clang", "--version"], text=True).splitlines()[0],
                    linker=subprocess.check_output(["ld.lld", "--version"], text=True).strip(),
                    observations=len(lines), evidence="external executable output; not physical PSP")
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Recorded {len(lines)} observations in {output}")


if __name__ == "__main__":
    main()
