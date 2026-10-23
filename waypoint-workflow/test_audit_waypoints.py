"""Evidence integrity regressions. File fixtures stay in TEMP for user cleanup."""
import copy
import json
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

import audit_waypoints as audit
import stop_policy
from export_manual_plan import HEADER, NODE, export


def plan_fixture():
    return {'map': 'unit_map', 'author': 'test',
            'nodes': [{'label': name, 'x': i * 10.0, 'y': 0.0, 'z': 0.0, 'radius': 4, 'flags': 0}
                      for i, name in enumerate(('a', 'b', 'c'))],
            'directed_edges': [{'from': source, 'to': target, 'flags': 0}
                               for source, target in (('a', 'b'), ('b', 'c'), ('c', 'b'), ('b', 'a'))]}


def record_fixture(route=None):
    route = [0, 1, 2, 1, 0] if route is None else route
    target = route[-1]
    return {'target': ('a', 'b', 'c')[target] if target < 3 else 'unknown', 'index': target,
            'status': 'arrived', 'route': ' '.join(map(str, route)),
            'before': {'team': 3, 'alive': 1},
            'after': {'team': 3, 'alive': 1, 'position': [target * 10.0, 0.0, 0.0], 'velocity': [0.0, 0.0, 0.0]},
            'stopped_distance': 0.0, 'target_radius': 4, 'settled': True,
            'log': ('test_unique_marker\n[AgPB] TestBot: walking to #%d, %d hop(s): %s\n'
                    '[AgPB] TestBot: arrived at waypoint #%d (%d hop(s) walked)\n' %
                    (target, len(route), ' '.join(map(str, route)), target, len(route) - 1))}


