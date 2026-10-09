#!/usr/bin/env python3
"""Build, inspect and stage the pinned Linux LGPL audio dependency.

No system FFmpeg discovery: the release archive, configuration and shared
library paths are explicit. Outputs and downloaded source stay under the
--deps directory (default: this checkout's build/deps); a game passes its own,
as Last Raven's scripts/build-tools.sh does. Last Raven's scripts/ffmpeg.py
until stage 2 of docs/PLAYER-LAYER.md moved it beside the player.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "third_party/ffmpeg/source.json"
PIN = json.loads(LOCK.read_text())
DEPS = ROOT / "build/deps"          # --deps
PREFIX = DEPS / "ffmpeg"
# Named for the game this recipe was written for; existing prefixes carry it,
# and a prefix without the marker is refused rather than replaced.
MARKER = ".last-raven-ffmpeg.json"
LICENSE = "LGPL version 2.1 or later"


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def configuration(prefix):
    return [
        f"--prefix={prefix}", "--libdir=" + str(prefix / "lib"),
        "--disable-gpl", "--disable-nonfree", "--disable-version3",
        "--disable-autodetect", "--disable-everything", "--disable-programs",
        "--disable-doc", "--disable-debug", "--disable-static", "--enable-shared",
        "--disable-avdevice", "--disable-avfilter", "--disable-avformat",
        "--disable-swresample", "--disable-swscale", "--disable-network",
        "--disable-x86asm", "--enable-decoder=atrac3,atrac3p",
    ]


def archive():
    path = DEPS / "downloads" / f"ffmpeg-{PIN['version']}.tar.xz"
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".part")
        try:
            run(["curl", "--fail", "--location", "--retry", "2",
                 "--connect-timeout", "20", "--max-time", "300",
                 PIN["url"], "--output", temporary])
            if digest(temporary) != PIN["sha256"]:
                raise ValueError("downloaded FFmpeg source checksum mismatch")
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    if digest(path) != PIN["sha256"]:
        raise ValueError(f"FFmpeg source checksum mismatch: {path}; remove it and retry")
    return path


def dynamic(path):
    return run(["readelf", "-d", path], capture_output=True, text=True).stdout


def probe(prefix):
    """Runs in a fresh process so prior dlopen calls cannot mask resolution."""
    codec = ctypes.CDLL(str(prefix / "lib/libavcodec.so"))
    codec.av_version_info.restype = ctypes.c_char_p
    version = codec.av_version_info().decode()
    if version != PIN["version"]:
        raise ValueError(f"expected FFmpeg {PIN['version']}, found {version}")
    # Resolve avutil THROUGH avcodec to inspect the dependency actually loaded.
    class DlInfo(ctypes.Structure):
        _fields_ = [("name", ctypes.c_char_p), ("base", ctypes.c_void_p),
                    ("symbol", ctypes.c_char_p), ("address", ctypes.c_void_p)]
    dladdr = ctypes.CDLL(None).dladdr
    dladdr.argtypes = [ctypes.c_void_p, ctypes.POINTER(DlInfo)]
    dladdr.restype = ctypes.c_int
    report = {"version": version}
    for name in ("avcodec", "avutil"):
        fn = getattr(codec, name + "_license")
        fn.restype = ctypes.c_char_p
        license_text = fn().decode()
        if license_text != LICENSE:
            raise ValueError(f"{name}: expected {LICENSE}, found {license_text}")
        info = DlInfo()
        if not dladdr(ctypes.cast(fn, ctypes.c_void_p), ctypes.byref(info)):
            raise ValueError(f"cannot locate loaded {name}")
        loaded = Path(os.fsdecode(info.name)).resolve()
        if loaded.parent != (prefix / "lib").resolve():
            raise ValueError(f"{name} resolved outside the bundle: {loaded}")
        config = getattr(codec, name + "_configuration")
        config.restype = ctypes.c_char_p
        flags = config().decode()
        for flag in ("--disable-gpl", "--disable-nonfree", "--disable-version3",
                     "--disable-static", "--enable-shared", "--disable-autodetect"):
            if flag not in shlex.split(flags):
                raise ValueError(f"{name}: missing release configuration {flag}")
        report[name] = {"license": license_text, "path": str(loaded),
                        "configuration": flags}
    class AVCodec(ctypes.Structure):
        _fields_ = [("name", ctypes.c_char_p)]
    iterate = codec.av_codec_iterate
    iterate.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    iterate.restype = ctypes.POINTER(AVCodec)
    opaque = ctypes.c_void_p()
    names = []
    while True:
        item = iterate(ctypes.byref(opaque))
        if not item:
            break
        names.append(item.contents.name.decode())
    if sorted(names) != ["atrac3", "atrac3plus"]:
        raise ValueError(f"unexpected codec set: {names}")
    report["decoders"] = sorted(names)
    print(json.dumps(report, indent=2))


def verify(prefix):
    if platform.system() != "Linux":
        raise ValueError("the bundled FFmpeg recipe currently supports Linux only")
    for name in ("avcodec", "avutil"):
        lib = prefix / "lib" / f"lib{name}.so"
        text = dynamic(lib)
        soname = f"lib{name}.so.{PIN[name + '_major']}"
        if f"[{soname}]" not in text:
            raise ValueError(f"unexpected shared-library ABI: {lib}")
        paths = [line for line in text.splitlines() if "(RUNPATH)" in line or "(RPATH)" in line]
        if len(paths) != 1 or "(RUNPATH)" not in paths[0] or "[$ORIGIN]" not in paths[0]:
            raise ValueError(f"{lib}: expected replaceable, relative RUNPATH $ORIGIN: {paths}")
        allowed = {"libm.so.6", "libc.so.6", "libpthread.so.0", "libdl.so.2",
                   "librt.so.1", "libatomic.so.1", f"libavutil.so.{PIN['avutil_major']}"}
        for line in text.splitlines():
            if "(NEEDED)" in line and line.split("[", 1)[1].split("]", 1)[0] not in allowed:
                raise ValueError(f"unexpected FFmpeg dependency: {line.strip()}")
    result = run([sys.executable, __file__, "_probe", "--prefix", prefix],
                 capture_output=True, text=True)
    return json.loads(result.stdout)


def build(prefix, rebuild):
    if platform.system() != "Linux":
        raise ValueError("the bundled FFmpeg recipe currently supports Linux only")
    for tool in ("make", "tar", "readelf", "patchelf"):
        if not shutil.which(tool):
            raise ValueError(f"building the FFmpeg dependency requires {tool}")
    compiler = os.environ.get("CC", "cc")
    compiler_version = run(shlex.split(compiler) + ["--version"], capture_output=True, text=True).stdout
    recipe = {"source": PIN, "configure": configuration(prefix),
              "recipe_sha256": digest(Path(__file__)), "compiler": compiler,
              "notice_sha256": digest(ROOT / "third_party/ffmpeg/NOTICE.txt"),
              "compiler_version": compiler_version,
              "patchelf_version": run(["patchelf", "--version"], capture_output=True, text=True).stdout.strip(),
              "post_install": "patchelf --set-rpath '$ORIGIN' lib/libavcodec.so.* lib/libavutil.so.* (regular files only)",
              "platform": platform.machine()}
    marker = prefix / MARKER
    if not rebuild and marker.exists() and json.loads(marker.read_text()) == recipe:
        verify(prefix)
        print(f"FFmpeg {PIN['version']}: verified cached LGPL build at {prefix}")
        return
    if prefix.exists() and not marker.exists():
        raise ValueError(f"refusing to replace an unowned prefix: {prefix}")
    source_archive = archive()
    DEPS.mkdir(parents=True, exist_ok=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    # Stage on the destination filesystem, including with a custom --prefix.
    with tempfile.TemporaryDirectory(prefix="ffmpeg-build-", dir=prefix.parent) as scratch:
        scratch = Path(scratch)
        source, obj, stage = [scratch / name for name in ("source", "obj", "stage")]
        for path in (source, obj, stage):
            path.mkdir()
        run(["tar", "-xJf", source_archive, "--strip-components=1", "-C", source])
        env = os.environ.copy()
        # External build flags must not silently change the pinned recipe.
        for name in ("CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS", "LD_LIBRARY_PATH"):
            env.pop(name, None)
        jobs = os.environ.get("JOBS", str(min(os.cpu_count() or 1, 8)))
        if not jobs.isdecimal() or int(jobs) < 1:
            raise ValueError("JOBS must be a positive integer")
        log_path = DEPS / "ffmpeg-build.log"
        print(f"Building FFmpeg {PIN['version']} (log: {log_path})", flush=True)
        with log_path.open("w") as log:
            run([source / "configure", *configuration(prefix), "--cc=" + compiler],
                cwd=obj, env=env, stdout=log, stderr=subprocess.STDOUT)
            run(["make", "-j" + jobs], cwd=obj, env=env, stdout=log, stderr=subprocess.STDOUT)
            run(["make", "install", "DESTDIR=" + str(stage)], cwd=obj, env=env,
                stdout=log, stderr=subprocess.STDOUT)
        installed = stage / prefix.relative_to("/")
        # Apply after installation to avoid configure/make/shell dollar
        # expansion. RUNPATH remains overridable by LD_LIBRARY_PATH.
        for lib in (installed / "lib").glob("*.so.*"):
            if not lib.is_symlink():
                run(["patchelf", "--set-rpath", "$ORIGIN", lib])
        license_dir = installed / "share/licenses/ffmpeg"
        license_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "COPYING.LGPLv2.1", license_dir)
        shutil.copy2(ROOT / "third_party/ffmpeg/NOTICE.txt", license_dir)
        (installed / MARKER).write_text(json.dumps(recipe, indent=2) + "\n")
        verify(installed)
        previous = scratch / "previous"
        if prefix.exists():
            prefix.rename(previous)
        try:
            installed.rename(prefix)
            verify(prefix)
        except BaseException:
            if prefix.exists():
                prefix.rename(scratch / "failed-install")
            if previous.exists():
                previous.rename(prefix)
            raise
    print(f"Installed and verified LGPL libraries: {prefix}")


def bundle(prefix, output):
    report = verify(prefix)
    recipe = json.loads((prefix / MARKER).read_text())
    if recipe["source"] != PIN:
        raise ValueError("installed FFmpeg does not match the pinned source")
    if (recipe["recipe_sha256"] != digest(Path(__file__)) or
            recipe["notice_sha256"] != digest(ROOT / "third_party/ffmpeg/NOTICE.txt")):
        raise ValueError("build recipe or notices changed; run the build command before bundling")
    if output.exists():
        raise ValueError(f"bundle output already exists: {output}; choose a new directory")
    source_archive = archive()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ffmpeg-bundle-", dir=output.parent) as scratch:
        stage = Path(scratch) / "bundle"
        libdir = stage / "lib"
        libdir.mkdir(parents=True)
        for name in ("avcodec", "avutil"):
            for lib in (prefix / "lib").glob(f"lib{name}.so*"):
                shutil.copy2(lib, libdir / lib.name, follow_symlinks=False)
        shutil.copytree(prefix / "share/licenses/ffmpeg", stage / "licenses/ffmpeg")
        source = stage / "source"
        rebuild = source / "rebuild"
        (rebuild / "player").mkdir(parents=True)
        shutil.copy2(source_archive, source)
        shutil.copy2(__file__, rebuild / "player/ffmpeg.py")
        shutil.copytree(ROOT / "third_party/ffmpeg", rebuild / "third_party/ffmpeg")
        (source / "BUILD.json").write_text(json.dumps(recipe, indent=2) + "\n")
        (source / "CHANGES.txt").write_text("No modifications to the upstream FFmpeg source.\n")
        (source / "README.txt").write_text(
            "Exact source and recipe for the accompanying FFmpeg libraries.\n"
            "Requires Linux, Python 3.9+, a C compiler, make, tar, xz, readelf and patchelf.\n"
            "To rebuild offline, from this source directory:\n"
            "  mkdir -p rebuild/build/deps/downloads\n"
            f"  cp ffmpeg-{PIN['version']}.tar.xz rebuild/build/deps/downloads/\n"
            "  python3 rebuild/player/ffmpeg.py build\n"
            "The output is rebuild/build/deps/ffmpeg/. BUILD.json records the\n"
            "original compiler and configure options. No NASM is required.\n")
        (stage / "README.txt").write_text(
            "FFmpeg audio dependency bundle for a psprecomp game (not a complete game app).\n"
            "Place lib/ beside the host executable; it searches $ORIGIN/lib.\n"
            "Keep licenses/ with the application. Offer source/ alongside the\n"
            "binary download, and add the FFmpeg attribution/source link to the\n"
            "download page and application's third-party notices.\n"
            "Users may replace lib/ with modified interface-compatible libraries.\n")
        report = verify(stage)
        # The manifest must not retain the temporary staging directory.
        for name in ("avcodec", "avutil"):
            report[name]["path"] = "lib/" + Path(report[name]["path"]).name
        (stage / "manifest.json").write_text(json.dumps({"source": PIN, "libraries": report}, indent=2) + "\n")
        files = sorted(p for p in stage.rglob("*") if p.is_file() and not p.is_symlink())
        (stage / "SHA256SUMS").write_text("".join(f"{digest(p)}  {p.relative_to(stage)}\n" for p in files))
        stage.rename(output)
    print(f"Staged relocatable FFmpeg libraries, notices and matching source: {output}")


def main():
    global DEPS, PREFIX
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "verify", "bundle", "_probe"))
    parser.add_argument("--deps", type=Path, default=DEPS,
                        help="downloads, build log and the default prefix (default: build/deps here)")
    parser.add_argument("--prefix", type=Path, help="install prefix (default: <deps>/ffmpeg)")
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("--output", type=Path, help="bundle directory (default: <deps>/../ffmpeg-bundle)")
    args = parser.parse_args()
    DEPS = args.deps.resolve()
    PREFIX = DEPS / "ffmpeg"
    prefix = (args.prefix or PREFIX).resolve()
    if args.output is None:
        args.output = DEPS.parent / "ffmpeg-bundle"
    try:
        if args.command == "build":
            build(prefix, args.rebuild)
        elif args.command == "bundle":
            bundle(prefix, args.output.resolve())
        elif args.command == "_probe":
            probe(prefix)
        else:
            print(json.dumps(verify(prefix), indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"FFmpeg: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
