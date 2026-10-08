#!/usr/bin/env python3
"""Internal offline build, invoked in the private container by
player/package-linux.py. The container's /work holds this toolkit's sources
under psprecomp/ and the pack's under pack/ (player/pack.py)."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import preparation

WORKDIR = Path(__file__).resolve().parents[3]
TOOLKIT = WORKDIR / "psprecomp"
sys.path.insert(0, str(TOOLKIT / "player"))
import pack as packs  # noqa: E402

PACK = packs.load(WORKDIR / "pack")
BUILD = WORKDIR / "build"
DEPS = BUILD / "package-prefix"
SOURCES = BUILD / "package-source"
RELEASE = BUILD / "release"
APP = RELEASE / PACK.appdir
RESOURCES = APP / "usr/share" / PACK.id
RECIPE = TOOLKIT / "player/linux"
DOWNLOADS = Path("/downloads")
FFMPEG = BUILD / "deps/ffmpeg"
JOBS = "8"
LOG = BUILD / "package-build.log"


def run(args, **kw):
    with LOG.open("a") as log:
        log.write("\n$ " + " ".join(map(str, args)) + "\n")
        log.flush()
        subprocess.run(list(map(str, args)), check=True, stdout=log, stderr=subprocess.STDOUT, **kw)


def capture(args):
    return subprocess.check_output(list(map(str, args)), text=True)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def unpack(archive, name):
    dest = SOURCES / name
    dest.mkdir(parents=True)
    run(["tar", "-xf", DOWNLOADS / archive, "--strip-components=1", "-C", dest])
    return dest


def cmake(source, name, flags):
    obj = BUILD / name
    run(["cmake", "-S", source, "-B", obj, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={DEPS}", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic",
         "-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic", *flags])
    run(["cmake", "--build", obj, "--parallel", JOBS])
    run(["cmake", "--install", obj])
    return obj


def copy(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest, follow_symlinks=False)


def template(src, dest):
    """A player/linux/*.in file with the pack's names filled in."""
    text = src.read_text()
    for key, value in (("@APP_ID@", PACK.id), ("@APP_NAME@", PACK.name),
                       ("@LEGACY_SETTINGS@", PACK.legacy_settings)):
        text = text.replace(key, value)
    if re.search(r"@[A-Z_]+@", text):
        raise ValueError(f"unfilled placeholder in {src.name}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text)


def check(command, ui, font, obj):
    """One of the pack's build checks: {ui}, {font} and {bin} are filled in."""
    fill = lambda s: s.replace("{ui}", str(ui)).replace("{font}", str(font)).replace("{bin}", str(obj))
    args = [fill(a) for a in command["run"]]
    args[0] = obj / args[0] if "/" not in args[0] else Path(args[0])
    env = {**os.environ, **{k: fill(v) for k, v in command.get("env", {}).items()}}
    if command.get("xvfb"):
        args = ["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", *args]
    run(args, env=env)


def audit():
    # libc and the C++ ABI stay with the OS, as do graphics/audio drivers.
    system = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
              "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2"}
    bundled = {p.name for p in (APP / "usr/lib").iterdir()}
    for link in APP.rglob("*"):
        if link.is_symlink() and (os.path.isabs(os.readlink(link)) or
                                  not link.resolve().is_relative_to(APP) or not link.exists()):
            raise ValueError(f"nonrelocatable or broken symlink: {link}")
    report = {}
    for path in sorted((APP / "usr").rglob("*")):
        if not path.is_file() or path.is_symlink() or path.read_bytes()[:4] != b"\x7fELF":
            continue
        dynamic = capture(["readelf", "-d", path])
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        unknown = set(needed) - system - bundled
        if unknown:
            raise ValueError(f"unbundled dependencies in {path.name}: {sorted(unknown)}")
        if "(NEEDED)" not in dynamic:
            report[str(path.relative_to(APP))] = {"static": True}
            continue
        relative = os.path.relpath(APP / "usr/lib", path.parent)
        expected = "$ORIGIN" + ("/" + relative if relative != "." else "")
        runpath = re.findall(r"\(RUNPATH\).*\[(.*?)\]", dynamic)
        if runpath != [expected]:
            raise ValueError(f"nonrelocatable RUNPATH in {path}: {runpath}")
        versions = capture(["readelf", "--version-info", path])
        glibc = sorted({tuple(map(int, v.split('.'))) for v in re.findall(r"GLIBC_([0-9.]+)", versions)})
        cxx = sorted({tuple(map(int, v.split('.'))) for v in re.findall(r"GLIBCXX_([0-9.]+)", versions)})
        if glibc and glibc[-1] > (2, 35):
            raise ValueError(f"{path.name} requires glibc newer than 2.35")
        if cxx and cxx[-1] > (3, 4, 29):
            raise ValueError(f"{path.name} requires a newer C++ ABI than GCC 11")
        linked = capture(["ldd", path])
        if "not found" in linked:
            raise ValueError(f"unresolved dependencies: {linked}")
        for line in linked.splitlines():
            match = re.search(r"(\S+) => (\S+)", line)
            if match and match[1] in bundled and not Path(match[2]).resolve().is_relative_to(APP):
                raise ValueError(f"library escaped package: {line}")
        report[str(path.relative_to(APP))] = {"needed": needed, "runpath": runpath,
            "glibc": '.'.join(map(str, glibc[-1])) if glibc else None,
            "glibcxx": '.'.join(map(str, cxx[-1])) if cxx else None}
    return report


def main():
    BUILD.mkdir(exist_ok=True)
    RELEASE.mkdir()
    os.environ["PKG_CONFIG_PATH"] = str(DEPS / "lib/pkgconfig")
    # The unprivileged namespace maps only one uid; archive owners are not
    # meaningful here (including archives extracted by the FFmpeg recipe).
    os.environ["TAR_OPTIONS"] = "--no-same-owner"
    print("Building pinned SDL, font renderer, OpenH264 and FFmpeg...", flush=True)
    sdl = unpack("SDL2-2.32.10.tar.gz", "sdl")
    cmake(sdl, "sdl-build", ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF",
        "-DSDL_TESTS=OFF", "-DSDL_X11=ON", "-DSDL_X11_SHARED=ON", "-DSDL_WAYLAND=ON",
        "-DSDL_WAYLAND_SHARED=ON", "-DSDL_KMSDRM=OFF", "-DSDL_PIPEWIRE=OFF",
        "-DSDL_PULSEAUDIO=ON", "-DSDL_PULSEAUDIO_SHARED=ON", "-DSDL_ALSA_SHARED=ON"])
    ttf = unpack("SDL2_ttf-2.24.0.tar.gz", "ttf")
    cmake(ttf, "ttf-build", [f"-DCMAKE_PREFIX_PATH={DEPS}", "-DBUILD_SHARED_LIBS=ON",
        "-DSDL2TTF_VENDORED=ON", "-DSDL2TTF_HARFBUZZ=OFF", "-DSDL2TTF_SAMPLES=OFF"])
    h264 = unpack("openh264-2.6.0.tar.gz", "openh264")
    run(["make", "-j" + JOBS, "OS=linux", "ARCH=x86_64", "USE_ASM=No", "BUILDTYPE=Release",
         "libraries"], cwd=h264)
    run(["make", "OS=linux", "ARCH=x86_64", "USE_ASM=No", "BUILDTYPE=Release",
         f"PREFIX={DEPS}", "install-shared"], cwd=h264)
    ff = json.loads((TOOLKIT / "third_party/ffmpeg/source.json").read_text())
    cached = BUILD / "deps/downloads"
    cached.mkdir(parents=True)
    copy(DOWNLOADS / f"ffmpeg-{ff['version']}.tar.xz", cached / f"ffmpeg-{ff['version']}.tar.xz")
    run(["python3", TOOLKIT / "player/ffmpeg.py", "build", "--deps", BUILD / "deps"])
    print("Building the bundled game preparation tools...", flush=True)
    tools = preparation.build_tools(sys.modules[__name__])
    font = unpack("dejavu-fonts-ttf-2.37.tar.bz2", "font")
    print("Building the launcher and running settings/UI checks...", flush=True)
    obj = BUILD / "launcher-build"
    run(["cmake", "-S", RECIPE, "-B", obj, "-G", "Ninja", f"-DDEPS={DEPS}", f"-DFFMPEG={FFMPEG}",
         f"-DTOOLKIT={TOOLKIT}", f"-DPACK={PACK.root}", f"-DPACK_CMAKE={PACK.cmake}",
         f"-DPACK_SLUGS={';'.join(t['slug'] for t in PACK.titles)}",
         "-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic"])
    run(["cmake", "--build", obj, "--parallel", JOBS])
    # The shared host's own checks, then the pack's.
    run([obj / "present-tests"])
    for mode in ("keyboard", "controller"):
        # The game's software presentation requests an accelerated SDL
        # renderer, which the dummy driver does not provide in this build.
        run(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", obj / "present-tests", mode],
            env={**os.environ, "SDL_AUDIODRIVER": "dummy"})
    ui = BUILD / "ui-checks"
    ui.mkdir()
    run([obj / "savedata-tests"])
    run([obj / "player-launcher-tests"], env={**os.environ, "SDL_VIDEODRIVER": "dummy",
        "PSPRECOMP_UI_FONT": str(font / "ttf/DejaVuSans.ttf")})
    for command in PACK.checks:
        check(command, ui, font / "ttf/DejaVuSans.ttf", obj)
    print("Staging only app binaries, dependencies, font and notices...", flush=True)
    (APP / "usr/lib").mkdir(parents=True)
    for name in ("launcher", "settings-tool"):
        copy(obj / name, APP / "usr/bin" / name)
        run(["strip", APP / "usr/bin" / name])
        run(["patchelf", "--set-rpath", "$ORIGIN/../lib", APP / "usr/bin" / name])
    for prefix, patterns in [(DEPS, ("libSDL2*.so*", "libopenh264.so*")),
                             (FFMPEG, ("libavcodec.so*", "libavutil.so*"))]:
        for pattern in patterns:
            for lib in (prefix / "lib").glob(pattern):
                copy(lib, APP / "usr/lib" / lib.name)
    for lib in (APP / "usr/lib").iterdir():
        if not lib.is_symlink():
            run(["patchelf", "--set-rpath", "$ORIGIN", lib])
    copy(font / "ttf/DejaVuSans.ttf", RESOURCES / "DejaVuSans.ttf")
    icon = unpack("adwaita-icon-theme-48.0.tar.xz", "adwaita")
    copy(icon / "Adwaita/scalable/mimetypes/application-x-executable.svg", APP / f"{PACK.id}.svg")
    template(RECIPE / "AppRun.in", APP / "AppRun")
    copy(PACK.desktop, APP / PACK.desktop.name)
    (APP / "AppRun").chmod(0o755)
    (APP / ".DirIcon").symlink_to(f"{PACK.id}.svg")
    runtime = unpack("runtime-source.tar.gz", "appimage-runtime")
    fuse = unpack("fuse-3.15.0.tar.xz", "fuse")
    squash = unpack("squashfuse-0.5.2.tar.gz", "squashfuse")
    notices = [(PACK.license, f"{PACK.id}/LICENSE"),
        (TOOLKIT / "LICENSE", "psprecomp/LICENSE"),
        (TOOLKIT / "third_party/stb/LICENSE", "stb/LICENSE"),
        (TOOLKIT / "third_party/imgui/LICENSE.txt", "imgui/LICENSE.txt"),
        (sdl / "LICENSE.txt", "SDL/LICENSE.txt"), (ttf / "LICENSE.txt", "SDL_ttf/LICENSE.txt"),
        (ttf / "external/freetype/docs/FTL.TXT", "FreeType/FTL.TXT"),
        (h264 / "LICENSE", "OpenH264/LICENSE"), (font / "LICENSE", "DejaVu/LICENSE"),
        (icon / "COPYING", "Adwaita/COPYING"), (runtime / "LICENSE", "AppImage/LICENSE"),
        (fuse / "LGPL2.txt", "AppImage/libfuse-LGPL2.txt"),
        (squash / "LICENSE", "AppImage/squashfuse-LICENSE"),
        *[(DOWNLOADS / name, "AppImage/" + name) for name in
          ("musl-COPYRIGHT", "zstd-LICENSE", "zlib-LICENSE")]]
    for src, dest in notices:
        copy(src, APP / "licenses" / dest)
    shutil.copytree(FFMPEG / "share/licenses/ffmpeg", APP / "licenses/ffmpeg")
    preparation.stage_tools(sys.modules[__name__], tools, obj)
    copy(PACK.readme, APP / "README.txt")
    for name, source in PACK.resources.items():
        copy(source, RESOURCES / name)
    # What the staged app's tests and scripts read about the pack.
    (RESOURCES / "app.json").write_text(json.dumps(PACK.app_info(), indent=2) + "\n")
    # The original font rendering credit is required by the FreeType license.
    (APP / "licenses/THIRD-PARTY.txt").write_text(
        "This software uses FFmpeg under the LGPL v2.1 or later. https://ffmpeg.org/\n"
        "Exact source and rebuild instructions accompany the AppImage in the sources archive.\n"
        "Portions of this software are copyright (C) 1996-2024 The FreeType Project\n"
        "(www.freetype.org). All rights reserved. FreeType is used under the FreeType License.\n"
        "Icon: GNOME Project (https://gnome.org), Adwaita 48.0, unchanged, CC BY-SA 3.0 US.\n"
        "https://creativecommons.org/licenses/by-sa/3.0/us/\n"
        "See each dependency's directory for its original notices.\n")
    manifest = {"kind": "game-import-preview", "architecture": "x86_64", "glibc_maximum": "2.35",
                "contains_game_code": False, "game_import_available": True,
                "game_builds": json.loads((RESOURCES / "game-builds.json").read_text()),
                "elf": audit(), "inputs": json.loads((WORKDIR / "INPUTS.json").read_text()),
                "dependencies": json.loads((RECIPE / "dependencies.json").read_text())}
    copy(Path("/build-packages.txt"), RELEASE / "build-packages.txt")
    (RELEASE / "BUILD.json").write_text(json.dumps(manifest, indent=2) + "\n")
    tests = TOOLKIT / "player/tests"
    run(["python3", tests / "test_package.py", APP])
    run([APP / "usr/python/bin/python3.12", "-I", "-B", tests / "test_import_game.py", APP])
    run(["python3", tests / "test_game_fingerprints.py"])
    # Xvfb exercises the actual SDL X11 backend at Deck screen dimensions.
    run(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", APP / "AppRun", "--check-startup"])
    print("Wrapping the validated AppDir into an AppImage...", flush=True)
    tool = BUILD / "appimagetool.AppImage"
    copy(DOWNLOADS / "appimagetool-x86_64.AppImage", tool)
    tool.chmod(0o755)
    tool_dir = BUILD / "appimage-tool"
    tool_dir.mkdir()
    run([tool, "--appimage-extract"], cwd=tool_dir)
    artifact = RELEASE / f"{PACK.artifact}-x86_64.AppImage"
    run([tool_dir / "squashfs-root/AppRun", "--no-appstream", "--runtime-file",
         DOWNLOADS / "runtime-x86_64", APP, artifact], env={**os.environ, "ARCH": "x86_64"})
    run([artifact, "--appimage-extract-and-run", "--check-startup"],
        env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
    print("Writing the matching source archive and build evidence...", flush=True)
    source = BUILD / "matching-source"
    source.mkdir()
    for path in WORKDIR.iterdir():
        if path.name == "build":
            continue
        if path.is_dir():
            shutil.copytree(path, source / path.name, ignore=shutil.ignore_patterns("__pycache__"))
        else:
            copy(path, source / path.name)
    shutil.copytree(DOWNLOADS, source / "build/package/downloads",
                    ignore=shutil.ignore_patterns("*.part", "initial-downloads.json"))
    run(["python3", TOOLKIT / "player/ffmpeg.py", "bundle", "--deps", BUILD / "deps",
         "--output", source / "ffmpeg"])
    copy(RELEASE / "BUILD.json", source / "BUILD.json")
    copy(RELEASE / "build-packages.txt", source / "build-packages.txt")
    (source / "REBUILD.txt").write_text(
        f"Matching source for {PACK.name}: the psprecomp toolkit (psprecomp/) and\n"
        "the title pack it was built with (pack/). From this directory, on an x86-64\n"
        "Linux host with Python 3.9+, curl, tar and Bubblewrap:\n\n"
        "  python3 psprecomp/player/package-linux.py build --pack pack \\\n"
        "      --work build/package --output build/release\n\n"
        "build/package/downloads holds every pinned archive, so only the Ubuntu\n"
        "builder's packages need the network. ffmpeg/ has FFmpeg's own source\n"
        "bundle; psprecomp/player/README.md describes the pipeline.\n")
    with tarfile.open(RELEASE / f"{PACK.artifact}-sources.tar.gz", "w:gz") as archive:
        archive.add(source, arcname=f"{PACK.id}-launcher-sources")
    copy(PACK.readme, RELEASE / "README.txt")
    copy(LOG, RELEASE / "build.log")
    shutil.copytree(ui, RELEASE / "ui-checks")
    files = sorted(p for p in RELEASE.rglob("*") if p.is_file() and not p.is_symlink())
    (RELEASE / "SHA256SUMS").write_text(''.join(f"{sha(p)}  {p.relative_to(RELEASE)}\n" for p in files))
    print(f"All checks passed: {artifact.name}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Package build failed: {error}", file=sys.stderr)
        if LOG.exists():
            print('\n'.join(LOG.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
        sys.exit(1)
