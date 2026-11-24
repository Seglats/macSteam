#!/usr/bin/env python3
"""Derive a runtime that carries none of Apple's files from one that does.

The assembled runtime holds Apple's Game Porting Toolkit redist (D3DMetal.framework,
libd3dshared.dylib, Apple's Direct3D modules) and Apple's notices. macSteam ships the runtime
inside its app, so this takes them out: each Apple file is removed and replaced by a link into
lib/apple-gptk, which is not in the runtime. The game tool's run script makes lib/apple-gptk a
link to the player's own copy (~/Library/Application Support/SteamDriver/GPTK, the contents of
redist/lib, which macSteam checks: Apple's signature, D3DMetal 4) before Wine starts.

Nothing of Apple's is changed, and nothing else in the runtime either: the d3d12 layer still
holds the d3d12.dll name and loads Apple's d3d12.dll as d3d12_gptk.dll, now through the link.
Apple's libraries find each other only through the preloaded libd3dshared.dylib
(CX_APPLEGPTK_LIBD3DSHARED_PATH), the framework path (D3DMETAL_FRAMEWORK_PATH) and their own
folder, so the links keep every path they use.

Every removed file must match Apple's redist (--gptk) byte for byte, and afterwards no file in
the runtime may match one of Apple's. Run after add_nt_hook.py:

    python3 environments/finals/scripts/unbundle_gptk.py --workspace $W \\
        --base-version macfinals-N --version macfinals-N+1
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

sys.path.append(str(Path(__file__).resolve().parents[3] / 'scripts'))  # shared helpers
from derive_runtime import run, sha, tree, write_archive

REPO = Path(__file__).resolve().parents[3]
LINK = 'lib/apple-gptk'  # made by compat-tool/run, never part of the runtime


def apple_files(redist):
    """Apple's redist as runtime paths: {runtime path: (redist path, sha or link)}. Apple's
    d3d12.dll lives at lib/wine/d3d12_gptk.dll (the layer holds the d3d12.dll name)."""
    out = {}
    for p in sorted(redist.rglob('*')):
        rel = str(p.relative_to(redist))
        if rel.startswith('external/D3DMetal.framework/'):
            continue  # the framework goes as a whole (one link)
        if p.is_symlink() or p.is_file():
            target = 'lib/wine/d3d12_gptk.dll' if rel == 'wine/x86_64-windows/d3d12.dll' else 'lib/' + rel
            out[target] = (rel, ('link', os.readlink(p)) if p.is_symlink() else ('file', sha(p)))
    return out


def link_to(runtime_path, redist_rel):
    """The relative link from RUNTIME_PATH to lib/apple-gptk/REDIST_REL."""
    return os.path.relpath(f'{LINK}/{redist_rel}', os.path.dirname(runtime_path))


def derive(artifacts, base_version, version, gptk):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid runtime version')
    base, dest = artifacts / base_version, artifacts / version
    if dest.exists() or Path(str(dest) + '.tar.gz').exists():
        raise ValueError('Use a new version; existing artifact is never overwritten')
    manifest = json.loads((base / 'runtime.json').read_text())
    if manifest['version'] != base_version:
        raise ValueError('The base artifact names a different version')
    if manifest.get('apple_gptk') == 'player':
        raise ValueError(f'{base_version} already carries none of Apple\'s files')
    redist = gptk / 'redist/lib'
    expected = apple_files(redist)
    framework = 'lib/external/D3DMetal.framework'

    # Every Apple path in the base must be Apple's file, unchanged.
    for path, (rel, entry) in expected.items():
        p = base / path
        found = ('link', os.readlink(p)) if p.is_symlink() else ('file', sha(p)) if p.is_file() else None
        if found != entry:
            raise ValueError(f'{path} in {base_version} is not Apple\'s {rel} from {redist}')
    for p in (redist / 'external/D3DMetal.framework').rglob('*'):
        q = base / framework / p.relative_to(redist / 'external/D3DMetal.framework')
        if p.is_symlink() != q.is_symlink() or (p.is_file() and not p.is_symlink() and sha(p) != sha(q)):
            raise ValueError(f'{q.relative_to(base)} is not Apple\'s')

    run('/bin/cp', '-Rc', base, dest)  # APFS clone
    removed = []
    for path, (rel, _) in expected.items():
        (dest / path).unlink()
        (dest / path).symlink_to(link_to(path, rel))
        removed.append(path)
    shutil.rmtree(dest / framework)
    (dest / framework).symlink_to(link_to(framework, 'external/D3DMetal.framework'))
    notices = dest / 'licenses/apple'
    if notices.exists():
        shutil.rmtree(notices)

    # None of Apple's files may remain anywhere in the runtime.
    apple_shas = {sha(p) for p in redist.rglob('*') if p.is_file() and not p.is_symlink()}
    left = [str(p.relative_to(dest)) for p in dest.rglob('*')
            if p.is_file() and not p.is_symlink() and sha(p) in apple_shas]
    if left:
        raise ValueError('Apple files remain: ' + ', '.join(left))

    files = {}
    for p in sorted(dest.rglob('*')):
        key = str(p.relative_to(dest))
        if key == 'runtime.json' or p.name == '.DS_Store':
            continue
        if p.is_symlink():
            files[key] = {'symlink': os.readlink(p)}
        elif p.is_file():
            files[key] = manifest['files'].get(key) or {'sha256': sha(p), 'bytes': p.stat().st_size}
    manifest['files'] = files
    manifest['version'] = version
    manifest['apple_gptk'] = 'player'
    manifest['apple_gptk_link'] = LINK
    manifest.pop('apple_component_unmodified', None)
    manifest['derived_from_unbundled'] = {'version': base_version, 'archive_sha256': sha(str(base) + '.tar.gz'),
                                          'built_against_gptk': manifest.get('gptk_version'),
                                          'replaced_by_links': sorted(removed + [framework])}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    allowed = set(removed) | {framework, 'runtime.json'} | {k for k in before if k.startswith(framework + '/') or k.startswith('licenses/apple')}
    if not differences <= allowed:
        raise ValueError('Unexpected differences from the base runtime: ' + ', '.join(sorted(differences - allowed)))

    archive = write_archive(dest)
    proof = json.loads(Path(str(base) + '.tar.json').read_text())
    proof.update(version=version, archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
                 unpacked_file_bytes=sum(v.get('bytes', 0) for v in files.values()),
                 files=len(files), derived_from=base_version, apple_gptk='player')
    archive.with_suffix('.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps({k: proof[k] for k in ('version', 'archive_sha256', 'archive_bytes', 'files', 'derived_from')}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--base-version', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--gptk', type=Path, default=REPO / 'gptk/gptk',
                        help='The Game Porting Toolkit folder the base was assembled from (holds redist/)')
    args = parser.parse_args()
    derive(args.workspace.resolve() / 'artifacts', args.base_version, args.version, args.gptk.resolve())
