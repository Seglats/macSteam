#!/usr/bin/env python3
"""Build Phase 3's native app using CLI tools; no Xcode project/GUI required.

Private preview embeds the exact qualified Phase 2 archive. No account, game,
cache, or installed prefix is copied. Uses the tracked owner artwork by default; --icon accepts an ICNS override.
The bundle is named Recall; its identifier and data folder keep their original values.
"""
import argparse,tarfile
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
from build_portable_setup import MINIMUM_MACOS, build as build_setup
from derive_runtime import archive_minimum, check_archive, format_macos, parse_macos

ROOT=Path(__file__).resolve().parents[1]
# The bundle and executable name players see; Brand.swift holds the in-app
# copy. Its own identifier and data folder keep it apart from Recall, which it is built on.
APP_NAME='MacVolvo'
APP_BUILD='9'
# The games' voice chat runs under this app's microphone permission (Microphone.swift); the
# hardened runtime gives an app signed without the entitlement silence, with no prompt.
ENTITLEMENTS={'com.apple.security.device.audio-input':True}
MICROPHONE_USAGE='Voice chat in Counter-Strike 2 and Deadlock uses your microphone.'
RESOURCES=('Wordmark.png','WordmarkDark.png')
# llvm-mingw (docs/HANDOFF.md) builds the Windows stand-in for Steam's steamwebhelper.exe.
MINGW_CLANG=ROOT/'runtime/toolchains/llvm-mingw-20261006-ucrt-macos-universal/bin/x86_64-w64-mingw32-clang'
def run(*args):subprocess.run([str(a) for a in args],check=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
# The runtime's Mac driver is compiled from the CrossOver 26.3 sources (docs/UPSTREAM-RECALL.md),
# so it also fits NotProton's runner of that build, named as NotProton names the runner folder.
NOTPROTON_DRIVER='lib/wine/x86_64-unix/winemac.so'
NOTPROTON_BUILDS=('26.3.0.39832',)
# MacVolvo's DXMT (Recall's, paired with this Mac driver's canvas) for the "MacVolvo DXMT" variant:
# (runtime file, place under the runner's lib/). CrossOver keeps a copy of winemetal.dll in
# lib/wine as well, and its PE half has to match winemetal.so, so that copy is replaced too.
NOTPROTON_DXMT=[
    ('lib/wine/x86_64-windows/d3d11.dll','dxmt/x86_64-windows/d3d11.dll'),
    ('lib/wine/x86_64-windows/dxgi.dll','dxmt/x86_64-windows/dxgi.dll'),
    ('lib/wine/x86_64-windows/d3d10core.dll','dxmt/x86_64-windows/d3d10core.dll'),
    ('lib/wine/x86_64-windows/winemetal.dll','dxmt/x86_64-windows/winemetal.dll'),
    ('lib/wine/x86_64-windows/winemetal.dll','wine/x86_64-windows/winemetal.dll'),
    ('lib/wine/i386-windows/d3d11.dll','dxmt/i386-windows/d3d11.dll'),
    ('lib/wine/i386-windows/dxgi.dll','dxmt/i386-windows/dxgi.dll'),
    ('lib/wine/i386-windows/d3d10core.dll','dxmt/i386-windows/d3d10core.dll'),
    ('lib/wine/x86_64-unix/winemetal.so','dxmt/x86_64-unix/winemetal.so'),
]
NOTPROTON_PROFILE='config/dxmt.conf'

def notproton_profiles(profile):
    """The runtime's DXMT renderer profile (written for Overwatch.exe) as one file per game, for
    the per-game profiles in NotProton (app/Sources/NotProton.swift). The fullscreen canvas size
    lines go: MacVolvo's own launcher fills them in from its resolution setting, NotProton's Steam
    has none, so DXMT keeps the game's own size. CS2 takes 4 shader compiler threads, as
    portable_steam.h gamesProfile gives it."""
    start=profile.index('[Overwatch.exe]')
    body=profile[start+len('[Overwatch.exe]'):]
    if '\n[' in body:body=body[:body.index('\n[')+1]
    lines=[l for l in body.split('\n') if l.split('=')[0].strip() not in ('dxgi.fullscreenCanvasWidth','dxgi.fullscreenCanvasHeight')]
    out={}
    for name,exe in (('cs2.conf','cs2.exe'),('deadlock.conf','deadlock.exe')):
        game=[l for l in lines if not (exe=='cs2.exe' and l.startswith('# Eight normal-priority workers'))]
        game=[('# Four for CS2, as MacVolvo\'s launcher gives it.\nd3d11.shaderCompilerThreads = 4' if exe=='cs2.exe' and l.split('=')[0].strip()=='d3d11.shaderCompilerThreads' else l) for l in game]
        out[name]=f'# MacVolvo renderer profile for {exe} (MacVolvo DXMT), from MacVolvo\'s runtime.\n[{exe}]'+'\n'.join(game).rstrip()+'\n'
    return out

# Games with a Wine of their own (environments/<name>): each ships as a Steam Play compatibility
# tool the app installs into Steam's compatibilitytools.d (app/Sources/GameTools.swift). The
# runtime archive comes from that environment's own build (--game-tool NAME=ARCHIVE).
GAME_TOOLS={'finals':{'id':'notproton-steamdriver-finals','game':'THE FINALS','display':'SteamDriver: THE FINALS',
                      'source':ROOT/'environments/finals/compat-tool','required':('bin/wine','lib/macfinals-nt-hook.dylib')}}
def bundle_game_tool(resources,name,archive):
    tool=GAME_TOOLS[name]
    with tarfile.open(archive) as t:
        names=set(t.getnames())
        manifest=json.load(t.extractfile('runtime.json'))
    missing=[r for r in tool['required'] if r not in names]
    if missing:raise ValueError(f'{archive} lacks {missing} for the {name} tool')
    vdf=(tool['source']/'compatibilitytool.vdf').read_text()
    if f'"{tool["id"]}"' not in vdf or f'"display_name" "{tool["display"]}"' not in vdf:raise ValueError(f'{name}: compatibilitytool.vdf does not match GAME_TOOLS')
    dest=resources/'GameTools'/tool['id'];dest.mkdir(parents=True)
    for f in ('run','compatibilitytool.vdf','toolmanifest.vdf'):shutil.copy2(tool['source']/f,dest/f)
    run('/bin/cp','-c',archive,dest/'runtime.tar.gz')
    record={'id':tool['id'],'game':tool['game'],'display':tool['display'],'runtime':manifest['version'],'runtime_sha256':sha(archive)}
    (dest/'tool.json').write_text(json.dumps(record,indent=2)+'\n')
    return record

def build(output,workspace,runtime_archive,icon=None,identity=None,variant='dxmt',game_tools=()):
    if variant not in ('dxmt','gptk4'):raise ValueError('Unknown app variant')
    evaluation=variant=='gptk4'
    app_name='MacVolvo GPTK4' if evaluation else APP_NAME
    bundle_id='app.macvolvo.gptk4' if evaluation else 'app.macvolvo.launcher'
    release=json.loads((ROOT/'app/Resources/release.json').read_text())
    if evaluation:
        runtime_manifest=check_archive(runtime_archive)
        proof=json.loads(Path(str(runtime_archive).removesuffix('.tar.gz')+'.proof.json').read_text())
        if runtime_manifest.get('graphics_backend')!='d3dmetal' or not runtime_manifest.get('gptk_version','').startswith('4.'):
            raise ValueError('GPTK4 app requires a D3DMetal 4 runtime')
        if proof['archive_sha256']!=sha(runtime_archive):raise ValueError('GPTK4 import proof mismatch')
        release.update(runtimeVersion=runtime_manifest['version'],runtimeSHA256=proof['archive_sha256'],distribution='gptk4-evaluation')
    if sha(runtime_archive)!=release['runtimeSHA256']:raise ValueError('Unqualified runtime archive')
    # The worker rejects any file its manifest does not list, so check before shipping.
    if check_archive(runtime_archive)['version']!=release['runtimeVersion']:raise ValueError('Runtime archive names a different version')
    # Every engine binary must open on the app's minimum macOS (derive_runtime.py --minimum-macos re-stamps them).
    # An engine stamped older (1.1's 15.0) is fine: the app is what refuses older macOS.
    newest,declared=archive_minimum(runtime_archive)
    if newest>parse_macos(MINIMUM_MACOS) or parse_macos(declared)>parse_macos(MINIMUM_MACOS):
        raise ValueError(f'The runtime needs macOS {format_macos(newest)} (manifest: {declared}); the app supports {MINIMUM_MACOS}')
    if output.exists():raise ValueError('Choose a new output bundle; previous candidates are retained')
    helpers=output/'Contents/Helpers';resources=output/'Contents/Resources';macos=output/'Contents/MacOS'
    for directory in (helpers,resources,macos):directory.mkdir(parents=True)
    build_setup(helpers/'ow2-setup',ROOT/'runtime/toolchains/libarchive-3.7.7',defines=('MACVOLVO_GPTK4',) if evaluation else ())
    shutil.move(helpers/'ow2-setup.build.json',output.parent/'worker-build.json')
    run('/usr/bin/swiftc','-file-prefix-map',str(ROOT)+'=/ow2-source','-O','-target','arm64-apple-macos'+MINIMUM_MACOS,'-parse-as-library',*(['-D','MACVOLVO_GPTK4'] if evaluation else []),*sorted((ROOT/'app/Sources').glob('*.swift')),'-o',macos/app_name)
    # Steam's window on Wine 11 (portable_steam.h): the worker puts this in steamwebhelper.exe's place.
    run(MINGW_CLANG,'-O2','-municode','-mwindows','-ffile-prefix-map='+str(ROOT)+'=/ow2-source',ROOT/'scripts/webhelper_wrapper.c','-o',resources/'steamwebhelper.exe')
    rosetta=helpers/'Rosetta Check.app/Contents';(rosetta/'MacOS').mkdir(parents=True)
    source=output.parent/'rosetta-check.c';source.write_text('int main(void) { return 0; }\n')
    run('/usr/bin/clang','-ffile-prefix-map='+str(ROOT)+'=/ow2-source','-arch','x86_64','-mmacosx-version-min='+MINIMUM_MACOS,source,'-o',rosetta/'MacOS/RosettaCheck')
    (rosetta/'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':bundle_id+'.rosetta-check' if evaluation else 'app.macvolvo.rosetta-check','CFBundleExecutable':'RosettaCheck','CFBundleName':'Rosetta Check','CFBundlePackageType':'APPL','LSUIElement':True,'LSMinimumSystemVersion':MINIMUM_MACOS}))
    run('/bin/cp','-c',runtime_archive,resources/release['runtimeArchive'])
    (resources/'release.json').write_text(json.dumps(release,separators=(',',':'))+'\n')
    shutil.copy2(ROOT/'docs/candidate-parity-contract.json',resources/'candidate-parity-contract.json')
    # NotProton (app/Sources/NotProton.swift): MacVolvo's Mac driver for NotProton's CrossOver runner,
    # one folder per CrossOver build it was compiled for, taken from the runtime it ships.
    if not evaluation:
        notproton=resources/'NotProton';notproton.mkdir()
        with tempfile.TemporaryDirectory() as folder:
            run('/usr/bin/tar','-xzf',runtime_archive,'-C',folder,NOTPROTON_DRIVER,NOTPROTON_PROFILE,*sorted({s for s,_ in NOTPROTON_DXMT}))
            for build in NOTPROTON_BUILDS:
                (notproton/build).mkdir()
                shutil.copy2(Path(folder)/NOTPROTON_DRIVER,notproton/build/'winemac.so')
                # The "MacVolvo DXMT" variant's tree, laid over the runner's lib/: CrossOver keeps
                # DXMT in lib/dxmt/<arch>, and a copy of winemetal.dll in lib/wine.
                for source,target in NOTPROTON_DXMT:
                    (notproton/build/'dxmt'/target).parent.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(Path(folder)/source,notproton/build/'dxmt'/target)
            (notproton/'profiles').mkdir()
            for name,text in notproton_profiles((Path(folder)/NOTPROTON_PROFILE).read_text()).items():
                (notproton/'profiles'/name).write_text(text)
    tools=[bundle_game_tool(resources,name,archive) for name,archive in game_tools]
    # Notices from the actual reviewed runtime; kept readable without extraction.
    with tempfile.TemporaryDirectory() as folder:
        run('/usr/bin/tar','-xzf',runtime_archive,'-C',folder,'licenses')
        shutil.copytree(Path(folder)/'licenses',resources/'Licenses')
    shutil.copy2(ROOT/'licenses/PROJECT-APACHE-2.0',resources/'PROJECT-LICENSE.txt')
    shutil.copy2(ROOT/'licenses/PROJECT-NOTICE',resources/'NOTICE.txt')
    shutil.copy2(ROOT/'licenses/GITHUB-MARK-NOTICE.txt',resources/'GITHUB-MARK-NOTICE.txt')
    shutil.copy2(ROOT/'licenses/PROJECT-LINKS-NOTICE.txt',resources/'PROJECT-LINKS-NOTICE.txt')
    for name in RESOURCES:
        shutil.copy2(ROOT/'app/Resources'/name,resources/name)
    # Chakra Petch (SIL OFL 1.1) with its license, registered by LauncherStyle at run time.
    shutil.copytree(ROOT/'app/Resources/Fonts',resources/'Fonts')
    if icon:shutil.copy2(icon,resources/'AppIcon.icns')
    else:
        iconset=output.parent/'AppIcon.iconset'
        run('/usr/bin/swift',ROOT/'scripts/app_icon.swift',ROOT/'app/Resources/AppIcon.png',iconset)
        run('/usr/bin/iconutil','-c','icns',iconset,'-o',resources/'AppIcon.icns')
    (output/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':bundle_id,'CFBundleExecutable':app_name,'CFBundleName':app_name,'CFBundleDisplayName':app_name,'CFBundlePackageType':'APPL','CFBundleShortVersionString':release['appVersion'],'CFBundleVersion':APP_BUILD,'CFBundleIconFile':'AppIcon','LSMinimumSystemVersion':MINIMUM_MACOS,'NSHighResolutionCapable':True,'LSMultipleInstancesProhibited':True,'NSPrincipalClass':'NSApplication','NSMicrophoneUsageDescription':MICROPHONE_USAGE,'NSHumanReadableCopyright':'Independent community project based on Recall. Not affiliated with or endorsed by Valve, Volvo or Apple.'}))
    if identity:
        timestamp=['--timestamp'] if identity!='-' else []
        for binary in (helpers/'ow2-setup',rosetta.parent):
            run('/usr/bin/codesign','--force','--options','runtime',*timestamp,'--sign',identity,binary)
        with tempfile.TemporaryDirectory() as folder:
            entitlements=Path(folder)/'app.entitlements';entitlements.write_bytes(plistlib.dumps(ENTITLEMENTS))
            run('/usr/bin/codesign','--force','--options','runtime',*timestamp,'--entitlements',entitlements,'--sign',identity,output)
        run('/usr/bin/codesign','--verify','--deep','--strict',output)
    proof={'app_version':release['appVersion'],'app_build':APP_BUILD,'variant':variant,'bundle_identifier':bundle_id,'candidate_contract_sha256':sha(resources/'candidate-parity-contract.json'),'runtime_sha256':sha(resources/release['runtimeArchive']),'source_files':{str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'app').rglob('*')) if p.is_file()},'signed':bool(identity and identity!='-'),'signature_type':'ad-hoc' if identity=='-' else ('Developer ID' if identity else 'none'),'game_tools':tools,'files':{str(p.relative_to(output)):sha(p) for p in output.rglob('*') if p.is_file()}}
    output.with_suffix('.build.json').write_text(json.dumps(proof,indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--workspace',type=Path,default=ROOT/'runtime');p.add_argument('--runtime-archive',type=Path,default=ROOT/'runtime/downloads/runtime.tar.gz');p.add_argument('--icon',type=Path);p.add_argument('--identity')
    p.add_argument('--variant',choices=('dxmt','gptk4'),default='dxmt')
    p.add_argument('--game-tool',action='append',default=[],metavar='NAME=ARCHIVE',help='bundle a game environment\'s compatibility tool with that runtime archive (names: '+', '.join(GAME_TOOLS)+')')
    a=p.parse_args()
    tools=[]
    for item in a.game_tool:
        name,_,archive=item.partition('=')
        if name not in GAME_TOOLS or not archive:p.error(f'--game-tool {item}: expected NAME=ARCHIVE with NAME in {sorted(GAME_TOOLS)}')
        tools.append((name,Path(archive).expanduser().resolve()))
    build(a.output.resolve(),a.workspace.resolve(),a.runtime_archive.resolve(),a.icon,a.identity,a.variant,tools)
