"""Benchmarks (app/Sources/Benchmarks.swift, docs/BENCHMARKS.md) on made-up logs.

Compiles the real Benchmarks.swift with a small driver and checks the Metal HUD parsing, the
summary formulas, the present-to-display latency from both renderers' logs, a whole run from a
session, and the run script's recording switch. Never touches Steam, Wine or the player's Library."""
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUN_SCRIPT = ROOT / 'environments/finals/compat-tool/run'
HARNESS = r'''
import Foundation
@main struct Harness { static func main() {
    let a=CommandLine.arguments; NotProton.home=URL(fileURLWithPath:a[1])
    var out:[String:Any]=[:]
    func nums(_ s:String)->[Double] { s.split(separator:",").compactMap { Double($0) } }
    func summary(_ s:Benchmarks.Summary)->[String:Any] {
        let data=try! JSONEncoder().encode(s);return try! JSONSerialization.jsonObject(with:data) as! [String:Any]
    }
    switch a[2] {
    case "hud":
        let text=try! String(contentsOfFile:a[3],encoding:.utf8)
        out["frames"]=Benchmarks.hudFrames(text).mapValues { $0.map { [$0.end,$0.interval,$0.gpu] } }
    case "summary": out["summary"]=summary(Benchmarks.summarize(intervals:nums(a[3]),gpu:nums(a[4]),latency:nums(a[5])))
    case "latency": out["latency"]=Benchmarks.presentLatencies(URL(fileURLWithPath:a[3]),from:Double(a[4])!,to:Double(a[5])!).sorted()
    case "analyse":
        let session=Benchmarks.Session(dir:a[4]=="-" ? nil : URL(fileURLWithPath:a[4]),appID:"2767030",game:"Marvel Rivals",source:"SteamDriver",
                                       renderer:"d3dmetal",runtime:"macfinals-191",upscaling:true,log:URL(fileURLWithPath:a[3]),logOffset:UInt64(a[5])!)
        do {
            let run=try Benchmarks.analyse(session,startUnix:Double(a[6])!,stopUnix:Double(a[7])!,startHost:Double(a[8])!,stopHost:Double(a[9])!,refreshHz:120)
            try Benchmarks.save(run)
            out["process"]=run.process;out["summary"]=summary(run.summary);out["settings"]=Benchmarks.settingsLine(run)
            out["saved"]=Benchmarks.runs().map(\.id)
        } catch { out["error"]=error.localizedDescription }
    case "hotkey":
        if a.count>3 {
            do { try Benchmarks.setHotkey(a[3]) } catch { out["error"]=error.localizedDescription }
        }
        out["hotkey"]=Benchmarks.hotkey();out["title"]=Benchmarks.hotkeyTitle
        out["file"]=(try? String(contentsOf:Benchmarks.hotkeyFile,encoding:.utf8)) ?? ""
    case "record":
        if a.count>3 { try! Benchmarks.setRecord(a[3]=="off" ? .off : a[3]=="all" ? .all : .games(Set(a[3].split(separator:",").map(String.init)))) }
        out["record"]="\(Benchmarks.record())"
        out["file"]=(try? String(contentsOf:Benchmarks.recordFile,encoding:.utf8)) ?? ""
    default: break
    }
    print(String(data:try! JSONSerialization.data(withJSONObject:out),encoding:.utf8)!)
}}
'''


def hud_line(unix, process, pid, counter, frames):
    """A Metal HUD line as the game's stderr gets it (local time, like NSLog)."""
    stamp = datetime.fromtimestamp(unix).strftime('%Y-%m-%d %H:%M:%S.') + f'{int(round(unix % 1 * 1000)) % 1000:03d}'
    values = ','.join(f'{i:.2f},{g:.2f}' for i, g in frames)
    return f'{stamp} {process}[{pid}:4785841] metal-HUD: {counter},5.59,332.91,{values}\n'


