#!/usr/bin/env python3
"""Run existing local checks on changed source and report uncovered analysis."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import types

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_MAP_HELPER = ROOT / "build-scripts/get_affected_files.py"
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
PY_SUFFIXES = {".py"}
JSON_SUFFIXES = {".json"}
EXCLUDED_PARTS = {
    ".git", ".de67", ".venv", "venv", "__pycache__", ".build", "build", "build_w",
    "obj", "obj_w", "dist", "distvenv", "userdata", "harness_runs", "sessions",
    "task-logs", "build_logs", "logs", "proof", "proofs", "captures", "cache",
    ".cache", ".pytest_cache", ".mypy_cache", ".ruff_cache", "artifacts", "generated",
    "third-party", "node_modules", "secrets", ".secrets", ".env", ".envs", "credentials",
}


def excluded(path):
    parts = Path(path).parts
    return any(part in EXCLUDED_PARTS or part.startswith(".userdata") for part in parts)


def run(command, cwd=ROOT, env=None, text=True):
    try:
        return subprocess.run(command, cwd=str(cwd), env=env, text=text,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError:
        return None


def changed_from_git(base):
    diff = run(["git", "diff", "--name-only", "-z", "--diff-filter=ACMRTUXB", base, "--"], text=False)
    if diff is None or diff.returncode:
        detail = "command unavailable" if diff is None else os.fsdecode(diff.stdout).strip()
        raise RuntimeError("cannot determine changed files from git: " + detail)
    untracked = run(["git", "ls-files", "-z", "--others", "--exclude-standard"], text=False)
    if untracked is None or untracked.returncode:
        raise RuntimeError("cannot list untracked files from git")
    paths = set(decode_git_paths(diff.stdout)) | set(decode_git_paths(untracked.stdout))
    return sorted(paths)


def decode_git_paths(output):
    """Decode NUL-delimited Git paths without treating newlines as separators."""
    return [os.fsdecode(path) for path in output.split(b"\0") if path]


def normalize_paths(paths, root=None):
    root = Path(root or ROOT).resolve()
    normalized = []
    incomplete = False
    for raw in paths:
        raw = os.fsdecode(raw)
        candidate = Path(raw)
        resolved = candidate.resolve() if candidate.is_absolute() else (root / candidate).resolve()
        try:
            relative = resolved.relative_to(root)
        except ValueError:
            print("INCOMPLETE: outside repository, skipped: {}".format(raw), file=sys.stderr)
            incomplete = True
            continue
        candidate = Path(os.path.normpath(str(relative)))
        if excluded(candidate):
            print("SKIPPED generated/user/runtime path: {}".format(candidate))
            continue
        normalized.append(candidate.as_posix())
    return sorted(set(normalized)), incomplete


def version_text(binary, args):
    result = run([binary] + args)
    return None if result is None or result.returncode else result.stdout.strip()


def source_classifier(path):
    suffix = Path(path).suffix.lower()
    if suffix in CPP_SUFFIXES:
        return "cpp"
    if suffix in PY_SUFFIXES:
        return "python"
    if suffix in JSON_SUFFIXES and path.startswith("tools/openclaw_harness/scenarios/"):
        return "declaration"
    return None


def run_astyle(paths):
    if not (ROOT / ".astylerc").is_file():
        print("INCOMPLETE: .astylerc is missing; formatting check skipped")
        return 2
    version = version_text("astyle", ["--version"])
    if version is None:
        print("INCOMPLETE: astyle is missing; formatting check skipped")
        return 2
    print("AStyle: {} (CI expects Artistic Style Version 3.1)".format(version))
    if version != "Artistic Style Version 3.1":
        print("INCOMPLETE: AStyle version differs from CI; formatting analysis skipped")
        return 2
    result = run(["astyle", "--options=.astylerc", "--dry-run", "-X", "-Q", "--ascii"] + paths)
    if result is None:
        print("INCOMPLETE: astyle could not be executed")
        return 2
    output = result.stdout.strip()
    if output:
        print(output)
    if result.returncode:
        print("FAIL: AStyle returned {}".format(result.returncode))
        return 1
    if re.search(r"^Formatted\s", result.stdout, re.MULTILINE):
        print("FAIL: formatting changes are required (dry-run; source left unchanged)")
        return 1
    print("PASS: AStyle 3.1 formatting check (dry-run)")
    return 0


def run_flake8(paths):
    if not (ROOT / ".flake8").is_file():
        print("INCOMPLETE: .flake8 is missing; changed Python check skipped")
        return 2
    binary = shutil.which("flake8")
    if not binary:
        print("INCOMPLETE: flake8 is missing; changed Python check skipped")
        return 2
    version = version_text(binary, ["--version"])
    print("Flake8: {}".format(version or "version unavailable"))
    result = run([binary] + paths)
    if result is None:
        print("INCOMPLETE: flake8 could not be executed")
        return 2
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.returncode:
        print("FAIL: flake8 returned {}".format(result.returncode))
        return 1
    print("PASS: flake8 on changed Python files")
    return 0


def run_declaration_lint(paths):
    cli = ROOT / "tools/openclaw_harness/scenario_registry_cli.py"
    if not cli.is_file():
        print("INCOMPLETE: scenario declaration linter is missing")
        return 2
    result = run([sys.executable, str(cli), "--json", "lint-declarations"] + paths)
    if result is None:
        print("INCOMPLETE: could not invoke scenario declaration linter")
        return 2
    if result.stdout.strip():
        # The existing CLI returns structured diagnostics with file/line/rule.
        try:
            import json
            payload = json.loads(result.stdout)
            for item in payload.get("results", []):
                print("{}: {}".format(item.get("path", "<scenario>"), item.get("status", "unknown")))
                for diagnostic in item.get("diagnostics", []):
                    location = diagnostic.get("file", item.get("path", "<scenario>"))
                    line = diagnostic.get("line")
                    if line is not None:
                        location += ":{}".format(line)
                    print("{}: {} [{}] {}: {}".format(
                        location, diagnostic.get("severity", "error"),
                        diagnostic.get("rule", "unknown"), diagnostic.get("field", ""),
                        diagnostic.get("message", "")))
            counts = payload.get("counts", {})
            print("declarations: valid={} review_required={} invalid={}".format(
                counts.get("valid", 0), counts.get("review_required", 0), counts.get("invalid", 0)))
        except (ValueError, AttributeError):
            print(result.stdout.rstrip())
    if result.returncode:
        print("FAIL: declaration lint returned {}".format(result.returncode))
        return 1
    print("PASS: scenario declaration lint")
    return 0


def select_cpp_tus(changed_cpp, root=None):
    """Reuse the build's existing include scan; never generate it here."""
    root = Path(root or ROOT).resolve()
    if any("\n" in path or "\r" in path for path in changed_cpp):
        return [], "the existing line-based include selector cannot represent newline paths"
    if not (root / "obj").is_dir() or not (root / "tests/obj").is_dir():
        return [], "existing obj and tests/obj include scans are required; this command does not build them"
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as changed:
        changed.write("\n".join(changed_cpp) + "\n")
        changed_path = changed.name
    try:
        result = run([sys.executable, str(INCLUDE_MAP_HELPER), "--changed-files-list", changed_path], cwd=root)
    finally:
        os.unlink(changed_path)
    if result is None or result.returncode:
        reason = "get_affected_files.py failed" if result is None else result.stdout.strip()
        return [], reason or "include dependency selection failed"
    tus = sorted(line.strip() for line in result.stdout.splitlines() if line.strip())
    parser_module = types.ModuleType("_caol_get_affected_files")
    parser_module.__file__ = str(INCLUDE_MAP_HELPER)
    try:
        parser_source = INCLUDE_MAP_HELPER.read_text(encoding="utf-8")
        exec(compile(parser_source, str(INCLUDE_MAP_HELPER), "exec"), parser_module.__dict__)
    except (OSError, SyntaxError) as exc:
        return tus, "existing include-map parser could not be loaded: {}".format(exc)

    class ScanTrackingMap(dict):
        def __init__(self):
            super().__init__()
            self.current_scan = None
            self.current_source = None
            self.scan_paths_by_input = {}
            self.scan_paths_by_source = {}

        def setdefault(self, key, default=None):
            self.scan_paths_by_input.setdefault(key, set()).add(self.current_scan)
            self.scan_paths_by_source.setdefault(self.current_source, set()).add(self.current_scan)
            return super().setdefault(key, default)

    class IncludesParserWithScanProvenance(parser_module.IncludesParser):
        def __init__(self):
            super().__init__()
            self.includes_to_sources = ScanTrackingMap()

        def parse_includes_file(self, includes_file, source_file_path, relative_root):
            self.includes_to_sources.current_scan = Path(includes_file.name).resolve()
            self.includes_to_sources.current_source = source_file_path
            return super().parse_includes_file(includes_file, source_file_path, relative_root)

    include_map = IncludesParserWithScanProvenance()
    source_scans_ok = include_map.parse_includes_files_from(str(root / "obj"), "src", ".")
    test_scans_ok = include_map.parse_includes_files_from(str(root / "tests/obj"), "tests", "tests")
    if not source_scans_ok or not test_scans_ok:
        return tus, "existing source and test include maps could not both be read"
    unmapped = [path for path in changed_cpp if not include_map.get_files_affected_by(path)]
    if unmapped:
        return tus, "changed C++ input(s) absent from existing include scan: " + ", ".join(unmapped)
    stale = []
    for changed in changed_cpp:
        changed_path = root / changed
        affected_sources = include_map.get_files_affected_by(changed)
        if not include_map.includes_to_sources.scan_paths_by_input.get(changed):
            stale.append("{} (include scan provenance unavailable)".format(changed))
            continue
        for source in affected_sources:
            scan_paths = include_map.includes_to_sources.scan_paths_by_source.get(source, set())
            if not scan_paths:
                stale.append("{} (include scan provenance unavailable)".format(source))
                continue
            for scan_path in sorted(scan_paths):
                if not scan_path.is_file():
                    stale.append("{} (include scan missing)".format(scan_path))
                elif changed_path.is_file() and changed_path.stat().st_mtime > scan_path.stat().st_mtime:
                    try:
                        display_path = scan_path.relative_to(root)
                    except ValueError:
                        display_path = scan_path
                    stale.append("{} (scan predates {})".format(display_path, changed))
    if stale:
        return tus, "existing include scan may be stale for: " + ", ".join(stale)
    return tus, None


