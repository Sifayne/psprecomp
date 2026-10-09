#!/usr/bin/env python3
"""Exercise the staged application outside its build tree, without a game
or a pack.

    test_package.py APPDIR"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

APP = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
ID = "psprecomp"
SETTINGS = "version=2\n\n[player]\nVOLUME=40\n"


def snapshot(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and not p.is_symlink()}


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="launcher package checks ")
        self.root = Path(self.tmp.name)
        self.app = self.root / "folder with spaces" / "psprecomp.AppDir"
        shutil.copytree(APP, self.app, symlinks=True)
        self.home = self.root / "user home"
        self.home.mkdir()
        self.cwd = self.root / "unrelated working directory"
        self.cwd.mkdir()
        self.env = {"HOME": str(self.home), "PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8",
                    "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
        self.config = self.home / ".config" / ID / "settings.ini"

    def tearDown(self):
        # Make copied read-only fixtures removable as an ordinary user.
        for p in self.root.rglob("*"):
            if not p.is_symlink():
                p.chmod(0o755 if p.is_dir() else 0o644)
        self.tmp.cleanup()

    def run_app(self, *args, success=True):
        r = subprocess.run([str(self.app / "AppRun"), *map(str, args)], cwd=self.cwd,
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=25)
        if success:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0)
        return r

    def write_settings(self, text=SETTINGS):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        self.config.write_text(text)

    def test_readonly_relocation_and_settings_survive_update(self):
        self.write_settings()
        before = self.config.read_bytes()
        image_before = snapshot(self.app)
        for p in self.app.rglob("*"):
            if not p.is_symlink():
                p.chmod(0o555 if p.is_dir() or os.access(p, os.X_OK) else 0o444)
        self.app.chmod(0o555)
        self.assertIn("Launcher startup OK", self.run_app("--check-startup").stdout)
        self.assertEqual(before, self.config.read_bytes())
        self.assertEqual(image_before, snapshot(self.app))
        relocated = self.root / "updated AppDir"
        # Moving a directory between parents changes its '..' entry. The
        # installed files stay read-only; permit just that filesystem move.
        self.app.chmod(0o755)
        self.app.rename(relocated)
        self.app = relocated
        self.app.chmod(0o555)
        self.assertIn("Launcher startup OK", self.run_app("--check-startup").stdout)
        self.assertEqual(before, self.config.read_bytes())
        self.assertEqual(list(self.cwd.iterdir()), [])
        self.assertTrue(list((self.home / ".local/state" / ID / "logs").glob("launcher-*.log")))

    def test_xdg_paths_and_invalid_relative_values(self):
        self.env.update(XDG_CONFIG_HOME=str(self.root / "config override"),
                        XDG_DATA_HOME=str(self.root / "data override"),
                        XDG_STATE_HOME=str(self.root / "state override"))
        paths = self.run_app("--print-paths").stdout
        self.assertIn(str(self.root / "config override" / ID / "settings.ini"), paths)
        self.assertIn(str(self.root / "data override" / ID / "games"), paths)
        self.assertIn(str(self.root / "data override" / ID / "states"), paths)
        self.run_app("--check-startup")
        self.assertFalse(self.config.exists())
        self.env["XDG_CONFIG_HOME"] = "relative-is-invalid"
        self.assertIn(str(self.config), self.run_app("--print-paths").stdout)

    def test_missing_library_fails_without_host_fallback(self):
        # No SDL development/runtime packages are installed in the builder.
        for p in (self.app / "usr/lib").glob("libSDL2-2.0*.so*"):
            p.unlink()
        self.run_app("--check-startup", success=False)

    def test_missing_bundled_font_falls_back_to_the_toolkit_font(self):
        # The launcher's text is Dear ImGui's; without the bundled font it
        # draws with ImGui's own rather than not starting.
        (self.app / "usr/share" / ID / "DejaVuSans.ttf").unlink()
        self.assertIn("Launcher startup OK", self.run_app("--check-startup").stdout)

    def test_malformed_preferences_are_preserved(self):
        self.write_settings("this is not a valid settings file\n")
        before = self.config.read_bytes()
        self.run_app("--check-startup", success=False)
        self.assertEqual(before, self.config.read_bytes())

    def test_no_game_payload_and_complete_notices(self):
        self.assertEqual({p.name for p in (self.app / "usr/bin").iterdir()},
                         {"launcher", "allegrexrecomp", "pspdecrypt", "import-game", "run-game"})
        self.assertFalse(list(self.app.rglob("pack.json")))
        forbidden = {".iso", ".elf", ".pbp", ".prx", ".at3", ".pad"}
        self.assertFalse([p for p in self.app.rglob("*") if p.suffix.lower() in forbidden])
        self.assertFalse(list(self.app.rglob("*_funcs*.c")))
        self.assertFalse(list(self.app.rglob("*_imports.c")))
        self.assertFalse(list(self.app.rglob("installed.json")))
        for name in ("ffmpeg/COPYING.LGPLv2.1", "SDL/LICENSE.txt", "SDL_ttf/LICENSE.txt",
                     "FreeType/FTL.TXT", "OpenH264/LICENSE", "DejaVu/LICENSE", "Adwaita/COPYING",
                     "Python/LICENSE", "pspdecrypt/LICENSE.TXT", "OpenSSL/LICENSE.txt", "Zig/LICENSE", "stb/LICENSE",
                     "imgui/LICENSE.txt"):
            self.assertTrue((self.app / "licenses" / name).is_file(), name)

    def test_empty_library_no_packs_and_missing_files(self):
        self.assertEqual(json.loads(self.run_app("--list-games").stdout)["games"], {})
        self.assertEqual(json.loads(self.run_app("--list-packs").stdout), {})
        self.run_app("--import", self.home / "missing.iso", success=False)
        self.run_app("--add-pack", self.home / "missing.zip", success=False)
        self.run_app("--remove-pack", "nothing", success=False)
        self.assertEqual(json.loads(self.run_app("--list-games").stdout)["games"], {})
        self.assertIn("Launcher startup OK", self.run_app("--check-startup").stdout)

    def test_steam_input_default_reaches_launcher_and_child(self):
        # Follow the real AppRun -> launcher -> child environment handoff.
        # SDL's synthetic joysticks bypass its Linux Steam-device filter, so
        # the ordinary virtual-controller fixture cannot catch this regression.
        launcher = self.app / "usr/bin/launcher"
        launcher.write_text('#!/bin/sh\nexec /bin/sh -c \'printf "%s" "$SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"\'\n')
        launcher.chmod(0o755)
        self.assertEqual(self.run_app("--check-startup").stdout, "1")
        self.env["SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"] = "0"
        self.assertEqual(self.run_app("--check-startup").stdout, "0")

    def test_controller_mappings_from_the_config_directory(self):
        # A gamecontrollerdb.txt beside the settings reaches SDL in the
        # launcher and its child; without one, SDL is told nothing; an
        # explicit setting is kept.
        launcher = self.app / "usr/bin/launcher"
        launcher.write_text('#!/bin/sh\nexec /bin/sh -c \'printf "%s" "$SDL_GAMECONTROLLERCONFIG_FILE"\'\n')
        launcher.chmod(0o755)
        self.assertEqual(self.run_app("--check-startup").stdout, "")
        mappings = self.config.parent / "gamecontrollerdb.txt"
        mappings.parent.mkdir(parents=True, exist_ok=True)
        mappings.write_text("# no mappings\n")
        self.assertEqual(self.run_app("--check-startup").stdout, str(mappings))
        self.assertIn(str(mappings), self.run_app("--print-paths").stdout)
        self.env["SDL_GAMECONTROLLERCONFIG_FILE"] = "/elsewhere/db.txt"
        self.assertEqual(self.run_app("--check-startup").stdout, "/elsewhere/db.txt")


if __name__ == "__main__":
    if APP is None or not (APP / "AppRun").is_file():
        sys.exit("usage: player/tests/test_package.py /path/to/<app>.AppDir")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
