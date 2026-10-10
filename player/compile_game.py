"""The on-device build recipes, separate from the library and launcher UI:
a pack's host code and launcher part when the pack is added, and a title's
game when its disc is -- with its pack, or, for a disc no pack knows, as the
plain recompiled game (title_plain.c).

This file is part of every game fingerprint. Changes to commands, flags or
linking here therefore invalidate games built with a different recipe. It is
staged in the app's resources folder, usr/share/psprecomp, beside the
headers, the runtime and player archives and emit-split.py, and reads them
from there.
"""
import concurrent.futures
from pathlib import Path
import subprocess
import sys

TARGET = ['-target', 'x86_64-linux-gnu.2.35']
# What the launcher may see of a pack's launcher.so; the rest stays its own.
EXPORTS = '{ global: psp_title_settings; psp_launcher_info; psp_pack_api; local: *; };\n'


def _runner(cwd, env, log):
    def run(args):
        log.write('\n$ ' + ' '.join(map(str, args)) + '\n')
        log.flush()
        result = subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdout=log, stderr=log)
        if result.returncode:
            raise ValueError(f'Preparation failed. See {log.name} for details.')
    return run


def _host_flags(resources, source):
    # The pack's sources find their headers beside them; psprecomp's and
    # SDL's are the app's own.
    return ['-std=gnu11', '-O2', '-DNDEBUG', '-DHAVE_SDL2', '-I', resources / 'include',
            '-I', resources / 'include/SDL2', '-I', resources / 'recomp', '-I', Path(source).parent]


def build_pack(app, pack, out, env, log, jobs=2):
    """A pack's launcher.so and its host objects, into out."""
    app, out = Path(app), Path(out)
    resources = Path(__file__).resolve().parent
    zig = [app / 'usr/zig/zig', 'cc', *TARGET]
    out.mkdir(parents=True, exist_ok=True)
    run = _runner(out, env, log)
    (out / 'exports.map').write_text(EXPORTS)
    run([*zig, '-shared', '-fPIC', '-std=gnu11', '-O2', '-DNDEBUG', '-I', resources / 'include',
         *[a for src in pack.launcher_sources for a in ('-I', src.parent)],
         *pack.launcher_sources, resources / 'pack_api.c',
         '-Wl,--version-script=' + str(out / 'exports.map'), '-Wl,-Bsymbolic', '-o', out / 'launcher.so'])
    host = out / 'host'
    host.mkdir(exist_ok=True)
    stems = [src.stem for src in pack.host_sources]
    if len(stems) != len(set(stems)):
        raise ValueError(f'{pack.name}: host sources need distinct file names')

    def compile_one(src):
        run([*zig, *_host_flags(resources, src), '-c', src, '-o', host / (src.stem + '.o')])

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        list(pool.map(compile_one, pack.host_sources))
    return sorted(host.glob('*.o'))


def c_string(text):
    """text as a C string literal: printable ASCII as it is, the rest octal."""
    out = []
    for byte in text.encode('utf-8'):
        ch = chr(byte)
        out.append('\\' + ch if ch in '"\\' else ch if 32 <= byte < 127 and ch != '?' else f'\\{byte:03o}')
    return '"' + ''.join(out) + '"'


