#!/usr/bin/env python3
"""Focused controls for atomic certification process/world lease ownership."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest.mock import Mock, patch


HARNESS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(HARNESS_DIR))

from certification_process_lease import (  # noqa: E402
    CertificationLeaseConflict,
    ProcessSnapshot,
    quarantine_released_live_lease,
    release_certification_lease_handle,
    reserve_certification_lease,
    transfer_certification_lease,
)
from identity_binding import canonical_digest, component_identity  # noqa: E402
from scenario_registry_store import _issue_registry_certification_authority, open_registry  # noqa: E402
import startup_harness as harness  # noqa: E402
import certification_process_lease as process_lease  # noqa: E402
import writable_run_owner as writable  # noqa: E402
import curses_terminal_transport as terminal  # noqa: E402


class FakeInspector:
    def __init__(self, snapshots):
        self.snapshots = snapshots
        self.signals = []
        self.exit_on_signal = set()

    def inspect(self, pid):
        return self.snapshots.get(pid, ProcessSnapshot(pid=pid, alive=False))

    def signal(self, pid, sig):
        self.signals.append((pid, sig))
        if pid in self.exit_on_signal:
            self.snapshots[pid] = ProcessSnapshot(pid=pid, alive=False)


class ProcessGenerationIdentityTest(unittest.TestCase):
    def setUp(self):
        self.expected = {"schema": "caol-owned-process-generation-v1", "pid": 42,
                         "birth_identity": "fixture-birth", "command": "fixture command"}
        self.adapters = (harness.process_generation_matches, writable._generation_matches)

    def test_actual_adapters_match_legacy_and_public_identity(self):
        public = process_lease.public_process_generation(self.expected)
        for match in self.adapters:
            for expected, observed in ((self.expected, dict(self.expected)),
                                       (public, self.expected), (self.expected, public),
                                       (dict(public, pid="42", birth_identity=" fixture-birth "), public)):
                with self.subTest(adapter=match.__module__, expected=expected):
                    self.assertTrue(match(expected, observed))

    def test_actual_adapters_reject_mismatched_and_incomplete_identity(self):
        cases = [{}, {"pid": None}, {"pid": "bad"}, {"pid": 0}, {"pid": -42},
                 {"pid": []}, {"pid": 43}, {"birth_identity": ""},
                 {"birth_identity": "  "}, {"birth_identity": "reused-pid"},
                 {"command": ""}, {"command": "other fixture"},
                 {"command_sha256": None}, {"command_sha256": "invalid"}]
        for match in self.adapters:
            self.assertFalse(match({}, self.expected))
            self.assertFalse(match(self.expected, {}))
            for changes in cases:
                expected = dict(self.expected, **changes)
                # Missing birth/command are distinct from a changed value.
                if not changes:
                    expected.pop("birth_identity")
                with self.subTest(adapter=match.__module__, changes=changes):
                    self.assertFalse(match(expected, self.expected))
                    self.assertFalse(match(self.expected, expected))

    def test_actual_adapters_preserve_malformed_exception_boundaries(self):
        malformed = dict(self.expected, command="\ud800")
        with self.assertRaises(UnicodeEncodeError):
            harness.process_generation_matches(malformed, self.expected)
        self.assertFalse(writable._generation_matches(malformed, self.expected))
        for match in self.adapters:
            self.assertFalse(match(malformed, dict(self.expected, pid=43)))
            with self.assertRaises(AttributeError):
                match(None, self.expected)
            with self.assertRaises(OverflowError):
                match(dict(self.expected, pid=float("inf")), self.expected)

    def test_writable_keeps_invalid_expected_pid_short_circuit(self):
        # Writable's original conjunction never accesses observed identity here;
        # startup deliberately converts both PID fields before its final test.
        for expected in ({}, {"pid": None}, {"pid": 0}, {"pid": -1}):
            with self.subTest(expected=expected):
                self.assertFalse(writable._generation_matches(expected, None))
                self.assertFalse(writable._generation_matches(expected, {"pid": float("inf")}))
                with self.assertRaises(AttributeError):
                    harness.process_generation_matches(expected, None)
                with self.assertRaises(OverflowError):
                    harness.process_generation_matches(expected, {"pid": float("inf")})

    def test_terminal_variant_keeps_its_eager_command_validation(self):
        # This intentionally distinct adapter is outside the extraction.
        with self.assertRaises(UnicodeEncodeError):
            terminal._same_generation(dict(self.expected, command="\ud800"),
                                      dict(self.expected, pid=43))

    def test_shared_owner_does_not_hash_refused_identity_or_repeat_expected_hash(self):
        with patch.object(process_lease, "process_command_identity",
                          wraps=process_lease.process_command_identity) as identity:
            for observed in (dict(self.expected, pid=43), dict(self.expected, birth_identity="other")):
                self.assertFalse(process_lease.process_generation_identity_matches(self.expected, observed))
            identity.assert_not_called()
            observed = dict(self.expected, alive=True)
            self.assertTrue(process_lease.process_generation_identity_matches(self.expected, observed))
            self.assertEqual(identity.call_count, 2)
            self.assertIs(identity.call_args_list[0].args[0], self.expected)
            self.assertIs(identity.call_args_list[1].args[0], observed)

    def test_identity_comparison_does_not_take_liveness_or_path_ownership(self):
        observed = dict(self.expected, alive=False, schema="other", executable_path="other")
        with patch.object(process_lease.SystemProcessInspector, "inspect", side_effect=AssertionError("no OS probe")) as inspect, \
             patch.object(process_lease.SystemProcessInspector, "signal", side_effect=AssertionError("no signal")) as signal:
            for match in self.adapters:
                self.assertTrue(match(self.expected, observed))
            inspect.assert_not_called()
            signal.assert_not_called()

    def test_standalone_owner_inspection_keeps_refusal_reasons_without_signaling(self):
        owner = object.__new__(writable.WritableRunOwner)
        owner.matches = None
        with patch.object(process_lease.SystemProcessInspector, "signal", side_effect=AssertionError("no signal")) as signal:
            for observed, reason in ((dict(self.expected, alive=True), "live_generation"),
                                     (dict(self.expected, alive=True, birth_identity="new"), "generation_mismatch"),
                                     (dict(self.expected, alive=None), "identity_unavailable")):
                owner.inspect = Mock(return_value=observed)
                with self.subTest(reason=reason), self.assertRaises(writable.WritableRootConflict) as caught:
                    owner._inspect_generation(self.expected, source="fixture")
                self.assertEqual(caught.exception.reason, reason)
                owner.inspect.assert_called_once_with(42)
            owner.inspect = Mock(return_value=dict(self.expected, alive=False))
            self.assertIsNone(owner._inspect_generation(self.expected, source="fixture"))
            signal.assert_not_called()

    def test_standalone_binding_writes_once_only_for_exact_live_identity(self):
        owner = object.__new__(writable.WritableRunOwner)
        owner.root = Path("fixture-not-created")
        owner.matches = None
        owner.record = {"launch_state": "launching"}
        owner._write = Mock()
        for observed in (dict(self.expected, alive=True, birth_identity="other"),
                         dict(self.expected, alive=False)):
            owner.inspect = Mock(return_value=observed)
            with self.assertRaises(writable.WritableRootConflict) as caught:
                owner.bind_game(self.expected)
            self.assertEqual(caught.exception.reason, "generation_mismatch")
            self.assertEqual(owner.record, {"launch_state": "launching"})
            owner._write.assert_not_called()
        owner.inspect = Mock(return_value=dict(self.expected, alive=True))
        owner.bind_game(self.expected)
        self.assertEqual(owner.record, {"launch_state": "bound", "game_generation": self.expected})
        owner._write.assert_called_once_with()


class CertificationProcessLeaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.exe = self.root / "cataclysm-tiles"
        self.exe.write_bytes(b"certification-game")
        self.digest = hashlib.sha256(self.exe.read_bytes()).hexdigest()
        self.db = open_registry(str(self.root / "registry.sqlite3"))
        self.inspector = FakeInspector({
            101: self.snapshot(101, "birth-101"),
            202: self.snapshot(202, "birth-202"),
            303: self.snapshot(303, "birth-303"),
            404: self.snapshot(404, "birth-404", executable=self.root / "unrelated-game-like-process"),
        })

    def tearDown(self):
        self.db.close()
        self.temp.cleanup()

    def snapshot(self, pid, birth, executable=None, command=None):
        executable = self.exe if executable is None else executable
        return ProcessSnapshot(pid=pid, alive=True, executable_path=str(executable),
                               birth_identity=birth,
                               command=command or f"{executable} --world certification")

    def manifest(self, round_id, world_name="world-A"):
        names = ("worktree", "executable", "data_config", "harness", "scenario", "fixture", "profile", "world_save", "player", "actors")
        authoritative = {name: {"identity": name} for name in names}
        authoritative["executable"] = {"path": str(self.exe), "identity": "executable",
                                        "content_sha256": self.digest}
        authoritative["scenario"] = {"identity": "scenario", "content_sha256": "a" * 64}
        authoritative["world_save"] = {"world": str(self.root / world_name), "identity": world_name}
        components = {name: component_identity(name, authoritative[name]) for name in names}
        binding = {"schema": 1, "components": components, "authoritative_components": authoritative}
        binding["sha256"] = canonical_digest({key: value["sha256"] for key, value in components.items()}, domain="caol-complete-binding:v1")
        result = {"schema": 1, "version": 1, "round_id": round_id, "scenario_lineage_id": "lineage",
                  "authority_id": "authority", "authority_kind": "automated-certification", "event_stream_id": "stream-" + round_id,
                  "event_stream_schema": 1, "binding_id": binding["sha256"], "binding": binding}
        result["manifest_sha256"] = canonical_digest(result, domain="caol-round-manifest:v1")
        authority = _issue_registry_certification_authority(
            self.db, round_id=round_id, binding_id=binding["sha256"],
            source_sha256="a" * 64, launch_token="lease-test-token",
        )
        result["authority_id"] = authority["authority_id"]
        result["manifest_sha256"] = canonical_digest(
            {key: value for key, value in result.items() if key != "manifest_sha256"},
            domain="caol-round-manifest:v1",
        )
        return result

    def reserve_and_bind(self, round_id="round-a", lease_id="lease-a", pid=101):
        manifest = self.manifest(round_id)
        reserved = reserve_certification_lease(self.db, manifest=manifest, lease_id=lease_id,
                                               executable_path=str(self.exe), executable_sha256=self.digest,
                                               inspector=self.inspector)
        bound = transfer_certification_lease(self.db, manifest=manifest, lease_id=lease_id, pid=pid,
                                             executable_path=str(self.exe), executable_sha256=self.digest,
                                             inspector=self.inspector)
        return manifest, reserved, bound

    def test_atomic_conflict_and_identical_reservation_are_safe(self):
        manifest = self.manifest("round-a")
        reserved = reserve_certification_lease(self.db, manifest=manifest, lease_id="lease-a",
                                               executable_path=str(self.exe), executable_sha256=self.digest,
                                               inspector=self.inspector)
        again = reserve_certification_lease(self.db, manifest=manifest, lease_id="lease-a",
                                            executable_path=str(self.exe), executable_sha256=self.digest,
                                            inspector=self.inspector)
        self.assertTrue(again["idempotent"])
        self.assertEqual(reserved["state"], "reserved")
        transfer_certification_lease(self.db, manifest=manifest, lease_id="lease-a", pid=101,
                                     executable_path=str(self.exe), executable_sha256=self.digest,
                                     inspector=self.inspector)
        with self.assertRaises(CertificationLeaseConflict):
            reserve_certification_lease(self.db, manifest=self.manifest("round-b"), lease_id="lease-b",
                                        executable_path=str(self.exe), executable_sha256=self.digest,
                                        inspector=self.inspector)

    def test_stale_pid_reuse_is_recovered_without_signal(self):
        manifest, _, bound = self.reserve_and_bind()
        self.inspector.snapshots[101] = self.snapshot(101, "reused-birth")
        other = reserve_certification_lease(self.db, manifest=self.manifest("round-b"), lease_id="lease-b",
                                            executable_path=str(self.exe), executable_sha256=self.digest,
                                            inspector=self.inspector)
        self.assertEqual(other["state"], "reserved")
        self.assertEqual(self.inspector.signals, [])
        events = self.db.execute("SELECT event_kind FROM certification_round_lease_history WHERE round_id = ? ORDER BY event_sequence", (manifest["round_id"],)).fetchall()
        self.assertIn("stale_recovered", [row[0] for row in events])
        self.assertEqual(bound["pid"], 101)

    def test_same_round_stale_pid_reuse_cannot_be_reserved_for_relaunch(self):
        manifest, _, _bound = self.reserve_and_bind()
        self.inspector.snapshots[101] = self.snapshot(101, "reused-birth")
        with self.assertRaisesRegex(Exception, "reused or replaced"):
            reserve_certification_lease(self.db, manifest=manifest, lease_id="lease-a",
                                        executable_path=str(self.exe), executable_sha256=self.digest,
                                        inspector=self.inspector)
        self.assertEqual(self.inspector.signals, [])

    def test_release_rejects_wrong_lease_round_world_or_executable(self):
        manifest = self.manifest("round-a")
        reserve_certification_lease(self.db, manifest=manifest, lease_id="lease-a",
                                    executable_path=str(self.exe), executable_sha256=self.digest,
                                    inspector=self.inspector, cleanup_token="sealed-token")
        bound = transfer_certification_lease(self.db, manifest=manifest, lease_id="lease-a", pid=101,
                                             executable_path=str(self.exe), executable_sha256=self.digest,
                                             inspector=self.inspector, cleanup_token="sealed-token")
        wrong_world = dict(bound, world_identity="other-world")
        self.assertEqual(release_certification_lease_handle(self.db, manifest=manifest, lease=wrong_world, inspector=self.inspector)["status"], "rejected_world_identity")
        wrong_token = dict(bound, cleanup_token="wrong-token")
        self.assertEqual(release_certification_lease_handle(self.db, manifest=manifest, lease=wrong_token, inspector=self.inspector)["status"], "rejected_cleanup_token")
        wrong_exe = dict(bound, executable_path=str(self.root / "other"))
        self.assertEqual(release_certification_lease_handle(self.db, manifest=manifest, lease=wrong_exe, inspector=self.inspector)["status"], "rejected_stored_identity")
        wrong_lease = dict(bound, lease_id="other-lease")
        self.assertEqual(release_certification_lease_handle(self.db, manifest=manifest, lease=wrong_lease, inspector=self.inspector)["status"], "rejected_lease_unknown")
        self.assertEqual(release_certification_lease_handle(self.db, manifest=self.manifest("round-b"), lease=bound, inspector=self.inspector)["status"], "rejected_lease_unknown")
        self.assertEqual(self.inspector.signals, [])

    def test_safe_release_and_explicit_same_round_relaunch_transfer(self):
        manifest, _, bound = self.reserve_and_bind()
        self.inspector.snapshots[101] = ProcessSnapshot(pid=101, alive=False)
        reserved = reserve_certification_lease(self.db, manifest=manifest, lease_id="lease-a",
                                               executable_path=str(self.exe), executable_sha256=self.digest,
                                               inspector=self.inspector)
        self.assertTrue(reserved["relaunch_transfer"])
        self.assertEqual(reserved["state"], "reserved")
        relaunched = transfer_certification_lease(self.db, manifest=manifest, lease_id="lease-a", pid=202,
                                                  executable_path=str(self.exe), executable_sha256=self.digest,
                                                  inspector=self.inspector)
        self.assertEqual(relaunched["pid"], 202)
        self.inspector.exit_on_signal.add(202)
        released = release_certification_lease_handle(self.db, manifest=manifest, lease=relaunched,
                                                      inspector=self.inspector)
        self.assertEqual(released["status"], "terminated")
        self.assertEqual([pid for pid, _ in self.inspector.signals], [202])
        self.assertNotEqual(bound["process_birth_identity"], relaunched["process_birth_identity"])

    def test_exact_release_remains_owned_until_exit_is_observed(self):
        manifest, _, bound = self.reserve_and_bind()
        first = release_certification_lease_handle(self.db, manifest=manifest, lease=bound,
                                                   inspector=self.inspector)
        second = release_certification_lease_handle(self.db, manifest=manifest, lease=bound,
                                                    inspector=self.inspector)
        self.assertEqual(first["status"], "exit_unobserved")
        self.assertEqual(second["status"], "exit_unobserved")
        self.assertEqual([pid for pid, _ in self.inspector.signals], [101])
        self.inspector.snapshots[101] = ProcessSnapshot(pid=101, alive=False)
        released = release_certification_lease_handle(self.db, manifest=manifest, lease=bound,
                                                       inspector=self.inspector)
        self.assertEqual(released["status"], "already_exited")

    def test_released_live_owner_quarantine_never_signals_and_fences_its_world(self):
        manifest, _, bound = self.reserve_and_bind()
        self.inspector.exit_on_signal.add(101)
        self.assertEqual(
            release_certification_lease_handle(self.db, manifest=manifest, lease=bound,
                                               inspector=self.inspector)["status"],
            "terminated",
        )
        signal_count_before_quarantine = len(self.inspector.signals)
        self.inspector.snapshots[101] = self.snapshot(101, "birth-101")
        quarantined = quarantine_released_live_lease(
            self.db, manifest=manifest, lease_id="lease-a",
            reason="released receipt contradicted by later external PID observation",
        )
        self.assertEqual(quarantined["status"], "quarantined")
        self.assertEqual(len(self.inspector.signals), signal_count_before_quarantine)
        with self.assertRaises(CertificationLeaseConflict):
            reserve_certification_lease(self.db, manifest=self.manifest("round-b"), lease_id="lease-b",
                                        executable_path=str(self.exe), executable_sha256=self.digest,
                                        inspector=self.inspector)
        fresh = reserve_certification_lease(self.db, manifest=self.manifest("round-c", "world-B"),
                                            lease_id="lease-c", executable_path=str(self.exe),
                                            executable_sha256=self.digest, inspector=self.inspector)
        self.assertEqual(fresh["state"], "reserved")
        self.assertEqual(len(self.inspector.signals), signal_count_before_quarantine)

    def test_unrelated_game_like_process_is_never_selected_or_signalled(self):
        manifest, _, bound = self.reserve_and_bind()
        unrelated = self.inspector.snapshots[404]
        self.assertIn("game", unrelated.executable_path)
        rejected = release_certification_lease_handle(self.db, manifest=manifest,
                                                       lease=dict(bound, pid=404, process_birth_identity="birth-404"),
                                                       inspector=self.inspector)
        self.assertEqual(rejected["status"], "rejected_stored_identity")
        self.assertEqual(self.inspector.signals, [])

    def test_startup_route_requires_explicit_registry_manifest_and_lease(self):
        manifest_path = self.root / "round.manifest.json"
        manifest_path.write_text(json.dumps(self.manifest("round-a")), encoding="utf-8")
        args = Namespace(
            certification_registry=str(self.root / "startup-registry.sqlite3"),
            certification_round_manifest=str(manifest_path),
            certification_lease_id="lease-a",
        )
        context = harness.certification_startup_lease_context(
            args, executable=self.exe, target_world="world-A"
        )
        self.assertEqual(context["lease_id"], "lease-a")
        context["connection"].close()
        with self.assertRaisesRegex(SystemExit, "requires explicit"):
            harness.certification_startup_lease_context(
                Namespace(certification_registry="registry-only", certification_round_manifest="", certification_lease_id=""),
                executable=self.exe, target_world="world-A",
            )


if __name__ == "__main__":
    unittest.main()
