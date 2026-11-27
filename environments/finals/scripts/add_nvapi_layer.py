#!/usr/bin/env python3
"""Derive a runtime in which the SteamDriver nvapi64 layer sits in front of Apple's NVAPI.

With MetalFX upscaling, D3DMetal presents an NVIDIA GPU and registers its driver version
(32.0.16.1656, NVIDIA 616.56), which DXGI and Direct3D 9 report; Apple's NVAPI alone answers
576.02, below Deadlock's minimum (581.08), so Deadlock turns asynchronous shader creation off and
doesn't offer DLSS. The layer (nvapi_layer/) passes every NVAPI call to Apple's NVAPI unchanged
except the two driver-version queries, which it answers with the registered version.

Layout, as the d3d12 layer's (tests/rosetta_nt/R15_SKIN_CURVE.md: Wine keys builtins on the PE's
internal name, so Apple's nvapi64.dll must live outside the arch dir):

    lib/wine/x86_64-windows/nvapi64.dll       the layer (was a link to Apple's nvapi64.dll)
    lib/wine/x86_64-windows/nvapi64_gptk.dll  placeholder reserving the nvapi64_gptk name
    lib/wine/nvapi64_gptk.dll                 -> ../apple-gptk/wine/x86_64-windows/nvapi64.dll
    lib/wine/nvapi64_gptk.so                  -> ../apple-gptk/wine/x86_64-unix/nvapi64.so

Apple's files are unchanged and stay in the player's toolkit (lib/apple-gptk). The compatibility
tool loads NVAPI only in upscaling launches and then also puts the placeholder into the prefix's
system32 (compat-tool/run). Proven with tests/windows_probe/dlss_probe.c (NvAPI 616.56).

    python3 environments/finals/scripts/add_nvapi_layer.py --workspace $W \\
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

HERE = Path(__file__).resolve().parents[1]
LAYER = 'lib/wine/x86_64-windows/nvapi64.dll'
PLACEHOLDER = 'lib/wine/x86_64-windows/nvapi64_gptk.dll'
LINKS = {'lib/wine/nvapi64_gptk.dll': '../apple-gptk/wine/x86_64-windows/nvapi64.dll',
         'lib/wine/nvapi64_gptk.so': '../apple-gptk/wine/x86_64-unix/nvapi64.so'}
APPLE_LINK = '../../apple-gptk/wine/x86_64-windows/nvapi64.dll'


def derive(workspace, base_version, version):
    artifacts = workspace / 'artifacts'
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
    if manifest['files'].get(LAYER) != {'symlink': APPLE_LINK}:
        raise ValueError(f'{base_version}: {LAYER} is not the link to Apple\'s nvapi64.dll')
    for path in [PLACEHOLDER, *LINKS]:
        if (base / path).exists() or (base / path).is_symlink():
            raise ValueError(f'{base_version} already has {path}')

    build = workspace / 'nvapi-layer'
    run(HERE / 'nvapi_layer/build.sh', build, workspace / 'wine-build/tools/winebuild/winebuild')

    run('/bin/cp', '-Rc', base, dest)  # APFS clone
    (dest / LAYER).unlink()
    (dest / LAYER).write_bytes((build / 'nvapi64.dll').read_bytes())
    (dest / PLACEHOLDER).write_bytes((build / 'nvapi64_gptk.dll').read_bytes())
    for path, target in LINKS.items():
        (dest / path).symlink_to(target)
        manifest['files'][path] = {'symlink': target}
    for path in (LAYER, PLACEHOLDER):
        manifest['files'][path] = {'sha256': sha(dest / path), 'bytes': (dest / path).stat().st_size}
    manifest['files'] = dict(sorted(manifest['files'].items()))
    manifest['version'] = version
    manifest['nvapi_layer'] = {'layer_sha256': sha(dest / LAYER), 'apple_nvapi': LINKS['lib/wine/nvapi64_gptk.dll']}
    manifest['derived_from'] = {**manifest.get('derived_from', {}), 'nvapi_layer_base': base_version}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    if not differences <= {LAYER, PLACEHOLDER, *LINKS, 'runtime.json'}:
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
    derive(args.workspace.resolve(), args.base_version, args.version)
