"""Memory-only route-verifier tests: no launcher, game process or deployment."""
import copy
import json
from pathlib import Path
import queue
import subprocess
import struct
import unittest
from unittest.mock import patch

from verify_routes import CommandFailure, ConsoleSender, RouteVerifier, disk_evidence, states, validate_waypoints
from export_manual_plan import HEADER, export, jump_velocity


PLAN = {"map": "fixture_map", "nodes": [{"label": "goal", "x": 10.0, "y": 20.0, "z": 30.0, "radius": 24, "flags": 0}], "directed_edges": []}
META = {"plan_sha256": "0" * 64, "plan_version": 1, "map": "fixture_map", "plan_file": "memory.json",
        "team": "T", "human_slot": 4, "bot_slot": 8, "bot_index": 2}


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakeSender:
    def __init__(self, clock, scenario="arrived", human_team=2, bot_team=2, bot_slot=8, node_radius=24, endpoint_position=None):
        self.clock, self.scenario = clock, scenario
        self.node_radius, self.endpoint_position = node_radius, endpoint_position
        self.human_team, self.bot_team, self.bot_slot = human_team, bot_team, bot_slot
        self.commands = []
        self.stopped = False
        self.tail_log = ""
        self.interrupted = False
        self.navigating = False

    def state(self):
        velocity = "20 0 0" if self.stopped and self.scenario == "unsettled" else "0 0 0"
        lines = []
        stamp = 1.0 if self.stopped and self.scenario == "frozen_time" else self.clock()
        for slot, fake, team, speed in ((4, 0, self.human_team, "0 0 0"), (8, 1, self.bot_team, velocity)):
            position = "50 20 30" if fake and self.stopped and self.scenario == "out_of_radius" else "10 20 30"
            if fake and self.stopped and self.endpoint_position is not None:
                position = " ".join(map(str, self.endpoint_position))
            flags = (-2147417725 if self.stopped else -2147417726) if fake and self.scenario == "signed_flags" else 1
            lines.append(f"[WP_PROBE_STATE] t={stamp:.3f} map=fixture_map slot={slot} userid={slot+100} fake={fake} team={team} alive=1 flags={flags} pos=({position}) vel=({speed}) zone=1 defusing=0\n")
        if self.scenario == "enemy_bot" or (self.scenario == "enemy_after_goto" and self.navigating):
            if fake and self.stopped and self.endpoint_position is not None:
                position = " ".join(map(str, self.endpoint_position))
            flags = (-2147417725 if self.stopped else -2147417726) if fake and self.scenario == "signed_flags" else 1
            lines.append(f"[WP_PROBE_STATE] t={stamp:.3f} map=fixture_map slot=12 userid=112 fake=1 team=3 alive=1 flags=1 pos=(0 0 0) vel=(0 0 0) zone=0 defusing=0\n")
        return "".join(lines)

    def send(self, command):
        self.commands.append(command)
        self.clock.sleep(0.25)
        if command.startswith("agpb_list;"):
            log = (f"[AgPB] 1 bot(s), IBotManager=ok, helpers=ok\n"
                   f"  [2] Test Bot slot={self.bot_slot} team={self.bot_team} want={self.bot_team} hp=100 wpn=knife pos=10,20,30\n"
                   "[AgPB] map=fixture_map file=fixture_map.agpw\n"
                   "[AgPB] 1 waypoint(s), 0 link(s) | ready\n"
                   f"  [  0] 10 20 30  flags=0x0  r={self.node_radius}  (no links)\n" + self.state())
        elif command == "agpb_wp_probe_state":
            log = self.state()
        elif command.startswith("agpb_bot_goto "):
            self.navigating = True
            walk = "[AgPB] Test Bot: walking to #0, 1 hop(s): 0\n"
            arrived = "[AgPB] Test Bot: arrived at waypoint #0 (0 hop(s) walked)\n"
            if self.scenario == "failed":
                log = "[AgPB] Test Bot: task move_to_position failed: no walkable route entry\n"
            elif self.scenario == "stale":
                log = arrived + walk
            elif self.scenario == "wrong_bot":
                log = walk + "[AgPB] Other Bot: arrived at waypoint #0 (1 hop(s) walked)\n"
            elif self.scenario in ("timeout", "interrupted", "enemy_after_goto"):
                log = walk
            else:
                log = "complete evidence " * 600 + "\n" + walk + arrived
        elif command == "agpb_bot_stop 2":
            self.stopped = self.scenario != "rejected_stop"
            log = "Unknown command agpb_bot_stop\n" if self.scenario == "rejected_stop" else "[AgPB] Test Bot: route stopped (roam off)\n"
        else:
            raise AssertionError("Unexpected command: " + command)
        self.tail_log += log
        return {"command": command, "stdout": log, "stderr": "", "log": log, "end_offset": len(self.tail_log)}

    def cursor(self):
        return len(self.tail_log)

    def tail(self, offset):
        if self.scenario == "interrupted" and not self.interrupted:
            self.interrupted = True
            raise KeyboardInterrupt
        return self.tail_log[offset:]