def run_cpp_analysis(changed_cpp):
    tus, reason = select_cpp_tus(changed_cpp)
    print("C++ changed files: {}".format(", ".join(changed_cpp)))
    if tus:
        print("Affected translation units from existing include scan: {}".format(", ".join(tus)))
    else:
        print("Affected translation units: none selected")
    tidy = shutil.which("clang-tidy-22")
    tidy_version = version_text(tidy, ["--version"]) if tidy else None
    if tidy:
        print("clang-tidy-22: {}".format(tidy_version or "version unavailable"))
    build_dir = Path(os.environ.get("CATA_BUILD_DIR", "build"))
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    database = build_dir / "compile_commands.json"
    plugin = build_dir / "tools/clang-tidy-plugin/libCataAnalyzerPlugin.so"
    prerequisites = []
    if reason:
        prerequisites.append(reason)
    if not (ROOT / ".clang-tidy").is_file():
        prerequisites.append(".clang-tidy is missing")
    if not database.is_file():
        prerequisites.append("{} is missing".format(database.relative_to(ROOT) if database.is_relative_to(ROOT) else database))
    if not plugin.is_file():
        prerequisites.append("{} is missing".format(plugin.relative_to(ROOT) if plugin.is_relative_to(ROOT) else plugin))
    if not tidy:
        prerequisites.append("clang-tidy-22 is missing (CI uses LLVM 22)")
    elif not tidy_version or not re.search(r"LLVM version 22(?:\.|\b)", tidy_version):
        prerequisites.append("clang-tidy tool version does not match CI LLVM 22")
    if not tus:
        prerequisites.append("no affected translation units were established")
    if prerequisites:
        print("INCOMPLETE: C++ correctness analysis skipped: " + "; ".join(prerequisites))
        return 2
    # Use the same local wrapper and plugin route as CI. This is read-only and
    # consumes only pre-existing compile metadata and plugin artifacts.
    env = os.environ.copy()
    env["CATA_CLANG_TIDY"] = tidy
    env["CATA_BUILD_DIR"] = str(build_dir)
    wrapper = ROOT / "build-scripts/clang-tidy-wrapper.sh"
    status = 0
    for tu in tus:
        # The existing wrapper writes clang-tidy-trace in its working
        # directory. Keep those generated profiles out of the workspace.
        try:
            with tempfile.TemporaryDirectory(prefix="check-changed-source-tidy-") as trace_dir:
                result = run(
                    ["bash", str(wrapper), "-p", str(build_dir), str((ROOT / tu).resolve())],
                    cwd=Path(trace_dir), env=env,
                )
        except OSError as exc:
            print("INCOMPLETE: could not create temporary clang-tidy output directory: {}".format(exc))
            status = 2
            continue
        if result is None:
            print("INCOMPLETE: could not execute clang-tidy wrapper for {}".format(tu))
            status = 2
        elif result.stdout.strip():
            print(result.stdout.rstrip())
        if result is not None and result.returncode:
            print("FAIL: clang-tidy failed for {} (exit {})".format(tu, result.returncode))
            status = 1
    if status == 0:
        print("PASS: clang-tidy-22 on selected affected translation units")
    return status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD", help="git diff base (default: HEAD)")
    parser.add_argument("--files", nargs="+", help="explicit changed paths, for review or focused checks")
    args = parser.parse_args(argv)
    started = time.monotonic()
    try:
        raw_paths = args.files if args.files else changed_from_git(args.base)
    except RuntimeError as exc:
        print("INCOMPLETE: {}".format(exc), file=sys.stderr)
        return 2
    paths, incomplete_input = normalize_paths(raw_paths)
    existing = []
    for path in paths:
        if (ROOT / path).is_file():
            existing.append(path)
        else:
            print("INCOMPLETE: changed path is missing or deleted: {}".format(path))
            incomplete_input = True
    paths = existing
    print("Changed source check: {} path(s)".format(len(paths)))
    if not paths:
        if incomplete_input:
            print("RESULT: incomplete coverage")
            return 2
        print("PASS: no changed source files selected")
        return 0
    groups = {name: [p for p in paths if source_classifier(p) == name]
              for name in ("cpp", "python", "declaration")}
    statuses = [2] if incomplete_input else []
    if groups["cpp"]:
        statuses.append(run_astyle(groups["cpp"]))
        statuses.append(run_cpp_analysis(groups["cpp"]))
    if groups["python"]:
        statuses.append(run_flake8(groups["python"]))
    if groups["declaration"]:
        statuses.append(run_declaration_lint(groups["declaration"]))
    unclassified = [p for p in paths if source_classifier(p) is None]
    if unclassified:
        print("INCOMPLETE: no lint route selected for: {}".format(", ".join(unclassified)))
        statuses.append(2)
    elapsed = time.monotonic() - started
    print("Elapsed in checks: {:.6f}s".format(elapsed))
    if 1 in statuses:
        print("RESULT: failed")
        return 1
    if 2 in statuses:
        print("RESULT: incomplete coverage")
        return 2
    print("RESULT: full selected coverage passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
