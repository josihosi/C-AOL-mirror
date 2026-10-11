"""Portable receipt/owner regression controls for ordinary native wait stopping.

Disposable native-shaped rows; connected actual factory/FIFO/report control is
retained with the chaperone evidence. These controls assert no native gameplay.
"""
from copy import deepcopy
import unittest
from cockpit import CockpitRunChannel, CockpitService

ABSENT_CLOCK = object()

class NativeStopTest(unittest.TestCase):
    def rows(self):
        operation = {'run_id':'run', 'state':'awaiting_decision',
                     'accepted_receipt':{'sequence':10}, 'start_game_minutes':100,
                     'requested_duration_game_minutes':120}
        prompt = {'kind':'prompt','run_id':'run','process_instance':'native',
                  'frame_id':'prompt','surface_id':'owner','sequence':20,
                  'payload':{'title':'CANCEL_ACTIVITY_OR_IGNORE_QUERY'},
                  'valid_actions':[{'id':'prompt.choose','stable_id':'fresh-yes',
                                    'label':'YES','enabled':True}]}
        request = {'action':'game.act','action_id':'prompt.choose',
                   'stable_id':'fresh-yes','observation_id':'prompt'}
        receipt = {'accepted':True,'action_id':'prompt.choose','run_id':'run',
                   'requested_run_id':'run','process_instance':'native','sequence':22,
                   'requested_surface_id':'owner','consuming_surface_id':'owner',
                   'requested_frame_id':'prompt','consuming_frame_id':'prompt',
                   'resulting_frame_id':'world','game_minutes':113,'game_turn':6780}
        world = {'kind':'world','run_id':'run','process_instance':'native',
                 'frame_id':'world','sequence':21,'game_minutes':113,'game_turn':6780}
        return operation,prompt,request,receipt,world

    def test_partial_stop_is_cancelled_not_duration_completion(self):
        operation,prompt,request,receipt,world=self.rows()
        cancelled=CockpitRunChannel._stopped_wait_operation(operation,prompt,request,receipt,world)
        self.assertEqual(cancelled['state'],'cancelled')
        self.assertEqual(cancelled['completed_progress_game_minutes'],13)
        self.assertEqual(cancelled['requested_duration_game_minutes'],120)
        self.assertEqual(cancelled['accepted_receipt'],operation['accepted_receipt'])
        self.assertFalse(cancelled['termination']['duration_fulfilled'])
        self.assertNotIn('termination',operation)

    def test_foreign_receipt_or_nonstop_option_cannot_settle_wait(self):
        for field,value in [('run_id','foreign'),('process_instance','foreign'),
                            ('accepted',False),('consuming_frame_id','old'),
                            ('resulting_frame_id','wrong')]:
            with self.subTest(field=field):
                o,p,q,r,w=self.rows();r[field]=value
                with self.assertRaisesRegex(ValueError,'owner_or_receipt_unproved'):
                    CockpitRunChannel._stopped_wait_operation(o,p,q,r,w)
        for label in ('NO','IGNORE','MANAGER'):
            with self.subTest(label=label):
                o,p,q,r,w=self.rows();p['valid_actions'][0]['label']=label
                with self.assertRaises(ValueError):CockpitRunChannel._stopped_wait_operation(o,p,q,r,w)

    def test_completed_or_non_scalar_wait_does_not_own_later_stop(self):
        for duration in (None,5,13):
            with self.subTest(duration=duration):
                o,p,q,r,w=self.rows();o['requested_duration_game_minutes']=duration
                self.assertIsNone(CockpitRunChannel._stopped_wait_operation(o,p,q,r,w))

    def accepted_legacy_duration(self, sequence=ABSENT_CLOCK):
        _, prompt, _, stop, world = self.rows()
        duration = {"event":"surface_descriptor", "schema_version":1,
                    "run_id":"run", "surface_id":"duration", "frame_id":"duration-frame",
                    "process_instance":"native", "sequence":10, "game_minutes":100,
                    "game_turn":6000, "kind":"menu", "breadcrumbs":["World","Wait"],
                    "payload":{}, "valid_actions":[{"id":"wait.2h", "stable_id":"",
                    "label":"2 hours", "enabled":True}]}
        waiting = {"run_id":"run", "frame_id":"run:6000:141", "state":"wait_activity",
                   "valid_actions":[], "game_minutes":100}
        world["surface_id"]="world-owner"
        for frame in (prompt, world):
            frame.update(event="surface_descriptor", schema_version=1, breadcrumbs=["World"],
                         payload=frame.get("payload",{}), valid_actions=frame.get("valid_actions",[]))
        current=[duration]
        dispatched=[]
        legacy={"accepted":True, "run_id":"run", "requested_run_id":"run",
                "frame_id":"duration-frame", "action_id":"wait.2h",
                "requested_frame_id":"duration-frame", "consuming_frame_id":"duration-frame",
                "requested_surface_id":"duration", "consuming_surface_id":"duration",
                "descriptor_game_turn":6000, "legacy_native_frame_id":"run:6000:140",
                "provenance":"native_wait_duration_legacy_receipt"}
        if sequence is not ABSENT_CLOCK:
            legacy["sequence"] = sequence
        def dispatch(frame, action, stable_id=None):
            dispatched.append(action)
            current[0]=waiting if action=="wait.2h" else world
            return {"native_receipt":legacy if action=="wait.2h" else stop,
                    "next_frame":current[0]}
        channel=CockpitRunChannel(lambda:current[0],dispatch,binding_id="bound",
                                  read_binding_id=lambda:"bound")
        service=CockpitService(run_channel=channel)
        observed=service.call({"action":"game.observe"})["result"]
        accepted=service.call({"action":"game.act", "observation_id":observed["observation_id"],
                               "action_id":"wait.2h"})
        self.assertTrue(accepted["ok"],accepted)
        self.assertEqual(channel._active_operation["accepted_receipt"], legacy)
        return service, channel, current, prompt, stop, world, legacy, dispatched

    def test_accepted_legacy_duration_then_native_stop_is_settled_once(self):
        service, channel, current, prompt, stop, world, legacy, dispatched = self.accepted_legacy_duration()
        self.assertNotIn("sequence", channel._active_operation["accepted_receipt"])
        current[0]=prompt
        observed=service.call({"action":"game.observe"})["result"]
        request={"action":"game.act", "observation_id":observed["observation_id"],
                 "action_id":"prompt.choose", "stable_id":"fresh-yes"}
        for field,value in (("process_instance","foreign"),("run_id","foreign"),
                            ("sequence",20),("frame_id","foreign"),("game_turn",6001)):
            with self.subTest(legacy_owner=field):
                invalid=deepcopy(channel._active_operation)
                invalid["accepted_owner"][field]=value
                with self.assertRaisesRegex(ValueError,"owner_or_receipt_unproved"):
                    CockpitRunChannel._stopped_wait_operation(invalid,prompt,request,stop,world)
        result=service.call(request)
        self.assertTrue(result["ok"],result)
        self.assertEqual(channel._active_operation["state"],"cancelled")
        self.assertEqual(channel._active_operation["completed_progress_game_minutes"],13)
        self.assertEqual(channel._active_operation["accepted_receipt"],legacy)
        self.assertFalse(service.call(request)["ok"])
        self.assertEqual(dispatched,["wait.2h","prompt.choose"])

    def test_present_invalid_clock_cannot_use_legacy_fallback(self):
        for sequence in (True, False, "bad", "10", None, -1, 20, 21):
            with self.subTest(sequence=sequence):
                service, channel, current, prompt, _, _, legacy, dispatched = self.accepted_legacy_duration(sequence)
                current[0] = prompt
                observed = service.call({"action":"game.observe"})["result"]
                request = {"action":"game.act", "observation_id":observed["observation_id"],
                           "action_id":"prompt.choose", "stable_id":"fresh-yes"}
                result = service.call(request)
                self.assertFalse(result["ok"], result)
                self.assertEqual(result["error"], "wait_stop_owner_or_receipt_unproved")
                self.assertTrue(result["receipt"]["native_receipt"]["accepted"])
                self.assertEqual(channel._active_operation["accepted_receipt"], legacy)
                self.assertEqual(channel._active_operation["state"], "failed")
                self.assertEqual(channel._active_operation["blocker"], "wait_stop_owner_or_receipt_unproved")
                self.assertNotIn("termination", channel._active_operation)
                self.assertFalse(service.call(request)["ok"])
                self.assertEqual(dispatched, ["wait.2h", "prompt.choose"])

    def test_native_clock_is_primary_over_legacy_owner_clock(self):
        service, channel, current, prompt, _, _, legacy, dispatched = self.accepted_legacy_duration(12)
        channel._active_operation["accepted_owner"]["sequence"] = 30
        current[0] = prompt
        observed = service.call({"action":"game.observe"})["result"]
        result = service.call({"action":"game.act", "observation_id":observed["observation_id"],
                               "action_id":"prompt.choose", "stable_id":"fresh-yes"})
        self.assertTrue(result["ok"], result)
        self.assertEqual(channel._active_operation["state"], "cancelled")
        self.assertEqual(channel._active_operation["accepted_receipt"], legacy)
        self.assertEqual(dispatched, ["wait.2h", "prompt.choose"])

    def test_unsequenced_unproved_receipt_refuses_without_key_error(self):
        o,p,q,r,w=self.rows()
        o["accepted_receipt"]={"provenance":"native_wait_duration_legacy_receipt"}
        with self.assertRaisesRegex(ValueError,"owner_or_receipt_unproved"):
            CockpitRunChannel._stopped_wait_operation(o,p,q,r,w)

if __name__=='__main__':unittest.main()