def run(scenario="arrived", require_precise_stop=False, **kwargs):
    clock = Clock()
    sender = FakeSender(clock, scenario, **kwargs)
    saved = []
    plan = copy.deepcopy(PLAN)
    plan['nodes'][0]['radius'] = sender.node_radius
    verifier = RouteVerifier(plan, copy.deepcopy(META), sender, timeout=2,
                             clock=clock, sleep=clock.sleep, save=lambda result: saved.append(copy.deepcopy(result)), require_precise_stop=require_precise_stop)
    return verifier.run([("goal", 0)]), sender, saved


class RouteTests(unittest.TestCase):
    def test_same_team_t_and_nondefault_slots_complete_with_full_logs(self):
        result, sender, saved = run()
        self.assertEqual(result["status"], "passed")
        record = result["records"][0]
        self.assertEqual(record["status"], "arrived")
        self.assertTrue(record["settled"])
        self.assertGreaterEqual(record["settled_seconds"], 0.8)
        self.assertGreaterEqual(len(record["samples"]), 4)
        self.assertGreater(len(record["log"]), 6000)
        self.assertIn("complete evidence", record["log"])
        self.assertEqual(record["route"], "0")
        self.assertEqual(record["stopped_distance"], 0)
        self.assertEqual(record["after"]["zone"], 1)
        self.assertEqual(record["after"]["defusing"], 0)
        self.assertIn("agpb_bot_stop 2", sender.commands)
        self.assertEqual(saved[-1]["status"], "passed")

    def test_human_on_different_team_refuses_before_navigation(self):
        result, sender, _ = run(human_team=3)
        self.assertEqual(result["status"], "failed")
        self.assertIn("requested team", result["error"])
        self.assertFalse(any(command.startswith("agpb_bot_goto") for command in sender.commands))

    def test_uncontrolled_enemy_bot_refuses_before_navigation(self):
        result, sender, _ = run("enemy_bot")
        self.assertEqual(result["status"], "failed")
        self.assertIn("Every active-team bot", result["error"])
        self.assertFalse(any(command.startswith("agpb_bot_goto") for command in sender.commands))

    def test_enemy_bot_appearing_during_walk_stops_controlled_bot(self):
        result, sender, _ = run("enemy_after_goto")
        self.assertEqual(result["status"], "failed")
        self.assertIn("Every active-team bot", result["error"])
        self.assertIn("agpb_bot_stop 2", sender.commands)

    def test_bot_index_must_map_to_requested_slot(self):
        result, sender, _ = run(bot_slot=9)
        self.assertEqual(result["status"], "failed")
        self.assertIn("map bot-index", result["error"])
        self.assertEqual(len(sender.commands), 1)

    def test_arrival_before_new_walking_is_stale(self):
        result, sender, _ = run("stale")
        self.assertEqual(result["records"][0]["status"], "timeout")
        self.assertEqual(result["status"], "failed")
        self.assertIn("agpb_bot_stop 2", sender.commands)

    def test_other_bot_arrival_does_not_complete(self):
        result, sender, _ = run("wrong_bot")
        self.assertEqual(result["records"][0]["status"], "timeout")
        self.assertIn("agpb_bot_stop 2", sender.commands)

    def test_navigation_failure_is_recorded_and_stopped(self):
        result, sender, _ = run("failed")
        self.assertEqual(result["records"][0]["status"], "failed")
        self.assertIn("failed: no walkable", result["records"][0]["log"])
        self.assertIn("agpb_bot_stop 2", sender.commands)

    def test_timeout_stops_bot_and_retains_receipts(self):
        result, sender, _ = run("timeout")
        record = result["records"][0]
        self.assertEqual(record["status"], "timeout")
        self.assertTrue(record["settled"])
        self.assertIn("agpb_bot_stop 2", [receipt["command"] for receipt in record["command_receipts"]])
        self.assertGreater(len(record["samples"]), 3)

    def test_arrival_preserves_traversal_when_stop_is_unsettled(self):
        result, sender, _ = run("unsettled")
        record = result["records"][0]
        self.assertEqual(record["status"], "arrived")
        self.assertEqual(record["stop_status"], "unsettled")
        self.assertFalse(record["settled"])
        self.assertEqual(record["stopped_distance"], 0)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["precise_stop_summary"]["status"], "not_passed")
        self.assertIn("agpb_bot_stop 2", sender.commands)

    def test_explicit_precise_stop_requirement_fails_on_unsettled(self):
        result, _, _ = run("unsettled", require_precise_stop=True)
        self.assertEqual(result["records"][0]["status"], "arrived")
        self.assertEqual(result["records"][0]["stop_status"], "unsettled")
        self.assertEqual(result["status"], "failed")

    def test_outside_radius_preserves_arrived_with_separate_stop_failure(self):
        result, _, _ = run("out_of_radius")
        record = result["records"][0]
        self.assertEqual(record["status"], "arrived")
        self.assertEqual(record["stop_status"], "out_of_radius")
        self.assertTrue(record["settled"])
        self.assertFalse(record["within_radius"])
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["precise_stop_summary"]["status"], "not_passed")

    def test_repeated_frozen_samples_do_not_prove_sustained_stop(self):
        result, _, _ = run("frozen_time")
        self.assertEqual(result["records"][0]["stop_status"], "unsettled")
        self.assertFalse(result["records"][0]["settled"])

    def test_radius_zero_uses_effective_xy_tolerance_without_modifying_plan(self):
        result, _, _ = run(node_radius=0, endpoint_position=[13, 20, 33], require_precise_stop=True)
        record = result['records'][0]
        self.assertEqual('passed', result['status'])
        self.assertEqual(0, record['raw_radius'])
        self.assertEqual(0, record['target_radius'])
        self.assertEqual(4, record['effective_radius'])
        self.assertGreater(record['stopped_distance'], 4)
        self.assertTrue(record['precise_stop_passed'])
        self.assertEqual('sampled_endpoint_only', record['stop_scope'])

    def test_large_radius_does_not_allow_different_floor(self):
        result, _, _ = run(node_radius=200, endpoint_position=[10, 20, 35], require_precise_stop=True)
        record = result['records'][0]
        self.assertEqual('arrived', record['status'])
        self.assertEqual('failed', result['status'])
        self.assertTrue(record['within_radius'])
        self.assertFalse(record['within_vertical'])
        self.assertEqual(5, record['vertical_error'])

    def test_signed_high_bit_jump_flags_preserve_slot_and_bot_stop_identity(self):
        result, sender, _ = run("signed_flags", require_precise_stop=True)
        record = result['records'][0]
        self.assertEqual('passed', result['status'])
        self.assertEqual(2147549570, record['before']['flags'])
        self.assertEqual(0, record['before']['flags'] & 1)
        self.assertEqual(2147549571, record['after']['flags'])
        self.assertEqual(1, record['after']['flags'] & 1)
        self.assertTrue(record['precise_stop_passed'])
        self.assertIn('agpb_bot_stop 2', sender.commands)

    def test_cfg_ack_without_agpb_stop_success_cannot_pass(self):
        result, sender, _ = run("rejected_stop")
        self.assertEqual(result["status"], "failed")
        self.assertFalse(sender.stopped)
        self.assertIn("did not acknowledge stopping", result["error"])
        self.assertIn("stop_error", result["records"][0])
        self.assertFalse(result["records"][0]["settled"])

    def test_ctrl_c_attempts_stop_and_saves_failed_record(self):
        result, sender, saved = run("interrupted")
        self.assertEqual(result["status"], "interrupted")
        self.assertEqual(result["records"][0]["status"], "interrupted")
        self.assertIn("agpb_bot_stop 2", sender.commands)
        self.assertEqual(saved[-1]["records"][0]["status"], "interrupted")


