"""Incremental evidence integrity tests; generated assets remain in system TEMP."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

import audit_incremental as inc
import audit_waypoints as audit
from export_manual_plan import HEADER, NODES, export
import stop_policy


def node(index, *, z=0, flags=0):
    return dict(label='n%d' % index, x=index * 100.0, y=0.0, z=float(z), radius=4, flags=flags)


def plans(ladder=False):
    old = dict(plan_version=1, agpw_version=2, map='unit_map', author='test',
               nodes=[node(0)], directed_edges=[])
    final = copy.deepcopy(old)
    final['nodes'] += ([node(1, z=10, flags=32), node(2, z=110, flags=32), node(3, z=110)]
                       if ladder else [node(1)])
    final['directed_edges'] = [{'from': 'n%d' % i, 'to': 'n%d' % (i + 1), 'flags': 0}
                               for i in range(len(final['nodes']) - 1)]
    manifest = {'schema_version': 1, 'additions': [{'from': i, 'to': i + 1, 'slot': 0}
                                                 for i in range(len(final['nodes']) - 1)]}
    return old, final, manifest


def evidence(plan):
    target = len(plan['nodes']) - 1
    after = dict(slot=2, userid=22, time=11.5, team=3, alive=1, flags=1,
                 position=[plan['nodes'][target][key] for key in ('x', 'y', 'z')], velocity=[0., 0., 0.])
    record = dict(index=target, target='n%d' % target, route=list(range(target + 1)),
                  status='arrived', before=dict(after, time=10.), after=after,
                  stop_samples=[dict(after, time=stamp) for stamp in (10.6, 11., 11.5)],
                  log='[AgPB] TestBot: walking to #%d, %d hop(s): %s\n' %
                      (target, target + 1, ' '.join(map(str, range(target + 1)))) +
                      '[AgPB] TestBot: arrived at waypoint #%d (%d hop(s) walked)\n' % (target, target))
    record.update(stop_policy.evaluate(plan['nodes'][target], record['stop_samples'], after))
    meta = dict(map='unit_map', team='CT', bot_slot=2, human_slot=1, bot_name='TestBot',
                plan_version=1, plan_sha256=audit.digest(json.dumps(plan).encode()),
                stop_policy=stop_policy.description(), jump_probe_sha256='a' * 64)
    return dict(schema_version=1, metadata=meta, records=[record])


def ladder_log():
    lines = []
    for i, (x, z, move, ground) in enumerate(((100, 10, 9, -1), (150, 60, 9, -1),
                                             (200, 110, 9, -1), (300, 110, 2, 0)), 1):
        lines.append('[AGPB_JUMP_SAMPLE] v=1 epoch=test generation=1 seq=%d map_serial=1 map=unit_map '
                     'tick=%d t=%.4f sample=%d bot_slot=2 bot_userid=22 bot_serial=3 bot_fake=1 bot_alive=1 '
                     'human_slot=1 human_userid=11 human_serial=2 human_fake=0 human_alive=1 team=3 '
                     'pos=(%d,0,%d) velocity=(0,0,0) flags=0x%X movetype=%d ground_known=1 ground_ent=%d' %
                     (i, 640 + i, 10 + i / 10, i, x, z, 1 if ground >= 0 else 0, move, ground))
    lines.append('[AgPB lifecycle] v=1 name=TestBot edict=2 userid=22 map=unit_map tick=642 time=10.200 '
                 'route=4 goal=3 owner=7')
    return '\n'.join(lines) + '\n'


class DeltaTests(unittest.TestCase):
    def setUp(self):
        self.old, self.final, self.manifest = plans()
        self.original, self.candidate = export(self.old), export(self.final)

    def compare(self, candidate=None, manifest=None, **kw):
        return inc.compare_incremental(self.original, candidate or self.candidate,
                                       manifest or self.manifest, **kw)

    def test_exact_append_and_declared_empty_slot_pass(self):
        result = self.compare()
        self.assertTrue(result['accepted'])
        self.assertTrue(result['old_record_bytes_preserved'])
        self.assertEqual(result['new_node_ids'], [1])
        self.assertEqual(result['new_directed_edges'], [{'from': 0, 'to': 1, 'slot': 0, 'flags': 0}])

    def test_undeclared_or_unused_declarations_fail(self):
        self.assertFalse(self.compare(manifest={'schema_version': 1, 'additions': []})['accepted'])
        extra = copy.deepcopy(self.manifest)
        extra['additions'].append({'from': 1, 'to': 0, 'slot': 0})
        self.assertFalse(self.compare(manifest=extra)['accepted'])

    def test_old_core_flags_radius_gravity_and_signed_zero_are_frozen(self):
        for offset in (0, 12, 16, 50):
            candidate = bytearray(self.candidate)
            candidate[HEADER.size + offset] ^= 1
            self.assertFalse(self.compare(candidate=bytes(candidate))['accepted'])
        candidate = bytearray(self.candidate)
        struct.pack_into('<f', candidate, HEADER.size + 4, -0.)
        self.assertFalse(self.compare(candidate=bytes(candidate))['accepted'])

    def test_empty_slot_flags_outside_declared_slot_are_frozen(self):
        candidate = bytearray(self.candidate)
        struct.pack_into('<H', candidate, HEADER.size + 34 + 2, 1)
        self.assertFalse(self.compare(candidate=bytes(candidate))['accepted'])

    def test_existing_edge_flags_and_slot_cannot_change(self):
        old = copy.deepcopy(self.final)
        original = export(old)
        changed = bytearray(original)
        struct.pack_into('<H', changed, HEADER.size + 34, 1)
        result = inc.compare_incremental(original, bytes(changed), {'schema_version': 1, 'additions': []})
        self.assertFalse(result['accepted'])

    def test_author_requires_explicit_exception_and_version_change_fails(self):
        final = copy.deepcopy(self.final)
        final['author'] = 'native save'
        self.assertFalse(self.compare(candidate=export(final))['accepted'])
        self.assertTrue(self.compare(candidate=export(final), allow_author_change=True)['accepted'])
        final['agpw_version'] = 1
        self.assertFalse(self.compare(candidate=export(final), allow_author_change=True)['accepted'])


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = Path(tempfile.mkdtemp(prefix='agpb_incremental_tests_'))

    @classmethod
    def tearDownClass(cls):
        print('Retained test fixtures (user cleanup only):', cls.temp)

    def run_audit(self, ladder=False, mutate=None):
        old, plan, manifest = plans(ladder)
        wrapper = evidence(plan)
        if ladder:
            wrapper['records'][0]['ladder_log'] = ladder_log()
        if mutate:
            mutate(wrapper)
        assets = {}
        for key, data in (('agpw', export(plan)), ('bsp', b'unit bsp'), ('dll', b'unit dll')):
            path = self.temp / ('ladder' if ladder else 'normal') / key
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(data)
            assets[key] = path
            wrapper['metadata'][key + '_sha256'] = audit.digest(data)
        return inc.coverage_incremental(export(old), export(plan), manifest, plan,
                                        wrapper['metadata']['plan_sha256'], [('current.json', wrapper)],
                                        assets, 'a' * 64)

    def test_only_new_normal_endpoint_and_edges_required(self):
        result = self.run_audit()
        self.assertTrue(result['accepted'], result)
        self.assertEqual(result['precise_new_normal_endpoint_ids'], [1])
        self.assertEqual(result['accepted_new_directed_edges'], [(0, 1)])
        self.assertNotIn(0, result['missing_new_normal_endpoint_ids'])

    def test_route_passage_does_not_certify_unstopped_new_normal_node(self):
        result = self.run_audit(mutate=lambda doc: doc['records'][0]['stop_samples'].clear())
        self.assertFalse(result['accepted'])
        self.assertEqual(result['missing_new_normal_endpoint_ids'], [1])

    def test_ladder_nodes_need_continuous_traversal_not_grounded_endpoints(self):
        result = self.run_audit(ladder=True)
        self.assertTrue(result['accepted'], result)
        self.assertEqual(result['accepted_new_ladder_node_ids'], [1, 2])
        self.assertEqual(result['precise_new_normal_endpoint_ids'], [3])

    def test_route_logs_without_ladder_samples_never_pass(self):
        result = self.run_audit(ladder=True, mutate=lambda doc: doc['records'][0].pop('ladder_log'))
        self.assertFalse(result['accepted'])
        self.assertEqual(result['missing_new_ladder_node_ids'], [1, 2])

    def test_ladder_requires_actual_movetype_progress_and_dismount(self):
        for old, new in (('movetype=9', 'movetype=2'), ('pos=(200,0,110)', 'pos=(200,0,10)'),
                         ('flags=0x0 movetype=9 ground_known=1 ground_ent=-1\n[AGPB_JUMP_SAMPLE] v=1 epoch=test generation=1 seq=3',
                          'flags=0x0 movetype=8 ground_known=1 ground_ent=-1\n[AGPB_JUMP_SAMPLE] v=1 epoch=test generation=1 seq=3'),
                         ('ground_ent=0', 'ground_ent=-1'), ('owner=7', 'owner=0'),
                         ('sample=2 ', 'sample=3 '), ('bot_serial=3', 'bot_serial=0')):
            with self.subTest(change=old):
                def mutate(doc):
                    record = doc['records'][0]
                    record['ladder_log'] = record['ladder_log'].replace(old, new)
                result = self.run_audit(ladder=True, mutate=mutate)
                self.assertFalse(result['accepted'], result)

    def test_stale_probe_or_outside_request_samples_fail(self):
        self.assertFalse(self.run_audit(ladder=True, mutate=lambda doc:
                         doc['metadata'].update(jump_probe_sha256='b' * 64))['accepted'])
        self.assertFalse(self.run_audit(ladder=True, mutate=lambda doc:
                         doc['records'][0]['before'].update(time=11.))['accepted'])

    def test_small_slope_ground_origin_roundoff_does_not_reject_real_ladder(self):
        _, plan, _ = plans(ladder=True)
        plan['nodes'][1]['z'], plan['nodes'][2]['z'] = -274.6033, -100.0
        wrapper = evidence(plan)
        record = wrapper['records'][0]
        record['ladder_log'] = (ladder_log().replace('pos=(100,0,10)', 'pos=(100,0,-274.4046)')
                                .replace('pos=(150,0,60)', 'pos=(150,0,-190)')
                                .replace('pos=(200,0,110)', 'pos=(200,0,-100)')
                                .replace('pos=(300,0,110)', 'pos=(300,0,-100)'))
        result = inc.ladder_evidence(record, wrapper['metadata'], plan, [0, 1, 2, 3], 'a' * 64)
        self.assertTrue(result['accepted'], result)
        self.assertAlmostEqual(result['height_progress'][0]['observed_dz'], 174.4046)
        record['ladder_log'] = record['ladder_log'].replace('movetype=9', 'movetype=2')
        self.assertFalse(inc.ladder_evidence(record, wrapper['metadata'], plan, [0, 1, 2, 3], 'a' * 64)['accepted'])


if __name__ == '__main__':
    unittest.main()
