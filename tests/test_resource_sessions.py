"""Production collector state machine, filesystem rotation and launch contract."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import measure_resources as resources


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        assert 0 <= duration <= 2
        self.now += duration


class Sink:
    def __init__(self):
        self.records = []

    def write(self, record):
        self.records.append(record)


SERVER = dict(pid=17, start_token="Fri Sep 11 20:00:00 2026", executable="/engine/bin/wineserver")


def game(pid, start="first"):
    return dict(pid=pid, start_token=start, engine="local", rss_mib=5)


def sample(games, complete=True):
    return dict(**resources.timestamp(), schema_version=2, games=games, process_scan_complete=complete)


class CollectorLifecycle(unittest.TestCase):
    def test_admission_retries_transient_prefix_visibility_without_changing_identity(self):
        clock = Clock()
        with patch.object(resources, "process_identity", return_value=SERVER), \
             patch.object(resources, "server_matches_prefix", side_effect=[False, subprocess.TimeoutExpired("lsof", 3), True]):
            identity, details = resources.admit_session(SERVER["pid"], SERVER["start_token"], SERVER["executable"],
                Path("/prefix"), monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(identity, SERVER)
        self.assertEqual(details["attempts"], 3)
        self.assertEqual(details["reason"], "matched")
        self.assertTrue(details["prefix_matches"])

    def test_admission_identity_change_is_immediate_and_never_retried(self):
        for change in ({"start_token": "later"}, {"executable": "/different/wineserver"}):
            clock = Clock()
            with patch.object(resources, "process_identity", side_effect=[SERVER | change, SERVER]) as identity, \
                 patch.object(resources, "server_matches_prefix") as prefix:
                with self.assertRaises(resources.SessionAdmissionError) as error:
                    resources.admit_session(SERVER["pid"], SERVER["start_token"], SERVER["executable"], Path("/prefix"),
                                           monotonic=clock.monotonic, sleep=clock.sleep)
            self.assertEqual(identity.call_count, 1)
            prefix.assert_not_called()
            self.assertEqual(error.exception.details["reason"], "process_identity_changed")
            self.assertEqual(clock.now, 0)

    def test_admission_wrong_prefix_expires_and_discloses_exact_failed_check(self):
        clock = Clock()
        with patch.object(resources, "process_identity", return_value=SERVER), \
             patch.object(resources, "server_matches_prefix", return_value=False):
            with self.assertRaises(resources.SessionAdmissionError) as error:
                resources.admit_session(SERVER["pid"], SERVER["start_token"], SERVER["executable"], Path("/prefix"),
                                       monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(clock.now, 5)
        self.assertTrue(error.exception.details["start_matches"])
        self.assertTrue(error.exception.details["executable_matches"])
        self.assertFalse(error.exception.details["prefix_matches"])

    def run_capture(self, states, **kwargs):
        clock, sink = Clock(), Sink()
        samples = iter(states)
        result = resources.collect(sink, sample_fn=lambda *args: next(samples),
                                   monotonic=clock.monotonic, sleep=clock.sleep, **kwargs)
        return sink.records, result, clock.now

    def test_session_rearms_and_stops_only_when_exact_server_exits(self):
        alive = iter([True] * 5 + [False])
        records, result, elapsed = self.run_capture(
            [sample([]), sample([game(31)]), sample([]), sample([game(42)]), sample([game(42)])],
            session=SERVER, alive_fn=lambda _: next(alive), session_seconds=100)
        self.assertEqual(result["reason"], "wine_session_ended")
        self.assertEqual(elapsed, 10)
        self.assertEqual([(r["event"], r["game"]["pid"]) for r in records if "game" in r],
                         [("game_started", 31), ("game_exited", 31), ("game_started", 42)])
        self.assertTrue(all(r["session"] == SERVER and "time" in r and "monotonic_ns" in r for r in records))

    def test_game_pid_reuse_is_a_new_lifecycle_and_incomplete_scan_is_not_exit(self):
        records, result, _ = self.run_capture(
            [sample([game(31)]), sample([], False), sample([game(31, "second")])],
            session=SERVER, alive_fn=lambda _: True, session_seconds=6)
        self.assertEqual([r["event"] for r in records if "game" in r],
                         ["game_started", "game_exited", "game_started"])
        self.assertEqual(result["reason"], "session_duration_reached")

    def test_legacy_wait_still_stops_first_game_exit(self):
        records, result, elapsed = self.run_capture(
            [sample([]), sample([game(31)]), sample([])], seconds=20, wait_for_game=10)
        self.assertEqual(result["reason"], "captured_game_exited")
        self.assertEqual(elapsed, 4)
        self.assertEqual(result["samples"], 3)

    def test_legacy_wait_timeout_and_direct_duration(self):
        for kwargs, reason in ((dict(seconds=20, wait_for_game=4), "game_wait_expired"),
                               (dict(seconds=4), "duration_reached")):
            _, result, elapsed = self.run_capture([sample([])] * 2, **kwargs)
            self.assertEqual((result["reason"], elapsed), (reason, 4))

    def test_failed_identity_reads_remain_bounded_without_false_exit(self):
        clock, sink = Clock(), Sink()
        alive = Mock(side_effect=subprocess.TimeoutExpired("ps", 3))
        get_sample = Mock()
        result = resources.collect(sink, session=SERVER, session_seconds=6, sample_fn=get_sample,
                                   alive_fn=alive, monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(result["reason"], "session_duration_reached")
        self.assertEqual(clock.now, 6)
        self.assertEqual(alive.call_count, 3)
        get_sample.assert_not_called()

    def test_sampling_error_is_redacted_and_does_not_lose_follow_mode(self):
        clock, sink = Clock(), Sink()
        fetch = Mock(side_effect=[OSError("sensitive token"), sample([game(42)])])
        result = resources.collect(sink, session=SERVER, session_seconds=4, sample_fn=fetch,
                                   alive_fn=lambda _: True, monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(result["samples"], 2)
        self.assertNotIn("sensitive", json.dumps(sink.records))
        self.assertEqual(sink.records[0]["error"], "OSError")

    def test_server_pid_reuse_is_not_same_session(self):
        with patch.object(resources, "process_identity", return_value=SERVER | {"start_token": "later"}):
            self.assertFalse(resources.session_alive(SERVER))


class CollectorFiles(unittest.TestCase):
    def test_follow_cli_writes_final_status_and_releases_lease(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(resources, "ROOT", Path(temp)):
            root = Path(temp)
            prefix, engine = root / "runtime/prefix", root / "runtime/engine"
            prefix.mkdir(parents=True)
            output = root / "logs/measured/resources.jsonl"
            server = SERVER | {"executable": str((engine / "bin/wineserver").resolve())}
            clock = Clock()
            original_collect = resources.collect
            def finite_collect(stream, **kwargs):
                kwargs['sample_fn'] = lambda *args: sample([])
                return original_collect(stream, **kwargs,
                                        alive_fn=lambda _: True, monotonic=clock.monotonic, sleep=clock.sleep)
            argv = ["measure_resources.py", "--follow-session", "--session-pid", "17",
                    "--session-start", server["start_token"], "--prefix", str(prefix),
                    "--engine", "engine", "--output", str(output), "--session-seconds", "4"]
            with patch.object(resources, "ENGINES", [engine]), patch.object(sys, "argv", argv), \
                 patch.object(resources, "process_identity", side_effect=lambda pid: server if pid == 17 else {"pid": pid}), \
                 patch.object(resources, "server_matches_prefix", return_value=True), \
                 patch.object(resources, "collect", side_effect=finite_collect):
                resources.main()
            status = json.loads((output.parent / "session-status.json").read_text())
            records = [json.loads(line) for line in output.read_text().splitlines()]
            self.assertEqual(status["status"], "complete")
            self.assertEqual(status["reason"], "session_duration_reached")
            self.assertEqual(status["samples"], 2)
            self.assertEqual(status["session"], server)
            self.assertEqual(records[-1]["event"], "capture_complete")
            lease = resources.SessionLease(prefix)
            try:
                self.assertTrue(lease.acquire())
            finally:
                lease.close()

    def test_rotation_keeps_only_bounded_complete_recent_records(self):
        with tempfile.TemporaryDirectory() as temp:
            stream = resources.RotatingJSONL(Path(temp) / "resources.jsonl", max_bytes=55, backups=3)
            for sequence in range(40):
                stream.write(dict(sequence=sequence))
            stream.close()
            parts = [stream.part(i) for i in range(3, -1, -1)]
            self.assertTrue(all(p.stat().st_size <= 55 for p in parts))
            values = [json.loads(line)["sequence"] for p in parts for line in p.read_text().splitlines()]
            self.assertEqual(values, list(range(values[0], 40)))
            self.assertGreater(values[0], 0)
            self.assertGreater(stream.rotations, 3)
            self.assertEqual(stream.records, 40)

    def test_existing_part_and_dangling_symlink_are_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "resources.jsonl"
            old = Path(temp) / "resources.1.jsonl"
            old.write_text("keep")
            with self.assertRaises(FileExistsError):
                resources.RotatingJSONL(path)
            self.assertEqual(old.read_text(), "keep")
            old.unlink()
            old.symlink_to(Path(temp) / "missing")
            with self.assertRaises(FileExistsError):
                resources.RotatingJSONL(path)
            self.assertTrue(old.is_symlink())

    def test_oversized_record_is_rejected_before_partial_write(self):
        with tempfile.TemporaryDirectory() as temp:
            stream = resources.RotatingJSONL(Path(temp) / "resources.jsonl", max_bytes=15)
            with self.assertRaises(ValueError):
                stream.write(dict(data="x" * 50))
            stream.close()
            self.assertEqual(stream.path.read_bytes(), b"")

    def test_one_lease_per_bottle_releases_without_unlinking_inode(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(resources, "ROOT", Path(temp)):
            first, duplicate = resources.SessionLease(Path(temp) / "prefix"), resources.SessionLease(Path(temp) / "prefix")
            unrelated = resources.SessionLease(Path(temp) / "other")
            try:
                self.assertTrue(first.acquire())
                inode = first.path.stat().st_ino
                self.assertFalse(duplicate.acquire())
                self.assertTrue(unrelated.acquire())
                first.close()
                self.assertTrue(duplicate.acquire())
                self.assertEqual(duplicate.path.stat().st_ino, inode)
            finally:
                first.close()
                duplicate.close()
                unrelated.close()

    def test_status_is_complete_json_after_replacement(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "status.json"
            resources.atomic_json(path, dict(status="running"))
            resources.atomic_json(path, dict(status="complete"))
            self.assertEqual(json.loads(path.read_text()), dict(status="complete"))
            self.assertEqual(list(Path(temp).glob("*.tmp-*")), [])


class AttributionAndLauncher(unittest.TestCase):

    def test_same_engine_other_bottle_does_not_match(self):
        with tempfile.TemporaryDirectory() as temp:
            engine = Path(temp) / "engine"
            prefix = Path(temp) / "prefix"
            prefix.mkdir()
            expected = str(engine / "bin/wineserver")
            wanted = resources.server_directory_name(prefix)
            def command(*args):
                if args == ("ps", "-axo", "pid=,comm="):
                    return f"17 {expected}\n18 {expected}\n"
                if args[0] == "ps":
                    return "Fri Sep 11 20:00:00 2026 " + expected
                if args[0] == "lsof":
                    return "n/private/tmp/.wine-501/" + (wanted if "18" in args else "server-other")
                self.fail(args)
            with patch.object(resources, "command", side_effect=command):
                identity = resources.find_session_server(engine, prefix)
            self.assertEqual(identity["pid"], 18)










if __name__ == "__main__":
    unittest.main()