class ExportAndCompareTests(unittest.TestCase):
    def setUp(self):
        self.plan = plan_fixture()

    def test_roundtrip_every_field(self):
        result = audit.compare(self.plan, export(self.plan))
        self.assertTrue(result['accepted'])
        self.assertEqual([], result['differences'])

    def test_missing_radius_rejected(self):
        del self.plan['nodes'][0]['radius']
        with self.assertRaisesRegex(ValueError, 'radius'):
            export(self.plan)

    def test_duplicate_directed_edge_rejected_even_identical(self):
        self.plan['directed_edges'].append(dict(self.plan['directed_edges'][0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate directed edge'):
            export(self.plan)

    def test_unknown_label_and_duplicate_node_rejected(self):
        self.plan['directed_edges'][0]['to'] = 'missing'
        with self.assertRaisesRegex(ValueError, 'unknown'):
            export(self.plan)
        self.plan = plan_fixture()
        self.plan['nodes'][1]['label'] = 'a'
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            export(self.plan)

    def test_numeric_types_and_finite_float32(self):
        for field, value in [('radius', True), ('radius', 4.5), ('x', '10'), ('x', float('inf')), ('x', 1e100)]:
            with self.subTest(field=field, value=value):
                plan = plan_fixture()
                plan['nodes'][0][field] = value
                with self.assertRaises(ValueError):
                    export(plan)

    def test_unsupported_plan_version_rejected(self):
        self.plan['plan_version'] = 2
        with self.assertRaisesRegex(ValueError, 'plan_version'):
            export(self.plan)

    def test_author_difference_requires_explicit_allowance(self):
        other = copy.deepcopy(self.plan)
        other['author'] = 'native save'
        result = audit.compare(self.plan, export(other))
        self.assertFalse(result['accepted'])
        result = audit.compare(self.plan, export(other), allow_author_change=True)
        self.assertTrue(result['accepted'])
        self.assertEqual('native save', result['author_change']['actual'])

    def test_link_slot_and_gravity_difference_reported(self):
        data = bytearray(export(self.plan))
        fields = list(NODE.unpack_from(data, HEADER.size))
        fields[14] = 1  # connectionFlags[0]
        fields[-1] = 0.5
        NODE.pack_into(data, HEADER.size, *fields)
        result = audit.compare(self.plan, data)
        self.assertFalse(result['accepted'])
        self.assertEqual({'connectionFlags[0]', 'gravity'}, {d['field'] for d in result['differences']})

    def test_truncated_and_trailing_bytes_rejected(self):
        data = export(self.plan)
        for invalid in (data[:10], data[:-1], data + b'x'):
            with self.assertRaises(ValueError):
                audit.decode_agpw(invalid)

    def test_team_restriction_is_filter_not_spawn(self):
        self.plan['nodes'][1]['flags'] = audit.WP_T
        result = audit.reachability(self.plan)
        self.assertTrue(result['T']['strongly_connected'])
        self.assertFalse(result['CT']['strongly_connected'])
        self.assertEqual([1], result['CT']['blocked_nodes'])
        self.assertFalse(audit.compare(self.plan, export(self.plan))['accepted'])

    def test_both_team_flags_and_avoid_double_jump_block(self):
        self.assertFalse(audit.passable(audit.WP_T | audit.WP_CT, 2))
        self.assertFalse(audit.passable(audit.WP_T | audit.WP_CT, 3))
        for flags in (audit.WP_DJUMP, audit.WP_AVOID):
            self.assertFalse(audit.passable(flags, 0))
        self.plan['directed_edges'][0]['flags'] = audit.PATH_DOUBLE
        self.assertFalse(audit.reachability(self.plan)['CT']['strongly_connected'])


class CoverageTests(unittest.TestCase):
    def setUp(self):
        self.plan = plan_fixture()
        self.plan_hash = audit.digest(json.dumps(self.plan).encode())
        self.record = record_fixture()
        self.wrapper = {'schema_version': 1,
                        'metadata': {'map': 'unit_map', 'plan_sha256': self.plan_hash, 'plan_version': 1, 'team': 3},
                        'records': [self.record]}

    def run_audit(self, **kwargs):
        return audit.coverage(self.plan, self.plan_hash, [('records.json', self.wrapper)], **kwargs)

    def test_bound_complete_coverage(self):
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertEqual(3, result['covered_nodes'])
        self.assertEqual(4, result['covered_directed_edges'])
        self.assertFalse(result['precise_stop_certified'])

    def test_driver_named_team_and_bot_binding(self):
        self.wrapper['metadata'].update(team='CT', bot_name='TestBot')
        self.assertTrue(self.run_audit()['accepted'])
        self.wrapper['metadata']['bot_name'] = 'OtherBot'
        self.assertFalse(self.run_audit()['accepted'])

    def test_unsettled_is_separate_from_route_arrival(self):
        self.record.update(settled=False, stop_status='unsettled', within_radius=False)
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertFalse(result['precise_stop_certified'])
        self.assertEqual('unsettled', result['stop_measurements'][0]['driver_stop_status'])

    def new_endpoint(self, radius=0, position=None):
        self.plan['nodes'][0]['radius'] = radius
        self.plan_hash = audit.digest(json.dumps(self.plan).encode())
        self.wrapper['metadata'].update(plan_sha256=self.plan_hash, bot_slot=2, stop_policy=stop_policy.description())
        self.record['before'].update(slot=2, userid=22)
        self.record['after'].update(slot=2, userid=22, time=.8, flags=1,
                                    position=list(position or [3., 0., 3.]))
        self.record['stop_samples'] = [dict(self.record['after'], time=t) for t in (0., .4, .8)]
        self.record.update(stop_policy.evaluate(self.plan['nodes'][0], self.record['stop_samples'], self.record['after']))
        self.record['target_radius'] = radius

    def test_versioned_radius_zero_endpoint_certifies_without_stopping_intermediate_nodes(self):
        self.new_endpoint()
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertTrue(result['precise_stop_certified'])
        self.assertEqual(3, result['covered_nodes'])
        self.assertEqual(1, len(result['stop_measurements']))
        measured = result['stop_measurements'][0]
        self.assertEqual(0, measured['raw_radius'])
        self.assertEqual(4, measured['effective_radius'])
        self.assertGreater(measured['distance'], 4)

    def test_new_radius_eight_and_vertical_errors_fail_precision_only(self):
        for radius, position in ((8, [56.9, 0., 0.]), (200, [3., 0., 5.])):
            self.new_endpoint(radius, position)
            result = self.run_audit()
            self.assertTrue(result['accepted'])
            self.assertFalse(result['precise_stop_certified'])

    def test_new_endpoint_missing_ground_velocity_or_final_window_cannot_certify(self):
        for missing in ('flags', 'velocity', 'stop_samples'):
            self.new_endpoint()
            if missing == 'stop_samples':
                self.record[missing] = []
            else:
                self.record['after'].pop(missing, None)
                for sample in self.record['stop_samples']:
                    sample.pop(missing, None)
            with self.subTest(missing=missing):
                result = self.run_audit()
                self.assertTrue(result['accepted'])
                self.assertFalse(result['precise_stop_certified'])

    def test_unversioned_schema_one_never_recertifies_old_stop_samples(self):
        self.new_endpoint()
        del self.wrapper['metadata']['stop_policy']
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertFalse(result['precise_stop_certified'])
        self.assertEqual('legacy_unversioned_policy', result['stop_measurements'][0]['policy_classification'])
        self.assertNotIn('effective_radius', result['stop_measurements'][0])

    def test_unknown_policy_parameters_and_mismatched_after_do_not_certify(self):
        self.new_endpoint()
        self.wrapper['metadata']['stop_policy']['max_vertical_error'] = 32
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertFalse(result['precise_stop_certified'])
        self.assertEqual('unsupported_policy', result['stop_measurements'][0]['policy_classification'])
        self.new_endpoint()
        self.record['after'] = dict(self.record['after'], time=.9)
        self.assertFalse(self.run_audit()['precise_stop_certified'])

    def test_stale_hash_rejected_even_when_legacy_allowed(self):
        self.wrapper['metadata']['plan_sha256'] = '0' * 64
        for allowed in (False, True):
            result = self.run_audit(allow_legacy=allowed)
            self.assertFalse(result['accepted'])
            self.assertEqual(0, result['covered_nodes'])
            self.assertIn('plan_sha256 mismatch (stale evidence)', result['files'][0]['errors'])

    def test_version_mismatch_rejected(self):
        self.wrapper['metadata']['plan_version'] = 2
        self.assertFalse(self.run_audit(allow_legacy=True)['accepted'])

    def test_legacy_requires_explicit_downgrade(self):
        self.wrapper = [self.record]
        self.assertFalse(self.run_audit()['accepted'])
        result = self.run_audit(allow_legacy=True)
        self.assertTrue(result['accepted'])
        self.assertEqual('legacy_unbound', result['classification'])

    def test_forged_success_without_logs_never_counts(self):
        original_log = self.record['log']
        self.record['log'] = ''
        result = self.run_audit(console_log=original_log)
        self.assertFalse(result['accepted'])
        self.assertEqual(0, result['covered_directed_edges'])

    def test_incomplete_coverage_rejected(self):
        self.wrapper['records'] = [record_fixture([0, 1])]
        result = self.run_audit()
        self.assertFalse(result['accepted'])
        self.assertEqual([2], result['missing_nodes'])
        self.assertEqual(3, len(result['missing_directed_edges']))

    def test_failed_status_not_overridden_by_success_log(self):
        self.record['status'] = 'timeout'
        result = self.run_audit()
        self.assertFalse(result['accepted'])
        self.assertEqual('timeout', result['failed_records'][0]['status'])

    def test_wrong_route_hop_count_or_bot_cannot_authenticate(self):
        original = self.record['log']
        for altered in (original.replace('0 1 2 1 0', '0 2 1 2 0'),
                        original.replace('(4 hop(s) walked)', '(3 hop(s) walked)'),
                        original.replace('TestBot: arrived', 'OtherBot: arrived')):
            with self.subTest(log=altered):
                self.record['log'] = altered
                self.assertFalse(self.run_audit()['accepted'])

    def test_intervening_failure_or_route_invalidates_arrival(self):
        original = self.record['log']
        for interruption in ('[AgPB] TestBot: route aborted: waypoint #1 no longer exists\n',
                             '[AgPB] TestBot: walking to #2, 2 hop(s): 1 2\n'):
            self.record['log'] = original.replace('[AgPB] TestBot: arrived', interruption + '[AgPB] TestBot: arrived')
            self.assertFalse(self.run_audit()['accepted'])

    def test_unknown_node_and_edge_reported(self):
        self.record['route'] = '0 9 0'
        result = self.run_audit()
        self.assertFalse(result['accepted'])
        self.assertEqual([9], result['unknown_nodes'])
        self.assertIn((0, 9), result['unknown_directed_edges'])

    def test_team_forbidden_route_rejected(self):
        self.plan['nodes'][1]['flags'] = audit.WP_T
        result = self.run_audit()
        self.assertFalse(result['accepted'])
        self.assertTrue(any('blocked for the actual team' in e for e in result['records'][0]['errors']))

    def test_stop_error_recomputed_and_never_certifies_precision(self):
        self.record['after']['position'] = [8.0, 0.0, 0.0]
        self.record['stopped_distance'] = 8.0
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertEqual(8, result['max_stop_distance'])
        self.assertEqual(1, result['outside_radius_count'])
        self.assertFalse(result['precise_stop_certified'])
        self.record['stopped_distance'] = 0.0
        self.assertFalse(self.run_audit()['accepted'])

    def test_truncated_log_needs_unique_exact_console_anchor(self):
        full = self.record['log'].replace('[AgPB] TestBot: arrived', 'UNIQUE_VERIFY_SAMPLE\n[AgPB] TestBot: arrived')
        self.record['log'] = full[full.index('UNIQUE_VERIFY_SAMPLE'):]
        self.assertFalse(self.run_audit()['accepted'])
        result = self.run_audit(console_log=full)
        self.assertTrue(result['accepted'])
        self.assertEqual('console-log-excerpt', result['records'][0]['log_evidence'])
        self.assertFalse(self.run_audit(console_log=full + full)['accepted'])
        self.assertFalse(self.run_audit(console_log=full.replace('UNIQUE_VERIFY_SAMPLE', 'UNRELATED_SAMPLE'))['accepted'])


    def asset_metadata(self):
        self.wrapper['metadata'].update(agpw_sha256='a' * 64, bsp_sha256='b' * 64, dll_sha256='c' * 64)

    def test_conflicting_assets_invalidate_all_sources_even_when_legacy_allowed(self):
        self.asset_metadata()
        other = copy.deepcopy(self.wrapper)
        other['metadata']['dll_sha256'] = 'd' * 64
        for allowed in (False, True):
            result = audit.coverage(self.plan, self.plan_hash,
                                    [('first.json', self.wrapper), ('second.json', other)], allow_legacy=allowed)
            self.assertFalse(result['accepted'])
            self.assertEqual(['dll'], result['asset_version_conflicts'])
            self.assertEqual(0, result['covered_nodes'])
            self.assertTrue(all(file['errors'] for file in result['files']))

    def test_explicit_current_assets_reject_missing_hashes_or_downgrade_legacy(self):
        self.new_endpoint()
        with patch('audit_waypoints.Path.read_bytes', return_value=b'current dll'):
            rejected = self.run_audit(assets={'dll': 'current.dll'})
            historical = self.run_audit(assets={'dll': 'current.dll'}, allow_legacy=True)
        self.assertFalse(rejected['accepted'])
        self.assertIn('missing asset hashes in explicit binding mode', rejected['files'][0]['errors'][0])
        self.assertTrue(historical['accepted'])
        self.assertEqual('legacy_assets_unbound', historical['asset_binding'])
        self.assertEqual('legacy_unbound', historical['classification'])
        self.assertFalse(historical['precise_stop_certified'])

    def test_expected_current_dll_matches_and_mismatches(self):
        self.asset_metadata()
        current = b'current compiled dll'
        self.wrapper['metadata']['dll_sha256'] = audit.digest(current)
        with patch('audit_waypoints.Path.read_bytes', return_value=current):
            good = self.run_audit(assets={'dll': 'current.dll'})
        self.assertTrue(good['accepted'])
        self.assertEqual('current_assets_bound', good['asset_binding'])
        self.assertEqual(['agpw', 'bsp'], good['unchecked_current_assets'])
        self.wrapper['metadata']['dll_sha256'] = 'f' * 64
        with patch('audit_waypoints.Path.read_bytes', return_value=current):
            bad = self.run_audit(assets={'dll': 'current.dll'}, allow_legacy=True)
        self.assertFalse(bad['accepted'])
        self.assertIn('dll_sha256 differs from current asset', bad['files'][0]['errors'])

    def test_current_agpw_must_also_match_explicit_plan(self):
        self.asset_metadata()
        current = export(self.plan)
        self.wrapper['metadata']['agpw_sha256'] = audit.digest(current)
        with patch('audit_waypoints.Path.read_bytes', return_value=current):
            self.assertTrue(self.run_audit(assets={'agpw': 'current.agpw'})['accepted'])
        changed = current + b'changed'
        self.wrapper['metadata']['agpw_sha256'] = audit.digest(changed)
        with patch('audit_waypoints.Path.read_bytes', return_value=changed):
            result = self.run_audit(assets={'agpw': 'current.agpw'})
        self.assertFalse(result['accepted'])
        self.assertIn('current AGPW data/map differs from explicit plan', result['files'][0]['errors'])

    def test_invalid_claimed_asset_hash_never_accepted_as_legacy(self):
        self.wrapper['metadata']['bsp_sha256'] = 'bad'
        for allowed in (False, True):
            self.assertFalse(self.run_audit(allow_legacy=allowed)['accepted'])

    def test_plan_only_compatibility_explicitly_reports_incomplete_assets(self):
        result = self.run_audit()
        self.assertTrue(result['accepted'])
        self.assertEqual('metadata_incomplete', result['asset_binding'])
        self.assertEqual(['agpw', 'bsp', 'dll'], result['unchecked_current_assets'])

    def test_precise_unique_endpoint_coverage_differs_from_route_traversal(self):
        self.new_endpoint()
        self.wrapper['records'].append(copy.deepcopy(self.record))
        result = self.run_audit()
        endpoint = result['endpoint_coverage']
        self.assertEqual(3, result['covered_nodes'])
        self.assertEqual(1, endpoint['unique_requested_endpoints'])
        self.assertEqual([0], endpoint['requested_endpoint_ids'])
        self.assertEqual(1, endpoint['unique_precise_endpoints'])
        self.assertEqual([0], endpoint['precise_endpoint_ids'])
        self.assertEqual([1, 2], endpoint['missing_plan_precise_endpoint_ids'])
        self.assertEqual([], endpoint['requested_missing_precise_endpoint_ids'])
        self.assertFalse(endpoint['all_plan_endpoints_precisely_verified'])
        self.assertTrue(result['precise_stop_certified'])

    def test_requested_failed_endpoint_reported_separately_from_unrequested(self):
        self.new_endpoint()
        failed = record_fixture([0, 1])
        failed['status'] = 'timeout'
        self.wrapper['records'].append(failed)
        endpoint = self.run_audit()['endpoint_coverage']
        self.assertEqual([0, 1], endpoint['requested_endpoint_ids'])
        self.assertEqual([0], endpoint['precise_endpoint_ids'])
        self.assertEqual([1], endpoint['requested_missing_precise_endpoint_ids'])
        self.assertEqual([2], endpoint['unrequested_endpoint_ids'])


class BundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_root = Path(tempfile.mkdtemp(prefix='agpb-audit-tests-'))
        print('\nRetained test fixtures (user cleanup only): ' + str(cls.fixture_root), flush=True)

    def setUp(self):
        self.root = self.fixture_root / self._testMethodName
        self.root.mkdir()
        self.asset = self.root / 'unit_map.agpw'
        self.asset.write_bytes(export(plan_fixture()))
        self.manifest = {'map': 'unit_map', 'agpw_sha256': audit.digest(self.asset.read_bytes()),
                         'files': {'unit_map.agpw': audit.digest(self.asset.read_bytes())}}
        self.manifest_path = self.root / 'manifest.json'
        self.save_manifest()

    def save_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding='utf-8')

    def test_valid_integrity_and_current_asset(self):
        result = audit.verify_bundle(self.manifest_path, {'agpw': self.asset})
        self.assertTrue(result['accepted'])
        self.assertEqual(1, result['verified_files'])

    def test_hash_tamper_rejected(self):
        self.asset.write_bytes(self.asset.read_bytes() + b'tampered')
        result = audit.verify_bundle(self.manifest_path)
        self.assertFalse(result['accepted'])
        self.assertIn('SHA256 mismatch', result['errors'][0]['error'])

    def test_manifest_path_escape_rejected(self):
        for path in ('../outside.bin', '..\\outside.bin', 'C:\\outside.bin', '/outside.bin', 'file:stream'):
            with self.subTest(path=path):
                self.manifest['files'] = {path: '0' * 64}
                self.save_manifest()
                result = audit.verify_bundle(self.manifest_path)
                self.assertFalse(result['accepted'])
                self.assertIn('unsafe manifest path', result['errors'][0]['error'])

    def test_bad_digest_missing_file_and_current_asset_mismatch(self):
        self.manifest['files']['unit_map.agpw'] = 'bad'
        self.save_manifest()
        self.assertFalse(audit.verify_bundle(self.manifest_path)['accepted'])
        self.manifest['files'] = {'missing.bin': '0' * 64}
        self.save_manifest()
        self.assertFalse(audit.verify_bundle(self.manifest_path)['accepted'])
        self.manifest['files'] = {'unit_map.agpw': audit.digest(self.asset.read_bytes())}
        self.save_manifest()
        alternate = self.root / 'alternate.agpw'
        alternate.write_bytes(b'changed')
        self.assertFalse(audit.verify_bundle(self.manifest_path, {'agpw': alternate})['accepted'])


if __name__ == '__main__':
    unittest.main()