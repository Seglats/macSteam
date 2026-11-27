"""Removing macSteam's own Steam with Keep games (app/Sources/OwnSteamGames.swift), in a throwaway home.

Compiles the real OwnSteamGames and moves the games of a fake own Steam into a fake Steam app."""
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
    let root=URL(fileURLWithPath:a[2])
    let listed=OwnSteamGames.list(root).map(\.name)
    do {
        let r=try OwnSteamGames.move(root)
        print(String(data:try! JSONSerialization.data(withJSONObject:["listed":listed,"moved":r.moved,"skipped":r.skipped]),encoding:.utf8)!)
    } catch { print(String(data:try! JSONSerialization.data(withJSONObject:["listed":listed,"error":error.localizedDescription]),encoding:.utf8)!) }
}}
'''
SOURCES = ['OwnSteamGames', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Brand', 'GameTools', 'CrossOverLicense', 'InstallLocation', 'ControllerSupport', 'Surround']


def manifest(appid, name, folder, size=100):
    return f'"AppState"\n{{\n\t"appid"\t\t"{appid}"\n\t"name"\t\t"{name}"\n\t"installdir"\t\t"{folder}"\n\t"SizeOnDisk"\t\t"{size}"\n}}\n'


class OwnSteamGamesMove(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='own-steam-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.tool = folder / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{n}.swift') for n in SOURCES],
                        str(folder / 'harness.swift'), '-o', str(cls.tool)], check=True, capture_output=True)

    def test_games_move_to_the_steam_app_and_ones_it_has_stay(self):
        with tempfile.TemporaryDirectory() as t:
            home, root = Path(t) / 'home', Path(t) / 'own'
            own = root / 'environment/drive_c/Program Files (x86)/Steam/steamapps'
            app = home / 'Library/Application Support/Steam/steamapps'
            for appid, name, folder in (('730', 'Counter-Strike 2', 'Counter-Strike Global Offensive'), ('1422450', 'Deadlock', 'Deadlock')):
                (own / 'common' / folder).mkdir(parents=True)
                (own / 'common' / folder / 'game.bin').write_text(name)
                (own / f'appmanifest_{appid}.acf').write_text(manifest(appid, name, folder))
            (own / 'appmanifest_9.acf').write_text(manifest('9', 'Half installed', 'Gone'))   # no folder: not a game
            (own / 'workshop/content/730/123').mkdir(parents=True)
            (own / 'workshop/appworkshop_730.acf').write_text('x')
            (app / 'common/Deadlock').mkdir(parents=True)        # the Steam app has Deadlock already
            (app / 'appmanifest_1422450.acf').write_text('theirs')
            out = json.loads(subprocess.run([self.tool, home, root], check=True, capture_output=True, text=True).stdout)
            self.assertEqual(out['listed'], ['Deadlock', 'Counter-Strike 2'])
            self.assertEqual(out['moved'], ['Counter-Strike 2'])
            self.assertEqual(out['skipped'], ['Deadlock'])
            self.assertEqual((app / 'common/Counter-Strike Global Offensive/game.bin').read_text(), 'Counter-Strike 2')
            self.assertIn('"730"', (app / 'appmanifest_730.acf').read_text())
            self.assertTrue((app / 'workshop/content/730/123').is_dir())
            self.assertTrue((app / 'workshop/appworkshop_730.acf').exists())
            self.assertFalse((own / 'common/Counter-Strike Global Offensive').exists())
            self.assertEqual((app / 'appmanifest_1422450.acf').read_text(), 'theirs')   # theirs untouched
            self.assertTrue((own / 'common/Deadlock/game.bin').exists())                # ours left for the Trash


if __name__ == '__main__':
    unittest.main()
