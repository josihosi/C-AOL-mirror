"""Positive and fail-closed controls for the R-008 channel observation surface."""

import json
import tempfile
import unittest
from pathlib import Path

from r008_indoor_channel_observation import (
    R008_CHANNELS,
    R008_CHANNEL_RECORD_FILENAME,
    R008_CHANNEL_SCHEMA,
    read_r008_indoor_channel_observation,
)


def _stream(run_id="run-a", source="source-a", executable="exe-a", scenario="scenario-a", binding="binding-a"):
    rows = []
    for sequence, channel in enumerate(R008_CHANNELS, 1):
        rows.append({
            "schema": R008_CHANNEL_SCHEMA,
            "sequence": sequence,
            "run_id": run_id,
            "scan_id": run_id + ":1",
            "binding": {
                "runtime_source_sha256": source,
                "executable_sha256": executable,
                "scenario_id": scenario,
                "binding_id": binding,
            },
            "scan": {"game_minutes": 100, "fresh": True, "isolated": True},
            "channel": channel,
            "signal_origin": "local_field" if channel in {"light", "smoke"} else "none",
            "consumer": "bandit_live_world.signal_scan",
            "observed": channel in {"light", "smoke"},
            "isolated": True,
        })
    return rows


class R008IndoorChannelObservationTest(unittest.TestCase):
    def write(self, rows, contract=True):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / R008_CHANNEL_RECORD_FILENAME).write_text(
            "".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8"
        )
        if contract:
            (root / "contract.preflight.json").write_text(
                json.dumps({"scenario": "scenario-a"}), encoding="utf-8"
            )
        return root

    def test_live_summary_validates_all_rows_and_detail_remains_exact(self):
        from startup_harness import compact_cockpit_live_evidence, RUNTIME_BINDING_FILENAME
        rows = _stream()
        for row in rows:
            row["diagnostic_context"] = "retained native context " * 1000
        root = self.write(rows)
        (root / RUNTIME_BINDING_FILENAME).write_text(json.dumps({
            "runtime_source_sha256": "source-a", "executable_sha256": "exe-a"}))
        summary = compact_cockpit_live_evidence(root, "run-a", binding_id="binding-a")
        channel = summary["production_channel_observation"]
        self.assertTrue(channel["eligible"], channel)
        self.assertEqual(channel["record_count"], len(rows))
        self.assertFalse(channel["records_included"])
        self.assertEqual(channel["records"], [])
        self.assertEqual(channel["latest_game_minutes"], 100)
        self.assertEqual(Path(channel["path"]), root / R008_CHANNEL_RECORD_FILENAME)
        detail = read_r008_indoor_channel_observation(root, run_id="run-a",
            source_sha256="source-a", executable_sha256="exe-a", binding_id="binding-a")
        self.assertEqual(detail["records"], rows)
        self.assertTrue(detail["records_included"])
        self.assertLess(len(json.dumps(summary)), len(json.dumps(detail)) // 10)
        # A malformed/foreign row is still a refusal when its detail is not
        # copied into the ordinary decision. Omission cannot conceal it.
        rows[2]["run_id"] = "foreign"
        (root / R008_CHANNEL_RECORD_FILENAME).write_text(
            "".join(json.dumps(row) + "\n" for row in rows) + "partial row")
        rejected = compact_cockpit_live_evidence(root, "run-a", binding_id="binding-a")
        channel = rejected["production_channel_observation"]
        self.assertFalse(channel["eligible"])
        self.assertIn("wrong_run", channel["issues"])
        self.assertIn("malformed_record", channel["issues"])

    def test_accepts_complete_bound_isolated_scan(self):
        result = read_r008_indoor_channel_observation(
            self.write(_stream()), run_id="run-a", source_sha256="source-a",
            executable_sha256="exe-a", scenario_id="scenario-a", observed_game_minutes=100,
            binding_id="binding-a",
        )
        self.assertTrue(result["eligible"])
        self.assertEqual(result["channels"], sorted(R008_CHANNELS))

    def test_sampling_state_is_preserved_and_contradictions_are_rejected(self):
        rows = _stream()
        rows[0].update(sampled=False, observed=False, detail="not_sampled")
        result = read_r008_indoor_channel_observation(
            self.write(rows), run_id="run-a", source_sha256="source-a",
            executable_sha256="exe-a", scenario_id="scenario-a", binding_id="binding-a")
        self.assertTrue(result["eligible"])
        self.assertFalse(result["records"][0]["sampled"])
        self.assertNotIn("sampled", result["records"][1])  # legacy is unknown
        rows[0]["observed"] = True
        result = read_r008_indoor_channel_observation(
            self.write(rows), run_id="run-a", source_sha256="source-a",
            executable_sha256="exe-a", scenario_id="scenario-a", binding_id="binding-a")
        self.assertIn("observed_without_sampling", result["issues"])

    def test_rejects_missing_stale_wrong_run_and_unisolated_rows(self):
        rows = _stream()
        rows.pop()
        rows[0]["run_id"] = "foreign"
        rows[1]["scan"]["fresh"] = False
        rows[2]["isolated"] = False
        result = read_r008_indoor_channel_observation(
            self.write(rows), run_id="run-a", source_sha256="source-a",
            executable_sha256="exe-a", scenario_id="scenario-a",
            binding_id="binding-a",
        )
        self.assertFalse(result["eligible"])
        self.assertIn("wrong_run", result["issues"])
        self.assertIn("stale_scan", result["issues"])
        self.assertIn("unisolated_record", result["issues"])
        self.assertTrue(any(issue.startswith("missing_channels:") for issue in result["issues"]))

    def test_rejects_wrong_source_executable_and_scenario(self):
        result = read_r008_indoor_channel_observation(
            self.write(_stream()), run_id="run-a", source_sha256="other-source",
            executable_sha256="other-exe", scenario_id="other-scenario",
            binding_id="binding-a",
        )
        self.assertFalse(result["eligible"])
        self.assertIn("wrong_runtime_source_sha256", result["issues"])
        self.assertIn("wrong_executable_sha256", result["issues"])
        self.assertIn("wrong_scenario_id", result["issues"])


if __name__ == "__main__":
    unittest.main()
