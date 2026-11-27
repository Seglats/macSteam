#!/usr/bin/env python3
"""Derive a runtime that also carries MacVolvo's DXMT (Direct3D 11 to Metal) from an assembled one.

The finals environment renders with Apple's D3DMetal: its lib/wine d3d11, dxgi and d3d12 link
into the player's Game Porting Toolkit. DX11 games run better on DXMT (the main environment's
renderer: CS2 and Deadlock), so this adds it beside D3DMetal and the compatibility tool picks
one per launch (compat-tool/run, STEAMDRIVER_DX11):

- lib/wine/x86_64-windows/winemetal.dll and lib/wine/x86_64-unix/winemetal.so: DXMT's bridge
  to Metal, byte-identical. The finals Wine has no winemetal of its own; the Unix half needs
  only NtSetEvent from ntdll.so and the Mac driver's macdrv_functions table (macdrv_v1_*),
  which the shared Mac driver patches export in both environments.
- lib/dxmt/<arch>-windows/{d3d11,dxgi,d3d10core}.dll: DXMT's Direct3D DLLs, which the tool
  copies into the prefix and loads as native. They are built as Wine builtins, and Wine never
  loads a builtin-marked file as native (unix/loader.c load_builtin: LO_NATIVE returns
  STATUS_DLL_NOT_FOUND), while a builtin load would find D3DMetal's copies in lib/wine first.
  So the copies have the 16-byte "Wine builtin DLL" marker in the DOS stub (offset 0x40)
  zeroed. Nothing else changes: they import only winemetal.dll and system DLLs.

The source is the main environment's runtime (the one the app's "SteamDriver DXMT" NotProton
variant ships). The result is an APFS clone of the base with those files added; the manifest,
archive and proof are written beside the base, as add_nt_hook.py does.
"""
import argparse
import json
from pathlib import Path
import re
import shutil

import sys as _sys
_sys.path.append(str(Path(__file__).resolve().parents[3] / 'scripts'))
from derive_runtime import run, sha, tree, write_archive

REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / 'runtime/phase-2/artifacts/macvolvo-1.2-dev.4'
MARKER_AT, MARKER = 0x40, b'Wine builtin DLL'
BRIDGE = ('lib/wine/x86_64-windows/winemetal.dll', 'lib/wine/x86_64-unix/winemetal.so')
DIRECT3D = [(f'lib/wine/{arch}-windows/{name}.dll', f'lib/dxmt/{arch}-windows/{name}.dll')
            for arch in ('x86_64', 'i386') for name in ('d3d11', 'dxgi', 'd3d10core')]


def unmark(data):
    """DATA with Wine's builtin marker zeroed, so Wine loads the DLL as native."""
    if data[:2] != b'MZ' or data[MARKER_AT:MARKER_AT + len(MARKER)] != MARKER:
        raise ValueError('not a Wine builtin PE')
    return data[:MARKER_AT] + bytes(len(MARKER)) + data[MARKER_AT + len(MARKER):]


def derive(artifacts, base_version, version, source):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid runtime version')
    base, dest = artifacts / base_version, artifacts / version
    if dest.exists() or Path(str(dest) + '.tar.gz').exists():
        raise ValueError('Use a new version; existing artifact is never overwritten')
    manifest = json.loads((base / 'runtime.json').read_text())
    if manifest['version'] != base_version:
        raise ValueError('The base artifact names a different version')
    added = [*BRIDGE, *(d for _, d in DIRECT3D)]
    if clash := [f for f in added if f in manifest['files']]:
        raise ValueError(f'{base_version} already has ' + ', '.join(clash))

    run('/bin/cp', '-Rc', base, dest)  # APFS clone: only the added files take space
    for relative in BRIDGE:
        shutil.copy2(source / relative, dest / relative)
    for src, relative in DIRECT3D:
        (dest / relative).parent.mkdir(parents=True, exist_ok=True)
        (dest / relative).write_bytes(unmark((source / src).read_bytes()))
        (dest / relative).chmod(0o755)

    for relative in added:
        manifest['files'][relative] = {'sha256': sha(dest / relative), 'bytes': (dest / relative).stat().st_size}
    manifest['version'] = version
    manifest['dxmt'] = {'source': source.name, 'builtin_marker_zeroed': [d for _, d in DIRECT3D],
                        'source_sha256': {s: sha(source / s) for s in [*BRIDGE, *(s for s, _ in DIRECT3D)]}}
    manifest['derived_from'] = {**manifest.get('derived_from', {}), 'dxmt_base': base_version}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    if not differences <= {*added, 'runtime.json'}:
        raise ValueError('Unexpected differences from the base runtime: ' + ', '.join(sorted(differences)))

    archive = write_archive(dest)
    proof = json.loads(Path(str(base) + '.tar.json').read_text())
    proof.update(version=version, archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
                 unpacked_file_bytes=sum(v.get('bytes', 0) for v in manifest['files'].values()),
                 files=len(manifest['files']), derived_from=base_version, dxmt_source=source.name)
    archive.with_suffix('.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--base-version', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--source', type=Path, default=SOURCE, help='runtime to take DXMT from')
    args = parser.parse_args()
    derive(args.workspace.resolve() / 'artifacts', args.base_version, args.version, args.source.resolve())
