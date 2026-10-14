"""Focused integration checks for tools/check_changed_source.py."""

import contextlib
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import tools.check_changed_source as lint

ROOT = Path(__file__).resolve().parent.parent
ENTRY = ROOT / "tools/check_changed_source.py"
SCENARIOS = ROOT / "tools/openclaw_harness/scenarios"
REGISTRY_SOURCE = ROOT / "tools/openclaw_harness/scenario_registry.py"


class ChangedSourceEntryPointTest(unittest.TestCase):
    def test_git_changed_path_traversal_uses_nul_delimiters(self):
        results = [
            subprocess.CompletedProcess([], 0, b"tools/changed.py\0tools/line\nbreak.py\0", b""),
            subprocess.CompletedProcess([], 0, b"tools/new_module.py\0.build/generated.py\0", b""),
        ]
        with mock.patch.object(lint, "run", side_effect=results) as mocked:
            paths = lint.changed_from_git("HEAD")
        self.assertEqual(paths, [".build/generated.py", "tools/changed.py", "tools/line\nbreak.py", "tools/new_module.py"])
        self.assertEqual(mocked.call_count, 2)
        self.assertIn("-z", mocked.call_args_list[0].args[0])
        self.assertIn("-z", mocked.call_args_list[1].args[0])
        self.assertFalse(mocked.call_args_list[0].kwargs["text"])

    def test_real_git_changed_python_traversal_and_generated_exclusion(self):
        with tempfile.TemporaryDirectory(prefix="changed-source-git-") as temp:
            root = Path(temp).resolve()
            (root / "tools").mkdir()
            tracked = root / "tools/tracked.py"
            unusual = root / "tools/line\nbreak.py"
            tracked.write_text("value = 1\n", encoding="utf-8")
            unusual.write_text("value = 1\n", encoding="utf-8")

            def git(*args):
                result = subprocess.run(
                    ["git"] + list(args), cwd=str(root), stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout)

            git("init", "-q")
            git("config", "user.name", "Lint Test")
            git("config", "user.email", "lint-test@example.invalid")
            git("add", "--", "tools")
            git("commit", "-qm", "baseline")
            tracked.write_text("value = 2\n", encoding="utf-8")
            unusual.write_text("value = 2\n", encoding="utf-8")
            (root / "tools/new.py").write_text("value = 3\n", encoding="utf-8")
            (root / ".build").mkdir()
            (root / ".build/generated.py").write_text("value = 4\n", encoding="utf-8")

            def local_git(command, cwd=lint.ROOT, env=None, text=True):
                return subprocess.run(
                    command, cwd=str(root), env=env, text=text,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )

            with mock.patch.object(lint, "ROOT", root), mock.patch.object(lint, "run", side_effect=local_git):
                selected_raw = lint.changed_from_git("HEAD")
                selected, incomplete = lint.normalize_paths(selected_raw, root=root)

            self.assertFalse(incomplete)
            self.assertEqual(selected, ["tools/line\nbreak.py", "tools/new.py", "tools/tracked.py"])

    def test_python_classification_and_runtime_exclusions(self):
        selected, incomplete = lint.normalize_paths([
            "tools/openclaw_harness/scenario_registry_cli.py",
            "build/generated.py",
            ".userdata/private.py",
            ".build/generated.py",
            "tools/openclaw_harness/build_logs/run.py",
            ".env/private.py",
            "src/generated/cache.py",
        ])
        self.assertFalse(incomplete)
        self.assertEqual(selected, ["tools/openclaw_harness/scenario_registry_cli.py"])
        self.assertEqual(lint.source_classifier(selected[0]), "python")

    def test_affected_header_selects_source_and_test_tus_and_flags_stale_scan(self):
        with tempfile.TemporaryDirectory(prefix="changed-source-deps-") as temp:
            root = Path(temp)
            (root / "src").mkdir()
            (root / "tests").mkdir()
            (root / "obj").mkdir()
            (root / "tests/obj").mkdir()
            header = root / "src/alpha.h"
            new_header = root / "src/new_header.h"
            header.write_text("// header fixture\n", encoding="utf-8")
            new_header.write_text("// new header fixture\n", encoding="utf-8")
            src_scan = root / "obj/alpha.inc"
            test_scan = root / "tests/obj/alpha_test.inc"
            src_scan.write_text(". ./src/alpha.h\n", encoding="utf-8")
            test_scan.write_text(".. ../src/alpha.h\n", encoding="utf-8")

            tus, reason = lint.select_cpp_tus(["src/alpha.h", "src/new_header.h"], root=root)
            self.assertEqual(tus, ["src/alpha.cpp", "tests/alpha_test.cpp"])
            self.assertIn("src/new_header.h", reason)
            self.assertIn("absent from existing include scan", reason)

            output = io.StringIO()
            with mock.patch.object(lint, "ROOT", root), mock.patch.object(lint, "run_astyle", return_value=0), \
                    contextlib.redirect_stdout(output):
                status = lint.main(["--files", "src/alpha.h", "src/new_header.h"])
            self.assertEqual(status, 2)
            self.assertIn("src/new_header.h", output.getvalue())
            self.assertIn("RESULT: incomplete coverage", output.getvalue())

            src_scan.write_text(". ./src/alpha.h\n. ./src/new_header.h\n", encoding="utf-8")
            fixed_tus, fixed_reason = lint.select_cpp_tus(
                ["src/alpha.h", "src/new_header.h"], root=root
            )
            self.assertIsNone(fixed_reason)
            self.assertEqual(fixed_tus, tus)
            fixed_output = io.StringIO()
            with mock.patch.object(lint, "ROOT", root), mock.patch.object(lint, "run_astyle", return_value=0), \
                    contextlib.redirect_stdout(fixed_output):
                fixed_status = lint.main(["--files", "src/alpha.h", "src/new_header.h"])
            self.assertEqual(fixed_status, 2)
            self.assertNotIn("absent from existing include scan", fixed_output.getvalue())
            self.assertIn("src/alpha.cpp, tests/alpha_test.cpp", fixed_output.getvalue())

            tus, reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertIsNone(reason)
            self.assertEqual(tus, fixed_tus)

            future = max(src_scan.stat().st_mtime, test_scan.stat().st_mtime) + 10
            os.utime(header, (future, future))
            stale_tus, stale_reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertEqual(stale_tus, tus)
            self.assertIn("scan predates src/alpha.h", stale_reason)
            newline_tus, newline_reason = lint.select_cpp_tus(["src/line\nbreak.h"], root=root)
            self.assertEqual(newline_tus, [])
            self.assertIn("cannot represent newline paths", newline_reason)

    def test_tiles_only_scan_freshness_and_mixed_layout_ambiguity(self):
        with tempfile.TemporaryDirectory(prefix="changed-source-tiles-") as temp:
            root = Path(temp).resolve()
            (root / "src").mkdir()
            (root / "tests").mkdir()
            (root / "obj/tiles").mkdir(parents=True)
            (root / "tests/obj").mkdir(parents=True)
            header = root / "src/alpha.h"
            header.write_text("// header fixture\n", encoding="utf-8")
            tiles_scan = root / "obj/tiles/alpha.inc"
            test_scan = root / "tests/obj/alpha_test.inc"
            tiles_scan.write_text(". ./src/alpha.h\n", encoding="utf-8")
            test_scan.write_text(".. ../src/alpha.h\n", encoding="utf-8")

            tus, reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertIsNone(reason)
            self.assertEqual(tus, ["src/alpha.cpp", "tests/alpha_test.cpp"])

            future = max(tiles_scan.stat().st_mtime, test_scan.stat().st_mtime) + 10
            os.utime(header, (future, future))
            os.utime(test_scan, (future + 10, future + 10))
            _, stale_reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertIn("obj/tiles/alpha.inc", stale_reason)
            self.assertIn("scan predates src/alpha.h", stale_reason)

            tiles_scan.write_text(". ./src/other.h\n", encoding="utf-8")
            normal_scan = root / "obj/alpha.inc"
            normal_scan.write_text(". ./src/alpha.h\n", encoding="utf-8")
            os.utime(normal_scan, (future + 10, future + 10))
            _, mixed_reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertIn("obj/tiles/alpha.inc", mixed_reason)
            self.assertIn("scan predates src/alpha.h", mixed_reason)

            tiles_scan.write_text(". ./src/alpha.h\n", encoding="utf-8")
            os.utime(tiles_scan, (future + 10, future + 10))
            mixed_tus, mixed_fixed_reason = lint.select_cpp_tus(["src/alpha.h"], root=root)
            self.assertIsNone(mixed_fixed_reason)
            self.assertEqual(mixed_tus, tus)

    def test_existing_clang_tidy_wrapper_forwards_build_dir_and_translation_unit(self):
        with tempfile.TemporaryDirectory(prefix="changed-source-wrapper-") as temp:
            root = Path(temp)
            stub = root / "clang-tidy-22-stub"
            stub.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
            stub.chmod(0o755)
            build_dir = root / "analysis-build"
            env = os.environ.copy()
            env["CATA_BUILD_DIR"] = str(build_dir)
            env["CATA_CLANG_TIDY"] = str(stub)
            result = subprocess.run(
                ["bash", str(ROOT / "build-scripts/clang-tidy-wrapper.sh"),
                 "-p", str(build_dir), str(root / "src/alpha.cpp")],
                cwd=str(root), env=env, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(str(build_dir), result.stdout)
            self.assertIn(str(root / "src/alpha.cpp"), result.stdout)

    def test_cached_tidy_uses_configured_build_dir_and_wrapper_arguments(self):
        with tempfile.TemporaryDirectory(prefix="changed-source-tidy-") as temp:
            root = Path(temp)
            build_dir = root / "analysis-build"
            plugin = build_dir / "tools/clang-tidy-plugin/libCataAnalyzerPlugin.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            (build_dir / "compile_commands.json").write_text("[]\n", encoding="utf-8")
            (root / ".clang-tidy").touch()
            observed = {}
            fake_run = subprocess.CompletedProcess([], 0, "", "")

            def capture(command, cwd=None, env=None, **kwargs):
                observed["command"] = command
                observed["cwd"] = cwd
                observed["env"] = env
                trace = Path(cwd) / "clang-tidy-trace"
                trace.mkdir()
                (trace / "analysis.json").write_text("{}\n", encoding="utf-8")
                return fake_run

            output = io.StringIO()
            with mock.patch.object(lint, "ROOT", root), \
                    mock.patch.object(lint, "select_cpp_tus", return_value=(["src/alpha.cpp"], None)), \
                    mock.patch.object(lint.shutil, "which", return_value="/isolated/clang-tidy-22"), \
                    mock.patch.object(lint, "version_text", return_value="Debian LLVM version 22.1.0"), \
                    mock.patch.object(lint, "run", side_effect=capture), \
                    mock.patch.dict(os.environ, {"CATA_BUILD_DIR": "analysis-build"}), \
                    contextlib.redirect_stdout(output):
                status = lint.run_cpp_analysis(["src/alpha.cpp"])

            self.assertEqual(status, 0)
            self.assertEqual(observed["command"][0:2], ["bash", str(root / "build-scripts/clang-tidy-wrapper.sh")])
            self.assertEqual(observed["command"][2:4], ["-p", str(build_dir)])
            self.assertEqual(observed["command"][4], str((root / "src/alpha.cpp").resolve()))
            self.assertNotEqual(Path(observed["cwd"]), root)
            self.assertFalse(Path(observed["cwd"]).exists())
            self.assertEqual(observed["env"]["CATA_BUILD_DIR"], str(build_dir))
            self.assertEqual(observed["env"]["CATA_CLANG_TIDY"], "/isolated/clang-tidy-22")

    def test_unclassified_selected_source_is_incomplete(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = lint.main(["--files", "README.md"])
        self.assertEqual(status, 2)
        self.assertIn("no lint route selected", output.getvalue())
        self.assertIn("RESULT: incomplete coverage", output.getvalue())

    def test_missing_astyle_configuration_is_incomplete(self):
        with tempfile.TemporaryDirectory(prefix="check-changed-source-config-") as temp:
            output = io.StringIO()
            with mock.patch.object(lint, "ROOT", Path(temp)):
                with contextlib.redirect_stdout(output):
                    status = lint.run_astyle(["src/example.cpp"])
        self.assertEqual(status, 2)
        self.assertIn(".astylerc is missing", output.getvalue())

    def test_missing_flake8_configuration_is_incomplete(self):
        with tempfile.TemporaryDirectory(prefix="check-changed-source-config-") as temp:
            output = io.StringIO()
            with mock.patch.object(lint, "ROOT", Path(temp)):
                with contextlib.redirect_stdout(output):
                    status = lint.run_flake8(["tools/example.py"])
        self.assertEqual(status, 2)
        self.assertIn(".flake8 is missing", output.getvalue())

    def test_missing_flake8_is_incomplete(self):
        with mock.patch.object(lint.shutil, "which", return_value=None):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = lint.run_flake8(["tools/example.py"])
        self.assertEqual(status, 2)
        self.assertIn("flake8 is missing", output.getvalue())

    def invoke(self, relative):
        return subprocess.run(
            [sys.executable, str(ENTRY), "--files", relative], cwd=str(ROOT),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )

    def test_declaration_invalid_and_corrected_through_entry_point(self):
        with tempfile.TemporaryDirectory(prefix="check-changed-source-", dir=str(SCENARIOS)) as temp:
            directory = Path(temp)
            invalid = directory / "bad.json"
            valid = directory / "fixed.json"
            registry_before = hashlib.sha256(REGISTRY_SOURCE.read_bytes()).hexdigest()
            invalid.write_text("[]\n", encoding="utf-8")
            valid.write_text((SCENARIOS / "ambient.weird_item_reaction.json").read_text(encoding="utf-8"), encoding="utf-8")
            invalid_before = hashlib.sha256(invalid.read_bytes()).hexdigest()
            bad_result = self.invoke(invalid.relative_to(ROOT).as_posix())
            self.assertEqual(bad_result.returncode, 1, bad_result.stdout)
            self.assertIn("[top_level_object]", bad_result.stdout)
            self.assertEqual(hashlib.sha256(invalid.read_bytes()).hexdigest(), invalid_before)
            valid_before = hashlib.sha256(valid.read_bytes()).hexdigest()
            fixed_result = self.invoke(valid.relative_to(ROOT).as_posix())
            self.assertEqual(fixed_result.returncode, 0, fixed_result.stdout)
            self.assertIn("RESULT: full selected coverage passed", fixed_result.stdout)
            self.assertEqual(hashlib.sha256(valid.read_bytes()).hexdigest(), valid_before)
            self.assertEqual(hashlib.sha256(REGISTRY_SOURCE.read_bytes()).hexdigest(), registry_before)

    @unittest.skipUnless(shutil.which("astyle"), "AStyle is not installed")
    def test_astyle_mismatch_skips_analysis_and_preserves_source(self):
        with tempfile.TemporaryDirectory(prefix="check-changed-source-", dir=str(ROOT / "tools")) as temp:
            fixture = Path(temp) / "format_fixture.cpp"
            fixture.write_text("int main(){return 0;}\n", encoding="utf-8")
            before = hashlib.sha256(fixture.read_bytes()).hexdigest()
            result = self.invoke_external(fixture)
            version = lint.version_text("astyle", ["--version"])
            if version != "Artistic Style Version 3.1":
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("formatting analysis skipped", result.stdout)
                self.assertNotIn("formatting changes are required", result.stdout)
            else:
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("formatting changes are required", result.stdout)
            self.assertEqual(hashlib.sha256(fixture.read_bytes()).hexdigest(), before)

    def invoke_external(self, path):
        return subprocess.run(
            [sys.executable, str(ENTRY), "--files", path.relative_to(ROOT).as_posix()], cwd=str(ROOT),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )


if __name__ == "__main__":
    unittest.main()
