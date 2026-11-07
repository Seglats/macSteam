"""MacVolvo's options for NotProton (app/Sources/NotProton.swift) in a throwaway home folder.

Compiles the real NotProton.swift with a small driver and runs it; never touches Steam, NotProton,
Wine or the player's own Library."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
import Foundation
@main struct Harness { static func main() {
    let a=CommandLine.arguments; NotProton.home=URL(fileURLWithPath:a[1]); let res=URL(fileURLWithPath:a[2])
    var out:[String:Any]=[:]
    do {
        switch a[3] {
        case "add": try NotProton.add(resources:res)
        case "remove": try NotProton.remove()
        default: break
        }
    } catch { out["error"]=error.localizedDescription }
    out["status"]=String(describing:NotProton.status(res)).components(separatedBy:"(").first ?? ""
    out["current"]=NotProton.isCurrent(resources:res)
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
        self.variants = self.lib / 'notproton/variants/macvolvo.conf'
        self.plugins = self.lib / 'notproton/plugins/macvolvo.conf'

    def run_tool(self, action):
        result = subprocess.run([str(self.tool), str(self.home), str(self.resources), action],
                                capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def test_adds_a_variant_and_rows_for_the_rosetta_build_only(self):
        self.assertEqual(self.run_tool('status'), {'status': 'available', 'current': False})
        self.assertEqual(self.run_tool('add'), {'status': 'added', 'current': True})
        variants, plugins = self.variants.read_text(), self.plugins.read_text()
        self.assertIn('[macvolvo-26.3.0.39832]', variants)
        self.assertIn('build = 26.3.0.39832', variants)
        self.assertIn('dst = lib/wine/x86_64-unix', variants)
        self.assertNotIn('41073', variants + plugins)
        self.assertEqual((self.lib / 'MacVolvo NotProton/26.3.0.39832/winemac.so').read_bytes(), b'driver v1')
        for row in ('[NOTPROTON_MACVOLVO_MOUSE]', '[NOTPROTON_MACVOLVO_HIGH_PRECISION]', '[NOTPROTON_MACVOLVO_CLASSIC]',
                    '[NOTPROTON_ENV_WINEMAC_MOUSELOOK_RAW_INTERVAL_US]'):
            self.assertIn(row, plugins)
        self.assertIn('env = WINEMAC_RAW_REQUEST=1', plugins)
        self.assertEqual(plugins.count('tools = notproton-26.3\n'), 4)

    def test_never_writes_a_tool_or_touches_notprotons_files(self):
        self.run_tool('add')
        tools = self.lib / 'Steam/compatibilitytools.d'
        self.assertFalse(tools.exists() and any(tools.iterdir()))
        self.assertEqual((self.lib / 'notproton/tools').read_text().count('\n'), 2)

    def test_a_newer_driver_makes_it_out_of_date_until_updated(self):
        self.run_tool('add')
        (self.resources / 'NotProton/26.3.0.39832/winemac.so').write_bytes(b'driver v2')
        self.assertEqual(self.run_tool('status'), {'status': 'added', 'current': False})
        self.assertEqual(self.run_tool('add'), {'status': 'added', 'current': True})

    def test_refuses_before_writing_when_a_file_is_not_macvolvos(self):
        self.plugins.parent.mkdir(parents=True)
        self.plugins.write_text('[NOTPROTON_MINE]\nlabel = Mine\n')
        result = self.run_tool('add')
        self.assertIn("wasn't written by MacVolvo", result['error'])
        self.assertEqual(self.plugins.read_text(), '[NOTPROTON_MINE]\nlabel = Mine\n')
        self.assertFalse(self.variants.exists())
        self.assertFalse((self.lib / 'MacVolvo NotProton').exists())

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
        self.assertFalse((self.lib / 'MacVolvo NotProton').exists())
        self.assertTrue((notprotons / 'run').exists())

    def test_without_notproton_it_says_so(self):
        (self.lib / 'notproton/tools').unlink()
        self.assertEqual(self.run_tool('status'), {'status': 'notInstalled', 'current': False})


if __name__ == '__main__':
    unittest.main()
