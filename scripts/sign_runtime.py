#!/usr/bin/env python3
"""Re-sign an existing qualified runtime without changing its unsigned code.

This records a signing derivation, not a fresh source build or release approval.
The pinned Apple library is preserved. Requires Python 3.11 with tar data filter.

The main runtime carries Apple's libd3dshared.dylib, which must stay Apple's. SteamDriver's runtime
(environments/finals, unbundle_gptk.py) carries none: that path is a link into the player's own
Game Porting Toolkit, left as it is. --keep-dyld adds allow-dyld-environment-variables to every
executable that has entitlements: SteamDriver's runtime loads its Rosetta hook with
DYLD_INSERT_LIBRARIES (compat-tool/run), which the hardened runtime otherwise drops
(add_nt_hook.py signs those executables ad hoc for that reason; notarization needs the hardened
runtime).
"""
import argparse
import copy
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import subprocess
import struct
import tarfile
import tempfile

from derive_runtime import sha, tree, write_archive

APPLE = 'lib/external/libd3dshared.dylib'
APPLE_SHA = '5131e631eee8b542eadf48f4df9fd662d9aeeb59139137e0e6e14047dc434995'
KEEP_DYLD = 'com.apple.security.cs.allow-dyld-environment-variables'
MAGICS = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xce\xfa\xed\xfe'}


def linkedit_normalized(data):
    """The Mach-O (thin or universal) with each __LINKEDIT segment's vmsize zeroed. A larger
    signature (Developer ID with a timestamp instead of ad hoc) makes codesign reserve more of
    __LINKEDIT in memory, and --remove-signature doesn't shrink it back; that field is signature
    room, not code, so the unsigned comparison ignores it and nothing else."""
    data = bytearray(data)

    def thin(base):
        magic = struct.unpack_from('<I', data, base)[0]
        if magic not in (0xfeedfacf, 0xfeedface):
            return
        wide = magic == 0xfeedfacf
        ncmds = struct.unpack_from('<I', data, base + 16)[0]
        at = base + (32 if wide else 28)
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, at)
            if cmd in (0x19, 0x1) and data[at + 8:at + 24].rstrip(b'\0') == b'__LINKEDIT':
                if wide:
                    struct.pack_into('<Q', data, at + 32, 0)
                else:
                    struct.pack_into('<I', data, at + 28, 0)
            at += size

    if struct.unpack_from('>I', data, 0)[0] == 0xcafebabe:
        for i in range(struct.unpack_from('>I', data, 4)[0]):
            thin(struct.unpack_from('>I', data, 8 + 20 * i + 8)[0])
    else:
        thin(0)
    return bytes(data)


def unsigned_same(original, resigned):
    """True when the two files are the same with their signatures removed (and __LINKEDIT's
    reserved size set aside, see linkedit_normalized)."""
    with tempfile.TemporaryDirectory() as folder:
        copies = [Path(folder) / 'a', Path(folder) / 'b']
        for source, copy in zip((original, resigned), copies):
            copy.write_bytes(source.read_bytes())
            run('codesign', '--remove-signature', copy)
        return linkedit_normalized(copies[0].read_bytes()) == linkedit_normalized(copies[1].read_bytes())


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True)


def signature(path):
    info = run('codesign', '-dvv', path).stderr.decode()
    identifier = re.search(r'^Identifier=(.+)$', info, re.M)
    if not identifier:
        raise ValueError(f'Missing signing identifier: {path.name}')
    xml = run('codesign', '-d', '--xml', '--entitlements', '-', path).stdout
    return identifier.group(1), plistlib.loads(xml) if xml.strip() else None


def extract_verified(archive, dest):
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        names = [m.name for m in members]
        if len(names) != len(set(names)):
            raise ValueError('Duplicate archive entries')
        for member in members:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or str(name) != member.name:
                raise ValueError('Unsafe archive path: ' + member.name)
            if not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError('Unsupported archive entry: ' + member.name)
            tarfile.data_filter(member, str(dest))
        manifest = json.load(tar.extractfile('runtime.json'))
        if {m.name for m in members if not m.isdir()} != set(manifest['files']) | {'runtime.json'}:
            raise ValueError('Archive differs from its file list')
        tar.extractall(dest, filter='data')
    for name, entry in manifest['files'].items():
        path = dest / name
        if 'symlink' in entry:
            valid = path.is_symlink() and os.readlink(path) == entry['symlink']
        else:
            valid = not path.is_symlink() and sha(path) == entry['sha256'] and path.stat().st_size == entry['bytes']
        if not valid:
            raise ValueError('Manifest mismatch: ' + name)
    return manifest


