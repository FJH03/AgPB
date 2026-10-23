"""Memory-only objective recorder tests. No game, process launch or deployment."""
import copy
import unittest
from unittest.mock import patch

from verify_objectives import ObjectiveVerifier, QUERY, parse
from verify_routes import CommandFailure

PLAN = {"map": "fixture_map", "nodes": [
    {"label": "goal", "x": 100, "y": 20, "z": 30, "radius": 24, "flags": 16},
    {"label": "escape", "x": 2000, "y": 20, "z": 30, "radius": 24}], "directed_edges": []}
META = {"mode": "T_escape", "site": "A", "human_slot": 1, "bot_slot": 2, "bot_index": 0}
SCENE = {"plant_target": "goal", "ct_escape_target": "escape", "drop_position": [-1200, -750, 132],
         "observer_position": [-700, -700, 134], "c4timer": 35}


class Clock:
    def __init__(self): self.now = 100.0
    def __call__(self): return self.now
    def sleep(self, value): self.now += value


class Sender:
    def __init__(self, clock, mode="T_escape", scenario="normal"):
        self.clock, self.mode, self.scenario = clock, mode, scenario
        self.log, self.commands = "", []
        self.team, self.round, self.owner, self.state = 2, 8, 1, 2
        self.human_alive = int(scenario != "human_dead_initially")
        self.bot = False
        self.events = {"planted": 0, "defused": 0, "exploded": 0}
        self.actors = {"planted": 0, "defused": 0, "exploded": 0}
        self.bomb = False
        self.ended = False
        self.goal = False
        self.auto_count = 0
        self.phase = None
        self.phase_queries = 0
        self.pos = "100 20 30"
        self.human_pos = "-700 -700 134"
        self.human_zone = 0

    def event(self, name, actor):
        self.events[name] += 1
        self.actors[name] = actor
        return (f"[WP_PROBE_EVENT] t={self.clock():.3f} map=fixture_map event=bomb_{name} userid={actor} "
                f"planted={self.events['planted']} defused={self.events['defused']} exploded={self.events['exploded']}\n")

    def players(self):
        stamp = self.clock()
        human_userid = 900 if self.ended and self.scenario == "human_replaced" else 101
        bot_userid = 901 if self.ended and self.scenario == "bot_replaced" else 102
        alive = 0 if self.ended and self.scenario == "bot_dead" else 1
        lines = f"[WP_PROBE_STATE] t={stamp:.3f} map=fixture_map slot=1 userid={human_userid} fake={int(self.scenario == "human_fake")} team={self.team} alive={self.human_alive} flags=1 pos=({self.human_pos}) vel=(0 0 0) zone={self.human_zone} defusing=0\n"
        if self.scenario == "human_missing": lines = ""
        if self.bot:
            lines += f"[WP_PROBE_STATE] t={stamp:.3f} map=fixture_map slot=2 userid={bot_userid} fake=1 team={self.team} alive={alive} flags=1 pos=({self.pos}) vel=(0 0 0) zone=1 defusing={int(self.phase == 'defuse' and not self.ended)}\n"
        return lines

    def bot_list(self):
        return f"  [0] Test Bot slot=2 team={self.team} want={self.team} hp=100 wpn=knife\n" if self.bot else "[AgPB] 0 bot(s)\n"

    def query(self):
        prefix = ""
        if self.phase:
            self.phase_queries += 1
            if self.phase_queries >= (3 if self.mode == "T_escape" else 2) and not self.ended:
                key = "defused" if self.phase == "defuse" else "exploded"
                prefix += self.event(key, 102)
                self.ended = True
                if key == "exploded": self.pos = "2000 20 30"
                if self.scenario == "round_changed": self.round += 1
        stamp = self.clock()
        ev = self.events
        events = (f"[WP_PROBE_BOMB_EVENTS] t={stamp:.3f} map=fixture_map planted={ev['planted']} planted_userid={self.actors['planted']} "
                  f"defused={ev['defused']} defused_userid={self.actors['defused']} exploded={ev['exploded']} exploded_userid={self.actors['exploded']}\n")
        if self.bomb:
            bomb_position = "100.00 20.00 30.60" if self.ended and self.scenario == "explosion_origin_shift" else "100.00 20.00 30.00"
            bomb = f"[WP_PROBE_BOMB] t={stamp:.3f} map=fixture_map ent={75 if self.scenario == 'bomb_replaced' and self.ended else 70} ticking={int(not self.ended)} defused=-1 blow=135.000 pos=({bomb_position})\n"
        else:
            bomb_position = "100.00 20.00 30.60" if self.ended and self.scenario == "explosion_origin_shift" else "100.00 20.00 30.00"
            bomb = f"[WP_PROBE_BOMB] t={stamp:.3f} map=fixture_map none\n"
        if self.mode == "CT_escape" and self.bomb and not self.phase:
            # The test clock is advanced to the same <=12s trigger, not a new threshold.
            self.clock.now = max(self.clock.now, 124)
        objective = (f"[AgPB] objectives: map-flags=1 round={self.round} sites=2 rescues=0 hostages=0 bomb-state={self.state} bomb-ent=70 owner={self.owner} "
                     "pos=known (100.0 20.0 30.0) blow-time=135.00 bomb-radius=1000.0 radius-source=map planted-observed=1\n")
        task = "[AgPB] Test Bot:   active escape_bomb wp=1 priority=100 deadline=0 token=3 entity=70\n" if self.phase == "escape" else ""
        return prefix + self.bot_list() + self.players() + events + bomb + objective + task

    def send(self, command):
        self.commands.append(command)
        self.clock.sleep(.05)
        if self.scenario == "transport_failure" and command == QUERY:
            raise CommandFailure("transport fault", {"command": command, "log": "partial receipt", "stdout": "PARTIAL"})
        log = ""
        if command == QUERY:
            log = self.query()
        elif command == "agpb_list; agpb_wp_probe_state":
            log = self.bot_list() + self.players()
        else:
            for part in command.split("; "):
                if part == "agpb_kick all" or part == "agpb_kick 0": self.bot = False
                elif part.startswith("agpb_reg_human_team"):
                    self.team = 3 if part.endswith("CT") else 2
                    self.human_alive = 1
                elif part == "mp_restartgame 1": self.round += 1; self.owner = 1; self.state = 2
                elif part == "agpb_wp_list 0":
                    log += "[AgPB] map=fixture_map file=fixture_map.agpw\n[AgPB] 2 waypoint(s), 0 link(s)\n  [0] 100 20 30 flags=0x10 r=24 (no links)\n  [1] 2000 20 30 flags=0x0 r=24 (no links)\n"
                elif part == "mp_c4timer": log += '"mp_c4timer" = "45" ( def. "35" )\n'
                elif part.startswith("agpb_add"): self.bot = True
                elif part.startswith("agpb_reg_place_human"):
                    self.human_pos = " ".join(part.split()[2:5])
                    self.human_zone = int(float(part.split()[2]) == 100)
                elif part.startswith("agpb_reg_drop_c4"): self.state = 3; self.owner = -1
                elif part.startswith("agpb_bot_goto"):
                    index = part.split()[-1]
                    self.goal = True
                    log += f"[AgPB] Test Bot: walking to #{index}, 1 hop(s): {index}\n[AgPB] Test Bot: arrived at waypoint #{index} (0 hop(s) walked)\n"
                elif part.startswith("agpb_reg_human_plant"):
                    self.bomb = True; self.state = 4; self.owner = -1
                    log += self.event("planted", 101)
                elif part == "agpb_bot_auto 0 1":
                    if self.team == 2 and not self.goal: self.state = 2; self.owner = 2
                    else:
                        if self.team == 2:
                            self.bomb = True; self.state = 4; self.owner = -1
                            if self.scenario != "stale_plant": log += self.event("planted", 102)
                            else: self.events["planted"] += 1; self.actors["planted"] = 102
                        self.phase = "defuse" if self.mode == "CT_defuse" else "escape"
                        if self.phase == "escape":
                            bot_name = "Other Bot" if self.scenario == "other_safe" else "Test Bot"
                            log += f"[AgPB] {bot_name}: escape plan: left=30 safe=yes defuse-eta=-1\n"
        self.log += log
        return {"command": command, "log": log, "stdout": log, "end_offset": len(self.log)}

    def cursor(self): return len(self.log)
    def tail(self, offset): return self.log[offset:]