class Benchmarks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='steamdriver-bench-build-')
        cls.addClassCleanup(cls.build.cleanup)
        folder = Path(cls.build.name)
        (folder / 'harness.swift').write_text(HARNESS)
        cls.tool = folder / 'harness'
        sources = ['Benchmarks', 'NotProton', 'NotProtonExtras', 'Profiles', 'KeyOverrides', 'Migration', 'Brand', 'InstallLocation', 'Surround']
        subprocess.run(['xcrun', 'swiftc', '-parse-as-library', *[str(ROOT / f'app/Sources/{s}.swift') for s in sources],
                        str(folder / 'harness.swift'), '-o', str(cls.tool)], check=True, capture_output=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='steamdriver-bench-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        (self.home / 'Library/Application Support').mkdir(parents=True)

    def run_tool(self, *args):
        result = subprocess.run([str(self.tool), str(self.home), *map(str, args)], capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def test_hud_lines_become_frames_ending_at_the_line_time(self):
        log = self.root / 'wine.log'
        t = 1795676000.5
        log.write_text('0024:err:something unrelated\n'
                       + hud_line(t, 'Marvel-Win64-Shipping.exe', 812, 3, [(10, 4), (20, 5), (10, 6)])
                       + hud_line(t + 0.7, 'steamwebhelper', 900, 1, [(16.67, 1)])
                       + 'garbage metal-HUD: line without a prefix\n')
        frames = self.run_tool('hud', log)['frames']
        self.assertEqual(sorted(frames), ['Marvel-Win64-Shipping.exe[812]', 'steamwebhelper[900]'])
        game = frames['Marvel-Win64-Shipping.exe[812]']
        self.assertEqual([f[1:] for f in game], [[10, 4], [20, 5], [10, 6]])
        # The last frame ends at the line's time, each earlier one an interval before the next.
        self.assertAlmostEqual(game[2][0], t, places=3)
        self.assertAlmostEqual(game[1][0], t - 0.010, places=3)
        self.assertAlmostEqual(game[0][0], t - 0.030, places=3)

    def test_summary_formulas(self):
        intervals = [10.0] * 99 + [50.0]
        gpu = [9.5] * 50 + [2.0] * 50
        s = self.run_tool('summary', ','.join(map(str, intervals)), ','.join(map(str, gpu)), '20,30,40,50')['summary']
        self.assertEqual(s['frames'], 100)
        self.assertAlmostEqual(s['seconds'], 1.04)
        self.assertAlmostEqual(s['avgFPS'], 100 / 1.04, places=6)
        self.assertAlmostEqual(s['low1FPS'], 20.0)        # the worst 1 % (one frame) averages 50 ms
        self.assertAlmostEqual(s['low01FPS'], 20.0)       # at least one frame
        self.assertEqual(s['p50'], 10.0)
        self.assertEqual(s['maxMs'], 50.0)
        self.assertEqual(s['stutters'], 1)                # over twice the median
        self.assertAlmostEqual(s['gpuBound'], 0.5)        # GPU time >= 90 % of the interval
        self.assertEqual(s['latencyFrames'], 4)
        self.assertAlmostEqual(s['latencyP50'], 35.0)     # linear interpolation between ranks
        self.assertNotIn('latencyP95', {k for k, v in s.items() if v is None})

    def test_summary_without_latency_leaves_it_out(self):
        s = self.run_tool('summary', '10,10', '1,1', '')['summary']
        self.assertEqual(s['latencyFrames'], 0)
        self.assertNotIn('latencyP50', s)

    def test_d3dmetal_present_latency_in_the_window(self):
        d = self.root / 'session'
        d.mkdir()
        (d / 'd3dm-frames-812.csv').write_text(
            '# t0 1000.000000 (CACurrentMediaTime s); times in ms after t0, presented -1 = never shown\n'
            'requested,acquired,handed,target,presented,how,sync,transaction,drawables,width,height\n'
            '0.000,0.5,1.0,0,25.000,p,0,0,3,2560,1440\n'       # before the window
            '1000.000,1000.5,1001.0,0,1021.500,p,0,0,3,2560,1440\n'
            '1010.000,1010.5,1011.0,0,-1,p,0,0,3,2560,1440\n'  # never shown: left out
            '1020.000,1020.5,1021.0,0,1050.000,p,0,0,3,2560,1440\n'
            '# 2 frames lost (ring full)\n')
        self.assertEqual(self.run_tool('latency', d, 1000.5, 1002)['latency'], [21.5, 30.0])

    def test_dxmt_present_latency_joins_requests_and_presents_across_segments(self):
        d = self.root / 'session'
        d.mkdir()
        head = ('schema_version,event,unix_ms,native_pid,sequence,layer_ptr,drawable_id,command_ptr,request_host_s,event_host_s,'
                'minimum_duration_s,presented_s,presented_state,gpu_start_s,gpu_end_s,gpu_state,command_status,error_code,'
                'callbacks_seen,dropped_events,dropped_requests,active_traces\n')

        def row(event, seq, req, presented):
            return f'1,{event},0,77,{seq},0x1,1,0x2,{req},{req},0,{presented},valid,0,0,valid,4,0,1,0,0,1\n'
        (d / 'display-77.1.csv').write_text(head + row('request', 1, 500.000, 0) + row('request', 2, 500.010, 0))
        (d / 'display-77.csv').write_text(head + row('presented', 1, 500.000, 500.020) + row('presented', 2, 500.010, 0)
                                          + row('request', 3, 600.0, 0) + row('presented', 3, 600.0, 600.008))
        (d / 'display-77.hitches.csv').write_text('schema_version,native_pid\n1,77\n')
        latency = self.run_tool('latency', d, 499, 501)['latency']
        self.assertEqual(len(latency), 1)                 # seq 2 never shown, seq 3 outside
        self.assertAlmostEqual(latency[0], 20.0, places=3)

    def test_a_run_reads_only_the_window_and_the_busiest_process(self):
        log, d = self.root / 'steamdriver-wine.log', self.root / 'session'
        d.mkdir()
        old = hud_line(1795670000.0, 'Marvel-Win64-Shipping.exe', 812, 2, [(5, 1), (5, 1)])
        t = 1795676000.0
        lines = [hud_line(t + k * 0.1, 'Marvel-Win64-Shipping.exe', 812, 10 * (k + 1), [(10, 8)] * 10) for k in range(1, 11)]
        lines.append(hud_line(t + 0.5, 'explorer.exe', 33, 1, [(16, 1)]))
        log.write_text(old + ''.join(lines))
        out = self.run_tool('analyse', log, d, len(old.encode()), t + 0.255, t + 0.855, 0, 1)
        self.assertEqual(out['process'], 'Marvel-Win64-Shipping.exe[812]')
        self.assertEqual(out['summary']['frames'], 60)    # frames ending in (start, stop]
        self.assertAlmostEqual(out['summary']['avgFPS'], 100.0)
        self.assertEqual(out['settings'], 'SteamDriver · D3DMetal · MetalFX upscaling · macfinals-191 · 120 Hz display')
        self.assertEqual(len(out['saved']), 1)
        saved = list((self.home / 'Library/Application Support/SteamDriver/benchmarks/runs/2767030').glob('*.json'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(len(json.loads(saved[0].read_text())['intervals']), 60)

    def test_a_run_without_hud_lines_says_why(self):
        log = self.root / 'steamdriver-wine.log'
        log.write_text('0024:trace:loaddll:...\n')
        out = self.run_tool('analyse', log, '-', 0, 0, 2e9, 0, 1)
        self.assertIn('No Metal HUD lines', out['error'])

    def test_record_setting_file(self):
        self.assertEqual(self.run_tool('record'), {'record': 'off', 'file': ''})
        self.assertEqual(self.run_tool('record', '2767030,730')['file'], '2767030\n730\n')
        self.assertEqual(self.run_tool('record', 'all')['file'], 'all\n')
        self.assertEqual(self.run_tool('record', 'off')['file'], '')

    def test_hotkey_defaults_to_control_option_b(self):
        self.assertEqual(self.run_tool('hotkey'), {'hotkey': 'ctrl+alt+b', 'title': '⌃⌥B', 'file': ''})

    def test_a_changed_hotkey_is_saved_and_reset_removes_the_file(self):
        out = self.run_tool('hotkey', 'cmd+shift+f9')
        self.assertEqual((out['hotkey'], out['title'], out['file']), ('cmd+shift+f9', '⇧⌘F9', 'cmd+shift+f9\n'))
        self.assertEqual(self.run_tool('hotkey', 'f13')['hotkey'], 'f13')       # a function key alone is fine
        self.assertEqual(self.run_tool('hotkey', 'ctrl+alt+b'), {'hotkey': 'ctrl+alt+b', 'title': '⌃⌥B', 'file': ''})

    def test_a_hotkey_that_would_take_typing_is_refused(self):
        for combo in ('b', 'shift+b', 'space', 'ctrl+nokey', 'hyper+b'):
            out = self.run_tool('hotkey', combo)
            self.assertIn('error', out, combo)
            self.assertEqual(out['hotkey'], 'ctrl+alt+b', combo)
        # A bad file (edited by hand) falls back to the default.
        f = self.home / 'Library/Application Support/SteamDriver/benchmarks/hotkey'
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text('shift+b\n')
        self.assertEqual(self.run_tool('hotkey')['hotkey'], 'ctrl+alt+b')

    def run_script_block(self, record, env=''):
        """The run script's recording block alone, with made-up launch variables."""
        text = RUN_SCRIPT.read_text()
        block = re.search(r'^    bench=\$\{STEAMDRIVER_BENCH.*?(?=^    "\$WINELOADER" "\$shim")', text, re.S | re.M).group(0)
        sd = self.root / 'SteamDriver'
        (sd / 'benchmarks').mkdir(parents=True, exist_ok=True)
        if record is not None:
            (sd / 'benchmarks/record').write_text(record)
        script = (f"say(){{ print -r -- \"say: $*\"; }}; steamdriver='{sd}' app_id=2767030 game_name='Marvel Rivals' "
                  f"dx11=d3dmetal upscale=0 upscale_via=d3dmetal surround=0 tool=/x/notproton-steamdriver engine=/nonexistent "
                  f"wlog=/tmp/w.log wlog_start=42 known_d3d=direct3d=dx12 {env}\n{block}\n"
                  "env | /usr/bin/grep -E '^(MTL_HUD_[A-Z_]+|WINEMAC_INPUT_STATS|DXMT_DISPLAY_LOG)=' | /usr/bin/sort")
        out = subprocess.run(['/bin/zsh', '-f', '-c', script], capture_output=True, text=True, check=True).stdout
        sessions = sorted((sd / 'benchmarks/sessions').glob('*/session.json'))
        return out, [json.loads(s.read_text()) for s in sessions]

    def test_run_script_records_for_a_chosen_game_with_the_hud_hidden(self):
        out, sessions = self.run_script_block('730\n2767030\n')
        self.assertIn('MTL_HUD_ENABLED=1', out)
        self.assertIn('MTL_HUD_LOG_ENABLED=1', out)
        self.assertIn('MTL_HUD_OPACITY=0', out)
        self.assertIn('WINEMAC_INPUT_STATS=', out)
        self.assertEqual(len(sessions), 1)
        self.assertEqual(sessions[0]['wine_log_offset'], 42)
        self.assertEqual(sessions[0]['renderer'], 'd3dmetal')
        self.assertFalse(sessions[0]['hud_visible'])

    def test_run_script_leaves_other_games_and_the_off_setting_alone(self):
        for record in ('730\n', None):
            out, sessions = self.run_script_block(record)
            self.assertNotIn('MTL_HUD', out)
            self.assertEqual(sessions, [])

    def test_launch_option_wins_and_a_visible_hud_stays_visible(self):
        out, sessions = self.run_script_block(None, 'STEAMDRIVER_BENCH=1 MTL_HUD_ENABLED=1 dx11=dxmt')
        self.assertNotIn('MTL_HUD_OPACITY', out)
        self.assertIn('DXMT_DISPLAY_LOG=', out)
        self.assertTrue(sessions[0]['hud_visible'])
        out, sessions = self.run_script_block('all\n', 'STEAMDRIVER_BENCH=0')
        self.assertNotIn('MTL_HUD', out)


if __name__ == '__main__':
    unittest.main()
