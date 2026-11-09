#!/usr/bin/env python3
"""Derive a runtime that carries the in-process Rosetta NT hook from an assembled one.

An assembled runtime has no hook and signs the Wine executables with the hardened runtime,
which makes dyld silently strip DYLD_INSERT_LIBRARIES. This makes an APFS clone with
lib/macfinals-nt-hook.dylib added and the Wine executables and game app re-signed ad hoc
without the hardened runtime, keeping identifiers and entitlements. Unsigned, each re-signed
executable differs from the base's only in the signature's size. Everything else stays byte-identical, and the
manifest, archive and proof are written beside the base.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

import sys as _sys
_sys.path.append(str(Path(__file__).resolve().parents[3] / 'scripts'))  # shared helpers (game_mode_app)
import game_mode_app
from derive_runtime import file_minimum, format_macos, parse_macos, run, sha, tree, write_archive

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / 'tests/rosetta_nt/hooklib.dylib'
HOOK_NAME = 'lib/macfinals-nt-hook.dylib'
EXECUTABLES = ('bin/wine', 'bin/wineserver', 'lib/wine/x86_64-unix/wine')


def resign_ad_hoc(path):
    """Sign PATH ad hoc without the hardened runtime, keeping identifier and entitlements."""
    info = subprocess.run(['codesign', '-dvv', str(path)], capture_output=True, text=True).stderr
    identifier = re.search(r'^Identifier=(.+)$', info, re.M).group(1)
    xml = subprocess.run(['codesign', '-d', '--xml', '--entitlements', '-', str(path)], capture_output=True).stdout
    args = ['codesign', '--force', '--sign', '-', '--identifier', identifier]
    with tempfile.TemporaryDirectory() as folder:
        if xml.strip():
            plist = Path(folder) / 'entitlements.plist'
            plist.write_bytes(plistlib.dumps(plistlib.loads(xml)))
            args += ['--entitlements', plist]
        run(*args, path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run('codesign', '--verify', '--strict', path)
    after = subprocess.run(['codesign', '-dvv', str(path)], capture_output=True, text=True).stderr
    if '(runtime)' in after or 'Signature=adhoc' not in after:
        raise ValueError(f'{path} is not signed ad hoc without the hardened runtime')


def code_of(path):
    """PATH unsigned, without __LINKEDIT's vmsize, which only follows the signature's size."""
    with tempfile.TemporaryDirectory() as folder:
        copy = Path(folder) / 'copy'
        shutil.copy2(path, copy)
        run('codesign', '--remove-signature', copy)
        data = bytearray(copy.read_bytes())
    for cmd, offset, _ in game_mode_app.load_commands(data):
        if cmd == game_mode_app.LC_SEGMENT_64 and data[offset + 8:offset + 24].rstrip(b'\0') == b'__LINKEDIT':
            data[offset + 32:offset + 40] = bytes(8)
    return bytes(data)


def derive(artifacts, base_version, version, hook):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid runtime version')
    base, dest = artifacts / base_version, artifacts / version
    if dest.exists() or Path(str(dest) + '.tar.gz').exists():
        raise ValueError('Use a new version; existing artifact is never overwritten')
    manifest = json.loads((base / 'runtime.json').read_text())
    if manifest['version'] != base_version:
        raise ValueError('The base artifact names a different version')
    if HOOK_NAME in manifest['files']:
        raise ValueError(f'{base_version} already carries the hook')
    minimum = manifest.get('minimum_macos', '26.0')
    if file_minimum(hook)[1] > parse_macos(minimum):
        raise ValueError(f'{hook} requires macOS {format_macos(file_minimum(hook)[1])}, newer than the runtime')

    run('/bin/cp', '-Rc', base, dest)  # APFS clone: only re-signed files take space
    shutil.copy2(hook, dest / HOOK_NAME)
    changed = [*EXECUTABLES, HOOK_NAME]
    for relative in EXECUTABLES:
        resign_ad_hoc(dest / relative)
        if code_of(base / relative) != code_of(dest / relative):
            raise ValueError(f'{relative} differs from the base beyond its signature')
    bundle = dest / game_mode_app.BUNDLE
    if bundle.exists():
        resign_ad_hoc(dest / game_mode_app.EXECUTABLE)  # inner first; signing the bundle seals it
        resign_ad_hoc(bundle)
        changed += [f for f in manifest['files'] if f.startswith(game_mode_app.BUNDLE + '/')
                    and 'symlink' not in manifest['files'][f]]

    for relative in changed:
        manifest['files'][relative] = {'sha256': sha(dest / relative), 'bytes': (dest / relative).stat().st_size}
    manifest['version'] = version
    manifest['derived_from'] = {'version': base_version, 'archive_sha256': sha(str(base) + '.tar.gz'),
                                'nt_hook_sha256': sha(hook), 'resigned_ad_hoc': sorted(set(changed) - {HOOK_NAME})}
    (dest / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')

    before, after = tree(base), tree(dest)
    differences = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
    if not differences <= {*changed, 'runtime.json'}:
        raise ValueError('Unexpected differences from the base runtime: ' + ', '.join(sorted(differences)))

    archive = write_archive(dest)
    proof = json.loads(Path(str(base) + '.tar.json').read_text())
    proof.update(version=version, archive_sha256=sha(archive), archive_bytes=archive.stat().st_size,
                 unpacked_file_bytes=sum(v.get('bytes', 0) for v in manifest['files'].values()),
                 files=len(manifest['files']), derived_from=base_version, nt_hook_sha256=sha(hook))
    archive.with_suffix('.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--base-version', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--hook', type=Path, default=HOOK)
    args = parser.parse_args()
    derive(args.workspace.resolve() / 'artifacts', args.base_version, args.version, args.hook.resolve())
