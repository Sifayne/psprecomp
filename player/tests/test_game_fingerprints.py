#!/usr/bin/env python3
"""Check which real staged ingredients invalidate each title's cached game.

Three synthetic titles in a fixture app: first includes common.h, second and
third share siblings.h (which includes common.h too), and second and third
run the code generator gen.py after the emit."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fingerprints', ROOT / 'linux/game_fingerprints.py')
builds = importlib.util.module_from_spec(spec); spec.loader.exec_module(builds)
APP_ID = 'fixture-app'
R = f'usr/share/{APP_ID}/'
FIRST, SECOND, THIRD = 'first', 'second', 'third'


class FingerprintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='game fingerprint checks ')
        self.app = Path(self.tmp.name) / 'AppDir'
        self.resource = self.app / R
        self.abis = dict(SDL2='libSDL2-2.0.so.0', openh264='libopenh264.so.8',
                         avcodec='libavcodec.so.63', avutil='libavutil.so.61')
        self.profiles = [dict(slug=slug, title=slug.title(), disc_id=f'FIXT1000{i}', elf_sha256=str(i) * 64,
                              replacements=f'replacements-{slug}.c', replace_list=f'replace-{slug}.txt',
                              codegen=[] if slug == FIRST else ['gen.py'])
                         for i, slug in enumerate((FIRST, SECOND, THIRD))]
        self.write(R + 'games.json', json.dumps(self.profiles))
        for path in (R + 'libruntime.a', R + 'include/shared.h', R + 'compile_game.py', R + 'emit-split.py',
                     R + 'gen.py', 'usr/zig/zig', 'usr/zig/lib/std.zig', 'usr/bin/allegrexrecomp'):
            self.write(path, 'original ' + path)
        self.write(R + 'host/common.h', '#include "nested.h"\n')
        self.write(R + 'host/nested.h', '#define COMMON 1\n')
        self.write(R + 'host/siblings.h', '#include "common.h"\n')
        for profile in self.profiles:
            slug = profile['slug']
            self.write(R + f'libhost-{slug}.a', 'host ' + slug)
            header = 'common.h' if slug == FIRST else 'siblings.h'
            self.write(R + 'host/' + profile['replacements'],
                       f'#include "{header}"\n#include "{slug}_funcs.h"\n')
            self.write(R + 'host/' + profile['replace_list'], '00102018\n')
        self.baseline = self.ids()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        path = self.app / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)

    def ids(self):
        return {slug: entry['id'] for slug, entry in builds.fingerprints(self.app, self.abis, APP_ID)['games'].items()}

    def changed(self):
        return {slug for slug, value in self.ids().items() if value != self.baseline[slug]}

    def test_launcher_and_packaging_changes_preserve_all_games(self):
        for path in ('usr/bin/launcher', 'usr/bin/import-game', 'usr/bin/run-game',
                     R + 'import_game.py', R + 'build-id', R + 'app.json',
                     R + 'DejaVuSans.ttf', APP_ID + '.svg', 'README.txt',
                     'licenses/THIRD-PARTY.txt', 'BUILD.json'):
            self.write(path, 'updated app UI or packaging')
        self.assertEqual(self.changed(), set())

    def test_display_names_and_tab_order_preserve_all_games(self):
        for profile in self.profiles:
            profile['title'] += ' new display label'
        self.write(R + 'games.json', json.dumps(list(reversed(self.profiles)), indent=4))
        self.assertEqual(self.changed(), set())

    def test_title_replacement_and_replace_list_are_independent(self):
        for profile in self.profiles:
            for field in ('replacements', 'replace_list'):
                path = self.resource / 'host' / profile[field]; original = path.read_text()
                path.write_text(original + '\n/* changed */\n')
                self.assertEqual(self.changed(), {profile['slug']})
                path.write_text(original)

    def test_title_host_and_supported_executable_are_independent(self):
        host = self.resource / f'libhost-{SECOND}.a'; host.write_text('updated title host')
        self.assertEqual(self.changed(), {SECOND})
        self.baseline = self.ids()
        self.profiles[0]['elf_sha256'] = 'f' * 64
        self.write(R + 'games.json', json.dumps(self.profiles))
        self.assertEqual(self.changed(), {FIRST})

    def test_shared_sibling_controls_affect_only_the_siblings(self):
        self.write(R + 'host/siblings.h', '#include "common.h"\n// changed\n')
        self.assertEqual(self.changed(), {SECOND, THIRD})

    def test_transitive_shared_header_affects_every_consumer(self):
        self.write(R + 'host/nested.h', '#define COMMON 2\n')
        self.assertEqual(self.changed(), {FIRST, SECOND, THIRD})

    def test_code_generator_affects_only_the_titles_that_run_it(self):
        self.write(R + 'gen.py', 'a changed generator')
        self.assertEqual(self.changed(), {SECOND, THIRD})

    def test_runtime_codegen_compiler_and_recipe_changes_invalidate_all(self):
        for path in (R + 'libruntime.a', R + 'include/shared.h',
                     'usr/bin/allegrexrecomp', 'usr/zig/zig', 'usr/zig/lib/std.zig',
                     R + 'compile_game.py', R + 'emit-split.py'):
            file = self.app / path; original = file.read_text(); file.write_text('new build ingredient')
            self.assertEqual(self.changed(), {FIRST, SECOND, THIRD}, path)
            file.write_text(original)

    def test_compatible_shared_library_update_needs_no_relink(self):
        self.write('usr/lib/libavcodec.so.63', 'new library implementation with same ABI')
        self.assertEqual(self.changed(), set())
        self.abis['avcodec'] = 'libavcodec.so.64'
        self.assertEqual(self.changed(), {FIRST, SECOND, THIRD})

    def test_app_relocation_preserves_ids(self):
        moved = self.app.with_name('new app location with spaces')
        shutil.move(self.app, moved); self.app = moved
        self.assertEqual(self.ids(), self.baseline)

    def test_unknown_quoted_include_fails_closed(self):
        self.write(R + f'host/replacements-{FIRST}.c', '#include "missing-game-input.h"\n')
        with self.assertRaisesRegex(ValueError, 'Untracked replacement include'):
            self.ids()


if __name__ == '__main__':
    unittest.main(verbosity=2)
