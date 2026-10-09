"""Bundled preparation tools; all inputs are pinned, offline source archives."""
import os
import json
import re
import shutil
import sys
from game_fingerprints import app_identity


def build_tools(b):
    zlib = b.unpack("zlib-1.3.1.tar.gz", "zlib")
    b.run(["./configure", "--static", f"--prefix={b.DEPS}"], cwd=zlib,
          env={**os.environ, "CFLAGS": "-O2 -fPIC -march=x86-64"})
    b.run(["make", "-j" + b.JOBS, "install"], cwd=zlib)
    ssl = b.unpack("openssl-3.5.8.tar.gz", "openssl")
    b.run(["./Configure", "linux-x86_64", "no-shared", "no-tests", "no-module",
           f"--prefix={b.DEPS}", "--libdir=lib", "-fPIC"], cwd=ssl)
    b.run(["make", "-j" + b.JOBS, "build_libs"], cwd=ssl)
    b.run(["make", "install_dev"], cwd=ssl)
    decrypt = b.unpack("pspdecrypt-c156627.tar.gz", "pspdecrypt")
    b.run(["make", "-j" + b.JOBS, "CC=cc", "CXX=c++", f"CFLAGS=-O2 -I{b.DEPS}/include",
           f"CXXFLAGS=-O2 -I{b.DEPS}/include", f"EXTRA_FLAG=-L{b.DEPS}/lib -lpthread -ldl"], cwd=decrypt)
    python = b.unpack("Python-3.12.13.tar.xz", "python")
    # Local filesystem/process/JSON tools only; omit modules needing optional
    # system libraries or network TLS. hashlib uses CPython's built-in SHA2.
    (python / "Modules/Setup.local").write_text(
        "*disabled*\n_ssl\n_hashlib\n_bz2\n_lzma\n_sqlite3\n_tkinter\nreadline\n_uuid\n"
        "_gdbm\n_dbm\n_ctypes\n_curses\n_curses_panel\n_crypt\nnis\n")
    prefix = b.BUILD / "python-prefix"
    env = {**os.environ, "CPPFLAGS": f"-I{b.DEPS}/include", "LDFLAGS": f"-L{b.DEPS}/lib"}
    b.run(["./configure", f"--prefix={prefix}", "--without-ensurepip", "--disable-test-modules",
           "--without-static-libpython"], cwd=python, env=env)
    b.run(["make", "-j" + b.JOBS], cwd=python, env=env)
    b.run(["make", "install"], cwd=python, env=env)
    zig = b.unpack("zig-x86_64-linux-0.15.2.tar.xz", "zig")
    return dict(zlib=zlib, ssl=ssl, decrypt=decrypt, python=python, prefix=prefix, zig=zig)


def stage_tools(b, tools, obj):
    resource = b.RESOURCES
    # The on-device recipes and what they read: psprecomp's and SDL's
    # headers, the recompiler's loader headers, the runtime and player
    # archives every game links, and the pack API marker each pack's
    # launcher.so is built with.
    for name in ("import_game.py", "compile_game.py", "emit-split.py", "pack.py"):
        b.copy(b.TOOLKIT / "player" / name, resource / name)
    b.copy(b.RECIPE / "game_fingerprints.py", resource / "game_fingerprints.py")
    shutil.copytree(b.TOOLKIT / "include", resource / "include")
    shutil.copytree(b.DEPS / "include/SDL2", resource / "include/SDL2")
    for header in sorted((b.TOOLKIT / "tools/allegrexrecomp").glob("*.h")):
        b.copy(header, resource / "recomp" / header.name)
    b.copy(obj / "libruntime.a", resource / "libruntime.a")
    b.copy(obj / "libplayer.a", resource / "libplayer.a")
    b.copy(b.TOOLKIT / "src/host/pack_api.c", resource / "pack_api.c")
    b.copy(b.TOOLKIT / "src/host/title_plain.c", resource / "title_plain.c")
    # Whole-app identity is provenance only. Game compatibility is recorded
    # separately below, after staging and relocating the actual build inputs.
    (resource / "build-id").write_text(b.sha(b.WORKDIR / "INPUTS.json") + "\n")
    for name in ("import-game", "run-game"):
        b.template(b.RECIPE / f"{name}.in", b.APP / "usr/bin" / name)
        (b.APP / "usr/bin" / name).chmod(0o755)
    b.copy(obj / "allegrexrecomp", b.APP / "usr/bin/allegrexrecomp")
    b.copy(tools["decrypt"] / "pspdecrypt", b.APP / "usr/bin/pspdecrypt")
    shutil.copytree(tools["zig"], b.APP / "usr/zig", symlinks=True)
    b.copy(tools["prefix"] / "bin/python3.12", b.APP / "usr/python/bin/python3.12")
    shutil.copytree(tools["prefix"] / "lib", b.APP / "usr/python/lib", symlinks=True,
        ignore=shutil.ignore_patterns("__pycache__", "test", "tests", "ensurepip", "idlelib", "tkinter"))
    # Relocate every dynamically linked helper and Python extension. The Zig
    # compiler is static and does not have a dynamic section to patch.
    for path in (b.APP / "usr").rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        if "(NEEDED)" not in b.capture(["readelf", "-d", path]):
            continue
        b.run(["strip", "--strip-unneeded", path])
        relative = os.path.relpath(b.APP / "usr/lib", path.parent)
        b.run(["patchelf", "--set-rpath", "$ORIGIN" + ("/" + relative if relative != "." else ""), path])
    for source, dest in ((tools["python"] / "LICENSE", "Python/LICENSE"),
                         (tools["decrypt"] / "LICENSE.TXT", "pspdecrypt/LICENSE.TXT"),
                         (tools["decrypt"] / "Readme.md", "pspdecrypt/Readme.md"),
                         (tools["ssl"] / "LICENSE.txt", "OpenSSL/LICENSE.txt"),
                         (tools["zlib"] / "LICENSE", "zlib/LICENSE"),
                         (tools["zig"] / "LICENSE", "Zig/LICENSE")):
        b.copy(source, b.APP / "licenses" / dest)
    b.run([b.APP / "usr/python/bin/python3.12", "-I", "-B", "-c",
           "import json, hashlib, subprocess, concurrent.futures, fcntl, zipfile, zlib; print(hashlib.sha256(b'check').hexdigest())"])
    abis = {}
    for name in ("SDL2", "SDL2_ttf", "openh264", "avcodec", "avutil"):
        dynamic = b.capture(["readelf", "-d", b.APP / f"usr/lib/lib{name}.so"])
        soname = re.findall(r"\(SONAME\).*\[(.*?)\]", dynamic)
        if len(soname) != 1:
            raise ValueError(f"Missing or ambiguous library ABI: {name}")
        abis[name] = soname[0]
    (resource / "app-build.json").write_text(json.dumps(app_identity(b.APP, abis), indent=2) + "\n")
