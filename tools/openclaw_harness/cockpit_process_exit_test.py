"""Bound process death must replace cached native input authority."""
import sys
import json
import os
import subprocess
import startup_harness
import tempfile
import sqlite3
from contextlib import closing
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cockpit import CockpitRunChannel, CockpitService
from cockpit_archive import Archive, ArchiveSequence
from cockpit_file_bridge import _request_identity


CHARTER = {"claim": "Inspect native interface usability", "material_proof": "Observed native facts",
           "current_uncertainty": "Whether the game remains actionable", "requested_evidence_ceiling": "zero-credit"}


def world():
    return {"schema_version": 1, "event": "surface_descriptor", "run_id": "run-a",
            "frame_id": "run-a:world:1", "surface_id": "world-1", "kind": "world",
            "breadcrumbs": ["world"], "payload": {"last_save_result": "unattempted"},
            "valid_actions": [{"id": action, "stable_id": "", "label": action, "enabled": True}
                              for action in ("world.move.north", "world.save_quit")]}


class ProcessExitTest(unittest.TestCase):
    def channel(self, *, alive=True, exit_code=None, dispatch=None):
        process = {"run_id": "run-a", "pid": 123, "alive": alive, "exit_code": exit_code,
                   "exit_observed_at": "2026-09-05T12:00:00Z", "evidence_ref": "game-process-exit.json"}
        read = Mock(side_effect=world)
        dispatch = dispatch or Mock()
        channel = CockpitRunChannel(read, dispatch, read_process_state=lambda: process,
                                    binding_id="bound-a", witness_charter=CHARTER,
                                    witness_identity={"scenario_id": "ergonomics", "source_identity": "src",
                                                      "executable_identity": "exe"},
                                    witness_evidence_ceiling="zero-credit")
        return channel, process, read, dispatch

    def test_abort_stays_live_until_actual_exit_then_allows_normal_reporting(self):
        channel, process, _, dispatch = self.channel()
        service = CockpitService(run_channel=channel)
        old = service.call({"action": "game.observe"})["result"]
        cleanup = Mock(return_value={"status": "graceful_quit_unconfirmed_process_retained",
                                     "signal": "SIGTERM", "native_exit_credit": False})
        channel._abort_owned_process = cleanup
        pending = service.call({"action": "run.quit", "abort": True})
        self.assertTrue(pending["ok"])
        self.assertEqual(pending["result"]["state"], "exit_observation_pending")
        self.assertEqual(channel.status()["state"], "active")
        self.assertEqual(service.call({"action": "game.act", "observation_id": old["observation_id"],
                                       "action_id": "world.move.north"})["error"],
                         "abort_exit_observation_pending")
        dispatch.assert_not_called()
        self.assertTrue(service.call({"action": "run.quit", "abort": True})["ok"])
        cleanup.assert_called_once()
        self.assertEqual(service.call({"action": "run.witness", "observation_id": old["observation_id"],
                                       "stop_reason": "done", "unused_authority": "released"})["error"],
                         "native_exit_required_before_reporting")
        process.update(alive=False, exit_code=None)
        ended = service.call({"action": "game.observe"})["result"]
        self.assertEqual(ended["surface"]["kind"], "process_exited")
        self.assertFalse(ended["surface"]["facts"]["gameplay_credit"])
        finished = service.call({"action": "run.finish", "observation_id": ended["observation_id"],
                                 "stop_reason": "done", "unused_authority": "released"})
        self.assertTrue(finished["ok"], finished)
        self.assertEqual(finished["result"]["closure_kind"], "explicit_abort")
        self.assertFalse(finished["result"]["native_exit_credit"])

    def test_started_but_unobserved_abort_is_not_reissued(self):
        channel, _, _, _ = self.channel()
        service = CockpitService(run_channel=channel)
        self.assertTrue(service.call({"action": "game.observe"})["ok"])
        cleanup = Mock(return_value={"status": "explicit_abort_exit_unobserved_process_retained",
                                     "native_exit_credit": False})
        channel._abort_owned_process = cleanup
        for _ in range(2):
            pending = service.call({"action": "run.quit", "abort": True})
            self.assertTrue(pending["ok"])
            self.assertFalse(pending["result"]["retry_allowed"])
        cleanup.assert_called_once()

    def test_refused_abort_then_external_exit_does_not_claim_abort_termination(self):
        channel, process, _, _ = self.channel()
        service = CockpitService(run_channel=channel)
        self.assertTrue(service.call({"action": "game.observe"})["ok"])
        channel._abort_owned_process = Mock(return_value={"status": "retained_explicit_abort_refused",
                                                         "native_exit_credit": False})
        refused = service.call({"action": "run.quit", "abort": True})
        self.assertTrue(refused["result"]["retry_allowed"])
        process.update(alive=False, exit_code=None)
        ended = service.call({"action": "game.observe"})["result"]
        finished = service.call({"action": "run.finish", "observation_id": ended["observation_id"],
                                 "stop_reason": "external exit", "unused_authority": "released"})
        self.assertTrue(finished["ok"], finished)
        self.assertNotEqual(finished["result"].get("closure_kind"), "explicit_abort")
        self.assertTrue(finished["result"]["stop_detail"]["abort_requested"])
        self.assertNotIn("explicit_player_abort", finished["result"]["stop_detail"])

    def test_live_seal_and_finish_refuse_without_mutation_and_save_quit_still_works(self):
        channel, process, _, dispatch = self.channel()
        observed = channel.observe()
        before = list(channel._transcript)
        finalizer = Mock()
        channel._finalize_session = finalizer
        args = dict(observation_id=observed["observation_id"], stop_reason="done", unused_authority="released")
        self.assertEqual(channel.seal_witness_journal(**args)["error"], "native_exit_required_before_reporting")
        self.assertEqual(channel.finish(**args)["error"], "native_exit_required_before_reporting")
        self.assertEqual(list(channel._transcript), before)
        self.assertEqual(channel._state, "active")
        self.assertIsNone(channel._sealed_journal)
        finalizer.assert_not_called()
        def save(frame, action):
            self.assertEqual(action, "world.save_quit")
            process.update(alive=False, exit_code=0)
            return {"native_receipt": {"run_id": "run-a", "requested_run_id": "run-a",
                    "requested_frame_id": frame["frame_id"], "action_id": action, "accepted": True,
                    "requested_surface_id": "world-1", "consuming_surface_id": "world-1",
                    "consuming_frame_id": frame["frame_id"]}}
        channel._dispatch_advertised_action = save
        result = channel.act(observation_id=observed["observation_id"], action_id="world.save_quit")
        self.assertTrue(result["ok"])
        self.assertEqual(result["observation"]["surface"]["kind"], "process_exited")

    def test_wrong_identity_or_unknown_liveness_cannot_seal_or_finish(self):
        for change in ({"run_id": "other"}, {"pid": 0}, {"alive": None},
                       {"identity_status": "identity_changed"}, {"identity_status": "identity_unavailable"}):
            with self.subTest(change=change):
                channel, process, _, _ = self.channel()
                observed = channel.observe()
                process.update(alive=False)
                process.update(change)
                args = dict(observation_id=observed["observation_id"], stop_reason="done", unused_authority="none")
                self.assertFalse(channel.seal_witness_journal(**args)["ok"])
                self.assertFalse(channel.finish(**args)["ok"])
                self.assertEqual(channel._state, "active")

    def test_crash_can_seal_but_live_replacement_blocks_reporting(self):
        channel, process, _, _ = self.channel(alive=False, exit_code=7)
        observed = channel.observe()
        args = dict(observation_id=observed["observation_id"], stop_reason="crashed", unused_authority="released")
        self.assertTrue(channel.seal_witness_journal(**args)["ok"])
        retained = channel._sealed_journal
        process.update(alive=True)
        finalizer = Mock()
        channel._finalize_session = finalizer
        self.assertFalse(channel.finish(**args, witness={})["ok"])
        self.assertIs(channel._sealed_journal, retained)
        finalizer.assert_not_called()

    def test_dead_process_replaces_cached_world_without_native_frame_or_actions(self):
        channel, process, read, dispatch = self.channel()
        old = channel.observe()
        process["alive"] = False
        ended = channel.observe()
        self.assertEqual(read.call_count, 1)
        self.assertEqual(ended["surface"]["kind"], "process_exited")
        self.assertEqual(ended["advertised_actions"], [])
        self.assertEqual(ended["surface"]["actions"], [])
        self.assertNotIn("frame_id", ended)
        self.assertEqual(ended["surface"]["facts"]["exit_code"], None)
        self.assertFalse(ended["surface"]["facts"]["gameplay_credit"])
        self.assertEqual(ended["last_native_observation_id"], old["observation_id"])
        result = channel.act(observation_id=old["observation_id"], action_id="world.move.north")
        self.assertFalse(result["ok"])
        self.assertEqual(result["observation"], ended)
        dispatch.assert_not_called()
        self.assertEqual(channel.status()["state"], "active")
        # Re-observation does not duplicate terminal facts or revive authority.
        count = len(channel._transcript)
        self.assertEqual(channel.observe(), ended)
        self.assertEqual(len(channel._transcript), count)

    def test_exit_during_native_action_retains_receipt_and_distinguishes_exit_codes(self):
        for exit_code in (0, 7, None):
            with self.subTest(exit_code=exit_code):
                channel, process, read, _ = self.channel()
                def dispatch(frame, action):
                    process.update(alive=False, exit_code=exit_code)
                    return {"native_receipt": {"run_id": "run-a", "requested_run_id": "run-a", "requested_frame_id": frame["frame_id"],
                            "action_id": action, "accepted": True,
                            "requested_surface_id": "world-1", "consuming_surface_id": "world-1",
                            "consuming_frame_id": frame["frame_id"]}}
                channel._dispatch_advertised_action = dispatch
                observed = channel.observe()
                result = channel.act(observation_id=observed["observation_id"], action_id="world.save_quit")
                self.assertEqual(result["ok"], exit_code == 0)
                self.assertTrue(result["receipt"]["native_receipt"]["accepted"])
                self.assertEqual(result["observation"]["surface"]["facts"]["exit_code"], exit_code)
                self.assertFalse(result["observation"]["surface"]["facts"]["gameplay_credit"])
                self.assertEqual(read.call_count, 1)
                self.assertEqual(channel.status()["state"], "active")

    def test_exit_facts_remain_citable_through_journal_and_finish(self):
        channel, process, _, _ = self.channel(alive=False, exit_code=0)
        service = CockpitService(run_channel=channel)
        ended = service.call({"action": "game.observe"})["result"]
        blocked = service.call({"action": "game.wait", "wait": {}})
        self.assertEqual(blocked["error"], "game_process_exited")
        sealed = service.call({"action": "run.witness", "observation_id": ended["observation_id"],
                               "stop_reason": "process_ended", "unused_authority": "released"})
        self.assertTrue(sealed["ok"])
        journal = sealed["result"]["evidence_journal"]
        native = journal["entries"][1]["value"]["value"]
        self.assertEqual(native["surface"]["facts"]["pid"], 123)
        witness = {"verdict": "inconclusive", "smallest_supported_claim": "The bound process exited with code zero.",
                   "causal_account": "The process receipt does not establish saving or feature correctness.",
                   "citations": [{"citation_id": "J0002", "meaning": "Bound process exit",
                                  "checks": {"value.surface.facts.pid": 123, "value.surface.facts.exit_code": 0,
                                             "value.surface.facts.evidence_ref": "game-process-exit.json"}}],
                   "recommended_disposition": "continue", "evidence_ceiling": "zero-credit"}
        finished = service.call({"action": "run.finish", "observation_id": ended["observation_id"],
                                 "stop_reason": "process_ended", "unused_authority": "released", "witness": witness})
        self.assertTrue(finished["ok"])
        self.assertEqual(channel.status()["state"], "finished")

    def test_late_exit_status_receipt_refines_unknown_without_rewriting_prior_observation(self):
        channel, process, _, _ = self.channel(alive=False)
        unknown = channel.observe()
        process.update(exit_code=0, evidence_ref="confirmed-exit.json")
        confirmed = channel.observe()
        self.assertIsNone(unknown["surface"]["facts"]["exit_code"])
        self.assertEqual(confirmed["surface"]["facts"]["exit_code"], 0)
        self.assertNotEqual(unknown["observation_id"], confirmed["observation_id"])
        self.assertEqual(confirmed["surface"]["facts"]["evidence_ref"], "confirmed-exit.json")
        self.assertTrue(channel._observations[unknown["observation_id"]]["used"])
        self.assertEqual(confirmed["advertised_actions"], [])

    def test_revoked_cached_exit_can_be_freshly_observed_without_rearming(self):
        with tempfile.TemporaryDirectory() as root, \
                closing(Archive(Path(root) / "evidence.sqlite", run_id="run-a", binding_id="bound-a")) as archive:
            process = {"run_id": "run-a", "pid": 123, "alive": False, "exit_code": 0,
                       "identity_status": "matched", "evidence_ref": "game-process-exit.json"}
            channel = CockpitRunChannel(world, Mock(), archive=archive, binding_id="bound-a",
                                        read_process_state=lambda: process, witness_charter=CHARTER,
                                        witness_identity={"scenario_id": "test", "source_identity": "src",
                                                          "executable_identity": "exe"})
            service = CockpitService(run_channel=channel)
            original = service.call({"action": "game.observe"})["result"]
            old = original["observation_id"]
            # Inject a journal construction error at the actual service caller.
            # This reproduces the revoked-cache state; it does not assert the
            # cause of any historical database lock.
            with patch("cockpit.build_evidence_journal", side_effect=sqlite3.OperationalError("database is locked")):
                failed = service.call({"action": "run.witness", "observation_id": old,
                                       "stop_reason": "done", "unused_authority": "released"})
            self.assertEqual(failed["error"], "database is locked")
            stale = service.call({"action": "run.witness", "observation_id": old,
                                  "stop_reason": "done", "unused_authority": "released"})
            self.assertEqual(stale["error"], "unknown_or_stale_observation")
            fresh = service.call({"action": "game.observe"})["result"]
            self.assertNotEqual(fresh["observation_id"], old)
            self.assertTrue(channel._observations[old]["used"])
            self.assertFalse(channel._observations[fresh["observation_id"]]["used"])
            self.assertEqual(fresh["surface"]["facts"]["pid"], 123)
            self.assertEqual(fresh["surface"]["facts"]["save_outcome"], "not_established_by_process_exit")
            self.assertEqual(fresh["advertised_actions"], [])
            # A second interruption does not reuse either earlier identity.
            channel._fail_operation("second_interruption", {})
            newer = service.call({"action": "game.observe"})["result"]
            self.assertNotIn(newer["observation_id"], {old, fresh["observation_id"]})
            self.assertTrue(channel._observations[fresh["observation_id"]]["used"])
            fresh = newer
            sealed = service.call({"action": "run.witness", "observation_id": fresh["observation_id"],
                                   "stop_reason": "done", "unused_authority": "released"})
            self.assertTrue(sealed["ok"])
            self.assertTrue(channel._observations[old]["used"])
            finished = service.call({"action": "run.finish", "observation_id": fresh["observation_id"],
                                     "stop_reason": "done", "unused_authority": "released",
                                     "outcome": {"summary": "process exited", "verdict": "inconclusive"}})
            self.assertTrue(finished["ok"])
            self.assertEqual(channel._state, "finished")
            self.assertTrue(channel._observations[old]["used"])

    def test_failed_finish_reobserves_exit_without_replaying_old_authority(self):
        channel, _, _, _ = self.channel(alive=False, exit_code=0)
        service = CockpitService(run_channel=channel)
        old = service.call({"action": "game.observe"})["result"]["observation_id"]
        request = {"action": "run.finish", "observation_id": old, "stop_reason": "done",
                   "unused_authority": "released",
                   "outcome": {"summary": "exit observed", "verdict": "inconclusive"}}
        with patch.object(channel, "_stop", side_effect=sqlite3.OperationalError("database is locked")):
            self.assertEqual(service.call(request)["error"], "database is locked")
        self.assertTrue(channel._observations[old]["used"])
        self.assertEqual(service.call(request)["error"], "unknown_or_stale_observation")
        fresh = service.call({"action": "game.observe"})["result"]["observation_id"]
        self.assertNotEqual(fresh, old)
        request["observation_id"] = fresh
        self.assertTrue(service.call(request)["ok"])
        self.assertTrue(channel._observations[old]["used"])

    def test_revoked_exit_reobservation_rejects_changed_or_missing_birth(self):
        for replacement in ("replacement", None):
            with self.subTest(replacement=replacement):
                channel, process, _, _ = self.channel(alive=False, exit_code=0)
                process["process_generation"] = {"birth_identity": "first"}
                original = channel.observe()
                channel._fail_operation("interrupted_reporting", {})
                process["process_generation"] = {"birth_identity": replacement}
                response = CockpitService(run_channel=channel).call({"action": "game.observe"})
                self.assertFalse(response["ok"])
                self.assertEqual(response["error"], "process_exit_birth_identity_mismatch")
                self.assertTrue(channel._observations[original["observation_id"]]["used"])

    def test_revoked_exit_reobservation_rejects_changed_process_identity(self):
        for change in ({"identity_status": "identity_changed"}, {"identity_status": "identity_unavailable"},
                       {"pid": 124}, {"run_id": "foreign"}, {"alive": True}):
            with self.subTest(change=change):
                channel, process, _, _ = self.channel(alive=False, exit_code=0)
                original = channel.observe()
                channel._fail_operation("interrupted_reporting", {})
                process.update(change)
                result = CockpitService(run_channel=channel).call({"action": "game.observe"})
                if change == {"alive": True}:
                    self.assertEqual(result["result"]["observation_id"], original["observation_id"])
                else:
                    self.assertFalse(result["ok"])
                self.assertTrue(channel._observations[original["observation_id"]]["used"])

    def test_wrong_run_process_cannot_replace_current_native_state(self):
        channel, process, _, _ = self.channel()
        channel.observe()
        process.update(alive=False, run_id="other-run")
        with self.assertRaisesRegex(ValueError, "process_exit_run_identity_mismatch"):
            channel.observe()


