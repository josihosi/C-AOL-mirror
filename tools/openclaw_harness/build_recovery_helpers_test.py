from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import build_recovery_helpers as recovery
import build_source_bound_macos as macos
import build_source_bound_windows as windows


class BuildRecoveryHelpersTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_explicit_paths_preserve_order_receipts_and_unselected_files(self):
        first, second, untouched = (self.root / name for name in ("first.pch", "second.d", "keep.o"))
        for path in (first, second, untouched):
            path.write_bytes(path.name.encode())
        directory = self.root / "directory.pch"
        directory.mkdir()
        paths = (first, self.root / "absent", directory, second, first)
        expected = [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in (first, second)]
        self.assertEqual(recovery.remove_owned_pch_files(iter(paths)), expected)
        self.assertEqual(recovery.remove_owned_pch_files(paths), [])
        self.assertTrue(directory.is_dir())
        self.assertEqual(untouched.read_bytes(), b"keep.o")

    def test_actual_wrappers_keep_their_path_sets_and_configuration_boundaries(self):
        for module in (macos, windows):
            for tiles in (False, True):
                prefix = f"{module.__name__}-{int(tiles)}-"
                paths = module.pch_recovery_paths(self.root, prefix, tiles=tiles)
                for i, path in enumerate(paths):
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(f"header-{i}".encode())
                untouched = paths[0].parent / "keep-object.o"
                untouched.write_bytes(b"reusable")
                expected = [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                            for p in paths]
                with self.subTest(module=module.__name__, tiles=tiles), \
                     patch.object(module, "pch_recovery_paths", wraps=module.pch_recovery_paths) as select:
                    self.assertEqual(module.invalidate_owned_pch(self.root, prefix, tiles=tiles), expected)
                    select.assert_called_once_with(self.root, prefix, tiles=tiles)
                self.assertEqual(module.invalidate_owned_pch(self.root, prefix, tiles=tiles), [])
                self.assertEqual(untouched.read_bytes(), b"reusable")

    def test_read_failure_at_actual_wrappers_does_not_delete_or_retry(self):
        read = Path.read_bytes
        for module in (macos, windows):
            prefix = module.__name__ + "-read-"
            selected = module.pch_recovery_paths(self.root, prefix, tiles=False)[0]
            selected.parent.mkdir(parents=True)
            selected.write_bytes(b"preserved")
            error = PermissionError("fixture read refused")
            def fail(path):
                if path == selected:
                    raise error
                return read(path)
            with self.subTest(module=module.__name__), patch.object(Path, "read_bytes", fail), \
                 patch.object(Path, "unlink", side_effect=AssertionError("no deletion")) as unlink:
                with self.assertRaises(PermissionError) as caught:
                    module.invalidate_owned_pch(self.root, prefix, tiles=False)
                self.assertIs(caught.exception, error)
                unlink.assert_not_called()
            self.assertEqual(selected.read_bytes(), b"preserved")

    def test_unlink_failure_at_actual_wrappers_propagates_without_retry(self):
        for module in (macos, windows):
            prefix = module.__name__ + "-unlink-"
            selected = module.pch_recovery_paths(self.root, prefix, tiles=False)[0]
            selected.parent.mkdir(parents=True)
            selected.write_bytes(b"preserved")
            error = PermissionError("fixture unlink refused")
            with self.subTest(module=module.__name__), patch.object(Path, "unlink", side_effect=error) as unlink:
                with self.assertRaises(PermissionError) as caught:
                    module.invalidate_owned_pch(self.root, prefix, tiles=False)
                self.assertIs(caught.exception, error)
                unlink.assert_called_once_with()
            self.assertEqual(selected.read_bytes(), b"preserved")

    def test_explicit_symlink_removal_preserves_its_target(self):
        target, link = self.root / "target", self.root / "selected.pch"
        target.write_bytes(b"target bytes")
        link.symlink_to(target)
        self.assertEqual(recovery.remove_owned_pch_files((link,)),
                         [{'path': str(link), 'sha256': hashlib.sha256(b"target bytes").hexdigest()}])
        self.assertFalse(link.is_symlink())
        self.assertEqual(target.read_bytes(), b"target bytes")

    def test_failure_payload_retains_fields_references_and_none_distinction(self):
        for status in (0, 7, -3):
            phase = {"label": "build", "exit_status": status, "diagnostic": "fixture é failure",
                     "diagnostic_truncated": True, "command": ["fixture"]}
            original = dict(phase)
            payload = recovery.build_failure_payload(phase)
            self.assertEqual(payload, {"ok": False, "phase": "build", "exit_status": status,
                                      "diagnostic": "fixture é failure", "logs": phase})
            self.assertIs(payload['logs'], phase)
            self.assertEqual(phase, original)
            source, pch = {}, {}
            payload = recovery.build_failure_payload(phase, source=source, pch_recovery=pch)
            self.assertIs(payload['source'], source)
            self.assertIs(payload['pch_recovery'], pch)

    def test_missing_required_failure_field_still_raises(self):
        phase = {"label": "build", "exit_status": 7, "diagnostic": "fixture"}
        for key in phase:
            missing = dict(phase)
            del missing[key]
            with self.subTest(key=key), self.assertRaises(KeyError) as caught:
                recovery.build_failure_payload(missing)
            self.assertEqual(caught.exception.args, (key,))

    def test_actual_main_version_failure_keeps_local_output_and_exit_status(self):
        for module in (macos, windows):
            for status in (7, -3):
                log_dir = self.root / f"{module.__name__}-{status}"
                argv = [module.__file__, "--renderer", "tiles", "--log-dir", str(log_dir)]
                if module is windows:
                    msys = self.root / "msys"
                    for relative in ("usr/bin/bash.exe", "ucrt64/bin/g++.exe"):
                        path = msys / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(b"fixture never executed")
                    argv += ["--msys-root", str(msys)]
                calls = []
                def run(command, **kwargs):
                    calls.append(command)
                    for name, data in (("stdout", b"version out\n"), ("stderr", b"fixture version failed\n")):
                        if name in kwargs:
                            kwargs[name].write(data)
                    return SimpleNamespace(returncode=status, stdout=b"version out\n",
                                           stderr=b"fixture version failed\n")
                output, error = io.StringIO(), io.StringIO()
                with self.subTest(module=module.__name__, status=status), ExitStack() as stack:
                    for obj, name, value in ((sys, "argv", argv), (sys, "platform", "darwin"),
                                             (sys, "stdout", output), (sys, "stderr", error),
                                             (module, "ROOT", self.root)):
                        stack.enter_context(patch.object(obj, name, value))
                    if module is windows:
                        stack.enter_context(patch.object(module, "os", SimpleNamespace(name="nt", environ={})))
                    stack.enter_context(patch.object(module.subprocess, "run", side_effect=run))
                    stack.enter_context(patch.object(module.startup_harness, "product_source_binding",
                                                    return_value={"ok": True, "sha256": "fixture"}))
                    stack.enter_context(patch.object(module.startup_harness, "current_head_short", return_value="aaaaaaaaaa"))
                    stack.enter_context(patch.object(module.startup_harness, "product_build_receipt_path",
                                                    return_value=self.root / "unpublished.json"))
                    payload_owner = stack.enter_context(patch.object(module, "build_failure_payload",
                                                                     wraps=recovery.build_failure_payload))
                    self.assertEqual(module.main(), status)
                    payload_owner.assert_called_once()
                payload = json.loads(error.getvalue())
                self.assertEqual(output.getvalue(), "")
                self.assertEqual(len(calls), 1)
                self.assertEqual(payload['phase'], 'version')
                self.assertEqual(payload['exit_status'], status)
                self.assertEqual(payload['diagnostic'], 'fixture version failed')
                self.assertEqual(payload['logs']['command'][-1], 'version')
                self.assertTrue(Path(payload['logs']['full_log']).is_file())


if __name__ == "__main__":
    unittest.main()
