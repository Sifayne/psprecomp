#!/usr/bin/env python3
"""Failure, persistence and ordering checks; fixtures contain no game content.

    test_import_game.py [APP]

With an AppDir, the importer under test is the one it staged; without one,
player/import_game.py. Either way the titles are three synthetic ones in a
fixture app: these checks are about the importer, not any pack's titles."""
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

APP = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
if APP:
    STAGED = [p.parent for p in APP.glob('usr/share/*/app.json')]
    if len(STAGED) != 1:
        sys.exit(f'{APP}: expected one usr/share/<id>/app.json')
    SOURCE = STAGED[0] / 'import_game.py'
else:
    SOURCE = Path(__file__).resolve().parents[1] / 'import_game.py'
spec = importlib.util.spec_from_file_location('importer', SOURCE)
APP_ID = 'fixture-app'
FIRST, SECOND, THIRD = 'first', 'second', 'third'   # in release order
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='game import checks ')
        self.root = Path(self.tmp.name)
        self.app = self.root / 'app with spaces'
        resources = self.app / 'usr/share' / APP_ID
        resources.mkdir(parents=True)
        self.module = b'\x7fELFsynthetic fixture, not a game'
        self.profiles = [dict(slug=slug, title=slug.title() + ' Title', disc_id=f'FIXT1000{i}',
                              elf_sha256=hashlib.sha256(self.module).hexdigest(),
                              replacements=f'{slug}.c', replace_list=f'{slug}.txt', codegen=[])
                         for i, slug in enumerate((FIRST, SECOND, THIRD))]
        (resources / 'games.json').write_text(json.dumps(self.profiles))
        (resources / 'build-id').write_text('fixture-build')
        (resources / 'game-builds.json').write_text(json.dumps({'version': 1, 'games': {
            p['slug']: {'id': hashlib.sha256(p['slug'].encode()).hexdigest()} for p in self.profiles}}))
        self.library = importer.Library(self.app, self.root / 'data', self.root / 'state', APP_ID)
        self.iso = self.root / 'my disc & spaces |.iso'
        self.iso.write_bytes(b'fixture'.ljust(32768, b'\0'))
        self.disc = self.profiles[0]['disc_id']

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, args, **kwargs):
        if args[1] == 'info':
            return subprocess.CompletedProcess(args, 0, 'DISC_ID  ' + self.disc + '\n')
        if args[1] == 'extract':
            Path(args[4], 'PSP_GAME_SYSDIR_EBOOT.BIN').write_bytes(self.module)
            return subprocess.CompletedProcess(args, 0, '')
        raise AssertionError('Unexpected helper invocation: ' + repr(args))

    def install(self, slug=FIRST):
        folder = self.library.data / 'games' / slug / 'previous build'
        folder.mkdir(parents=True)
        (folder / 'game').write_text('#!/bin/sh\nexit 0\n')
        (folder / 'game').chmod(0o755)
        (folder / 'module.elf').write_bytes(self.module)
        stat = self.iso.stat()
        entry = dict(directory=str(folder), iso=str(self.iso), build_id=self.library.build_id, game_build_id=self.library.game_builds[slug],
                     iso_size=stat.st_size, iso_mtime_ns=stat.st_mtime_ns)
        book = self.library.records()
        book['games'][slug] = entry
        book['selected'] = slug
        self.library.record.write_text(json.dumps(book))
        return entry

    def test_release_order_independent_of_import_order(self):
        for slug in (THIRD, SECOND, FIRST):
            self.install(slug)
        self.library.refresh()
        fields = (self.library.data / 'library.bin').read_bytes().split(b'\0')
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

    def test_unsupported_disc_and_exact_version_keep_existing_game(self):
        entry = self.install()
        before = self.library.record.read_bytes()
        self.disc = 'UNSUPPORTED'
        with patch.object(importer.subprocess, 'run', self.fake_run):
            with self.assertRaisesRegex(ValueError, 'Unsupported disc'):
                self.library.import_iso(self.iso)
            self.disc = self.profiles[0]['disc_id']
            self.module += b'wrong executable'
            with self.assertRaisesRegex(ValueError, 'unsupported executable version'):
                self.library.import_iso(self.iso)
        self.assertEqual(before, self.library.record.read_bytes())
        self.assertTrue(Path(entry['directory'], 'game').exists())
        self.assertEqual(list((self.library.data / 'preparing').iterdir()), [])

    def test_corrupt_and_external_records_preserved(self):
        self.library.data.mkdir()
        for content in ('bad json', '[]', '{"version":2,"games":{}}',
                        json.dumps({'version':1, 'games':{FIRST:dict(iso=str(self.iso), build_id='x', directory='/tmp/outside')}})):
            self.library.record.write_text(content)
            with self.assertRaises(ValueError):
                self.library.refresh()
            self.assertEqual(self.library.record.read_text(), content)

    def test_single_import_lock(self):
        self.library.data.mkdir()
        with (self.library.data / '.import.lock').open('w') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, 'Another game'):
                self.library.import_iso(self.iso)

    def test_relocated_iso_reuses_verified_cache_and_preserves_saves(self):
        entry = self.install()
        save = self.library.data / 'saves' / FIRST / 'ms/PSP/SAVEDATA/save'
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
        self.assertEqual((self.library.data / 'library.bin').read_bytes().split(b'\0')[4], b'')
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
        self.library.game_builds[SECOND] = 'f' * 64
        self.library.refresh()
        fields = (self.library.data / 'library.bin').read_bytes().split(b'\0')
        self.assertTrue(fields[4]); self.assertEqual(fields[9], b''); self.assertTrue(fields[14])
        for slug, entry in self.library.records()['games'].items():
            self.assertEqual(self.library.ready(slug, entry), slug != SECOND)

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

    def test_invalid_game_build_manifest_is_rejected(self):
        for manifest in ({'version': 2, 'games': {}}, {'version': 1, 'games': {}},
                         {'version': 1, 'games': {FIRST: {'id': 'invalid'}}}):
            (self.library.resources / 'game-builds.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                importer.Library(self.app, self.library.data, self.library.state, APP_ID)

    def test_runtime_rebuild_switches_only_after_success(self):
        old = self.install()
        sibling = self.install(SECOND)
        before = self.library.record.read_bytes()
        self.library.game_builds[FIRST] = 'f' * 64
        helper = self.library.resources / 'compile_game.py'
        helper.write_text('def compile_game(*args):\n    raise ValueError("compiler failed")\n')
        with patch.object(importer.subprocess, 'run', self.fake_run):
            with self.assertRaisesRegex(ValueError, 'compiler failed'):
                self.library.import_iso(self.iso)
        self.assertEqual(self.library.record.read_bytes(), before)
        self.assertTrue(Path(old['directory'], 'game').exists())
        self.assertEqual(list((self.library.data / 'preparing').iterdir()), [])
        helper.write_text('def compile_game(app, profile, module, output, jobs, env, log):\n'
                          '    output.write_text("new executable")\n    output.chmod(0o755)\n')
        with patch.object(importer.subprocess, 'run', self.fake_run):
            self.library.import_iso(self.iso)
        book = self.library.records()
        updated = book['games'][FIRST]
        self.assertNotEqual(updated['directory'], old['directory'])
        self.assertEqual(updated['game_build_id'], 'f' * 64)
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
        self.assertEqual(list((self.library.data / 'preparing').iterdir()), [])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
