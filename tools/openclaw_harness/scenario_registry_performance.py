"""Narrow native performance evidence on the existing registry witness route.

The declaration authorizes collection, never gameplay or the planned load sweep.
``registry-record-witness`` accepts an ordinary cited statement plus optional
``performance_evidence`` containing ``behavior`` and ``measurements`` lists of
the existing evidence-event source handles (path, offset, length, sha256).
Those handles are reread from the bound native producers, not trusted summaries.
The current native save supplies census context, not an assertion that its final
population was present throughout a measurement. Coordinator review still owns
causal sufficiency, mixed-load opportunity, budgets and comparisons.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
from copy import deepcopy
from datetime import datetime
from pathlib import Path, PureWindowsPath
from typing import Any, Mapping


SCENARIO = "performance.mixed_hostile_windows_stress_r5"
SCHEMA = "caol-native-performance-witness-v1"
CAPABILITY = "capabilities.performance.mixed_hostile_windows_stress"
CONTRACT = {"schema": SCHEMA, "capability": CAPABILITY,
            "scope": "observed_native_work_interval"}


def performance_contract(declaration: Mapping[str, Any]) -> bool:
    """Validate explicit fresh authority; old declarations remain zero-credit."""
    evidence = declaration.get("evidence_contract", {})
    raw = evidence.get("performance_witness") if isinstance(evidence, Mapping) else None
    if raw is None:
        return False
    runtime = _object(declaration.get("runtime_contract"))
    requirements = _object(runtime.get("requirements"))
    capabilities = declaration.get("capabilities", {})
    steps = declaration.get("steps", [])
    if (not isinstance(raw, Mapping) or any(raw.get(key) != value for key, value in CONTRACT.items()) or
            declaration.get("name") != SCENARIO or
            declaration.get("manifest_version") != 1 or
            not isinstance(capabilities, Mapping) or
            capabilities.get(CAPABILITY) != "baseline_then_adaptive_active_party_expansion" or
            capabilities.get("runtime.native_transport") != "windows_conpty" or
            runtime.get("grants_gameplay_proof") is not False or
            runtime.get("disposable_copy") is not True or
            runtime.get("playtest_mode") != "terminal" or
            runtime.get("terminal_transport") != "pty" or
            runtime.get("platform") != ["windows"] or
            requirements.get("os") != "windows" or
            requirements.get("source") != "current-worktree" or
            declaration.get("fixture") or runtime.get("fixture") or declaration.get("post_relaunch") or
            runtime.get("require_screen_observability") is True or
            not isinstance(steps, list) or
            [step.get("kind") for step in steps if isinstance(step, Mapping)] !=
            ["native_semantic_bootstrap", "cockpit_live_session"]):
        raise ValueError("performance_witness requires the exact Windows native interval contract")
    # The native new-world command saves and exits. Its historical declarations
    # remain setup, never authority for an interactive interval.
    if declaration.get("harness_new_world") and raw == CONTRACT and not declaration.get("saved_world_snapshot"):
        return False
    initial = _object(raw.get("initial_world"))
    snapshot = declaration.get("saved_world_snapshot")
    startup = initial.get("generation_startup")
    absolute = lambda value: isinstance(value, str) and (Path(value).is_absolute() or PureWindowsPath(value).is_absolute())
    if (set(raw) not in ({*CONTRACT, "initial_world", "input_sha256"},
                         {*CONTRACT, "initial_world", "input_sha256", "parent_report"}) or
            set(initial) != {"sha256", "generation_startup", "startup_sha256", "runtime_sha256"} or
            not all(isinstance(initial.get(key), str) and re.fullmatch(r"[0-9a-f]{64}", initial[key])
                    for key in ("sha256", "startup_sha256", "runtime_sha256")) or
            not absolute(snapshot) or not absolute(startup) or not declaration.get("world") or
            any(declaration.get(key) for key in ("harness_new_world", "harness_raw_seed",
                                                "harness_new_world_scenario", "harness_bandit_feasibility"))):
        raise ValueError("performance_witness requires the generated saved-world continuation and its exact source hashes")
    if declaration.get("capture_world_after") is not True:
        raise ValueError("performance_witness requires the current saved-world reference producer")
    if not _digest(raw.get("input_sha256")):
        raise ValueError("performance_witness requires an exact input snapshot hash")
    if "parent_report" in raw:
        parent = _object(raw["parent_report"])
        if set(parent) != {"path", "sha256"} or not absolute(parent.get("path")) or not _digest(parent.get("sha256")):
            raise ValueError("performance_witness requires one exact parent report reference")
    return True


def _digest(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError("performance_witness:" + reason)


def _object(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _number(value: Any, *, positive: bool = False) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and (value > 0 if positive else value >= 0)


def _json_file(path: Path) -> Mapping[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    _require(isinstance(value, Mapping), "source_not_object")
    return value


def _initial_world(declaration: Mapping[str, Any]) -> dict[str, Any]:
    """Read the completed native generator, not a caller's claim of freshness."""
    from startup_harness import harness_process_ready, sha256_tree, userdir_for_profile
    initial = _object(_object(_object(declaration.get("evidence_contract")).get("performance_witness")).get("initial_world"))
    startup_path = Path(str(initial.get("generation_startup", ""))).resolve()
    _require(startup_path.name == "startup.result.json" and
             hashlib.sha256(startup_path.read_bytes()).hexdigest() == initial.get("startup_sha256"),
             "generation_startup_source_mismatch")
    startup = _json_file(startup_path)
    origin = startup_path.parent
    identity, readiness = _object(startup.get("generation_identity")), _object(startup.get("readiness"))
    world, profile, run_id = identity.get("world"), identity.get("profile"), identity.get("run_id")
    source = (userdir_for_profile(str(profile)) / "save" / str(world)).resolve()
    base = str(declaration.get("profile", ""))
    _require(isinstance(profile, str) and (profile == base or re.fullmatch(re.escape(base) + r"-run-[0-9a-f]{32}", profile)) and
             world == declaration.get("world") == startup.get("harness_new_world") and
             profile == startup.get("profile") and bool(run_id) and
             identity.get("raw_seed") == startup.get("harness_raw_seed") and bool(identity.get("raw_seed")) and
             Path(str(identity.get("run_dir", ""))).resolve() == origin == Path(str(startup.get("run_dir", ""))).resolve() and
             source == (userdir_for_profile(profile) / "save" / str(world)).resolve(),
             "generated_world_identity_mismatch")
    source_hash, error = sha256_tree(source)
    _require(not error and source_hash == initial.get("sha256"), "initial_save_hash_mismatch")
    _require(startup.get("ok") is True and startup.get("strategy") == "harness_new_world" and
             startup.get("reason") == "harness_world_saved" and startup.get("exit_code") == 0 and
             startup.get("feature_proof") is False and readiness.get("kind") == "harness_process_exit" and
             readiness.get("pid") == startup.get("pid") and readiness.get("returncode") == 0 and
             readiness.get("world_owned_by_run") is True and readiness.get("save_marker_created") is True and
             _object(readiness.get("marker")).get("world_name") == world and
             harness_process_ready(0, _object(readiness.get("marker")), readiness.get("baseline_save_marker_mtime", 0),
                                   readiness.get("debug_error_evidence_count", -1)), "native_generation_unproved")
    runtime_path = origin / "runtime.binding.json"
    _require(hashlib.sha256(runtime_path.read_bytes()).hexdigest() == initial.get("runtime_sha256"),
             "generation_runtime_source_mismatch")
    runtime, owner = _json_file(runtime_path), _json_file(origin / "terminal.owner.json")
    generation = _object(owner.get("game_process_generation"))
    _require(owner.get("run_id") == run_id and owner.get("game_pid") == startup.get("pid") == generation.get("pid") and
             bool(generation.get("birth_identity")) and owner.get("launch_state") == "exited" and
             owner.get("child_exit_code") == 0 and owner.get("native_handles_closed") is True and
             owner.get("native_lease_handles_closed") is True and
             Path(str(generation.get("executable_path", ""))).resolve() ==
             Path(str(startup.get("executable_path", ""))).resolve() ==
             Path(str(runtime.get("executable_path", ""))).resolve() and
             all(isinstance(runtime.get(key), str) and re.fullmatch(r"[0-9a-f]{64}", runtime[key])
                 for key in ("runtime_source_sha256", "executable_sha256")),
             "generation_native_owner_mismatch")
    return {"source": str(source), **dict(initial), "world": world, "run_id": run_id,
            "raw_seed": identity["raw_seed"], "runtime": runtime}


