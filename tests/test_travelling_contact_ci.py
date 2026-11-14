#!/usr/bin/env python3
"""Check renderer binding and strict per-case execution in the CI route."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import travelling_contact_ci as route


class TravellingContactCITest(unittest.TestCase):
    def test_catalogue_preserves_existing_controls_and_one_guarded_addition(self):
        self.assertEqual(len(route.SELECTIONS), 40)
        self.assertEqual(len(set(route.SELECTIONS)), 40)
        self.assertNotIn(route.CURSES_ONLY_NOOP, route.SELECTIONS[:27])
        self.assertEqual(route.SELECTIONS[27:].count(route.CURSES_ONLY_NOOP), 1)
        self.assertFalse(any("benchmark" in selection.lower() for selection in route.SELECTIONS))

    def run_with(self, tiles, marker_output, outcomes):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "cata_test"
            binary.touch()
            marker = subprocess.CompletedProcess([], 1, marker_output + "\n")
            results = [marker] + [subprocess.CompletedProcess([], code, text) for code, text in outcomes]
            with patch.object(route.subprocess, "run", side_effect=results) as runner:
                result = route.run_controls(binary, tiles)
            return result, runner.call_args_list

    def test_curses_runs_the_native_owner_case(self):
        selections = ("first", route.CURSES_ONLY_NOOP, "last")
        passed = (0, "All tests passed (1 assertion in 1 test case)\n")
        with patch.object(route, "SELECTIONS", selections):
            result, calls = self.run_with(False, route.CURSES_ONLY_NOOP, [passed] * 3)
        self.assertEqual(result, 0)
        self.assertEqual([call.args[0][1] for call in calls[1:]], list(selections))

    def test_tiles_skips_only_the_guarded_case(self):
        selections = ("first", route.CURSES_ONLY_NOOP, "last")
        passed = (0, "All tests passed (2 assertions in 1 test case)\n")
        with patch.object(route, "SELECTIONS", selections):
            result, calls = self.run_with(True, route.TILES_MARKER, [passed] * 2)
        self.assertEqual(result, 0)
        self.assertEqual([call.args[0][1] for call in calls[1:]], ["first", "last"])

    def test_unexpected_missing_case_fails_and_later_cases_run(self):
        selections = ("missing", "later")
        outcomes = [(0, "No test cases matched\n"),
                    (0, "All tests passed (1 assertion in 1 test case)\n")]
        with patch.object(route, "SELECTIONS", selections):
            result, calls = self.run_with(True, route.TILES_MARKER, outcomes)
        self.assertEqual(result, 1)
        self.assertEqual([call.args[0][1] for call in calls[1:]], list(selections))

    def test_renderer_mismatch_and_missing_markers_fail_closed(self):
        for tiles, marker in ((False, route.TILES_MARKER), (True, route.CURSES_ONLY_NOOP),
                              (False, ""), (True, "")):
            with self.subTest(tiles=tiles, marker=marker):
                result, calls = self.run_with(tiles, marker, [])
                self.assertEqual(result, 1)
                self.assertEqual(len(calls), 1)

    def test_renderer_query_error_and_timeout_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "cata_test"
            binary.touch()
            for response in (subprocess.CompletedProcess([], 2, route.TILES_MARKER),
                             subprocess.TimeoutExpired("cata_test", 30)):
                with self.subTest(response=response):
                    with patch.object(route.subprocess, "run", side_effect=[response]) as runner:
                        self.assertEqual(route.run_controls(binary, True), 1)
                    self.assertEqual(runner.call_count, 1)

    def test_workflows_bind_real_build_configurations(self):
        matrix = Path(".github/workflows/matrix.yml").read_text()
        windows = Path(".github/workflows/msvc-full-features.yml").read_text()
        self.assertIn("tests/travelling_contact_ci.py tests/cata_test --tiles 0", matrix)
        self.assertIn("tests/travelling_contact_ci.py tests/cata_test --tiles ${{ matrix.tiles }}", matrix)
        self.assertIn(r"tests/travelling_contact_ci.py .\Cataclysm-test-vcpkg-static-Release-x64.exe --tiles 1",
                      windows)
        self.assertEqual(matrix.count("python3 tests/test_travelling_contact_ci.py"), 2)
        self.assertEqual(windows.count("py -3 tests/test_travelling_contact_ci.py"), 1)


if __name__ == "__main__":
    unittest.main()
