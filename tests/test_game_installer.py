"""Install a Windows game (app/Sources/GameInstaller.swift, docs/GOG.md) in a throwaway home.

Compiles the real GameInstaller.swift with a small driver: Steam's binary shortcuts.vdf, the
compatibility tool in config.vdf, the artwork files, names from installer file names. Then the
run script's installer handling: GOG's goggame-*.info and other installers' Start Menu shortcuts.
Never touches Steam, the network or the player's Library."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUN_SCRIPT = ROOT / 'environments/finals/compat-tool/run'
HARNESS = r'''
import Foundation
@main struct Harness { static func main() {
    let a=CommandLine.arguments; NotProton.home=URL(fileURLWithPath:a[1])
    var out:[String:Any]=[:]
    switch a[2] {
    case "add":
        do {
            let art:[String:Data]=["p.jpg":Data("cover".utf8),"_hero.jpg":Data("hero".utf8),"_icon.jpg":Data("icon".utf8)]
            out["id"]=try GameInstaller.addToSteam(name:a[3],exe:URL(fileURLWithPath:a[4]),art:a.count>5 ? [:] : art)
        } catch { out["error"]=error.localizedDescription }
    case "name": out["names"]=a[3...].map { GameInstaller.guessName(URL(fileURLWithPath:$0)) }
    case "files": out["files"]=GameInstaller.files(for:URL(fileURLWithPath:a[3])).map(\.lastPathComponent)
    case "roundtrip":
        let data=try! Data(contentsOf:URL(fileURLWithPath:a[3]))
        out["same"]=BinaryVDF.parse(data).map { BinaryVDF.encode($0)==data } ?? false
    case "list": out["games"]=InstalledGames.list().map { "\($0.name)|\($0.shortcutID.map(String.init) ?? "-")|\($0.inSteam)" }
    case "unsteam": do { try InstalledGames.removeFromSteam(UInt32(a[3])!) } catch { out["error"]=error.localizedDescription }
    case "keep":
        let game=InstalledGames.list().first { $0.name==a[3] }!
        do { try InstalledGames.keepSaves(game) } catch { out["error"]=error.localizedDescription }
    case "place":
        do { try GameInstaller.checkLocation(URL(fileURLWithPath:a[4]));try GameInstaller.placePrefix(id:UInt32(a[3])!,slug:"Game",in:URL(fileURLWithPath:a[4])) }
        catch { out["error"]=error.localizedDescription }
    case "where": out["where"]=InstalledGames.list().map { $0.elsewhere?.path ?? "steam" }
    case "link": out["ids"]=a[3...].map { GameInstaller.appID(fromLink:$0) ?? 0 }
    case "id": out["id"]=GameInstaller.shortcutID(exe:a[3],name:a[4])
    case "follow":
        // The panel's status lines while an install runs, from the run script's log (a[3]).
        let said=NSMutableArray()
        let done=DispatchSemaphore(value:0)
        Task.detached {
            // The installer's name comes in the environment: on the command line, this harness
            // would itself look like the installer's process still running.
            let installer=ProcessInfo.processInfo.environment["FOLLOW_INSTALLER"] ?? ""
            do { try await GameInstaller.follow(log:URL(fileURLWithPath:a[3]),from:0,name:"Alpha",installer:installer) { said.add($0) } }
            catch { out["error"]=error.localizedDescription }
            done.signal()
        }
        done.wait()
        out["said"]=said as? [String] ?? []
    case "delete":
        // Steam closed (a test never touches the Mac's real Steam); the size and saves flags first.
        NotProton.steamRunningForTests=false
        let game=InstalledGames.list().first { $0.name==a[3] }!
        out["size"]=InstalledGames.size(game)
        let done=DispatchSemaphore(value:0)
        Task.detached {
            do { try await InstalledGames.delete(game,keep:a[4]=="keep") { _ in } } catch { out["error"]=error.localizedDescription }
            done.signal()
        }
        done.wait()
        out["saves"]=InstalledGames.hasSaves(game)
    default: break
    }
    print(String(data:try! JSONSerialization.data(withJSONObject:out),encoding:.utf8)!)
}}
'''
CONFIG = '''"InstallConfigStore"
{
\t"Software"
\t{
\t\t"Valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"CompatToolMapping"
\t\t\t\t{
\t\t\t\t\t"346110"
\t\t\t\t\t{
\t\t\t\t\t\t"name"\t\t"notproton-26.3"
\t\t\t\t\t\t"config"\t\t""
\t\t\t\t\t\t"priority"\t\t"250"
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
}
'''


def read_shortcuts(path):
    """A small reader for Steam's binary KeyValues, independent of the Swift one."""
    data, i = path.read_bytes(), 0

    def cstr():
        nonlocal i
        end = data.index(b'\0', i)
        s, i = data[i:end].decode(), end + 1
        return s

    def node():
        nonlocal i
        out = {}
        while True:
            t = data[i]
            i += 1
            if t == 8:
                return out
            name = cstr()
            if t == 0:
                out[name] = node()
            elif t == 1:
                out[name] = cstr()
            elif t == 2:
                out[name] = struct.unpack_from('<I', data, i)[0]
                i += 4
            else:
                raise ValueError(t)
    return node()


