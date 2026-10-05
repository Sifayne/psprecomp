#!/usr/bin/env python3
"""Prepare a user's ISO locally with the tools contained in the application.

No shell commands, network downloads, system compiler, or game bytes in the
package. Install records are committed only after a complete successful build.
Staged in usr/share/<pack id> with the pack's titles as games.json. The
LR_*_ROOT variables and the LRLIB1 library format are the contract with the
launcher, which is still Last Raven's (docs/PLAYER-LAYER.md, stage 2).
"""
import argparse
import importlib.util
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


class Library:
    def __init__(self, app, data, state, app_id):
        self.app, self.data, self.state = map(lambda p: Path(p).resolve(), (app, data, state))
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,31}", app_id or ""):
            raise ValueError("An app id (the pack's folder name) is required.")
        self.resources = self.app / "usr/share" / app_id
        self.profiles = json.loads((self.resources / "games.json").read_text())
        self.build_id = (self.resources / "build-id").read_text().strip()
        manifest = json.loads((self.resources / "game-builds.json").read_text())
        if not isinstance(manifest, dict) or manifest.get("version") != 1 or not isinstance(manifest.get("games"), dict):
            raise ValueError("This app has an invalid game build manifest.")
        self.game_builds = {}
        for profile in self.profiles:
            entry = manifest["games"].get(profile["slug"], {})
            fingerprint = entry.get("id") if isinstance(entry, dict) else None
            if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
                raise ValueError("This app is missing a valid game build fingerprint.")
            self.game_builds[profile["slug"]] = fingerprint
        self.record = self.data / "installed.json"
        self.env = os.environ.copy()
        self.env.setdefault("PSPRECOMP_UI_FONT", str(self.resources / "DejaVuSans.ttf"))
        for key in ("CC", "CFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
            self.env.pop(key, None)
        self.env["ZIG_GLOBAL_CACHE_DIR"] = str(self.data / "compiler-cache")
        self.env["ZIG_LOCAL_CACHE_DIR"] = str(self.data / "compiler-cache/local")
        # Scoped to our helpers and game process. User replacement libraries
        # retain precedence; drivers still come from the operating system.
        previous = self.env.get("LD_LIBRARY_PATH", "")
        self.env["LD_LIBRARY_PATH"] = (previous + ":" if previous else "") + str(self.app / "usr/lib")

    def records(self):
        if not self.record.exists():
            return {"version": 1, "selected": "", "games": {}}
        value = json.loads(self.record.read_text())
        if not isinstance(value, dict) or value.get("version") != 1 or not isinstance(value.get("games"), dict):
            raise ValueError("Cannot read the installed game list. It has been left unchanged.")
        if not isinstance(value.get("selected", ""), str):
            raise ValueError("Invalid selected game; the library has been left unchanged.")
        known = {p["slug"] for p in self.profiles}
        for slug, game in value["games"].items():
            if slug not in known or not isinstance(game, dict):
                raise ValueError("Invalid installed game entry; the library has been left unchanged.")
            for key in ("iso", "build_id", "directory"):
                if not isinstance(game.get(key), str):
                    raise ValueError("Incomplete installed game entry; import the ISO again.")
            directory = Path(game["directory"])
            if not directory.is_absolute() or not directory.resolve().is_relative_to(self.data / "games" / slug):
                raise ValueError("Installed game directory is outside the game library.")
        return value

    def compatible(self, slug, entry):
        # Old preview records lack this field and need preparation once. Never
        # infer runtime compatibility from the old whole-application build ID.
        return entry.get("game_build_id") == self.game_builds[slug]

    def ready(self, slug, entry):
        folder = Path(entry["directory"])
        try:
            iso = Path(entry["iso"]).stat()
            unchanged = iso.st_size == entry.get("iso_size") and iso.st_mtime_ns == entry.get("iso_mtime_ns")
        except OSError:
            unchanged = False
        return (self.compatible(slug, entry) and unchanged and (folder / "game").is_file()
                and os.access(folder / "game", os.X_OK) and (folder / "module.elf").is_file())

    def refresh(self):
        book = self.records()
        fields = ["LRLIB1", book.get("selected", "")]
        for profile in self.profiles:  # Release order: the pack's title order.
            slug = profile["slug"]
            if slug not in book["games"]:
                continue
            game = book["games"][slug]
            fields += [slug, profile["title"], str(self.app / "usr/bin/run-game") if self.ready(slug, game) else "",
                       str(Path(game["directory"]) / "module.elf"), game["iso"]]
        atomic_write(self.data / "library.bin", b"\0".join(x.encode() for x in fields) + b"\0")
        return book

    def import_iso(self, iso, jobs=2):
        iso = Path(iso).expanduser().resolve(strict=True)
        if not iso.is_file() or iso.suffix.lower() != ".iso":
            raise ValueError("Select a PSP .iso file. Compressed images must first be converted to ISO.")
        stat = iso.stat()
        if stat.st_size < 32768 or stat.st_size > 4 * 1024**3:
            raise ValueError("This file does not have a supported PSP ISO size.")
        self.data.mkdir(parents=True, exist_ok=True)
        with (self.data / ".import.lock").open("w") as guard:
            try:
                fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError("Another game is being prepared. Wait for it to finish.")
            return self._prepare(iso, stat, max(1, min(jobs, 4)))

    def _prepare(self, iso, stat, jobs):
        book = self.records()  # Read before doing work; never overwrite corrupt records.
        logs = self.state / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        log_path = logs / f"import-{time.time_ns()}.log"
        print(f"Checking your disc. Log: {log_path}", flush=True)
        staging = self.data / "preparing"
        staging.mkdir(exist_ok=True)
        if shutil.disk_usage(staging).free < 2 * 1024**3:
            raise ValueError("Game preparation needs at least 2 GB of free space in the library folder.")
        ar = self.app / "usr/bin/allegrexrecomp"
        with log_path.open("w") as log, tempfile.TemporaryDirectory(prefix="import-", dir=staging) as tmp:
            tmp = Path(tmp)

            def run(args, capture=False):
                log.write("\n$ " + " ".join(map(str, args)) + "\n")
                log.flush()
                p = subprocess.run(list(map(str, args)), cwd=tmp, env=self.env,
                    stdout=subprocess.PIPE if capture else log, stderr=log, text=True)
                if p.returncode:
                    raise ValueError(f"Preparation failed. See {log_path} for details.")
                if capture:
                    log.write(p.stdout)
                return p.stdout

            info = run([ar, "info", iso], capture=True)
            match = re.search(r"DISC_ID\s+(\S+)", info)
            disc_id = match[1] if match else "unknown"
            profile = next((p for p in self.profiles if p["disc_id"] == disc_id), None)
            if profile is None:
                expected = ", ".join(p["disc_id"] for p in self.profiles)
                raise ValueError(f"Unsupported disc ({disc_id}). This build supports {expected}.")
            print(f"Checking {profile['title']}...", flush=True)
            extracted = tmp / "extracted"
            extracted.mkdir()
            run([ar, "extract", iso, "SYSDIR/EBOOT.BIN", extracted])
            candidates = list(extracted.glob("*EBOOT.BIN"))
            if len(candidates) != 1 or candidates[0].stat().st_size > 32 * 1024**2:
                raise ValueError("Could not find a supported game executable on this disc.")
            module = tmp / "module.elf"
            if candidates[0].read_bytes()[:4] == b"\x7fELF":
                shutil.copy2(candidates[0], module)
            else:
                run([self.app / "usr/bin/pspdecrypt", "-o", module, candidates[0]])
            if digest(module) != profile["elf_sha256"]:
                raise ValueError(f"{profile['title']} has an unsupported executable version. Your library was not changed.")
            slug = profile["slug"]
            # Cache reuse still checks the disc and exact executable above.
            previous = book["games"].get(slug)
            if (previous and self.compatible(slug, previous)
                    and (Path(previous["directory"]) / "game").is_file()
                    and os.access(Path(previous["directory"]) / "game", os.X_OK)):
                destination = Path(previous["directory"])
                if not (destination / "module.elf").is_file() or digest(destination / "module.elf") != profile["elf_sha256"]:
                    previous = None
            else:
                previous = None
            if not previous:
                print("Preparing this game for the current runtime...", flush=True)
                spec = importlib.util.spec_from_file_location("game_compiler", self.resources / "compile_game.py")
                compiler = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(compiler)
                compiler.compile_game(self.app, profile, module, tmp / "game", jobs, self.env, log)
                directory = self.data / "games" / slug
                directory.mkdir(parents=True, exist_ok=True)
                destination = directory / (self.game_builds[slug][:16] + "-" + str(time.time_ns()))
                ready = tmp / "ready"
                ready.mkdir()
                shutil.move(tmp / "game", ready / "game")
                shutil.move(module, ready / "module.elf")
            after = iso.stat()
            if (after.st_size, after.st_mtime_ns) != (stat.st_size, stat.st_mtime_ns):
                raise ValueError("The ISO changed during preparation. Please import it again.")
            entry = dict(iso=str(iso), iso_size=stat.st_size, iso_mtime_ns=stat.st_mtime_ns,
                         build_id=previous["build_id"] if previous else self.build_id,
                         game_build_id=self.game_builds[slug], directory=str(destination),
                         elf_sha256=profile["elf_sha256"])
            if not previous:
                (ready / "build.json").write_text(json.dumps(entry, indent=2) + "\n")
                ready.rename(destination)
            book["games"][slug] = entry
            book["selected"] = slug
            atomic_write(self.record, (json.dumps(book, indent=2) + "\n").encode())
            self.refresh()
            print(f"Ready: {profile['title']}. Choose Save & Play.", flush=True)
            return slug

    def launch(self, module, iso, args):
        module = str(Path(module).resolve())
        args = list(args)
        for i, arg in enumerate(args):
            if arg == "--config" and i + 1 < len(args):
                args[i + 1] = str(Path(args[i + 1]).resolve())
            elif arg.startswith("--config="):
                args[i] = "--config=" + str(Path(arg.split("=", 1)[1]).resolve())
        book = self.records()
        found = next(((slug, entry) for slug, entry in book["games"].items()
                      if Path(entry["directory"]) / "module.elf" == Path(module)), None)
        if not found or not self.ready(*found):
            raise ValueError("This game needs preparation or its ISO has moved. Use Add Game to select the ISO again.")
        slug, entry = found
        save_root = self.data / "saves" / slug
        save_root.mkdir(parents=True, exist_ok=True)
        logs = self.state / "logs"; logs.mkdir(parents=True, exist_ok=True)
        log_path = logs / f"game-{slug}-{time.time_ns()}.log"
        print(f"Game log: {log_path}", file=sys.stderr, flush=True)
        executable = str(Path(entry["directory"]) / "game")
        # All user paths are absolute before changing the guest filesystem root.
        os.chdir(save_root)
        with log_path.open("w") as log:
            os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
        os.execve(executable, [executable, module, entry["iso"], *args], self.env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", default=os.environ.get("LR_APP_ROOT"))
    parser.add_argument("--app-id", required=True, help="the pack's id, usr/share/<id>")
    parser.add_argument("--data-root", default=os.environ.get("LR_DATA_ROOT"))
    parser.add_argument("--state-root", default=os.environ.get("LR_STATE_ROOT"))
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("refresh")
    commands.add_parser("list")
    prepare = commands.add_parser("import")
    prepare.add_argument("iso")
    prepare.add_argument("--jobs", type=int, default=2)
    launch = commands.add_parser("run")
    launch.add_argument("module"); launch.add_argument("iso"); launch.add_argument("args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not all((args.app_root, args.data_root, args.state_root)):
        parser.error("app, data and state paths are required; normally supplied by AppRun")
    try:
        library = Library(args.app_root, args.data_root, args.state_root, args.app_id)
        if args.command == "import":
            library.import_iso(args.iso, args.jobs)
        elif args.command == "run":
            library.launch(args.module, args.iso, args.args)
        elif args.command == "list":
            print(json.dumps(library.records(), indent=2))
        else:
            library.refresh()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    # The launcher cancels our whole process group, including compiler workers.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(130))
    sys.exit(main())
