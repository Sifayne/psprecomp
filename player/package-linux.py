#!/usr/bin/env python3
"""Build psprecomp's AppImage, and the file of each pack named, in an
unprivileged Ubuntu 22.04 container.

    player/package-linux.py build [--pack PATH]... [--output DIR] [--work DIR]

The app holds no pack; a player adds a pack's file (<file>-pack.zip) to it.
Each pack named is checked, written to its file, and added to the staged
app as a player would add it. Only Bubblewrap, Python 3, curl and tar are
required on the build host. No daemon, root, game dump, generated C, or
developer build products are used: the container sees this toolkit's
sources and the files each pack's manifest names (player/pack.py), nothing
else. Last Raven's scripts/package-linux.py until stage 2 of
docs/PLAYER-LAYER.md moved it here.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

TOOLKIT = Path(__file__).resolve().parent.parent
RECIPE = TOOLKIT / "player/linux"
LOCK = json.loads((RECIPE / "dependencies.json").read_text())
sys.path.insert(0, str(TOOLKIT / "player"))
import pack as packs  # noqa: E402

# Set by main(): the packs, and the build cache.
PACKS = []
WORK = DOWNLOADS = ROOTFS = None
MARKER = ".player-builder"


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch():
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    pins = dict(LOCK)
    ff = json.loads((TOOLKIT / "third_party/ffmpeg/source.json").read_text())
    pins[f"ffmpeg-{ff['version']}.tar.xz"] = ff
    for name, pin in pins.items():
        dest = DOWNLOADS / name
        if not dest.exists():
            # A development FFmpeg build may already hold the archive.
            cached = next((p.root / "build/deps/downloads" / name for p in PACKS
                           if (p.root / "build/deps/downloads" / name).exists()), None)
            if cached and sha(cached) == pin["sha256"]:
                shutil.copy2(cached, dest)
            else:
                part = dest.with_name(name + ".part")
                try:
                    run(["curl", "-fL", "--retry", "2", "--connect-timeout", "20",
                         "--max-time", "600", pin["url"], "-o", part])
                    if sha(part) != pin["sha256"]:
                        raise ValueError(f"checksum mismatch: {name}")
                    part.replace(dest)
                finally:
                    part.unlink(missing_ok=True)
        if sha(dest) != pin["sha256"]:
            raise ValueError(f"checksum mismatch: {dest}; remove it and fetch again")
    print("All pinned downloads verified.", flush=True)


def container(args, *, network=False, mounts=(), writable_root=False):
    cmd = ["bwrap", "--die-with-parent", "--unshare-all", "--uid", "0", "--gid", "0"]
    if network:
        cmd += ["--share-net"]
    cmd += ["--bind" if writable_root else "--ro-bind", ROOTFS, "/",
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
            "--clearenv", "--setenv", "PATH", "/usr/bin:/bin",
            "--setenv", "HOME", "/tmp", "--setenv", "LC_ALL", "C.UTF-8"]
    if network:
        cmd += ["--ro-bind", Path("/etc/resolv.conf").resolve(), "/etc/resolv.conf"]
    for source, target, writable in mounts:
        (ROOTFS / target.lstrip("/")).mkdir(parents=True, exist_ok=True)
        cmd += ["--bind" if writable else "--ro-bind", source, target]
    return run(cmd + list(args))


def bootstrap():
    marker = ROOTFS / MARKER
    expected = sha(RECIPE / "bootstrap.sh") + LOCK["ubuntu-base-22.04.5-base-amd64.tar.gz"]["sha256"]
    # A builder Last Raven's own pipeline bootstrapped from the same recipe
    # carries the same identity under its old name.
    legacy = ROOTFS / ".last-raven-builder"
    if not marker.exists() and legacy.exists() and legacy.read_text() == expected:
        legacy.rename(marker)
    if marker.exists() and marker.read_text() == expected:
        return
    if not ROOTFS.exists():
        with tempfile.TemporaryDirectory(prefix="rootfs-", dir=WORK) as tmp:
            root = Path(tmp) / "root"
            root.mkdir()
            # Official, checksum-verified Ubuntu base archive; skip devices.
            run(["tar", "--no-same-owner", "--exclude=dev/*", "-xf",
                 DOWNLOADS / "ubuntu-base-22.04.5-base-amd64.tar.gz", "-C", root])
            (root / "etc/resolv.conf").unlink(missing_ok=True)
            (root / "etc/resolv.conf").touch()
            (root / "etc/apt/apt.conf.d/99player").write_text(
                'APT::Sandbox::User "root";\nDPkg::Options { "--force-not-root"; "--force-bad-path"; };\n')
            # Never attempt to start services in the build namespace.
            policy = root / "usr/sbin/policy-rc.d"
            policy.write_text("#!/bin/sh\nexit 101\n")
            policy.chmod(0o755)
            root.rename(ROOTFS)
    container(["/bin/sh", "/recipe/bootstrap.sh"], network=True, writable_root=True,
              mounts=[(RECIPE, "/recipe", False)])
    marker.write_text(expected)


def copy_tree(source, dest, suffixes=None):
    for path in sorted(source.rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        if suffixes and path.suffix not in suffixes:
            continue
        target = dest / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def copy_inputs(dest):
    # Allowlists keep dumps, keys, generated game code and build products out
    # of both the container and the matching source archive: this toolkit's
    # runtime, recompiler and player sources, and each pack's named files.
    toolkit = dest / "psprecomp"
    if not (TOOLKIT / "src/cpu.c").is_file():
        raise ValueError(f"{TOOLKIT} is not a psprecomp checkout")
    for folder in ("src", "include", "player", "third_party/stb", "third_party/imgui", "third_party/ffmpeg"):
        copy_tree(TOOLKIT / folder, toolkit / folder)
    copy_tree(TOOLKIT / "tools/allegrexrecomp", toolkit / "tools/allegrexrecomp", {".c", ".h"})
    for name in ("LICENSE", "tests/test_present.c", "tests/test_savedata.c", "tests/test_launcher.c",
                 "tests/test_pack_plugin.c"):
        (toolkit / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(TOOLKIT / name, toolkit / name)
    for pack in PACKS:
        for name in pack.files():
            target = dest / "packs" / pack.id / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(pack.root / name, target)
    manifest = {str(p.relative_to(dest)): sha(p) for p in sorted(dest.rglob("*")) if p.is_file()}
    (dest / "INPUTS.json").write_text(json.dumps(manifest, indent=2) + "\n")


def build(output):
    if output.exists():
        raise ValueError(f"output exists: {output}; select a new --output directory")
    bootstrap()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="build-", dir=WORK) as tmp:
        stage = Path(tmp)
        source = stage / "work"
        source.mkdir()
        copy_inputs(source)
        try:
            container(["/usr/bin/python3", "/work/psprecomp/player/linux/build.py"], mounts=[
                (source, "/work", True), (DOWNLOADS, "/downloads", False)])
        finally:
            log = source / "build/package-build.log"
            if log.exists():
                shutil.copy2(log, WORK / "last-build.log")
        # Move on the destination filesystem only after all checks pass.
        with tempfile.TemporaryDirectory(prefix="package-", dir=output.parent) as publish:
            ready = Path(publish) / "ready"
            shutil.copytree(source / "build/release", ready, symlinks=True)
            ready.rename(output)
    print(f"Validated psprecomp" + "".join(f" and {p.name}" for p in PACKS) + f": {output}")


def main():
    global PACKS, WORK, DOWNLOADS, ROOTFS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=("fetch", "bootstrap", "build"), nargs="?", default="build")
    p.add_argument("--pack", type=Path, action="append", default=[],
                   help="a directory holding pack.json; repeat for several")
    p.add_argument("--work", type=Path, help="download and builder cache (default: build/package here)")
    p.add_argument("--output", type=Path, help="a new release directory (default: build/releases/player here)")
    args = p.parse_args()
    try:
        PACKS = [packs.load(path) for path in args.pack]
        ids = [pack.id for pack in PACKS]
        if len(ids) != len(set(ids)):
            raise ValueError("each pack may be named once")
        WORK = (args.work or TOOLKIT / "build/package").resolve()
        DOWNLOADS = WORK / "downloads"
        ROOTFS = WORK / "ubuntu-22.04"
        output = (args.output or TOOLKIT / "build/releases/player").resolve()
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            raise ValueError("this pipeline currently requires an x86-64 Linux build host")
        WORK.mkdir(parents=True, exist_ok=True)
        with (WORK / ".lock").open("w") as guard:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fetch()
            if args.command != "fetch":
                if not shutil.which("bwrap"):
                    raise ValueError("install Bubblewrap (bwrap) on the build host")
                if args.command == "bootstrap":
                    bootstrap()
                else:
                    build(output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Packaging: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
