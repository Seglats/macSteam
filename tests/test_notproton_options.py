"""SteamDriver's options for NotProton (app/Sources/NotProton.swift) in a throwaway home folder.

Compiles the real NotProton.swift with a small driver and runs it; never touches Steam, NotProton,
Wine or the player's own Library."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APPLE_REDIST = ROOT / 'gptk/gptk/redist/lib'
HARNESS = r'''
import Foundation
@main struct Harness { static func main() {
    let a=CommandLine.arguments; NotProton.home=URL(fileURLWithPath:a[1]); let res=URL(fileURLWithPath:a[2])
    var out:[String:Any]=[:]
    do {
        switch a[3] {
        case "add": out["change"]="\(try NotProton.add(resources:res))"
        case "restore": out["change"]="\(try NotProton.add(resources:res,restoreRows:true))"
        case "remove": try NotProton.remove()
        case "cs2": try Profiles.addGame(appID:"730",name:"Counter-Strike 2",kind:.dx11,resources:res);try NotProton.add(resources:res)
        case "deadlock": try Profiles.addGame(appID:"1422450",name:"Deadlock",kind:.dx11,resources:res)
        case "dx11": try Profiles.addGlobal(.dx11,resources:res);out["change"]="\(try NotProton.writeRows(resources:res))"
        case "dx12": try Profiles.addGlobal(.dx12,resources:res);out["change"]="\(try NotProton.writeRows(resources:res))"
        case "game": try Profiles.addGame(appID:a[4],name:a[5],kind:a[6]=="dx12" ? .dx12 : .dx11,resources:res);out["change"]="\(try NotProton.writeRows(resources:res))"
        case "unprofile": try Profiles.remove(a[4]);out["change"]="\(try NotProton.writeRows(resources:res))"
        case "profiles": out["profiles"]=Profiles.list().map { "\($0.id) \($0.kind.rawValue) \($0.name)" }
        case "refresh": let c=NotProton.refresh(resources:res);out["changed"]=c != .none;out["change"]="\(c)"
        case "migrate": Migration.run(defaults:false);out["notes"]=Migration.notes
        case "gptk": out["gptk"]="\(NotProton.gptkStatus())"
        case "detected": out["detected"]=a[4...].map { "\($0)=\(Profiles.detected($0)?.rawValue ?? "none")/\(Profiles.suggestedKind($0).rawValue)" }
        case "settings": out["settings"]=Profiles.settings(a[4]).map { "\($0)" } ?? "none"
        case "edit":
            var s=Profiles.settings(a[4])!
            s.frameLatency=2;s.frameCap=120;s.lateInput=true;s.shaderThreads=6;s.sharpScaling=true;s.upscaling=true
            try Profiles.save(a[4],s);out["settings"]="\(Profiles.settings(a[4])!)"
        case "metal": out["metal"]="\(Profiles.d3dmetalSettings(a[4]))"
        case "metaledit": try Profiles.saveD3DMetal(a[4],Profiles.D3DMetalSettings(upscaling:a[5]=="on",surround:a.count>6 && a[6]=="surround"));out["metal"]="\(Profiles.d3dmetalSettings(a[4]))"
        case "surround":
            var s=Profiles.settings(a[4])!;s.surround=a[5]=="on"
            try Profiles.save(a[4],s);out["settings"]="\(Profiles.settings(a[4])!)";out["summary"]=Profiles.summary(Profiles.settings(a[4])!)
        case "experimental": try Surround.setEnabled(a[4]=="on");out["enabled"]=Surround.enabled
        case "reset": try Profiles.resetSettings(a[4],resources:res)
        case "switch": try Profiles.setKind(a[4],a[5]=="dx12" ? .dx12 : .dx11,resources:res);out["change"]="\(try NotProton.writeRows(resources:res))"
        default: break
        }
    } catch { out["error"]=error.localizedDescription }
    out["status"]=String(describing:NotProton.status(res)).components(separatedBy:"(").first ?? ""
    out["current"]=NotProton.isCurrent(resources:res)
    if NotProton.rowsRemoved { out["rowsRemoved"]=true }
    print(String(data:try! JSONSerialization.data(withJSONObject:out),encoding:.utf8)!)
}}
'''


class NotProtonOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='macvolvo-notproton-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.tool = folder / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', str(ROOT / 'app/Sources/NotProton.swift'),
                        str(ROOT / 'app/Sources/NotProtonExtras.swift'), str(ROOT / 'app/Sources/Profiles.swift'), str(ROOT / 'app/Sources/KeyOverrides.swift'),
                        str(ROOT / 'app/Sources/Migration.swift'), str(ROOT / 'app/Sources/Brand.swift'),
                        str(ROOT / 'app/Sources/InstallLocation.swift'), str(ROOT / 'app/Sources/Surround.swift'),
                        str(folder / 'harness.swift'), '-o', str(cls.tool)], check=True, capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='macvolvo-notproton-')
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.home = root / 'home'
        self.lib = self.home / 'Library/Application Support'
        (self.lib / 'notproton').mkdir(parents=True)
        (self.lib / 'notproton/tools').write_text('notproton-26.3\t26.3.0.39832\trosetta\tCrossOver 26.3\n'
                                                  'notproton-27.0-fex\t27.0.0.41073-fex\tfex\tCrossOver Preview\n')
        self.resources = root / 'Resources'
        (self.resources / 'NotProton/26.3.0.39832').mkdir(parents=True)
        (self.resources / 'NotProton/26.3.0.39832/winemac.so').write_bytes(b'driver v1')
        dxmt = self.resources / 'NotProton/26.3.0.39832/dxmt/dxmt/x86_64-unix'
        dxmt.mkdir(parents=True)
        (dxmt / 'winemetal.so').write_bytes(b'dxmt v1')
        (self.resources / 'NotProton/profiles').mkdir()
        for name in ('cs2', 'deadlock'):
            (self.resources / f'NotProton/profiles/{name}.conf').write_text(f'[{name}.exe]\nd3d11.x = 1\n')
        (self.resources / 'NotProton/profiles/dx11.conf').write_text('d3d11.x = 2\n')
        self.variants = self.lib / 'notproton/variants/steamdriver.conf'
        self.plugins = self.lib / 'notproton/plugins/steamdriver.conf'

    def run_tool(self, action, *args):
        result = subprocess.run([str(self.tool), str(self.home), str(self.resources), action, *args],
                                capture_output=True, text=True, check=True)
        out = json.loads(result.stdout)
        if action in ('status', 'remove', 'cs2', 'deadlock', 'migrate', 'gptk', 'settings', 'edit', 'reset', 'detected'):
            out.pop('change', None)
        return out

    def test_adds_a_variant_and_rows_for_the_rosetta_build_only(self):
        self.assertEqual(self.run_tool('status'), {'status': 'available', 'current': False})
        self.assertEqual(self.run_tool('add'), {'status': 'added', 'current': True, 'change': 'restart'})
        variants, plugins = self.variants.read_text(), self.plugins.read_text()
        self.assertIn('[macvolvo-26.3.0.39832]', variants)
        self.assertIn('build = 26.3.0.39832', variants)
        self.assertIn('dst = lib/wine/x86_64-unix', variants)
        self.assertNotIn('41073', variants + plugins)
        self.assertEqual((self.lib / 'SteamDriver/26.3.0.39832/winemac.so').read_bytes(), b'driver v1')
        for row in ('[NOTPROTON_MACVOLVO_MOUSE]', '[NOTPROTON_MACVOLVO_HIGH_PRECISION]', '[NOTPROTON_MACVOLVO_CLASSIC]',
                    '[NOTPROTON_ENV_WINEMAC_MOUSELOOK_RAW_INTERVAL_US]'):
            self.assertIn(row, plugins)
        self.assertIn('env = WINEMAC_RAW_REQUEST=1', plugins)
        self.assertEqual(plugins.count('tools = notproton-26.3\n'), 4)

    def test_adds_macvolvo_dxmt_with_the_driver_and_no_d3dmetal_until_imported(self):
        self.run_tool('add')
        variants = self.variants.read_text()
        self.assertIn('[macvolvo-dxmt-26.3.0.39832]\nlabel = SteamDriver DXMT\nvariant = SteamDriver renderer', variants)
        self.assertIn('variant = SteamDriver driver', variants)
        self.assertNotIn('d3dmetal', variants)
        self.assertEqual((self.lib / 'SteamDriver/26.3.0.39832/dxmt/dxmt/x86_64-unix/winemetal.so').read_bytes(), b'dxmt v1')

    def test_a_profile_adds_its_row_pointing_dxmt_at_its_file(self):
        self.run_tool('add')
        self.assertNotIn('PROFILE', self.plugins.read_text())
        self.assertEqual(self.run_tool('cs2'), {'status': 'added', 'current': True})
        plugins = self.plugins.read_text()
        self.assertIn('[NOTPROTON_MACVOLVO_PROFILE_CS2]', plugins)
        self.assertIn('env = DXMT_CONFIG_FILE=Z:' + str(self.lib / 'SteamDriver/profiles/app-730/dxmt.conf').replace('/', '\\'), plugins)
        self.assertIn('apps = 730\n', plugins)
        self.assertNotIn('DEADLOCK', plugins)
        self.assertEqual((self.lib / 'SteamDriver/profiles/app-730/dxmt.conf').read_text(), '[cs2.exe]\nd3d11.x = 1\n')

    def test_global_dx11_comes_before_game_rows_and_changes_only_rows(self):
        self.run_tool('add')
        variants = self.variants.read_text()
        self.assertEqual(self.run_tool('game', '2483190', 'Some Game', 'dx11')['change'], 'rows')
        self.assertEqual(self.run_tool('dx11')['change'], 'rows')
        plugins = self.plugins.read_text()
        self.assertEqual(self.variants.read_text(), variants)
        self.assertLess(plugins.index('[NOTPROTON_MACVOLVO_PROFILE_DX11]'), plugins.index('[NOTPROTON_MACVOLVO_PROFILE_APP_2483190]'))
        self.assertIn('label = SteamDriver profile: Some Game', plugins)
        dx11 = plugins[plugins.index('[NOTPROTON_MACVOLVO_PROFILE_DX11]'):plugins.index('[NOTPROTON_MACVOLVO_PROFILE_APP_2483190]')]
        self.assertNotIn('apps =', dx11)
        self.assertEqual((self.lib / 'SteamDriver/profiles/app-2483190/dxmt.conf').read_text(), 'd3d11.x = 2\n')
        self.assertTrue(self.run_tool('status')['current'])
        self.assertEqual(self.run_tool('unprofile', 'app-2483190')['change'], 'rows')
        self.assertNotIn('2483190', self.plugins.read_text())

    def test_dx12_profiles_carry_no_row(self):
        self.run_tool('add')
        self.assertEqual(self.run_tool('dx12')['change'], 'none')
        self.assertEqual(self.run_tool('game', '2073850', 'THE FINALS', 'dx12')['change'], 'none')
        self.assertNotIn('PROFILE', self.plugins.read_text())
        self.assertFalse((self.lib / 'SteamDriver/profiles/global-dx12/dxmt.conf').exists())
        self.assertEqual(self.run_tool('profiles')['profiles'], ['global-dx12 dx12 Global DX12', 'app-2073850 dx12 THE FINALS'])

    def test_key_combos_kept_in_profiles_go_back_to_global_and_game_lists(self):
        sd = self.lib / 'SteamDriver'
        for name, combos in (('global-dx11', 'ctrl+space\n'), ('global-dx12', 'cmd+space\nctrl+space\n'), ('app-2073850', 'cmd+tab\n')):
            (sd / 'profiles' / name).mkdir(parents=True)
            (sd / 'profiles' / name / 'keys.txt').write_text(combos)
        self.run_tool('migrate')
        self.assertEqual(sorted((sd / 'key-overrides/global.txt').read_text().splitlines()[1:]), ['cmd+space', 'ctrl+space'])
        self.assertEqual((sd / 'key-overrides/app-2073850.txt').read_text().splitlines()[1:], ['cmd+tab'])
        self.assertEqual(list(sd.glob('profiles/*/keys.txt')), [])

    def test_profile_settings_are_read_and_saved_in_place(self):
        (self.resources / 'NotProton/profiles/cs2.conf').write_text(
            '# CS2\n[cs2.exe]\nd3d11.releaseShaderIR = True\n# one frame\ndxgi.maxFrameLatency = 1\n'
            'd3d11.shaderCompilerThreads = 4\ndxgi.presentPacing = True\ndxgi.presentPacingMode = 3\n'
            'dxgi.presentPacingTargetUs = 1000\nd3d11.preferredMaxFrameRate = 0\ndxgi.nativeFullscreen = True\n')
        self.run_tool('cs2')
        self.assertEqual(self.run_tool('settings', 'app-730')['settings'],
                         'Settings(frameLatency: 1, lateInput: true, frameCap: 0, shaderThreads: 4, shaderNormalPriority: false, sharpScaling: false, releaseShaderIR: true, upscaling: false, surround: false)')
        plugins = self.plugins.read_text()
        self.run_tool('edit', 'app-730')
        text = (self.lib / 'SteamDriver/profiles/app-730/dxmt.conf').read_text()
        self.assertTrue(text.startswith('# CS2\n[cs2.exe]\nd3d11.releaseShaderIR = True\n# one frame\ndxgi.maxFrameLatency = 2\n'))
        self.assertIn('d3d11.shaderCompilerThreads = 6\n', text)
        self.assertIn('d3d11.preferredMaxFrameRate = 120\n', text)
        self.assertIn('dxgi.nativeFullscreen = True\n', text)
        self.assertIn('dxgi.sharpPresentation = True\n', text)
        # Upscaling is the SteamDriver tool's key in the same file (compat-tool/run reads it).
        self.assertIn('steamdriver.upscaling = True\n', text)
        self.assertEqual(text.count('dxgi.maxFrameLatency'), 1)
        # Settings change the file DXMT reads, not Steam's rows.
        self.assertEqual(self.plugins.read_text(), plugins)
        self.run_tool('reset', 'app-730')
        self.assertIn('frameLatency: 1', self.run_tool('settings', 'app-730')['settings'])
        self.assertEqual(self.run_tool('settings', 'global-dx12')['settings'], 'none')

    def test_dx12_profiles_hold_d3dmetal_upscaling(self):
        self.run_tool('dx12')
        conf = self.lib / 'SteamDriver/profiles/global-dx12/d3dmetal.conf'
        # A new DX12 profile starts with upscaling off, in the file the SteamDriver tool reads.
        self.assertIn('upscaling = False\n', conf.read_text())
        self.assertEqual(self.run_tool('metal', 'global-dx12')['metal'], 'D3DMetalSettings(upscaling: false, surround: false)')
        self.assertEqual(self.run_tool('metaledit', 'global-dx12', 'on')['metal'], 'D3DMetalSettings(upscaling: true, surround: false)')
        self.assertIn('upscaling = True\n', conf.read_text())
        # A game profile switched DX12 -> DX11 -> DX12 gets its D3DMetal setting back.
        self.run_tool('game', '1808500', 'ARC Raiders', 'dx12')
        self.run_tool('metaledit', 'app-1808500', 'on')
        game = self.lib / 'SteamDriver/profiles/app-1808500'
        self.run_tool('switch', 'app-1808500', 'dx11')
        self.assertFalse((game / 'd3dmetal.conf').exists())
        self.assertTrue((game / 'dxmt.conf').exists())
        self.run_tool('switch', 'app-1808500', 'dx12')
        self.assertFalse((game / 'dxmt.conf').exists())
        self.assertEqual(self.run_tool('metal', 'app-1808500')['metal'], 'D3DMetalSettings(upscaling: true, surround: false)')

    def test_profiles_hold_headphone_surround(self):
        # DX12: "surround" in d3dmetal.conf, beside upscaling.
        self.run_tool('dx12')
        conf = self.lib / 'SteamDriver/profiles/global-dx12/d3dmetal.conf'
        self.assertIn('surround = False\n', conf.read_text())
        self.assertEqual(self.run_tool('metaledit', 'global-dx12', 'off', 'surround')['metal'],
                         'D3DMetalSettings(upscaling: false, surround: true)')
        self.assertIn('surround = True\n', conf.read_text())
        # DX11: the SteamDriver tool's own key in dxmt.conf, written only once it is used.
        self.run_tool('add')
        self.run_tool('game', '824270', "KovaaK's", 'dx11')
        dxmt = self.lib / 'SteamDriver/profiles/app-824270/dxmt.conf'
        self.run_tool('edit', 'app-824270')
        self.assertNotIn('steamdriver.surround', dxmt.read_text())
        out = self.run_tool('surround', 'app-824270', 'on')
        self.assertIn('surround: true', out['settings'])
        self.assertIn('steamdriver.surround = True\n', dxmt.read_text())
        # The summary names it only while Experimental > Headphone surround is on.
        self.assertNotIn('headphone surround', out['summary'])
        self.assertTrue(self.run_tool('experimental', 'on')['enabled'])
        self.assertTrue((self.lib / 'SteamDriver/headphone-surround').exists())
        self.assertIn('headphone surround', self.run_tool('surround', 'app-824270', 'on')['summary'])
        # Off again keeps the profile's setting; only the switch file goes.
        self.assertFalse(self.run_tool('experimental', 'off')['enabled'])
        self.assertIn('steamdriver.surround = True\n', dxmt.read_text())
        self.run_tool('surround', 'app-824270', 'off')
        self.assertIn('steamdriver.surround = False\n', dxmt.read_text())

    def test_a_game_profile_switches_direct3d_and_keeps_dx11_settings(self):
        self.run_tool('add')
        self.run_tool('game', '824270', "KovaaK's", 'dx11')
        self.assertIn('PROFILE_APP_824270', self.plugins.read_text())
        self.run_tool('edit', 'app-824270')
        edited = self.run_tool('settings', 'app-824270')['settings']
        self.run_tool('switch', 'app-824270', 'dx12')
        self.assertIn("app-824270 dx12 KovaaK's", self.run_tool('profiles')['profiles'])
        self.assertEqual(self.run_tool('settings', 'app-824270')['settings'], 'none')   # DX12 has no renderer settings
        self.assertNotIn('PROFILE_APP_824270', self.plugins.read_text())                 # no DX11 row for it now
        self.run_tool('switch', 'app-824270', 'dx11')
        self.assertIn("app-824270 dx11 KovaaK's", self.run_tool('profiles')['profiles'])
        self.assertEqual(self.run_tool('settings', 'app-824270')['settings'], edited)    # the DX11 settings came back
        self.assertIn('PROFILE_APP_824270', self.plugins.read_text())

    def test_the_direct3d_the_tool_detected_is_read_per_game(self):
        games = self.lib / 'SteamDriver/games'
        games.mkdir(parents=True)
        (games / '2073850').write_text('THE FINALS\nnotproton-steamdriver\n2026-11-24 15:00:00\ndirect3d=dx12\n')
        (games / '504230').write_text('Celeste\nnotproton-steamdriver\n2026-11-24 15:00:00\ndirect3d=dx11\n')
        (games / '1942280').write_text('Brotato\nnotproton-steamdriver\n2026-11-24 15:00:00\n')
        self.assertEqual(self.run_tool('detected', '2073850', '504230', '1942280', '1422450')['detected'],
                         ['2073850=dx12/dx12', '504230=dx11/dx11', '1942280=none/dx11', '1422450=none/dx11'])

    def test_refuses_a_bad_app_id(self):
        self.run_tool('add')
        self.assertIn("isn’t a Steam app id", self.run_tool('game', '0730', 'X', 'dx11')['error'])
        self.assertFalse((self.lib / 'SteamDriver/profiles/app-0730').exists())

    def test_rows_removed_in_notprotons_plugin_page_stay_removed(self):
        self.run_tool('add')
        self.plugins.unlink()
        self.assertEqual(self.run_tool('status'), {'status': 'added', 'current': True, 'rowsRemoved': True})
        self.assertEqual(self.run_tool('refresh')['changed'], False)
        self.assertEqual(self.run_tool('dx11')['change'], 'none')
        (self.resources / 'NotProton/26.3.0.39832/winemac.so').write_bytes(b'driver v2')
        self.assertEqual(self.run_tool('refresh')['change'], 'files')
        self.assertFalse(self.plugins.exists())
        self.assertEqual(self.run_tool('restore')['change'], 'rows')
        self.assertIn('[NOTPROTON_MACVOLVO_PROFILE_DX11]', self.plugins.read_text())
        self.assertNotIn('rowsRemoved', self.run_tool('status'))

    def test_the_deadlock_profile_keeps_deadlock_on_directx_11_with_a_backup(self):
        cfg = self.lib / 'Steam/steamapps/common/Deadlock/game/citadel/cfg/boot.vcfg'
        cfg.parent.mkdir(parents=True)
        original = '"config"\n{\n\t"DefaultRenderSystemOption"\t\t"-vulkan"\n\t"Other"\t"1"\n}\n'
        cfg.write_text(original)
        self.run_tool('deadlock')
        self.assertEqual(cfg.read_text(), original.replace('-vulkan', '-dx11'))
        self.assertEqual(cfg.with_name('boot.vcfg.macvolvo-backup').read_text(), original)

    def test_never_writes_a_tool_or_touches_notprotons_files(self):
        self.run_tool('add')
        tools = self.lib / 'Steam/compatibilitytools.d'
        self.assertFalse(tools.exists() and any(tools.iterdir()))
        self.assertEqual((self.lib / 'notproton/tools').read_text().count('\n'), 2)

    def test_a_newer_driver_makes_it_out_of_date_until_updated(self):
        self.run_tool('add')
        (self.resources / 'NotProton/26.3.0.39832/winemac.so').write_bytes(b'driver v2')
        self.assertEqual(self.run_tool('status'), {'status': 'added', 'current': False})
        self.assertEqual(self.run_tool('add'), {'status': 'added', 'current': True, 'change': 'files'})
        (self.resources / 'NotProton/26.3.0.39832/dxmt/dxmt/x86_64-unix/winemetal.so').write_bytes(b'dxmt v2')
        self.assertEqual(self.run_tool('status'), {'status': 'added', 'current': False})
        self.assertEqual(self.run_tool('add'), {'status': 'added', 'current': True, 'change': 'files'})

    def test_the_macvolvo_rename_moves_everything_over_once_and_keeps_the_ids(self):
        old = self.lib / 'MacVolvo NotProton'
        (old / '26.3.0.39832').mkdir(parents=True)
        (old / '26.3.0.39832/winemac.so').write_bytes(b'driver v1')
        (old / 'key-overrides/games').mkdir(parents=True)
        (old / 'key-overrides/global.txt').write_text('ctrl+space\n')
        (old / 'key-overrides/app-2073850.txt').write_text('cmd+space\n')
        (old / 'key-overrides/games/2073850').write_text('THE FINALS\nnotproton-steamdriver-finals\n2026-11-24\n')
        (old / 'profiles').mkdir()
        (old / 'profiles/cs2.conf').write_text('[cs2.exe]\nd3d11.x = 1\n')
        data = self.lib / 'MacVolvo'
        data.mkdir()
        (data / '.macvolvo-owner').write_text('x')
        for kind in ('variants', 'plugins'):
            (self.lib / f'notproton/{kind}').mkdir(parents=True, exist_ok=True)
            (self.lib / f'notproton/{kind}/macvolvo.conf').write_text(
                '# Written by MacVolvo.app, which also removes it.\n\n[macvolvo-26.3.0.39832]\nlabel = MacVolvo driver\n')
        result = self.run_tool('migrate')
        self.assertEqual(len(result['notes']), 6, result['notes'])
        self.assertFalse(old.exists())
        sd = self.lib / 'SteamDriver'
        self.assertEqual((sd / '26.3.0.39832/winemac.so').read_bytes(), b'driver v1')
        # Key combo lists stay Global and per game; the CS2 renderer profile becomes a game profile.
        self.assertEqual((sd / 'key-overrides/global.txt').read_text(), 'ctrl+space\n')
        self.assertEqual((sd / 'key-overrides/app-2073850.txt').read_text(), 'cmd+space\n')
        self.assertEqual((sd / 'profiles/app-730/dxmt.conf').read_text(), '[cs2.exe]\nd3d11.x = 1\n')
        self.assertTrue((sd / 'games/2073850').exists())
        self.assertFalse((sd / 'key-overrides/games').exists())
        self.assertFalse((sd / 'profiles/cs2.conf').exists())
        self.assertEqual(self.run_tool('profiles')['profiles'], ['app-730 dx11 Counter-Strike 2'])
        self.assertFalse(data.exists())
        self.assertTrue((self.lib / 'SteamDriver Steam/.macvolvo-owner').exists())
        self.assertFalse((self.lib / 'notproton/variants/macvolvo.conf').exists())
        self.assertFalse((self.lib / 'notproton/plugins/macvolvo.conf').exists())
        # The moved files still count as SteamDriver's; a refresh rewrites them with the new
        # names and the same ids, so games keep what was chosen for them in Steam.
        self.assertTrue(self.run_tool('refresh')['changed'])
        variants, plugins = self.variants.read_text(), self.plugins.read_text()
        self.assertTrue(variants.startswith('# Written by macSteam.app'))
        self.assertIn('[macvolvo-26.3.0.39832]\nlabel = SteamDriver driver\nvariant = SteamDriver driver', variants)
        self.assertIn('[NOTPROTON_MACVOLVO_MOUSE]\nlabel = SteamDriver mouse', plugins)
        self.assertIn('Application Support/SteamDriver/hid-status.json', plugins)
        self.assertIn('[NOTPROTON_MACVOLVO_PROFILE_CS2]', plugins)
        self.assertNotIn('MacVolvo', variants + plugins)
        # Running again changes nothing.
        self.assertEqual(self.run_tool('migrate')['notes'], [])

    def test_refuses_before_writing_when_a_file_is_not_macvolvos(self):
        self.plugins.parent.mkdir(parents=True)
        self.plugins.write_text('[NOTPROTON_MINE]\nlabel = Mine\n')
        result = self.run_tool('add')
        self.assertIn("wasn't written by macSteam", result['error'])
        self.assertEqual(self.plugins.read_text(), '[NOTPROTON_MINE]\nlabel = Mine\n')
        self.assertFalse(self.variants.exists())
        self.assertFalse((self.lib / 'SteamDriver').exists())

    def test_remove_takes_back_only_macvolvos_files_and_the_earlier_tool(self):
        earlier = self.lib / 'Steam/compatibilitytools.d/notproton-macvolvo-26.3'
        earlier.mkdir(parents=True)
        (earlier / '.macvolvo-owner').write_text('x')
        notprotons = self.lib / 'Steam/compatibilitytools.d/notproton-26.3'
        notprotons.mkdir()
        (notprotons / 'run').write_text('#!/bin/sh\n')
        self.run_tool('add')
        self.assertFalse(earlier.exists())
        self.assertEqual(self.run_tool('remove'), {'status': 'available', 'current': False})
        self.assertFalse(self.variants.exists() or self.plugins.exists())
        self.assertFalse((self.lib / 'SteamDriver').exists())
        self.assertTrue((notprotons / 'run').exists())

    def test_a_newer_driver_is_updated_in_place_and_says_so_once(self):
        self.run_tool('add')
        self.assertEqual(self.run_tool('refresh'), {'status': 'added', 'current': True, 'changed': False, 'change': 'none'})
        (self.resources / 'NotProton/26.3.0.39832/winemac.so').write_bytes(b'driver v2')
        # A new driver needs no Steam restart: NotProton rebuilds its runner copy at the next launch.
        self.assertEqual(self.run_tool('refresh'), {'status': 'added', 'current': True, 'changed': True, 'change': 'files'})
        self.assertEqual((self.lib / 'SteamDriver/26.3.0.39832/winemac.so').read_bytes(), b'driver v2')
        self.assertEqual(self.run_tool('refresh')['changed'], False)

    def test_refresh_never_adds_what_was_not_added(self):
        self.assertEqual(self.run_tool('refresh'), {'status': 'available', 'current': False, 'changed': False, 'change': 'none'})

    def test_the_gptk_folder_is_checked_before_anything_uses_it(self):
        gptk = self.lib / 'SteamDriver/GPTK'
        self.assertEqual(self.run_tool('gptk')['gptk'], 'empty')
        # A half-copied redist folder is incomplete.
        (gptk / 'redist/lib/external').mkdir(parents=True)
        self.assertIn('missing', self.run_tool('gptk')['gptk'])
        shutil.rmtree(gptk / 'redist')
        (gptk / 'external').mkdir()
        self.assertIn('missing', self.run_tool('gptk')['gptk'])
        self.run_tool('add')
        self.assertNotIn('d3dmetal', self.variants.read_text())

    @unittest.skipUnless((APPLE_REDIST / 'external/libd3dshared.dylib').exists(), 'no Game Porting Toolkit in gptk/gptk')
    def test_apples_own_files_become_the_d3dmetal_variant_and_a_changed_one_is_refused(self):
        gptk = self.lib / 'SteamDriver/GPTK'
        gptk.parent.mkdir(parents=True)
        subprocess.run(['/usr/bin/ditto', str(APPLE_REDIST), str(gptk)], check=True)
        self.assertTrue(self.run_tool('gptk')['gptk'].startswith('ready("4.'))
        self.run_tool('add')
        self.assertIn(f'[macvolvo-d3dmetal]\nlabel = Apple D3DMetal 4.', self.variants.read_text())
        self.assertIn(f'src = {gptk}\ndst = lib64/apple_gptk', self.variants.read_text())
        with open(gptk / 'external/libd3dshared.dylib', 'ab') as f:
            f.write(b'\0')
        self.assertEqual(self.run_tool('gptk')['gptk'], 'notApple("libd3dshared.dylib")')
        self.assertTrue(self.run_tool('refresh')['changed'])
        self.assertNotIn('d3dmetal', self.variants.read_text())

    @unittest.skipUnless((APPLE_REDIST / 'external/libd3dshared.dylib').exists(), 'no Game Porting Toolkit in gptk/gptk')
    def test_the_redist_folder_copied_in_whole_is_found(self):
        gptk = self.lib / 'SteamDriver/GPTK'
        gptk.mkdir(parents=True)
        subprocess.run(['/usr/bin/ditto', str(APPLE_REDIST.parent), str(gptk / 'redist')], check=True)
        self.assertTrue(self.run_tool('gptk')['gptk'].startswith('ready("4.'))
        self.run_tool('add')
        self.assertIn(f'src = {gptk}/redist/lib\ndst = lib64/apple_gptk', self.variants.read_text())

    def test_a_d3dmetal_imported_before_moves_into_the_gptk_folder(self):
        old = self.lib / 'SteamDriver/d3dmetal'
        (old / 'apple_gptk/external').mkdir(parents=True)
        (old / 'apple_gptk/external/libd3dshared.dylib').write_bytes(b'x')
        (old / 'licenses').mkdir()
        (old / 'licenses/License.rtf').write_text('license')
        (old / 'version').write_text('4.0b3\n')
        self.run_tool('migrate')
        gptk = self.lib / 'SteamDriver/GPTK'
        self.assertEqual((gptk / 'external/libd3dshared.dylib').read_bytes(), b'x')
        self.assertEqual((gptk / 'licenses/License.rtf').read_text(), 'license')
        self.assertFalse(old.exists())

    def test_without_notproton_it_says_so(self):
        (self.lib / 'notproton/tools').unlink()
        self.assertEqual(self.run_tool('status'), {'status': 'notInstalled', 'current': False})


if __name__ == '__main__':
    unittest.main()