def run(mode="T_escape", scenario="normal"):
    clock = Clock()
    sender = Sender(clock, mode, scenario)
    metadata = dict(META, mode=mode)
    saved = []
    verifier = ObjectiveVerifier(copy.deepcopy(PLAN), metadata, copy.deepcopy(SCENE), sender,
                                 save=lambda result: saved.append(copy.deepcopy(result)), clock=clock, sleep=clock.sleep)
    return verifier.run(), sender, saved


class ObjectiveTests(unittest.TestCase):
    def test_all_modes_need_real_events_and_preserve_scene_and_cleanup(self):
        for mode in ("T_escape", "CT_escape", "CT_defuse"):
            with self.subTest(mode=mode):
                result, sender, saved = run(mode)
                self.assertEqual("passed", result["status"], result.get("error"))
                self.assertEqual(101, result["metadata"]["human_userid"])
                self.assertEqual(102, result["metadata"]["bot_userid"])
                self.assertIn("planted", result["event_evidence"])
                self.assertIn("defused" if mode == "CT_defuse" else "exploded", result["event_evidence"])
                self.assertIn("mp_c4timer 45", sender.commands)
                self.assertIn("agpb_kick 0", sender.commands)
                self.assertEqual("passed", saved[-1]["status"])

    def test_engine_can_shift_bomb_origin_after_real_explosion(self):
        result, _, _ = run(scenario="explosion_origin_shift")
        self.assertEqual("passed", result["status"], result.get("error"))

    def test_dead_initial_human_identity_is_captured_before_fixture_respawn(self):
        result, sender, _ = run(scenario="human_dead_initially")
        self.assertEqual("passed", result["status"], result.get("error"))
        self.assertEqual(0, result["stages"]["no_bots"]["players"][1]["alive"])
        self.assertEqual(101, result["metadata"]["human_userid"])
        self.assertEqual(1, result["stages"]["human_T_no_bots"]["players"][1]["alive"])
        self.assertIn("agpb_reg_human_team 1 T", sender.commands)

    def test_missing_or_fake_human_rejects_before_fixture_respawn(self):
        for scenario in ("human_missing", "human_fake"):
            with self.subTest(scenario=scenario):
                result, sender, _ = run(scenario=scenario)
                self.assertEqual("failed", result["status"])
                self.assertFalse(any(command.startswith("agpb_reg_human_team") for command in sender.commands))

    def test_far_bomb_cannot_pass_even_with_correct_goal_planter_and_real_events(self):
        original = Sender.query
        def far_bomb(sender):
            return original(sender).replace("pos=(100.00 20.00 30.00)", "pos=(9000.00 9000.00 30.00)")
        with patch.object(Sender, "query", far_bomb):
            result, _, _ = run("CT_defuse")
        self.assertEqual("failed", result["status"])
        self.assertIn("requested GOAL", result["error"])
        self.assertFalse(result["site_identity"]["passed"])

    def test_far_planter_preplant_cannot_be_replaced_by_correct_bomb_position(self):
        original = Sender.players
        def far_planter(sender):
            return original(sender).replace("pos=(100 20 30)", "pos=(9000 9000 30)")
        with patch.object(Sender, "players", far_planter):
            result, _, _ = run()
        self.assertEqual("failed", result["status"])
        self.assertIn("requested GOAL", result["error"])

    def test_postplant_zone_zero_and_escape_motion_preserve_preplant_zone_evidence(self):
        original = Sender.players
        def escaped_planter(sender):
            log = original(sender)
            if sender.bomb:
                log = log.replace("pos=(100 20 30)", "pos=(1400 20 30)").replace("zone=1", "zone=0")
            return log
        with patch.object(Sender, "players", escaped_planter):
            result, _, _ = run()
        self.assertEqual("passed", result["status"], result.get("error"))
        evidence = result["site_identity"]
        self.assertEqual(1, evidence["planter"]["zone"])
        self.assertEqual(0, evidence["postplant_planter"]["zone"])
        self.assertEqual(1400, evidence["postplant_planter"]["position"][0])

    def test_native_downward_c4_projection_is_distinct_from_goal_stop_radius(self):
        original = Sender.query
        def projected_bomb(sender):
            return original(sender).replace("pos=(100.00 20.00 30.00)", "pos=(100.00 20.00 -150.00)").replace("pos=known (100.0 20.0 30.0)", "pos=known (100.0 20.0 -150.0)")
        with patch.object(Sender, "query", projected_bomb):
            result, _, _ = run()
        self.assertEqual("passed", result["status"], result.get("error"))
        self.assertEqual(180, result["site_identity"]["measurements"]["bomb_goal_z"])
        self.assertEqual("planted_site_identity_only", result["site_identity"]["scope"])

    def slow_wait(self, game_rate, predicate, game_time):
        wall = Clock()
        started = wall()
        verifier = ObjectiveVerifier(copy.deepcopy(PLAN), dict(META), dict(SCENE), None,
                                     clock=wall, sleep=wall.sleep)
        verifier.previous = {"time": 100.0}
        def snapshot(*args):
            wall.sleep(1.0)
            return {"time": 100.0 + (wall() - started) * game_rate}
        with patch.object(verifier, "snap", snapshot):
            outcome = verifier.wait("defused", predicate, 15, "CT", game_time=game_time,
                                    wall_timeout=60 if game_time else None)
        return outcome, verifier, wall() - started

    def test_slow_simulation_retains_fifteen_game_seconds_for_real_defuse(self):
        # A ten-game-second defuse takes forty wall seconds at 0.25 simulation speed.
        with self.assertRaisesRegex(RuntimeError, "defused timed out"):
            self.slow_wait(.25, lambda data: data["time"] >= 110, game_time=False)
        outcome, verifier, elapsed = self.slow_wait(.25, lambda data: data["time"] >= 110, game_time=True)
        self.assertGreater(elapsed, 15)
        self.assertLessEqual(outcome["time"] - 100, 15)
        self.assertEqual("passed", verifier.result["wait_timing"]["defused"]["status"])

    def test_frozen_game_time_still_has_a_bounded_wall_stall_cap(self):
        with self.assertRaisesRegex(RuntimeError, "wall stall cap"):
            self.slow_wait(0, lambda data: False, game_time=True)

    def test_game_time_confirmation_window_does_not_expand_to_wall_cap(self):
        with self.assertRaisesRegex(RuntimeError, "15.0 game seconds"):
            self.slow_wait(.5, lambda data: data["time"] >= 116, game_time=True)

    def test_failed_transport_receipt_is_saved(self):
        result, _, _ = run(scenario="transport_failure")
        self.assertEqual("failed", result["status"])
        self.assertTrue(any(receipt.get("stdout") == "PARTIAL" for receipt in result["receipts"]))

    def test_counter_alone_cannot_establish_real_plant(self):
        result, _, _ = run(scenario="stale_plant")
        self.assertEqual("failed", result["status"])
        self.assertIn("fresh WP_PROBE_EVENT", result["error"])

    def test_dead_or_replaced_players_and_new_round_never_pass_as_survival(self):
        for scenario in ("bot_dead", "bot_replaced", "human_replaced", "round_changed", "bomb_replaced"):
            with self.subTest(scenario=scenario):
                result, _, _ = run(scenario=scenario)
                self.assertEqual("failed", result["status"])
                self.assertNotIn("exploded_alive", result["stages"])

    def test_another_bots_safe_plan_does_not_count(self):
        result, _, _ = run(scenario="other_safe")
        self.assertEqual("failed", result["status"])
        self.assertIn("same-bot", result["error"])

    def test_parse_requires_one_current_complete_snapshot(self):
        sender = Sender(Clock())
        log = sender.query()
        self.assertEqual(8, parse(log, "fixture_map")["round"])
        for bad in (log + log, log.replace("map=fixture_map", "map=other"), log.replace("[WP_PROBE_BOMB_EVENTS]", "missing")):
            with self.assertRaises(ValueError): parse(bad, "fixture_map")

    def test_same_userids_are_enforced_before_any_success(self):
        result, _, _ = run(scenario="bot_replaced")
        self.assertIn("identity changed", result["error"])


if __name__ == "__main__":
    unittest.main()
