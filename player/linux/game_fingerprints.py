"""Compute per-title compatibility from the staged game build ingredients.

Hash actual host/runtime archives, compiler inputs and each title's replacement
source closure, and the pack's code-generation scripts each title runs.
Launcher, importer UI, packaging recipes, notices and display labels are
deliberately outside this contract. Shared libraries contribute
SONAMEs: compatible library updates can be used by already-linked games.
"""
import hashlib
import json
from pathlib import Path
import re


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


def replacement_inputs(resource, profile):
    host, headers = resource / 'host', resource / 'include'
    pending = [host / profile['replacements']]
    inputs = {}
    while pending:
        source = pending.pop().resolve()
        name = source.relative_to(resource).as_posix()
        if name in inputs:
            continue
        inputs[name] = digest(source)
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', source.read_text(), re.M):
            if include == profile['slug'] + '_funcs.h':
                continue  # Generated from the pinned ELF, generator and replace list.
            candidates = (source.parent / include, host / include, headers / include)
            found = next((p for p in candidates if p.is_file()), None)
            if found is None:
                raise ValueError(f'Untracked replacement include: {name}: {include}')
            pending.append(found)
    inputs['host/' + profile['replace_list']] = digest(host / profile['replace_list'])
    return inputs


def fingerprints(app, library_abis, app_id):
    app = Path(app).resolve()
    resource = app / 'usr/share' / app_id
    shared = {
        'runtime': digest(resource / 'libruntime.a'),
        'runtime_headers': tree_identity(resource / 'include'),
        'generator': digest(app / 'usr/bin/allegrexrecomp'),
        'compiler': digest(app / 'usr/zig/zig'),
        'compiler_support': tree_identity(app / 'usr/zig/lib'),
        'compile_recipe': digest(resource / 'compile_game.py'),
        'split_recipe': digest(resource / 'emit-split.py'),
        'library_abis': library_abis,
    }
    games = {}
    for profile in json.loads((resource / 'games.json').read_text()):
        slug = profile['slug']
        inputs = {**shared,
                  'profile': {key: profile[key] for key in ('slug', 'elf_sha256', 'replacements', 'replace_list')},
                  'host': digest(resource / f'libhost-{slug}.a'),
                  'replacements': replacement_inputs(resource, profile),
                  'codegen': {script: digest(resource / script) for script in profile.get('codegen', [])}}
        games[slug] = {'id': identity({'version': 1, 'inputs': inputs}), 'inputs': inputs}
    return {'version': 1, 'games': games}
