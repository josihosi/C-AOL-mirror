"""Connected Python route tests; synthetic native producers earn no live proof."""
from __future__ import annotations

import copy
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cockpit
import evidence_events
import process_performance
import scenario_registry
import scenario_registry_cli
import scenario_registry_ingestion_test as ingestion_fixtures
import scenario_registry_store as store
import startup_harness
from evidence_display import retain
from scenario_registry_performance import CONTRACT, CAPABILITY, SCENARIO, performance_contract, validate_performance_witness


CHARTER = {"claim": "Observe a native faction work interval", "material_proof": "Native work, resources, census and save/exit",
           "current_uncertainty": "Which actual interval is supported", "requested_evidence_ceiling": "focused"}


def manifest():
    return {
        "manifest_version": 1, "name": SCENARIO, "profile": "performance-test", "world": "",
        "harness_new_world": "PerformanceTest",
        "capabilities": {CAPABILITY: "baseline_then_adaptive_active_party_expansion",
                         "runtime.native_transport": "windows_conpty",
                         "capabilities.performance.target_population_sequence": {"active_parties": [1, 10, 20, 50, 100]}},
        "runtime_contract": {
            "grants_gameplay_proof": False, "setup_only_debug": True, "disposable_copy": True,
            "permitted_input": ["cockpit:game.observe", "cockpit:run.witness"], "forbidden_input": ["direct-state-mutation"],
            "helpers": ["WindowsCursesTerminalTransport"], "permissions": [], "platform": ["windows"],
            "profile": "performance-test", "fixture": "", "playtest_mode": "terminal", "terminal_transport": "pty",
            "requirements": {"os": "windows", "source": "current-worktree", "profile": "performance-test", "fixture": "none",
                             "helper": "WindowsCursesTerminalTransport", "peekaboo": False, "input": ["native"], "ocr": False, "cleanup": True}},
        "steps": [{"kind": "native_semantic_bootstrap", "label": "start"}, {"kind": "cockpit_live_session", "label": "work"}],
        "proof_route": {"precondition": ["start"], "production_behavior": ["work"], "terminal_persistence": ["work"],
                        "artifact_verdict": ["work"], "disallowed_shortcuts": ["work"]},
        "evidence_contract": {"performance_witness": dict(CONTRACT)},
    }


