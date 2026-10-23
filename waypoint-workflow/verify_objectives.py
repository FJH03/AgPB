#!/usr/bin/env python3
"""Run real bomb-event regressions in an already running, cheat-enabled game.

Requires the manually loaded read-only probe and human-only behavior fixture.
Scene preparation moves/respawns only the human and repositions a normally dropped
C4. Bot actions use AgPB commands. No deployment, server launch or file deletion.
All run artifacts must be under TEMP; use a distinct output for every attempt.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import shutil
import time

from export_manual_plan import export
from verify_routes import (BOT, TEAM, CommandFailure, ConsoleSender, disk_evidence,
                           sha256, states, temp_child, validate_pair, validate_waypoints)

QUERY = "agpb_list; agpb_wp_probe_state; agpb_wp_probe_bomb; agpb_objectives; agpb_bot_tasks"
EVENTS = re.compile(r"\[WP_PROBE_BOMB_EVENTS\] t=([\d.]+) map=(\S+) planted=(\d+) planted_userid=(\d+) defused=(\d+) defused_userid=(\d+) exploded=(\d+) exploded_userid=(\d+)")
BOMB = re.compile(r"\[WP_PROBE_BOMB\] t=([\d.]+) map=(\S+) ent=(\d+) ticking=(-?\d+) defused=(-?\d+) blow=(-?[\d.]+) pos=\(([^)]+)\)")
NONE = re.compile(r"\[WP_PROBE_BOMB\] t=([\d.]+) map=(\S+) none")
EVENT = re.compile(r"\[WP_PROBE_EVENT\] t=([\d.]+) map=(\S+) event=(bomb_planted|bomb_defused|bomb_exploded) userid=(\d+) planted=(\d+) defused=(\d+) exploded=(\d+)")
OBJECTIVE = re.compile(r"\[AgPB\] objectives: map-flags=\d+ round=(\d+) sites=\d+ rescues=\d+ hostages=\d+ bomb-state=(\d+) bomb-ent=(-?\d+) owner=(-?\d+) pos=(known|unknown) \(([^)]+)\) blow-time=(-?[\d.]+) bomb-radius=(-?[\d.]+) radius-source=(\S+) planted-observed=([01])")
COUNTS = ("planted", "defused", "exploded")
# Site identity guards, independent of waypoint endpoint-stop policy/radius.
# Native CC4::PrimaryAttack creates at player origin and projects vertically down
# 200 units (source-engine-czero/game/shared/cstrike/weapon_c4.cpp:1052-1061).
# A 64-unit XY neighborhood identifies the requested authored GOAL; the planter
# must be on its floor within 32 Z units. C4 gets the native 200-unit drop plus
# that 32-unit floor band. Nonlocal bomb-mount relocation is not certified here.
SITE_XY = 64.0
SITE_PLANTER_Z = 32.0
SITE_BOMB_Z = 232.0
SITE_ZONE_MAX_AGE = 2.0


def finite_vector(text):
    result = [float(part) for part in text.split()]
    if len(result) != 3 or not all(map(math.isfinite, result)):
        raise ValueError("Invalid probe position")
    return result


def parse(log, map_name):
    """Only parse the current acknowledged query, never a cumulative stale log."""
    event_rows, bomb_rows, none_rows = list(EVENTS.finditer(log)), list(BOMB.finditer(log)), list(NONE.finditer(log))
    objectives = list(OBJECTIVE.finditer(log))
    if len(event_rows) != 1 or len(objectives) != 1 or len(bomb_rows) + len(none_rows) != 1:
        raise ValueError("Expected one complete current event/objective/bomb probe snapshot")
    event = event_rows[0]
    if event[2] != map_name or any(row[2] != map_name for row in bomb_rows + none_rows):
        raise ValueError("Bomb probe map differs from plan")
    stamp = float(event[1])
    if not math.isfinite(stamp):
        raise ValueError("Invalid event probe time")
    objective = objectives[0]
    data = {"players": states(log, map_name), "time": stamp, "round": int(objective[1]),
            "events": dict(zip(("planted", "planted_userid", "defused", "defused_userid", "exploded", "exploded_userid"), map(int, event.groups()[2:]))),
            "objective": {"state": int(objective[2]), "entity": int(objective[3]), "owner": int(objective[4]),
                          "position_known": objective[5] == "known", "position": finite_vector(objective[6]),
                          "blow_time": float(objective[7]), "radius": float(objective[8]), "radius_source": objective[9],
                          "planted_observed": int(objective[10])}, "bomb": None, "log": log}
    if bomb_rows:
        bomb = bomb_rows[0]
        values = {"time": float(bomb[1]), "entity": int(bomb[3]), "ticking": int(bomb[4]),
                  "defused": int(bomb[5]), "blow": float(bomb[6]), "position": finite_vector(bomb[7])}
        values["left"] = values["blow"] - values["time"]
        if not all(math.isfinite(values[key]) for key in ("time", "blow", "left")):
            raise ValueError("Invalid bomb time")
        data["bomb"] = values
    if not data["players"]:
        raise ValueError("No player probe rows in snapshot")
    return data


class ObjectiveVerifier:
    def __init__(self, plan, metadata, scene, sender, save=None, clock=time.monotonic, sleep=time.sleep):
        self.plan, self.metadata, self.scene, self.sender = plan, metadata, scene, sender
        self.save_callback, self.clock, self.sleep = save, clock, sleep
        self.labels = {node["label"]: index for index, node in enumerate(plan["nodes"])}
        self.human_userid = self.bot_userid = self.bot_name = self.round = None
        self.previous = self.baseline = self.planted = None
        self.start_offset = None
        self.original_timer = None
        self.timer_changed = False
        self.bot_added = False
        self.expected_team = None
        self.result = {"schema_version": 1, "metadata": metadata, "scene": scene, "status": "running",
                       "receipts": [], "samples": [], "stages": {}, "cleanup": {},
                       "started_at": datetime.now(timezone.utc).isoformat()}

    def save(self):
        if self.save_callback:
            self.save_callback(self.result)

    def send(self, command):
        try:
            receipt = self.sender.send(command)
        except CommandFailure as exc:
            self.result["receipts"].append(exc.receipt)
            self.save()
            raise
        self.result["receipts"].append(receipt)
        if re.search(r"Unknown command|\[AGPB_REG\] (?:rejected:|usage:)", receipt["log"], re.I):
            raise ValueError("Command rejected; inspect retained receipt: " + command)
        return receipt

    def human(self, data, team=None, alive=True):
        player = data["players"].get(self.metadata["human_slot"])
        if not player or player["fake"]:
            raise ValueError("Configured human is missing or is a fake client")
        if self.human_userid is not None and player["userid"] != self.human_userid:
            raise ValueError("Human userid changed during regression")
        if team and player["team"] != TEAM[team]:
            raise ValueError("Human is on the wrong team")
        if alive and not player["alive"]:
            raise ValueError("Human died during regression")
        if any(p["fake"] and p["team"] in (2, 3) and p["team"] != player["team"] for p in data["players"].values()):
            raise ValueError("Every playing bot must share the human team (AGENTS.md)")
        return player

    def pair(self, data):
        human, bot = validate_pair(data["players"], self.metadata["human_slot"], self.metadata["bot_slot"],
                                   self.expected_team, (self.human_userid, self.bot_userid))
        rows = list(BOT.finditer(data["log"]))
        matching = [row for row in rows if int(row[1]) == self.metadata["bot_index"]]
        if len(matching) != 1 or int(matching[0][3]) != bot["slot"] or int(matching[0][4]) != bot["team"]:
            raise ValueError("agpb_list bot index/slot/team mapping changed")
        name = matching[0][2]
        if (self.bot_name and name != self.bot_name) or sum(row[2] == name for row in rows) != 1:
            raise ValueError("Bot route-message name changed or is ambiguous")
        return human, bot

    def snap(self, team=None, allow_round_change=False, allow_dead=False):
        receipt = self.send(QUERY)
        data = parse(receipt["log"], self.plan["map"])
        data["receipt_index"] = len(self.result["receipts"]) - 1
        self.result["samples"].append(data)
        self.human(data, team, not allow_dead)
        if self.round is not None and not allow_round_change and data["round"] != self.round:
            raise ValueError("Round changed during regression; respawn cannot count as survival")
        if self.previous:
            if data["time"] < self.previous["time"]:
                raise ValueError("Probe game time reset during regression")
            if any(data["events"][key] < self.previous["events"][key] for key in COUNTS):
                raise ValueError("Probe event counters reset during regression")
        self.previous = data
        if self.bot_userid is not None:
            self.pair(data)
        if self.planted:
            self.check_planted_history(data)
        self.save()
        return data

    def wait(self, label, predicate, timeout, team=None, allow_round_change=False, allow_dead=False,
             game_time=False, wall_timeout=None):
        wall_start = self.clock()
        wall_limit = wall_timeout if game_time else timeout
        if game_time and (wall_limit is None or wall_limit <= 0):
            raise ValueError("Game-time waits require a bounded wall-time cap")
        game_start = self.previous["time"] if game_time and self.previous else None
        timing = None
        if game_time:
            timing = {"clock": "probe_game_time", "timeout_game_seconds": timeout,
                      "wall_stall_cap_seconds": wall_limit, "start_game_time": game_start}
            self.result.setdefault("wait_timing", {})[label] = timing
        while self.clock() - wall_start < wall_limit:
            data = self.snap(team, allow_round_change, allow_dead)
            if game_time:
                if game_start is None:
                    game_start = data["time"]
                    timing["start_game_time"] = game_start
                timing.update(elapsed_game_seconds=data["time"] - game_start,
                              elapsed_wall_seconds=self.clock() - wall_start)
            if predicate(data):
                self.result["stages"][label] = data
                if timing is not None:
                    timing["status"] = "passed"
                self.save()
                print("PASS_STAGE", label, flush=True)
                return data
            if game_time and data["time"] - game_start >= timeout:
                timing["status"] = "game_time_timeout"
                raise RuntimeError(label + " timed out after %.1f game seconds" % timeout)
            self.sleep(0.2)
        if game_time:
            timing.update(status="wall_stall_cap", elapsed_wall_seconds=self.clock() - wall_start)
            raise RuntimeError(label + " reached %.1f-second wall stall cap" % wall_limit)
        raise RuntimeError(label + " timed out")

    @staticmethod
    def no_bots(data):
        # Require all fake clients gone, including observers, before switching scene.
        return not any(p["fake"] for p in data["players"].values())

    def event_seen(self, data, key, actor=None):
        base = self.baseline["events"]
        expected = base[key] + 1
        if data["events"][key] < expected:
            return False
        if data["events"][key] != expected:
            raise ValueError("Unexpected extra " + key + " event")
        if actor is not None and data["events"][key + "_userid"] != actor:
            raise ValueError("Real " + key + " event belongs to a different userid")
        column = {"planted": 5, "defused": 6, "exploded": 7}[key]
        rows = [row for row in EVENT.finditer(self.sender.tail(self.start_offset))
                if row[2] == self.plan["map"] and row[3] == "bomb_" + key and
                float(row[1]) >= self.baseline["time"] and int(row[column]) == expected and
                (actor is None or int(row[4]) == actor)]
        if len(rows) != 1:
            raise ValueError("Missing or ambiguous fresh WP_PROBE_EVENT for " + key)
        self.result.setdefault("event_evidence", {})[key] = rows[0][0]
        return True

    def check_planted_history(self, data):
        initial = self.planted
        if data["events"]["planted"] != initial["events"]["planted"] or data["events"]["planted_userid"] != initial["events"]["planted_userid"]:
            raise ValueError("Planted bomb event identity changed")
        bomb = data["bomb"]
        if bomb and (bomb["entity"] != initial["bomb"]["entity"] or
                     (bomb["ticking"] == 1 and bomb["position"] != initial["bomb"]["position"]) or
                     abs(bomb["blow"] - initial["bomb"]["blow"]) > 0.002):
            raise ValueError("Planted bomb entity/position/deadline changed")
        if not bomb and data["events"]["exploded"] == self.baseline["events"]["exploded"]:
            raise ValueError("Planted bomb disappeared before a real explosion")

    def site_evidence(self, data, actor):
        label = self.scene["plant_target"]
        index = self.labels[label]
        node = self.plan["nodes"][index]
        goal = [node[key] for key in ("x", "y", "z")]
        evidence = {"scope": "planted_site_identity_only", "target": label, "target_id": index,
                    "target_position": goal, "target_flags": node.get("flags", 0),
                    "bomb_position": data["bomb"]["position"],
                    "limits": {"xy": SITE_XY, "planter_z": SITE_PLANTER_Z,
                               "bomb_z": SITE_BOMB_Z, "preplant_zone_max_age": SITE_ZONE_MAX_AGE}}
        self.result["site_identity"] = evidence
        if not node.get("flags", 0) & 16:
            raise ValueError("Requested plant target is not an authored GOAL")
        event = EVENT.search(self.result["event_evidence"]["planted"])
        event_time = float(event[1])
        slot = self.metadata["bot_slot"] if self.metadata["mode"] == "T_escape" else self.metadata["human_slot"]
        candidates = []
        for sample in self.result["samples"]:
            player = sample["players"].get(slot)
            if (player and sample["round"] == data["round"] and player["userid"] == actor and
                    player["fake"] == int(self.metadata["mode"] == "T_escape") and
                    player["team"] == 2 and player["alive"] == 1 and player["zone"] == 1 and
                    player["flags"] & 1 and sample["events"]["planted"] == self.baseline["events"]["planted"] and
                    0 <= event_time - player["time"] <= SITE_ZONE_MAX_AGE):
                candidates.append((player["time"], player, sample["receipt_index"]))
        if not candidates:
            raise ValueError("No recent same-userid grounded real bomb-zone planter sample")
        _, planter, receipt_index = max(candidates, key=lambda item: item[0])
        bomb = data["bomb"]["position"]
        xy = lambda left, right: math.hypot(left[0] - right[0], left[1] - right[1])
        measured = {"planter_goal_xy": xy(planter["position"], goal),
                    "planter_goal_z": abs(planter["position"][2] - goal[2]),
                    "bomb_goal_xy": xy(bomb, goal), "bomb_goal_z": abs(bomb[2] - goal[2]),
                    "bomb_planter_xy": xy(bomb, planter["position"]),
                    "bomb_planter_z": abs(bomb[2] - planter["position"][2])}
        evidence.update(planter=planter, planter_receipt_index=receipt_index,
                        event_time=event_time, zone_sample_age=event_time - planter["time"], measurements=measured)
        if (measured["planter_goal_xy"] > SITE_XY or measured["planter_goal_z"] > SITE_PLANTER_Z or
                measured["bomb_goal_xy"] > SITE_XY or measured["bomb_goal_z"] > SITE_BOMB_Z or
                measured["bomb_planter_xy"] > SITE_XY or measured["bomb_planter_z"] > SITE_BOMB_Z):
            evidence["passed"] = False
            raise ValueError("Actual planted C4/planter is outside the requested GOAL site neighborhood")
        evidence["passed"] = True
        # Postplant zone may immediately become zero and escape movement may begin;
        # only the fresh pre-event zone sample above establishes planting region.
        evidence["postplant_planter"] = data["players"].get(slot)
        return evidence

    def planted_now(self, data, actor):
        base, current = self.baseline["events"], data["events"]
        if current["defused"] != base["defused"] or current["exploded"] != base["exploded"]:
            raise ValueError("Bomb ended before plant could be verified")
        if not self.event_seen(data, "planted", actor):
            return False
        bomb = data["bomb"]
        if not bomb or bomb["ticking"] != 1 or bomb["defused"] == 1 or bomb["left"] <= 0:
            return False
        self.site_evidence(data, actor)
        objective = data["objective"]
        if (objective["state"] != 4 or not objective["position_known"] or
                objective["entity"] != bomb["entity"] or math.dist(objective["position"], bomb["position"]) > 1.0):
            # The AgPB snapshot is throttled; wait for concordance, without
            # treating an old weapon_c4 snapshot as the new planted entity.
            return False
        self.planted = data
        self.result["bomb_position"] = bomb["position"]
        self.result["bomb_entity"] = bomb["entity"]
        return True

    def place_human(self, position):
        self.send("agpb_reg_place_human %d %.6f %.6f %.6f 0" % (self.metadata["human_slot"], *position))

    def node_position(self, label):
        node = self.plan["nodes"][self.labels[label]]
        return [node["x"], node["y"], node["z"] + 2]

    def add_bot(self, team):
        # Disable autonomy in the same acknowledged CFG, before another server tick.
        self.bot_added = True  # Even a failed transport may already have applied add.
        self.send("agpb_add %d; agpb_bot_auto %d 0" % (TEAM[team], self.metadata["bot_index"]))
        data = self.wait("same_team_bot", lambda d: d["players"].get(self.metadata["bot_slot"], {}).get("alive") == 1 and
                         d["players"][self.metadata["bot_slot"]]["team"] == TEAM[team], 8, team)
        human, bot = validate_pair(data["players"], self.metadata["human_slot"], self.metadata["bot_slot"], team)
        self.bot_userid, self.expected_team = bot["userid"], team
        self.pair(data)
        self.bot_name = next(row[2] for row in BOT.finditer(data["log"]) if int(row[1]) == self.metadata["bot_index"])
        self.metadata.update(human_userid=human["userid"], bot_userid=bot["userid"], bot_name=self.bot_name, round=self.round)
        return data

    def goto(self, label):
        index = self.labels[label]
        receipt = self.send("agpb_bot_goto %d %d" % (self.metadata["bot_index"], index))
        prefix = re.escape("[AgPB] " + self.bot_name + ": ")
        walk = re.search(prefix + r"walking to #" + str(index) + r", \d+ hop\(s\): [0-9 ]+", receipt["log"])
        if not walk:
            raise ValueError("No fresh walking acknowledgment for the configured bot/target")
        def arrived(data):
            log = receipt["log"] + self.sender.tail(receipt["end_offset"])
            following = log[walk.end():]
            success = re.search(prefix + r"arrived at waypoint #" + str(index) + r"\b", following)
            failure = re.search(prefix + r"[^\r\n]*(?:failed:|stuck|could not|no walkable|cannot|unable to|route aborted| - stopped)", following, re.I)
            if failure and (not success or failure.start() < success.start()):
                raise ValueError("Route failed before arrival: " + failure[0])
            return bool(success)
        self.wait("route_" + label, arrived, 90, self.expected_team)

    def setup(self):
        self.send("agpb_kick all; agpb_reg_clear")
        initial = self.wait("no_bots", self.no_bots, 5, allow_dead=True)
        self.human_userid = self.human(initial, alive=False)["userid"]
        self.metadata["human_userid"] = self.human_userid
        receipt = self.send("agpb_wp_list 0; mp_c4timer")
        validate_waypoints(receipt["log"], self.plan)
        timer = re.search(r'(?m)^\s*"?mp_c4timer"?\s*=\s*"?([0-9.]+)', receipt["log"])
        if not timer or not math.isfinite(float(timer[1])):
            raise ValueError("Cannot record original mp_c4timer; refusing to change it")
        self.original_timer = timer[1]
        self.result["original_settings"] = {"mp_c4timer": self.original_timer}
        self.send("agpb_reg_human_team %d T" % self.metadata["human_slot"])
        self.wait("human_T_no_bots", lambda d: self.no_bots(d) and d["players"][self.metadata["human_slot"]]["team"] == 2, 5)
        before_restart = self.previous["round"]
        self.timer_changed = True  # A failed transport can still have applied the CFG.
        self.send("mp_c4timer %s; mp_restartgame 1" % self.scene["c4timer"])
        data = self.wait("human_owns_c4", lambda d: d["round"] > before_restart and
                         d["objective"]["state"] == 2 and d["objective"]["owner"] == self.metadata["human_slot"], 12, "T", True, True)
        self.human(data, "T")
        if data["bomb"] is not None:
            raise ValueError("Restart did not clear the old planted bomb")
        self.round, self.baseline = data["round"], data
        self.result["event_baseline"] = data

    def prepare_plant(self):
        if self.metadata["mode"] == "T_escape":
            self.add_bot("T")
            self.send("agpb_reg_drop_c4 %d %.6f %.6f %.6f" % (self.metadata["human_slot"], *self.scene["drop_position"]))
            self.wait("real_drop", lambda d: d["objective"]["state"] == 3 and d["objective"]["owner"] not in d["players"], 8, "T")
            self.place_human(self.scene["observer_position"])
            self.send("agpb_bot_auto %d 1" % self.metadata["bot_index"])
            self.wait("bot_pickup", lambda d: d["objective"]["state"] == 2 and d["objective"]["owner"] == self.metadata["bot_slot"], 20, "T")
            self.goto(self.scene["plant_target"])
            self.send("agpb_bot_auto %d 1" % self.metadata["bot_index"])
            return self.wait("bot_planted", lambda d: self.planted_now(d, self.bot_userid), 15, "T")
        self.place_human(self.node_position(self.scene["plant_target"]))
        self.wait("human_in_zone", lambda d: d["players"][self.metadata["human_slot"]]["zone"] == 1 and
                  d["players"][self.metadata["human_slot"]]["flags"] & 1, 5, "T")
        self.send("agpb_reg_human_plant %d" % self.metadata["human_slot"])
        planted = self.wait("human_planted", lambda d: self.planted_now(d, self.human_userid), 12, "T")
        self.send("agpb_kick all; agpb_reg_clear")
        self.wait("no_bots_before_CT", self.no_bots, 5, "T")
        self.send("agpb_reg_human_team %d CT" % self.metadata["human_slot"])
        self.wait("human_CT_no_bots", lambda d: self.no_bots(d) and d["players"][self.metadata["human_slot"]]["team"] == 3, 5)
        self.place_human(self.scene["observer_position"])
        self.add_bot("CT")
        if self.metadata["mode"] == "CT_escape":
            self.goto(self.scene["ct_escape_target"])
            self.wait("late_bomb", lambda d: d["bomb"] is not None and d["bomb"]["ticking"] == 1 and d["bomb"]["left"] <= 12, 35, "CT")
        self.send("agpb_bot_auto %d 1" % self.metadata["bot_index"])
        return planted

    def verify_outcome(self):
        base = self.baseline["events"]
        team = self.expected_team
        if self.metadata["mode"] == "CT_defuse":
            self.wait("defusing", lambda d: d["players"][self.metadata["bot_slot"]]["defusing"] == 1, 25, team)
            def defused(data):
                if data["events"]["exploded"] != base["exploded"]:
                    raise ValueError("Bomb exploded during defuse trial")
                return self.event_seen(data, "defused", self.bot_userid) and data["bomb"] is not None and data["bomb"]["ticking"] == 0 and data["bomb"]["defused"] in (-1, 1)
            # Native defuse takes game time. A slow simulation must still get
            # the original 15-game-second confirmation window; wall time only
            # bounds a stalled/paused server or console transport.
            self.wait("defused", defused, 15, team, game_time=True, wall_timeout=60)
            return
        def selected(data):
            if data["events"]["defused"] != base["defused"]:
                raise ValueError("Bomb was defused during escape trial")
            prefix = re.escape("[AgPB] " + self.bot_name + ": ")
            return bool(re.search(prefix + r"[^\r\n]*escape_bomb", data["log"]))
        self.wait("escape_selected", selected, 35, team)
        def exploded(data):
            if data["events"]["defused"] != base["defused"]:
                raise ValueError("Bomb was defused during escape trial")
            return self.event_seen(data, "exploded")
        observed = self.wait("exploded_alive", exploded, 40, team)
        # STATE precedes BOMB in QUERY. Re-sample in a later game tick so a
        # pre-damage alive line cannot be paired with a just-fired explosion.
        final = self.wait("post_explosion_alive", lambda d: d["time"] > observed["time"], 5, team)
        radius, source = final["objective"]["radius"], final["objective"]["radius_source"]
        distance = math.dist(final["players"][self.metadata["bot_slot"]]["position"], self.planted["bomb"]["position"])
        self.result.update(post_explosion_distance=distance, radius=radius, radius_source=source)
        if not math.isfinite(radius) or radius <= 0 or source == "unknown" or distance <= radius:
            raise ValueError("No confirmed post-explosion position outside the actual damage radius")
        # Bind safe-plan evidence to this bot, this plant, and a fresh log segment.
        complete = self.sender.tail(self.start_offset)
        plant_event = self.result["event_evidence"]["planted"]
        plant_offset = complete.find(plant_event)
        if plant_offset < 0:
            raise ValueError("Plant event no longer belongs to the current log")
        segment = complete[plant_offset + len(plant_event):]
        pattern = re.escape("[AgPB] " + self.bot_name + ": escape plan: ") + r"[^\r\n]*\bsafe=yes\b"
        safe = re.findall(pattern, segment)
        if not safe:
            raise ValueError("Missing fresh same-bot escape plan and safe-position evidence")
        self.result["safe_plan_evidence"] = safe

    def cleanup(self):
        # Only resolve/stop the original bot; never stop a replacement at its slot.
        if self.bot_added:
            try:
                receipt = self.send("agpb_list; agpb_wp_probe_state")
                players = states(receipt["log"], self.plan["map"])
                original = players.get(self.metadata["bot_slot"])
                rows = [row for row in BOT.finditer(receipt["log"]) if original and int(row[3]) == original["slot"]]
                if not original or not original["fake"] or self.bot_userid is None or original["userid"] != self.bot_userid or len(rows) != 1:
                    raise ValueError("Cannot safely resolve original bot for cleanup")
                self.send("agpb_bot_stop " + rows[0][1])
                self.send("agpb_kick " + rows[0][1])
                self.result["cleanup"]["bot"] = "stopped_and_kicked"
            except (Exception, KeyboardInterrupt) as exc:
                self.result["cleanup"]["bot_error"] = str(exc) or "KeyboardInterrupt"
        for key, command in (("fixture", "agpb_reg_clear"), ("mp_c4timer", "mp_c4timer " + self.original_timer if self.timer_changed and self.original_timer is not None else None)):
            if command:
                try:
                    self.send(command)
                    self.result["cleanup"][key] = "restored" if key == "mp_c4timer" else "cleared"
                except (Exception, KeyboardInterrupt) as exc:
                    self.result["cleanup"][key + "_error"] = str(exc) or "KeyboardInterrupt"

    def run(self):
        try:
            self.save()
            self.start_offset = self.sender.cursor()
            self.setup()
            self.prepare_plant()
            self.verify_outcome()
            for name in ("plan", "agpw", "bsp", "dll"):
                path, expected = self.metadata.get(name + "_file"), self.metadata.get(name + "_sha256")
                if path and expected and sha256(Path(path).read_bytes()) != expected:
                    raise ValueError(name.upper() + " changed during regression")
            self.result["status"] = "passed"
        except (Exception, KeyboardInterrupt) as exc:
            self.result.update(status="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed", error=str(exc) or "KeyboardInterrupt")
        finally:
            self.cleanup()
            try:
                if self.start_offset is not None:
                    self.result["log"] = self.sender.tail(self.start_offset)
            except Exception as exc:
                self.result["log_error"] = str(exc)
                self.result["status"] = "failed"
            if any(key.endswith("_error") for key in self.result["cleanup"]):
                self.result["status"] = "failed"
                self.result.setdefault("error", "Cleanup failed; inspect cleanup receipts")
            self.result["finished_at"] = datetime.now(timezone.utc).isoformat()
            self.save()
        return self.result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--session-name", required=True)
    parser.add_argument("--session-directory", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output", type=Path, help="Defaults to TEMP session-directory/name.json; must not already exist")
    parser.add_argument("--mode", choices=("T_escape", "CT_escape", "CT_defuse"), required=True)
    parser.add_argument("--site", choices=("A", "B"), required=True)
    parser.add_argument("--human-slot", type=int, default=1)
    parser.add_argument("--bot-slot", type=int, default=2)
    parser.add_argument("--bot-index", type=int, default=0)
    parser.add_argument("--plant-target", help="Authored plan label; dust2 defaults A_goal/b_goal")
    parser.add_argument("--ct-escape-target", help="Authored plan label; dust2 defaults C_east1/ctb_mouth")
    parser.add_argument("--drop-label", help="Authored plan label, z+2; dust2 defaults t_w1")
    parser.add_argument("--drop-position", type=float, nargs=3)
    parser.add_argument("--observer-position", type=float, nargs=3, help="Human setup/observation location; dust2 defaults -700 -700 134")
    parser.add_argument("--c4timer", type=float, default=35)
    parser.add_argument("--powershell", default=shutil.which("pwsh") or shutil.which("powershell") or "powershell.exe")
    args = parser.parse_args()
    if not re.fullmatch(r"agpb_wp_[A-Za-z0-9_]+", args.session_name) or not re.fullmatch(r"[A-Za-z0-9_-]+", args.name):
        parser.error("Invalid session name or run name")
    if args.bot_index != 0 or min(args.human_slot, args.bot_slot) < 1 or args.human_slot == args.bot_slot:
        parser.error("One fresh bot is created after kicking all; bot-index must be 0 and player slots must differ")
    if not math.isfinite(args.c4timer) or args.c4timer <= 0:
        parser.error("c4timer must be positive and finite")
    session = temp_child(args.session_directory)
    output = temp_child(args.output or session / (args.name + ".json"))
    if output.exists() or output == session / "console.log":
        parser.error("Output already exists or is the session log; use a distinct output")
    plan_file = args.plan.resolve()
    raw = plan_file.read_bytes()
    plan = json.loads(raw.decode("utf-8-sig"))
    expected = export(plan)
    if not re.fullmatch(r"[A-Za-z0-9_-]+", plan["map"]):
        parser.error("Invalid map name")
    labels = {node["label"]: node for node in plan["nodes"]}
    dust2 = plan["map"] == "de_dust2"
    target = args.plant_target or (("A_goal" if args.site == "A" else "b_goal") if dust2 else None)
    escape_target = args.ct_escape_target or (("C_east1" if args.site == "A" else "ctb_mouth") if dust2 else None)
    observer = args.observer_position or ([-700, -700, 134] if dust2 else None)
    drop_label = args.drop_label or ("t_w1" if dust2 else None)
    if target not in labels or (args.mode == "CT_escape" and escape_target not in labels) or observer is None:
        parser.error("Specify valid plant/escape plan labels and observer-position for this map")
    if args.drop_label and args.drop_position:
        parser.error("Specify drop-label or drop-position, not both")
    drop = args.drop_position
    if drop is None and drop_label in labels:
        drop = [labels[drop_label][key] + (2 if key == "z" else 0) for key in ("x", "y", "z")]
    if args.mode == "T_escape" and drop is None:
        parser.error("T_escape needs drop-position or drop-label")
    if any(not math.isfinite(value) or abs(value) > 1000000 for value in observer + (drop or [])):
        parser.error("Scene positions must be finite fixture coordinates")
    metadata = {"scope": "real_bomb_objective_regression", "name": args.name, "mode": args.mode, "site": args.site,
                "map": plan["map"], "plan_file": str(plan_file), "plan_sha256": sha256(raw),
                "plan_version": plan.get("plan_version", 1), "human_slot": args.human_slot,
                "bot_slot": args.bot_slot, "bot_index": args.bot_index, "session_name": args.session_name,
                "session_directory": str(session), "output": str(output), "plant_selection": "specified_route" if args.mode == "T_escape" else "human_fixture"}
    scene = {"plant_target": target, "ct_escape_target": escape_target, "drop_position": drop,
             "observer_position": observer, "c4timer": args.c4timer, "late_bomb_seconds": 12,
             "human_fixture_used": True, "bot_control": "AgPB CUserCmd"}
    output.parent.mkdir(parents=True, exist_ok=True)
    def save(result):
        output.write_text(json.dumps(result, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    try:
        metadata.update(disk_evidence(plan, expected))
        for required in ("agpw_sha256", "bsp_sha256", "dll_sha256"):
            if not metadata.get(required):
                raise ValueError("Missing required disk evidence: " + required)
    except Exception as exc:
        save({"schema_version": 1, "metadata": metadata, "scene": scene, "status": "failed", "error": str(exc), "receipts": [], "samples": [], "stages": {}})
        print(json.dumps({"status": "failed", "error": str(exc), "output": str(output)}))
        return 1
    sender = ConsoleSender(args.powershell, args.session_name, session)
    try:
        result = ObjectiveVerifier(plan, metadata, scene, sender, save=save).run()
    finally:
        sender.close()
    print(json.dumps({"status": result["status"], "output": str(output), "error": result.get("error")}))
    return 0 if result["status"] == "passed" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())