class JournalLockPublicationTest(unittest.TestCase):
 def test_locked_exit_fail_closed_then_fresh_observation_single_finish(self):
  with tempfile.TemporaryDirectory() as t, closing(Archive(Path(t)/'evidence.sqlite',run_id='run-a',binding_id='bound-a')) as a:
   a.connection.execute('PRAGMA busy_timeout=20')
   process={'run_id':'run-a','pid':123,'alive':True,'exit_code':None,'identity_status':'matched','evidence_ref':'game-process-exit.json'}
   dispatch=Mock(); finalize=Mock(return_value={'cleanup':{'status':'already_exited','native_exit_credit':False}})
   c=CockpitRunChannel(world,dispatch,archive=a,binding_id='bound-a',read_process_state=lambda:process,finalize_session=finalize,witness_charter=CHARTER,witness_identity={'scenario_id':'test','source_identity':'src','executable_identity':'exe'})
   service=CockpitService(run_channel=c)
   old_world=service.call({'action':'game.observe'})['result']['observation_id']
   self.assertFalse(c._observations[old_world]['used'])
   process.update(alive=False,exit_code=0)
   old=service.call({'action':'game.observe'})['result']['observation_id']
   self.assertTrue(c._observations[old_world]['used'])
   finish={'action':'run.finish','observation_id':old,'stop_reason':'done','unused_authority':'released','outcome':{'summary':'exit observed','verdict':'inconclusive'}}
   with closing(sqlite3.connect(Path(t)/'evidence.sqlite',timeout=.02)) as lock:
    lock.execute('BEGIN IMMEDIATE')
    failure=service.call({'action':'run.witness','observation_id':old,'stop_reason':'done','unused_authority':'released'})
    self.assertEqual(failure['error'],'reporting_revocation_pending')
    self.assertEqual(failure['unused_authority'],'revocation_pending')
    self.assertEqual(failure['journal_state'],'unconfirmed')
    for action in (finish,{'action':'game.observe'},{'action':'game.act','observation_id':old_world,'action_id':'world.move.north'}):
     self.assertEqual(service.call(action)['error'],'reporting_revocation_pending')
    replacement_service=CockpitService(run_channel=c)
    self.assertEqual(replacement_service.call(finish)['error'],'reporting_revocation_pending')
    self.assertIsNone(c._sealed_journal);dispatch.assert_not_called();finalize.assert_not_called()
    lock.rollback()
   self.assertEqual(service.call({'action':'game.act','observation_id':old_world,'action_id':'world.move.north'})['error'],'fresh_observation_required')
   self.assertTrue(c._observations[old_world]['used']);dispatch.assert_not_called()
   self.assertEqual(service.call(finish)['error'],'unknown_or_stale_observation')
   fresh=service.call({'action':'game.observe'})['result']
   self.assertNotEqual(old,fresh['observation_id']);self.assertEqual(fresh['surface']['facts']['exit_code'],0)
   self.assertEqual(fresh['surface']['facts']['save_outcome'],'not_established_by_process_exit')
   finish['observation_id']=fresh['observation_id'];result=service.call(finish)
   self.assertTrue(result['ok'],result);self.assertEqual(result['result']['cleanup']['status'],'already_exited')
   self.assertFalse(result['result']['declared_reentry_ready']);finalize.assert_called_once();dispatch.assert_not_called()
   self.assertFalse(service.call(finish)['ok']);finalize.assert_called_once()


