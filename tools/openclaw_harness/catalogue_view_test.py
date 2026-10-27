#!/usr/bin/env python3
"""Discovery and source-bound selection checks without native gameplay."""
import contextlib
import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock
import cockpit
import scenario_registry_store as store
import scenario_registry_cli as cli
import startup_harness
from catalogue_view import family_view
import scenario_registry_cli_test as cli_fixtures

HERE = Path(__file__).resolve().parent

class CatalogueViewTest(unittest.TestCase):
    def cli(self, *args):
        result = subprocess.run([sys.executable, '-B', str(HERE / 'startup_harness.py'), *args], text=True, capture_output=True, check=True)
        return json.loads(result.stdout)

    def test_default_expansion_and_real_fixture_links(self):
        families = {family['id']: family for family in self.cli('list-scenarios')['families']}
        for name in ('migo-tower-journey', 'human-signal-journeys', 'stalker-pursuit'):
            self.assertIn(name, families)
        self.assertNotIn('mixed-load-controls', families)
        soak = families['natural-play-endurance']
        self.assertEqual(soak['variants'], [])
        self.assertEqual(soak['starting_save'], 'Starting save not yet defined')
        self.assertTrue(soak['recipe'].endswith('/long-running-soak.md'))
        refused = subprocess.run([sys.executable, '-B', str(HERE / 'startup_harness.py'),
                                  'list-scenarios', '--scenario-id', 'natural-play-endurance'],
                                 text=True, capture_output=True)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn('missing or ambiguous', refused.stderr)
        variant = self.cli('list-scenarios', '--family', 'migo-tower-journey')['families'][0]['variants'][0]
        self.assertEqual(variant['name'], 'migo.r050_tower_remote_initial')
        self.assertEqual(variant['fixture']['name'], 'r050_migo_tower_remote_initial_v1')
        self.assertTrue(variant['fixture']['manifest_present'])
        self.assertEqual(variant['fixture']['qualification'], 'unknown')
        self.assertEqual(variant['scenario_id'], store.manifest_identity(Path(variant['path'])))
        fixtures = self.cli('list-fixtures', '--profile', 'live-debug')['fixtures']
        linked = next(x for x in fixtures if x['fixture'] == variant['fixture']['name'])
        self.assertIn(variant['scenario_id'], [x['scenario_id'] for x in linked['declared_variants']])
        self.assertEqual({x['id'] for x in self.cli('list-scenarios', '--diagnostic')['families']}, {'mixed-load-controls'})

    def test_search_invalid_identity_and_missing_manifest(self):
        full = self.cli('list-scenarios', '--all')['scenarios']
        self.assertTrue(any(x['name'].startswith('manual.') for x in full))
        name = 'cannibal.r_caol_r051_r067_post_report_8341_service_continuation_20261004'
        row = next(x for x in self.cli('list-scenarios', '--search', name)['other_scenarios'] if x['name'] == name)
        self.assertEqual(row['membership'], 'unknown')
        self.assertEqual(row['scenario_manifest']['validation']['status'], 'invalid')
        self.assertEqual(self.cli('list-scenarios', '--scenario-id', row['scenario_id'])['scenarios'][0]['name'], name)
        missing = self.cli('list-scenarios', '--scenario-id', 'cannibal.r_caol_first_smoke_048_rural_a_preharm_safety_continue_20260930')['scenarios'][0]
        self.assertFalse(missing['fixture']['manifest_present'])
        self.assertEqual(missing['fixture']['qualification'], 'unknown')

    def test_family_conflict_and_malformed_membership(self):
        m = dict(kind='maintained', family_id='f', family_title='One', family_purpose='Purpose', variant='A')
        with self.assertRaises(ValueError):
            family_view([{'catalogue': m}, {'catalogue': dict(m, family_title='Other')}])
        invalid = family_view([{'name': 'bad', 'catalogue': {'kind': 'maintained'}}], kind='all')
        self.assertEqual(invalid['other_scenarios'][0]['membership'], 'unknown')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'bad.json'
            path.write_text(json.dumps({'name': 'bad', 'catalogue': {'kind': 'maintained'}}))
            with self.assertRaises(SystemExit):
                startup_harness.load_scenario('bad', source_path=path)

    def test_metadata_change_does_not_authorize_old_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            helper = cli_fixtures.ScenarioRegistryCliTest()
            registry, scenarios, token = helper.issue_selection_token(Path(tmp))
            manifest = scenarios / 'cli.json'
            connection = store.open_registry(str(registry))
            try:
                self.assertTrue(store.reload_selection_token_for_launch(connection, token).accepted)
                declaration = json.loads(manifest.read_text())
                declaration['catalogue'] = dict(kind='maintained', family_id='f', family_title='F', family_purpose='P', variant='V')
                manifest.write_text(json.dumps(declaration))
                self.assertFalse(store.reload_selection_token_for_launch(connection, token).accepted)
                store.rebuild_manifest_projection(connection, scenarios)
                self.assertFalse(store.reload_selection_token_for_launch(connection, token).accepted)
            finally:
                connection.close()
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = cli.main(['--json', '--registry', str(registry), 'registry-query', '--query-json',
                                   json.dumps({'requirements': [], 'preferences': []})])
            self.assertEqual(status, 0)
            result = json.loads(out.getvalue())['result']
            candidate = next(x for x in result['candidates'] if x['name'] == declaration['name'])
            self.assertEqual(candidate['catalogue']['family_id'], 'f')
            self.assertNotEqual(result.get('token_id'), token)

    def test_cockpit_select_uses_requested_exact_identity(self):
        candidate = SimpleNamespace(scenario_id='other', facts={}, lifecycle_state='active', explanation={})
        target = SimpleNamespace(scenario_id='wanted', facts={}, lifecycle_state='active', explanation={})
        execution = SimpleNamespace(evaluation=SimpleNamespace(candidates=[candidate, target]), selection_id='selection', selected_scenario_id='wanted')
        conn = mock.MagicMock()
        conn.__enter__.return_value = conn
        conn.execute.return_value.fetchone.return_value = {'fit_reason': 'matched'}
        with mock.patch.object(cockpit, 'open_registry', return_value=conn), mock.patch.object(cockpit, 'execute_registry_query', return_value=execution) as query:
            result = cockpit.CockpitService().call({'action': 'scenario.select', 'scenario_id': 'wanted'})
        self.assertTrue(result['ok'])
        self.assertEqual(query.call_args.kwargs['scenario_id'], 'wanted')
        self.assertEqual(result['result']['selection']['scenario_id'], 'wanted')

    def test_real_cockpit_registry_selection_and_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            registry, scenarios, _ = cli_fixtures.ScenarioRegistryCliTest().issue_selection_token(Path(tmp))
            identity = store.manifest_identity(scenarios / 'cli.json')
            query = {'action': 'scenario.select', 'scenario_id': identity,
                     'requirements': [{'key': 'player.injured', 'op': 'eq', 'value': False,
                                       'minimum_evidence': 'declared'}]}
            result = cockpit.CockpitService(str(registry)).call(query)
            self.assertTrue(result['ok'])
            self.assertEqual(result['result']['selection']['scenario_id'], identity)
            query['scenario_id'] = 'missing-identity'
            rejected = cockpit.CockpitService(str(registry)).call(query)
            self.assertFalse(rejected['ok'])
            self.assertIn('missing or ambiguous', rejected['error'])

    def test_browse_variant_selects_same_real_registry_source(self):
        class AnnotatedFixture(cli_fixtures.ScenarioRegistryCliTest):
            def strict_manifest(self):
                declaration = super().strict_manifest()
                declaration['catalogue'] = dict(kind='maintained', family_id='test-family',
                    family_title='Test family', family_purpose='Exercise exact selection', variant='Test start')
                return declaration
        with tempfile.TemporaryDirectory() as tmp:
            registry, scenarios, _ = AnnotatedFixture().issue_selection_token(Path(tmp))
            with mock.patch.object(cockpit.startup_harness, 'scenarios_root', return_value=scenarios):
                service = cockpit.CockpitService(str(registry))
                browse = service.call({'action': 'scenario.browse', 'family': 'test-family'})
                identity = browse['result']['families'][0]['variants'][0]['scenario_id']
                self.assertEqual(identity, store.manifest_identity(scenarios / 'cli.json'))
                result = service.call({'action': 'scenario.select', 'scenario_id': identity,
                    'requirements': [{'key': 'player.injured', 'op': 'eq', 'value': False,
                                      'minimum_evidence': 'declared'}]})
                self.assertTrue(result['ok'])
                self.assertEqual(result['result']['selection']['scenario_id'], identity)

    def test_cockpit_family_browse_and_search(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = cockpit.CockpitService(str(Path(tmp) / 'registry.sqlite3'))
            default = service.call({'action': 'scenario'})
            self.assertTrue(default['ok'])
            families = {row['id']: row for row in default['result']['families']}
            self.assertIn('migo-tower-journey', families)
            self.assertNotIn('mixed-load-controls', families)
            expanded = service.call({'action': 'scenario.browse', 'family': 'migo-tower-journey'})
            variant = expanded['result']['families'][0]['variants'][0]
            self.assertTrue(variant['scenario_id'])
            self.assertEqual(variant['fixture']['qualification'], 'unknown')
            diagnostic = service.call({'action': 'scenario.browse', 'view': 'diagnostic'})
            self.assertEqual(diagnostic['result']['families'][0]['id'], 'mixed-load-controls')
            search = service.call({'action': 'scenario.browse', 'view': 'all', 'search': 'r051_r067_post_report_8341_service'})
            self.assertTrue(any(row['scenario_manifest']['validation']['status'] == 'invalid'
                                for row in search['result']['other_scenarios']))

if __name__ == '__main__':
    unittest.main()
