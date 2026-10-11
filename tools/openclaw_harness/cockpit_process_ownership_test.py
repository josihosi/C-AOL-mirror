import json
import io
from contextlib import redirect_stdout
import os
import subprocess
import sys
import time
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import startup_harness as harness
from cockpit_file_bridge import FileBackedCockpitBridge

class ProcessOwnershipTest(unittest.TestCase):
    def test_early_identity_is_persisted_and_retained_before_hud(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp) / "session"
            bridge = FileBackedCockpitBridge(session, ["unused"], binding_id="bound")
            bridge.prepare()
            process = Mock(pid=123)
            command = "/test/cataclysm-tiles --userdir /test/disposable"
            with patch.object(harness, "pid_command", return_value=command):
                harness.record_bridge_game_process(process, {
                    "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": str(session),
                    "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound",
                    "OPENCLAW_HARNESS_RUN_ID": "run"})
                record = json.loads((session / "game-process.json").read_text())
                self.assertEqual(record["pid"], 123)
                self.assertEqual(record["run_id"], "run")
                generations = [json.loads(line) for line in
                               (session / "game-process.generations.jsonl").read_text().splitlines()]
                self.assertEqual(generations[-1]["process_generation"]["pid"], 123)
                retained = bridge._emergency_cleanup()
                self.assertEqual(retained["game"]["status"], "retained_process_identity_unavailable")
            with patch.object(harness, "pid_command", return_value="unrelated process"), patch.object(harness, "cleanup_game_process") as cleanup:
                bridge._startup_progress = {"pid": 123}
                result = bridge._emergency_cleanup()
                cleanup.assert_not_called()
                self.assertEqual(result["ownership"], "process_exited_or_identity_changed")

    def test_process_record_publishes_actual_run_log_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp) / "session"
            run_dir = Path(tmp) / "harness-run"
            run_dir.mkdir()
            session.mkdir()
            process = Mock(pid=123)
            with patch.object(harness, "pid_command", return_value="/test/cataclysm-tiles --userdir /test/disposable"):
                harness.record_bridge_game_process(process, {
                    "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": str(session),
                    "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound",
                    "OPENCLAW_HARNESS_RUN_ID": "run",
                    "OPENCLAW_HARNESS_RUN_DIR": str(run_dir),
                    "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH": str(run_dir / "semantic.native.events.jsonl"),
                    "OPENCLAW_HARNESS_TRANSITION_EVENT_PATH": str(run_dir / "transition.events.jsonl"),
                    "OPENCLAW_HARNESS_PROFILE": "test-profile"})
            record = json.loads((session / "game-process.json").read_text())
            self.assertEqual(record["launch_cwd"], str(harness.repo_root().resolve()))
            self.assertEqual(record["log_paths"]["native_semantic_events"], {
                "path": str(run_dir / "semantic.native.events.jsonl"), "scope": "run_bound"})
            self.assertEqual(record["log_paths"]["transition_events"]["path"],
                             str(run_dir / "transition.events.jsonl"))
            self.assertEqual(record["log_paths"]["profile_diagnostic_debug"]["scope"], "profile_shared")

    def test_relaunch_metadata_uses_operational_snapshot_and_bound_transition_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp) / "session"
            operational = Path(tmp) / "post-relaunch"
            artifact = Path(tmp) / "initial-artifacts"
            session.mkdir()
            operational.mkdir()
            artifact.mkdir()
            env = harness.semantic_request_transport_child_environment(operational, "run")
            env.update({
                "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": str(session),
                "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound",
                "OPENCLAW_HARNESS_RUN_ID": "run",
                "OPENCLAW_HARNESS_RUN_DIR": str(artifact),
                "OPENCLAW_HARNESS_TRANSITION_EVENT_PATH": str(artifact / "transition.events.jsonl"),
            })
            with patch.object(harness, "pid_command", return_value="/test/cataclysm-tiles"):
                harness.record_bridge_game_process(Mock(pid=123), env)
            logs = json.loads((session / "game-process.json").read_text())["log_paths"]
            self.assertEqual(Path(logs["native_semantic_snapshot"]["path"]),
                             (operational / "semantic.native.log").resolve())
            self.assertEqual(Path(logs["native_semantic_events"]["path"]).parent, operational.resolve())
            self.assertEqual(Path(logs["transition_events"]["path"]), artifact / "transition.events.jsonl")

    def test_deferred_scenario_cleanup_requires_the_player_quit_record(self):
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)", "cataclysm-tiles"])
        try:
            # The macOS Python launcher replaces itself once after Popen;
            # record the executable process generation, not that launcher.
            time.sleep(0.1)
            with tempfile.TemporaryDirectory() as temp, redirect_stdout(io.StringIO()):
                run_dir = Path(temp)
                generation = harness.process_generation_snapshot(process.pid)
                (run_dir / "process.json").write_text(json.dumps({
                    "pid": process.pid, "process_generation": generation,
                }), encoding="utf-8")
                disconnected = {"mode":"probe", "steps":[]}
                harness.finalize_probe_report(run_dir, disconnected, cleanup_pid=process.pid)
                self.assertEqual(disconnected["cleanup"]["status"], "retained_waiting_for_player_quit")
                self.assertIsNone(process.poll())
                report = {"termination_requested":True, "run_id":"test-run", "binding_id":"test-binding",
                          "stop_reason":"player_quit", "state":"finished"}
                harness.finalize_cockpit_live_session(run_dir, process.pid, report, cleanup_process=False)
                self.assertIsNone(process.poll())
                ended = {"mode":"probe", "steps":[]}
                with patch.object(harness, "pending_adaptive_semantic_recovery", return_value={"unresolved":"test"}):
                    harness.finalize_probe_report(run_dir, ended, cleanup_pid=process.pid,
                                                 report_filename="explicit-quit.report.json")
                self.assertEqual(ended["cleanup"]["status"], "terminated")
                process.wait(timeout=3)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)

    def test_live_process_survives_cleanup_until_explicit_player_quit(self):
        process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)", "cataclysm-tiles"])
        try:
            time.sleep(0.1)
            unbound = harness.cleanup_game_process(process.pid, explicit_quit=True)
            self.assertEqual(unbound["status"], "retained_process_identity_unavailable")
            self.assertIsNone(process.poll())
            generation = harness.process_generation_snapshot(process.pid)
            retained = harness.cleanup_game_process(
                process.pid, expected_process_generation=generation,
            )
            self.assertEqual(retained["status"], "retained_waiting_for_player_quit")
            self.assertIsNone(process.poll())
            ended = harness.cleanup_game_process(
                process.pid, explicit_quit=True, expected_process_generation=generation,
            )
            self.assertEqual(ended["status"], "terminated")
            process.wait(timeout=3)
            self.assertFalse(ended["native_exit_credit"])
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)

    def test_reused_pid_generation_is_retained_without_signalling(self):
        class ReusedPidInspector:
            def inspect(self, pid):
                return harness.ProcessSnapshot(
                    pid=pid, alive=True, birth_identity="birth-new",
                    command="/test/Cataclysm-AOL --userdir /test/disposable",
                )

        expected = {
            "pid": 123,
            "birth_identity": "birth-old",
            "command": "/test/Cataclysm-AOL --userdir /test/disposable",
        }
        with patch.object(harness.os, "kill") as kill:
            cleanup = harness.cleanup_game_process(
                123, explicit_quit=True, expected_process_generation=expected,
                inspector=ReusedPidInspector(),
            )
        self.assertEqual(cleanup["status"], "retained_process_identity_changed")
        kill.assert_not_called()

    def test_windows_explicit_cleanup_retains_native_quit_blocker(self):
        class StillAliveInspector:
            def inspect(self, pid):
                return harness.ProcessSnapshot(
                    pid=pid, alive=True, birth_identity="birth-123",
                    command="C:/test/cataclysm.exe",
                )

        expected = {
            "pid": 123, "birth_identity": "birth-123", "command": "C:/test/cataclysm.exe",
        }
        with patch.object(harness.os, "name", "nt"), patch.object(harness.os, "kill") as kill:
            result = harness.cleanup_game_process(
                123, explicit_quit=True, expected_process_generation=expected,
                inspector=StillAliveInspector(),
            )
        self.assertEqual(result["status"], "graceful_quit_unconfirmed_process_retained")
        self.assertEqual(result["expected_process_generation"], expected)
        self.assertFalse(result["native_exit_credit"])
        kill.assert_not_called()

    def test_explicit_cleanup_does_not_escalate_to_forced_kill(self):
        class StillAliveInspector:
            def inspect(self, pid):
                return harness.ProcessSnapshot(
                    pid=pid, alive=True, birth_identity="birth-123",
                    command="/test/Cataclysm-AOL",
                )

        expected = {
            "pid": 123, "birth_identity": "birth-123", "command": "/test/Cataclysm-AOL",
        }
        with patch.object(harness.os, "kill") as kill:
            cleanup = harness.cleanup_game_process(
                123, explicit_quit=True, grace_seconds=0.0,
                expected_process_generation=expected, inspector=StillAliveInspector(),
            )
        self.assertEqual(cleanup["status"], "graceful_quit_unconfirmed_process_retained")
        kill.assert_called_once_with(123, harness.signal.SIGTERM)
        self.assertFalse(cleanup["native_exit_credit"])

    def test_failed_start_reports_the_live_owned_generation_without_closing_it(self):
        process = subprocess.Popen([
            os.path.realpath(sys.executable), "-u", "-c", "import time; time.sleep(60)", "Cataclysm-AOL",
        ])
        try:
            with tempfile.TemporaryDirectory() as temp:
                run_dir = Path(temp)
                generation = harness.process_generation_snapshot(process.pid)
                (run_dir / "process.json").write_text(json.dumps({
                    "pid": process.pid, "process_generation": generation,
                }), encoding="utf-8")
                result = harness.finalize_failed_certification_start(None, run_dir)
            self.assertEqual(result["status"], "retained_startup_failure_requires_explicit_quit")
            self.assertEqual(result["process_generation"]["expected"], generation)
            self.assertEqual(result["next_action"]["action"], "explicit_native_quit_or_bound_cleanup")
            self.assertIsNone(process.poll())
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)

    def test_unbound_finish_and_report_finalization_retain_a_live_fake_child(self):
        process = subprocess.Popen([
            os.path.realpath(sys.executable), "-u", "-c", "import time; time.sleep(60)", "Cataclysm-AOL",
        ])
        try:
            with tempfile.TemporaryDirectory() as temp:
                run_dir = Path(temp)
                finished = harness.finalize_cockpit_live_session(
                    run_dir, process.pid, {"termination_requested": True},
                )
                report = {"mode": "probe", "steps": []}
                harness.finalize_probe_report(
                    run_dir, report, cleanup_pid=process.pid,
                    report_filename="unbound.probe.report.json",
                )
            self.assertEqual(finished["cleanup"]["status"], "retained_process_identity_unavailable")
            self.assertEqual(report["cleanup"]["status"], "retained_process_identity_unavailable")
            self.assertIsNone(process.poll())
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)

    def test_explicit_finish_uses_current_replacement_generation_only(self):
        original = subprocess.Popen([
            os.path.realpath(sys.executable), "-u", "-c", "import time; time.sleep(60)", "Cataclysm-AOL",
        ])
        replacement = subprocess.Popen([
            os.path.realpath(sys.executable), "-u", "-c", "import time; time.sleep(60)", "Cataclysm-AOL",
        ])
        try:
            with tempfile.TemporaryDirectory() as temp:
                run_dir = Path(temp)
                original_generation = harness.process_generation_snapshot(original.pid)
                replacement_generation = harness.process_generation_snapshot(replacement.pid)
                (run_dir / "process.json").write_text(json.dumps({
                    "pid": original.pid, "process_generation": original_generation,
                }), encoding="utf-8")
                (run_dir / "process_chain.json").write_text(json.dumps({
                    "current_process_generation": replacement_generation,
                }), encoding="utf-8")
                result = harness.finalize_cockpit_live_session(
                    run_dir, original.pid,
                    {"termination_requested": True, "run_id": "run", "binding_id": "bound"},
                )
            self.assertEqual(result["cleanup"]["pid"], replacement.pid)
            self.assertEqual(result["cleanup"]["status"], "terminated")
            self.assertFalse(result["cleanup"]["native_exit_credit"])
            replacement.wait(timeout=3)
            self.assertIsNone(original.poll())
        finally:
            for process in (original, replacement):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=3)

    def test_progress_pid_without_owned_identity_is_not_killed(self):
        with tempfile.TemporaryDirectory() as tmp:
            bridge = FileBackedCockpitBridge(Path(tmp) / "session", ["unused"], binding_id="bound")
            bridge.prepare()
            bridge._startup_progress = {"pid": 123}
            with patch.object(harness, "cleanup_game_process") as cleanup:
                result = bridge._emergency_cleanup()
                cleanup.assert_not_called()
                self.assertEqual(result["ownership"], "unconfirmed_missing_process_record")

    @unittest.skipUnless(os.name == "posix", "POSIX wait ownership")
    def test_liveness_does_not_steal_nonzero_child_exit_status(self):
        process = subprocess.Popen([sys.executable, "-c", "import sys; sys.exit(17)"])
        try:
            deadline = time.monotonic() + 5
            while harness.pid_is_alive(process.pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(harness.pid_is_alive(process.pid))
            self.assertEqual(process.wait(timeout=2), 17)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_liveness_probe_leaves_running_child_alive_and_preserves_exit(self):
        process = subprocess.Popen([sys.executable, "-u", "-c",
            "import sys; print('ready',flush=True); sys.stdin.readline(); sys.exit(17)"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), "ready")
            self.assertTrue(harness.pid_is_alive(process.pid))
            self.assertIsNone(process.poll())
            process.communicate("finish\n", timeout=5)
            self.assertEqual(process.returncode, 17)
            self.assertFalse(harness.pid_is_alive(process.pid))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            process.stdout.close()

    def test_exit_receipt_binds_observed_return_code_to_owned_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            harness.record_bridge_game_exit(Mock(pid=123, _bridge_exit_generation={"pid": 123, "birth_identity": "birth", "command": "game"}), {
                "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": tmp,
                "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound",
                "OPENCLAW_HARNESS_RUN_ID": "run"}, 0)
            record = json.loads((Path(tmp) / "game-process-exit.json").read_text())
            self.assertEqual((record["binding_id"], record["run_id"], record["pid"], record["exit_code"]),
                             ("bound", "run", 123, 0))
            self.assertTrue(record["exit_observed_at"])

    def test_missing_process_identity_reports_failure_and_retains_game(self):
        process = Mock(pid=123)
        with tempfile.TemporaryDirectory() as tmp, patch.object(harness, "pid_command", return_value=""):
            with self.assertRaisesRegex(OSError, "process identity unavailable"):
                harness.record_bridge_game_process(process, {
                    "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": tmp,
                    "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound"})
            process.terminate.assert_not_called()
            process.wait.assert_not_called()
            self.assertFalse((Path(tmp) / "game-process.json").exists())

    def test_ownership_write_failure_retains_the_new_game(self):
        process = Mock(pid=123)
        with tempfile.TemporaryDirectory() as tmp, patch.object(harness, "pid_command", return_value="game"):
            with self.assertRaises(OSError):
                harness.record_bridge_game_process(process, {
                    "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": str(Path(tmp) / "missing"),
                    "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "bound"})
            process.terminate.assert_not_called()

class DisposableConPTYOwnershipTest(unittest.TestCase):
    def fixture(self, *, alive, disposable=True):
        from windows_native_process import NativeConPTY
        child = NativeConPTY.__new__(NativeConPTY)
        child._disposable_test = disposable
        child.child_role = 'disposable_test' if disposable else 'game'
        child._test_generation = {'pid': 123, 'birth_identity': 'windows-filetime:456'}
        child.pi = Mock(process=77, pid=123)
        child.k = Mock()
        child.k.TerminateProcess.return_value = True
        child.k.WaitForSingleObject.return_value = 0
        child.alive = Mock(side_effect=alive)
        child.exit_code = Mock(return_value=1)
        child.transcript_path = Path('retained-fixture.vt')
        child.reader_error = ''
        child._closed = False
        child.close_after_exit = Mock(side_effect=lambda: setattr(child, '_closed', True))
        return child

    def test_utf8_setup_preserves_fixture_or_gameplay_ownership(self):
        from windows_native_process import NativeConPTY
        for disposable in (False, True):
            with self.subTest(disposable=disposable), tempfile.TemporaryDirectory() as directory:
                child = self.fixture(alive=[], disposable=disposable)
                child.transcript_path = Path(directory) / 'fixture.vt'
                receipt = child.transcript_path.with_name('fixture.vt.utf8.json')
                def launch_helper(argv, cwd):
                    receipt.write_text(json.dumps({'pid': 123, 'set_output_ok': True,
                        'after_output_cp': 65001, 'before_input_cp': 437, 'after_input_cp': 437}))
                    return {'pid': 123}
                child._launch_child = launch_helper
                child.exit_code.return_value = 0
                child._prime_utf8(Path(directory))
                self.assertEqual(child._disposable_test, disposable)
                self.assertEqual(child.child_role, 'unlaunched')
                self.assertEqual(child.utf8_setup_receipt['exit_code'], 0)

    def test_settled_fixture_cannot_reuse_closed_handles(self):
        child = self.fixture(alive=[])
        child._closed = True
        with self.assertRaisesRegex(RuntimeError, 'already_settled'):
            child.wait_for_disposable_test(timeout=1)
        child.k.TerminateProcess.assert_not_called()

    def test_invalid_timeout_retains_child_and_output(self):
        for timeout in (0, -1, float('inf'), float('nan')):
            with self.subTest(timeout=timeout):
                child = self.fixture(alive=[])
                with self.assertRaisesRegex(ValueError, 'finite_positive'):
                    child.wait_for_disposable_test(timeout=timeout)
                child.k.TerminateProcess.assert_not_called()
                child.close_after_exit.assert_not_called()

    def test_gameplay_child_cannot_use_fixture_disposal(self):
        child = self.fixture(alive=[True], disposable=False)
        with self.assertRaisesRegex(RuntimeError, 'disposable_test_ownership_required'):
            child.wait_for_disposable_test(timeout=1)
        child.k.TerminateProcess.assert_not_called()
        child.close_after_exit.assert_not_called()

    def test_timeout_and_cancel_bind_original_handle_and_report_failure(self):
        from threading import Event
        for cancelled in (False, True):
            with self.subTest(cancelled=cancelled):
                child = self.fixture(alive=[True, True]); event = Event()
                if cancelled: event.set()
                with patch('windows_native_process.time.monotonic', side_effect=[0, 2, 2]), patch('windows_native_process.handle_birth', return_value=456):
                    result = child.wait_for_disposable_test(timeout=1, cancel=event)
                self.assertEqual(result['status'], 'cancelled' if cancelled else 'timed_out')
                self.assertFalse(result['native_gameplay_exit_credit'])
                self.assertTrue(result['handles_closed'])
                child.k.TerminateProcess.assert_called_once_with(77, 1)
                child.close_after_exit.assert_called_once()

    def test_already_exited_fixture_is_collected_without_termination(self):
        child = self.fixture(alive=[False, False]); child.exit_code.return_value = 0
        result = child.wait_for_disposable_test(timeout=1)
        self.assertEqual(result['status'], 'completed')
        self.assertEqual(result['exit_code'], 0)
        child.k.TerminateProcess.assert_not_called()
        self.assertTrue(result['handles_closed'])

    def test_nonzero_native_completion_is_failure_with_retained_output(self):
        child = self.fixture(alive=[False, False])
        result = child.wait_for_disposable_test(timeout=1)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['transcript_path'], 'retained-fixture.vt')
        self.assertTrue(result['handles_closed'])
        child.k.TerminateProcess.assert_not_called()

    def test_unobserved_exit_retains_handles_and_does_not_report_cleanup(self):
        child = self.fixture(alive=[True, True])
        child.k.WaitForSingleObject.return_value = 258
        with patch('windows_native_process.time.monotonic', side_effect=[0, 2]), patch('windows_native_process.handle_birth', return_value=456):
            with self.assertRaisesRegex(RuntimeError, 'exit_unobserved_handles_retained'):
                child.wait_for_disposable_test(timeout=1)
        child.close_after_exit.assert_not_called()
        self.assertFalse(child._closed)

    def test_changed_birth_refuses_disposal_and_retains_output_owner(self):
        child = self.fixture(alive=[True, True])
        with patch('windows_native_process.time.monotonic', side_effect=[0, 2]), patch('windows_native_process.handle_birth', return_value=999):
            with self.assertRaisesRegex(RuntimeError, 'generation_changed_child_retained'):
                child.wait_for_disposable_test(timeout=1)
        child.k.TerminateProcess.assert_not_called()
        child.close_after_exit.assert_not_called()

@unittest.skipUnless(os.name == 'nt', 'actual Windows ConPTY fixture controls')
class DisposableConPTYNativeTest(unittest.TestCase):
    def test_normal_input_wait_timeout_cancel_and_already_exited(self):
        from windows_native_process import NativeConPTY
        from threading import Event
        for condition in ('normal', 'failed', 'timeout', 'cancel', 'already_exited'):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as directory:
                transcript = Path(directory) / 'test.vt'
                child = NativeConPTY(transcript, disposable_test=True)
                source = "import sys; print('fixture output',flush=True); " + ("sys.stdin.readline()" if condition in ('timeout', 'cancel') else "sys.exit(1)" if condition == 'failed' else "sys.exit(0)")
                child.launch([sys.executable, '-u', '-c', source], Path(directory))
                event = Event()
                if condition in ('timeout', 'cancel'):
                    end = time.monotonic() + 10
                    while b'fixture output' not in transcript.read_bytes() and child.alive() and time.monotonic() < end:
                        time.sleep(.01)
                    if b'fixture output' not in transcript.read_bytes():
                        result = child.wait_for_disposable_test(timeout=.5)
                        self.fail('input-wait fixture did not publish its startup output: ' + repr(result))
                    if condition == 'cancel': event.set()
                if condition == 'already_exited':
                    end = time.monotonic() + 5
                    while child.alive() and time.monotonic() < end: time.sleep(.01)
                    self.assertFalse(child.alive())
                result = child.wait_for_disposable_test(timeout=.5 if condition == 'timeout' else 5, cancel=event)
                self.assertEqual(result['status'], {'timeout': 'timed_out', 'cancel': 'cancelled', 'failed': 'failed'}.get(condition, 'completed'))
                self.assertTrue(result['handles_closed'])
                self.assertFalse(child.alive())
                self.assertFalse(result['native_gameplay_exit_credit'])
                self.assertTrue(transcript.exists())
                self.assertIn(b'fixture output', transcript.read_bytes())
                # Hosts may retain the complete raw fixture output and result in
                # their existing task evidence before TemporaryDirectory cleanup.
                evidence = os.environ.get('CAOL_DISPOSABLE_TEST_EVIDENCE_DIR')
                if evidence:
                    destination = Path(evidence)
                    destination.mkdir(parents=True, exist_ok=True)
                    (destination / (condition + '.vt')).write_bytes(transcript.read_bytes())
                    (destination / (condition + '.json')).write_text(json.dumps(result, indent=2))

if __name__ == "__main__": unittest.main()
