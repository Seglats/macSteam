"""MacFinals keeps its own identity and root, apart from MacVolvo's, and a DirectX 12 profile.

Native workers operate only on scratch directories. No Wine or Steam runs here.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from build_portable_setup import build


class MacFinalsIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='macfinals-identity-build-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        worker = (ROOT / 'scripts/portable_setup.m').read_text()
        env = worker[worker.index('static NSMutableDictionary *wineEnv('):worker.index('static NSTask *task(')]
        harness = r'''#import <Foundation/Foundation.h>
#include "candidate_contract.generated.h"
static NSString *root=@"/fixture";
static NSFileManager *fm;
static NSString *join(NSString *a,NSString *b) {return [a stringByAppendingPathComponent:b];}
static NSString *gameList(NSString *separator) {return [@[@"Discovery.exe"] componentsJoinedByString:separator];}
static NSMutableDictionary *state(void) {
    NSMutableDictionary *s=[NSMutableDictionary dictionary];
    if(getenv("FIXTURE_MSYNC"))s[@"session_msync"]=@(getenv("FIXTURE_MSYNC"));
    if(getenv("FIXTURE_AVX"))s[@"session_advertise_avx"]=@(getenv("FIXTURE_AVX"));
    return s;
}
''' + env + r'''
int main(void) {@autoreleasepool {
    fm=NSFileManager.defaultManager;
    NSData *data=[NSJSONSerialization dataWithJSONObject:wineEnv(@"/fixture/engine") options:0 error:nil];
    fwrite(data.bytes,1,data.length,stdout);
}}
'''
        env_path = folder / 'environment.m'
        env_path.write_text(harness)
        brand_path = folder / 'VariantProbe.swift'
        brand_path.write_text('''import Foundation
@main struct Probe {
    static func main() throws {
        let root=URL(fileURLWithPath:CommandLine.arguments[1])
        let result:[String:Any]=["name":Brand.name,"id":Brand.bundleIdentifier,
            "folder":InstallLocation.folderName,"marker":Brand.ownerMarker,
            "updates":Brand.allowsUpdates,"problem":InstallLocation.problem(for:root) ?? ""]
        print(String(data:try JSONSerialization.data(withJSONObject:result),encoding:.utf8)!)
    }
}
''')
        cls.worker = folder / 'ow2-setup'
        build(cls.worker, ROOT / 'runtime/toolchains/libarchive-3.7.7')
        profile = folder / 'environment'
        subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-I', str(ROOT / 'scripts'), str(env_path), '-framework',
                        'Foundation', '-o', str(profile)], check=True, capture_output=True)
        cls.profile = json.loads(subprocess.check_output([str(profile)]))
        cls.profile_binary = profile
        cls.brand = folder / 'brand'
        result=subprocess.run(['swiftc', '-parse-as-library', '-target', 'arm64-apple-macos26.5',
                        str(ROOT / 'app/Sources/Brand.swift'),
                        str(ROOT / 'app/Sources/InstallLocation.swift'),
                        str(brand_path), '-o', str(cls.brand)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='macfinals-identity-roots-')
        self.addCleanup(self.fixture.cleanup)
        self.base = Path(self.fixture.name)

    def call(self, root):
        result = subprocess.run([str(self.worker), 'status', '--root', str(root)],
                                capture_output=True, text=True, timeout=15)
        return result, json.loads(result.stdout.splitlines()[-1])

    def test_own_marker_and_macvolvo_roots_rejected(self):
        root = self.base / 'MacFinals'
        result, _ = self.call(root)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((root / '.macfinals-owner').read_text(), 'macfinals-v01\n')
        for marker in ('.macvolvo-owner', '.macvolvo-gptk4-owner'):
            other = self.base / marker
            other.mkdir()
            (other / marker).write_text('owned by MacVolvo')
            for candidate in (other, other / 'nested'):
                result, row = self.call(candidate)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(row['code'], 'directory_not_owned_by_app')
            self.assertFalse((other / 'nested').exists())

    def test_swift_identity_and_location_checks_match_worker(self):
        root = self.base / 'MacFinals'
        root.mkdir()
        result = json.loads(subprocess.check_output([str(self.brand), str(root)]))
        self.assertEqual((result['name'], result['folder'], result['marker'], result['id']),
                         ('MacFinals', 'MacFinals', '.macfinals-owner', 'app.macfinals.launcher'))
        self.assertFalse(result['updates'])
        self.assertEqual(result['problem'], '')
        for marker in ('.macvolvo-owner', '.macvolvo-gptk4-owner'):
            other = self.base / marker
            other.mkdir()
            (other / marker).write_text('owned by MacVolvo')
            result = json.loads(subprocess.check_output([str(self.brand), str(other / 'nested')]))
            self.assertIn('MacVolvo app', result['problem'])

    def test_d3dmetal_profile_loads_directx_12(self):
        env = self.profile
        self.assertEqual(env['CX_GRAPHICS_BACKEND'], 'd3dmetal')
        self.assertEqual(env['CX_ACTIVE_GRAPHICS_BACKEND'], 'd3dmetal')
        self.assertFalse(any(key.startswith('DXMT_') for key in env))
        self.assertEqual(env['WINEDLLOVERRIDES'],
                         'd3d11,dxgi,d3d10,d3d12,d3d12core=b;d3d10core,winemetal,nvapi64,nvngx=')
        self.assertEqual(env['D3DM_ENABLE_METALFX'], '0')
        self.assertNotIn('MACFINALS_DISPLAY_FIX', env)
        self.assertEqual(env['D3DMETAL_FRAMEWORK_PATH'],
                         '/fixture/engine/lib/external/D3DMetal.framework/D3DMetal')
        self.assertNotIn('D3DM_MTL4', env)
        # Wine session settings shared with the launch contract stay as they were.
        for key, value in (('WINEMSYNC', '1'), ('WINEESYNC', '0'), ('ROSETTA_ADVERTISE_AVX', '1'),
                           ('WINEPREFIX', '/fixture/environment'), ('WINEMAC_MOUSELOOK', 'Discovery.exe'),
                           ('WINE_GAME_MODE', 'Discovery.exe'), ('WEBHELPER_EXTRA_ARGS', '--use-angle=vulkan')):
            self.assertEqual(env[key], value, key)

    def test_session_settings_change_only_what_was_chosen(self):
        env = json.loads(subprocess.check_output([str(self.profile_binary)],
                                                 env={'FIXTURE_MSYNC': '0'}))
        self.assertNotIn('WINEMSYNC', env)
        self.assertEqual(env['ROSETTA_ADVERTISE_AVX'], '1')
        env = json.loads(subprocess.check_output([str(self.profile_binary)],
                                                 env={'FIXTURE_AVX': '0'}))
        self.assertEqual(env['WINEMSYNC'], '1')
        self.assertEqual(env['ROSETTA_ADVERTISE_AVX'], '0')


if __name__ == '__main__':
    unittest.main()
