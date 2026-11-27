"""macSteam's Uninstall outside its own folders (app/Sources/Uninstaller.swift), in a throwaway home.

Compiles the real Uninstaller with what it uses and runs it against a home folder holding
everything macSteam and its game tools leave behind, next to look-alikes that are not ours and
must stay. Deletes instead of trashing and never touches the real preferences or permissions."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
import Foundation
@main struct Harness { static func main() {
    let a=CommandLine.arguments
    NotProton.home=URL(fileURLWithPath:a[1])
    Uninstaller.useTrash=false; Uninstaller.touchDefaults=false
    if a.count>3 && a[2]=="setup" {
        // What the app sets up, through its own code: NotProton's options (driver, DXMT, a
        // renderer profile, Deadlock's renderer), a game tool, and key combo overrides.
        let res=URL(fileURLWithPath:a[3])
        try! Profiles.addGame(appID:"730",name:"Counter-Strike 2",kind:.dx11,resources:res)
        try! Profiles.addGlobal(.dx11,resources:res)
        try! Profiles.addGlobal(.dx12,resources:res)
        try! Profiles.addGame(appID:"2073850",name:"THE FINALS",kind:.dx12,resources:res)
        try! NotProton.add(resources:res)
        NotProton.keepDeadlockOnDirectX11()
        try! GameTools.install(GameTools.Tool(id:"notproton-steamdriver",game:"SteamDriver",display:"SteamDriver",
                                              runtime:"macfinals-test",bundle:URL(fileURLWithPath:a[4])))
        try! KeyOverrides.save(KeyOverrides.globalID,["ctrl+space"])
        try! KeyOverrides.save("app-2073850",["cmd+space"])
        print("{}");return
    }
    if a.count>2 && a[2]=="uninstall-all" {
        // AppModel.uninstallEverything's steps outside the app bundle and preferences.
        try! NotProton.remove()
        print(String(data:try! JSONSerialization.data(withJSONObject:["notes":Uninstaller.run()]),encoding:.utf8)!);return
    }
    let keep=a.count>2 && a[2]=="keep"
    print(String(data:try! JSONSerialization.data(withJSONObject:["notes":Uninstaller.run(removePrefixes:!keep)]),encoding:.utf8)!)
}}
'''
SOURCES = ['Uninstaller', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Brand', 'GameTools', 'InstallLocation', 'ControllerSupport', 'Surround']


class UninstallCleanup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='uninstall-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.tool = folder / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{n}.swift') for n in SOURCES],
                        str(folder / 'harness.swift'), '-o', str(cls.tool)], check=True, capture_output=True)

    def test_removes_what_is_ours_and_keeps_everything_else(self):
        with tempfile.TemporaryDirectory(prefix='uninstall-home-') as temp:
            home = Path(temp)
            sup = home / 'Library/Application Support'
            steam = sup / 'Steam'
            def mk(path, text='x'):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            # a game tool of ours, and someone else's tool without the marker
            mk(steam / 'compatibilitytools.d/notproton-steamdriver-finals/.steamdriver-tool.json', '{}')
            mk(steam / 'compatibilitytools.d/notproton-steamdriver-finals/runtime/bin/wine')
            mk(steam / 'compatibilitytools.d/notproton-someoneelse/run')
            # a game our tool ran: its files go, the prefix stays
            game = steam / 'steamapps/compatdata/2073850'
            for name in ('steamdriver-run.log', 'steamdriver-wine.log', 'steamdriver-wine.log.old',
                         'steamdriver-gpu-warning-seen', 'perf/20261124/wine.1', 'captures/a.gputrace',
                         'pfx/system.reg'):
                mk(game / name)
            mk(game / 'shortcuts-taken/999999', '60\n')   # a dead process; 60 = Ctrl+Space, already on
            # a game our tool never ran: its perf folder is not ours
            mk(steam / 'steamapps/compatdata/730/perf/x')
            mk(steam / 'steamapps/compatdata/730/pfx/system.reg')   # another tool's prefix
            # NotProton runner copies: ours (built with our variants) and a plain one
            mk(sup / 'notproton/runners/crossover-26.3+macvolvo-26.3@abc/bin/wine')
            mk(sup / 'notproton/runners/crossover-26.3@def/bin/wine')
            vcfg = steam / 'steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg'
            mk(vcfg, '"config"\n{\n\t"DefaultRenderSystemOption"\t\t"-dx11"\n\t"setting.fullscreen"\t\t"0"\n}\n')
            mk(vcfg.with_name('boot.vcfg.macvolvo-backup'), '"config"\n{\n\t"DefaultRenderSystemOption"\t\t"-vulkan"\n\t"setting.fullscreen"\t\t"1"\n}\n')
            # the old name's leftovers, and a folder named MacVolvo that is not ours
            mk(sup / 'MacVolvo NotProton/hid-status.json')
            mk(sup / 'notproton/variants/macvolvo.conf', '# Written by MacVolvo.app, which also removes it.\n')
            mk(sup / 'notproton/plugins/macvolvo.conf', '# someone else\n')
            mk(sup / 'MacVolvo/.macvolvo-owner')
            # More controller support's login item, and someone else's
            mk(home / 'Library/LaunchAgents/app.steamdriver.controllers.plist')
            mk(home / 'Library/LaunchAgents/com.example.other.plist')
            out = subprocess.run([str(self.tool), str(home)], check=True, capture_output=True, text=True).stdout
            notes = json.loads(out)['notes']

            gone = [steam / 'compatibilitytools.d/notproton-steamdriver-finals', game / 'steamdriver-run.log',
                    game / 'steamdriver-wine.log', game / 'steamdriver-wine.log.old', game / 'steamdriver-gpu-warning-seen',
                    game / 'perf', game / 'captures', game / 'shortcuts-taken', game / 'pfx', game,
                    sup / 'notproton/runners/crossover-26.3+macvolvo-26.3@abc',
                    steam / 'steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg.macvolvo-backup',
                    sup / 'MacVolvo NotProton', sup / 'notproton/variants/macvolvo.conf', sup / 'MacVolvo',
                    home / 'Library/LaunchAgents/app.steamdriver.controllers.plist']
            kept = [steam / 'compatibilitytools.d/notproton-someoneelse/run',
                    steam / 'steamapps/compatdata/730/perf/x', steam / 'steamapps/compatdata/730/pfx/system.reg', sup / 'notproton/runners/crossover-26.3@def/bin/wine',
                    steam / 'steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg', sup / 'notproton/plugins/macvolvo.conf',
                    home / 'Library/LaunchAgents/com.example.other.plist']
            for path in gone:
                self.assertFalse(path.exists(), f'{path} should be gone; notes: {notes}')
            for path in kept:
                self.assertTrue(path.exists(), f'{path} should stay')
            self.assertTrue(any('handed macOS shortcuts [60]' in n for n in notes), notes)
            # Deadlock: the renderer line goes back to Vulkan; a setting changed later stays.
            self.assertIn('"-vulkan"', vcfg.read_text())
            self.assertIn('"setting.fullscreen"\t\t"0"', vcfg.read_text())
            # A second run finds nothing left.
            again = json.loads(subprocess.run([str(self.tool), str(home)], check=True, capture_output=True, text=True).stdout)['notes']
            self.assertEqual(again, [])


    def test_keeping_prefixes_keeps_them(self):
        with tempfile.TemporaryDirectory(prefix='uninstall-home-') as temp:
            home = Path(temp)
            game = home / 'Library/Application Support/Steam/steamapps/compatdata/2073850'
            (game / 'pfx').mkdir(parents=True)
            (game / 'pfx/system.reg').write_text('x')
            (game / 'steamdriver-run.log').write_text('x')
            subprocess.run([str(self.tool), str(home), 'keep'], check=True, capture_output=True)
            self.assertTrue((game / 'pfx/system.reg').exists())
            self.assertFalse((game / 'steamdriver-run.log').exists())


    def test_set_up_everything_then_uninstall_leaves_the_home_as_steam_left_it(self):
        """Round trip: snapshot a home as Steam and NotProton left it, set everything up with
        the app's own code, add what a game launch leaves, uninstall, and compare. Anything of
        ours still there, or anything of Steam's or NotProton's gone or changed, fails."""
        with tempfile.TemporaryDirectory(prefix='uninstall-roundtrip-') as temp:
            root = Path(temp)
            home, res, bundle = root / 'home', root / 'Resources', root / 'tool'
            sup = home / 'Library/Application Support'
            steam = sup / 'Steam'
            def mk(path, data='x'):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data if isinstance(data, bytes) else data.encode())
            # Steam and NotProton, before macSteam: a CrossOver runner, a game's Steam folder,
            # Deadlock saved on Vulkan, another app's tool.
            mk(sup / 'notproton/tools', 'notproton-26.3\t26.3.0.39832\trosetta\tCrossOver 26.3\n')
            mk(sup / 'notproton/runners/crossover-26.3.0.39832@aaa/bin/wine')
            mk(steam / 'steamapps/appmanifest_2073850.acf', '"AppState"\n{\n\t"name"\t\t"THE FINALS"\n}\n')
            (steam / 'steamapps/compatdata/2073850').mkdir(parents=True)
            mk(steam / 'steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg',
               '"config"\n{\n\t"DefaultRenderSystemOption"\t\t"-vulkan"\n}\n')
            mk(steam / 'compatibilitytools.d/notproton-someoneelse/run')
            def snapshot():
                out = {}
                for p in sorted(home.rglob('*')):
                    out[str(p.relative_to(home))] = 'dir' if p.is_dir() else p.read_bytes()
                return out
            before = snapshot()
            # THE FINALS' own tool from an earlier version: adding the global SteamDriver tool replaces it.
            legacy = steam / 'compatibilitytools.d/notproton-steamdriver-finals'
            mk(legacy / '.steamdriver-tool.json', '{"runtime":"macfinals-143"}')
            mk(legacy / 'run')

            # What the app carries: a driver, DXMT, renderer profiles, a game tool bundle.
            mk(res / 'NotProton/26.3.0.39832/winemac.so', b'driver')
            mk(res / 'NotProton/26.3.0.39832/dxmt/dxmt/x86_64-unix/winemetal.so', b'dxmt')
            for name in ('cs2', 'deadlock'):
                mk(res / f'NotProton/profiles/{name}.conf', f'[{name}.exe]\nd3d11.x = 1\n')
            mk(res / 'NotProton/profiles/dx11.conf', 'd3d11.x = 2\n')
            for name in ('run', 'compatibilitytool.vdf', 'toolmanifest.vdf'):
                mk(bundle / name, f'{name}\n')
            mk(root / 'rt/bin/wine', b'wine')
            subprocess.run(['/usr/bin/tar', '-czf', str(bundle / 'runtime.tar.gz'), '-C', str(root / 'rt'), '.'], check=True)
            subprocess.run([str(self.tool), str(home), 'setup', str(res), str(bundle)], check=True, capture_output=True)
            self.assertFalse(legacy.exists(), 'adding SteamDriver left the old THE FINALS tool')

            # What a game launch through the tool leaves (environments/finals/compat-tool/run).
            game = steam / 'steamapps/compatdata/2073850'
            for name in ('pfx/system.reg', 'pfx/drive_c/users/x', 'steamdriver-run.log', 'steamdriver-wine.log',
                         'steamdriver-wine.log.old', 'steamdriver-gpu-warning-seen', 'perf/20261124/wine.1',
                         'captures/a.gputrace'):
                mk(game / name)
            mk(game / 'shortcuts-taken/999999', '60\n')
            mk(sup / 'SteamDriver/games/2073850', 'THE FINALS\nnotproton-steamdriver\n2026-11-24\n')
            # NotProton's runner copy built with macSteam's variants.
            mk(sup / 'notproton/runners/crossover-26.3.0.39832+macvolvo-26.3.0.39832@bbb/bin/wine')
            during = snapshot()
            self.assertGreater(len(set(during) - set(before)), 15, 'the setup made too little to test')

            notes = subprocess.run([str(self.tool), str(home), 'uninstall-all'], check=True, capture_output=True, text=True).stdout
            after = snapshot()
            # Steam's own folder for the game goes once it is empty; Steam makes it again.
            allowed_gone = {'Library/Application Support/Steam/steamapps/compatdata/2073850'}
            left = sorted(set(after) - set(before))
            gone = sorted(set(before) - set(after) - allowed_gone)
            changed = sorted(p for p in set(before) & set(after) if before[p] != after[p])
            self.assertEqual(left, [], f'left behind after Uninstall: {left}\n{notes}')
            self.assertEqual(gone, [], f'removed but not ours: {gone}')
            self.assertEqual(changed, [], f'changed and not put back: {changed}')


if __name__ == '__main__':
    unittest.main()
