"""The CrossOver licence check before the SteamDriver tool is installed (app/Sources/CrossOverLicense.swift),
as NotProton checks before installing its own tool: the licence file and its signature, verified with
macOS's /usr/bin/openssl against CodeWeavers' key in an installed CrossOver, and its expiry. A key made
for the test stands in for CodeWeavers'; a fake CrossOver and fake Preferences live in a temporary folder."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KEY = 'Contents/SharedSupport/CrossOver/share/crossover/data/tie.pub'
NAME = 'com.codeweavers.CrossOver'

HARNESS = r'''
import Foundation
import SwiftUI
enum LauncherStyle { static let accent=Color.accentColor }
@main struct H { @MainActor static func main() {
    let a=CommandLine.arguments
    CrossOverLicense.systemRoot=URL(fileURLWithPath:a[1])
    NotProton.home=URL(fileURLWithPath:a[2])
    var out:[String:Any]=["status":String(describing:CrossOverLicense.check()).components(separatedBy:"(").first ?? ""]
    if a.count > 3 {
        do {
            try GameTools.install(GameTools.Tool(id:"notproton-steamdriver",game:"SteamDriver",display:"SteamDriver",
                                                 runtime:"macfinals-test",bundle:URL(fileURLWithPath:a[3])))
            out["installed"]=true
        } catch { out["installed"]=false;out["error"]=error.localizedDescription }
    }
    print(String(data:try! JSONSerialization.data(withJSONObject:out),encoding:.utf8)!)
}}
'''
SOURCES = ['CrossOverLicense', 'GameTools', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Brand',
           'InstallLocation', 'ControllerSupport', 'Surround']


class CrossOverLicenseCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='crossover-license-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.harness = folder / 'harness'
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{s}.swift') for s in SOURCES],
                        str(folder / 'harness.swift'), '-o', str(cls.harness)], check=True, capture_output=True)
        cls.private, cls.public = folder / 'key.pem', folder / 'key.pub'
        cls.other = folder / 'other.pem'
        for key in (cls.private, cls.other):
            subprocess.run(['/usr/bin/openssl', 'genrsa', '-out', str(key), '2048'], check=True, capture_output=True)
        subprocess.run(['/usr/bin/openssl', 'rsa', '-in', str(cls.private), '-pubout', '-out', str(cls.public)],
                       check=True, capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='crossover-license-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'root'
        self.home = Path(self.temp.name) / 'home'
        (self.root / 'Library/Preferences').mkdir(parents=True)
        (self.home / 'Library/Preferences').mkdir(parents=True)

    def crossover(self, app='CrossOver.app', where=None):
        key = (where or self.root) / 'Applications' / app / KEY
        key.parent.mkdir(parents=True)
        shutil.copy(self.public, key)

    def license(self, folder=None, expires='2999/01/01', signed_with=None, suffix='sha256', tamper=False):
        folder = folder or self.root / 'Library/Preferences'
        text = f'customer=Test\nemail=test@example.invalid\nexpires={expires}\nid=TEST-1\n'
        (folder / f'{NAME}.license').write_text(text)
        subprocess.run(['/usr/bin/openssl', 'dgst', '-sha256', '-sign', str(signed_with or self.private),
                        '-out', str(folder / f'{NAME}.{suffix}'), str(folder / f'{NAME}.license')], check=True, capture_output=True)
        if tamper:
            (folder / f'{NAME}.license').write_text(text.replace('2999', '3999'))

    def check(self, bundle=None):
        args = [str(self.harness), str(self.root), str(self.home)] + ([str(bundle)] if bundle else [])
        return json.loads(subprocess.run(args, capture_output=True, text=True, check=True, timeout=60).stdout)

    def test_without_crossover_it_says_so(self):
        self.license()
        self.assertEqual(self.check()['status'], 'noCrossOver')

    def test_crossover_without_a_licence_is_not_activated(self):
        self.crossover()
        self.assertEqual(self.check()['status'], 'notActivated')

    def test_a_signed_licence_is_valid(self):
        self.crossover()
        self.license()
        self.assertEqual(self.check()['status'], 'valid')

    def test_crossover_preview_and_a_licence_in_the_home_folder_count_too(self):
        self.crossover('CrossOver Preview.app', where=self.home)
        self.license(folder=self.home / 'Library/Preferences', suffix='sig')
        self.assertEqual(self.check()['status'], 'valid')

    def test_a_changed_licence_or_another_key_is_refused(self):
        self.crossover()
        self.license(tamper=True)
        self.assertEqual(self.check()['status'], 'invalid')
        self.license(signed_with=self.other)
        self.assertEqual(self.check()['status'], 'invalid')

    def test_an_expired_licence_is_refused(self):
        self.crossover()
        self.license(expires='2020/01/01')
        self.assertEqual(self.check()['status'], 'expired')

    def test_one_valid_licence_is_enough(self):
        self.crossover()
        self.license(folder=self.home / 'Library/Preferences', signed_with=self.other)   # an old one that no longer verifies
        self.license()
        self.assertEqual(self.check()['status'], 'valid')

    def test_installing_steamdriver_is_refused_without_a_licence(self):
        bundle = Path(self.temp.name) / 'bundle'
        bundle.mkdir()
        self.crossover()
        result = self.check(bundle)
        self.assertEqual((result['status'], result['installed']), ('notActivated', False))
        self.assertIn('licensed copy of CrossOver', result['error'])
        self.assertIn('Nothing was installed', result['error'])
        self.assertFalse((self.home / 'Library/Application Support/Steam/compatibilitytools.d/notproton-steamdriver').exists())


if __name__ == '__main__':
    unittest.main()
