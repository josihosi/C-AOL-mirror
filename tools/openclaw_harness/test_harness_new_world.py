#!/usr/bin/env python3
"""Focused tests for the hidden fresh-normal-world harness seam."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import startup_harness as harness


class HarnessNewWorldTest(unittest.TestCase):
    def test_validate_preserves_decimal_seed_provenance(self) -> None:
        self.assertEqual(
            harness.validate_harness_new_world("natural-r002", "830205018"),
            ("natural-r002", "830205018"),
        )

    def test_validate_rejects_zero_overflow_and_path_names(self) -> None:
        for world, seed, message in (
            ("natural-r002", "0", "harness raw seed must be non-zero"),
            ("natural-r002", "4294967296", "harness raw seed is outside the uint32 range"),
            ("../natural-r002", "830205018", "harness new-world name must be one path component"),
        ):
            with self.subTest(world=world, seed=seed):
                with self.assertRaisesRegex(ValueError, message):
                    harness.validate_harness_new_world(world, seed)

    def test_game_argv_is_exact_and_harness_args_are_suppressed_player_ui(self) -> None:
        command = harness.build_game_command(
            Path("/game/cataclysm-tiles"),
            "r002-m93",
            "natural-r002",
            harness_new_world="natural-r002",
            harness_raw_seed="830205018",
        )
        self.assertEqual(
            command,
            [
                "/game/cataclysm-tiles",
                "--userdir", ".userdata/r002-m93/",
                "--harness-new-world", "natural-r002",
                "--harness-raw-seed", "830205018",
            ],
        )

    def test_game_argv_carries_an_explicit_ordinary_scenario(self) -> None:
        command = harness.build_game_command(
            Path("/game/cataclysm-tiles"), "r002-missed", "natural-missed",
            harness_new_world="natural-missed", harness_raw_seed="2026091515",
            harness_new_world_scenario="missed",
        )
        self.assertEqual(command[-2:], ["--harness-new-world-scenario", "missed"])

    def test_harness_scenario_requires_a_fresh_named_world(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires harness new-world mode"):
            harness.build_plan("profile", "", "", harness_new_world_scenario="missed")

    def test_harness_startup_skips_permission_focus_and_input_owners(self) -> None:
        self.assertFalse(harness.startup_gui_automation_required("harness_new_world"))
        self.assertTrue(harness.startup_gui_automation_required("play_now_default"))

    def test_exit_success_requires_zero_code_and_newer_marker(self) -> None:
        marker = {"mtime": 20.0}
        self.assertTrue(harness.harness_save_ready(0, marker, 10.0))
        self.assertFalse(harness.harness_save_ready(1, marker, 10.0))
        self.assertFalse(harness.harness_save_ready(0, {"mtime": 10.0}, 10.0))
        self.assertTrue(harness.harness_process_ready(0, marker, 10.0, 0))
        self.assertFalse(harness.harness_process_ready(0, marker, 10.0, 1))

    def test_feasibility_artifact_is_required_and_surfaced(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            run_dir = Path(temp_dir)
            artifact = {
                "artifact_kind": "harness_new_world_feasibility",
                "artifact_version": 1,
                "raw_seed": "830205385",
                "natural_bandit_site_found": True,
                "natural_bandit_site_id": "overmap_special:bandit_camp@16,33,0",
                "player_omt": {"x": 46, "y": 48, "z": 0},
                "natural_bandit_site_anchor": {"x": 16, "y": 33, "z": 0},
                "natural_bandit_site_nearest_omt": {"x": 17, "y": 34, "z": 0},
                "natural_bandit_site_distance_omt": 32.202484376209235,
                "persistent_ecology_unchanged": True,
                "persistent_ecology_state_before": "{\"sites\":[]}",
                "persistent_ecology_state_after": "{\"sites\":[]}",
                "bootstrap_radius_omt": 40,
                "bootstrap_created_sites": 1,
                "bootstrap_recognized_tiles": 4,
                "structural_scan_budget": 12,
                "structural_scan_budget_used": 4,
                "structural_scan_sites_considered": 1,
                "structural_scan_candidates_sampled": 4,
                "structural_scan_notes": ["bounded"],
                "candidate_prefix_limit": 3,
                "candidate_rows": [
                    {
                        "lead_id": f"lead-{index}",
                        "target_omt": {"x": index, "y": 0, "z": 0},
                        "selector": "non_frontier",
                        "outcome": "rejected",
                        "watch": None,
                        "watch_route_cost": -1,
                        "analyzer_record": "outcome=rejected",
                        "watch_budget_before": 8,
                        "watch_budget_after": 8,
                        "read": {
                            "reachable": False,
                            "complete_route_cost": 22,
                            "max_segment_risk": 400,
                            "summary": "route cap exceeded",
                            "watch_geography_supplied": False,
                            "watch_candidates": [],
                            "watch_shared_route": [],
                        },
                    }
                    for index in range(3)
                ],
                "route_watch_budget_before": 8,
                "route_watch_budget_after": 8,
                "route_watch_budget_used": 0,
            }
            (run_dir / "harness_new_world_feasibility.json").write_text(
                json.dumps(artifact), encoding="utf-8"
            )
            self.assertEqual(harness.load_harness_feasibility_artifact(run_dir), artifact)
            self.assertTrue(harness.harness_feasibility_artifact_ready(artifact))
            self.assertTrue(harness.harness_feasibility_artifact_ready(artifact, "830205385"))
            self.assertFalse(harness.harness_feasibility_artifact_ready(artifact, "830205018"))
            artifact["persistent_ecology_unchanged"] = False
            self.assertFalse(harness.harness_feasibility_artifact_ready(artifact))
            artifact["persistent_ecology_unchanged"] = True
            artifact["candidate_rows"].pop()
            self.assertFalse(harness.harness_feasibility_artifact_ready(artifact))
        self.assertFalse(harness.harness_feasibility_artifact_ready({}))

    def test_cleanup_accepts_supported_executable_identities(self) -> None:
        class TerminatingInspector:
            def __init__(self, command: str) -> None:
                self.command = command
                self.alive = True

            def inspect(self, pid: int):
                return harness.ProcessSnapshot(
                    pid=pid, alive=self.alive, birth_identity="birth-123", command=self.command,
                )

        for command in (
            "/game/Cataclysm-AOL --userdir .userdata/p/",
            "/game/Cataclysm-AOL.exe --userdir .userdata/p/",
            "/game/cataclysm-tiles --userdir .userdata/p/",
            "/game/cataclysm-tlg-tiles.exe --userdir .userdata/p/",
            "/game/r027-closure-007-tiles --userdir .userdata/r027/",
        ):
            inspector = TerminatingInspector(command)
            expected = {"pid": 123, "birth_identity": "birth-123", "command": command}
            def signal(pid: int, _signal: int) -> None:
                self.assertEqual(pid, 123)
                inspector.alive = False
            with self.subTest(command=command), patch.object(harness.os, "kill", side_effect=signal) as kill:
                result = harness.cleanup_game_process(
                    123, grace_seconds=0.0, explicit_quit=True,
                    expected_process_generation=expected, inspector=inspector,
                )
            self.assertEqual(result["status"], "terminated")
            kill.assert_called_once()

    def test_plan_rejects_fixture_world_and_reused_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with patch.object(harness, "repo_root", return_value=root), patch.object(
                harness, "detect_executable", return_value=root / "game"
            ), patch.object(harness, "create_run_dir", return_value=root / "run"):
                with self.assertRaisesRegex(ValueError, "cannot be combined"):
                    harness.build_plan("profile", "", "fixture", "natural-r002", "830205018")

                (root / ".userdata" / "profile" / "save" / "natural-r002").mkdir(parents=True)
                with self.assertRaisesRegex(ValueError, "already exists"):
                    harness.build_plan("profile", "", "", "natural-r002", "830205018")

    def test_plan_propagates_named_world_and_raw_seed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with patch.object(harness, "repo_root", return_value=root), patch.object(
                harness, "detect_executable", return_value=root / "game"
            ), patch.object(harness, "create_run_dir", return_value=root / "run"):
                plan = harness.build_plan("profile", "", "", "natural-r002", "830205018")
        self.assertEqual(plan.strategy, "harness_new_world")
        self.assertEqual(plan.target_world, "natural-r002")
        self.assertEqual(plan.harness_new_world, "natural-r002")
        self.assertEqual(plan.harness_raw_seed, "830205018")
        self.assertFalse(plan.harness_bandit_feasibility)

    def test_plan_requires_explicit_fresh_world_feasibility_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with patch.object(harness, "repo_root", return_value=root), patch.object(
                harness, "detect_executable", return_value=root / "game"
            ), patch.object(harness, "create_run_dir", return_value=root / "run"):
                with self.assertRaisesRegex(ValueError, "requires harness new-world mode"):
                    harness.build_plan("profile", "", "", harness_bandit_feasibility=True)
                with self.assertRaisesRegex(ValueError, "requires raw seed 830205385"):
                    harness.build_plan(
                        "profile", "", "", "natural-r002", "830205018", True
                    )
                plan = harness.build_plan(
                    "profile", "", "", "natural-r002", "830205385", True
                )
        self.assertTrue(plan.harness_bandit_feasibility)

    def test_failed_run_cleanup_removes_only_the_named_fresh_world(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            world_dir = root / ".userdata" / "profile" / "save" / "natural-r002"
            other_world = root / ".userdata" / "profile" / "save" / "keep-me"
            world_dir.mkdir(parents=True)
            other_world.mkdir()
            run_dir = root / "run"
            run_dir.mkdir()
            (world_dir / ".openclaw-harness-owner").write_text(str(run_dir.resolve()), encoding="utf-8")
            with patch.object(harness, "repo_root", return_value=root):
                result = harness.cleanup_harness_world("profile", "natural-r002", run_dir)
            self.assertEqual(result["status"], "removed")
            self.assertFalse(world_dir.exists())
            self.assertTrue(other_world.exists())

    def test_failed_run_cleanup_refuses_unowned_world(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            world_dir = root / ".userdata" / "profile" / "save" / "natural-r002"
            world_dir.mkdir(parents=True)
            (world_dir / ".openclaw-harness-owner").write_text("another-run", encoding="utf-8")
            run_dir = root / "run"
            run_dir.mkdir()
            with patch.object(harness, "repo_root", return_value=root):
                result = harness.cleanup_harness_world("profile", "natural-r002", run_dir)
            self.assertEqual(result["status"], "skipped_unowned_world")
            self.assertTrue(world_dir.exists())


def generation_scenario():
    from scenario_registry import PROOF_ROUTE_ROLES
    return {
        "name": "terminal.offline_generation", "manifest_version": 1,
        "profile": "offline", "world": "", "fixture": "",
        "harness_new_world": "ordinary-offline", "harness_raw_seed": "830205385",
        "harness_new_world_scenario": "evacuee",
        "capabilities": {"runtime.entry_mode": "native_world_generation"},
        "steps": [{"kind": "native_world_generation", "label": "generate_save"}],
        "proof_route": {role: ["generate_save"] for role in PROOF_ROUTE_ROLES},
        "runtime_contract": {
            "playtest_mode": "terminal", "terminal_transport": "pty",
            "permitted_input": [], "forbidden_input": ["keyboard"],
            "setup_only_debug": True, "disposable_copy": True, "helpers": [],
            "permissions": [], "platform": ["macos"], "profile": "offline",
            "fixture": "", "grants_gameplay_proof": False,
            "requirements": {"os": "macos", "source": "selected-build", "profile": "offline",
                "fixture": "", "helper": "none", "peekaboo": False, "input": [],
                "ocr": False, "cleanup": True},
        },
    }


def generation_startup(run_dir):
    return {
        "ok": True, "reason": "harness_world_saved", "strategy": "harness_new_world",
        "pid": 101, "profile": "offline", "run_dir": str(run_dir), "exit_code": 0,
        "feature_proof": False,
        "generation_identity": {"world": "ordinary-offline", "raw_seed": "830205385",
            "ordinary_scenario": "evacuee", "profile": "offline", "run_dir": str(run_dir), "run_id": "offline-run"},
        "readiness": {"kind": "harness_process_exit", "pid": 101, "returncode": 0,
            "world_owned_by_run": True, "feasibility_artifact_ready": True,
            "marker": {"mtime": 20.0, "world_name": "ordinary-offline"}, "baseline_save_marker_mtime": 10.0,
            "debug_error_evidence_count": 0},
    }


class RegisteredGenerationOnlyTest(unittest.TestCase):
    def test_registry_declaration_and_terminal_readiness_share_generation_contract(self):
        from scenario_registry import validate_manifest
        scenario = generation_scenario()
        with tempfile.TemporaryDirectory() as raw:
            declaration = Path(raw)/"offline-generation.json"
            declaration.write_text(json.dumps(scenario))
            validate_manifest(scenario, path=declaration)
        with patch.object(harness.sys, "platform", "darwin"), patch.object(
            harness, "validate_selected_product_build", return_value=({
                "build_configuration": {"renderer": "curses", "make_variables": ["SOUND=0"]}}, "")):
            self.assertEqual(harness.terminal_mode_readiness(scenario, Path("game"))["status"], "ready")
        self.assertEqual(harness.declared_generation_only(scenario), {
            "world": "ordinary-offline", "raw_seed": "830205385", "ordinary_scenario": "evacuee"})

    def test_declared_generation_does_not_accept_feature_or_reused_world_operations(self):
        import copy
        for key, value in [("fixture", "old-fixture"), ("world", "existing"),
            ("saved_world_snapshot", "/old/save"), ("post_relaunch", {"steps": []}),
            ("advance_turns", 1), ("capture_world_after", True), ("harness_raw_seed", "0")]:
            with self.subTest(key=key):
                scenario = copy.deepcopy(generation_scenario()); scenario[key] = value
                with self.assertRaises(ValueError): harness.declared_generation_only(scenario)
        scenario = generation_scenario()
        scenario["steps"].append({"kind": "cockpit_live_session", "label": "play"})
        with self.assertRaises(ValueError): harness.declared_generation_only(scenario)

    def test_authenticated_save_exit_has_setup_credit_and_no_world_gate(self):
        with tempfile.TemporaryDirectory() as raw:
            report = harness.generation_only_probe_report(generation_scenario(), 0, generation_startup(Path(raw)), expected_profile="offline", expected_run_id="offline-run")
        self.assertTrue(report["ok"])
        self.assertFalse(report["feature_proof"])
        self.assertEqual(report["evidence_class"], "setup")
        self.assertEqual(report["screen"]["status"], "not_required_generation_only")

    def test_failure_missing_stale_save_wrong_owner_seed_or_scenario_stays_red(self):
        import copy
        base = generation_startup(Path("/offline/run"))
        variants = [({"exit_code": 1}, {}), ({"pid": 0}, {"pid": 0}), ({"profile": "other"}, {}), ({"ok": False}, {}),
                    ({"feature_proof": True}, {}), ({}, {"marker": {}}),
                    ({}, {"marker": {"mtime": 10}}), ({}, {"world_owned_by_run": False}),
                    ({}, {"debug_error_evidence_count": 1}), ({}, {"pid": 999})]
        for root_delta, ready_delta in variants:
            start = copy.deepcopy(base); start.update(root_delta); start["readiness"].update(ready_delta)
            with self.subTest(root=root_delta, readiness=ready_delta):
                self.assertFalse(harness.generation_only_probe_report(generation_scenario(), 0, start, expected_profile="offline", expected_run_id="offline-run")["ok"])
        for key, value in [("world", "different"), ("raw_seed", "42"),
                           ("ordinary_scenario", "missed"), ("profile", "other"),
                           ("run_dir", "/other/run"), ("run_id", "other-run")]:
            start = copy.deepcopy(base); start["generation_identity"][key] = value
            with self.subTest(identity=key):
                self.assertFalse(harness.generation_only_probe_report(generation_scenario(), 0, start, expected_profile="offline", expected_run_id="offline-run")["ok"])
        self.assertFalse(harness.generation_only_probe_report(generation_scenario(), 1, base, expected_profile="offline", expected_run_id="offline-run")["ok"])

    def test_saved_generation_normal_load_reuses_same_profile_bytes(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw); world = root/".userdata/offline/save/ordinary-offline"
            world.mkdir(parents=True); (world/"master.gsav").write_text("native-save-stub")
            before = (world/"master.gsav").read_bytes()
            with patch.object(harness, "repo_root", return_value=root), patch.object(
                harness.shutil, "copytree", side_effect=AssertionError("same-profile continuation copied data")):
                result = harness.install_saved_world_snapshot("offline", "ordinary-offline", str(world))
                with patch.object(harness, "detect_executable", return_value=root/"game"), patch.object(
                    harness, "create_run_dir", return_value=root/"run"):
                    plan = harness.build_plan("offline", "ordinary-offline", "")
            self.assertEqual(result["status"], "green_saved_world_snapshot_reused")
            self.assertEqual(plan.strategy, "autoload_world")
            self.assertEqual((world/"master.gsav").read_bytes(), before)



class GenerationRegistryFinalizationTest(unittest.TestCase):
    def test_native_generation_finalizer_ingests_setup_without_feature_verification(self):
        for variant in ("valid", "wrong_owner", "wrong_run", "missing_save", "failed_exit"):
            with self.subTest(variant=variant):
                self.exercise_finalization(variant)

    def exercise_finalization(self, variant):
        from contextlib import redirect_stdout
        import hashlib
        import io
        import scenario_registry_cli as cli
        import scenario_registry_store as store
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw).resolve(); run = root/"run"; run.mkdir()
            registry_path = root/"registry.sqlite3"
            scenario = generation_scenario(); source = root/"generation.json"
            source.write_text(json.dumps(scenario))
            connection = store.open_registry(str(registry_path))
            try:
                store.rebuild_manifest_projection(connection, source_paths=[source])
                selection = store.execute_registry_query(
                    connection, store.parse_registry_query_request({"requirements": [], "preferences": []}),
                    scenario_id=scenario["name"], coordinator_brief={"outcome": "generate an owned save only"})
                self.assertTrue(selection.token_id)
                token = selection.token_id
                authority = store.issue_wec_authority(
                    connection, evidence_class="setup support", authority="registry", run_id=token,
                    binding_id="e"*64, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
            finally:
                connection.close()
            native = generation_startup(run); native["generation_identity"]["run_id"] = token
            if variant == "wrong_owner": native["readiness"]["world_owned_by_run"] = False
            if variant == "wrong_run": native["generation_identity"]["run_id"] = "foreign-run"
            if variant == "missing_save": native["readiness"]["marker"] = {}
            if variant == "failed_exit": native["exit_code"] = 1
            accepted = variant == "valid"
            declaration = harness.load_scenario(scenario["name"], source_path=str(source))
            receipt = json.dumps({"registry_path": str(registry_path), "token_id": token,
                "source_path": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "wec_authority": authority, "runtime_binding": {"executable_sha256": "e"*64}})
            adapters = store.BindingAdapters(
                runtime=lambda _: {"status": "compatible", "facts": {"source_sha256": "a"*64, "executable_sha256": "e"*64}},
                fixture=lambda _: {"status": "compatible", "facts": {"source_sha256": "f"*64}},
                profile=lambda _: {"status": "compatible", "facts": {"source_sha256": "1"*64}})
            actual = []
            callback = cli._registry_post_finalize_ingest(receipt)
            def hook(path, payload):
                actual.append(callback(path, payload))
            args = harness.build_parser().parse_args(["probe", scenario["name"], "--profile", "offline"])
            args._loaded_scenario = declaration
            args.registry_launch_receipt = receipt
            args.registry_post_finalize_hook = hook
            with patch.object(cli, "production_binding_adapters", return_value=adapters), \
                 patch.object(harness, "repo_root", return_value=root), \
                 patch.object(harness, "detect_executable", return_value=root/"game"), \
                 patch.object(harness, "run_startup_in_process", return_value=(0, native, "native generator stub", "")), \
                 patch.object(harness, "current_owned_process_generation", return_value={
                     "status": "exited", "pid": 101, "expected": {"pid": 101}}), \
                 patch.object(harness, "cleanup_game_process", side_effect=AssertionError("offline test signalled process")), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(harness._run_probe_mode(args), 0 if accepted else 1)
            report = json.loads((run/"probe.report.json").read_text())
            connection = store.open_registry(str(registry_path))
            try:
                events = [(r["event_kind"],r["reason"]) for r in connection.execute(
                    "SELECT event_kind,reason FROM token_history WHERE token_id=? ORDER BY token_event_id", (token,))]
                ingestion_errors = [dict(r) for r in connection.execute(
                    "SELECT ingestion_status,error_text FROM report_ingestion_history")]
                counts = {t:connection.execute("SELECT count(*) FROM "+t).fetchone()[0]
                    for t in ["verification_history", "scenario_intervention_history"]}
                preparation = [r[0] for r in connection.execute(
                    "SELECT preparation_status FROM scenario_intervention_history")]
            finally:
                connection.close()
            print(json.dumps({"control": "native_generation_finalizer_ingestion", "variant": variant, "callback": actual,
                "token_events": events, "counts": counts, "preparation": preparation, "ingestion_errors": ingestion_errors, "native_game_started": False}))
            self.assertEqual(actual[0]["status"], "ingested_setup_only", actual)
            self.assertIn(("verification_run","setup_only_report_ingested"), events)
            self.assertFalse(any(kind=="invalidated" for kind,_ in events))
            self.assertEqual(counts["verification_history"], 0)
            self.assertFalse(report["feature_proof"])
            self.assertEqual(report["wec_authority"], authority)
            self.assertEqual(report["run_id"], token)
            self.assertEqual(report["binding_id"], "e"*64)
            self.assertEqual(counts["scenario_intervention_history"], 1)
            self.assertEqual(report["ok"], accepted)
            self.assertEqual(report["scenario_setup"]["interventions"][0]["native_receipt"]["accepted"], accepted)
            self.assertEqual(preparation, ["prepared" if accepted else "unprepared"])
            self.assertFalse(actual[0]["final_gates"]["automated_certification"])
            self.assertFalse(actual[0]["final_gates"]["windows_feel"])


if __name__ == "__main__":
    unittest.main()
