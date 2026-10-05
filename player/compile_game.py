"""The game generation/compilation recipe, separate from library and launcher UI.

This file is part of every game fingerprint. Changes to commands, flags or
linking here therefore invalidate games built with a different recipe. It is
staged in the app's resources folder, usr/share/<pack id>, beside the pack's
host sources and code-generation scripts, and reads them from there.
"""
import concurrent.futures
from pathlib import Path
import subprocess
import sys


def compile_game(app, profile, module, output, jobs, env, log):
    app, module, output = map(Path, (app, module, output))
    resources, tmp = Path(__file__).resolve().parent, output.parent
    slug = profile['slug']

    def run(args):
        log.write('\n$ ' + ' '.join(map(str, args)) + '\n')
        log.flush()
        result = subprocess.run(list(map(str, args)), cwd=tmp, env=env, stdout=log, stderr=log)
        if result.returncode:
            raise ValueError(f'Preparation failed. See {log.name} for details.')

    generated, obj = tmp / 'generated', tmp / 'objects'
    generated.mkdir(); obj.mkdir()
    host = resources / 'host'
    run([app / 'usr/bin/allegrexrecomp', 'emit', module, generated, slug,
         '--replace', '@' + str(host / profile['replace_list'])])
    # The pack's own generators, such as Last Raven's fps-loop.py.
    for script in profile.get('codegen', []):
        run([sys.executable, '-I', '-B', resources / script, slug, generated])
    run([sys.executable, '-I', '-B', resources / 'emit-split.py',
         generated / f'{slug}_funcs.c', obj, '32'])
    cc = [app / 'usr/zig/zig', 'cc', '-target', 'x86_64-linux-gnu.2.35',
          '-std=gnu11', '-fno-strict-aliasing', '-fwrapv', '-I', resources / 'include',
          '-I', generated, '-I', obj, '-I', host]
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
    run([*cc, '-O2', '-DHAVE_SDL2', '-c', host / profile['replacements'], '-o', obj / 'replacements.o'])
    print('Finishing game setup...', flush=True)
    run([*cc, '-O2', *sorted(obj.glob('*.o')), resources / f'libhost-{slug}.a',
         resources / 'libruntime.a', '-L', app / 'usr/lib', '-lSDL2', '-lSDL2_ttf', '-lopenh264', '-lavcodec', '-lavutil',
         '-lm', '-lpthread', '-Wl,-rpath,$ORIGIN/lib', '-o', output])
    run([output, '--print-settings'])