class JumpVelocityBindingTests(unittest.TestCase):
    def plan(self, velocity=None):
        plan = copy.deepcopy(PLAN)
        plan['nodes'].append(dict(label='other', x=40, y=20, z=30, radius=24, flags=0))
        plan['directed_edges'] = [dict(**{'from':'goal', 'to':'other'}, flags=1),
                                  dict(**{'from':'other', 'to':'goal'}, flags=0)]
        if velocity is not None:
            plan['directed_edges'][0]['jumpVelocity'] = velocity
        return plan

    def log(self, first='', second=''):
        return ('[AgPB] map=fixture_map file=fixture_map.agpw\n'
                '[AgPB] 2 waypoint(s), 2 link(s) | ready\n'
                '  [  0] 10 20 30 flags=0x0 r=24 1/0x1' + first + '\n'
                '  [  1] 40 20 30 flags=0x0 r=24 0/0x0' + second + '\n')

    def test_old_zero_logs_and_explicit_zero_vectors_are_compatible(self):
        validate_waypoints(self.log(), self.plan())
        validate_waypoints(self.log(' velocity=(0.000000,0.000000,0.000000)',
                                    ' velocity=(0,0,0)'), self.plan([0,0,0]))

    def test_nonzero_vector_requires_matching_target_and_float32_console_rounding(self):
        velocity = [1000.1234567, -222.333333, 300.123456]
        actual = jump_velocity(velocity)
        text = ' velocity=(' + ','.join('%.6f' % value for value in actual) + ')'
        validate_waypoints(self.log(text), self.plan(velocity))
        for log in (self.log(), self.log('', text), self.log(' velocity=(1,2,3)')):
            with self.assertRaisesRegex(ValueError, 'jumpVelocity'):
                validate_waypoints(log, self.plan(velocity))

    def test_invalid_duplicate_or_nonzero_unexpected_vectors_are_rejected(self):
        for value in ('(1,0,0)', '(nan,0,0)', '(inf,0,0)', '(0,0)', '(0,0,0,0)', '(2001,0,0)', ''):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, 'jumpVelocity'):
                    validate_waypoints(self.log(' velocity=' + value), self.plan())
        with self.assertRaisesRegex(ValueError, 'jumpVelocity'):
            validate_waypoints(self.log(' velocity=(0,0,0) velocity=(0,0,0)'), self.plan())
        with self.assertRaisesRegex(ValueError, 'links differ'):
            validate_waypoints(self.log(' 1/0x1'), self.plan())

    def disk(self, plan, actual):
        with patch('verify_routes.Path.read_text', return_value='- 部署目标： `E:\\fixture_game`\n'), \
                patch('verify_routes.Path.read_bytes', return_value=actual), \
                patch('verify_routes.Path.is_file', return_value=False):
            return disk_evidence(plan, export(plan))

    def test_disk_normalization_allows_version_and_author_only(self):
        plan = self.plan()
        current = copy.deepcopy(plan)
        current.update(agpw_version=2, author='native save')
        result = self.disk(plan, export(current))
        self.assertEqual(result['agpw_version'], 2)
        self.assertTrue(result['agpw_author_differs'])
        for offset, fmt, value in ((HEADER.size + 16, '<B', 23),
                                   (HEADER.size + 17, '<B', 7),
                                   (HEADER.size + 48, '<H', 3),
                                   (HEADER.size + 50, '<f', 0.5)):
            altered = bytearray(export(current))
            struct.pack_into(fmt, altered, offset, value)
            with self.assertRaisesRegex(ValueError, 'data/map differs'):
                self.disk(plan, altered)

    def test_disk_cannot_certify_missing_or_changed_jump_vector(self):
        plan = self.plan([100, -20, 300])
        with self.assertRaisesRegex(ValueError, 'data/map differs'):
            self.disk(plan, export(self.plan()))
        changed = self.plan([101, -20, 300])
        with self.assertRaisesRegex(ValueError, 'data/map differs'):
            self.disk(plan, export(changed))


