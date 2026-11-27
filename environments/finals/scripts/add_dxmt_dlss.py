#!/usr/bin/env python3
"""Derive a runtime that also carries DXMT's DLSS and NVAPI stand-ins, so a DX11 game can have
MetalFX upscaling while it renders with DXMT.

The main environment's DXMT (the source of add_dxmt.py) ships, with its MetalFX upscaling
patch (patches/dxmt-metalfx-upscaling.patch, docs/METALFX.md), a DLSS stand-in (nvngx.dll) that
runs a game's DLSS on MetalFX and an NVAPI stand-in (nvapi64.dll); with DXMT_ENABLE_NVEXT=1
DXMT presents an NVIDIA GPU so the game offers DLSS. Apple's DLSS works only on D3DMetal's own
device, so for DX11 games on DXMT these are the ones to use.

Like DXMT's d3d11 (add_dxmt.py), they are built as Wine builtins and the names nvngx/nvapi64 are
taken in lib/wine by Apple's modules, so they go to lib/dxmt/x86_64-windows/ with Wine's builtin
marker zeroed, and the compatibility tool copies them into the prefix and loads them as native
(compat-tool/run). Zeroing the marker changes the file, and NVIDIA's loader (part of the game)
loads only a validly signed nvngx.dll, so nvngx.dll is signed again (scripts/sign_pe.py) with
SteamDriver's own certificate (--signing; made once with
`sign_pe.py create-certificate DIR --name SteamDriver`, kept outside git). Under Wine an intact
signature from an untrusted root passes (sign_pe.py).

    python3 environments/finals/scripts/add_dxmt_dlss.py --workspace $W \\
        --base-version macfinals-N --version macfinals-N+1
"""
import argparse
import json
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.append(str(REPO / 'scripts'))  # shared helpers
from derive_runtime import run, sha, tree, write_archive
import sign_pe

SOURCE = REPO / 'runtime/phase-2/artifacts/macvolvo-1.2-dev.4'
SIGNING = Path.home() / 'SteamDriver-build/finals/signing/steamdriver-nvngx'
MARKER_AT, MARKER = 0x40, b'Wine builtin DLL'
FILES = {'lib/wine/x86_64-windows/nvngx.dll': 'lib/dxmt/x86_64-windows/nvngx.dll',
         'lib/wine/x86_64-windows/nvapi64.dll': 'lib/dxmt/x86_64-windows/nvapi64.dll'}


def unmark(data):
    if data[:2] != b'MZ' or data[MARKER_AT:MARKER_AT + len(MARKER)] != MARKER:
        raise ValueError('not a Wine builtin PE')
    return data[:MARKER_AT] + bytes(len(MARKER)) + data[MARKER_AT + len(MARKER):]


def derive(workspace, base_version, version, source, signing):
    artifacts = workspace / 'artifacts'
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid runtime version')
    base, dest = artifacts / base_version, artifacts / version
    if dest.exists() or Path(str(dest) + '.tar.gz').exists():
        raise ValueError('Use a new version; existing artifact is never overwritten')
    manifest = json.loads((base / 'runtime.json').read_text())
    if manifest['version'] != base_version:
        raise ValueError('The base artifact names a different version')
    if 'dxmt' not in manifest:
        raise ValueError(f'{base_version} has no DXMT (run add_dxmt.py first)')
    if clash := [d for d in FILES.values() if d in manifest['files']]:
        raise ValueError(f'{base_version} already has ' + ', '.join(clash))
    if not (signing / sign_pe.KEY).exists():
        raise ValueError(f'No signing certificate in {signing} (sign_pe.py create-certificate DIR --name SteamDriver)')

    run('/bin/cp', '-Rc', base, dest)  # APFS clone
    for src, rel in FILES.items():
        (dest / rel).write_bytes(unmark((source / src).read_bytes()))
        (dest / rel).chmod(0o755)
    nvngx = dest / FILES['lib/wine/x86_64-windows/nvngx.dll']
    sign_pe.sign(signing, nvngx, nvngx)
    if not sign_pe.check(nvngx, signing):
        raise ValueError('nvngx.dll signature check failed')

    for rel in FILES.values():
        manifest['files'][rel] = {'sha256': sha(dest / rel), 'bytes': (dest / rel).stat().st_size}
    manifest['files'] = dict(sorted(manifest['files'].items()))
    manifest['version'] = version
    manifest['dxmt_dlss'] = {'source': source.name, 'builtin_marker_zeroed': list(FILES.values()),
                             'signed': FILES['lib/wine/x86_64-windows/nvngx.dll'],
                             'certificate_sha256': sha(signing / sign_pe.LEAF),
                             'source_sha256': {s: sha(source / s) for s in FILES}}
    manifest['derived_from'] = {**manifest.get('derived_from', {}), 'dxmt_dlss_base': base_version}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    if not differences <= {*FILES.values(), 'runtime.json'}:
        raise ValueError('Unexpected differences from the base runtime: ' + ', '.join(sorted(differences)))

    archive = write_archive(dest)
    proof = json.loads(Path(str(base) + '.tar.json').read_text())
    proof.update(version=version, archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
                 files=len(manifest['files']), derived_from=base_version)
    archive.with_suffix('.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps({k: proof[k] for k in ('version', 'archive_sha256', 'archive_bytes', 'files', 'derived_from')}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--base-version', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--source', type=Path, default=SOURCE, help='runtime to take DXMT\'s stand-ins from')
    parser.add_argument('--signing', type=Path, default=SIGNING, help='sign_pe.py certificate folder')
    args = parser.parse_args()
    derive(args.workspace.resolve(), args.base_version, args.version, args.source.resolve(), args.signing)
