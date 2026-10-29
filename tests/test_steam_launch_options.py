"""Steam Properties edits against synthetic data only; never opens Wine or Steam."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SteamLaunchOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory(prefix="macfinals-steam-options-tests-")
        cls.addClassCleanup(cls.build_dir.cleanup)
        folder = Path(cls.build_dir.name)
        header = (ROOT / "scripts/portable_steam.h").read_text()
        # Compile the actual persistence/parser and profile functions in isolation.
        source = header.split("static NSString *mvProcessName(")[0]
        profile = header[header.index("static NSString *gamesProfile("):]
        harness = r'''
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <errno.h>
static NSString *root;
static NSFileManager *fm;
static BOOL activeSession;
static NSString *join(NSString *a, NSString *b) {return [a stringByAppendingPathComponent:b];}
static BOOL matches(NSString *text, NSString *pattern) {
    return [text rangeOfString:pattern options:NSRegularExpressionSearch].location!=NSNotFound;
}
static NSArray *sessionProcesses(void) {return activeSession?@[@"synthetic-session"]:@[];}
static void event(NSString *name, NSDictionary *details) {(void)name;(void)details;}
'''+source+profile+r'''
int main(int argc, const char *argv[]) { @autoreleasepool {
    if(argc!=2)return 3;
    NSDictionary *input=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[1])] options:0 error:nil];
    fm=NSFileManager.defaultManager;root=input[@"root"];activeSession=[input[@"active"] boolValue];
    NSDictionary *defaults=steamLaunchDefaults(input[@"options"]);
    NSDictionary *result;
    @try {
        if([input[@"op"] isEqual:@"configure"]) {
            BOOL needs=NO,valid=steamLaunchOptionsNeedUpdate(defaults,&needs);
            result=@{@"valid":@(valid),@"needs":@(needs),@"configured":@(configureSteamLaunchOptions(defaults))};
        } else if([input[@"op"] isEqual:@"profile"]) {
            result=@{@"text":gamesProfile(input[@"text"],input[@"options"])};
        } else result=@{@"text":mvUpdatedSteamVDF(input[@"text"],defaults)};
    } @catch(NSException *error) {(void)error;result=@{@"error":@"invalid"};}
    NSData *data=[NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
    fwrite(data.bytes,1,data.length,stdout);return 0;
} }
'''
        path = folder / "steam-options.m"
        path.write_text(harness)
        cls.binary = folder / "steam-options-tests"
        subprocess.run(["/usr/bin/clang", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", str(path), "-framework", "Foundation", "-o", str(cls.binary)], check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="macfinals-steam-options-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.options = {"finals": {"width": 2560, "height": 1600, "fullscreen": False}}

    def call(self, **values):
        fixture = self.base / "input.json"
        fixture.write_text(json.dumps({"options": self.options, **values}))
        result = subprocess.run([str(self.binary), str(fixture)], capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def vdf(self, apps):
        return ('"UserLocalConfigStore"\n{\n\t"PrivateFixture" "keep untouched"\n'
                '\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
                '\t\t\t\t"apps"\n\t\t\t\t{\n'+apps+'\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n')

    def test_preserves_unrelated_config_and_quoted_arguments(self):
        text = self.vdf('"2073850" {"LaunchOptions" "-log -ini:Engine:[Core.Log]:LogTemp=Verbose \\\"quoted arg\\\" -dx11 -d3d12 -sm5 -ResX=999 -ResY=777 -fullscreen -nojoy -w 640 -h 480"}\n'
                        '"42" {"LaunchOptions" "-special untouched"}')
        out = self.call(text=text)["text"]
        self.assertIn('"PrivateFixture" "keep untouched"', out)
        self.assertIn('"42" {"LaunchOptions" "-special untouched"}', out)
        self.assertIn('-windowed -ResX=2560 -ResY=1600 -dx12 -log -ini:Engine:[Core.Log]:LogTemp=Verbose \\\"quoted arg\\\"', out)
        for conflicting in ["-dx11", "-d3d12", "-sm5", "999", "777", "-fullscreen", "-nojoy", "640", "480"]:
            self.assertNotIn(conflicting, out)
        self.assertEqual(self.call(text=out)["text"], out)

    def test_creates_only_missing_keys_once(self):
        out = self.call(text=self.vdf('"2073850" {"Other" "keep"}'))["text"]
        self.assertIn('"Other" "keep"', out)
        self.assertEqual(out.count('"LaunchOptions"'), 1)
        self.assertEqual(self.call(text=out)["text"], out)
        out = self.call(text=self.vdf('"42" {}'))["text"]
        self.assertIn('"2073850"', out)
        self.assertIn('-windowed -ResX=2560 -ResY=1600 -dx12', out)

    def test_preserves_crlf_comments_case_and_trailing_space_option(self):
        text = self.vdf('// untouched comment\n"2073850" {"launchoptions" "-log "}').replace("\n", "\r\n")
        out = self.call(text=text)["text"]
        self.assertIn('// untouched comment\r\n', out)
        self.assertIn('"launchoptions"', out)
        self.assertNotIn("\n", out.replace("\r\n", ""))
        self.assertEqual(self.call(text=out)["text"], out)

    def test_rejects_ambiguous_or_malformed_config_without_guessing(self):
        for text in [self.vdf('"2073850" {"LaunchOptions" "-log" "LaunchOptions" "-dx12"}'),
                     self.vdf('"2073850" {"LaunchOptions" "-log \\\"unfinished"}'),
                     '"UserLocalConfigStore" { "Software" "scalar not object" }',
                     self.vdf('"2073850" {} "2073850" {}'), '"unterminated']:
            with self.subTest(text=text):
                self.assertEqual(self.call(text=text), {"error": "invalid"})

    def config_path(self):
        path = self.base / "environment/drive_c/Program Files (x86)/Steam/userdata/123/config/localconfig.vdf"
        path.parent.mkdir(parents=True)
        return path

    def test_refuses_live_session_then_backs_up_and_updates_closed_config(self):
        path = self.config_path()
        before = self.vdf('"2073850" {"LaunchOptions" "-log"}')
        path.write_text(before)
        self.assertEqual(self.call(op="configure", root=str(self.base), active=True),
                         {"valid": True, "needs": True, "configured": False})
        self.assertEqual(path.read_text(), before)
        self.assertFalse(Path(str(path)+".macfinals-backup").exists())
        self.assertTrue(self.call(op="configure", root=str(self.base))["configured"])
        backup = Path(str(path)+".macfinals-backup")
        self.assertEqual(backup.read_text(), before)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.call(op="configure", root=str(self.base)),
                         {"valid": True, "needs": False, "configured": True})

    def test_refuses_symlink_config_and_backup(self):
        path = self.config_path()
        outside = self.base / "outside.txt"
        before = self.vdf('"2073850" {}')
        outside.write_text(before)
        path.symlink_to(outside)
        result = self.call(op="configure", root=str(self.base))
        self.assertFalse(result["valid"])
        self.assertFalse(result["configured"])
        self.assertEqual(outside.read_text(), before)
        path.unlink();path.write_text(before)
        Path(str(path)+".macfinals-backup").symlink_to(outside)
        self.assertFalse(self.call(op="configure", root=str(self.base))["configured"])
        self.assertEqual(path.read_text(), before)
        self.assertEqual(outside.read_text(), before)

    def test_game_profile_section_takes_the_chosen_canvas(self):
        text = (ROOT / "config/dxmt-source60-1200.conf").read_text()
        out = self.call(op="profile", text=text)["text"]
        self.assertIn("[Discovery.exe]", out)
        self.assertNotIn("[Overwatch.exe]", out)
        self.assertIn("dxgi.fullscreenCanvasWidth = 2560", out)
        self.assertIn("dxgi.fullscreenCanvasHeight = 1600", out)


if __name__ == "__main__":
    unittest.main()