class MemoryReadPipe:
    def __init__(self):
        self.lines = queue.Queue()
        self.closed = False

    def readline(self):
        return self.lines.get()

    def emit(self, text):
        for line in text.splitlines(keepends=True):
            self.lines.put(line)

    def close(self):
        self.closed = True
        self.lines.put("")


class MemoryWritePipe:
    def __init__(self, process):
        self.process = process
        self.pending = ""
        self.closed = False

    def write(self, text):
        if self.closed:
            raise ValueError("Pipe is closed")
        self.pending += text
        return len(text)

    def flush(self):
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            self.process.accept(line)

    def close(self):
        self.closed = True


class MemoryHelper:
    """A fake process with real pipe reader threading, but no OS subprocess."""
    def __init__(self, responses):
        self.responses = list(responses)
        self.stdout = MemoryReadPipe()
        self.stdin = MemoryWritePipe(self)
        self.requests = []
        self.returncode = None
        self.terminated = False
        self.killed = False
        self.stdout.emit("AGPB_NATIVE_SESSION_READY: JSON lines; END closes this helper only.\n")

    def accept(self, line):
        self.requests.append(line)
        if line == "END":
            self.returncode = 0
            self.stdout.lines.put("")
            return
        response = self.responses.pop(0)
        if response == "EOF":
            self.returncode = 2
            self.stdout.lines.put("")
        elif response is not None:
            self.stdout.emit(response)

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        if self.returncode is None:
            raise subprocess.TimeoutExpired("fake helper", timeout)
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = -15
        self.stdout.lines.put("")

    def kill(self):
        self.killed = True
        self.returncode = -9
        self.stdout.lines.put("")