class GameInstaller(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='steamdriver-installer-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.tool = folder / 'harness'
        sources = ['GameInstaller', 'InstalledGames', 'Benchmarks', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Migration', 'Brand', 'InstallLocation', 'Surround']
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{s}.swift') for s in sources],
                        str(folder / 'harness.swift'), '-o', str(cls.tool)], check=True, capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='steamdriver-installer-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.steam = self.home / 'Library/Application Support/Steam'
        (self.steam / 'config').mkdir(parents=True)
        (self.steam / 'config/config.vdf').write_text(CONFIG)
        (self.steam / 'config/loginusers.vdf').write_text(
            '"users"\n{\n\t"76561198328213426"\n\t{\n\t\t"AccountName"\t\t"a"\n\t\t"timestamp"\t\t"1795571963"\n\t}\n'
            '\t"76561198813080713"\n\t{\n\t\t"AccountName"\t\t"b"\n\t\t"timestamp"\t\t"1794760537"\n\t}\n}\n')
        for account in ('367947698', '852814985'):
            (self.steam / f'userdata/{account}/config').mkdir(parents=True)
        self.user = self.steam / 'userdata/367947698/config'   # the newest login
        self.user.joinpath('shortcuts.vdf').write_bytes(b'\x00shortcuts\x00\x08\x08')   # what Steam writes with none

    def run_tool(self, *args):
        result = subprocess.run([str(self.tool), str(self.home), *map(str, args)], capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def test_steams_shortcut_id_formula(self):
        # Steam: CRC-32 of the quoted exe followed by the name, with the top bit set.
        import zlib
        exe, name = '"/x/setup.exe"', 'Hollow Knight'
        self.assertEqual(self.run_tool('id', exe, name)['id'], zlib.crc32((exe + name).encode()) | 0x80000000)

    def test_adds_a_shortcut_on_steamdriver_with_artwork_for_the_signed_in_account(self):
        exe = '/x/installers/Hollow-Knight/setup_hollow_knight_1.5.exe'
        out = self.run_tool('add', 'Hollow Knight', exe)
        sid = out['id']
        self.assertGreaterEqual(sid, 0x80000000)
        shortcuts = read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']
        self.assertEqual(list(shortcuts), ['0'])
        s = shortcuts['0']
        self.assertEqual((s['appid'], s['AppName'], s['Exe'], s['StartDir']),
                         (sid, 'Hollow Knight', f'"{exe}"', '"/x/installers/Hollow-Knight/"'))
        self.assertEqual(s['tags'], {})
        config = (self.steam / 'config/config.vdf').read_text()
        self.assertIn(f'\t\t\t\t\t"{sid}"\n\t\t\t\t\t{{\n\t\t\t\t\t\t"name"\t\t"notproton-steamdriver"', config)
        self.assertIn('"346110"', config)                       # other games keep theirs
        self.assertEqual((self.user / f'grid/{sid}p.jpg').read_text(), 'cover')
        self.assertEqual((self.user / f'grid/{sid}_hero.jpg').read_text(), 'hero')
        self.assertEqual((self.user / f'grid/{sid}_icon.jpg').read_text(), 'icon')
        self.assertEqual(s['icon'], str(self.user / f'grid/{sid}_icon.jpg'))   # the library list's icon
        self.assertTrue((self.user / 'shortcuts.vdf.steamdriver-backup').exists())
        self.assertTrue((self.steam / 'config/config.vdf.steamdriver-backup').exists())
        self.assertFalse((self.steam / 'userdata/852814985/config/shortcuts.vdf').exists())

    def test_adding_the_same_installer_again_keeps_one_shortcut_and_one_mapping(self):
        exe = '/x/installers/Game/setup_game.exe'
        first = self.run_tool('add', 'Game', exe)['id']
        second = self.run_tool('add', 'Game', exe, 'noart')['id']
        self.assertEqual(first, second)
        self.assertEqual(len(read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']), 1)
        self.assertEqual((self.steam / 'config/config.vdf').read_text().count(f'"{first}"'), 1)

    def test_a_newer_installer_in_the_same_folder_updates_the_shortcut_and_keeps_its_id(self):
        folder = self.home / 'Library/Application Support/SteamDriver/installers/Game'
        first = self.run_tool('add', 'Game', folder / 'setup_game_1.0.exe')['id']
        second = self.run_tool('add', 'Game', folder / 'setup_game_1.1.exe', 'noart')['id']
        self.assertEqual(first, second)
        shortcuts = read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']
        self.assertEqual(len(shortcuts), 1)
        self.assertEqual(shortcuts['0']['Exe'], f'"{folder}/setup_game_1.1.exe"')
        # Outside macSteam's installers folder, two files in one folder stay two games.
        self.run_tool('add', 'Other', '/x/a/one.exe')
        self.run_tool('add', 'Other 2', '/x/a/two.exe')
        self.assertEqual(len(read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']), 3)

    def test_existing_shortcuts_survive_byte_for_byte(self):
        self.run_tool('add', 'One', '/x/one.exe')
        before = (self.user / 'shortcuts.vdf').read_bytes()
        self.assertTrue(self.run_tool('roundtrip', self.user / 'shortcuts.vdf')['same'])
        self.run_tool('add', 'Two', '/x/two.exe')
        shortcuts = read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']
        self.assertEqual([s['AppName'] for s in shortcuts.values()], ['One', 'Two'])
        self.assertTrue((self.user / 'shortcuts.vdf').read_bytes().startswith(before[:-2]))

    def test_a_config_without_a_mapping_block_gets_one(self):
        (self.steam / 'config/config.vdf').write_text(CONFIG.replace(CONFIG[CONFIG.index('\t\t\t\t"CompatToolMapping"'):CONFIG.index('\t\t\t}\n\t\t}')], ''))
        sid = self.run_tool('add', 'Game', '/x/g.exe')['id']
        config = (self.steam / 'config/config.vdf').read_text()
        self.assertIn(f'"CompatToolMapping"\n\t\t\t\t{{\n\t\t\t\t\t"{sid}"', config)

    def test_names_from_installer_file_names(self):
        names = self.run_tool('name', 'setup_hollow_knight_1.5.12620_(64bit)_(89718).exe', 'setup_the_witcher_3_wild_hunt_v4.04.exe',
                              'Celeste-Setup.exe', 'SomeGame.exe')['names']
        self.assertEqual(names, ['Hollow Knight', 'The Witcher 3 Wild Hunt', 'Celeste', 'SomeGame'])

    def test_the_app_id_from_a_steam_store_link(self):
        ids = self.run_tool('link', 'https://store.steampowered.com/app/367520/Hollow_Knight/', ' 367520 ',
                            'store.steampowered.com/app/1030300', 'https://steamcommunity.com/app/730/discussions/',
                            'steam://store/1422450', 'https://store.steampowered.com/search/?term=hollow', 'hollow knight')['ids']
        self.assertEqual(ids, [367520, 367520, 1030300, 730, 1422450, 0, 0])

    def installed(self, name, exe_name, shortcut_file=True):
        """A game as macSteam installs it: its installers folder, the shortcut in Steam."""
        folder = self.home / f'Library/Application Support/SteamDriver/installers/{name.replace(" ", "-")}'
        folder.mkdir(parents=True)
        (folder / exe_name).write_text('MZ')
        (folder / 'name').write_text(name + '\n')
        sid = self.run_tool('add', name, folder / exe_name)['id']
        if shortcut_file:
            (folder / 'shortcut').write_text(f'{sid}\n')
        return folder, sid

    def test_installed_games_are_listed_with_their_shortcut(self):
        _, a = self.installed('Alpha', 'setup_alpha.exe')
        _, b = self.installed('Beta Game', 'setup_beta.exe', shortcut_file=False)   # found by its target
        self.run_tool('unsteam', a)
        games = self.run_tool('list')['games']
        self.assertEqual(games, [f'Alpha|{a}|false', f'Beta Game|{b}|true'])

    def test_removing_from_steam_takes_only_that_game(self):
        _, a = self.installed('Alpha', 'setup_alpha.exe')
        _, b = self.installed('Beta', 'setup_beta.exe')
        self.assertIn(f'"{a}"', (self.steam / 'config/config.vdf').read_text())
        self.assertEqual(self.run_tool('unsteam', a), {})
        shortcuts = read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts']
        self.assertEqual(list(shortcuts), ['0'])                  # renumbered from 0
        self.assertEqual(shortcuts['0']['AppName'], 'Beta')
        config = (self.steam / 'config/config.vdf').read_text()
        self.assertNotIn(f'"{a}"', config)
        self.assertIn(f'"{b}"', config)
        self.assertIn('"346110"', config)
        self.assertEqual(sorted(p.name for p in (self.user / 'grid').iterdir()),
                         sorted([f'{b}p.jpg', f'{b}_hero.jpg', f'{b}_icon.jpg']))
        self.assertEqual(config.count('{'), config.count('}'))

    def prefix_with_game(self, sid, where=None):
        """The installed game in its prefix (in Steam's compatdata, or where `where` says)."""
        pfx = (where or self.steam / f'steamapps/compatdata/{sid}') / 'pfx'
        game = pfx / 'drive_c/GOG Games/Alpha/alpha.exe'
        game.parent.mkdir(parents=True, exist_ok=True)
        game.write_bytes(b'MZ' + b'\0' * 70000)
        save = pfx / 'drive_c/users/crossover/Saved Games/slot1'
        save.parent.mkdir(parents=True, exist_ok=True)
        save.write_text('progress')
        return pfx

    def test_delete_removes_that_game_everywhere_and_nothing_else(self):
        folder, a = self.installed('Alpha', 'setup_alpha.exe')
        other, b = self.installed('Beta', 'setup_beta.exe')
        self.prefix_with_game(a)
        self.prefix_with_game(b)
        record = self.home / f'Library/Application Support/SteamDriver/games/{a}'
        record.mkdir(parents=True)
        out = self.run_tool('delete', 'Alpha', 'all')
        self.assertNotIn('error', out)
        self.assertGreater(out['size'], 70000)                   # the list's size: the prefix
        self.assertFalse(out['saves'])
        self.assertFalse(folder.exists())                        # the installer copy
        self.assertFalse((self.steam / f'steamapps/compatdata/{a}').exists())   # the installed game
        self.assertFalse(record.exists())                        # macSteam's record
        self.assertNotIn(f'"{a}"', (self.steam / 'config/config.vdf').read_text())
        self.assertEqual([s['AppName'] for s in read_shortcuts(self.user / 'shortcuts.vdf')['shortcuts'].values()], ['Beta'])
        self.assertFalse(any(p.name.startswith(str(a)) for p in (self.user / 'grid').iterdir()))
        # Beta is untouched.
        self.assertTrue(other.exists())
        self.assertEqual((self.steam / f'steamapps/compatdata/{b}/pfx/drive_c/users/crossover/Saved Games/slot1').read_text(), 'progress')
        self.assertIn(f'"{b}"', (self.steam / 'config/config.vdf').read_text())
        self.assertEqual(self.run_tool('list')['games'], [f'Beta|{b}|true'])

    def test_delete_keeping_saves_leaves_them_for_a_reinstall(self):
        _, a = self.installed('Alpha', 'setup_alpha.exe')
        self.prefix_with_game(a)
        out = self.run_tool('delete', 'Alpha', 'keep')
        self.assertNotIn('error', out)
        self.assertTrue(out['saves'])
        kept = self.home / 'Library/Application Support/SteamDriver/saved-games/Alpha'
        self.assertEqual((kept / 'users/crossover/Saved Games/slot1').read_text(), 'progress')
        self.assertFalse((kept / 'users/crossover/../../drive_c').exists())
        self.assertFalse((self.steam / f'steamapps/compatdata/{a}').exists())

    def test_delete_of_a_game_on_another_disk_removes_its_folder_there_and_the_link(self):
        _, a = self.installed('Alpha', 'setup_alpha.exe')
        disk = self.root / 'Other Disk/Games'
        self.assertEqual(self.run_tool('place', a, disk), {})
        self.prefix_with_game(a, disk / f'Game-{a}')
        neighbour = disk / 'Someone Elses Game'
        neighbour.mkdir()
        (neighbour / 'keep.txt').write_text('keep')
        out = self.run_tool('delete', 'Alpha', 'all')
        self.assertNotIn('error', out)
        self.assertGreater(out['size'], 70000)                   # measured where the game really is
        self.assertFalse((disk / f'Game-{a}').exists())
        self.assertFalse((self.steam / f'steamapps/compatdata/{a}').is_symlink())
        self.assertEqual((neighbour / 'keep.txt').read_text(), 'keep')
        self.assertTrue(disk.is_dir())

    def test_delete_never_follows_a_link_to_a_folder_macsteam_didnt_make(self):
        """compatdata/<id> linked by hand to a whole folder: only the link goes, never the folder."""
        _, a = self.installed('Alpha', 'setup_alpha.exe')
        library = self.root / 'My Games'
        (library / 'Other Game').mkdir(parents=True)
        (library / 'Other Game/save.dat').write_text('precious')
        compat = self.steam / 'steamapps/compatdata'
        compat.mkdir(parents=True)
        (compat / str(a)).symlink_to(library)
        out = self.run_tool('delete', 'Alpha', 'all')
        self.assertNotIn('error', out)
        self.assertEqual((library / 'Other Game/save.dat').read_text(), 'precious')
        self.assertFalse((compat / str(a)).is_symlink())

    def test_delete_of_a_game_already_removed_in_steam_still_removes_its_files(self):
        folder, a = self.installed('Alpha', 'setup_alpha.exe')
        self.prefix_with_game(a)
        self.run_tool('unsteam', a)
        out = self.run_tool('delete', 'Alpha', 'all')
        self.assertNotIn('error', out)
        self.assertFalse(folder.exists())
        self.assertFalse((self.steam / f'steamapps/compatdata/{a}').exists())

    def follow(self, steps, installer='setup_alpha_1.0.exe', stand_in=None):
        """Runs the panel's follower while `steps` ((seconds, log line)) are written to the run log."""
        log = self.steam / 'steamapps/compatdata/2999999999/steamdriver-run.log'
        log.parent.mkdir(parents=True)
        log.write_text('')
        follower = subprocess.Popen([str(self.tool), str(self.home), 'follow', str(log)], stdout=subprocess.PIPE, text=True,
                                    env={**os.environ, 'FOLLOW_INSTALLER': installer})
        for delay, line in steps:
            time.sleep(delay)
            if line == 'START':
                stand_in = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(3.5)', installer])
                continue
            with log.open('a') as f:
                f.write(f'[12:00:00] steamdriver: {line}\n')
        out, _ = follower.communicate(timeout=60)
        if stand_in: stand_in.wait()
        return json.loads(out)

    def test_the_panel_follows_a_gog_install_to_the_end(self):
        out = self.follow([(0.2, 'creating the prefix'), (1.6, 'installer: running setup_alpha_1.0.exe; the next launch starts the game it installs'),
                           (1.6, 'installer: setup_alpha_1.0.exe installed a GOG game; the next launch starts it')])
        self.assertNotIn('error', out)
        self.assertEqual(out['said'], ['Steam is starting Alpha’s installer…',
                                       'Preparing Wine for Alpha (only the first time, about 15 seconds)…',
                                       'Starting Alpha’s installer…'])

    def test_the_panel_says_installed_once_the_installers_process_closes(self):
        """The installer may start the game from its last page: its own process closing is enough."""
        out = self.follow([(0.2, 'installer: running setup_alpha_1.0.exe; the next launch starts the game it installs'),
                           (0.1, 'START'), (0.3, 'launching: wine setup_alpha_1.0.exe')])
        self.assertNotIn('error', out)
        self.assertIn('Alpha’s installer is opening; a big installer takes a little while…', out['said'])

    def test_the_panel_says_when_an_installer_left_nothing(self):
        out = self.follow([(0.2, 'installer: setup_alpha_1.0.exe exited (0) and left no game shortcut or goggame-*.info; the next launch runs it again')])
        self.assertIn('closed without installing a game', out['error'])

    def test_the_panel_says_when_an_installer_closed_early(self):
        out = self.follow([(0.2, "game exited (1); ending the prefix's Wine session")])
        self.assertIn('closed before it finished', out['error'])

    def test_an_already_installed_game_needs_no_waiting(self):
        out = self.follow([(0.2, 'installed game: Alpha (from setup_alpha_1.0.exe); starting drive_c/GOG Games/Alpha/alpha.exe')])
        self.assertEqual(out, {'said': ['Steam is starting Alpha’s installer…']})

    def test_keeping_saves_copies_the_user_folder_and_registry_without_temp(self):
        _, sid = self.installed('Alpha', 'setup_alpha.exe')
        pfx = self.steam / f'steamapps/compatdata/{sid}/pfx'
        save = pfx / 'drive_c/users/crossover/AppData/LocalLow/Team/Alpha/user1.dat'
        save.parent.mkdir(parents=True)
        save.write_text('progress')
        temp = pfx / 'drive_c/users/crossover/AppData/Local/Temp/big.tmp'
        temp.parent.mkdir(parents=True)
        temp.write_text('x')
        (pfx / 'user.reg').write_text('WINE REGISTRY Version 2\n')
        self.assertEqual(self.run_tool('keep', 'Alpha'), {})
        kept = self.home / 'Library/Application Support/SteamDriver/saved-games/Alpha'
        self.assertEqual((kept / 'users/crossover/AppData/LocalLow/Team/Alpha/user1.dat').read_text(), 'progress')
        self.assertEqual((kept / 'user.reg').read_text(), 'WINE REGISTRY Version 2\n')
        self.assertFalse((kept / 'users/crossover/AppData/Local/Temp').exists())

    def test_kept_saves_come_back_when_the_game_is_installed_again(self):
        compat = self.root / 'compat'
        (compat / 'pfx/drive_c/users/crossover').mkdir(parents=True)
        (compat / 'pfx/user.reg').write_text('new\n')
        inst_dir = self.root / 'SD/installers/Alpha'
        inst_dir.mkdir(parents=True)
        kept = self.root / 'SD/saved-games/Alpha'
        (kept / 'users/crossover/Saved Games').mkdir(parents=True)
        (kept / 'users/crossover/Saved Games/slot1').write_text('progress')
        (kept / 'user.reg').write_text('old settings\n')
        out = self.run_choice(compat, inst_dir / 'setup_alpha.exe', "WINESERVER=/usr/bin/true")
        self.assertIn('put back', out)
        self.assertEqual((compat / 'pfx/drive_c/users/crossover/Saved Games/slot1').read_text(), 'progress')
        self.assertEqual((compat / 'pfx/user.reg').read_text(), 'old settings\n')
        # Only once: the next launch leaves the prefix alone.
        (compat / 'pfx/user.reg').write_text('played since\n')
        self.assertNotIn('put back', self.run_choice(compat, inst_dir / 'setup_alpha.exe', "WINESERVER=/usr/bin/true"))
        self.assertEqual((compat / 'pfx/user.reg').read_text(), 'played since\n')

    def test_install_to_another_folder_links_the_prefix_there(self):
        _, sid = self.installed('Alpha', 'setup_alpha.exe')
        other = self.root / 'Other Disk/Games'
        self.assertEqual(self.run_tool('place', sid, other), {})
        link = self.steam / f'steamapps/compatdata/{sid}'
        self.assertTrue(link.is_symlink())
        self.assertEqual(Path(link.readlink()), other / f'Game-{sid}')
        self.assertTrue((other / f'Game-{sid}').is_dir())
        self.assertEqual(self.run_tool('where')['where'], [str(other / f'Game-{sid}')])
        self.assertEqual(list(other.iterdir()), [other / f'Game-{sid}'])     # the probe is gone
        # A prefix that is already there stays where it is.
        self.assertEqual(self.run_tool('place', sid, self.root / 'Third'), {})
        self.assertEqual(Path(link.readlink()), other / f'Game-{sid}')

    def test_a_folder_that_cant_hold_a_game_is_refused(self):
        locked = self.root / 'locked'
        locked.mkdir()
        locked.chmod(0o500)
        self.addCleanup(locked.chmod, 0o700)
        out = self.run_tool('place', 2999999999, locked)
        self.assertIn('isn’t writable', out['error'])
        self.assertFalse((self.steam / 'steamapps/compatdata/2999999999').exists())

    def test_the_run_script_stops_when_the_games_disk_is_not_connected(self):
        compat = self.root / 'compatdata/3000000001'
        compat.parent.mkdir(parents=True)
        compat.symlink_to(self.root / 'Unplugged/Game-3000000001')
        text = RUN_SCRIPT.read_text()
        block = re.search(r'^\[\[ -n \$compat \]\].*?(?=^/bin/mkdir -p "\$compat")', text, re.S | re.M).group(0)
        script = f"alert(){{ print -r -- \"alert: $2\"; }}; verb=waitforexitandrun compat='{compat}'\n{block}\nprint reached"
        result = subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("isn't connected", result.stdout)
        self.assertNotIn('reached', result.stdout)
        self.assertFalse((self.root / 'Unplugged').exists())             # no empty prefix made

    def test_gogs_bin_parts_come_along(self):
        d = self.root / 'dl'
        d.mkdir()
        for n in ('setup_game_1.0.exe', 'setup_game_1.0-1.bin', 'setup_game_1.0-2.bin', 'setup_other.exe', 'other-1.bin'):
            (d / n).write_text('x')
        self.assertEqual(self.run_tool('files', d / 'setup_game_1.0.exe')['files'],
                         ['setup_game_1.0.exe', 'setup_game_1.0-1.bin', 'setup_game_1.0-2.bin'])

    # MARK: the run script
    def script_part(self, start, end):
        text = RUN_SCRIPT.read_text()
        return re.search(start + r'.*?(?=' + end + ')', text, re.S | re.M).group(0)

    def run_choice(self, compat, target, env=''):
        """Which program a launch starts: the run script's installer block alone."""
        block = self.script_part(r'^  inst= inst_dir= inst_ran=', r'^  /bin/mkdir -p "\$steamdriver/games"')
        script = (f"say(){{ print -r -- \"say: $*\"; }}; steamdriver='{self.root}/SD' compat='{compat}' WINEPREFIX='{compat}/pfx' game_name= {env}\n"
                  f"set -- '{target}' --arg\n{block}\n"
                  "print -r -- \"args: $*\"; print -r -- \"name=$game_name ran=$inst_ran dir=$inst_dir\"")
        return subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout

    def run_after(self, compat, inst, rc=0):
        """What the run script keeps after an installer exits."""
        block = self.script_part(r'^    if \[\[ -n \$\{inst_ran:-\} \]\]; then', r'^    say "game exited')
        script = (f"say(){{ print -r -- \"say: $*\"; }}; compat='{compat}' WINEPREFIX='{compat}/pfx' inst='{inst}' inst_ran=1 rc={rc}\n{block}")
        return subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout

    def test_a_macsteam_installer_runs_then_its_start_menu_shortcut_starts_the_game(self):
        compat = self.root / 'compat'
        drive = compat / 'pfx/drive_c'
        (drive / 'windows').mkdir(parents=True)
        inst_dir = self.root / 'SD/installers/Some-Game'
        inst_dir.mkdir(parents=True)
        (inst_dir / 'name').write_text('Some Game\n')
        target = inst_dir / 'SomeGameSetup.exe'
        out = self.run_choice(compat, target)
        self.assertIn('running SomeGameSetup.exe', out)
        self.assertIn('name=Some Game ran=1', out)
        # The installer: the game, an uninstaller, and their Start Menu shortcuts.
        (compat / '.installer-started').write_text('')
        game = drive / 'Games/Some Game/SomeGame.exe'
        game.parent.mkdir(parents=True)
        game.write_text('MZ')
        (game.parent / 'unins000.exe').write_text('MZ')
        menu = drive / 'ProgramData/Microsoft/Windows/Start Menu/Programs/Some Game'
        menu.mkdir(parents=True)
        import os
        import time
        later = time.time() + 5
        for lnk, path in (('Uninstall Some Game.lnk', r'C:\Games\Some Game\unins000.exe'), ('Some Game.lnk', r'C:\Games\Some Game\SomeGame.exe')):
            (menu / lnk).write_bytes(b'L\0\0\0\x01\x14\x02\0' + b'\0' * 60 + path.encode() + b'\0' + 'x'.encode('utf-16-le'))
            os.utime(menu / lnk, (later, later))
        out = self.run_after(compat, 'SomeGameSetup.exe')
        self.assertIn('installed drive_c/Games/Some Game/SomeGame.exe', out)
        self.assertEqual((compat / 'installed-exe').read_text().strip(), str(game))
        self.assertEqual((compat / 'installed-from').read_text().strip(), 'SomeGameSetup.exe')
        out = self.run_choice(compat, target)
        self.assertIn(f'args: {game} --arg', out)
        self.assertIn('ran= dir=' + str(game.parent), out)
        # A new installer (an update) runs again; STEAMDRIVER_SETUP=1 forces it.
        self.assertIn('running SomeGameSetup2.exe', self.run_choice(compat, inst_dir / 'SomeGameSetup2.exe'))
        self.assertIn('ran=1', self.run_choice(compat, target, 'STEAMDRIVER_SETUP=1'))

    def test_an_installer_that_left_nothing_runs_again(self):
        compat = self.root / 'compat'
        (compat / 'pfx/drive_c').mkdir(parents=True)
        (compat / '.installer-started').write_text('')
        self.assertIn('left no game shortcut', self.run_after(compat, 'setup_x.exe', 1))
        self.assertFalse((compat / 'installed-from').exists())

    def test_a_gog_installer_anywhere_is_still_recognised(self):
        compat = self.root / 'compat'
        game = compat / 'pfx/drive_c/GOG Games/Foo'
        game.mkdir(parents=True)
        out = self.run_choice(compat, '/Users/x/Downloads/setup_foo_1.0.exe')
        self.assertIn('ran=1', out)
        (game / 'goggame-1.info').write_text(json.dumps({'gameId': '1', 'rootGameId': '1', 'name': 'Foo',
                                                         'playTasks': [{'isPrimary': True, 'path': 'bin\\foo.exe', 'arguments': '-x'}]}))
        (game / 'bin').mkdir()
        (game / 'bin/foo.exe').write_text('MZ')
        self.assertIn('installed a GOG game', self.run_after(compat, 'setup_foo_1.0.exe'))
        out = self.run_choice(compat, '/Users/x/Downloads/setup_foo_1.0.exe')
        self.assertIn(f'args: {game}/bin/foo.exe -x --arg', out)
        self.assertIn('name=Foo ran=', out)

    def test_plain_programs_are_left_alone(self):
        compat = self.root / 'compat'
        (compat / 'pfx/drive_c').mkdir(parents=True)
        out = self.run_choice(compat, '/x/game.exe')
        self.assertIn('args: /x/game.exe --arg', out)
        self.assertNotIn('say:', out)


if __name__ == '__main__':
    unittest.main()


class SharedPrefix(unittest.TestCase):
    """The run script's prefix creation: one template per runtime, cloned for new prefixes."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='steamdriver-prefix-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.engine = self.root / 'runtime'
        (self.engine / 'bin').mkdir(parents=True)
        (self.engine / 'runtime.json').write_text('{\n  "version": "macfinals-999"\n}\n')
        # A stand-in for wineboot: writes what a prefix needs to count as made.
        wine = self.engine / 'bin/wine'
        wine.write_text('#!/bin/sh\nmkdir -p "$WINEPREFIX/drive_c/windows"\necho booted > "$WINEPREFIX/system.reg"\n'
                        'echo "$WINEPREFIX" > "$WINEPREFIX/drive_c/windows/made-in"\n')
        wine.chmod(0o755)
        (self.engine / 'bin/wineserver').write_text('#!/bin/sh\nexit 0\n')
        (self.engine / 'bin/wineserver').chmod(0o755)
        self.sd = self.root / 'SteamDriver'
        self.sd.mkdir()

    def make(self, app_id):
        text = RUN_SCRIPT.read_text()
        block = re.search(r'^# First run: make the prefix.*?(?=^# NotProton\'s Steam bridge)', text, re.S | re.M).group(0)
        compat = self.root / 'compatdata' / str(app_id)
        compat.mkdir(parents=True)
        script = (f"say(){{ print -r -- \"say: $*\"; }}; engine='{self.engine}' steamdriver='{self.sd}' app_id={app_id} "
                  f"WINEPREFIX='{compat}/pfx' WINELOADER='{self.engine}/bin/wine' WINESERVER='{self.engine}/bin/wineserver' log=/dev/null; export WINEPREFIX\n{block}")
        out = subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout
        return out, compat / 'pfx'

    def test_the_first_prefix_becomes_the_template_and_the_next_is_a_copy(self):
        out, first = self.make(100)
        self.assertIn('creating the prefix', out)
        self.assertIn('saved this prefix as the shared macfinals-999 template', out)
        template = self.sd / 'prefix-templates/macfinals-999'
        self.assertTrue((template / '.complete').exists())
        self.assertEqual((template / 'system.reg').read_text(), 'booted\n')
        out, second = self.make(200)
        self.assertIn('prefix cloned from the shared macfinals-999 template', out)
        self.assertNotIn('creating', out)
        # The copy is the first prefix as wineboot made it, without the template's marker.
        self.assertEqual((second / 'drive_c/windows/made-in').read_text(), (first / 'drive_c/windows/made-in').read_text())
        self.assertFalse((second / '.complete').exists())
        self.assertEqual(list((self.sd / 'prefix-templates').iterdir()), [template])   # no leftovers

    def test_an_own_runtime_copy_is_built_and_never_shared(self):
        self.make(100)
        (self.sd / 'custom-runtimes').write_text('on\n')
        (self.sd / 'profiles/app-300').mkdir(parents=True)
        (self.sd / 'profiles/app-300/own-runtime').write_text('own\n')
        out, own = self.make(300)
        self.assertIn('its own runtime copy, not shared', out)
        self.assertEqual((own / 'drive_c/windows/made-in').read_text().strip(), str(own))

    def test_own_runtime_needs_the_advanced_switch(self):
        self.make(100)
        (self.sd / 'profiles/app-300').mkdir(parents=True)
        (self.sd / 'profiles/app-300/own-runtime').write_text('own\n')
        out, _ = self.make(300)
        self.assertIn('cloned from the shared', out)

    def test_an_own_copy_first_doesnt_make_a_template(self):
        (self.sd / 'custom-runtimes').write_text('on\n')
        (self.sd / 'profiles/app-300').mkdir(parents=True)
        (self.sd / 'profiles/app-300/own-runtime').write_text('own\n')
        self.make(300)
        self.assertFalse((self.sd / 'prefix-templates').exists())

    def test_an_existing_prefix_is_left_alone(self):
        self.make(100)
        pfx = self.root / 'compatdata/400/pfx'
        pfx.mkdir(parents=True)
        (pfx / 'system.reg').write_text('mine\n')
        out = subprocess.run(['/bin/zsh', '-f', '-c', 'true'], capture_output=True, text=True).stdout
        self.make_existing = True
        text = RUN_SCRIPT.read_text()
        block = re.search(r'^# First run: make the prefix.*?(?=^# NotProton\'s Steam bridge)', text, re.S | re.M).group(0)
        script = (f"say(){{ print -r -- \"say: $*\"; }}; engine='{self.engine}' steamdriver='{self.sd}' app_id=400 "
                  f"WINEPREFIX='{pfx}' WINELOADER='{self.engine}/bin/wine' WINESERVER='{self.engine}/bin/wineserver' log=/dev/null; export WINEPREFIX\n{block}")
        out = subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout
        self.assertEqual(out, '')
        self.assertEqual((pfx / 'system.reg').read_text(), 'mine\n')
