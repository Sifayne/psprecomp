"""Which inputs invalidate a title's prepared game.

The app's part is fixed when the app is built (app_identity, staged as
usr/share/psprecomp/app-build.json): the runtime and player archives, the
headers, the generator, the compiler, the build recipes and the shared
libraries' ABIs (SONAMEs: compatible library updates keep games). A pack's
part comes from the pack as added: its host code's include closure, and per
title the replacements' closure, the replace list and the code generators.
Launcher, importer UI, packaging recipes, notices and display labels are
deliberately outside this contract.

Staged in the app beside import_game.py, which reads it; build.py uses it
at build time.
"""
import hashlib
import json
from pathlib import Path
import re

VERSION = 2


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def identity(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def tree_identity(root):
    if not root.is_dir():
        raise ValueError(f'Missing game build inputs: {root}')
    files = {p.relative_to(root).as_posix(): digest(p)
             for p in sorted(root.rglob('*')) if p.is_file()}
    if not files:
        raise ValueError(f'Empty game build inputs: {root}')
    return identity(files)


def app_identity(app, library_abis):
    resource = Path(app).resolve() / 'usr/share/psprecomp'
    inputs = {
        'runtime': digest(resource / 'libruntime.a'),
        'player': digest(resource / 'libplayer.a'),
        'headers': tree_identity(resource / 'include'),
        'recomp_headers': tree_identity(resource / 'recomp'),
        'pack_api': digest(resource / 'pack_api.c'),
        'generator': digest(Path(app) / 'usr/bin/allegrexrecomp'),
        'compiler': digest(Path(app) / 'usr/zig/zig'),
        'compiler_support': tree_identity(Path(app) / 'usr/zig/lib'),
        'build_recipe': digest(resource / 'compile_game.py'),
        'split_recipe': digest(resource / 'emit-split.py'),
        'library_abis': library_abis,
    }
    return {'version': VERSION, 'id': identity({'version': VERSION, 'inputs': inputs}), 'inputs': inputs}


def closure(pack_root, starts, generated_skip=None):
    """Each file starts names and every pack file its quoted includes reach,
    by path within the pack: digests. An include found nowhere in the pack is
    the app's (its headers are in the app's part) or generated."""
    root = Path(pack_root).resolve()
    pending = [Path(p).resolve() for p in starts]
    inputs = {}
    while pending:
        source = pending.pop()
        name = source.relative_to(root).as_posix()
        if name in inputs:
            continue
        inputs[name] = digest(source)
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', source.read_text(errors='replace'), re.M):
            if include == generated_skip:
                continue
            found = (source.parent / include).resolve()
            if found.is_file() and found.is_relative_to(root):
                pending.append(found)
    return inputs


def pack_identity(pack):
    """The part of a pack every title of it shares: its host code."""
    return identity({'version': VERSION, 'host': closure(pack.root, pack.host_sources)})


def title_identity(app_id, pack, profile):
    replacements = pack.device_file(profile['replacements'])
    scripts = {p.name: p for p in pack.device_scripts}
    inputs = {
        'app': app_id,
        'pack': pack_identity(pack),
        'profile': {key: profile[key] for key in ('slug', 'elf_sha256', 'replacements', 'replace_list')},
        'replacements': closure(pack.root, [replacements], profile['slug'] + '_funcs.h'),
        'replace_list': digest(pack.device_file(profile['replace_list'])),
        'codegen': {script: digest(scripts[script]) for script in profile.get('codegen', [])},
    }
    return identity({'version': VERSION, 'inputs': inputs})