def listener_frame(char, message="probe state"):
    nonce = char * 32
    begin, end = "AGPB_SESSION_BEGIN_" + nonce, "AGPB_SESSION_END_" + nonce
    ack = {"nonce": nonce, "beginMarker": begin, "endMarker": end, "endByteOffset": 100, "logResetCount": 0}
    return begin + "\n" + message + "\n" + end + "\nCONSOLE_ACK: " + json.dumps(ack) + "\nAGPB_NATIVE_SESSION_READY\n"


class ListenerTests(unittest.TestCase):
    def sender(self):
        return ConsoleSender("fake-powershell", "agpb_wp_memory", Path("memory-session"))

    def test_one_helper_frames_multiple_requests_and_end_closes_only_helper(self):
        helper = MemoryHelper([listener_frame("a", "first reply"), listener_frame("b", "second reply")])
        sender = self.sender()
        with patch("verify_routes.subprocess.Popen", return_value=helper) as popen:
            try:
                first = sender.send("agpb_list")
                second = sender.send("agpb_wp_probe_state")
                self.assertEqual(popen.call_count, 1)
                self.assertIn("-Listen", popen.call_args.args[0])
                self.assertIn("first reply", first["log"])
                self.assertNotIn("second reply", first["stdout"])
                self.assertIn("second reply", second["log"])
                self.assertEqual(first["ack"]["nonce"], "a" * 32)
                self.assertEqual(second["ack"]["nonce"], "b" * 32)
                self.assertEqual(json.loads(helper.requests[1])["commands"], "agpb_wp_probe_state")
            finally:
                sender.close()
        self.assertEqual(helper.requests[-1], "END")
        self.assertFalse(helper.terminated)
        self.assertFalse(helper.killed)
        self.assertTrue(helper.stdin.closed)
        self.assertTrue(helper.stdout.closed)

    def test_error_frame_is_nonzero_and_next_complete_frame_remains_usable(self):
        helper = MemoryHelper(["AGPB_NATIVE_SESSION_ERROR: rejected request\nAGPB_NATIVE_SESSION_READY\n", listener_frame("c")])
        sender = self.sender()
        with patch("verify_routes.subprocess.Popen", return_value=helper) as popen:
            try:
                with self.assertRaises(CommandFailure) as caught:
                    sender.send("agpb_list")
                self.assertEqual(caught.exception.receipt["returncode"], 1)
                self.assertIn("rejected request", caught.exception.receipt["stdout"])
                self.assertEqual(sender.send("agpb_wp_probe_state")["returncode"], 0)
                self.assertEqual(popen.call_count, 1)
            finally:
                sender.close()

    def test_ack_without_ready_times_out_and_partial_frame_cannot_pollute_retry(self):
        incomplete = listener_frame("d").replace("AGPB_NATIVE_SESSION_READY\n", "")
        first, second = MemoryHelper([incomplete]), MemoryHelper([listener_frame("e")])
        sender = self.sender()
        sender.frame_timeout = 0.03
        with patch("verify_routes.subprocess.Popen", side_effect=[first, second]) as popen:
            try:
                with self.assertRaises(CommandFailure) as caught:
                    sender.send("agpb_list")
                self.assertIn("timed out before READY", str(caught.exception))
                self.assertIn("CONSOLE_ACK", caught.exception.receipt["stdout"])
                self.assertTrue(first.terminated)
                self.assertEqual(sender.send("agpb_wp_probe_state")["ack"]["nonce"], "e" * 32)
                self.assertEqual(popen.call_count, 2)
            finally:
                sender.close()

    def test_unexpected_eof_is_failure_and_releases_helper(self):
        helper = MemoryHelper(["EOF"])
        sender = self.sender()
        with patch("verify_routes.subprocess.Popen", return_value=helper):
            with self.assertRaises(CommandFailure) as caught:
                sender.send("agpb_list")
        self.assertIn("exited before READY", str(caught.exception))
        self.assertIsNone(sender.process)
        sender.close()

    def test_startup_failure_retains_attempted_command_and_startup_output(self):
        helper = MemoryHelper([])
        helper.stdout.lines.get_nowait()
        helper.stdout.emit("startup diagnostic\n")
        helper.stdout.lines.put("")
        helper.returncode = 2
        sender = self.sender()
        with patch("verify_routes.subprocess.Popen", return_value=helper):
            with self.assertRaises(CommandFailure) as caught:
                sender.send("agpb_list")
        self.assertEqual(caught.exception.receipt["command"], "agpb_list")
        self.assertIn("startup diagnostic", caught.exception.receipt["helper_startup_stdout"])
        self.assertFalse(sender.request_lock.locked())
        sender.close()

    def test_concurrent_request_is_rejected_before_starting_a_helper(self):
        sender = self.sender()
        sender.request_lock.acquire()
        try:
            with patch("verify_routes.subprocess.Popen") as popen:
                with self.assertRaises(CommandFailure):
                    sender.send("agpb_list")
                popen.assert_not_called()
        finally:
            sender.request_lock.release()
            sender.close()


if __name__ == "__main__":
    unittest.main()