class PerformanceRouteTest(unittest.TestCase):
    def write(self, path, value):
        Path(path).write_text(json.dumps(value), encoding="utf-8")

    def setUp(self):
        self.charter = CHARTER
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = self.root / "run"
        self.session = self.root / "session"
        self.userdir = self.root / "userdata"
        self.world = self.userdir / "save" / "PerformanceTest"
        for path in (self.run, self.session, self.world):
            path.mkdir(parents=True)
        self.patch = mock.patch.object(startup_harness, "userdir_for_profile", return_value=self.userdir)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        evidence_store = mock.patch.object(evidence_events, "retain", side_effect=lambda value: retain(value, self.root / "evidence-display"))
        evidence_store.start()
        self.addCleanup(evidence_store.stop)
        self.connection = store.open_registry(str(self.root / "registry.sqlite3"))
        self.addCleanup(self.connection.close)
        self.declaration = manifest()
        self.scenarios = self.root / "scenarios"
        self.scenarios.mkdir()
        self.path = self.scenarios / "performance.json"
        self.write(self.path, self.declaration)
        store.rebuild_manifest_projection(self.connection, self.scenarios)
        self.manifest_id = self.connection.execute("SELECT manifest_id FROM manifest_current").fetchone()[0]
        (self.root / "game.exe").write_bytes(b"synthetic executable, never launched")
        self.execution = store.execute_registry_query(self.connection, store.parse_registry_query_request({"requirements": [
            {"key": CAPABILITY, "op": "eq", "value": "baseline_then_adaptive_active_party_expansion", "minimum_evidence": "declared"}], "preferences": []}),
            witness_charter=CHARTER, drafts_root=self.root / "drafts")
        self.run_id = self.execution.token_id
        self.assertIsNotNone(self.run_id)
        self.write(self.session / "bridge.manifest.json", {"authorization_token_id": self.run_id, "binding_id": "binding"})
        self.runtime = {"status": "matched", "runtime_source_sha256": "a" * 64,
                        "executable_sha256": hashlib.sha256((self.root / "game.exe").read_bytes()).hexdigest(),
                        "executable_path": str(self.root / "game.exe")}
        self.write(self.run / "runtime.binding.json", self.runtime)
        self.generation = {"pid": 123, "birth_identity": "windows-filetime:456", "executable_path": self.runtime["executable_path"]}
        self.owner = {"run_id": self.run_id, "binding_id": "binding", "pid": 123,
                      "process_generation": self.generation, "userdir": str(self.userdir),
                      "world": "", "playtest_mode": "terminal",
                      "log_paths": {"transition_events": {"path": str(self.run / "transition.events.jsonl"), "scope": "run_bound"}}}
        self.write(self.session / "game-process.json", self.owner)
        self.write(self.session / "game-process-exit.json", {**self.owner, "exit_code": 0,
            "exit_observed_at": (datetime.now(timezone.utc) + timedelta(seconds=1)).isoformat(timespec="milliseconds")})
        self.process = {"run_id": self.run_id, "pid": 123, "alive": False, "exit_code": 0,
                        "identity_status": "exited", "process_generation": self.generation,
                        "evidence_ref": str(self.session / "game-process-exit.json")}
        self.dimension = {"overmapbuffer": {"bandit_live_world": {"sites": [
            {"site_id": "site-a", "site_kind": "camp_style", "members": [{"npc_id": 7, "state": "outbound"}]}]},
            "migo_populations": [], "migo_towers": []}}
        self.save_dimension()
        self.behavior = {"schema_version": 1, "sequence": 1, "run_id": self.run_id, "domain": "bandit_live_world",
                         "site_id": "site-a", "operation_id": "outing-a", "actor_ids": [7], "game_minutes": 101,
                         "transition": "local_pair_abstract_resume_progress", "outcome": "committed"}
        self.write_record(self.run / "transition.events.jsonl", self.behavior)
        # Exercise the real ProcessPerformance producer with controlled process samples.
        samples = iter([self.sample(1), self.sample(2), self.sample(3)])
        performance = process_performance.ProcessPerformance(
            self.session, pid=123, run_id=self.run_id, binding_id="binding",
            source_binding={"runtime_binding_sha256": hashlib.sha256((self.run / "runtime.binding.json").read_bytes()).hexdigest()},
            sampler=lambda pid: next(samples))
        frame = {"kind": "world", "game_minutes": 100}
        performance.begin_action(frame, "world.wait")
        performance.end_action(frame, {"accepted": True, "next_frame": {"game_minutes": 102}})
        self.assertIsNotNone(performance.latest)
        self.measurement = performance.latest
        # Existing cockpit producer supplies its own save completion matching and exit observation.
        channel = cockpit.CockpitRunChannel(lambda: {}, lambda *args: {}, binding_id="binding",
            read_process_state=lambda: self.process, witness_charter=CHARTER,
            witness_identity={"scenario_id": SCENARIO, "source_identity": "a" * 64,
                              "executable_identity": self.runtime["executable_sha256"]})
        channel._run_id = self.run_id
        world_receipt = {"accepted": True, "run_id": self.run_id, "request_id": "save-1", "action_id": "world.save_quit",
                         "requested_run_id": self.run_id, "requested_surface_id": "world-1", "requested_frame_id": "frame-1"}
        completion = {**world_receipt, "schema": "caol-native-save-completion-v1", "serializer_result": "saved",
                      "save_succeeded": True, "world_name": "PerformanceTest", "player_save_id": "player",
                      "artifact_identity": {"kind": "harness_run_directory", "value": str(self.run)}}
        channel._transcript.extend([
            {"kind": "action", "action_id": "world.save_quit", "result": {"receipt": {"native_receipt": world_receipt}}},
            {"kind": "action", "action_id": "prompt.choose", "result": {"receipt": {"native_receipt": {
                "accepted": True, "run_id": self.run_id, "native_save_completion": {"status": "matched", "completion": completion}}}}}])
        observation = channel.observe_process_exit()
        self.assertTrue(channel.seal_witness_journal(observation_id=observation["observation_id"], stop_reason="interval ended", unused_authority="none")["ok"])
        self.journal = channel._sealed_journal
        citation = next(entry["citation_id"] for entry in self.journal["entries"] if entry["kind"] == "observation")
        self.statement = {"verdict": "proved", "smallest_supported_claim": "The named faction member advanced its native route during the resource interval.",
                          "causal_account": "Exact producer records identify the actor, committed route progress and elapsed game-time interval.",
                          "recommended_disposition": "accept", "evidence_ceiling": "focused",
                          "citations": [{"citation_id": citation, "meaning": "bound OS exit", "checks": ["value.surface.facts"]}]}
        initial = {**self.statement, "verdict": "inconclusive", "recommended_disposition": "continue"}
        final_result = channel.finish(observation_id=observation["observation_id"], stop_reason="interval ended", unused_authority="none", witness=initial)
        self.assertTrue(final_result["ok"], final_result)
        self.final = channel._final_report
        self.report = ingestion_fixtures.ScenarioRegistryIngestionTest().report(self.path)
        self.report.update(scenario=SCENARIO, contract={"profile": "performance-test-run-" + self.run_id,
                                                     "config_profile": "performance-test", "world": ""},
                           steps=[{"cockpit_live_session": {"final": self.final}}],
                           proof_classification={"status": "yellow", "feature_proof": False, "evidence_class": "startup/load-or-inconclusive"},
                           feature_proof=False, evidence_class="startup/load-or-inconclusive", verdict="yellow_interval", certification_lifecycle={})
        self.report["startup"]["screen"].update(runtime_binding_status="matched", runtime_binding_observed=self.runtime)
        self.report["world_snapshot"] = {"status": "current_saved_world_reference", "source": str(self.world),
                                         "source_sha256": startup_harness.sha256_tree(self.world)[0]}
        self.report_path = self.run / "probe.report.json"
        self.write(self.report_path, self.report)
        self.statement["performance_evidence"] = {"behavior": [self.handle(self.run / "transition.events.jsonl")],
                                                    "measurements": [self.handle(self.session / "performance.jsonl")]}

    def save_dimension(self):
        (self.world / "dimension_data.gsav").write_text("# version 1\n" + json.dumps(self.dimension), encoding="utf-8")

    def sample(self, seconds):
        return {"pid": 123, "platform": "windows", "process_identity": str(456 + 504911232000000000),
                "sampled_monotonic_seconds": seconds, "cpu_seconds": seconds / 2,
                "resident_memory": {"status": "available", "value": 100000, "unit": "bytes"}}

    def write_record(self, path, value):
        path.write_text(json.dumps(value) + "\n", encoding="utf-8")

    def handle(self, path):
        # Consume real evidence query handles including producer and retained_raw.
        result = evidence_events.query([{"path": str(path), "producer": "native_test_double"}], {}, limit=1)
        return result["rows"][0]["source"]

    def ingest(self, *, selected=True):
        adapters = store.BindingAdapters(runtime=lambda expected: {"status": "compatible", "facts": self.runtime},
            fixture=lambda expected: {"status": "compatible", "facts": {"source_sha256": "f" * 64}},
            profile=lambda expected: {"status": "compatible", "facts": {"source_sha256": "b" * 64}})
        if selected:
            result = store.ingest_token_linked_report_reference(self.connection, self.execution.token_id, self.report_path, adapters=adapters)
        else:
            result = store.ingest_report_reference(self.connection, self.report_path, adapters=adapters)
        self.assertEqual(result["status"], "ingested", result)
        self.report_id = result["report_id"]
        return result

    def record(self):
        return store.record_playtest_witness(self.connection, manifest_id=self.manifest_id, report_ids=[self.report_id],
                                             charter=self.charter, journal=self.journal, statement=self.statement)

    def validate_native(self, **changes):
        arguments = dict(declaration=self.declaration, report=self.report, report_path=self.report_path,
                         final=self.final, journal=self.journal, statement=self.statement, runtime=self.runtime)
        arguments.update(changes)
        return validate_performance_witness(**arguments)

    def test_selected_producer_consumer_and_cli_preserve_only_witnessed_interval(self):
        self.ingest()
        result = self.record()
        self.assertEqual(result["performance"]["scope"], "observed_native_work_interval")
        self.assertFalse(result["performance"]["whole_sequence_proven"])
        store.review_playtest_witness(self.connection, witness_id=result["witness_id"], decision="accept", rationale="test scoped acceptance")
        self.assertFalse(store.final_gate_eligibility(self.connection)["automated_certification"])
        visible = store.registry_status(self.connection)[0]["playtest_witnesses"]
        scoped = next(row for row in visible if row["witness_id"] == result["witness_id"])
        self.assertEqual(scoped["performance"], {"scope": "observed_native_work_interval", "whole_sequence_proven": False,
                                                "gameplay_credit": False})
        row = self.connection.execute("SELECT route_key, verification_id FROM verification_history").fetchone()
        self.assertEqual(store._playtest_witness_hard_proven_candidates(self.connection, manifest_id=self.manifest_id,
            route_key=row["route_key"], candidate_rows={row["verification_id"]: {}}, report_local_hard_proven={row["verification_id"]}), set())
        for name, value in (("journal", self.journal), ("statement", self.statement), ("charter", CHARTER)):
            self.write(self.root / (name + ".json"), value)
        output = io.StringIO()
        with redirect_stdout(output):
            code = scenario_registry_cli.main(["--registry", str(self.root / "registry.sqlite3"), "registry-record-witness",
                "--manifest-id", self.manifest_id, "--report-id", self.report_id, "--journal", str(self.root / "journal.json"),
                "--statement", str(self.root / "statement.json"), "--charter", str(self.root / "charter.json")])
        self.assertEqual(code, 0, output.getvalue())
        self.assertEqual(json.loads(output.getvalue())["result"]["witness_id"], result["witness_id"])

    def test_count_load_idle_partial_and_mismatched_sources_never_prove(self):
        self.ingest()
        original = copy.deepcopy(self.statement)
        cases = [({"behavior": []}, "native_sources_required"), ({"measurements": []}, "native_sources_required")]
        for delta, reason in cases:
            with self.subTest(delta=delta):
                self.statement = copy.deepcopy(original)
                self.statement["performance_evidence"].update(delta)
                with self.assertRaisesRegex(store.ScenarioRegistryStoreError, reason):
                    self.record()
        self.statement = copy.deepcopy(original)
        for delta in ({"transition": "active_sortie_dispatch"}, {"transition": "idle"}, {"outcome": "cadence_service"},
                      {"actor_ids": [999]}, {"game_minutes": 999}, {"run_id": "other"}):
            with self.subTest(behavior=delta):
                self.write_record(self.run / "transition.events.jsonl", {**self.behavior, **delta})
                self.statement["performance_evidence"]["behavior"] = [self.handle(self.run / "transition.events.jsonl")]
                with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "no_meaningful_native_actor_work"):
                    self.record()
        self.write_record(self.run / "transition.events.jsonl", self.behavior)
        self.statement = copy.deepcopy(original)
        self.write_record(self.session / "performance.jsonl", {**self.measurement, "game_time": {"source": "native_frame", "before_minutes": 100, "after_minutes": 100}})
        self.statement["performance_evidence"]["measurements"] = [self.handle(self.session / "performance.jsonl")]
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "measurement_not_native_work"):
            self.record()

    def test_unselected_and_historical_reports_cannot_borrow_fresh_authority(self):
        self.ingest(selected=False)
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "fresh_selected_report_required"):
            self.record()
        adapters = store.BindingAdapters(runtime=lambda expected: {"status": "compatible", "facts": self.runtime},
            fixture=lambda expected: {"status": "compatible", "facts": {"source_sha256": "f" * 64}},
            profile=lambda expected: {"status": "compatible", "facts": {"source_sha256": "b" * 64}})
        # A fresh token issued for the same bytes must not bless an old native run.
        another = store.execute_registry_query(self.connection, store.parse_registry_query_request({"requirements": [
            {"key": CAPABILITY, "op": "eq", "value": "baseline_then_adaptive_active_party_expansion", "minimum_evidence": "declared"}],
            "preferences": []}), witness_charter={**CHARTER, "current_uncertainty": "new attempt"}, drafts_root=self.root / "drafts")
        self.assertNotEqual(another.token_id, self.run_id)
        linked = store.ingest_token_linked_report_reference(self.connection, another.token_id, self.report_path, adapters=adapters)
        self.assertEqual(linked["status"], "ingested", linked)
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "fresh_selected_report_required"):
            self.record()

    def test_changed_native_save_metric_hash_birth_and_exit_rejected(self):
        self.ingest()
        self.write_record(self.session / "performance.jsonl", {**self.measurement, "record_id": "tampered"})
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "source_hash_or_length_mismatch"):
            self.record()
        self.write_record(self.session / "performance.jsonl", self.measurement)
        self.owner["process_generation"] = {**self.generation, "birth_identity": "windows-filetime:999"}
        self.write(self.session / "game-process.json", self.owner)
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "process_owner_mismatch"):
            self.record()
        self.owner["process_generation"] = self.generation
        self.write(self.session / "game-process.json", self.owner)
        self.dimension["overmapbuffer"]["migo_towers"].append("changed")
        self.save_dimension()
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "saved_world_drift"):
            self.record()

    def test_native_persistence_exit_and_producer_identity_negatives(self):
        self.assertEqual(self.validate_native()["scope"], "observed_native_work_interval")
        for mutate in (lambda final: final.update(state="active"),
                       lambda final: final.update(closure_kind="explicit_abort"),
                       lambda final: final["native_save_completion"].update(status="missing"),
                       lambda final: final["native_save_completion"]["completion"].update(save_succeeded=False),
                       lambda final: final["native_save_completion"]["completion"].update(request_id="other-save"),
                       lambda final: final["native_save_completion"]["completion"].update(world_name="old-fixture")):
            final = copy.deepcopy(self.final)
            mutate(final)
            with self.assertRaisesRegex(ValueError, "non_native_terminal|native_persistence_unproved"):
                self.validate_native(final=final)
        # .NET ticks and FILETIME of the same birth pass above; PID reuse and
        # a wrong birth in either representation fail independently.
        for mutate in (lambda record: record["owner"].update(process_identity="456"),
                       lambda record: record["owner"].update(process_identity=str(457 + 504911232000000000)),
                       lambda record: record["owner"].update(pid=124),
                       lambda record: record["owner"].update(run_id="other"),
                       lambda record: record["owner"].update(binding_id="other"),
                       lambda record: record["owner"]["source_binding"].update(runtime_binding_sha256="other")):
            record = copy.deepcopy(self.measurement)
            mutate(record)
            self.write_record(self.session / "performance.jsonl", record)
            statement = copy.deepcopy(self.statement)
            statement["performance_evidence"]["measurements"] = [self.handle(self.session / "performance.jsonl")]
            with self.assertRaisesRegex(ValueError, "measurement_birth_mismatch|measurement_not_native_work"):
                self.validate_native(statement=statement)
        self.write_record(self.session / "performance.jsonl", self.measurement)
        (self.root / "game.exe").write_bytes(b"replacement executable")
        with self.assertRaisesRegex(ValueError, "executable_bytes_drift"):
            self.validate_native()

    def test_bridge_authority_and_save_lifetime_are_checked_without_graphical_snapshot(self):
        report = copy.deepcopy(self.report)
        report.pop("world_snapshot")
        self.assertEqual(self.validate_native(report=report)["scope"], "observed_native_work_interval")
        for delta in ({"authorization_token_id": "another-selection", "binding_id": "binding"},
                      {"authorization_token_id": self.run_id, "binding_id": "other"}):
            self.write(self.session / "bridge.manifest.json", delta)
            with self.assertRaisesRegex(ValueError, "selected_bridge_authority_mismatch"):
                self.validate_native(report=report)
        self.write(self.session / "bridge.manifest.json", {"authorization_token_id": self.run_id, "binding_id": "binding"})
        for delta in ({"world": "other-world"}, {"playtest_mode": "tiles"}):
            self.write(self.session / "game-process.json", {**self.owner, **delta})
            with self.assertRaisesRegex(ValueError, "world_or_transport_owner_mismatch"):
                self.validate_native(report=report)
        self.write(self.session / "game-process.json", self.owner)
        path = self.world / "dimension_data.gsav"
        os.utime(path, (4102444800, 4102444800))  # synthetic 2100 timestamp after the recorded exit
        with self.assertRaisesRegex(ValueError, "save_outside_native_process_lifetime"):
            self.validate_native(report=report)

    def test_terminal_readiness_uses_supported_current_caller(self):
        with mock.patch.object(startup_harness.sys, "platform", "win32"), mock.patch.object(
            startup_harness, "validate_selected_product_build", return_value=({"build_configuration": {
                "renderer": "curses", "make_variables": ["SOUND=0"]}}, "")):
            readiness = startup_harness.terminal_mode_readiness(self.declaration, self.root / "game.exe")
        self.assertEqual(readiness["status"], "ready", readiness)

    def lower_ceiling_through_live_producer(self, charter):
        """Exercise execute_probe_steps' actual ceiling choice, then the real journal producer."""
        self.write(self.run / startup_harness.TRANSITION_EVENT_BINDING_FILENAME, {"run_id": self.run_id})
        channels = []

        def factory(**kwargs):
            channel = cockpit.CockpitRunChannel(lambda: {}, lambda *args: {}, binding_id="binding",
                read_process_state=lambda: self.process, witness_charter=kwargs["witness_charter"],
                witness_identity=kwargs["witness_identity"], witness_evidence_ceiling=kwargs["witness_evidence_ceiling"])
            channel._run_id = self.run_id
            channel._transcript.extend(copy.deepcopy([event for event in self.final["action_observation_sequence"]
                                                      if event.get("kind") == "action"]))
            channels.append(channel)
            return cockpit.CockpitService(run_channel=channel)

        def serve(service, _input, _output):
            channel = service.run_channel
            observed = channel.observe_process_exit()
            sealed = channel.seal_witness_journal(observation_id=observed["observation_id"], stop_reason="done", unused_authority="none")
            self.assertTrue(sealed["ok"], sealed)
            self.journal = channel._sealed_journal
            citation = next(entry["citation_id"] for entry in self.journal["entries"] if entry["kind"] == "observation")
            initial = {key: value for key, value in self.statement.items() if key != "performance_evidence"}
            initial.update(verdict="inconclusive", recommended_disposition="continue", evidence_ceiling=channel._witness_evidence_ceiling,
                           citations=[{"citation_id": citation, "meaning": "bound exit", "checks": ["value.surface.facts"]}])
            result = channel.finish(observation_id=observed["observation_id"], stop_reason="done", unused_authority="none", witness=initial)
            self.assertTrue(result["ok"], result)
            return 0

        with redirect_stdout(io.StringIO()), mock.patch.object(startup_harness, "semantic_step_trace_start_for_source", return_value=0), \
                mock.patch.object(startup_harness, "active_playtest_witness_charter", return_value=charter), \
                mock.patch.object(startup_harness, "open_cockpit_game_service", side_effect=factory), \
                mock.patch.object(startup_harness, "serve_cockpit_live", side_effect=serve), \
                mock.patch.object(startup_harness, "read_completed_cockpit_final", side_effect=lambda service, *args, **kwargs: service.run_channel._final_report):
            startup_harness.execute_probe_steps(123, self.run, [{"kind": "cockpit_live_session", "label": "work"}],
                profile=self.report["contract"]["profile"], world="", scenario_identity=SCENARIO,
                runtime_binding=self.runtime, grants_gameplay_proof=False, semantic_only=True)
        self.final = channels[0]._final_report
        self.charter = channels[0]._witness_charter
        self.report["steps"] = [{"cockpit_live_session": {"final": self.final}}]
        self.write(self.report_path, self.report)
        return channels[0]._witness_evidence_ceiling

    def test_lower_launch_authority_cannot_borrow_later_focused_collection(self):
        for ceiling in ("setup-only", "diagnostic"):
            with self.subTest(ceiling=ceiling):
                fixture = PerformanceRouteTest("test_lower_launch_authority_cannot_borrow_later_focused_collection")
                fixture.setUp()
                try:
                    # The standalone receipt is immutable too; it is not a
                    # required row for the independent detached launcher.
                    selected = store.execute_registry_query(fixture.connection, store.parse_registry_query_request({
                        "requirements": [{"key": CAPABILITY, "op": "eq", "value": "baseline_then_adaptive_active_party_expansion", "minimum_evidence": "declared"}],
                        "preferences": []}), drafts_root=fixture.root / "drafts")
                    with mock.patch.object(store, "_cockpit_evidence_effect", return_value=ceiling):
                        opening = store.open_cockpit_run(fixture.connection, selection_id=selected.selection_id,
                            owner_id="older-observation", executable=fixture.root / "game.exe")
                    self.assertEqual(fixture.lower_ceiling_through_live_producer({**CHARTER, "requested_evidence_ceiling": ceiling}), ceiling)
                    with mock.patch.object(store, "_cockpit_evidence_effect", return_value="focused"):
                        fixture.ingest()
                        self.assertEqual(store._run_evidence_ceiling(fixture.connection, fixture.manifest_id, fixture.declaration), "focused")
                        self.assertEqual(store.cockpit_run_authority_status(fixture.connection, run_id=opening["run_id"])["evidence_ceiling"], ceiling)
                        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "witness_evidence_promotion_rejected"):
                            fixture.record()
                    persisted = fixture.connection.execute("SELECT validation_json FROM playtest_witness_history").fetchone()[0]
                    self.assertEqual(json.loads(persisted)["performance"]["scope"], "none")
                finally:
                    fixture.doCleanups()

    def test_absent_live_charter_keeps_actual_false_gameplay_producer_diagnostic(self):
        self.assertEqual(store._run_evidence_ceiling(self.connection, self.manifest_id, self.declaration), "focused")
        self.assertEqual(self.lower_ceiling_through_live_producer(None), "diagnostic")
        self.ingest()
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "witness_evidence_promotion_rejected"):
            self.record()

    def test_witness_ceiling_hash_and_exact_identity_negatives(self):
        self.ingest()
        original = copy.deepcopy(self.statement)
        for ceiling in ("zero-credit", "setup-only", "diagnostic", "certification"):
            self.statement = copy.deepcopy(original)
            self.statement["evidence_ceiling"] = ceiling
            with self.assertRaises(store.ScenarioRegistryStoreError):
                self.record()
        self.statement = original
        for identity_key in ("scenario_id", "source_identity", "executable_identity", "run_id", "binding_id"):
            journal = copy.deepcopy(self.journal)
            journal["identity"][identity_key] = "other"
            with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "binding mismatched"):
                store.record_playtest_witness(self.connection, manifest_id=self.manifest_id, report_ids=[self.report_id],
                    charter=CHARTER, journal=journal, statement=self.statement)
        with mock.patch.object(store, "_cockpit_evidence_effect", return_value="diagnostic"):
            with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "collection_ceiling_rejects_proof"):
                self.record()
        self.report_path.write_text(self.report_path.read_text() + "\n", encoding="utf-8")
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "report identity is stale"):
            self.record()

    def test_declaration_ceiling_and_validation_do_not_flip_gameplay_allowlist(self):
        self.assertTrue(performance_contract(self.declaration))
        self.assertEqual(store._run_evidence_ceiling(self.connection, self.manifest_id, self.declaration), "focused")
        for state in ("setup-only", "diagnostic"):
            with mock.patch.object(store, "_cockpit_evidence_effect", return_value=state):
                self.assertEqual(store._run_evidence_ceiling(self.connection, self.manifest_id, self.declaration), state)
        old = copy.deepcopy(self.declaration)
        old["evidence_contract"].pop("performance_witness")
        self.assertEqual(store._run_evidence_ceiling(self.connection, self.manifest_id, old), "zero-credit")
        for key, value in (("name", "other.scenario"), ("capture_world_after", True), ("saved_world_snapshot", "old-save")):
            bad = copy.deepcopy(self.declaration)
            bad[key] = value
            self.write(self.path, bad)
            with self.assertRaises(scenario_registry.ManifestValidationError):
                scenario_registry.validate_manifest(bad, path=self.path)
            self.assertEqual(scenario_registry.lint_manifest(self.path)["status"], "invalid")
        bad = copy.deepcopy(self.declaration)
        bad["runtime_contract"]["grants_gameplay_proof"] = True
        self.write(self.path, bad)
        with self.assertRaises(scenario_registry.ManifestValidationError):
            scenario_registry.validate_manifest(bad, path=self.path)

    def test_fresh_authority_lifecycle_rechecks_executable_and_never_relabels_old_openings(self):
        execution = store.execute_registry_query(self.connection, store.parse_registry_query_request({
            "requirements": [{"key": CAPABILITY, "op": "eq", "value": "baseline_then_adaptive_active_party_expansion", "minimum_evidence": "declared"}],
            "preferences": []}), drafts_root=self.root / "drafts")
        self.assertIsNone(execution.token_id)
        opened = store.open_cockpit_run(self.connection, selection_id=execution.selection_id,
                                       owner_id="test-owner", executable=self.root / "game.exe")
        self.assertEqual(opened["evidence_ceiling"], "focused")
        self.assertFalse(opened["proof_promotion_authority"])
        with self.assertRaisesRegex(store.ScenarioRegistryStoreError, "already consumed"):
            store.open_cockpit_run(self.connection, selection_id=execution.selection_id,
                                  owner_id="test-owner", executable=self.root / "game.exe")
        (self.root / "game.exe").write_bytes(b"changed after authority opened")
        status = store.cockpit_run_authority_status(self.connection, run_id=opened["run_id"])
        self.assertEqual(status["state"], "invalidated")
        self.assertEqual(status["evidence_ceiling"], "zero-credit")
        self.assertEqual(status["terminal"]["reason"], "executable_binding_drift")
        self.assertEqual(self.connection.execute("SELECT evidence_ceiling FROM cockpit_run_authority").fetchone()[0], "focused")


if __name__ == "__main__":
    unittest.main()
