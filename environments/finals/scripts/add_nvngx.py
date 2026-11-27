#!/usr/bin/env python3
"""Derive a runtime in which Apple's DLSS-to-MetalFX module answers to the name nvngx.

Apple's Game Porting Toolkit ships its DLSS stand-in as nvngx-on-metalfx.dll / .so, and its
Read Me ("experimental MetalFX integration") says to use them as nvngx.dll / nvngx.so, with
nvngx.dll and nvapi64.dll in the prefix's system32 and D3DM_ENABLE_METALFX=1. Under its own
name nothing ever loads it, so D3DMetal's MetalFX switch has nothing to work with.

This adds two links beside the ones unbundle_gptk.py made, into the player's toolkit
(lib/apple-gptk), unchanged:

    lib/wine/x86_64-windows/nvngx.dll -> ../../apple-gptk/wine/x86_64-windows/nvngx-on-metalfx.dll
    lib/wine/x86_64-unix/nvngx.so     -> ../../apple-gptk/wine/x86_64-unix/nvngx-on-metalfx.so

wine.inf places unsigned Wine placeholders for nvngx.dll and nvapi64.dll in system32 (CW HACK
22435). They load the builtins, but a game's NGX loader checks the signature of the nvngx.dll
file on disk, and only Apple's is signed (Apple Inc.), so the compatibility tool copies Apple's
files over them when upscaling is asked for (compat-tool/run, STEAMDRIVER_UPSCALE). It keeps
nvngx and nvapi64 disabled otherwise, so a launch without it is unchanged. Proven with tests/windows_probe/dlss_probe.c. Run on an
unbundled runtime (after unbundle_gptk.py, or a runtime derived from one):

    python3 environments/finals/scripts/add_nvngx.py --workspace $W \\
        --base-version macfinals-N --version macfinals-N+1
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

sys.path.append(str(Path(__file__).resolve().parents[3] / 'scripts'))  # shared helpers
from derive_runtime import run, sha, tree, write_archive

LINKS = {'lib/wine/x86_64-windows/nvngx.dll': '../../apple-gptk/wine/x86_64-windows/nvngx-on-metalfx.dll',
         'lib/wine/x86_64-unix/nvngx.so': '../../apple-gptk/wine/x86_64-unix/nvngx-on-metalfx.so'}


def derive(artifacts, base_version, version):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid runtime version')
    base, dest = artifacts / base_version, artifacts / version
    if dest.exists() or Path(str(dest) + '.tar.gz').exists():
        raise ValueError('Use a new version; existing artifact is never overwritten')
    manifest = json.loads((base / 'runtime.json').read_text())
    if manifest['version'] != base_version:
        raise ValueError('The base artifact names a different version')
    if manifest.get('apple_gptk') != 'player':
        raise ValueError(f'{base_version} still carries Apple\'s files; run unbundle_gptk.py first')
    for path, target in LINKS.items():
        if (base / path).exists() or (base / path).is_symlink():
            raise ValueError(f'{base_version} already has {path}')
        # The name it links to must be one of the base's own GPTK links.
        beside = str(Path(path).parent / Path(target).name)
        if manifest['files'].get(beside) != {'symlink': target}:
            raise ValueError(f'{base_version} has no {beside} link to {target}')

    run('/bin/cp', '-Rc', base, dest)  # APFS clone
    for path, target in LINKS.items():
        (dest / path).symlink_to(target)
        manifest['files'][path] = {'symlink': target}
    manifest['files'] = dict(sorted(manifest['files'].items()))
    manifest['version'] = version
    manifest['nvngx_metalfx'] = {'links': LINKS, 'from': 'Apple Game Porting Toolkit Read Me, MetalFX integration'}
    manifest['derived_from'] = {**manifest.get('derived_from', {}), 'nvngx_base': base_version}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    if not differences <= {*LINKS, 'runtime.json'}:
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
    args = parser.parse_args()
    derive(args.workspace.resolve() / 'artifacts', args.base_version, args.version)