class RestartExitPublicationTest(unittest.TestCase):
 def test_real_archive_restart_old_request_rejected_fresh_finish_once(self):
  with tempfile.TemporaryDirectory() as t:
   path=Path(t)/'evidence.sqlite'
   process={'run_id':'run-a','pid':123,'alive':False,'exit_code':0,'identity_status':'matched','evidence_ref':'game-process-exit.json'}
   dispatch=Mock();finalize=Mock(return_value={'cleanup':{'status':'already_exited','native_exit_credit':False}})
   def channel(archive):
    return CockpitRunChannel(world,dispatch,archive=archive,binding_id='bound-a',read_process_state=lambda:process,finalize_session=finalize,witness_charter=CHARTER,witness_identity={'scenario_id':'test','source_identity':'src','executable_identity':'exe'})
   with closing(Archive(path,run_id='run-a',binding_id='bound-a')) as archive:
    archive.connection.execute('PRAGMA busy_timeout=20');first=channel(archive);first_service=CockpitService(run_channel=first)
    first._transcript.append({'kind':'probe','marker':'predecessor-transcript-sentinel'})
    stream_id=first._transcript.stream
    old=first_service.call({'action':'game.observe'})['result']['observation_id']
    with closing(sqlite3.connect(path,timeout=.02)) as lock:
     lock.execute('BEGIN IMMEDIATE')
     request={'action':'run.witness','observation_id':old,'stop_reason':'pending-reason-sentinel','unused_authority':'released'}
     self.assertEqual(first_service.call(request)['error'],'reporting_revocation_pending')
     self.assertIsNone(first._sealed_journal)
     with self.assertRaisesRegex(sqlite3.OperationalError,'database is locked'):
      channel(archive)
     lock.rollback()
   with closing(Archive(path,run_id='run-a',binding_id='bound-a')) as archive:
    new=channel(archive);service=CockpitService(run_channel=new)
    fresh=service.call({'action':'game.observe'})['result']
    new_id=fresh['observation_id']
    self.assertNotEqual(new_id,old)
    self.assertEqual(service.call({'action':'game.observe'})['result']['observation_id'],new_id)
    self.assertEqual(fresh['surface']['facts']['exit_code'],0)
    self.assertEqual(fresh['surface']['facts']['save_outcome'],'not_established_by_process_exit')
    self.assertNotEqual(new._transcript.stream,stream_id)
    prior=list(ArchiveSequence(archive,stream_id))
    self.assertTrue(any(x.get('marker')=='predecessor-transcript-sentinel' for x in prior))
    self.assertFalse(any(x.get('marker')=='predecessor-transcript-sentinel' for x in new._transcript))
    durable_values=[row[0] for row in archive.connection.execute('SELECT value FROM records')]
    durable_values += [row[0] for row in archive.connection.execute('SELECT value FROM objects')]
    durable_values += [row[0] for row in archive.connection.execute('SELECT value FROM metadata')]
    self.assertFalse(any('pending-reason-sentinel' in value for value in durable_values))
    request={'action':'run.finish','observation_id':old,'stop_reason':'done','unused_authority':'released','outcome':{'summary':'exit observed','verdict':'inconclusive'}}
    self.assertEqual(_request_identity(json.dumps(request).encode())['observation_id'],old)
    self.assertEqual(service.call(request)['error'],'unknown_or_stale_observation')
    finalize.assert_not_called();dispatch.assert_not_called()
    sealed=service.call({'action':'run.witness','observation_id':new_id,'stop_reason':'done','unused_authority':'released'})
    self.assertTrue(sealed['ok'],sealed)
    journal=list(sealed['result']['evidence_journal']['entries'])
    self.assertFalse(any('predecessor-transcript-sentinel' in json.dumps(entry) for entry in journal))
    request['observation_id']=new_id
    result=service.call(request)
    self.assertTrue(result['ok'],result)
    self.assertFalse(result['result']['declared_reentry_ready'])
    self.assertEqual(result['result']['cleanup']['status'],'already_exited')
    finalize.assert_called_once();dispatch.assert_not_called()
    self.assertFalse(service.call(request)['ok']);finalize.assert_called_once()


