"""Native lease identity tests; kernel handles are doubles, not game proof."""
from __future__ import annotations

from contextlib import ExitStack
from copy import deepcopy
import ctypes
from dataclasses import replace
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from certification_process_lease import ProcessSnapshot, process_command_identity
import startup_harness
import windows_native_process as native
from writable_run_owner import WindowsWritableRunOwner


class LeaseKernel:
    """Only the native handle API is replaced; producer and owner are real."""

    def __init__(self):
        self.paths = {}
        self.next_handle = 100
        self.opened_pids = []
        self.duplicates = []
        self.closed = []

    def file_handle(self, path):
        self.next_handle += 1
        self.paths[self.next_handle] = os.path.normcase(os.path.normpath(str(path)))
        return self.next_handle

    def CreateFileW(self, path, *_):
        return self.file_handle(path)

    def GetFinalPathNameByHandleW(self, handle, buffer, *_):
        buffer.value = self.paths[getattr(handle, "value", handle)]
        return len(buffer.value)

    def OpenProcess(self, access, inherit, pid):
        self.opened_pids.append(pid)
        return 9000

    def GetCurrentProcess(self):
        return 9001

    def DuplicateHandle(self, source, handle, destination, output, *_):
        duplicate = self.file_handle(self.paths[handle])
        ctypes.cast(output, ctypes.POINTER(native.w.HANDLE)).contents.value = duplicate
        self.duplicates.append(duplicate)
        return True

    def CloseHandle(self, handle):
        self.closed.append(handle)
        return True


class NativeLeaseAdoptionTest(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        temporary = self.stack.enter_context(tempfile.TemporaryDirectory())
        self.root = Path(temporary).resolve() / "profile"
        self.root.mkdir()
        self.kernel = LeaseKernel()
        self.snapshot = ProcessSnapshot(os.getpid(), True, "owned-python.exe",
                                        "windows-filetime:123456789", "owned-launcher --test")
        self.inspector = SimpleNamespace(inspect=lambda pid: self.snapshot)
        self.stack.enter_context(patch.object(native, "lease_kernel", return_value=self.kernel))
        self.stack.enter_context(patch.object(native, "WindowsProcessInspector",
                                              return_value=self.inspector))
        self.stack.enter_context(patch.object(os, "O_BINARY", getattr(os, "O_BINARY", 0),
                                              create=True))
        self.stack.enter_context(patch.dict(sys.modules, {
            "msvcrt": SimpleNamespace(get_osfhandle=lambda fd:
                                      self.kernel.file_handle(self.root / ".harness-owner.lock")),
        }))
        # This is the actual startup caller's public generation producer.
        inspect = lambda pid: startup_harness.process_generation_snapshot(
            pid, inspector=self.inspector)
        self.owner = WindowsWritableRunOwner(self.root, "native-test", inspect,
                                             startup_harness.process_generation_matches)
        self.stack.callback(self.owner.close)
        self.owner.begin_launch()
        self.export = self.owner.export_native_lease()

    def adopt(self, export=None, run_id="native-test"):
        return native.adopt_native_lease(export or self.export, run_id=run_id)

    def assert_identity_rejected(self):
        with self.assertRaisesRegex(OSError, "launcher identity unavailable or changed"):
            self.adopt()
        self.assertEqual(self.kernel.opened_pids, [])
        self.assertEqual(self.kernel.duplicates, [])

    def test_public_producer_owner_export_adopts_exact_hashed_generation(self):
        persisted = json.loads(self.owner.path.read_text())["launcher_generation"]
        self.assertNotIn("command", persisted)
        self.assertEqual(persisted["command_sha256"],
                         process_command_identity({"command": self.snapshot.command}))
        self.assertEqual(self.export["owner_generation"], persisted)
        held = self.adopt()
        self.addCleanup(lambda: [self.kernel.CloseHandle(handle) for handle in held])
        self.assertEqual(len(held), len(self.export["handles"]))
        self.assertEqual(self.kernel.opened_pids, [os.getpid()])
        self.assertIn(9000, self.kernel.closed)

    def test_pid_reuse_birth_executable_command_or_dead_launcher_rejected(self):
        original = self.snapshot
        changes = ({"pid": original.pid + 1}, {"birth_identity": "windows-filetime:other"},
                   {"executable_path": "other-python.exe"}, {"command": "different-launcher"},
                   {"alive": False}, {"birth_identity": ""}, {"executable_path": ""})
        for change in changes:
            with self.subTest(change=change):
                self.snapshot = replace(original, **change)
                self.assert_identity_rejected()

    def test_wrong_missing_or_malformed_command_fingerprint_rejected(self):
        for digest in ("0" * 64, "", "not-a-digest", None):
            with self.subTest(digest=digest):
                self.export["owner_generation"]["command_sha256"] = digest
                self.assert_identity_rejected()

    def test_wrong_host_run_or_root_alias_rejected_before_handle_adoption(self):
        for key, value in (("host", "other-host"), ("run_id", "other-run"),
                           ("userdir", str(self.root / ".." / "profile"))):
            with self.subTest(key=key):
                export = deepcopy(self.export)
                export[key] = value
                with self.assertRaises(OSError):
                    self.adopt(export)
                self.assertEqual(self.kernel.opened_pids, [])
                self.assertEqual(self.kernel.duplicates, [])

    def test_reservation_path_and_generation_still_must_match(self):
        marker = json.loads(self.owner.path.read_text())
        for change in ({"userdir": str(self.root.parent)}, {"launch_state": "bound"},
                       {"launcher_generation": {"pid": os.getpid()}}):
            with self.subTest(change=change):
                self.owner.path.write_text(json.dumps({**marker, **change}))
                with self.assertRaisesRegex(OSError, "reservation mismatch"):
                    self.adopt()
                self.assertEqual(self.kernel.opened_pids, [])

    def test_wrong_transferred_path_closes_duplicate_and_process_handles(self):
        export = deepcopy(self.export)
        export["paths"][0] += "-other"
        with self.assertRaisesRegex(OSError, "handle path mismatch"):
            self.adopt(export)
        self.assertEqual(len(self.kernel.duplicates), 1)
        self.assertTrue(set(self.kernel.duplicates + [9000]).issubset(self.kernel.closed))

    def test_identity_change_during_transfer_closes_all_adopted_handles(self):
        snapshots = iter((self.snapshot, replace(self.snapshot,
                                                birth_identity="windows-filetime:reused")))
        self.inspector.inspect = lambda pid: next(snapshots)
        with self.assertRaisesRegex(OSError, "launcher changed during handle transfer"):
            self.adopt()
        self.assertEqual(len(self.kernel.duplicates), len(self.export["handles"]))
        self.assertTrue(set(self.kernel.duplicates + [9000]).issubset(self.kernel.closed))


if __name__ == "__main__":
    unittest.main()
