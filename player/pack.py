"""A title pack: everything about one game, or one family of games, that the
player needs -- as source and data, never game bytes. The player's app holds
no pack of its own; a player adds a pack's file to it, and the app builds
the pack on their machine (docs/PLAYER-LAYER.md, stage 10).

A pack is a directory with a pack.json. Its paths are relative to that
directory and may not leave it. The same manifest is checked where the pack
is built into its file (package-linux.py) and where it is added
(import_game.py, which stages this module in the app).

    {
      "version": 2,
      "pack": {
        "id":      "last-raven",              its section of the settings file and
                                              its folder; the id of its own app
                                              before there was one for every pack
        "name":    "Armored Core",            how the app names it
        "file":    "Armored-Core",            optional: its file, <file>-pack.zip
        "license": "LICENSE",
        "resources": {"FPS.md": "docs/FPS.md"}   optional documents shipped with it
      },
      "titles": [                             release order; the launcher's tabs
        {"slug": "aclr", "title": "...", "disc_id": "NPUH10024",
         "elf_sha256": "<64 hex>", "replacements": "replacements.c",
         "replace_list": "replace.txt", "codegen": ["fps-loop.py"]}
      ],
      "host": {
        "sources":  ["host/boot.c", ...],     the pack's host code, compiled when it
                                              is added and linked into each game
        "launcher": ["host/settings.c", "host/launcher_info.c"]
                                              its part of the launcher, launcher.so
      },
      "device": {                             compiled with each title's game
        "host":    ["host/replacements.c", ...],   replacements, replace lists, headers
        "scripts": ["scripts/fps-loop.py"]         code generators
      },
      "build": {
        "cmake": "packaging/linux/pack.cmake",  the pack's checks (see linux/CMakeLists.txt)
        "checks": [{"run": ["settings-tests"]}, ...]
      },
      "sources": ["host/launcher_tests.c", ...] everything else the checks and the
                                                pack's file must carry
    }

A title's "replacements" and "replace_list" name files in device.host by
their base name, and its "codegen" names files in device.scripts; each runs
after the emit as `python3 <script> <slug> <generated dir>`. Host sources
find their headers beside them, among the pack's files.
"""
import json
from pathlib import Path, PurePosixPath
import re

SLUG = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}")
SHA256 = re.compile(r"[0-9a-f]{64}")
VERSION = 2


class PackError(ValueError):
    pass


def _need(cond, message):
    if not cond:
        raise PackError(message)


def _text(table, key, where, optional=False):
    value = table.get(key)
    if value is None and optional:
        return None
    _need(isinstance(value, str) and value, f"{where}: '{key}' must be a non-empty string")
    return value


