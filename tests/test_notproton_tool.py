"""MacVolvo's NotProton tool (app/Resources/NotProton/run.sh) against a fake NotProton tool.

Runs the real script in a throwaway HOME; never touches Steam, NotProton or Wine."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = (ROOT / 'app/Resources/NotProton/run.sh').read_text()
# What NotProton's compat.c written_here() looks for before removing a notproton-* tool folder.
NOTPROTON_MARK = 'notproton CrossOver compatibility tool shim'
FAKE_RUN = r'''#!/bin/sh
# notproton CrossOver compatibility tool shim
/usr/bin/python3 -c 'import json,os,sys; print(json.dumps({"args":sys.argv[1:],"env":{k:v for k,v in os.environ.items() if k.startswith(("WINEMAC_","NOTPROTON_"))}}))' "$@"
'''


class NotProtonTool(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='macvolvo-notproton-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        tools = self.home / 'Library/Application Support/Steam/compatibilitytools.d'
        fake = tools / 'notproton-26.3'
        fake.mkdir(parents=True)
        (fake / 'run').write_text(FAKE_RUN)
        (fake / 'run').chmod(0o755)
        self.ours = tools / 'notproton-macvolvo-26.3'
        self.ours.mkdir()
        run = self.ours / 'run'
        run.write_text(TEMPLATE.replace('@NP_TOOL@', 'notproton-26.3').replace('@VARIANT@', 'macvolvo-26.3.0.39832'))
        run.chmod(0o755)
        self.settings = self.home / 'Library/Application Support/MacVolvo NotProton/settings'
        self.settings.parent.mkdir(parents=True)

    def launch(self, *args, env=None, code=0):
        clean = {'HOME': str(self.home), 'PATH': '/usr/bin:/bin'} | (env or {})
        result = subprocess.run([str(self.ours / 'run'), *args], env=clean, capture_output=True, text=True)
        self.assertEqual(result.returncode, code, result.stderr)
        return json.loads(result.stdout) if code == 0 else result

    def test_adds_the_variant_and_settings_then_runs_notproton_unchanged(self):
        self.settings.write_text('WINEMAC_MOUSELOOK=*;!steam.exe\nWINEMAC_INPUT_VARIANT=precise\nWINEMAC_RAW_SOURCE=hid\n'
                                 'WINEMAC_MOUSELOOK_RAW_INTERVAL_US=0\n')
        out = self.launch('waitforexitandrun', '/games/game.exe', '-flag')
        self.assertEqual(out['args'], ['waitforexitandrun', '/games/game.exe', '-flag'])
        self.assertEqual(out['env'], {'NOTPROTON_RUNNER_VARIANT': 'macvolvo-26.3.0.39832',
                                      'WINEMAC_MOUSELOOK': '*;!steam.exe', 'WINEMAC_INPUT_VARIANT': 'precise',
                                      'WINEMAC_RAW_SOURCE': 'hid', 'WINEMAC_MOUSELOOK_RAW_INTERVAL_US': '0'})

    def test_keeps_a_variant_chosen_for_the_game_and_never_doubles_its_own(self):
        out = self.launch('run', 'x.exe', env={'NOTPROTON_RUNNER_VARIANT': 'dxmt-example'})
        self.assertEqual(out['env']['NOTPROTON_RUNNER_VARIANT'], 'dxmt-example+macvolvo-26.3.0.39832')
        out = self.launch('run', 'x.exe', env={'NOTPROTON_RUNNER_VARIANT': 'macvolvo-26.3.0.39832+dxmt-example'})
        self.assertEqual(out['env']['NOTPROTON_RUNNER_VARIANT'], 'macvolvo-26.3.0.39832+dxmt-example')

    def test_the_games_launch_options_win_and_only_known_safe_lines_are_read(self):
        self.settings.write_text('WINEMAC_INPUT_VARIANT=precise\nWINEMAC_RAW_SOURCE=hid\nDYLD_INSERT_LIBRARIES=/tmp/x.dylib\n'
                                 'WINEMAC_MOUSELOOK=$(touch /tmp/pwned)\n# comment\n\nWINEPREFIX=/tmp/elsewhere\n')
        out = self.launch('run', 'x.exe', env={'WINEMAC_INPUT_VARIANT': 'classic'})
        self.assertEqual(out['env']['WINEMAC_INPUT_VARIANT'], 'classic')
        self.assertEqual(out['env']['WINEMAC_RAW_SOURCE'], 'hid')
        self.assertNotIn('WINEMAC_MOUSELOOK', out['env'])

    def test_other_verbs_pass_straight_through(self):
        self.settings.write_text('WINEMAC_RAW_SOURCE=hid\n')
        out = self.launch('getcompatpath', '/games')
        self.assertEqual(out, {'args': ['getcompatpath', '/games'], 'env': {}})

    def test_without_notproton_tool_it_says_so(self):
        (self.home / 'Library/Application Support/Steam/compatibilitytools.d/notproton-26.3/run').unlink()
        result = self.launch('run', 'x.exe', code=1)
        self.assertIn('Open NotProton', result.stderr)

    def test_notproton_does_not_take_the_folder_for_its_own(self):
        # compat.c removes an unlisted notproton-* folder only when it has a flavor file or a run
        # script whose first 127 bytes carry NotProton's mark.
        self.assertFalse((self.ours / 'flavor').exists())
        self.assertNotIn(NOTPROTON_MARK, (self.ours / 'run').read_bytes()[:127].decode())
        self.assertNotIn(NOTPROTON_MARK, TEMPLATE)


if __name__ == '__main__':
    unittest.main()
