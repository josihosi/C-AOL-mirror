"""Complete native records reach the existing bounded, lossless public channel."""
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

import startup_harness as startup


class DescriptorIntakeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = self.root / "run"
        self.run.mkdir()
        self.config = self.root / "config"
        self.config.mkdir()
        self.trace = self.run / "semantic.native.events.jsonl"
        (self.run / "terminal.owner.json").write_text("{}")
        self.descriptor = {
            "event": "surface_descriptor", "schema_version": 1,
            "run_id": "fixture-run", "process_instance": "fixture-process", "sequence": 2,
            "game_turn": 100, "game_minutes": 1, "surface_id": "fixture-surface",
            "frame_id": "fixture-frame", "kind": "inventory", "breadcrumbs": ["Inventory"],
            "payload": {"title": "Inventory"}, "valid_actions": [
                {"id": "inventory.filter", "stable_id": "", "label": "Filter", "enabled": True}],
        }
        for context in (
            patch.object(startup, "config_dir_for_profile", return_value=self.config),
            patch.object(startup, "resolve_profile_name", return_value="fixture"),
            patch.object(startup, "current_owned_process_generation", return_value={
                "pid": os.getpid(), "status": "alive",
                "expected": {"pid": os.getpid(), "birth_identity": "disposable-fixture"},
            }),
            patch.dict(startup.os.environ, {
                "OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": "",
                "OPENCLAW_COCKPIT_BRIDGE_BINDING_ID": "",
            }),
        ):
            context.start()
            self.addCleanup(context.stop)

    def encode(self, event):
        return (startup.SEMANTIC_STEP_PREFIX + json.dumps(event) + "\n").encode()

    def publish(self, data):
        self.trace.write_bytes(data)
        (self.config / "debug.log").write_bytes(data)

    def service(self):
        return startup.open_cockpit_game_service(
            profile="fixture", run_dir=self.run, run_id=self.descriptor["run_id"], trace_start_offset=0,
            pid=os.getpid(),
            session_id="disposable-fixture", cleanup_on_finish=False,
            transition_timeout_seconds=getattr(self, "transition_timeout", 3),
            observe_interval_seconds=.001,
        )

    def frame(self):
        return startup.current_semantic_step_frame(
            profile="fixture", run_dir=self.run, run_id=self.descriptor["run_id"], start_offset=0)

    def test_replaced_larger_journal_changes_public_owner_not_cached_authority(self):
        for replace_inode in (True, False):
            with self.subTest(replace_inode=replace_inode):
                first_header = self.encode({"event": "trace_generation", "run_id": "fixture-run",
                                            "generation": "first"})
                self.publish(first_header + self.encode(self.descriptor))
                service = self.service()
                observed = service.call({"action": "game.observe"})
                self.assertTrue(observed["ok"], observed)
                newer = {**self.descriptor, "surface_id": "new-owner", "frame_id": "new-frame",
                         "sequence": 3, "payload": {"facts": "x" * 10000}}
                replacement = self.encode({"event": "trace_generation", "run_id": "fixture-run",
                                           "generation": "second"}) + self.encode(newer)
                if replace_inode:
                    temporary = self.trace.with_suffix(".next")
                    temporary.write_bytes(replacement)
                    temporary.replace(self.trace)
                else:
                    self.trace.write_bytes(replacement)
                current = service.call({"action": "game.observe"})
                self.assertTrue(current["ok"], current)
                self.assertEqual(current["result"]["observation_id"], "new-frame")
                rejected = service.call({"action": "game.act", "observation_id": "fixture-frame",
                                         "action_id": "inventory.filter"})
                self.assertFalse(rejected["ok"])
                # A stale observation never earns acceptance or a new input grant.
                # A replaced file without a valid new owner must never borrow
                # the previous generation's cached descriptor.
                self.trace.write_bytes(self.encode({"event": "trace_generation", "run_id": "fixture-run",
                                                    "generation": "third"}) +
                                       self.encode({**newer, "run_id": "foreign-run"}))
                self.assertFalse(service.call({"action": "game.observe"})["ok"])

    def test_large_action_and_payload_catalogs_are_lossless_not_item_capped(self):
        original = {**self.descriptor, "payload": {"title": "Inventory", "facts": "x" * 600000},
                    "valid_actions": [
                        {"id": "inventory.toggle", "stable_id": "item:" + str(i),
                         "label": "Item " + str(i), "enabled": True} for i in range(50000)]}
        raw = self.encode(original)
        self.assertGreater(len(raw), startup.SEMANTIC_STEP_MAX_BYTES * 16)
        self.publish(raw)
        frame = self.frame()
        self.assertEqual(frame["valid_actions"], original["valid_actions"])
        self.assertEqual(frame["payload"], original["payload"])
        self.assertEqual(frame["_source_path"], str(self.trace.resolve()))
        self.assertEqual(frame["_source_offset"], 0)
        self.assertEqual(frame["_source_end"], len(raw))
        self.assertLessEqual((self.run / "semantic.native.log").stat().st_size,
                             startup.SEMANTIC_STEP_MAX_BYTES)
        self.assertEqual(startup.latest_semantic_source_descriptor(self.trace)["frame_id"],
                         original["frame_id"])
        observed = self.service().call({"action": "game.observe"})
        self.assertTrue(observed["ok"], observed)
        self.assertEqual(observed["result"]["observation_id"], original["frame_id"])
        self.assertFalse((self.run / "semantic.requests.jsonl").exists())

    def test_oversized_first_owner_after_actor_output_is_not_replaced_by_old_mirror(self):
        current = {**self.descriptor, "payload": {"facts": "x" * 600000}}
        self.trace.write_bytes(b"actor output " + b"x" * 5000000 + b"\n" + self.encode(current))
        (self.config / "debug.log").write_bytes(self.encode({
            **self.descriptor, "sequence": 1, "frame_id": "old-mirror",
        }))
        self.assertEqual(startup.semantic_step_source_trace("fixture", self.run), self.trace)
        frame = self.frame()
        self.assertEqual(frame["frame_id"], current["frame_id"])
        self.assertEqual(frame["payload"], current["payload"])
        self.assertEqual(frame["valid_actions"], current["valid_actions"])

    def test_sampled_partial_record_refuses_instead_of_restoring_previous_owner(self):
        previous = self.encode({**self.descriptor, "frame_id": "previous"})
        newer = self.encode({**self.descriptor, "payload": {"facts": "x" * 5000000}})
        self.publish(previous + newer[:-1])
        with self.assertRaisesRegex(ValueError, "incomplete_native_semantic_record"):
            self.frame()
        with self.assertRaisesRegex(ValueError, "incomplete_native_semantic_record"):
            startup.latest_semantic_source_descriptor(self.trace)
        self.assertFalse((self.run / "semantic.requests.jsonl").exists())

    def test_sample_does_not_include_a_record_terminated_only_after_sample(self):
        raw = self.encode(self.descriptor)
        with self.assertRaisesRegex(ValueError, "incomplete_native_semantic_record"):
            list(startup.iter_complete_semantic_trace_records(io.BytesIO(raw), len(raw) - 1))

    def test_malformed_and_unsupported_records_refuse(self):
        for raw, error in (
            (startup.SEMANTIC_STEP_PREFIX.encode() + b'{"event":broken}\n',
             "malformed_native_semantic_record"),
            (self.encode({**self.descriptor, "event": "unsupported"}),
             "malformed_semantic_step"),
        ):
            with self.subTest(error=error):
                self.publish(raw)
                with self.assertRaisesRegex(ValueError, error):
                    self.frame()

    def test_foreign_run_and_duplicate_action_catalog_do_not_authorize(self):
        for descriptor in (
            {**self.descriptor, "run_id": "foreign"},
            {**self.descriptor, "valid_actions": self.descriptor["valid_actions"] * 2},
        ):
            self.publish(self.encode(descriptor))
            with self.assertRaises(ValueError):
                self.frame()
            self.assertFalse((self.run / "semantic.requests.jsonl").exists())

    def test_optional_malformed_mirror_does_not_poison_canonical_observe(self):
        self.trace.write_bytes(self.encode(self.descriptor))
        mirror = self.config / "debug.log"
        mirror.write_bytes(self.encode(self.descriptor).rstrip(b"\n") + b" skipped=0\n")
        with self.assertRaisesRegex(ValueError, "malformed_native_semantic_record"):
            startup.latest_semantic_source_descriptor(mirror)
        self.assertEqual(startup.semantic_step_source_trace("fixture", self.run), self.trace)
        observed = self.service().call({"action": "game.observe"})
        self.assertTrue(observed["ok"], observed)
        self.assertEqual(observed["result"]["observation_id"], self.descriptor["frame_id"])
        self.assertFalse((self.run / "semantic.requests.jsonl").exists())
        # The original diagnostic is neither rewritten nor accepted as JSON.
        self.assertTrue(mirror.read_bytes().endswith(b" skipped=0\n"))

    def test_newer_mirror_cannot_replace_canonical_owner_or_break_settlement(self):
        # The original diagnostic joined an activity owner and a movement
        # receipt on one line. Later valid mirror records hide that damage
        # outside the selector's suffix probe, but not a source-switch read.
        self.trace.write_bytes(self.encode(self.descriptor))
        mirror = self.config / "debug.log"
        joined = self.encode(self.descriptor).rstrip(b"\n") + \
            b'12:05:30.415 INFO : ' + self.encode({
                "event": "receipt", "run_id": "fixture-run", "accepted": True})
        newer = {**self.descriptor, "sequence": 99, "frame_id": "mirror-only-frame"}
        mirror_bytes = joined + b"diagnostic padding " + \
            b"x" * (startup.SEMANTIC_STEP_MAX_BYTES * 16 + 1) + b"\n" + self.encode(newer)
        mirror.write_bytes(mirror_bytes.replace(b'"sequence": 99', b'"sequence": 1'))
        self.assertEqual(startup.latest_semantic_source_descriptor(mirror)["frame_id"],
                         "mirror-only-frame")
        with mirror.open("rb") as stream:
            with self.assertRaisesRegex(ValueError, "malformed_native_semantic_record"):
                list(startup.iter_complete_semantic_trace_records(stream, mirror.stat().st_size))
        service = self.service()
        observed = service.call({"action": "game.observe"})
        self.assertTrue(observed["ok"], observed)
        current = observed["result"]["observation_id"]
        self.assertEqual(current, self.descriptor["frame_id"])
        self.assertFalse(service.call({"action": "game.act", "observation_id": "mirror-only-frame",
                                      "action_id": "inventory.filter"})["ok"])
        self.assertFalse((self.run / "semantic.requests.jsonl").exists())

        def native_fixture(*args, **kwargs):
            request = json.loads((self.run / "semantic.requests.jsonl").read_text().splitlines()[-1])
            successor = {**self.descriptor, "sequence": 4, "frame_id": "canonical-successor"}
            receipt = {"event": "surface_receipt", "run_id": "fixture-run", "sequence": 3,
                "request_id": request["request_id"], "action_id": request["action_id"],
                "requested_run_id": "fixture-run", "requested_surface_id": request["surface_id"],
                "requested_frame_id": request["frame_id"], "consuming_surface_id": request["surface_id"],
                "consuming_frame_id": request["frame_id"], "accepted": True,
                "rejection_reason": "", "resulting_frame_id": successor["frame_id"]}
            with self.trace.open("ab") as stream:
                stream.write(self.encode(receipt) + self.encode(successor))
            # The optional suffix is sampled after native publication. A
            # later-looking mirror must not change this transaction's source
            # or discard the canonical receipt already emitted for its input.
            mirror.write_bytes(mirror_bytes)
            return 1

        with patch.object(startup, "semantic_wake_pipe_contract", return_value={
                "status": "bound", "path": "fixture", "contract": {"transport": "fixture"}}), \
                patch.object(startup, "write_semantic_wake_pipe", side_effect=native_fixture):
            acted = service.call({"action": "game.act", "observation_id": current,
                                  "action_id": "inventory.filter"})
        self.assertTrue(acted["ok"], acted)
        self.assertEqual(acted["observation"]["observation_id"], "canonical-successor")
        self.assertEqual(service.call({"action": "game.observe"})["result"]["observation_id"],
                         "canonical-successor")
        self.assertFalse(service.call({"action": "game.act", "observation_id": current,
                                      "action_id": "inventory.filter"})["ok"])
        self.assertEqual(len((self.run / "semantic.requests.jsonl").read_text().splitlines()), 1)
        self.assertEqual(mirror.read_bytes(), mirror_bytes)

    def test_canonical_corruption_and_foreign_run_still_refuse_with_valid_mirror(self):
        mirror = self.config / "debug.log"
        mirror.write_bytes(self.encode({**self.descriptor, "sequence": 99}))
        for raw in (
            self.encode(self.descriptor).rstrip(b"\n") + b" skipped=0\n",
            self.encode(self.descriptor)[:-1],
            self.encode({**self.descriptor, "run_id": "foreign"}),
        ):
            with self.subTest(raw=raw[-40:]):
                self.trace.write_bytes(raw)
                observed = self.service().call({"action": "game.observe"})
                self.assertFalse(observed["ok"], observed)
                self.assertFalse((self.run / "semantic.requests.jsonl").exists())

    def test_foreign_process_or_run_mirror_cannot_replace_current_canonical_owner(self):
        self.trace.write_bytes(self.encode(self.descriptor))
        for field in ("run_id", "process_instance"):
            with self.subTest(field=field):
                (self.config / "debug.log").write_bytes(self.encode({
                    **self.descriptor, field: "foreign", "sequence": 99,
                    "frame_id": "foreign-frame"}))
                self.assertEqual(startup.semantic_step_source_trace("fixture", self.run), self.trace)
                observed = self.service().call({"action": "game.observe"})
                self.assertTrue(observed["ok"], observed)
                self.assertEqual(observed["result"]["observation_id"], self.descriptor["frame_id"])
                self.assertFalse((self.run / "semantic.requests.jsonl").exists())

    def test_public_current_choice_dispatches_once_stale_and_disabled_refuse(self):
        self.publish(self.encode(self.descriptor))
        service = self.service()
        current = service.call({"action": "game.observe"})["result"]["observation_id"]
        self.assertFalse(service.call({"action": "game.act", "observation_id": "old",
                                      "action_id": "inventory.filter"})["ok"])
        self.assertFalse((self.run / "semantic.requests.jsonl").exists())

        def native_fixture(*args, **kwargs):
            request = json.loads((self.run / "semantic.requests.jsonl").read_text().splitlines()[-1])
            successor = {**self.descriptor, "surface_id": "next-surface", "frame_id": "next-frame",
                         "sequence": 4, "valid_actions": [
                             {**self.descriptor["valid_actions"][0], "enabled": False}]}
            receipt = {"event": "surface_receipt", "run_id": "fixture-run", "sequence": 3,
                       "request_id": request["request_id"], "action_id": request["action_id"],
                       "requested_run_id": "fixture-run", "requested_surface_id": request["surface_id"],
                       "requested_frame_id": request["frame_id"],
                       "consuming_surface_id": request["surface_id"],
                       "consuming_frame_id": request["frame_id"], "accepted": True,
                       "rejection_reason": "", "resulting_frame_id": "next-frame"}
            successor_bytes = self.encode(successor)
            if getattr(self, "replace_during_dispatch", False):
                temporary = self.trace.with_suffix(".next")
                header = {"event": "trace_generation", "run_id": "fixture-run", "generation": "dispatch"}
                successor["payload"] = {"facts": "x" * 600000}
                refused = {**receipt, "request_id": "unrelated-refusal", "accepted": False,
                           "rejection_reason": "wrong_surface", "resulting_frame_id": ""}
                diagnostics = b"".join(self.encode({"event": "request_transport",
                    "run_id": "fixture-run", "sequence": index + 10, "stage": "idle",
                    "request_id": "", "offset_before": 0, "offset_after": 0, "transport_end": 0,
                    "queued": False, "wake_pending": False})
                    for index in range(getattr(self, "transport_count", startup.SEMANTIC_STEP_MAX_EVENTS + 10)))
                redraw = ({**successor, "sequence": 5, "frame_id": "current-redraw"}
                          if getattr(self, "redraw_after_receipt", False) else None)
                temporary.write_bytes(self.encode(header) + self.encode(receipt) + self.encode(successor)
                                      + (self.encode(redraw) if redraw else b"")
                                      + self.encode(refused) + diagnostics)
                temporary.replace(self.trace)
                return 1
            with self.trace.open("ab") as stream:
                stream.write(self.encode(receipt))
                if getattr(self, "partial_delivery", False):
                    stream.write(successor_bytes[:-1])
                else:
                    stream.write(successor_bytes)
            if getattr(self, "partial_delivery", False):
                def complete_native_record():
                    with self.trace.open("ab") as stream:
                        stream.write(successor_bytes[-1:])
                completion = threading.Timer(.04, complete_native_record)
                completion.start()
                self.addCleanup(completion.join)
            return 1

        with patch.object(startup, "semantic_wake_pipe_contract",
                          return_value={"status": "bound", "path": "fixture",
                                        "contract": {"transport": "disposable-native-fixture"}}), \
                patch.object(startup, "write_semantic_wake_pipe", side_effect=native_fixture):
            acted = service.call({"action": "game.act", "observation_id": current,
                                  "action_id": "inventory.filter", "parameters": {"filter": "M240"}})
        self.assertTrue(acted["ok"], acted)
        self.assertEqual(acted["receipt"]["native_receipt"]["resulting_frame_id"], "next-frame")
        self.assertEqual(len((self.run / "semantic.requests.jsonl").read_text().splitlines()), 1)
        self.assertFalse(service.call({"action": "game.act", "observation_id": current,
                                      "action_id": "inventory.filter"})["ok"])
        observed_after = service.call({"action": "game.observe"})
        self.assertTrue(observed_after["ok"], observed_after)
        after = observed_after["result"]["observation_id"]
        self.assertEqual(after, "current-redraw" if getattr(self, "redraw_after_receipt", False)
                         else "next-frame")
        if getattr(self, "cold_after_receipt", False):
            # Cold/current-generation reconstruction must agree with the hot
            # adapter, whose cursor can already lie after the old successor.
            cold = self.service().call({"action": "game.observe"})
            self.assertTrue(cold["ok"], cold)
            self.assertEqual(cold["result"]["observation_id"], after)
        self.assertFalse(service.call({"action": "game.act", "observation_id": after,
                                      "action_id": "inventory.filter"})["ok"])
        self.assertEqual(len((self.run / "semantic.requests.jsonl").read_text().splitlines()), 1)

    def test_optional_diagnostic_error_keeps_same_controller_save_choice_callable(self):
        # Real factory/channel/receipt path; fixture transport is not a game save.
        self.descriptor = {**self.descriptor, "kind": "world", "payload": {"turn": "5217816"},
            "valid_actions": [{"id": "world.quicksave", "stable_id": "", "label": "Quicksave", "enabled": True}]}
        self.trace.write_bytes(self.encode(self.descriptor))
        mirror = self.config / "debug.log"
        malformed = self.encode(self.descriptor).rstrip(b"\n") + b" skipped=0\n"
        mirror.write_bytes(malformed)
        service = self.service()
        before = service.call({"action": "game.observe"})
        self.assertTrue(before["ok"], before)
        current = before["result"]["observation_id"]
        self.assertFalse(service.call({"action": "game.act", "observation_id": "foreign", "action_id": "world.quicksave"})["ok"])
        def native_fixture(*args, **kwargs):
            request = json.loads((self.run / "semantic.requests.jsonl").read_text().splitlines()[-1])
            receipt = {"event": "surface_receipt", "run_id": "fixture-run", "sequence": 3,
                "request_id": request["request_id"], "action_id": request["action_id"],
                "requested_run_id": "fixture-run", "requested_surface_id": request["surface_id"],
                "requested_frame_id": request["frame_id"], "consuming_surface_id": request["surface_id"],
                "consuming_frame_id": request["frame_id"], "accepted": True,
                "rejection_reason": "", "resulting_frame_id": "saved-world"}
            with self.trace.open("ab") as sink:
                sink.write(self.encode(receipt) + self.encode({**self.descriptor, "sequence": 4, "frame_id": "saved-world"}))
            return 1
        with patch.object(startup, "semantic_wake_pipe_contract", return_value={"status": "bound", "path": "fixture", "contract": {"transport": "disposable-native-fixture"}}), patch.object(startup, "write_semantic_wake_pipe", side_effect=native_fixture):
            saved = service.call({"action": "game.act", "observation_id": current, "action_id": "world.quicksave"})
        self.assertTrue(saved["ok"], saved)
        self.assertEqual(service.call({"action": "game.observe"})["result"]["observation_id"], "saved-world")
        self.assertFalse(service.call({"action": "game.act", "observation_id": current, "action_id": "world.quicksave"})["ok"])
        self.assertEqual(len((self.run / "semantic.requests.jsonl").read_text().splitlines()), 1)
        self.assertEqual(mirror.read_bytes(), malformed)

    def test_journal_replacement_during_dispatch_keeps_receipt_and_successor_once(self):
        self.replace_during_dispatch = True
        self.test_public_current_choice_dispatches_once_stale_and_disabled_refuse()

    def test_current_owner_survives_replacement_and_receipt_suffix_overflow(self):
        self.replace_during_dispatch = True
        self.redraw_after_receipt = True
        self.test_public_current_choice_dispatches_once_stale_and_disabled_refuse()

    def test_current_owner_keeps_its_slot_when_receipt_pins_shrink_the_suffix(self):
        self.replace_during_dispatch = True
        self.redraw_after_receipt = True
        # Current redraw + refusal + diagnostics just fit the initial deque.
        # Adding the older receipt/successor must not trim the redraw away.
        self.transport_count = startup.SEMANTIC_STEP_MAX_EVENTS - 2
        self.cold_after_receipt = True
        self.test_public_current_choice_dispatches_once_stale_and_disabled_refuse()

    def test_travel_only_reader_keeps_trip_facts_without_large_action_authority(self):
        current = {**self.descriptor, "payload": {"facts": "x" * 600000}}
        trip = {"event": "travel", "run_id": "fixture-run", "travel_id": "trip-1",
                "destination": [129, 149, 0], "avatar_omt": [129, 149, 0]}
        records = [{**trip, "receipt_id": "trip-1:" + state, "state": state,
                    "destination_present": state != "completed_cleared",
                    "destination_cleared": state == "completed_cleared"}
                   for state in ("active", "progress", "completed_cleared")]
        self.publish(self.encode(current) + b"".join(self.encode(event) for event in records))
        result = startup.evaluate_native_travel_completion_boundary(
            profile="fixture", run_dir=self.run, run_id="fixture-run", trace_start_offset=0,
            expected_destination=[129, 149, 0])
        self.assertEqual(result["status"], "green", result)
        self.assertEqual(result["event_count"], 3)
        self.assertLess(Path(result["source"]).stat().st_size, startup.SEMANTIC_STEP_MAX_BYTES)

    def test_completed_receipt_survives_partial_successor_poll_without_replaying(self):
        self.partial_delivery = True
        self.test_public_current_choice_dispatches_once_stale_and_disabled_refuse()

    def test_movement_child_choice_returns_before_coordinate_completion(self):
        self.transition_timeout = .05
        self.descriptor = {**self.descriptor, "kind": "world", "valid_actions": [
            {"id": "world.move.east", "stable_id": "", "label": "East", "enabled": True}]}
        self.trace.write_bytes(self.encode(self.descriptor))
        service = self.service()
        world = service.call({"action": "game.observe"})["result"]["observation_id"]
        prompt = {**self.descriptor, "sequence": 3, "surface_id": "fire-prompt",
            "frame_id": "fire-choice", "kind": "prompt", "payload": {
                "title": "YESNO", "text": "Really step into small fire? (Case Sensitive)"},
            "valid_actions": [{"id": "prompt.choose", "stable_id": "prompt-option:21",
                               "label": "NO", "enabled": True}]}
        after = {**self.descriptor, "sequence": 5, "surface_id": "returned-world",
                 "frame_id": "after-no"}

        def native_fixture(*args, **kwargs):
            request = json.loads((self.run / "semantic.requests.jsonl").read_text().splitlines()[-1])
            successor = prompt if request["action_id"] == "world.move.east" else after
            receipt = {"event": "surface_receipt", "run_id": "fixture-run",
                "sequence": successor["sequence"] + 1, "request_id": request["request_id"],
                "action_id": request["action_id"], "requested_run_id": "fixture-run",
                "requested_surface_id": request["surface_id"], "requested_frame_id": request["frame_id"],
                "consuming_surface_id": request["surface_id"], "consuming_frame_id": request["frame_id"],
                "accepted": True, "rejection_reason": "", "resulting_frame_id": successor["frame_id"]}
            with self.trace.open("ab") as stream:
                # Real native manager ordering: publish child, then receipt.
                stream.write(self.encode(successor) + self.encode(receipt))
            return 1

        with patch.object(startup, "semantic_wake_pipe_contract", return_value={
                "status": "bound", "path": "fixture", "contract": {"transport": "fixture"}}), \
                patch.object(startup, "write_semantic_wake_pipe", side_effect=native_fixture):
            opened = service.call({"action": "game.act", "observation_id": world,
                                   "action_id": "world.move.east"})
            self.assertTrue(opened["ok"], opened)
            self.assertEqual(opened["observation"]["observation_id"], "fire-choice")
            self.assertEqual(json.loads((self.run / "semantic.steps.jsonl").read_text().splitlines()[-1])["reason"],
                             "native_surface_child_accepted")
            self.assertNotIn("coordinate_space", opened["receipt"]["native_receipt"])
            self.assertFalse(service.call({"action": "game.act", "observation_id": world,
                                          "action_id": "world.move.east"})["ok"])
            closed = service.call({"action": "game.act", "observation_id": "fire-choice",
                                   "action_id": "prompt.choose", "stable_id": "prompt-option:21"})
            self.assertTrue(closed["ok"], closed)
            self.assertEqual(closed["observation"]["observation_id"], "after-no")
        requests = [json.loads(line) for line in (self.run / "semantic.requests.jsonl").read_text().splitlines()]
        self.assertEqual([row["action_id"] for row in requests], ["world.move.east", "prompt.choose"])
        self.assertEqual(service.call({"action": "game.observe"})["result"]["observation_id"], "after-no")

    def test_movement_completion_and_child_identity_refusals(self):
        self.transition_timeout = .03
        world = {**self.descriptor, "kind": "world", "valid_actions": [
            {"id": "world.move.east", "stable_id": "", "label": "East", "enabled": True}]}
        for mode in ("completed_world", "unproved_world", "unnamed_child", "same_owner_child", "actionless_child"):
            with self.subTest(mode=mode):
                self.descriptor = world
                self.trace.write_bytes(self.encode(world))
                request_path = self.run / "semantic.requests.jsonl"
                request_path.unlink(missing_ok=True)
                service = self.service()
                current = service.call({"action": "game.observe"})["result"]["observation_id"]
                successor = {**world, "sequence": 3, "frame_id": "next-frame", "surface_id": "child-owner"}
                if mode.endswith("child"):
                    successor.update(kind="prompt", valid_actions=[
                        {"id": "prompt.choose", "stable_id": "NO", "label": "NO", "enabled": True}])
                if mode == "same_owner_child":
                    successor["surface_id"] = world["surface_id"]
                if mode == "actionless_child":
                    successor["valid_actions"] = []

                def native_fixture(*args, **kwargs):
                    request = json.loads(request_path.read_text().splitlines()[-1])
                    receipt = {"event": "surface_receipt", "run_id": "fixture-run", "sequence": 4,
                        "request_id": request["request_id"], "action_id": request["action_id"],
                        "requested_run_id": "fixture-run", "requested_surface_id": request["surface_id"],
                        "requested_frame_id": request["frame_id"], "consuming_surface_id": request["surface_id"],
                        "consuming_frame_id": request["frame_id"], "accepted": True, "rejection_reason": "",
                        "resulting_frame_id": "another-child" if mode == "unnamed_child" else "next-frame"}
                    with self.trace.open("ab") as stream:
                        stream.write(self.encode(successor) + self.encode(receipt))
                        if mode == "completed_world":
                            stream.write(self.encode({"event": "receipt", "run_id": "fixture-run",
                                "action_id": "world.move.east", "frame_id": "fixture-run:100:1",
                                "accepted": True, "coordinate_space": "absolute_ms", "outcome": "moved",
                                "before_absolute_ms": [10, 20, 0], "expected_absolute_ms": [11, 20, 0],
                                "after_absolute_ms": [11, 20, 0]}))
                    return 1

                with patch.object(startup, "semantic_wake_pipe_contract", return_value={
                        "status": "bound", "path": "fixture", "contract": {"transport": "fixture"}}), \
                        patch.object(startup, "write_semantic_wake_pipe", side_effect=native_fixture):
                    result = service.call({"action": "game.act", "observation_id": current,
                                           "action_id": "world.move.east"})
                self.assertEqual(result["ok"], mode == "completed_world", result)
                if mode == "completed_world":
                    self.assertEqual(result["receipt"]["native_receipt"]["after_absolute_ms"], [11, 20, 0])
                else:
                    self.assertEqual(result["error"], "native_surface_successor_timeout", result)
                self.assertEqual(len(request_path.read_text().splitlines()), 1)

    def test_following_valid_record_after_oversized_nonsemantic_line_is_current(self):
        self.publish(b"unrelated native output " + b"x" * 5000000 + b"\n" + self.encode(self.descriptor))
        self.assertEqual(self.frame()["frame_id"], self.descriptor["frame_id"])
        self.assertEqual(startup.latest_semantic_source_descriptor(self.trace)["frame_id"],
                         self.descriptor["frame_id"])



if __name__ == "__main__":
    unittest.main()
