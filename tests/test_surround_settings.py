"""Headphone surround › Settings…: the live preview (scripts/surround_preview.c) and the app's settings
(app/Sources/SurroundSettings.swift): changes heard while playing, Save baking the file Wine loads,
saved values coming back, Reset to Fitted removing the file. Plays silently (gain 0) on the Mac's
output; never touches the real SteamDriver folder."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEAD = ROOT / 'scripts/surround/kemar.head'
CLIP = ROOT / 'scripts/surround/ball-bounce.wav'

APP_HARNESS = r'''
import Foundation
import SwiftUI
enum LauncherStyle { static let accent=Color.accentColor }
// SurroundSettings as the page drives it, in a throwaway home: a[2] is the step.
@main struct H { @MainActor static func main() {
    let a=CommandLine.arguments
    NotProton.home=URL(fileURLWithPath:a[1])
    let resources=URL(fileURLWithPath:a[3]),helpers=URL(fileURLWithPath:a[4])
    let s=SurroundSettings()
    var report:[String:Any]=["front":s.values["front"]!,"changed":s.changed,"fitted":s.isFitted]
    func finish() {
        report["baked"]=FileManager.default.fileExists(atPath:SurroundSettings.bakedFile.path)
        report["json"]=FileManager.default.fileExists(atPath:SurroundSettings.settingsFile.path)
        report["notice"]=s.notice
        s.stop()
        print(String(data:try! JSONSerialization.data(withJSONObject:report),encoding:.utf8)!);exit(0)
    }
    if a[2]=="load" { finish() }
    s.volume=0
    s.start(resources:resources,helpers:helpers)
    var waited=0
    Timer.scheduledTimer(withTimeInterval:0.1,repeats:true) { t in MainActor.assumeIsolated {
        waited+=1
        guard s.phase == .ready || waited>100 else { return }
        t.invalidate()
        report["phase"]="\(s.phase)"
        if a[2]=="save" { s.set("front",60.0) } else { s.resetToFitted() }
        DispatchQueue.main.asyncAfter(deadline:.now()+0.5) {
            report["changed"]=s.changed
            s.save { ok in report["saved"]=ok;finish() }
        }
    } }
    RunLoop.main.run()
}}
'''


class LivePreview(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='surround-preview-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.preview = cls.root / 'Helpers/surround_preview'
        cls.preview.parent.mkdir()
        subprocess.run(['/usr/bin/clang', '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_preview.c', '-framework', 'AudioToolbox',
                        '-o', cls.preview], check=True, capture_output=True)

    def test_settings_are_heard_while_it_plays_and_save_bakes_the_file(self):
        baked = self.root / 'live.sdsurr'
        p = subprocess.Popen([str(self.preview), str(HEAD), str(CLIP), 'gain=0.5'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        lines = []
        threading.Thread(target=lambda: [lines.append(l.strip()) for l in p.stdout], daemon=True).start()

        def say(*commands):
            for c in commands: p.stdin.write(c + '\n')
            p.stdin.flush()

        def wait_for(prefix, limit=10):
            end = time.time() + limit
            while time.time() < end:
                if any(l.startswith(prefix) for l in lines): return True
                time.sleep(0.05)
            return False
        try:
            if not wait_for('ready', 5):
                if any(l.startswith('error') for l in lines): self.skipTest('no audio output: ' + str(lines))
                self.fail(lines)
            self.assertIn('length 36.8', lines)
            say('gain 0', 'play')
            self.assertTrue(wait_for('hit 0.0 1'), lines)
            say('set front=60 el_front=0', 'surround 0', 'surround 1', 'seek 34')
            self.assertTrue(wait_for('hit 270.0 5'), lines)          # on after the jump, still playing
            say(f'bake {baked}')
            self.assertTrue(wait_for('baked'), lines)
            self.assertEqual(baked.stat().st_size, 8 + 12 + 4 * 17 + 12 * 2 * 304 * 4)
            self.assertTrue(wait_for('end', 10), lines)
            say('set nonsense=1')
            self.assertTrue(wait_for('error no knob nonsense'), lines)
        finally:
            say('quit')
            p.stdin.close()
            p.wait(timeout=5)
            p.stdout.close()

    def test_a_file_plays_and_a_bad_one_says_why(self):
        p = subprocess.run([str(self.preview), str(HEAD), str(CLIP)], input=f'file {ROOT / "README.md"}\nfile {CLIP}\nquit\n',
                           capture_output=True, text=True, timeout=20)
        if 'error no audio output' in p.stdout: self.skipTest('no audio output')
        self.assertIn('error not a WAV file', p.stdout)
        self.assertIn('file 1 ', p.stdout)                            # the clip: mono


class AppSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='surround-settings-app-')
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        cls.helpers, cls.resources = root / 'Helpers', root / 'Resources'
        (cls.resources / 'SurroundTest').mkdir(parents=True)
        cls.helpers.mkdir()
        shutil.copy(HEAD, cls.resources / 'SurroundTest/kemar.head')
        shutil.copy(CLIP, cls.resources / 'SurroundTest/ball-bounce.wav')
        subprocess.run(['/usr/bin/clang', '-O2', '-I', ROOT / 'scripts', ROOT / 'scripts/surround_preview.c', '-framework', 'AudioToolbox',
                        '-o', cls.helpers / 'surround_preview'], check=True, capture_output=True)
        (root / 'harness.swift').write_text(APP_HARNESS)
        cls.harness = root / 'harness'
        sources = ['SurroundSettings', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Migration', 'Brand', 'InstallLocation', 'Surround', 'GameTools', 'CrossOverLicense', 'ControllerSupport']
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{s}.swift') for s in sources], str(root / 'harness.swift'),
                        '-o', str(cls.harness)], check=True, capture_output=True)
        cls.home = root / 'home'

    def step(self, name):
        out = subprocess.run([str(self.harness), str(self.home), name, str(self.resources), str(self.helpers)],
                             capture_output=True, text=True, timeout=60).stdout
        return json.loads(out)

    def test_save_keeps_the_settings_for_games_and_reset_removes_them(self):
        first = self.step('load')
        self.assertEqual((first['fitted'], first['baked'], first['json']), (True, False, False))
        saved = self.step('save')
        if 'error' in saved.get('notice', '').lower() or saved.get('phase') != 'ready': self.skipTest(f'no audio output: {saved}')
        self.assertEqual((saved['changed'], saved['saved'], saved['baked'], saved['json']), (True, True, True, True))
        folder = self.home / 'Library/Application Support/SteamDriver'
        self.assertEqual(json.loads((folder / 'surround-settings.json').read_text())['front'], 60)
        self.assertEqual((folder / 'surround-settings.sdsurr').read_bytes()[:7], b'SDSURR1')
        again = self.step('load')                                      # the next start: the saved values
        self.assertEqual((again['front'], again['fitted']), (60, False))
        reset = self.step('reset')
        self.assertEqual((reset['saved'], reset['baked'], reset['json']), (True, False, False))   # fitted: Wine's built-in filters
        self.assertTrue(self.step('load')['fitted'])


if __name__ == '__main__':
    unittest.main()