class Pack:
    def __init__(self, root):
        self.root = Path(root).resolve()
        manifest = self.root / "pack.json"
        _need(manifest.is_file(), f"{self.root}: no pack.json")
        try:
            data = json.loads(manifest.read_text())
        except (json.JSONDecodeError, UnicodeDecodeError) as error:
            raise PackError(f"{manifest}: {error}") from None
        _need(isinstance(data, dict) and data.get("version") == VERSION,
              f"{manifest}: version must be {VERSION}; this pack was made for another version of psprecomp")
        self.data = data

        pack = data.get("pack")
        _need(isinstance(pack, dict), "pack.json: 'pack' must be an object")
        self.id = _text(pack, "id", "pack")
        _need(SLUG.fullmatch(self.id), "pack: 'id' must be a lowercase folder name")
        self.name = _text(pack, "name", "pack")
        _need(re.fullmatch(r"[^'\\\n\"$`]+", self.name), "pack: 'name' may not contain quotes, $, ` or \\")
        self.file = _text(pack, "file", "pack", optional=True) or self.id
        _need(re.fullmatch(r"[A-Za-z0-9._-]+", self.file), "pack: 'file' must be a plain file name")
        self.license = self.path(_text(pack, "license", "pack"))
        resources = pack.get("resources", {})
        _need(isinstance(resources, dict), "pack: 'resources' must be an object")
        self.resources = {}
        for name, path in resources.items():
            _need(re.fullmatch(r"[A-Za-z0-9._-]+", name), f"pack.resources: bad name {name!r}")
            self.resources[name] = self.path(path)

        host = data.get("host")
        _need(isinstance(host, dict), "pack.json: 'host' must be an object")
        self.host_sources = [self.path(p) for p in self._list(host, "sources", "host")]
        self.launcher_sources = [self.path(p) for p in self._list(host, "launcher", "host")]
        _need(self.host_sources and self.launcher_sources, "host: 'sources' and 'launcher' must name files")
        for p in (*self.host_sources, *self.launcher_sources):
            _need(p.suffix == ".c", f"host: {p.name} is not a C source")

        device = data.get("device")
        _need(isinstance(device, dict), "pack.json: 'device' must be an object")
        self.device_host = [self.path(p) for p in self._list(device, "host", "device")]
        self.device_scripts = [self.path(p) for p in self._list(device, "scripts", "device", optional=True)]
        for group in (self.device_host, self.device_scripts):
            names = [p.name for p in group]
            _need(len(names) == len(set(names)), "device: file names must be unique")
        host_names = {p.name for p in self.device_host}
        script_names = {p.name for p in self.device_scripts}

        titles = data.get("titles")
        _need(isinstance(titles, list) and titles, "pack.json: 'titles' must be a non-empty list")
        self.titles = []
        for i, title in enumerate(titles):
            where = f"titles[{i}]"
            _need(isinstance(title, dict), f"{where} must be an object")
            entry = {key: _text(title, key, where) for key in
                     ("slug", "title", "disc_id", "elf_sha256", "replacements", "replace_list")}
            _need(SLUG.fullmatch(entry["slug"]), f"{where}: bad slug")
            _need(SHA256.fullmatch(entry["elf_sha256"]), f"{where}: 'elf_sha256' must be 64 hex digits")
            for key in ("replacements", "replace_list"):
                _need(entry[key] in host_names, f"{where}: '{key}' must name a file in device.host")
            codegen = title.get("codegen", [])
            _need(isinstance(codegen, list) and all(c in script_names for c in codegen),
                  f"{where}: 'codegen' must name files in device.scripts")
            entry["codegen"] = list(codegen)
            self.titles.append(entry)
        slugs = [t["slug"] for t in self.titles]
        _need(len(slugs) == len(set(slugs)), "titles: slugs must be unique")
        discs = [t["disc_id"] for t in self.titles]
        _need(len(discs) == len(set(discs)), "titles: disc IDs must be unique")

        build = data.get("build", {})
        _need(isinstance(build, dict), "pack.json: 'build' must be an object")
        cmake = _text(build, "cmake", "build", optional=True)
        self.cmake = self.path(cmake) if cmake else None
        checks = build.get("checks", [])
        _need(isinstance(checks, list), "build: 'checks' must be a list")
        for i, check in enumerate(checks):
            _need(isinstance(check, dict) and isinstance(check.get("run"), list) and check["run"]
                  and all(isinstance(a, str) for a in check["run"]), f"build.checks[{i}]: needs a 'run' list")
            _need(isinstance(check.get("env", {}), dict), f"build.checks[{i}]: 'env' must be an object")
        _need(not checks or self.cmake, "build: checks need a cmake file to build them")
        self.checks = checks

        self.sources = [self.path(p) for p in self._list(data, "sources", "pack.json", optional=True)]

    def _list(self, table, key, where, optional=False):
        value = table.get(key, [] if optional else None)
        _need(isinstance(value, list) and all(isinstance(v, str) for v in value),
              f"{where}: '{key}' must be a list of paths")
        return value

    def path(self, relative):
        _need(isinstance(relative, str) and relative and not relative.startswith("/")
              and "\\" not in relative and ".." not in PurePosixPath(relative).parts,
              f"pack path must be relative: {relative!r}")
        full = (self.root / relative).resolve()
        _need(full.is_relative_to(self.root), f"pack path leaves the pack: {relative}")
        _need(full.is_file(), f"pack file missing: {relative}")
        return full

    def files(self):
        """Every file the pack's file carries, relative to its root."""
        paths = {self.root / "pack.json", self.license, *self.resources.values(),
                 *self.host_sources, *self.launcher_sources, *self.device_host, *self.device_scripts,
                 *self.sources}
        if self.cmake:
            paths.add(self.cmake)
        return sorted(p.relative_to(self.root) for p in paths)

    def device_file(self, name):
        """A device.host file by the base name a title gives it."""
        return next(p for p in self.device_host if p.name == name)

    def profiles(self):
        """The titles in release order, each with its pack's id."""
        return [dict(t, pack=self.id) for t in self.titles]


def load(root):
    return Pack(root)
