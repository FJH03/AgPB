"""Endpoint acceptance regressions, with no game or filesystem fixtures."""
import copy
import unittest

import stop_policy as policy

NODE = {"x": 0.0, "y": 0.0, "z": 0.0, "radius": 0}


def samples(position=None, flags=1, velocity=None, times=(0.0, 0.4, 0.8)):
    return [{"slot": 2, "userid": 22, "time": stamp, "flags": flags,
             "position": list(position or [3.0, 0.0, 3.0]), "velocity": list(velocity or [0.0, 0.0, 0.0])}
            for stamp in times]


class StopPolicyTests(unittest.TestCase):
    def evaluate(self, sequence, node=None, after=None):
        return policy.evaluate(node or NODE, sequence, sequence[-1] if after is None else after)

    def test_radius_zero_uses_separate_xy_floor_and_vertical_bound(self):
        result = self.evaluate(samples())
        self.assertTrue(result['precise_stop_passed'])
        self.assertEqual(0, result['raw_radius'])
        self.assertEqual(4, result['effective_radius'])
        self.assertEqual(3, result['xy_distance'])
        self.assertEqual(3, result['vertical_error'])
        self.assertGreater(result['stopped_distance'], 4)

    def test_radius_eight_does_not_accept_fifty_six_point_nine(self):
        result = self.evaluate(samples([56.9, 0, 0]), dict(NODE, radius=8))
        self.assertTrue(result['settled'])
        self.assertFalse(result['precise_stop_passed'])
        self.assertEqual('out_of_radius', result['stop_status'])

    def test_large_xy_radius_never_relaxes_height(self):
        result = self.evaluate(samples([3, 0, 4.01]), dict(NODE, radius=200))
        self.assertTrue(result['within_radius'])
        self.assertFalse(result['within_vertical'])
        self.assertFalse(result['precise_stop_passed'])

    def test_airborne_missing_flags_or_velocity_never_pass(self):
        for field, value in (('flags', 0), ('flags', None), ('velocity', None)):
            sequence = samples()
            for sample in sequence:
                sample[field] = value
            with self.subTest(field=field, value=value):
                self.assertFalse(self.evaluate(sequence)['precise_stop_passed'])
        self.assertTrue(self.evaluate(samples(flags=3))['precise_stop_passed'])

    def test_speed_duration_time_order_and_gap_are_enforced(self):
        for sequence in (samples(velocity=[5.01, 0, 0]), samples(times=(0, .4, .79)),
                         samples(times=(0, .4, .4)), samples(times=(.8, .4, 0)),
                         samples(times=(0, .4, 1.91))):
            with self.subTest(sequence=sequence):
                self.assertFalse(self.evaluate(sequence)['precise_stop_passed'])

    def test_failed_samples_reset_final_continuous_window(self):
        for field, value in (('flags', 0), ('position', [4.01, 0, 0]), ('position', [3, 0, 4.01])):
            sequence = samples(times=(0, .4, .8, 1.2, 1.6))
            sequence[2][field] = value
            with self.subTest(field=field, value=value):
                self.assertFalse(self.evaluate(sequence)['precise_stop_passed'])

    def test_old_good_window_cannot_hide_bad_last_sample(self):
        sequence = samples(times=(0, .4, .8, 1.2))
        sequence[-1]['flags'] = 0
        self.assertFalse(self.evaluate(sequence)['precise_stop_passed'])

    def test_bool_or_float_identity_fields_cannot_alias_integer_evidence(self):
        sequence = samples()
        for field, value in (("flags", True), ("slot", 2.0), ("userid", 22.0), ("time", True)):
            after = copy.deepcopy(sequence[-1])
            after[field] = value
            with self.subTest(field=field):
                self.assertFalse(self.evaluate(sequence, after=after)['precise_stop_passed'])

    def test_signed_high_bit_flags_use_ground_bit_not_sign(self):
        self.assertEqual(2147549570, policy.normalize_flags(-2147417726))
        self.assertFalse(self.evaluate(samples(flags=-2147417726))['precise_stop_passed'])
        self.assertTrue(self.evaluate(samples(flags=-2147417725))['precise_stop_passed'])
        for invalid in (True, 1.0, -(1 << 31) - 1, 1 << 32):
            with self.subTest(invalid=invalid):
                self.assertIsNone(policy.normalize_flags(invalid))
                self.assertFalse(self.evaluate(samples(flags=invalid))['precise_stop_passed'])

    def test_after_must_match_final_same_client_sample(self):
        sequence = samples()
        after = copy.deepcopy(sequence[-1])
        after['position'][0] = 3.1
        self.assertFalse(self.evaluate(sequence, after=after)['precise_stop_passed'])
        sequence[-1]['userid'] = 99
        self.assertFalse(self.evaluate(sequence)['precise_stop_passed'])


if __name__ == '__main__':
    unittest.main()
