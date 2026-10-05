"""A title pack: everything about one game, or one family of games, that the
player builds -- as source and data, never game bytes.

A pack is a directory with a pack.json. Its paths are relative to that
directory and may not leave it. See docs/PLAYER-LAYER.md, section 1, and the
example in the README next to this file.

    {
      "version": 1,
      "app": {
        "name":    "Armored Core Portable",     the application's display name
        "id":      "last-raven",                XDG folders, usr/share/<id>, icon
        "appdir":  "Last-Raven.AppDir",
        "artifact": "Armored-Core-Portable",    release file names
        "readme":  "packaging/linux/README.txt",
        "desktop": "packaging/linux/last-raven.desktop",
        "license": "LICENSE",
        "legacy_settings": "Last Raven/settings.ini",   optional, under XDG_DATA_HOME
        "check_preset": "Controller",           a starter preset the package test expects
        "resources": {"FPS.md": "docs/FPS.md"}  extra files for usr/share/<id>
      },
      "titles": [                               release order; the launcher's tabs
        {"slug": "aclr", "title": "...", "disc_id": "NPUH10024",
         "elf_sha256": "<64 hex>", "replacements": "replacements.c",
         "replace_list": "replace.txt", "codegen": ["fps-loop.py"]}
      ],
      "device": {                               compiled on the player's machine
        "host":    ["host/replacements.c", ...],   staged flat into usr/share/<id>/host
        "scripts": ["scripts/fps-loop.py"]         staged into usr/share/<id>; codegen
      },
      "build": {
        "cmake": "packaging/linux/pack.cmake",  the pack's targets (see linux/CMakeLists.txt)
        "checks": [{"run": ["settings-tests"]}, ...]
      },
      "sources": ["host/launcher.c", ...]       everything else the build and the
                                                matching source archive need
    }

A title's "replacements" and "replace_list" name files in device.host, and its
"codegen" names files in device.scripts; each runs after the emit as
`python3 <script> <slug> <generated dir>`.
"""
import json
from pathlib import Path
import re

SLUG = re.compile(r"[a-z0-9][a-z0-9_-]{0,31}")
SHA256 = re.compile(r"[0-9a-f]{64}")


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
        except json.JSONDecodeError as error:
            raise PackError(f"{manifest}: {error}") from None
        _need(isinstance(data, dict) and data.get("version") == 1, f"{manifest}: version must be 1")
        self.data = data

        app = data.get("app")
        _need(isinstance(app, dict), "pack.json: 'app' must be an object")
        self.name = _text(app, "name", "app")
        # It is written into shell quotes and desktop entries.
        _need(re.fullmatch(r"[^'\\\n\"$`]+", self.name), "app: 'name' may not contain quotes, $, ` or \\")
        self.id = _text(app, "id", "app")
        _need(SLUG.fullmatch(self.id), "app: 'id' must be a lowercase folder name")
        self.appdir = _text(app, "appdir", "app")
        _need(self.appdir.endswith(".AppDir") and "/" not in self.appdir, "app: 'appdir' must name an .AppDir")
        self.artifact = _text(app, "artifact", "app")
        _need(re.fullmatch(r"[A-Za-z0-9._-]+", self.artifact), "app: 'artifact' must be a plain file name")
        self.readme = self.path(_text(app, "readme", "app"))
        self.desktop = self.path(_text(app, "desktop", "app"))
        self.license = self.path(_text(app, "license", "app"))
        self.legacy_settings = _text(app, "legacy_settings", "app", optional=True) or ""
        _need(not self.legacy_settings.startswith("/") and ".." not in Path(self.legacy_settings).parts,
              "app: 'legacy_settings' must be relative to XDG_DATA_HOME")
        self.check_preset = _text(app, "check_preset", "app", optional=True) or ""
        resources = app.get("resources", {})
        _need(isinstance(resources, dict), "app: 'resources' must be an object")
        self.resources = {}
        for name, path in resources.items():
            _need(re.fullmatch(r"[A-Za-z0-9._-]+", name), f"app.resources: bad name {name!r}")
            self.resources[name] = self.path(path)

        device = data.get("device")
        _need(isinstance(device, dict), "pack.json: 'device' must be an object")
        self.device_host = [self.path(p) for p in self._list(device, "host", "device")]
        self.device_scripts = [self.path(p) for p in self._list(device, "scripts", "device", optional=True)]
        for group in (self.device_host, self.device_scripts):
            names = [p.name for p in group]
            _need(len(names) == len(set(names)), "device: staged file names must be unique")
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

        build = data.get("build")
        _need(isinstance(build, dict), "pack.json: 'build' must be an object")
        self.cmake = self.path(_text(build, "cmake", "build"))
        checks = build.get("checks", [])
        _need(isinstance(checks, list), "build: 'checks' must be a list")
        for i, check in enumerate(checks):
            _need(isinstance(check, dict) and isinstance(check.get("run"), list) and check["run"]
                  and all(isinstance(a, str) for a in check["run"]), f"build.checks[{i}]: needs a 'run' list")
            _need(isinstance(check.get("env", {}), dict), f"build.checks[{i}]: 'env' must be an object")
        self.checks = checks

        self.sources = [self.path(p) for p in self._list(data, "sources", "pack.json")]

    def _list(self, table, key, where, optional=False):
        value = table.get(key, [] if optional else None)
        _need(isinstance(value, list) and all(isinstance(v, str) for v in value),
              f"{where}: '{key}' must be a list of paths")
        return value

    def path(self, relative):
        _need(isinstance(relative, str) and relative and not relative.startswith("/"),
              f"pack path must be relative: {relative!r}")
        full = (self.root / relative).resolve()
        _need(full.is_relative_to(self.root), f"pack path leaves the pack: {relative}")
        _need(full.is_file(), f"pack file missing: {relative}")
        return full

    def files(self):
        """Every file the build reads from the pack, relative to its root."""
        paths = {self.root / "pack.json", self.readme, self.desktop, self.license, self.cmake,
                 *self.resources.values(), *self.device_host, *self.device_scripts, *self.sources}
        return sorted(p.relative_to(self.root) for p in paths)

    def profiles(self):
        """The importer's games.json: the titles in release order."""
        return [dict(t) for t in self.titles]

    def app_info(self):
        """What the staged app's scripts and tests read about the pack."""
        return {"name": self.name, "id": self.id, "appdir": self.appdir, "artifact": self.artifact,
                "legacy_settings": self.legacy_settings, "check_preset": self.check_preset}


def load(root):
    return Pack(root)
