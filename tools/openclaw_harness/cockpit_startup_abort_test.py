"""No-frame abort through the service, native finalizer and final reader."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import cockpit
import startup_harness as harness
from cockpit_archive import Archive


class StartupAbortTest(unittest.TestCase):
    def test_bound_abort_and_identity_refusals(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            archive = Archive(directory / 'evidence.sqlite', run_id='run-a', binding_id='binding-a')
            self.addCleanup(archive.close)
            state = {'pid': 42, 'run_id': 'run-a', 'alive': True, 'identity_status': 'alive'}
            current_binding = ['binding-a']
            final_path = directory / 'final.json'
            channel = cockpit.CockpitRunChannel(
                lambda: {}, archive=archive, binding_id='binding-a',
                read_binding_id=lambda: current_binding[0], read_process_state=lambda: dict(state),
                finalize_session=lambda report: harness.finalize_cockpit_live_session(
                    directory, 42, report, cleanup_process=False, final_path=final_path))
            service = cockpit.CockpitService(run_channel=channel)
            original = dict(state)
            for change in ({'run_id': 'wrong'}, {'run_id': ''}, {'run_id': None}, {'pid': 0}, {'pid': True},
                           {'identity_status': 'identity_changed'}, {'identity_status': 'identity_unavailable'},
                           {'alive': None}, {'alive': False}):
                with self.subTest(change=change):
                    state.update(original)
                    state.update(change)
                    result = service.call({'action': 'run.quit', 'abort': True})
                    self.assertEqual(result['error'], 'startup_abort_identity_unavailable')
                    self.assertEqual(channel.status()['state'], 'active')
                    self.assertFalse(final_path.exists())
            state.update(original)
            current_binding[0] = 'other'
            self.assertEqual(service.call({'action': 'run.quit', 'abort': True})['error'],
                             'startup_abort_identity_unavailable')
            current_binding[0] = 'binding-a'
            with patch.object(harness, 'current_owned_process_generation', return_value={
                    'pid': 42, 'status': 'alive', 'expected': {'pid': 42}}):
                result = service.call({'action': 'run.quit', 'abort': True})
            self.assertTrue(result['ok'], result)
            final = harness.read_completed_cockpit_final(service, final_path,
                                                          run_id='run-a', binding_id='binding-a')
            self.assertEqual(final['run_id'], 'run-a')
            self.assertEqual(channel.status()['run_id'], 'run-a')
            self.assertEqual(final['closure_kind'], 'explicit_abort')
            for key in ('native_save_credit', 'native_exit_credit', 'normal_exit_credit',
                        'declared_reentry_ready'):
                self.assertFalse(final[key])
            self.assertEqual(service.call({'action': 'run.quit', 'abort': True})['result'], result['result'])
            self.assertEqual(harness.read_completed_cockpit_final(service, final_path,
                             run_id='run-a', binding_id='binding-a'), final)
            with self.assertRaisesRegex(ValueError, 'owned_cockpit_final_identity_mismatch'):
                harness.read_completed_cockpit_final(service, final_path,
                                                     run_id='wrong', binding_id='binding-a')

    def test_unbound_archive_is_not_startup_abort_authority(self):
        state = {'pid': 42, 'run_id': 'run-a', 'alive': True, 'identity_status': 'alive'}
        channel = cockpit.CockpitRunChannel(lambda: {}, binding_id='binding-a',
                                             read_process_state=lambda: state)
        result = cockpit.CockpitService(run_channel=channel).call({'action': 'run.quit', 'abort': True})
        self.assertEqual(result['error'], 'startup_abort_identity_unavailable')
        self.assertEqual(channel.status()['state'], 'active')


if __name__ == '__main__':
    unittest.main()