def compile_game(app, pack, host_objects, profile, module, output, jobs, env, log, modules=()):
    """A title's game; pack None builds it plain, named profile['title'].
    `modules` are the ones it loads at run time (import_game.disc_modules),
    each recompiled at its base and registered when the game loads it
    (docs/MODULES.md)."""
    app, module, output = map(Path, (app, module, output))
    resources, tmp = Path(__file__).resolve().parent, output.parent
    slug = profile['slug']
    run = _runner(tmp, env, log)
    generated, obj = tmp / 'generated', tmp / 'objects'
    generated.mkdir(); obj.mkdir()
    if pack:
        replacements = pack.device_file(profile['replacements'])
        replace_list = pack.device_file(profile['replace_list'])
        run([app / 'usr/bin/allegrexrecomp', 'emit', module, generated, slug,
             '--replace', '@' + str(replace_list)])
        # The pack's own generators, such as Last Raven's fps-loop.py.
        scripts = {p.name: p for p in pack.device_scripts}
        for script in profile.get('codegen', []):
            run([sys.executable, '-I', '-B', scripts[script], slug, generated])
    else:
        run([app / 'usr/bin/allegrexrecomp', 'emit', module, generated, slug])
    run([sys.executable, '-I', '-B', resources / 'emit-split.py',
         generated / f'{slug}_funcs.c', obj, '32'])
    includes = sorted({p.parent for p in pack.device_host}) if pack else []
    cc = [app / 'usr/zig/zig', 'cc', *TARGET,
          '-std=gnu11', '-fno-strict-aliasing', '-fwrapv', '-I', resources / 'include',
          '-I', generated, '-I', obj, *[a for d in includes for a in ('-I', d)]]
    chunks = sorted(obj.glob(f'{slug}_funcs_[0-9][0-9].c'))

    def compile_chunk(src):
        run([*cc, '-O2', '-c', src, '-o', src.with_suffix('.o')])

    # Warm the compiler cache before parallel compilation.
    compile_chunk(chunks[0])
    print(f'Compiling game: 1/{len(chunks)}', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for i, _ in enumerate(pool.map(compile_chunk, chunks[1:]), 2):
            print(f'Compiling game: {i}/{len(chunks)}', flush=True)
    run([*cc, '-O0', '-c', obj / f'{slug}_funcs_reg.c', '-o', obj / 'register.o'])
    run([*cc, '-O2', '-c', generated / f'{slug}_imports.c', '-o', obj / 'imports.o'])

    module_objects = []
    for m in modules:
        name = f"m{m['index']}"
        mgen, mobj = tmp / f'generated-{name}', tmp / f'objects-{name}'
        mgen.mkdir(); mobj.mkdir()
        print(f"Compiling module {m['index']} of {len(modules)}...", flush=True)
        run([app / 'usr/bin/allegrexrecomp', 'emit', m['image'], mgen, name,
             '--base', f"0x{m['base']:08X}", '--module', str(m['index'])])
        split = max(1, min(32, m['image'].stat().st_size // (512 * 1024) + 1))
        run([sys.executable, '-I', '-B', resources / 'emit-split.py', mgen / f'{name}_funcs.c', mobj, str(split)])
        mcc = [app / 'usr/zig/zig', 'cc', *TARGET, '-std=gnu11', '-fno-strict-aliasing', '-fwrapv',
               '-I', resources / 'include', '-I', mgen, '-I', mobj]
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            list(pool.map(lambda src: run([*mcc, '-O2', '-c', src, '-o', src.with_suffix('.o')]),
                          sorted(mobj.glob(f'{name}_funcs_[0-9][0-9].c'))))
        run([*mcc, '-O0', '-c', mobj / f'{name}_funcs_reg.c', '-o', mobj / 'register.o'])
        run([*mcc, '-O2', '-c', mgen / f'{name}_imports.c', '-o', mobj / 'imports.o'])
        module_objects += sorted(mobj.glob('*.o'))
    if pack:
        run([*cc, '-O2', '-DHAVE_SDL2', '-I', resources / 'include/SDL2', '-c', replacements, '-o', obj / 'replacements.o'])
    else:
        # The plain game: its name, as its disc gives it, and the player's
        # settings alone.
        # And its modules, one row per file the game may read each from.
        rows = [f'    {{"{sha1}", "modules/{m["file"]}", 0x{m["base"]:08X}u, 0x{m["size"]:08X}u, '
                f'psp_recomp_register_module_{m["index"]}}},' for m in modules for sha1 in m['sha1s']]
        (obj / 'title.c').write_text(
            '#include "psprecomp/host/title.h"\n'
            'const char psp_plain_title[] = ' + c_string(profile['title']) + ';\n'
            + ''.join(f"void psp_recomp_register_module_{m['index']}(void);\n" for m in modules)
            + 'const psp_title_module psp_plain_modules[] = {\n' + ('\n'.join(rows) if rows else '    {0},') + '\n};\n')
        run([*cc, '-O2', '-c', obj / 'title.c', '-o', obj / 'title.o'])
        run([*cc, '-O2', '-DHAVE_SDL2', f'-DPSP_PLAIN_MODULE_COUNT={len(rows)}', '-c', resources / 'title_plain.c',
             '-o', obj / 'title_plain.o'])
    print('Finishing game setup...', flush=True)
    run([*cc, '-O2', *sorted(obj.glob('*.o')), *module_objects, *host_objects,
         '-Wl,--start-group', resources / 'libplayer.a', resources / 'libruntime.a', '-Wl,--end-group',
         '-L', app / 'usr/lib', '-lSDL2', '-lSDL2_ttf', '-lopenh264', '-lavcodec', '-lavutil',
         '-lm', '-lpthread', '-Wl,-rpath,$ORIGIN/lib', '-o', output])
    run([output, '--print-settings'])
