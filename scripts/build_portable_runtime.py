#!/usr/bin/env python3
"""Build a separate Wine runtime from pinned upstream inputs; never install it.

Developer tool only. The native setup worker does not require Python, Homebrew,
an installed development engine, or this checkout on a player's Mac.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

import rosetta_toolchain

ROOT = Path(__file__).resolve().parents[1]
SOJU = '3a350b32bf906dd2a509b18c642a5a2676de022a'
INPUTS = {
    'wine.tar.gz': ('https://media.codeweavers.com/pub/crossover/source/crossover-sources-26.3.0.tar.gz', 'ac99c8ca4b3848f3e81784135f023df266b61c2345726ea55a50b3e030dd6872'),
    'libraries.tar.gz': ('https://github.com/frankea/Whisky/releases/download/v3.1.1/Libraries.tar.gz', '01f3a1b43b98065fe20c529c1023b61dd79a6d2ad93bba6040865f646481ccf3'),
    'freetype.tar.gz': ('https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz', '5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747'),
    'soju.tar.gz': ('https://codeload.github.com/BCD1210/soju/tar.gz/' + SOJU, 'e719e417fe92f2600ef4dc3896eb499c0933f05d785918cf7aa8f616cdd75e38'),
    'mono.tar.xz': ('https://github.com/wine-mono/wine-mono/releases/download/wine-mono-10.4.1/wine-mono-10.4.1-x86.tar.xz', 'a16606ef0724202e6a6848ece6e0cbba64d11e2f11aefe744af1d93c6d9f99bb'),
}
PATCHES = {
    'ncrypt-persisted-keys.patch': '6283b3637b1d98bf43f68618c0991ee23e719c8bddaed05eff05e3989c954efe',
}
# Homebrew's mingw-w64 (GCC 16.2.0, binutils 2.47 when this was written), the toolchain
# CrossOver 26.3 builds its Windows modules with. Build machine only.
MINGW_GCC = Path('/opt/homebrew/opt/mingw-w64/bin')


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run(command, **kwargs):
    subprocess.run([str(a) for a in command], check=True, **kwargs)


def fetch(work):
    downloads = work / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in INPUTS.items():
        target = downloads / name
        if not target.exists():
            part = target.with_suffix(target.suffix + '.part')
            run(['/usr/bin/curl', '--fail', '--location', '--retry', '2',
                 '--proto', '=https', '--proto-redir', '=https', '-o', part, url])
            if sha(part) != expected:
                raise RuntimeError('Downloaded input hash mismatch: ' + name)
            part.rename(target)
        if sha(target) != expected:
            raise RuntimeError('Input hash mismatch: ' + name)


def extract(work, name, prefix=None):
    destination = work / 'sources' / name
    marker = destination / '.extracted.json'
    filename = name + ('.tar.xz' if name == 'mono' else '.tar.gz')
    identity = INPUTS[filename][1]
    if marker.exists():
        if json.loads(marker.read_text())['sha256'] != identity:
            raise RuntimeError('Source identity changed: ' + name)
        return destination
    # An interrupted extraction is repeatable only inside this owned source tree.
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(work / 'downloads' / filename) as archive:
        members = [m for m in archive.getmembers() if not prefix or m.name.startswith(prefix)]
        archive.extractall(destination, members=members, filter='data')
    marker.write_text(json.dumps({'sha256': identity}) + '\n')
    return destination


# Applied to CrossOver's Wine in this order; names without a folder are in patches/.
# Edit a patch or add one here to change the runtime; git tracks their contents and the
# build record lists each one's hash. Soju's comes from the pinned download (PATCHES).
WINE_PATCHES = [
    'wine-v1-canvas.patch',
    # Mouselook with raw mouse input; inert unless the app lists the game in
    # WINEMAC_MOUSELOOK.
    'wine-mouselook.patch',
    'wine-portable-directory-boolean.patch',
    'soju/ncrypt-persisted-keys.patch',
    # v1.0: the registry save from a forked server, Unix library loads outside the
    # virtual-memory lock, the window driver's activation, canvas and pointer fixes.
    # Their diagnostics (WINESERVER_REGISTRY_LOG, WINE_SYSCALL_LOG, WINE_IO_ERROR_LOG,
    # WINE_DISPLAY_LOG, WINEMAC_INPUT_STATS) stay off unless set.
    'wine-server-registry-save.patch',
    'wine-ntdll-syscall-log.patch',
    'wine-win32u-display-log.patch',
    'wine-winemac-activation.patch',
    # ProcessParameters.Flags gets RTL_USER_PROC_IMAGE_KEY_MISSING (0x4000) when the
    # image has no Image File Execution Options key, as Windows and Proton set it.
    'wine-ntdll-image-key-missing.patch',
    # The real GS base sits on the TEB while Windows code runs and on macOS thread
    # data while Unix code runs (dispatchers, signal handlers, user callbacks), so
    # every gs-relative access from Windows code sees the TEB, like on Linux.
    # Also skips CW HACK 18756 for marked threads (fast TlsAlloc slots back).
    # Rosetta-only, and MACFINALS_NO_GS_TEB disables it.
    'wine-macos-gsbase-teb-switch.patch',
    # Follow-up: the loader.c marker moves from TEB Instrumentation[1] (which
    # D3D12-loaded code dereferences as a pointer) into SYSTEM_DLL_INIT_BLOCK,
    # Wine's existing Unix-to-PE channel, filled by the Unix loader.
    'wine-macos-gsbase-loader-marker.patch',
    # All Windows executable memory (VirtualAlloc, VirtualProtect, section views, images
    # mapped by hand) is registered with GPTK's register_non_native_code_region, as CW HACK
    # 22434 does for PE modules, so Rosetta gives that code TEB semantics through %gs.
    # Rosetta-only, and MACFINALS_NO_EXEC_REGIONS disables it. The GS-base switch above
    # crashes with D3DMetal, so it becomes opt-in (MACFINALS_GS_TEB).
    'wine-macos-register-exec-regions.patch',
    # With every Windows code region registered, gs-relative TLS reads see the TEB, so CW
    # HACK 18756 is skipped and the first TlsAlloc returns 2 as on Windows, not 65.
    'wine-macos-exec-regions-tls.patch',
    # GPTK's region table silently ignores anything past 1024 entries, repeats included.
    # Reserved Windows address space is registered whole and once; elsewhere whole views,
    # once each, with an ERR if the table fills.
    'wine-macos-exec-region-budget.patch',
    # Address searches past host mappings Wine has no view for: skip each mapping instead of
    # stepping through it 64 KB at a time under the virtual-memory lock (over 1 s measured),
    # and treat addresses above the host's address space as taken, which used to end the
    # search with STATUS_NO_MEMORY while free memory remained.
    'wine-macos-host-region-search.patch',
    # macOS delivers a received datagram's TOS byte as an IP_RECVTOS control message, not
    # IP_TOS, which Wine dropped ("Unhandled IPPROTO_IP message header type 27"). Steam's
    # networking then asserts ("No control data returned even though we asked for TOS?")
    # and writes a minidump from inside THE FINALS, whose thread suspension can hang the game.
    'wine-socket-recvtos-cmsg.patch',
    # Diagnostics only: when a thread is killed for "nested exception on signal stack", also
    # log the exception (code, fault address), the thread's start address and its stacks.
    # Seen in THE FINALS at usr1_handler's first read of its signal context.
    'wine-nested-exception-diag.patch',
]


def apply_once(source, patch):
    result = subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source,
                            capture_output=True)
    if result.returncode == 0:
        run(['git', 'apply', patch], cwd=source)
    else:
        reverse = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)],
                                 cwd=source, capture_output=True)
        if reverse.returncode == 0:
            return
        # Some pinned Soju patches have overlapping hunk context rejected by
        # git apply. BSD patch accepts them without fuzzy context matching.
        run(['/usr/bin/patch', '--dry-run', '-N', '-F0', '-p1', '-i', patch], cwd=source)
        run(['/usr/bin/patch', '-N', '-F0', '-p1', '-i', patch], cwd=source)


def build(work, tools, headers, jobs):
    fetch(work)
    wine = extract(work, 'wine', 'sources/wine/') / 'sources/wine'
    libraries = extract(work, 'libraries', 'Libraries/Wine/lib/') / 'Libraries/Wine/lib'
    ft = extract(work, 'freetype') / 'freetype-2.13.3'
    soju = extract(work, 'soju') / ('soju-' + SOJU)
    mono = extract(work, 'mono') / 'wine-mono-10.4.1'
    # Later patches change lines earlier ones added, so a patched tree can't tell which
    # patches it holds; this record can. New entries at the end of WINE_PATCHES apply
    # to the existing tree. Changing or reordering an applied one needs clean source.
    record_path = wine / '.macfinals-patches.json'
    recorded = json.loads(record_path.read_text()) if record_path.exists() else []
    applied = {}
    for index, name in enumerate(WINE_PATCHES):
        if name.startswith('soju/'):
            p = soju / 'patches' / name.removeprefix('soju/')
            if sha(p) != PATCHES[p.name]:
                raise RuntimeError('Upstream compatibility patch changed')
        else:
            p = ROOT / 'patches' / name
        applied[name] = sha(p)
        if index < len(recorded):
            if recorded[index] != [name, applied[name]]:
                raise RuntimeError(name + ' differs from the one already applied; delete '
                                   + str(work / 'sources/wine') + ' to rebuild from clean source')
            continue
        apply_once(wine, p)
        recorded.append([name, applied[name]])
        record_path.write_text(json.dumps(recorded, indent=2) + '\n')
    if len(recorded) > len(WINE_PATCHES):
        raise RuntimeError('A patch was removed from WINE_PATCHES; delete '
                           + str(work / 'sources/wine') + ' to rebuild from clean source')
    # The complete native driver supersedes Soju's native-window patches.
    # Omit its GOG-only Chromium injection (not used by the v6 launcher), which
    # also needs a rebase against CodeWeavers' Ubisoft environment-variable hook.
    # Focus and mouse changes come only from the pinned driver patches above.
    deps = work / 'deps'
    (deps / 'lib').mkdir(parents=True, exist_ok=True)
    for p in libraries.glob('*.dylib'):
        shutil.copy2(p, deps / 'lib' / p.name, follow_symlinks=True)
    env = os.environ.copy()
    env.pop('DESTDIR', None)
    # Phase 2 was compiled with the macOS 26.5 SDK; pin it so an Xcode update can't
    # switch it under an existing build tree. Configure runs under Rosetta, so clang
    # comes from a developer folder whose xcrun has an Intel slice (rosetta_toolchain.py).
    env['DEVELOPER_DIR'] = rosetta_toolchain.developer_dir()
    env['SDKROOT'] = rosetta_toolchain.SDKROOT
    # Windows modules are built with MinGW-w64 GCC and GNU binutils, as CrossOver's own
    # Wine is. THE FINALS rejects (E111000D) an ntdll.dll of the same source built with
    # llvm-mingw's clang and lld, whose PE layout differs (no .edata/.xdata/.idata);
    # swapping only that file decides pass or fail. Only the MinGW folder is put on PATH,
    # so winebuild finds GNU as/ld/ar and nothing else from Homebrew leaks in.
    if not (MINGW_GCC / 'x86_64-w64-mingw32-gcc').is_file():
        raise RuntimeError('MinGW-w64 GCC is missing: brew install mingw-w64')
    env['PATH'] = ':'.join([str(tools / 'bison/bin'), str(MINGW_GCC),
                          '/usr/bin', '/bin', '/usr/sbin', '/sbin'])
    env['MACOSX_DEPLOYMENT_TARGET'] = '26.0'
    flags = '-O2 -g0 -ffile-prefix-map=' + str(work) + '=/build/overwatch-2-mac'
    logdir = work / 'build-logs'
    logdir.mkdir(exist_ok=True)
    with (logdir / 'freetype.log').open('w') as log:
        if not (ft / '.portable-configured').exists():
            if (ft / 'config.mk').exists():
                run(['make', 'distclean'], cwd=ft, env=env, stdout=log, stderr=subprocess.STDOUT)
            run(['/usr/bin/arch', '-x86_64', './configure', '--prefix=' + str(deps),
                 '--without-harfbuzz', '--without-png', '--without-brotli', '--without-bzip2',
                 '--enable-shared', '--disable-static', 'CC=/usr/bin/clang -arch x86_64',
                 'CFLAGS=' + flags], cwd=ft, env=env, stdout=log, stderr=subprocess.STDOUT)
            (ft / '.portable-configured').write_text(str(deps))
        if (ft / '.portable-configured').read_text() != str(deps):
            raise RuntimeError('FreeType configured for a different workspace')
        run(['make', '-j' + str(jobs)], cwd=ft, env=env, stdout=log, stderr=subprocess.STDOUT)
        run(['make', 'install'], cwd=ft, env=env, stdout=log, stderr=subprocess.STDOUT)
    print('FreeType built from pinned source', flush=True)
    builddir = work / 'wine-build'
    builddir.mkdir(exist_ok=True)
    with (logdir / 'wine.log').open('w') as log:
        configuration = builddir / '.portable-configuration'
        # 1.2 adds SDL2 (controllers other than PlayStation ones); an older tree is
        # configured again so config.h gains SONAME_LIBSDL2. MinGW GCC (above) also needs a
        # new configure; its marker removes everything an LLVM cross build left behind.
        if (not (builddir / 'Makefile').exists() or not configuration.exists()
                or configuration.read_text() != 'vulkan-sdl2-mingw-gcc-v1\n'):
            for architecture in ('i386-windows', 'x86_64-windows'):
                for output in builddir.glob('**/' + architecture + '/**/*'):
                    if output.is_file() and not output.is_symlink(): output.unlink()
            run(['/usr/bin/arch', '-x86_64', wine / 'configure', '--prefix=/usr/local',
                 '--enable-archs=i386,x86_64', '--without-x', '--disable-tests',
                 '--without-gstreamer', '--with-vulkan',
                 'FREETYPE_CFLAGS=-I' + str(deps / 'include/freetype2'),
                 'FREETYPE_LIBS=-L' + str(deps / 'lib') + ' -lfreetype',
                 'GNUTLS_CFLAGS=-I' + str(headers),
                 'GNUTLS_LIBS=-L' + str(deps / 'lib') + ' -lgnutls',
                 'INOTIFY_CFLAGS=-I' + str(soju / 'third_party/libinotify-kqueue'),
                 'INOTIFY_LIBS=-L' + str(deps / 'lib') + ' -linotify',
                 'SDL2_CFLAGS=-I' + str(work / 'clean-deps/include/SDL2'),
                 'SDL2_LIBS=-L' + str(work / 'clean-deps/lib') + ' -lSDL2',
                 'LDFLAGS=-Wl,-headerpad,0x1000 -L' + str(deps / 'lib'), 'CFLAGS=' + flags,
                 'CROSSCFLAGS=' + flags,
                 'x86_64_CC=x86_64-w64-mingw32-gcc', 'i386_CC=i686-w64-mingw32-gcc',
                 'CC=/usr/bin/clang -arch x86_64', 'CXX=/usr/bin/clang++ -arch x86_64'],
                cwd=builddir, env=env, stdout=log, stderr=subprocess.STDOUT)
            configuration.write_text('vulkan-sdl2-mingw-gcc-v1\n')
        config = builddir / 'include/config.h'
        text = config.read_text()
        for required in ('#define HAVE_SYS_INOTIFY_H 1', '#define SONAME_LIBGNUTLS ', '#define SONAME_LIBFREETYPE ',
                         '#define SONAME_LIBSDL2 '):
            if required not in text:
                raise RuntimeError('Wine lacks a required dependency: ' + required)
        for name, library in [('GNUTLS', 'libgnutls.30.dylib'), ('FREETYPE', 'libfreetype.6.dylib'),
                              ('VULKAN', 'libMoltenVK.dylib'), ('MOLTENVK', 'libMoltenVK.dylib'),
                              ('SDL2', 'libSDL2-2.0.0.dylib')]:
            text = re.sub(r'#define SONAME_LIB' + name + r' .*',
                          '#define SONAME_LIB' + name + ' "@rpath/' + library + '"', text)
        if config.read_text() != text:config.write_text(text)
        # Link commands do not themselves invalidate existing make targets.
        # On an older workspace, relink only newly built native outputs once.
        padding = builddir / '.portable-header-padding'
        if not padding.exists():
            for output in list(builddir.glob('dlls/**/*.so')) + [builddir / p for p in
                    ('loader/wine', 'server/wineserver', 'tools/wine/wine')]:
                if output.is_file() and not output.is_symlink(): output.unlink()
        cross_paths = builddir / '.portable-cross-paths'
        if not cross_paths.exists():
            # Wine has separate PE compiler flags. Native CFLAGS alone do not
            # sanitize Windows assertion strings or suppress Windows debug data.
            for architecture in ('i386-windows','x86_64-windows'):
                for output in builddir.glob('**/' + architecture + '/**/*.o'):
                    if not output.is_symlink(): output.unlink()
        nested_paths = builddir / '.portable-cross-nested-paths'
        if not nested_paths.exists():
            for architecture in ('i386-windows','x86_64-windows'):
                for output in builddir.glob('**/' + architecture + '/**/*.o'):
                    if output.parent.name != architecture and not output.is_symlink(): output.unlink()
        print('Building full Wine engine (isolated, two architectures)', flush=True)
        run(['make', '-j' + str(jobs), 'LDFLAGS=-Wl,-headerpad,0x1000 -L' + str(deps / 'lib'),
             'i386_CFLAGS=' + flags, 'x86_64_CFLAGS=' + flags],
            cwd=builddir, env=env, stdout=log, stderr=subprocess.STDOUT)
        padding.write_text('0x1000\n')
        cross_paths.write_text(flags + '\n')
        nested_paths.write_text(flags + '\n')
        run(['make', 'install', 'DESTDIR=' + str(work / 'install')], cwd=builddir,
            env=env, stdout=log, stderr=subprocess.STDOUT)
    installed = work / 'install/usr/local'
    shutil.copytree(mono, installed / 'share/wine/mono/wine-mono-10.4.1', dirs_exist_ok=True)
    record = {'inputs': {n: {'url': v[0], 'sha256': v[1]} for n,v in INPUTS.items()},
              'wine_patches': applied,
              'gnutls_header_sha256': sha(headers / 'gnutls/gnutls.h'),
              'minimum_macos': '26.0', 'runtime_scope': 'Wine + FreeType built from source; DXMT assembly is separate',
              'without_vulkan': False, 'without_gstreamer': True, 'with_sdl2': True,
              'native_linker_header_padding': '0x1000',
              'windows_compiler_paths_remapped': True,
              'driver_matches_accepted_patch': True}
    (work / 'wine-build-proof.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Full Wine build complete; no live installation modified', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--toolchains', type=Path, default=ROOT / 'runtime/toolchains')
    parser.add_argument('--gnutls-headers', type=Path,
                        help='Defaults to the pinned native dependency build headers')
    parser.add_argument('--download-only', action='store_true',
                        help='Fetch verified inputs before building native dependencies')
    parser.add_argument('--jobs', type=int, choices=range(1,5), default=2)
    args = parser.parse_args()
    work = args.workspace.resolve()
    if work == ROOT or work in ROOT.parents or work.name != 'phase-2':
        parser.error('Use a dedicated directory named phase-2')
    # A configured tree only rebuilds what changed and reinstalls in place.
    rebuild = (work / 'wine-build/.portable-configuration').exists()
    if shutil.disk_usage(work.parent).free < (3 if rebuild else 12) * 2**30:
        parser.error('A rebuild needs 3 GiB of free working headroom' if rebuild
                     else 'A full source build needs 12 GiB of free working headroom')
    work.mkdir(parents=True, exist_ok=True)
    if args.download_only:
        fetch(work)
    else:
        headers = (args.gnutls_headers or work / 'clean-deps/include').resolve()
        if not (headers / 'gnutls/gnutls.h').is_file():
            parser.error('Build the pinned native dependencies first, or supply --gnutls-headers')
        if not (work / 'clean-deps/include/SDL2/SDL.h').is_file():
            parser.error('Build the pinned native dependencies first: SDL2 is missing')
        build(work, args.toolchains.resolve(), headers, args.jobs)
