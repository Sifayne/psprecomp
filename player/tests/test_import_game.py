#!/usr/bin/env python3
"""Failure, persistence and ordering checks; fixtures contain no game content.

    test_import_game.py [APP]

With an AppDir, the importer, pack loader and fingerprints under test are
the ones it staged; without one, this checkout's. Either way they run in a
fixture app whose build recipes are stand-ins, with a synthetic pack of
three titles: these checks are about the importer, not any pack's titles or
the compiler."""
import fcntl
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

APP = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
CHECKOUT = Path(__file__).resolve().parents[1]
STAGED = APP / 'usr/share/psprecomp' if APP else None
SOURCES = {name: (STAGED / name if STAGED else CHECKOUT / path) for name, path in
           (('import_game.py', 'import_game.py'), ('pack.py', 'pack.py'),
            ('game_fingerprints.py', 'linux/game_fingerprints.py'))}
spec = importlib.util.spec_from_file_location('importer', SOURCES['import_game.py'])
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)
FIRST, SECOND, THIRD = 'first', 'second', 'third'   # in release order

# Stand-ins for the on-device recipes: a pack "builds" to a launcher.so and
# one host object; a game is a shell script.
RECIPES = '''
from pathlib import Path
def build_pack(app, pack, out, env, log, jobs=2):
    out = Path(out); (out / 'host').mkdir(parents=True, exist_ok=True)
    (out / 'launcher.so').write_text('launcher of ' + pack.id)
    (out / 'host/boot.o').write_text('host of ' + pack.id)
    return [out / 'host/boot.o']
def compile_game(app, pack, host_objects, profile, module, output, jobs, env, log):
    Path(output).write_text('#!/bin/sh\\nexit 0\\n'); Path(output).chmod(0o755)
'''


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='game import checks ')
        self.root = Path(self.tmp.name)
        self.app = self.root / 'app with spaces'
        self.resources = self.app / 'usr/share/psprecomp'
        self.resources.mkdir(parents=True)
        for name, source in SOURCES.items():
            shutil.copy2(source, self.resources / name)
        (self.resources / 'compile_game.py').write_text(RECIPES)
        (self.resources / 'build-id').write_text('fixture-build')
        (self.resources / 'app-build.json').write_text(json.dumps({'version': 2, 'id': 'a' * 64}))
        self.module = b'\x7fELFsynthetic fixture, not a game'
        self.pack_dir = self.make_pack(self.root / 'pack source', 'fixture', 'Fixture',
                                       (FIRST, SECOND, THIRD))
        self.data = self.root / 'data' / 'psprecomp'
        self.library = self.library_now()
        self.add(self.pack_dir)
        self.iso = self.root / 'my disc & spaces |.iso'
        self.iso.write_bytes(b'fixture'.ljust(32768, b'\0'))
        self.disc = 'FIXT10000'

    def tearDown(self):
        self.tmp.cleanup()

    def library_now(self):
        return importer.Library(self.app, self.data, self.root / 'state')

    def make_pack(self, root, pack_id, name, slugs, version=2):
        root.mkdir(parents=True)
        (root / 'LICENSE').write_text('MIT')
        (root / 'host').mkdir()
        (root / 'host/boot.c').write_text('#include "shared.h"\nint boot;\n')
        (root / 'host/shared.h').write_text('#define SHARED 1\n')
        (root / 'host/settings.c').write_text('int settings;\n')
        (root / 'host/launcher_info.c').write_text('int info;\n')
        titles = []
        for i, slug in enumerate(slugs):
            (root / f'host/{slug}.c').write_text(f'#include "shared.h"\n#include "{slug}_funcs.h"\n')
            (root / f'host/{slug}.txt').write_text('00102018\n')
            titles.append(dict(slug=slug, title=slug.title() + ' Title', disc_id=f'FIXT1000{i}' if pack_id == 'fixture' else f'OTHR1000{i}',
                               elf_sha256=hashlib.sha256(self.module).hexdigest(),
                               replacements=f'{slug}.c', replace_list=f'{slug}.txt'))
        (root / 'pack.json').write_text(json.dumps({
            'version': version, 'pack': {'id': pack_id, 'name': name, 'license': 'LICENSE'},
            'titles': titles,
            'host': {'sources': ['host/boot.c'], 'launcher': ['host/settings.c', 'host/launcher_info.c']},
            'device': {'host': ['host/shared.h', *[f'host/{s}.{e}' for s in slugs for e in ('c', 'txt')]]}}))
        return root

    def zip_of(self, root, extra=None):
        path = root.with_suffix('.zip')
        with zipfile.ZipFile(path, 'w') as z:
            for p in sorted(root.rglob('*')):
                if p.is_file():
                    z.write(p, p.relative_to(root).as_posix())
            for name, data in (extra or {}).items():
                z.writestr(name, data)
        return path

    def add(self, root):
        self.library.install_pack(self.zip_of(root))
        self.library = self.library_now()

    def fake_run(self, args, **kwargs):
        if args[1] == 'info':
            return subprocess.CompletedProcess(args, 0, '  DISC_ID            ' + self.disc + '\n'
                                                        '  TITLE              A Game & "Its" Name\n')
        if args[1] == 'extract':
            Path(args[4], 'PSP_GAME_SYSDIR_EBOOT.BIN').write_bytes(self.module)
            return subprocess.CompletedProcess(args, 0, '')
        raise AssertionError('Unexpected helper invocation: ' + repr(args))

    def install(self, slug=FIRST):
        folder = self.data / 'games' / slug / 'previous build'
        folder.mkdir(parents=True)
        (folder / 'game').write_text('#!/bin/sh\nexit 0\n')
        (folder / 'game').chmod(0o755)
        (folder / 'module.elf').write_bytes(self.module)
        stat = self.iso.stat()
        entry = dict(directory=str(folder), iso=str(self.iso), build_id=self.library.build_id,
                     game_build_id=self.library.game_builds[slug],
                     iso_size=stat.st_size, iso_mtime_ns=stat.st_mtime_ns)
        book = self.library.records()
        book['games'][slug] = entry
        book['selected'] = slug
        self.library.record.write_text(json.dumps(book))
        return entry

    # ---- packs -----------------------------------------------------------------

    def test_pack_added_and_built(self):
        self.assertEqual(list(self.library.packs), ['fixture'])
        pack = self.library.packs['fixture']
        self.assertTrue(self.library.built(pack))
        self.assertEqual((pack.root / 'built/launcher.so').read_text(), 'launcher of fixture')
        self.assertEqual([p['slug'] for p in self.library.profiles], [FIRST, SECOND, THIRD])
        self.assertEqual(list((self.data / 'packs').iterdir()), [self.data / 'packs/fixture'])

    def test_bad_pack_files_change_nothing(self):
        before = sorted(p.relative_to(self.data) for p in self.data.rglob('*'))
        broken = self.root / 'broken.zip'; broken.write_bytes(b'not a zip')
        with self.assertRaisesRegex(ValueError, 'not a valid .zip'):
            self.library.install_pack(broken)
        with self.assertRaisesRegex(ValueError, 'may not'):
            self.library.install_pack(self.zip_of(self.make_pack(self.root / 'escape', 'escape', 'Escape', ('esc',)),
                                                  {'../outside.txt': 'no'}))
        with self.assertRaisesRegex(ValueError, 'another version'):
            self.library.install_pack(self.zip_of(self.make_pack(self.root / 'old', 'old', 'Old', ('old',), version=1)))
        with self.assertRaisesRegex(ValueError, 'already has'):
            self.library.install_pack(self.zip_of(self.make_pack(self.root / 'twin', 'twin', 'Twin', (FIRST,))))
        txt = self.root / 'pack.txt'; txt.write_text('x')
        with self.assertRaisesRegex(ValueError, ".zip"):
            self.library.install_pack(txt)
        self.assertEqual(before, sorted(p.relative_to(self.data) for p in self.data.rglob('*')))

    def test_pack_replaced_by_a_newer_one(self):
        (self.pack_dir / 'host/boot.c').write_text('#include "shared.h"\nint boot_v2;\n')
        old = dict(self.library.game_builds)
        self.add(self.pack_dir)
        self.assertNotEqual(old[FIRST], self.library.game_builds[FIRST])
        self.assertIn('boot_v2', (self.data / 'packs/fixture/host/boot.c').read_text())
        self.assertFalse([p for p in (self.data / 'packs').iterdir() if p.name.startswith('.')])

    def test_removed_pack_keeps_records_and_comes_back_ready(self):
        entry = self.install()
        self.library.refresh()
        self.library.remove_pack('fixture')
        self.assertFalse((self.data / 'packs/fixture').exists())
        self.assertEqual(self.library.records()['games'][FIRST], entry)
        self.assertEqual((self.data / 'library.bin').read_bytes(), b'LRLIB1\0first\0')
        with self.assertRaisesRegex(ValueError, 'not added'):
            self.library.remove_pack('fixture')
        self.add(self.pack_dir)
        self.library.refresh()
        self.assertTrue(self.library.ready(FIRST, entry))

    def test_stale_pack_rebuilt_on_refresh(self):
        built = self.data / 'packs/fixture/built'
        (built / 'build.json').write_text(json.dumps({'app': 'b' * 64, 'pack': 'old'}))
        (built / 'launcher.so').unlink()
        self.library.refresh()
        self.assertTrue((built / 'launcher.so').is_file())
        self.assertTrue(self.library.built(self.library.packs['fixture']))

    def test_earlier_app_data_moves_in_once(self):
        old = self.data.parent / 'other'
        (old / 'saves/oth/ms').mkdir(parents=True)
        (old / 'saves/oth/ms/save').write_text('keep my save')
        (old / 'states/oth').mkdir(parents=True)
        (old / 'states/oth/slot-1.state').write_text('state')
        game = old / 'games/oth/build-1'
        game.mkdir(parents=True)
        (game / 'game').write_text('old game')
        (old / 'installed.json').write_text(json.dumps({'version': 1, 'selected': 'oth', 'games': {
            'oth': dict(iso=str(self.iso), build_id='old', directory=str(game))}}))
        # Saves already here stay as they are.
        (self.data / 'saves/oth2/ms').mkdir(parents=True)
        (old / 'saves/oth2/ms').mkdir(parents=True)
        self.add(self.make_pack(self.root / 'other pack', 'other', 'Other', ('oth', 'oth2')))
        self.assertEqual((self.data / 'saves/oth/ms/save').read_text(), 'keep my save')
        self.assertEqual((old / 'saves/oth/ms/save').read_text(), 'keep my save')   # a copy, never moved
        self.assertTrue((self.data / 'states/oth/slot-1.state').is_file())
        entry = self.library.records()['games']['oth']
        self.assertEqual(Path(entry['directory']), self.data / 'games/oth/build-1')
        self.assertEqual((Path(entry['directory']) / 'game').read_text(), 'old game')
        self.assertTrue((old / 'saves/oth2').is_dir())
        note = (old / importer.MOVED_NOTE).read_text()
        self.assertIn('saves/oth', note)
        self.assertFalse(self.library.ready('oth', entry))      # prepared by the old app: once more
        # Not again: what the old folder holds later stays there.
        (old / 'saves/oth/ms/later').write_text('later')
        shutil.rmtree(self.data / 'saves/oth')
        self.add(self.root / 'other pack')
        self.assertFalse((self.data / 'saves/oth').exists())

    # ---- games -------------------------------------------------------------------

    def test_release_order_independent_of_import_order(self):
        for slug in (THIRD, SECOND, FIRST):
            self.install(slug)
        self.library.refresh()
        fields = (self.data / 'library.bin').read_bytes().split(b'\0')
        self.assertEqual(fields[2:-1:5], [FIRST.encode(), SECOND.encode(), THIRD.encode()])
        self.assertEqual(fields[1], FIRST.encode())

    def test_missing_compressed_and_short_files(self):
        with self.assertRaises(FileNotFoundError):
            self.library.import_iso(self.root / 'missing.iso')
        cso = self.root / 'disc.cso'; cso.write_bytes(self.iso.read_bytes())
        with self.assertRaisesRegex(ValueError, 'Select a PSP'):
            self.library.import_iso(cso)
        self.iso.write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'size'):
            self.library.import_iso(self.iso)
        self.assertFalse(self.library.record.exists())

    def test_unknown_disc_plays_plain(self):
        entry = self.install()
        self.disc = 'UNKN12345'
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.assertEqual(self.library.import_iso(self.iso), 'plain-unkn12345')
        book = self.library.records()
        plain = book['games']['plain-unkn12345']
        self.assertTrue(plain['plain'] and plain['disc_id'] == 'UNKN12345')
        self.assertEqual(plain['title'], 'A Game & "Its" Name')
        self.assertNotIn('pack', plain)
        self.assertEqual(book['games'][FIRST], entry)
        self.assertTrue(self.library.ready('plain-unkn12345', plain))
        fields = (self.data / 'library.bin').read_bytes().split(b'\0')
        self.assertEqual(fields[2:-1:5], [FIRST.encode(), b'plain-unkn12345'])   # the packs' first
        self.assertEqual(fields[8], 'A Game & "Its" Name'.encode())

    def test_other_version_of_a_pack_disc_plays_plain(self):
        entry = self.install()
        self.module += b'another version'
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.assertEqual(self.library.import_iso(self.iso), 'plain-fixt10000')
        book = self.library.records()
        self.assertEqual(book['games'][FIRST], entry)
        self.assertEqual(book['games']['plain-fixt10000']['elf_sha256'], hashlib.sha256(self.module).hexdigest())

    def test_without_packs_any_disc_plays_plain(self):
        self.library.remove_pack('fixture')
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.assertEqual(self.library.import_iso(self.iso), 'plain-fixt10000')
        self.assertTrue(self.library.ready('plain-fixt10000', self.library.records()['games']['plain-fixt10000']))

    def test_pack_added_takes_over_its_plain_game(self):
        self.library.remove_pack('fixture')
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.library.import_iso(self.iso)
        save = self.data / 'saves/plain-fixt10000/ms/save'
        save.parent.mkdir(parents=True); save.write_text('keep my save')
        self.add(self.pack_dir)
        book = self.library.records()
        self.assertNotIn('plain-fixt10000', book['games'])
        self.assertEqual(book['games'][FIRST]['pack'], 'fixture')
        self.assertEqual(book['selected'], FIRST)
        self.assertEqual((self.data / 'saves' / FIRST / 'ms/save').read_text(), 'keep my save')
        self.assertFalse(self.library.ready(FIRST, book['games'][FIRST]))   # prepared once with the pack
        fields = (self.data / 'library.bin').read_bytes().split(b'\0')
        self.assertEqual(fields[2:-1:5], [FIRST.encode()])
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.assertEqual(self.library.import_iso(self.iso), FIRST)
        self.assertTrue(self.library.ready(FIRST, self.library.records()['games'][FIRST]))

    def test_unsupported_executable_keeps_existing_game(self):
        entry = self.install()
        before = self.library.record.read_bytes()
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.disc = 'FIXT10000'
            self.iso.write_bytes(b'short')
            with self.assertRaisesRegex(ValueError, 'size'):
                self.library.import_iso(self.iso)
        self.assertEqual(before, self.library.record.read_bytes())
        self.assertTrue(Path(entry['directory'], 'game').exists())

    def test_corrupt_and_external_records_preserved(self):
        for content in ('bad json', '[]', '{"version":2,"games":{}}',
                        json.dumps({'version': 1, 'games': {FIRST: dict(iso=str(self.iso), build_id='x', directory='/tmp/outside')}}),
                        json.dumps({'version': 1, 'games': {'Bad Slug': {}}})):
            self.library.record.write_text(content)
            with self.assertRaises(ValueError):
                self.library.refresh()
            self.assertEqual(self.library.record.read_text(), content)

    def test_single_import_lock(self):
        with (self.data / '.import.lock').open('w') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, 'Another game'):
                self.library.import_iso(self.iso)
            with self.assertRaisesRegex(ValueError, 'being prepared'):
                self.library.install_pack(self.zip_of(self.pack_dir))

    def test_relocated_iso_reuses_verified_cache_and_preserves_saves(self):
        entry = self.install()
        save = self.data / 'saves' / FIRST / 'ms/PSP/SAVEDATA/save'
        save.parent.mkdir(parents=True); save.write_bytes(b'keep my save')
        moved = self.iso.with_name('relocated disc.iso')
        self.iso.rename(moved)
        self.assertFalse(self.library.ready(FIRST, entry))
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.assertEqual(self.library.import_iso(moved), FIRST)
        updated = self.library.records()['games'][FIRST]
        self.assertEqual(updated['directory'], entry['directory'])
        self.assertEqual(updated['iso'], str(moved))
        self.assertTrue(self.library.ready(FIRST, updated))
        self.assertEqual(save.read_bytes(), b'keep my save')

    def test_new_build_and_nonexecutable_cache_need_preparation(self):
        entry = self.install()
        self.assertTrue(self.library.ready(FIRST, entry))
        self.library.game_builds[FIRST] = 'f' * 64
        self.library.refresh()
        self.assertEqual((self.data / 'library.bin').read_bytes().split(b'\0')[4], b'')
        self.library.game_builds[FIRST] = entry['game_build_id']
        Path(entry['directory'], 'game').chmod(0o644)
        self.assertFalse(self.library.ready(FIRST, entry))

    def test_launcher_update_keeps_all_games_ready(self):
        for slug in (FIRST, SECOND, THIRD):
            self.install(slug)
        before = self.library.record.read_bytes()
        self.library.build_id = 'new launcher'
        self.library.refresh()
        self.assertEqual(before, self.library.record.read_bytes())
        for slug, entry in self.library.records()['games'].items():
            self.assertTrue(self.library.ready(slug, entry))
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.library.import_iso(self.iso)
        entry = self.library.records()['games'][FIRST]
        self.assertEqual(entry['build_id'], 'fixture-build')  # Keep original build provenance.
        self.assertEqual(Path(entry['directory']).name, 'previous build')

    def test_only_changed_title_needs_preparation(self):
        for slug in (FIRST, SECOND, THIRD):
            self.install(slug)
        # A change to the second title's replacements alone.
        (self.data / 'packs/fixture/host/second.c').write_text('#include "shared.h"\nint changed;\n')
        self.library = self.library_now()
        self.library.refresh()
        fields = (self.data / 'library.bin').read_bytes().split(b'\0')
        self.assertTrue(fields[4]); self.assertEqual(fields[9], b''); self.assertTrue(fields[14])
        for slug, entry in self.library.records()['games'].items():
            self.assertEqual(self.library.ready(slug, entry), slug != SECOND)
        # A change to the pack's host code: every title.
        (self.data / 'packs/fixture/host/shared.h').write_text('#define SHARED 2\n')
        self.library = self.library_now()
        self.assertFalse(any(self.library.ready(s, e) for s, e in self.library.records()['games'].items()))

    def test_legacy_record_requires_one_preparation_without_data_loss(self):
        entry = self.install()
        book = self.library.records()
        del book['games'][FIRST]['game_build_id']
        self.library.record.write_text(json.dumps(book))
        before = self.library.record.read_bytes()
        self.library.refresh()
        self.assertFalse(self.library.ready(FIRST, book['games'][FIRST]))
        self.assertEqual(before, self.library.record.read_bytes())
        self.assertTrue(Path(entry['directory'], 'game').exists())

    def test_invalid_app_build_manifest_is_rejected(self):
        for manifest in ({'version': 1, 'id': 'a' * 64}, {'version': 2}, {'version': 2, 'id': 'invalid'}):
            (self.resources / 'app-build.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                self.library_now()

    def test_runtime_rebuild_switches_only_after_success(self):
        old = self.install()
        sibling = self.install(SECOND)
        before = self.library.record.read_bytes()
        self.library.game_builds[FIRST] = 'f' * 64
        recipes = self.resources / 'compile_game.py'
        recipes.write_text(RECIPES + 'def compile_game(*args):\n    raise ValueError("compiler failed")\n')
        with patch.object(importer.subprocess, 'run', self.fake_run):
            with self.assertRaisesRegex(ValueError, 'compiler failed'):
                self.library.import_iso(self.iso)
        self.assertEqual(self.library.record.read_bytes(), before)
        self.assertTrue(Path(old['directory'], 'game').exists())
        self.assertEqual(list((self.data / 'preparing').iterdir()), [])
        recipes.write_text(RECIPES)
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.library.import_iso(self.iso)
        book = self.library.records()
        updated = book['games'][FIRST]
        self.assertNotEqual(updated['directory'], old['directory'])
        self.assertEqual(updated['game_build_id'], 'f' * 64)
        self.assertEqual(updated['pack'], 'fixture')
        self.assertTrue(self.library.ready(FIRST, updated))
        self.assertEqual(book['games'][SECOND], sibling)
        self.assertTrue(Path(old['directory'], 'game').exists())

    def test_interrupted_preparation_preserves_library(self):
        self.install(); before = self.library.record.read_bytes()
        def interrupted(*args, **kwargs):
            raise KeyboardInterrupt()
        with patch.object(importer.subprocess, 'run', interrupted):
            with self.assertRaises(KeyboardInterrupt):
                self.library.import_iso(self.iso)
        self.assertEqual(before, self.library.record.read_bytes())
        self.assertEqual(list((self.data / 'preparing').iterdir()), [])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
