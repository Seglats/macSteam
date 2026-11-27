"""macSteam's own Steam on 1x and 2x main displays: Wine's Retina mode, Steam's scaling and the
games' largest resolution follow the main display's scale (portable_steam.h mainDisplayIsRetina,
configureScaling; portable_preferences.h fittedResolution).

On a 1x main display (a 3840x1600 monitor beside a MacBook) Retina mode doubled every display, so
games drew four times the pixels the monitor shows (2026-11-27). Compiles the real functions with
a fake main display in place of CoreGraphics' and a fake registry tool; touches nothing real."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    """The function starting at `marker`, through its closing brace."""
    start = source.index(marker)
    depth, i = 0, source.index('{', start)
    while True:
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        i += 1
        if depth == 0:
            return source[start:i] + '\n'


class OwnSteamDisplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='own-steam-display-')
        cls.addClassCleanup(cls.build.cleanup)
        steam = (ROOT / 'scripts/portable_steam.h').read_text()
        preferences = (ROOT / 'scripts/portable_preferences.h').read_text()
        source = r'''#import <Foundation/Foundation.h>
// The main display, from the test: its mode in points and in pixels (pixels = 2x points: HiDPI).
typedef const void *CGDisplayModeRef;
static long fakeWidth, fakeHeight, fakePixelWidth;
static int fakeMode;
#define CGMainDisplayID() 1
#define CGDisplayCopyDisplayMode(display) ((CGDisplayModeRef)&fakeMode)
#define CGDisplayModeGetWidth(mode) fakeWidth
#define CGDisplayModeGetHeight(mode) fakeHeight
#define CGDisplayModeGetPixelWidth(mode) fakePixelWidth
#define CGDisplayModeRelease(mode) (void)0
static NSString *scaling;
static BOOL registryValue(NSString *engine, NSString *key, NSString *name, NSString *type, NSString *data, NSString *log) {
    (void)engine;(void)key;(void)type;(void)log;
    if([name isEqual:@"LogPixels"]) scaling=data;
    return YES;
}
''' + block(steam, 'static BOOL mainDisplayIsRetina(void) {') + block(steam, 'static void configureScaling(NSString *engine) {') \
    + block(preferences, 'static BOOL gameResolution(') + block(preferences, 'static void fitResolution(') \
    + block(preferences, 'static void fittedResolution(') + r'''
int main(int argc, const char **argv) { @autoreleasepool {
    if(argc!=6) return 2;
    fakeWidth=atol(argv[1]);fakeHeight=atol(argv[2]);fakePixelWidth=atol(argv[3]);
    NSInteger width=atol(argv[4]),height=atol(argv[5]);
    configureScaling(@"engine");
    fittedResolution(&width,&height);
    NSData *data=[NSJSONSerialization dataWithJSONObject:@{@"retina":@(mainDisplayIsRetina()),@"scaling":scaling ?: @"",
                                                          @"width":@(width),@"height":@(height)} options:0 error:nil];
    fwrite(data.bytes,1,data.length,stdout);return 0;
} }
'''
        path = Path(cls.build.name) / 'display.m'
        path.write_text(source)
        cls.binary = Path(cls.build.name) / 'display'
        subprocess.run(['/usr/bin/clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(path),
                        '-framework', 'Foundation', '-o', str(cls.binary)], check=True, capture_output=True)

    def run_display(self, points, pixel_width, choice, scale=None):
        env = {k: v for k, v in os.environ.items() if k != 'SETUP_DISPLAY_SCALE'}
        if scale: env['SETUP_DISPLAY_SCALE'] = scale
        out = subprocess.run([str(self.binary), str(points[0]), str(points[1]), str(pixel_width), str(choice[0]), str(choice[1])],
                             capture_output=True, text=True, check=True, env=env).stdout
        return json.loads(out)

    def test_a_1x_monitor_gets_no_retina_mode_96_dpi_and_games_up_to_its_own_size(self):
        """The 3840x1600 monitor: the game chose 7680x3200 (Wine's doubled size) and gets the monitor's."""
        out = self.run_display((3840, 1600), 3840, (7680, 3200))
        self.assertEqual(out['retina'], False)
        self.assertEqual(out['scaling'], '96')
        self.assertEqual((out['width'], out['height']), (3840, 1600))

    def test_a_1x_monitor_keeps_a_choice_that_fits(self):
        out = self.run_display((3840, 1600), 3840, (2560, 1080))
        self.assertEqual((out['width'], out['height']), (2560, 1080))

    def test_a_retina_screen_keeps_retina_mode_192_dpi_and_twice_its_size(self):
        """A MacBook screen at 1728x1117 points (3456x2234 pixels), as before."""
        out = self.run_display((1728, 1117), 3456, (3456, 2234))
        self.assertEqual(out['retina'], True)
        self.assertEqual(out['scaling'], '192')
        self.assertEqual((out['width'], out['height']), (3456, 2234))
        too_big = self.run_display((1728, 1117), 3456, (7680, 3200))
        self.assertLessEqual(too_big['width'], 3456)
        self.assertLessEqual(too_big['height'], 2234)

    def test_the_setup_tests_override_wins_over_the_display(self):
        self.assertEqual(self.run_display((3840, 1600), 3840, (1920, 1080), scale='2')['retina'], True)
        self.assertEqual(self.run_display((1728, 1117), 3456, (1920, 1080), scale='1')['retina'], False)


if __name__ == '__main__':
    unittest.main()
