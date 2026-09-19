#!/usr/bin/env python3
"""Generate our probe's import stubs from a locally provided BSD PSPSDK tree."""
import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sdk", type=Path)
parser.add_argument("manifest", type=Path)
args = parser.parse_args()
spec = json.loads(args.manifest.read_text())
catalog = {}
for path in (args.sdk / "src").rglob("*.S"):
    for module, nid, name in re.findall(r'IMPORT_FUNC\s+"([^"]+)",\s*(0x[\da-fA-F]+),\s*(\w+)', path.read_text()):
        catalog[module, name] = nid
groups = list(spec["imports"].items())
name = spec["name"]
assert len(name.encode("ascii")) < 28 and all(c.isalnum() for c in name)
lines = ["/* Project-authored stubs; NIDs from BSD PSPSDK via make_imports.py. */", ".set noreorder"]
for index, (module, names) in enumerate(groups):
    lines += ['.section .rodata,"a"', f'name_{index}: .asciz "{module}"',
              '.align 2', f'nids_{index}:']
    for name in names:
        explicit = spec.get("opaque_imports", {}).get(module, {}).get(name)
        if explicit is not None:
            assert re.fullmatch(r'0x[0-9a-fA-F]{8}', explicit)
        lines += ['.word ' + (explicit if explicit is not None else catalog[module, name])]
    lines += ['.section .sceStub.text,"ax",@progbits', f'stubs_{index}:']
    for name in names:
        lines += [f'.global {name}', f'{name}: jr $ra', 'nop']
lines += ['.section .lib.stub,"a",@progbits', 'imports_begin:']
for index, (_, names) in enumerate(groups):
    lines += [f'.word name_{index}, 0x00010000, {(len(names)<<16)|5}, nids_{index}, stubs_{index}']
lines += ['imports_end:', '.section .rodata.sceModuleInfo,"a",@progbits', '.align 2',
          '.word 0', f'.asciz "{spec["name"]}"', f'.space {27-len(spec["name"])}',
          f'.word {int(spec.get("gp", 0))},0,0,imports_begin,imports_end']
args.manifest.with_name("imports.S").write_text("\n".join(lines) + "\n")