class ActualExitCleanupPublicationTest(unittest.TestCase):
    def test_lock_retry_finalizes_actual_exited_owned_child_once(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            with tempfile.TemporaryDirectory() as root:
                directory = Path(root)
                generation = startup_harness.process_generation_snapshot(child.pid)
                self.assertTrue(generation.get("alive"))
                (directory / "process.json").write_text(json.dumps({"pid": child.pid, "process_generation": generation}))
                child.terminate()
                child.wait(timeout=5)
                self.assertIsNotNone(child.returncode)
                owned = startup_harness.current_owned_process_generation(directory)
                self.assertEqual(owned["status"], "exited")
                process = {"run_id": "run-a", "pid": child.pid, "alive": False,
                           "exit_code": child.returncode, "identity_status": "matched",
                           "process_generation": generation}
                def finalize(report):
                    with patch.dict(os.environ, {"OPENCLAW_COCKPIT_BRIDGE_SESSION_DIR": str(directory)}):
                        return startup_harness.finalize_cockpit_live_session(directory, child.pid, report)
                final = Mock(wraps=finalize)
                with closing(Archive(directory / "evidence.sqlite", run_id="run-a", binding_id="bound-a")) as archive:
                    archive.connection.execute("PRAGMA busy_timeout=20")
                    dispatch = Mock()
                    channel = CockpitRunChannel(world, dispatch, archive=archive, binding_id="bound-a",
                        read_process_state=lambda: process, finalize_session=final, witness_charter=CHARTER,
                        witness_identity={"scenario_id": "test", "source_identity": "src", "executable_identity": "exe"})
                    service = CockpitService(run_channel=channel)
                    old = service.call({"action": "game.observe"})["result"]["observation_id"]
                    with closing(sqlite3.connect(directory / "evidence.sqlite", timeout=.02)) as lock:
                        lock.execute("BEGIN IMMEDIATE")
                        failed = service.call({"action": "run.witness", "observation_id": old,
                                               "stop_reason": "done", "unused_authority": "released"})
                        self.assertEqual(failed["error"], "reporting_revocation_pending")
                        final.assert_not_called()
                        lock.rollback()
                    fresh = service.call({"action": "game.observe"})["result"]["observation_id"]
                    self.assertNotEqual(old, fresh)
                    request = {"action": "run.finish", "observation_id": old, "stop_reason": "done",
                               "unused_authority": "released", "outcome": {"summary": "exit observed", "verdict": "inconclusive"}}
                    self.assertEqual(service.call(request)["error"], "unknown_or_stale_observation")
                    request["observation_id"] = fresh
                    finished = service.call(request)
                    self.assertTrue(finished["ok"], finished)
                    self.assertEqual(finished["result"]["cleanup"]["status"], "already_exited")
                    self.assertFalse(finished["result"]["cleanup"]["native_exit_credit"])
                    self.assertFalse(service.call(request)["ok"])
                    final.assert_called_once()
                    dispatch.assert_not_called()
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
