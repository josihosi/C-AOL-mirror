#!/usr/bin/env python3
"""Saved-world snapshot copies retain bytes while becoming usable profiles."""

from __future__ import annotations

import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HARNESS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(HARNESS_DIR))

import startup_harness


class SavedWorldSnapshotInstallTest(unittest.TestCase):
    def test_read_only_snapshot_and_destination_install_as_writable_exact_copy(self):
        with tempfile.TemporaryDirectory(prefix="saved_world_snapshot_") as temporary:
            root = Path(temporary)
            source = root / "source" / "TestSetup00"
            source_maps = source / "maps"
            source_maps.mkdir(parents=True)
            source_file = source_maps / "4.4.-3.zzip"
            source_file.write_bytes(b"saved map bytes")
            source_dir_original_mode = stat.S_IMODE(source_maps.stat().st_mode)
            source_file_original_mode = stat.S_IMODE(source_file.stat().st_mode)
            source_maps.chmod(0o500)
            source_file.chmod(0o400)
            source_readonly_dir_mode = stat.S_IMODE(source_maps.stat().st_mode)
            source_readonly_file_mode = stat.S_IMODE(source_file.stat().st_mode)
            if os.name == "nt":
                # Windows chmod controls the read-only attribute, not POSIX
                # execute bits (0500 is reported as 0555 on this host).
                self.assertTrue(source_file.stat().st_file_attributes & stat.FILE_ATTRIBUTE_READONLY)
                with self.assertRaises(PermissionError):
                    with source_file.open("ab"):
                        pass
            else:
                self.assertEqual(source_readonly_dir_mode, 0o500)
                self.assertEqual(source_readonly_file_mode, 0o400)

            profile_save = root / "profile" / "save"
            old_world = profile_save / "TestSetup00"
            old_maps = old_world / "maps"
            old_maps.mkdir(parents=True)
            old_file = old_maps / "obsolete.zzip"
            old_file.write_bytes(b"old disposable profile")
            old_maps.chmod(0o500)
            old_file.chmod(0o400)

            try:
                source_hash, source_error = startup_harness.sha256_tree(source)
                self.assertEqual(source_error, "")
                with mock.patch.object(startup_harness, "save_dir_for_profile",
                                       return_value=profile_save):
                    result = startup_harness.install_saved_world_snapshot(
                        "disposable-profile", "TestSetup00", source, replace=True,
                    )

                installed = profile_save / "TestSetup00"
                installed_file = installed / "maps" / "4.4.-3.zzip"
                installed_hash, installed_error = startup_harness.sha256_tree(installed)
                self.assertEqual(result["status"], "green_saved_world_snapshot_installed")
                self.assertEqual(result["source_sha256"], source_hash)
                self.assertEqual(installed_error, "")
                self.assertEqual(installed_hash, source_hash)
                self.assertEqual(installed_file.read_bytes(), b"saved map bytes")
                self.assertFalse((installed / "maps" / "obsolete.zzip").exists())
                self.assertTrue(stat.S_IMODE((installed / "maps").stat().st_mode) & stat.S_IWUSR)
                self.assertTrue(stat.S_IMODE(installed_file.stat().st_mode) & stat.S_IWUSR)
                marker = installed / "profile-copy-is-writable"
                marker.write_text("ok", encoding="utf-8")
                marker.unlink()
                self.assertEqual(stat.S_IMODE(source_maps.stat().st_mode), source_readonly_dir_mode)
                self.assertEqual(stat.S_IMODE(source_file.stat().st_mode), source_readonly_file_mode)
                self.assertEqual(startup_harness.sha256_tree(source), (source_hash, ""))
                if os.name == "nt":
                    self.assertTrue(source_file.stat().st_file_attributes & stat.FILE_ATTRIBUTE_READONLY)
                self.assertEqual(result["profile_copy_mode_policy"],
                                 "owner_writable_files_and_directories")
            finally:
                source_file.chmod(source_file_original_mode)
                source_maps.chmod(source_dir_original_mode)

    def test_long_world_save_paths_use_short_owned_sibling_and_preserve_bytes(self):
        with tempfile.TemporaryDirectory(prefix="snapshot_path_") as temporary:
            root = Path(temporary)
            world = "mixed_stress_f7c_seed830205385"
            relative = Path("#RGFtaWFuIE8nQ29ubm9y.mm1") / "#RGFtaWFuIE8nQ29ubm9y.mm1.cold.zzip"

            def save_root(label):
                parent = root / label
                padding = 224 - len(str(parent / world / relative)) - 1
                self.assertGreater(padding, 0)
                return parent / ("p" * padding)

            source = save_root("source") / world
            payload = source / relative
            payload.parent.mkdir(parents=True)
            payload.write_bytes(b"native save payload")
            source_hash = startup_harness.sha256_tree(source)
            destination_root = save_root("profile")
            self.assertEqual(len(str(destination_root / world / relative)), 224)
            old_staging = destination_root / ("." + world + ".replacement-" + "0" * 32 + ".tmp")
            self.assertEqual(len(str(old_staging / relative)), 274)
            copied = []
            copytree = shutil.copytree

            def copy(src, dst, *args, **kwargs):
                copied.append(Path(dst))
                return copytree(src, dst, *args, **kwargs)

            with mock.patch.object(startup_harness, "save_dir_for_profile", return_value=destination_root), \
                    mock.patch.object(startup_harness.shutil, "copytree", side_effect=copy):
                result = startup_harness.install_saved_world_snapshot("profile", world, source)
            self.assertEqual(result["status"], "green_saved_world_snapshot_installed")
            self.assertEqual(result["source_sha256"], result["installed_sha256"])
            self.assertEqual(startup_harness.sha256_tree(source), source_hash)
            self.assertEqual((destination_root / world / relative).read_bytes(), payload.read_bytes())
            self.assertLess(len(str(copied[0] / relative)), 260)
            self.assertFalse(copied[0].exists())
            self.assertEqual(list(destination_root.iterdir()), [destination_root / world])

    def test_copy_or_hash_failure_removes_only_owned_staging_and_preserves_destination(self):
        for failure in ("copy", "hash"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(prefix="snapshot_failure_") as temporary:
                root = Path(temporary)
                source, save_root = root / "source", root / "save"
                source.mkdir()
                (source / "native.sav").write_bytes(b"new save")
                destination, unrelated = save_root / "World", save_root / ".unrelated"
                destination.mkdir(parents=True)
                (destination / "old.sav").write_bytes(b"existing save")
                unrelated.mkdir()
                (unrelated / "keep").write_bytes(b"other owner")
                original_source = startup_harness.sha256_tree(source)
                original_destination = startup_harness.sha256_tree(destination)

                def fail_copy(src, dst, **kwargs):
                    (Path(dst) / "partial").write_bytes(b"partial")
                    raise OSError("controlled copy failure")

                tree_hash = startup_harness.sha256_tree

                def fail_hash(path):
                    return ("", "controlled hash failure") if Path(path).name.startswith(".snapshot-") else tree_hash(path)

                patch = mock.patch.object(startup_harness.shutil, "copytree", side_effect=fail_copy) if failure == "copy" else \
                    mock.patch.object(startup_harness, "sha256_tree", side_effect=fail_hash)
                with mock.patch.object(startup_harness, "save_dir_for_profile", return_value=save_root), patch:
                    with self.assertRaisesRegex(OSError if failure == "copy" else SystemExit, "controlled .* failure"):
                        startup_harness.install_saved_world_snapshot("profile", "World", source, replace=True)
                self.assertEqual(startup_harness.sha256_tree(source), original_source)
                self.assertEqual(startup_harness.sha256_tree(destination), original_destination)
                self.assertEqual((unrelated / "keep").read_bytes(), b"other owner")
                self.assertEqual(set(save_root.iterdir()), {destination, unrelated})


if __name__ == "__main__":
    unittest.main()