def sign_runtime(archive, output, version, identity, keep_dyld=False):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', version):
        raise ValueError('Invalid version')
    if output.exists() or Path(str(output) + '.tar.gz').exists():
        raise ValueError('Use a new output path')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        base = Path(temporary) / 'base'
        base.mkdir()
        manifest = extract_verified(archive, base)
        before = tree(base)
        run('/bin/cp', '-Rc', base, output)
        # Apple's own file, when the runtime carries it (a link into the player's GPTK otherwise).
        apple = APPLE in manifest['files'] and 'symlink' not in manifest['files'][APPLE]
        if apple:
            if sha(base / APPLE) != APPLE_SHA:
                raise ValueError('Unexpected Apple component')
            run('codesign', '--verify', '--strict', '-R=anchor apple', base / APPLE)
        native = []
        for name, entry in manifest['files'].items():
            if 'symlink' not in entry:
                with (base / name).open('rb') as f:
                    if f.read(4) in MAGICS and name != APPLE:
                        native.append(name)
        bundles = sorted({str(p.relative_to(output)) for p in output.rglob('*.app')}, key=lambda s: -s.count('/'))
        for name in [*sorted(native), *bundles]:
            identifier, entitlements = signature(base / name)
            if keep_dyld and entitlements is not None:
                entitlements = {**entitlements, KEEP_DYLD: True}
            args = ['codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', identity, '--identifier', identifier]
            if entitlements is not None:
                plist = Path(temporary) / 'entitlements.plist'
                plist.write_bytes(plistlib.dumps(entitlements))
                args += ['--entitlements', plist]
            run(*args, output / name)
            run('codesign', '--verify', '--strict', output / name)
            if signature(output / name) != (identifier, entitlements):
                raise ValueError('Signing metadata changed: ' + name)
            info = run('codesign', '-dvv', output / name).stderr.decode()
            if 'Authority=Developer ID Application:' not in info or '(runtime)' not in info:
                raise ValueError('Missing Developer ID or hardened runtime: ' + name)
            print('Signed ' + name, flush=True)
        for name in native:
            if not unsigned_same(base / name, output / name):
                raise ValueError('Unsigned code changed: ' + name)
        after = tree(output)
        changed = {k for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
        allowed = set(native) | {b + '/Contents/_CodeSignature/CodeResources' for b in bundles}
        if not changed <= allowed or before.get(APPLE) != after.get(APPLE):
            raise ValueError('Unexpected runtime changes: ' + str(sorted(changed - allowed)))
        original = copy.deepcopy(manifest)
        for name in changed:
            path = output / name
            manifest['files'][name] = {'sha256': sha(path), 'bytes': path.stat().st_size}
        manifest['version'] = version
        manifest['signing_derivation'] = {'base_version': original['version'], 'base_archive_sha256': sha(archive),
            'identity': identity, 'changed': sorted(changed), 'unsigned_code_identical': True,
            'apple_component_unmodified': True, 'apple_component_bundled': apple,
            'allow_dyld_environment_variables': keep_dyld}
        (output / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n')
        result = write_archive(output)
        proof = {'version': version, 'archive_sha256': sha(result), 'archive_bytes': result.stat().st_size,
                 'signed_native_components': len(native), **manifest['signing_derivation']}
        result.with_suffix('.json').write_text(json.dumps(proof, indent=2) + '\n')
        print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--keep-dyld', action='store_true', help='SteamDriver runtime: keep DYLD_INSERT_LIBRARIES for the Rosetta hook')
    args = parser.parse_args()
    sign_runtime(args.archive.resolve(), args.output.resolve(), args.version, args.identity, args.keep_dyld)
