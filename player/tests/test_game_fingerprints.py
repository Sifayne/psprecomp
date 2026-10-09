#!/usr/bin/env python3
"""Check which inputs invalidate each title's prepared game.

A fixture app and a synthetic pack of three titles: the pack's host code
includes host.h; first's replacements include common.h, second's and third's
share siblings.h (which includes common.h too), and second and third run the
code generator gen.py after the emit."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec); spec.loader.exec_module(loaded)
    return loaded


builds = load('fingerprints', ROOT / 'linux/game_fingerprints.py')
packs = load('pack', ROOT / 'pack.py')
R = 'usr/share/psprecomp/'
FIRST, SECOND, THIRD = 'first', 'second', 'third'


class FingerprintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='game fingerprint checks ')
        self.app = Path(self.tmp.name) / 'AppDir'
        self.pack_root = Path(self.tmp.name) / 'pack'
        self.abis = dict(SDL2='libSDL2-2.0.so.0', openh264='libopenh264.so.8',
                         avcodec='libavcodec.so.63', avutil='libavutil.so.61')
        for path in (R + 'libruntime.a', R + 'libplayer.a', R + 'include/shared.h', R + 'recomp/loader.h',
                     R + 'pack_api.c', R + 'title_plain.c', R + 'compile_game.py', R + 'emit-split.py',
                     'usr/zig/zig', 'usr/zig/lib/std.zig', 'usr/bin/allegrexrecomp'):
            self.write(self.app / path, 'original ' + path)
        titles = []
        for i, slug in enumerate((FIRST, SECOND, THIRD)):
            header = 'common.h' if slug == FIRST else 'siblings.h'
            self.write(self.pack_root / f'host/replacements-{slug}.c', f'#include "{header}"\n#include "{slug}_funcs.h"\n')
            self.write(self.pack_root / f'host/replace-{slug}.txt', '00102018\n')
            titles.append(dict(slug=slug, title=slug.title(), disc_id=f'FIXT1000{i}', elf_sha256=str(i) * 64,
                               replacements=f'replacements-{slug}.c', replace_list=f'replace-{slug}.txt',
                               codegen=[] if slug == FIRST else ['gen.py']))
        self.write(self.pack_root / 'host/common.h', '#include "nested.h"\n')
        self.write(self.pack_root / 'host/nested.h', '#define COMMON 1\n')
        self.write(self.pack_root / 'host/siblings.h', '#include "common.h"\n')
        self.write(self.pack_root / 'host/boot.c', '#include "host.h"\n#include <psprecomp/state.h>\n')
        self.write(self.pack_root / 'host/host.h', '#define HOST 1\n')
        self.write(self.pack_root / 'host/settings.c', 'int settings;\n')
        self.write(self.pack_root / 'host/launcher_info.c', 'int info;\n')
        self.write(self.pack_root / 'scripts/gen.py', 'print(1)\n')
        self.write(self.pack_root / 'LICENSE', 'MIT')
        self.write(self.pack_root / 'notes.md', 'docs')
        device = [f'host/{n}' for n in ('common.h', 'nested.h', 'siblings.h')] + \
                 [f'host/{k}-{s}.{e}' for s in (FIRST, SECOND, THIRD) for k, e in (('replacements', 'c'), ('replace', 'txt'))]
        self.write(self.pack_root / 'pack.json', json.dumps({
            'version': 2, 'pack': {'id': 'fixture', 'name': 'Fixture', 'license': 'LICENSE'}, 'titles': titles,
            'host': {'sources': ['host/boot.c'], 'launcher': ['host/settings.c', 'host/launcher_info.c']},
            'device': {'host': device, 'scripts': ['scripts/gen.py']}, 'sources': ['notes.md']}))
        self.baseline = self.ids()

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)

    def ids(self):
        app = builds.app_identity(self.app, self.abis)['id']
        pack = packs.load(self.pack_root)
        return {p['slug']: builds.title_identity(app, pack, p) for p in pack.profiles()}

    def changed(self):
        return {slug for slug, value in self.ids().items() if value != self.baseline[slug]}

    def test_app_inputs_invalidate_every_title(self):
        for path in (R + 'libruntime.a', R + 'libplayer.a', R + 'include/shared.h', R + 'recomp/loader.h',
                     R + 'pack_api.c', R + 'title_plain.c', R + 'compile_game.py', R + 'emit-split.py',
                     'usr/zig/zig', 'usr/zig/lib/std.zig', 'usr/bin/allegrexrecomp'):
            original = (self.app / path).read_text()
            self.write(self.app / path, 'changed')
            self.assertEqual(self.changed(), {FIRST, SECOND, THIRD}, path)
            self.write(self.app / path, original)
        self.abis['SDL2'] = 'libSDL2-2.0.so.1'
        self.assertEqual(self.changed(), {FIRST, SECOND, THIRD})

    def test_pack_host_code_invalidates_every_title(self):
        for name in ('host/boot.c', 'host/host.h'):
            self.write(self.pack_root / name, 'changed')
            self.assertEqual(self.changed(), {FIRST, SECOND, THIRD}, name)
            self.setUp()

    def test_replacement_closure_only_its_titles(self):
        self.write(self.pack_root / 'host/siblings.h', '#include "common.h"\n#define CHANGED 1\n')
        self.assertEqual(self.changed(), {SECOND, THIRD})
        self.write(self.pack_root / 'host/nested.h', 'changed')
        self.assertEqual(self.changed(), {FIRST, SECOND, THIRD})

    def test_one_title_own_inputs(self):
        self.write(self.pack_root / 'host/replacements-first.c', '#include "common.h"\nint x;\n')
        self.assertEqual(self.changed(), {FIRST})
        self.setUp()
        self.write(self.pack_root / 'host/replace-third.txt', '00102019\n')
        self.assertEqual(self.changed(), {THIRD})

    def test_codegen_only_its_users(self):
        self.write(self.pack_root / 'scripts/gen.py', 'print(2)\n')
        self.assertEqual(self.changed(), {SECOND, THIRD})

    def test_launcher_docs_and_licenses_change_nothing(self):
        for name in ('host/settings.c', 'host/launcher_info.c', 'notes.md', 'LICENSE'):
            self.write(self.pack_root / name, 'changed')
        self.assertEqual(self.changed(), set())

    def test_plain_game_follows_the_app_and_its_executable(self):
        app = builds.app_identity(self.app, self.abis)['id']
        plain = dict(slug='ulus10567', elf_sha256='9' * 64, title='Any game')
        first = builds.title_identity(app, None, plain)
        self.assertEqual(first, builds.title_identity(app, None, dict(plain, title='Renamed')))
        self.assertNotEqual(first, builds.title_identity(app, None, dict(plain, elf_sha256='8' * 64)))
        self.write(self.pack_root / 'host/boot.c', 'changed')             # a pack's change: not this game's
        self.assertEqual(first, builds.title_identity(builds.app_identity(self.app, self.abis)['id'], None, plain))
        self.write(self.app / R / 'title_plain.c', 'changed')
        self.assertNotEqual(first, builds.title_identity(builds.app_identity(self.app, self.abis)['id'], None, plain))

    def test_missing_app_inputs_fail(self):
        (self.app / R / 'libplayer.a').unlink()
        with self.assertRaises(OSError):
            self.ids()


if __name__ == '__main__':
    unittest.main(verbosity=2)