def prepare_saved_world_continuation(declaration: Mapping[str, Any], generation_startup: str | Path,
                                     *, saved_world_snapshot: str | Path | None = None,
                                     parent_report: str | Path | None = None) -> dict[str, Any]:
    """Select an origin or retained native save through the ordinary snapshot route.

    A non-origin input without a captured parent is a noncredit bootstrap. Its
    fresh native save/report can establish custody for later work; it cannot
    invent the missing earlier snapshot or attest any earlier interval.
    """
    from startup_harness import sha256_tree, userdir_for_profile
    from cockpit_report_reference import load_report
    path = Path(generation_startup).resolve()
    startup = _json_file(path)
    result = deepcopy(dict(declaration))
    existing = _object(_object(_object(result.get("evidence_contract")).get("performance_witness")).get("initial_world"))
    if not existing:
        _require(declaration.get("name") == SCENARIO and
                 declaration.get("harness_new_world") == startup.get("harness_new_world") and
                 declaration.get("harness_raw_seed") == startup.get("harness_raw_seed"), "declared_generation_mismatch")
        origin = userdir_for_profile(str(startup.get("profile", ""))) / "save" / str(startup.get("harness_new_world", ""))
        digest, error = sha256_tree(origin)
        _require(not error, "initial_save_unavailable")
        existing = {"sha256": digest, "generation_startup": str(path),
            "startup_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "runtime_sha256": hashlib.sha256((path.parent / "runtime.binding.json").read_bytes()).hexdigest()}
    _require(Path(str(existing.get("generation_startup", ""))).resolve() == path, "generation_startup_source_mismatch")
    result.update(world=startup["harness_new_world"], config_profile=declaration["profile"], capture_world_after=True)
    for key in ("harness_new_world", "harness_raw_seed", "harness_new_world_scenario", "harness_bandit_feasibility"):
        result.pop(key, None)
    raw = {**CONTRACT, "initial_world": dict(existing)}
    result["evidence_contract"]["performance_witness"] = raw
    initial = _initial_world(result)
    source = Path(saved_world_snapshot).resolve() if saved_world_snapshot is not None else Path(initial["source"])
    if parent_report is not None:
        report_path, report_hash, parent = load_report(parent_report)
        snapshot = _object(parent.get("world_snapshot"))
        _require(snapshot.get("status") == "current_saved_world_reference" and
                 _digest(snapshot.get("source_sha256")) and bool(snapshot.get("source")),
                 "saved_world_producer_mismatch")
        parent_source = Path(str(snapshot.get("source", ""))).resolve()
        _require(saved_world_snapshot is None or source == parent_source, "parent_snapshot_path_mismatch")
        source = parent_source
        raw["parent_report"] = {"path": report_path, "sha256": report_hash}
    profile = source.parent.parent.name
    base = str(result["profile"])
    _require((profile == base or re.fullmatch(re.escape(base) + r"-run-[0-9a-f]{32}", profile)) and
             source == (userdir_for_profile(profile) / "save" / str(result["world"])).resolve(),
             "generated_world_identity_mismatch")
    digest, error = sha256_tree(source)
    _require(not error, "input_save_unavailable")
    result["saved_world_snapshot"] = str(source)
    raw["input_sha256"] = digest
    performance_contract(result)
    _validate_lineage(result)
    return result


def _source_record(handle: Any, expected_path: Path) -> tuple[Mapping[str, Any], dict[str, Any]]:
    """Use the existing exact byte-range handle emitted by evidence_events."""
    value = _object(handle)
    _require({"path", "offset", "length", "sha256"} <= set(value), "invalid_source_handle")
    path = Path(str(value["path"])).resolve()
    offset, length = value["offset"], value["length"]
    _require(path == expected_path.resolve() and type(offset) is int and offset >= 0 and
             type(length) is int and length > 0, "source_path_or_range_mismatch")
    with path.open("rb") as stream:
        # A handle must select exactly one complete JSONL record, not a substring
        # that could relabel an inner object as the native producer's record.
        if offset:
            stream.seek(offset - 1)
            _require(stream.read(1) == b"\n", "source_not_record_boundary")
        stream.seek(offset)
        raw = stream.read(length)
    _require(len(raw) == length and raw.endswith(b"\n") and
             hashlib.sha256(raw).hexdigest() == value["sha256"], "source_hash_or_length_mismatch")
    record = json.loads(raw)
    _require(isinstance(record, Mapping), "source_not_object")
    return record, {key: value[key] for key in ("path", "offset", "length", "sha256")}


def performance_report_lineage(declaration: Mapping[str, Any]) -> dict[str, Any]:
    """Only selected continuation inputs, not a declaration/history archive."""
    return {key: deepcopy(declaration[key]) for key in ("world", "profile", "saved_world_snapshot")} | {
        "performance_witness": deepcopy(declaration["evidence_contract"]["performance_witness"])}


def _report_lineage(declaration: Mapping[str, Any], report: Mapping[str, Any],
                    run_id: str, binding_id: str) -> None:
    from wec_evidence import validate_authority_fact
    manifest = _object(report.get("scenario_manifest"))
    source = _object(manifest.get("source"))
    normalized = _object(manifest.get("normalized"))
    _require(all(normalized.get(key) == {"state": "declared", "review_required": False, "value": declaration.get(key)}
                 for key in ("manifest_version", "capabilities", "runtime_contract", "proof_route")),
             "report_contract_incompatible")
    authority = validate_authority_fact(report.get("wec_authority"))
    fact = _object(authority.get("fact"))
    _require(_digest(source.get("sha256")) and bool(source.get("path")) and
             authority.get("status") == "sealed" and fact.get("source_sha256") == source["sha256"] and
             fact.get("run_id") == report.get("run_id") == run_id and
             bool(report.get("binding_id")) and fact.get("binding_id") == report["binding_id"] and
             fact.get("evidence_class") != "diagnostic replay" and
             manifest.get("performance_lineage") == performance_report_lineage(declaration),
             "report_lineage_source_mismatch")


def _validate_lineage(declaration: Mapping[str, Any], *, source_path: str | None = None) -> dict[str, Any]:
    """Follow one immediate existing-report edge at a time, never credit its work."""
    from cockpit_report_reference import load_report
    from startup_harness import sha256_tree, userdir_for_profile
    initial = _initial_world(declaration)
    current = declaration
    visited_paths, visited_runs = set(), set()
    edges = 0
    immediate = None
    expected_source = Path(source_path).resolve() if source_path else None
    while True:
        performance_contract(current)
        raw = _object(_object(current.get("evidence_contract")).get("performance_witness"))
        _require(raw.get("initial_world") == _object(_object(declaration.get("evidence_contract")).get("performance_witness")).get("initial_world") and
                 current.get("world") == declaration.get("world") and current.get("profile") == declaration.get("profile"),
                 "parent_origin_mismatch")
        source = Path(str(current["saved_world_snapshot"])).resolve()
        expected_hash = raw["input_sha256"]
        profile = source.parent.parent.name
        base = str(declaration["profile"])
        _require((profile == base or re.fullmatch(re.escape(base) + r"-run-[0-9a-f]{32}", profile)) and
                 source == (userdir_for_profile(profile) / "save" / str(declaration["world"])).resolve(),
                 "generated_world_identity_mismatch")
        if not edges:
            digest, error = sha256_tree(source)
            _require(not error and digest == expected_hash, "input_save_hash_mismatch")
        parent = _object(raw.get("parent_report"))
        if not parent:
            origin_input = source == Path(initial["source"]) and expected_hash == initial["sha256"]
            break
        parent_path = Path(str(parent["path"])).resolve()
        _require(parent_path not in visited_paths, "parent_report_cycle")
        visited_paths.add(parent_path)
        canonical, digest, report = load_report(parent_path)
        _require(digest == parent["sha256"], "parent_report_source_mismatch")
        manifest = _object(report.get("scenario_manifest"))
        parent_source = _object(manifest.get("source")).get("path")
        _require(isinstance(parent_source, str) and Path(parent_source).is_absolute(), "parent_source_path_mismatch")
        if expected_source is None:
            expected_source = Path(parent_source).resolve()
        _require(Path(parent_source).resolve() == expected_source, "parent_source_path_mismatch")
        metadata = _object(manifest.get("performance_lineage"))
        _require(set(metadata) == {"world", "profile", "saved_world_snapshot", "performance_witness"},
                 "parent_lineage_missing")
        parent_decl = deepcopy(dict(declaration))
        for key in ("world", "profile", "saved_world_snapshot"):
            parent_decl[key] = metadata[key]
        parent_decl["evidence_contract"]["performance_witness"] = deepcopy(metadata["performance_witness"])
        performance_contract(parent_decl)
        finals = [_object(step.get("cockpit_live_session")).get("final") for step in report.get("steps", [])
                  if isinstance(step, Mapping) and _object(step.get("cockpit_live_session")).get("final")]
        _require(len(finals) == 1 and isinstance(finals[0], Mapping), "parent_native_report_required")
        final = finals[0]
        journal = _object(_object(final.get("stop_detail")).get("evidence_journal"))
        runtime = _json_file(parent_path.parent / "runtime.binding.json")
        context = _native_saved_report(declaration=parent_decl, report=report, report_path=parent_path,
                                       final=final, journal=journal, runtime=runtime, current=False)
        parent_run = context["run_id"]
        _require(parent_run not in visited_runs and parent_run != initial["run_id"], "parent_run_cycle")
        visited_runs.add(parent_run)
        snapshot = context["snapshot"]
        _require(source == Path(snapshot["source"]).resolve() and expected_hash == snapshot["source_sha256"],
                 "parent_snapshot_mismatch")
        if not edges:
            immediate = {"path": canonical, "sha256": digest}
        edges += 1
        current = parent_decl
    return {"input_continuity": "captured_native_parent" if edges else ("generated_origin" if origin_input else "noncredit_bootstrap"),
            "parent_report": immediate, "validated_parent_edges": edges, "parent_runs": visited_runs,
            "prior_interval_hash_missing": not origin_input,
            "scope": "current observed interval only; parent saves establish input custody, never inherited work credit"}


def _native_saved_report(*, declaration: Mapping[str, Any], report: Mapping[str, Any], report_path: Path,
                         final: Mapping[str, Any], journal: Mapping[str, Any], runtime: Mapping[str, Any],
                         current: bool, citations: set | None = None) -> dict[str, Any]:
    """One native save/exit and installed input, independent of interval credit."""
    _require(journal.get("schema") == "caol-playtest-evidence-journal-v1", "one_native_run_required")
    identity = _object(journal.get("identity"))
    run_id, binding_id = identity.get("run_id"), identity.get("binding_id")
    _report_lineage(declaration, report, run_id, binding_id)
    live = [_object(step.get("cockpit_live_session")) for step in report.get("steps", [])
            if isinstance(step, Mapping) and _object(step.get("cockpit_live_session")).get("final")]
    _require(len(live) == 1 and _object(live[0].get("final")).get("run_id") == run_id and
             _object(live[0].get("final")).get("binding_id") == binding_id and
             _object(live[0].get("descriptor")).get("schema") == "caol-cockpit-live-session-v1" and
             _object(live[0].get("descriptor")).get("run_id") == run_id and
             _object(live[0].get("descriptor")).get("binding_id") == binding_id and
             _object(live[0].get("descriptor")).get("bridge_binding_id") == binding_id,
             "native_descriptor_binding_mismatch")
    _require(identity.get("scenario_id") == SCENARIO and report.get("scenario") == SCENARIO and
             identity.get("source_identity") == runtime.get("runtime_source_sha256") and
             identity.get("executable_identity") == runtime.get("executable_sha256"), "runtime_identity_mismatch")
    _require(not report.get("diagnostic_replay") and final.get("state") == "finished" and
             final.get("closure_kind") != "explicit_abort", "non_native_terminal")
    detail = _object(final.get("stop_detail"))
    retained = _object(detail.get("evidence_journal"))
    _require(retained.get("journal_sha256") == journal.get("journal_sha256"), "journal_not_from_report")

    entries = journal.get("entries", [])
    exits = [entry for entry in entries if entry.get("kind") == "observation" and
             _object(_object(entry.get("value")).get("value")).get("surface", {}).get("kind") == "process_exited"]
    _require(len(exits) == 1 and (citations is None or exits[0].get("citation_id") in citations), "cited_os_exit_required")
    process = _object(_object(_object(exits[0]["value"]).get("value")).get("surface")).get("facts", {})
    process = _object(process)
    generation = _object(process.get("process_generation"))
    _require(process.get("run_id") == run_id and process.get("alive") is False and
             process.get("identity_status") == "exited" and process.get("exit_code") == 0 and
             type(process.get("pid")) is int and process["pid"] > 0 and
             generation.get("pid") == process["pid"] and bool(generation.get("birth_identity")),
             "os_exit_identity_unproved")
    exit_path = Path(str(process.get("evidence_ref", ""))).resolve()
    _require(exit_path.name == "game-process-exit.json", "bound_exit_source_required")
    exit_record = _json_file(exit_path)
    owner = _json_file(exit_path.parent / "game-process.json")
    bridge = _json_file(exit_path.parent / "bridge.manifest.json")
    _require(bridge.get("authorization_token_id") == run_id and bridge.get("binding_id") == binding_id,
             "selected_bridge_authority_mismatch")
    for record in (owner, exit_record):
        _require(record.get("run_id") == run_id and record.get("binding_id") == binding_id and
                 record.get("pid") == process["pid"] and
                 _object(record.get("process_generation")).get("birth_identity") == generation["birth_identity"] and
                 _object(record.get("process_generation")).get("pid") == generation["pid"] and
                 Path(str(_object(record.get("process_generation")).get("executable_path", ""))).resolve() ==
                 Path(str(generation.get("executable_path", ""))).resolve(),
                 "process_owner_mismatch")
    _require(exit_record.get("exit_code") == 0, "abnormal_exit")
    logs = _object(owner.get("log_paths"))
    transition_path = Path(str(_object(logs.get("transition_events")).get("path", ""))).resolve()
    run_dir = report_path.parent.resolve()
    _require(transition_path == run_dir / "transition.events.jsonl", "transition_source_not_report_bound")
    runtime_path = run_dir / "runtime.binding.json"
    runtime_record = _json_file(runtime_path)
    screen = _object(_object(report.get("startup")).get("screen"))
    observed = _object(screen.get("runtime_binding_observed"))
    _require(screen.get("runtime_binding_status") == observed.get("status") == "matched", "native_product_binding_unmatched")
    _require(all(runtime_record.get(key) == runtime.get(key) == observed.get(key) for key in
                 ("runtime_source_sha256", "executable_sha256", "executable_path")), "native_runtime_binding_mismatch")
    _require(Path(str(generation.get("executable_path", ""))).resolve() ==
             Path(str(runtime.get("executable_path", ""))).resolve(), "process_executable_mismatch")
    _require(all(_digest(runtime.get(key)) for key in ("runtime_source_sha256", "executable_sha256")),
             "native_runtime_binding_mismatch")
    if current:
        _require(hashlib.sha256(Path(str(runtime.get("executable_path", ""))).read_bytes()).hexdigest() ==
                 runtime.get("executable_sha256"), "executable_bytes_drift")

    save = _object(final.get("native_save_completion"))
    completion = _object(save.get("completion"))
    world_receipt = _object(save.get("world_action_receipt"))
    terminal_receipt = _object(save.get("terminal_action_receipt"))
    contract = _object(report.get("contract"))
    world_name = declaration.get("world")
    _require(not contract.get("post_relaunch"), "extra_native_relaunch_unbound")
    _require(contract.get("world") == world_name and owner.get("world") == world_name and
             owner.get("playtest_mode") == "terminal", "world_or_transport_owner_mismatch")
    _require(save.get("status") == "matched" and completion.get("schema") == "caol-native-save-completion-v1" and
             completion.get("run_id") == run_id and completion.get("requested_run_id") == run_id and
             completion.get("action_id") == "world.save_quit" and completion.get("save_succeeded") is True and
             completion.get("serializer_result") == "saved" and
             completion.get("world_name") == world_name and bool(completion.get("player_save_id")) and
             world_receipt.get("action_id") == "world.save_quit" and world_receipt.get("accepted") is True and
             world_receipt.get("run_id") == run_id and terminal_receipt.get("accepted") is True and
             terminal_receipt.get("run_id") == run_id and
             all(completion.get(key) and completion.get(key) == world_receipt.get(key) for key in
                 ("request_id", "requested_run_id", "requested_surface_id", "requested_frame_id")) and
             _object(completion.get("artifact_identity")).get("kind") == "harness_run_directory" and
             Path(str(_object(completion.get("artifact_identity")).get("value", ""))).resolve() == run_dir,
             "native_persistence_unproved")
    # Existing producer owns the profile-to-userdir mapping. No caller may
    # substitute an old fixture or another world's dimension_data.gsav.
    from startup_harness import userdir_for_profile, sha256_tree
    userdir = userdir_for_profile(str(contract.get("profile", ""))).resolve()
    base_profile = str(declaration.get("profile", ""))
    profile = str(contract.get("profile", ""))
    startup, plan = _object(report.get("startup")), _json_file(run_dir / "plan.json")
    installed = _object(startup.get("saved_world_snapshot"))
    _require((profile == base_profile or re.fullmatch(re.escape(base_profile) + r"-run-[0-9a-f]{32}", profile)) and
             startup.get("profile") == plan.get("profile") == profile and
             plan.get("strategy") == "autoload_world" and plan.get("target_world") == world_name and
             Path(str(plan.get("userdir", ""))).resolve() == userdir and
             contract.get("config_profile", base_profile) == base_profile and
             Path(str(owner.get("userdir", ""))).resolve() == userdir, "profile_owner_mismatch")
    world = userdir / "save" / str(world_name)
    lineage = _object(_object(declaration.get("evidence_contract")).get("performance_witness"))
    expected_hash = lineage["input_sha256"]
    _require(installed.get("status") in ("green_saved_world_snapshot_installed", "green_saved_world_snapshot_reused") and
             Path(str(installed.get("source", ""))).resolve() == Path(str(declaration.get("saved_world_snapshot", ""))).resolve() and
             Path(str(installed.get("destination", ""))).resolve() == world.resolve() and
             installed.get("source_sha256") == installed.get("installed_sha256") == expected_hash,
             "initial_save_installation_mismatch")
    snapshot = _object(report.get("world_snapshot"))
    _require(contract.get("capture_world_after") is True and
             _json_file(run_dir / "saved_world.json") == snapshot, "saved_world_producer_mismatch")
    _require(snapshot.get("status") == "current_saved_world_reference" and
             snapshot.get("profile") == profile and snapshot.get("world") == world_name and
             Path(str(snapshot.get("source", ""))).resolve() == world.resolve() and
             _digest(snapshot.get("source_sha256")), "saved_world_drift")
    if current:
        _require(snapshot.get("source_sha256") == sha256_tree(world)[0], "saved_world_drift")
    return {"run_id": run_id, "binding_id": binding_id, "process": process, "generation": generation,
            "exit_path": exit_path, "exit_record": exit_record, "runtime_path": runtime_path,
            "transition_path": transition_path, "world": world, "snapshot": snapshot}


def validate_performance_witness(*, declaration: Mapping[str, Any], report: Mapping[str, Any],
                                 report_path: Path, final: Mapping[str, Any],
                                 journal: Mapping[str, Any], statement: Mapping[str, Any],
                                 runtime: Mapping[str, Any]) -> dict[str, Any]:
    """Ground only native work actually witnessed, without full-sweep promotion."""
    declared = performance_contract(declaration)
    requested = statement.get("evidence_ceiling", journal.get("evidence_ceiling", "zero-credit"))
    if requested in {"zero-credit", "setup-only", "diagnostic"}:
        _require(not statement.get("performance_evidence"), "negative_ceiling_has_no_performance_credit")
        return {"scope": "none", "whole_sequence_proven": False}
    _require(declared and requested == "focused", "undeclared_or_excessive_authority")
    if statement.get("verdict") != "proved":
        _require(not statement.get("performance_evidence"), "unproved_statement_has_no_performance_credit")
        return {"scope": "none", "whole_sequence_proven": False}
    initial = _initial_world(declaration)
    lineage = _validate_lineage(declaration, source_path=_object(_object(report.get("scenario_manifest")).get("source")).get("path"))
    _require(lineage["input_continuity"] != "noncredit_bootstrap", "unattested_input_requires_bootstrap")
    _require(initial["run_id"] != _object(journal.get("identity")).get("run_id"), "generation_source_or_run_mismatch")
    citations = {item.get("citation_id") for item in statement.get("citations", []) if isinstance(item, Mapping)}
    context = _native_saved_report(declaration=declaration, report=report, report_path=report_path,
        final=final, journal=journal, runtime=runtime, current=True, citations=citations)
    run_id, binding_id = context["run_id"], context["binding_id"]
    _require(initial["run_id"] != run_id and run_id not in lineage.pop("parent_runs"), "generation_source_or_run_mismatch")
    process, generation = context["process"], context["generation"]
    exit_path, exit_record = context["exit_path"], context["exit_record"]
    runtime_path, transition_path, world = context["runtime_path"], context["transition_path"], context["world"]
    from startup_harness import summarize_bandit_live_world_site
    dimension_path = world / "dimension_data.gsav"
    stat = dimension_path.stat()
    birth = str(generation["birth_identity"])
    _require(birth.startswith("windows-filetime:") and birth.split(":", 1)[1].isdigit(), "native_birth_domain_required")
    birth_ns = (int(birth.split(":", 1)[1]) - 116444736000000000) * 100
    exit_ns = int(datetime.fromisoformat(str(exit_record.get("exit_observed_at", ""))).timestamp() * 1_000_000_000)
    # The exit producer quantizes ISO time to milliseconds. Use that recorded
    # millisecond's end, not an invented grace window or a current wall clock.
    _require(birth_ns <= stat.st_mtime_ns < exit_ns + 1_000_000, "save_outside_native_process_lifetime")
    raw = dimension_path.read_bytes()
    after = dimension_path.stat()
    _require(all(getattr(after, field) == getattr(stat, field) for field in
                 ("st_dev", "st_ino", "st_size", "st_mtime_ns")), "save_changed_during_read")
    payload = json.loads(raw.split(b"\n", 1)[1] if raw.startswith(b"#") else raw)
    overmap = _object(_object(payload).get("overmapbuffer"))
    _require(isinstance(overmap.get("bandit_live_world"), Mapping) and all(
        isinstance(overmap.get(key), (Mapping, list)) for key in ("migo_populations", "migo_towers")),
             "native_census_schema_missing")
    raw_sites = _object(overmap.get("bandit_live_world")).get("sites")
    _require(isinstance(raw_sites, list) and all(isinstance(site, Mapping) for site in raw_sites),
             "native_census_sites_missing")
    sites = []
    site_members = {}
    for raw_site in raw_sites:
        summary = summarize_bandit_live_world_site(raw_site)
        sites.append({key: summary.get(key) for key in
                      ("site_id", "site_kind", "member_count", "member_state_counts", "active_group_id",
                       "active_job_type", "active_member_ids", "active_hostile_operation")})
        site_members[str(summary["site_id"])] = {str(member.get("npc_id")) for member in raw_site.get("members", [])}

    evidence = _object(statement.get("performance_evidence"))
    _require(set(evidence) == {"behavior", "measurements"}, "native_sources_required")
    behavior_handles, measurement_handles = evidence["behavior"], evidence["measurements"]
    _require(isinstance(behavior_handles, list) and bool(behavior_handles) and
             isinstance(measurement_handles, list) and bool(measurement_handles), "native_sources_required")
    measurements, sources = [], []
    for handle in measurement_handles:
        record, source = _source_record(handle, exit_path.parent / "performance.jsonl")
        metric_owner = _object(record.get("owner"))
        resources = _object(record.get("resources"))
        game_time = _object(record.get("game_time"))
        birth = str(generation["birth_identity"])
        # Windows resource sampling reports .NET UTC ticks; the native process
        # inspector reports FILETIME ticks. Their epochs differ by this constant.
        metric_birth = str(metric_owner.get("process_identity", ""))
        _require(birth.startswith("windows-filetime:") and metric_birth.isdigit() and
                 int(metric_birth) == int(birth.split(":", 1)[1]) + 504911232000000000,
                 "measurement_birth_mismatch")
        _require(record.get("schema") == "caol-process-performance-v1" and
                 metric_owner.get("run_id") == run_id and metric_owner.get("binding_id") == binding_id and
                 metric_owner.get("pid") == process["pid"] and metric_owner.get("platform") == "windows" and
                 _object(metric_owner.get("source_binding")).get("runtime_binding_sha256") ==
                 hashlib.sha256(runtime_path.read_bytes()).hexdigest() and
                 _number(resources.get("interval_wall_seconds"), positive=True) and
                 _number(resources.get("interval_cpu_seconds")) and
                 _object(resources.get("resident_memory")).get("status") == "available" and
                 _number(_object(resources.get("resident_memory")).get("value"), positive=True) and
                 game_time.get("source") == "native_frame" and
                 _number(game_time.get("before_minutes")) and _number(game_time.get("after_minutes")) and
                 game_time["after_minutes"] > game_time["before_minutes"], "measurement_not_native_work")
        measurements.append({"record_id": record.get("record_id"), "game_time": dict(game_time),
                             "resources": dict(resources), "source": source})
        sources.append(source)
    behavior = []
    meaningful = {"active_sortie_local_contact", "local_pair_abstract_resume_progress",
                  "structural_member_physical_return"}
    for handle in behavior_handles:
        record, source = _source_record(handle, transition_path)
        actors = record.get("actor_ids", [])
        _require(record.get("run_id") == run_id and record.get("domain") == "bandit_live_world" and
                 record.get("transition") in meaningful and record.get("outcome") == "committed" and
                 bool(record.get("operation_id")) and isinstance(actors, list) and bool(actors) and
                 set(map(str, actors)) <= site_members.get(str(record.get("site_id")), set()) and
                 _number(record.get("game_minutes")) and any(
                     metric["game_time"]["before_minutes"] <= record["game_minutes"] <=
                     metric["game_time"]["after_minutes"] for metric in measurements),
                 "no_meaningful_native_actor_work_in_interval")
        behavior.append({key: record.get(key) for key in
                         ("sequence", "site_id", "operation_id", "actor_ids", "transition", "game_minutes")})
        sources.append(source)
    _require(len({(item["path"], item["offset"]) for item in sources}) == len(sources), "duplicate_source")
    return {"schema": SCHEMA, "scope": CONTRACT["scope"], "whole_sequence_proven": False,
            "gameplay_credit": False, "population_sequence_credit": False,
            "initial_world": {key: value for key, value in initial.items() if key != "runtime"},
            "continuation": lineage,
            "behavior": behavior, "measurements": measurements, "sources": sources,
            "census": {"dimension_data": {"path": str(dimension_path), "sha256": hashlib.sha256(raw).hexdigest()},
                       "sites": sites, "migo_schema_present": True,
                       "scope": "terminal native census; no interval-wide population inference"},
            "limitations": ["No natural-generation, full mixed-load, budget, overload, inactive-history or persistence-reload proof is inferred.",
                            "Coordinator review must judge the causal claim against the exact native work and interval."]}
