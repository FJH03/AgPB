#!/usr/bin/env python3
"""Recompute sampled minimum-combat evidence from unedited console receipts.

API: parse(log), evaluate_case(case, metadata), evaluate(document).
metadata contains map, subject_slot/userid/name, human_slot/userid, six HASHES,
controlled_enemy_authorized, guard_expected_crc and prelude receipt array.
Each case has kind, binding (same six hashes), target_identity (slot/userid/serial)
and optional identity overrides for subject_slot/userid/name/serial and human_slot/userid/serial
and stages. Negative cases use before/run; visible_damage_resume additionally
uses resumed and goal; stop_interrupt uses quiet. Optional target is the original
route endpoint {index,x,y,z,radius}. Receipt/observed_log rules match M3a/M3b.
This checks supplied samples, not unsampled time or complete enemy perception.
"""
import argparse
import json
import math
from pathlib import Path
import re
import sys

# Reuse the existing pure identity/endpoint parsers from this checkout.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "waypoint-workflow"))

import stop_policy
import verify_lifecycle as lifecycle

VERSION = 1
KINDS = ("friendly_rejection", "occluded_rejection", "visible_damage_resume", "stop_interrupt")
HASHES = tuple(name + "_sha256" for name in ("plan", "agpw", "bsp", "dll", "probe", "server"))
TOKEN = re.compile(r'([A-Za-z_][A-Za-z_0-9-]*)=(\([^)]*\)|"[^"\r\n]*"|[^\s]+)')
TAGS = {"[WP_COMBAT_" + name.upper() + "] ": name for name in ("scene", "player", "weapon", "trace", "counts", "event", "damage", "team")}
require = lifecycle.require


def fields(text):
    result, cursor = {}, 0
    for match in TOKEN.finditer(text):
        require(not text[cursor:match.start()].strip() and match[1] not in result, "Malformed/duplicate combat field")
        key, value = match[1], match[2]
        if value.startswith("("):
            value = lifecycle.vector(value, 2 if key in ("move", "view") and value.count(",") == 1 else len(re.split(r"[,\s]+", value[1:-1].strip())))
        elif value.startswith('"'):
            value = value[1:-1]
        elif re.fullmatch(r"-?\d+", value):
            value = int(value)
        elif re.fullmatch(r"0x[0-9A-Fa-f]+", value):
            value = int(value, 16)
        elif re.fullmatch(r"[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?", value):
            value = lifecycle.number(value)
        result[key], cursor = value, match.end()
    require(not text[cursor:].strip(), "Malformed combat suffix")
    return result


def parse(log):
    result = {name: [] for name in ("core", "combat", "guard", *TAGS.values())}
    for line in log.splitlines():
        line = line.strip()
        if line.startswith("[AgPB lifecycle] "):
            result["core"].append(lifecycle.core(line[len("[AgPB lifecycle] "):]))
        elif line.startswith("[AgPB Guard] active="):
            result["guard"].append(fields(line[len("[AgPB Guard] "):]))
        elif re.match(r"\[AgPB\] .+: combat v=", line):
            match = re.fullmatch(r"\[AgPB\] (.+?): combat (v=.*)", line)
            row = fields(match[2]); row["name"] = match[1]
            require(row["v"] == VERSION, "Unknown core combat version")
            result["combat"].append(row)
        else:
            for tag, kind in TAGS.items():
                if line.startswith(tag):
                    row = fields(line[len(tag):])
                    require(row["v"] == VERSION and row["seq"] > 0 and row["tick"] >= 0, "Unknown combat probe version/time")
                    require(isinstance(row["epoch"], str) and row["epoch"] and (row["map_serial"] > 0 or kind == "event" and row.get("event") == "plugin_start" and row["map_serial"] == 0), "Unknown combat reload/map identity")
                    require(row["replay"] in (0, 1), "Unknown replay status")
                    result[kind].append(row)
                    break
    return result


def hash_binding(metadata, case):
    for key in HASHES:
        value = metadata.get(key)
        require(isinstance(value, str) and re.fullmatch(r"[a-fA-F0-9]{64}", value), "Missing artifact version: " + key)
        bound = case.get("binding", {}).get(key)
        require(isinstance(bound, str) and bound.lower() == value.lower(), "Case artifact version differs: " + key)


def original(receipt, seen, offset):
    log, marker, finish = lifecycle.receipt_log(receipt)
    require(marker not in seen and finish > offset[0], "Receipt was reused/reordered")
    seen.add(marker)
    if "observed_log" in receipt:
        start, end = receipt.get("observed_start_offset"), receipt.get("observed_end_offset")
        require(type(start) is int and type(end) is int and offset[0] <= start <= finish <= end, "Observed offsets missing/reused/reversed")
        offset[0] = end
    else:
        offset[0] = finish
    return log


def guard_prelude(metadata, seen, offset):
    prelude = metadata.get("prelude")
    require(isinstance(prelude, list) and prelude, "Missing original guard prelude")
    guards = [guard for receipt in prelude for guard in parse(original(receipt, seen, offset))["guard"]]
    require(guards and all(row["active"] == row["intact"] == 1 for row in guards), "Guard is not active and intact in the original prelude")
    require(metadata.get("guard_expected_crc") == "770BAE64", "Unknown declared Radio guard CRC")
    return guards


def identity(row, expected):
    return all(row[key] == expected[key] for key in ("slot", "userid", "serial"))


def actor(event, expected, prefix="actor"):
    return all(event[prefix + "_" + key] == expected[key] for key in ("slot", "userid", "serial"))


def snapshot(data, metadata, target):
    require(len(data["core"]) == 1, "Ambiguous lifecycle snapshot")
    core = data["core"][0]
    require((core["edict"], core["userid"], core["name"], core["map"]) == (metadata["subject_slot"], metadata["subject_userid"], metadata["subject_name"], metadata["map"]), "Subject core identity/map changed")
    require(len(data["combat"]) == 1 and data["combat"][0]["name"] == core["name"], "Missing/ambiguous subject combat snapshot")
    combat = data["combat"][0]
    require(combat["enabled"] in (0, 1) and combat["global"] == 1, "Unknown/disabled global combat state")
    require(len(data["scene"]) == len(data["counts"]) == 1, "Missing/ambiguous combat scene/counter snapshot")
    scene, count = data["scene"][0], data["counts"][0]
    players = {row["slot"]: row for row in data["player"]}
    weapons = {row["slot"]: row for row in data["weapon"]}
    require(len(players) == len(data["player"]) and len(weapons) == len(data["weapon"]), "Ambiguous player/weapon snapshot")
    bot, human, victim = players[metadata["subject_slot"]], players[metadata["human_slot"]], players[target["slot"]]
    weapon = weapons[bot["slot"]]
    require((bot["userid"], bot["fake"], human["userid"], human["fake"]) == (metadata["subject_userid"], 1, metadata["human_userid"], 0), "Subject/human identity changed")
    require(identity(victim, target) and victim["fake"] == 1, "Controlled target identity changed/reused")
    for role, row in (("subject", bot), ("human", human)):
        if role + "_serial" in metadata:
            require(row["serial"] == metadata[role + "_serial"], "Declared player serial differs")
    require(bot["team"] == human["team"] in (2, 3) and human["alive"] == 1 and human["player_state"] == 0 and human["state_known"] == 1, "Human must stay alive ACTIVE on the subject team")
    require(bot["alive"] == 1 and bot["state_known"] == 1 and bot["player_state"] == 0, "Subject must remain alive ACTIVE during acceptance")
    require(human["slot"] < bot["slot"] and human["slot"] < victim["slot"], "Human must occupy the lower slot")
    require(all(row["team"] == human["team"] or identity(row, target) for row in players.values() if row["fake"] == 1 and row["team"] in (2, 3)), "An unbound opposing fake exists")
    require(scene["watching"] == 1 and scene["unbound_opposing_count"] == 0 and scene["subject_present"] == scene["human_present"] == scene["target_present"] == 1, "Scene binding missing or unsafe")
    for role, player in (("subject", bot), ("human", human), ("target", victim)):
        require(all(scene[role + "_" + key] == player[key] for key in ("slot", "userid", "serial")), "Scene binding identity differs")
    require(scene["sample"] > 0 and scene["subject_human_ok"] == scene["scene_binding_ok"] == 1, "Scene does not confirm live subject/human binding")
    opposing = sum(row["fake"] == 1 and row["team"] in (2, 3) and row["team"] != human["team"] for row in players.values())
    require(scene["opposing_fake_count"] == opposing and scene["target_opposing"] == int(victim["team"] != human["team"]), "Scene opponent counts differ from actual players")
    rows = [scene, count, *players.values(), *weapons.values(), *data["trace"]]
    require(all((row["epoch"], row["map_serial"], row["generation"], row["map"], row["sample"]) == (scene["epoch"], scene["map_serial"], scene["generation"], metadata["map"], scene["sample"]) for row in rows if row.get("sample", 0) != 0), "Probe samples mix reloads/maps/bindings")
    require(all(abs(row["tick"] - core["tick"]) <= 2 for row in rows if row.get("sample", 0) != 0), "Probe/core sample tick differs")
    require(all(count[key] == 1 for key in ("weapon_fire_hooked", "player_hurt_hooked", "player_death_hooked")) and count["damage_overflows"] == 0, "Missing event hooks or damage context overflow")
    require((bot["alive"], bot["life_state"], bot["team"], bot["health"]) == (core["alive"], core["lifestate"], core["team"], core["hp"]) and bot["health_known"] == 1, "Core/player state disagrees")
    require(math.dist(bot["pos"], core["pos"]) <= 1 and core["auto"] == core["roam"] == 0, "Position/manual-navigation isolation differs")
    require(identity(weapon, bot), "Weapon actor identity differs")
    require((combat["routeowner"], combat["goal"], combat["hop"], combat["node"]) == (core["owner"], core["goal"], core["index"], core["node"]), "Combat/lifecycle route snapshots disagree")
    return dict(core=core, combat=combat, bot=bot, human=human, victim=victim, weapon=weapon, scene=scene, counts=count, traces=data["trace"])


def collect(receipts, metadata, target, seen, offset, retained, commands, logs):
    frames = []
    for receipt in receipts:
        log = original(receipt, seen, offset)
        commands.append(receipt["command"]); logs.append(log)
        data = parse(log)
        for kind in ("event", "damage", "team", "trace"):
            for event in data[kind]:
                if kind == "trace" and event["sample"] != 0:
                    continue
                key = event["epoch"], event["seq"]
                plain = {k: v for k, v in event.items() if k != "replay"}
                value = kind, plain
                require(key not in retained or retained[key] == value, "Replay sequence changed contents/type")
                retained[key] = value
        for block in re.split(r"(?m)(?=^\[AgPB lifecycle\] )", log):
            part = parse(block)
            if part["core"]:
                frames.append(snapshot(part, metadata, target))
    return frames


def continuous(frames, minimum=0):
    require(len(frames) >= (3 if minimum else 1), "Too few combat samples")
    first = frames[0]
    for frame in frames:
        for role in ("bot", "human", "victim"):
            require(identity(frame[role], first[role]) and frame[role]["team"] == first[role]["team"], "Player identity/team changed within case")
        require(all(frame["scene"][key] == first["scene"][key] for key in ("epoch", "map_serial", "generation")), "Reload/map/binding changed within case")
    for previous, frame in zip(frames, frames[1:]):
        dt = frame["core"]["time"] - previous["core"]["time"]
        require(dt > 0 and (not minimum or dt <= 1.5) and frame["core"]["tick"] > previous["core"]["tick"] and frame["core"]["cmdnum"] > previous["core"]["cmdnum"], "Stale/reversed/gapped combat samples")
        require(frame["bot"]["fires"] >= previous["bot"]["fires"] and frame["combat"]["attackcmds"] >= previous["combat"]["attackcmds"], "Combat counters reversed")
    require(frames[-1]["core"]["time"] - first["core"]["time"] >= minimum, "Combat window is shorter than 0.8 seconds")


def selected_events(retained, baseline, final):
    scene = baseline["scene"]
    values = [(kind, event) for kind, event in retained.values() if (event["epoch"], event["map_serial"], event["generation"], event["map"]) == (scene["epoch"], scene["map_serial"], scene["generation"], scene["map"]) and baseline["counts"]["seq"] < event["seq"] <= final["counts"]["seq"]]
    require(all(baseline["core"]["tick"] <= row["tick"] <= final["core"]["tick"] + 2 for _, row in values), "Fresh event has stale/future tick")
    return values


def no_friendly_damage(values, bot):
    for kind, row in values:
        if kind == "event" and row["event"] == "player_hurt" and actor(row, bot):
            require(not (row["actor_team"] == row["victim_team"] and row["dmg_health"] > 0), "Subject caused same-team player_hurt damage")
        if kind == "damage" and row["event"] == "damage_post" and actor(row, bot, "attacker"):
            require(not (row["attacker_team"] == row["victim_team"] and row["context_valid"] == row["health_before_known"] == row["health_now_known"] == 1 and row["health_now"] < row["health_before"]), "Subject caused same-team measured damage")


def ammo_unchanged(frames, baseline):
    old = baseline["weapon"]
    require(old["clip_known"] == old["reserve_known"] == 1 and old["clip"] >= 0 and old["reserve"] >= 0, "Unknown baseline firearm ammunition")
    for frame in frames:
        weapon = frame["weapon"]
        require(weapon["clip_known"] == weapon["reserve_known"] == 1 and (weapon["active_ref"], weapon["class"], weapon["clip"], weapon["reserve"]) == (old["active_ref"], old["class"], old["clip"], old["reserve"]), "Observed weapon/ammunition changed during quiet rejection")


def no_attack(frames, baseline, events):
    require(all(frame["combat"]["attackcmds"] == baseline["combat"]["attackcmds"] and frame["bot"]["fires"] == baseline["bot"]["fires"] and not frame["core"]["buttons"] & (1 | (1 << 11)) for frame in frames), "New attack command or real fire during rejection/quiet window")
    require(not any(row["event"] in ("weapon_fire", "firebullets_post") and actor(row, baseline["bot"]) for row in events), "New actual subject fire during rejection/quiet window")
    ammo_unchanged(frames, baseline)


def evaluate_case(case, metadata):
    result = {"kind": case.get("kind"), "status": "pending", "scope": "sampled_minimum_combat_only"}
    try:
        kind = case["kind"]; require(kind in KINDS, "Unknown combat case kind")
        hash_binding(metadata, case)
        override = case.get("identity", {})
        require(isinstance(override, dict) and set(override) <= set("subject_slot subject_userid subject_name subject_serial human_slot human_userid human_serial".split()), "Unknown per-case identity override")
        metadata = dict(metadata, **override)
        seen, offset = set(), [0]
        guard_prelude(metadata, seen, offset)
        target = case["target_identity"]
        require(all(type(target[key]) is int and target[key] > 0 for key in ("slot", "userid", "serial")) and target["slot"] not in (metadata["subject_slot"], metadata["human_slot"]), "Missing/distinct controlled target identity")
        names = ["before", "run"] + (["resumed"] if kind == "visible_damage_resume" else ["quiet"] if kind == "stop_interrupt" else [])
        require(set(case["stages"]) == set(names), "Missing/unknown combat stages")
        retained, commands, logs, stages = {}, {}, [], {}
        for name in names:
            require(isinstance(case["stages"][name], list) and case["stages"][name], "Empty combat stage: " + name)
            commands[name] = []
            stages[name] = collect(case["stages"][name], metadata, target, seen, offset, retained, commands[name], logs)
            continuous(stages[name], 0.8 if kind.endswith("rejection") and name == "run" or name == "quiet" else 0)
            if name != "before":
                require(all(frame["combat"]["enabled"] == 1 for frame in stages[name]), "Combat disabled during active acceptance stage")
        frames = [frame for name in names for frame in stages[name]]
        continuous(frames)
        baseline, final = stages["before"][-1], frames[-1]
        values = selected_events(retained, baseline, final)
        events = [row for tag, row in values if tag == "event"]
        subject_events = [row for row in events if actor(row, baseline["bot"])]
        require(final["bot"]["fires"] - baseline["bot"]["fires"] == sum(row["event"] == "weapon_fire" for row in subject_events), "Fire counter does not match retained unique real events")
        for key, prefix in (("hurts_dealt", "actor"), ("hurts_taken", "victim")):
            require(final["bot"][key] - baseline["bot"][key] == sum(row["event"] == "player_hurt" and actor(row, baseline["bot"], prefix) for row in events), "Hurt counter does not match retained unique real events")
        for row in subject_events:
            require(row["actor_fake"] == 1 and row["actor_team"] == baseline["bot"]["team"], "Event subject fake/team attribution differs")
            if actor(row, target, "victim"):
                require(row["victim_fake"] == 1 and row["victim_team"] == baseline["victim"]["team"], "Event target fake/team attribution differs")
        no_friendly_damage(values, baseline["bot"])
        enemy = baseline["victim"]["team"] != baseline["bot"]["team"]
        require(baseline["victim"]["alive"] == 1 and baseline["victim"]["player_state"] == 0 and baseline["victim"]["state_known"] == 1, "Target was not alive ACTIVE before acceptance")
        if enemy:
            require(metadata.get("controlled_enemy_authorized") is True and baseline["victim"]["team"] in (2, 3), "Missing explicit controlled-enemy authorization")
        for name in names[1:]:
            require(not any(re.search(r"\b(?:agpb_netwrite|ent_fire|ent_remove|ent_create|agpb_bot_testweapon|agpb_reg_lifecycle_kill|agpb_bot_team|agpb_wp_load|agpb_wp_clear|kill)\b", command) for command in commands[name]), "External mutation/forced damage during acceptance")
            require(not any(re.search(r"\bagpb_(?:bot_combat\s+\S+\s+[01]|combat\s+[01])\b", command) for command in commands[name]), "Combat enable state changed during acceptance")
        if kind.endswith("rejection"):
            require(enemy == (kind == "occluded_rejection"), "Target team does not match rejection kind")
            for frame in stages["run"]:
                require(frame["victim"]["alive"] == 1 and frame["victim"]["state_known"] == 1 and frame["victim"]["player_state"] == 0, "Rejected target must remain alive ACTIVE")
                require(frame["core"]["route"] > 0 and frame["core"]["owner"] > 0 and frame["combat"]["state"] == "scanning", "Rejection requires active navigation and scanning intent")
                weapon = frame["weapon"]
                require(weapon["clip_known"] == weapon["primary_known"] == weapon["player_attack_known"] == weapon["reload_known"] == 1 and weapon["clip"] > 0 and weapon["reload"] == 0 and weapon["primary"] <= frame["core"]["time"] and weapon["player_attack"] <= frame["core"]["time"], "Rejected scene lacks a ready firearm")
            no_attack(stages["run"], baseline, subject_events)
            require(all(frame["combat"]["ent"] == -1 and frame["combat"]["owner"] == 0 for frame in stages["run"]), "Rejected target was acquired by combat")
            if enemy:
                for frame in stages["run"]:
                    trace = [row for row in frame["traces"] if row["sample"] == frame["scene"]["sample"] and row["shooter_slot"] == baseline["bot"]["slot"] and row["target_slot"] == target["slot"]]
                    require(len(trace) == 1 and actor(trace[0], baseline["bot"], "shooter") and actor(trace[0], target, "target") and trace[0]["unobstructed"] == 0 and trace[0]["did_hit"] == 1 and trace[0]["hit_ent"] != target["slot"], "Missing blocked eye-to-eye trace")
                result["scope"] = "sampled_eye_trace_occlusion_and_no_acquisition_only"
        else:
            require(enemy, "Positive combat requires the authorized opposing target")
            shots = [row for row in subject_events if row["event"] == "weapon_fire" and row["weapon"] not in ("unknown", "knife")]
            require(shots, "No new real subject weapon_fire")
            owned = [frame for frame in stages["run"] if frame["combat"]["owner"] > 0 and frame["combat"]["ent"] == target["slot"] and frame["combat"]["userid"] == target["userid"]]
            require(owned and all(frame["combat"]["state"] in ("reacting", "aiming", "firing", "reloading", "fire-blocked", "recoil-pause") for frame in owned), "No sampled combat ownership of the controlled target")
            boundary_tick = stages["resumed"][0]["core"]["tick"] if kind == "visible_damage_resume" else stages["run"][-1]["core"]["tick"]
            require(any(row["tick"] <= boundary_tick for row in shots), "Shot is not before interruption/resumption")
            if kind == "visible_damage_resume":
                hurts = [row for row in subject_events if row["event"] == "player_hurt" and actor(row, target, "victim") and row["actor_team"] != row["victim_team"] and row["dmg_health"] > 0 and row["health"] >= 0]
                known = [frame for frame in frames if frame["victim"]["health_known"] == 1]
                hits = [row for row in hurts if any(frame["core"]["tick"] < row["tick"] and frame["victim"]["health"] > row["health"] for frame in known) and any(frame["core"]["tick"] >= row["tick"] and frame["victim"]["health"] <= row["health"] for frame in known) and any(shot["tick"] <= row["tick"] and 0 <= row["t"] - shot["t"] <= 0.25 and shot["weapon"].removeprefix("weapon_") == row["weapon"].removeprefix("weapon_") for shot in shots)]
                require(hits, "No identity-attributed player_hurt with independently observed health decrease after a real shot")
                first_owned = frames.index(owned[0])
                prior_route = [frame for frame in frames[:first_owned] if frame["core"]["route"] > 0 and frame["combat"]["owner"] == 0]
                require(prior_route, "No pre-acquisition original route snapshot")
                original_route = prior_route[-1]["core"]
                initial_route = stages["before"][-1]["core"]
                require(initial_route["owner"] == original_route["owner"] and initial_route["goal"] == original_route["goal"], "Original route was replaced before acquisition")
                goal, owner, hop, node = case["goal"], original_route["owner"], original_route["index"], original_route["node"]
                require(original_route["route"] > 0 and original_route["goal"] == goal and owner > 0, "No original owned route before combat")
                require(len(owned) >= 2, "Need at least two increasing owned combat snapshots; retained " + str(len(owned)))
                require(all(frame["core"]["route"] == original_route["route"] and frame["core"]["owner"] == owner and frame["core"]["goal"] == goal and frame["core"]["index"] == hop and frame["core"]["node"] == node and frame["combat"]["owner"] == frame["core"]["token"] for frame in owned), "Combat changed original route owner/goal/hop")
                require(all(all(abs(value) <= 0.001 for value in frame["core"]["move"]) for frame in owned), "Combat ownership did not suspend actual locomotion commands")
                gotos = re.findall(r"\bagpb_bot_goto\s+(\d+)\s+(\d+)", ";".join(command for name in names for command in commands[name]))
                require(len(gotos) == 1 and int(gotos[0][0]) == metadata.get("subject_index", 0) and int(gotos[0][1]) == goal and not any(re.search(r"\bagpb_bot_goto\b", command) for command in commands["resumed"]), "Resume requires one original goto and no replacement goto")
                resumed = stages["resumed"]
                require(all(frame["combat"]["ent"] == -1 and frame["combat"]["owner"] == 0 for frame in resumed), "Enemy target was not released")
                live_route = [frame for frame in resumed if frame["core"]["route"] > 0]
                require(len(live_route) >= 2 and all(frame["core"]["owner"] == owner and frame["core"]["goal"] == goal for frame in live_route), "Original owner/goal did not resume")
                require(any(any(abs(value) > 0.001 for value in frame["core"]["move"]) for frame in live_route) and any(math.dist(live_route[0]["bot"]["pos"], frame["bot"]["pos"]) > 1 for frame in live_route[1:]), "No actual route movement after target loss")
                release_text = re.escape("[AgPB] " + metadata["subject_name"] + ": combat released ent=" + str(target["slot"]))
                require(any(frame["victim"]["alive"] == 0 or frame["combat"]["state"] == "target-lost" for frame in resumed) or re.search(release_text, "\n".join(logs)), "Missing observed target-loss/death or native release")
                if "target" in case:
                    endpoint = case["target"]
                    require(endpoint["index"] == goal and all(stop_policy.finite_number(endpoint[key]) for key in ("x", "y", "z", "radius")) and endpoint["radius"] >= 0, "Invalid original-goal endpoint")
                    samples = [{"time": frame["core"]["time"], "slot": frame["bot"]["slot"], "userid": frame["bot"]["userid"], "flags": frame["core"]["flags"], "position": frame["bot"]["pos"], "velocity": frame["core"]["vel"]} for frame in resumed]
                    result["endpoint"] = stop_policy.evaluate(endpoint, samples, samples[-1])
                    require(result["endpoint"]["precise_stop_passed"], "No final strict endpoint settle")
                result.update(real_hurt_events=len(hits), route_owner=owner, goal=goal)
            else:
                quiet = stages["quiet"]
                require(stages["run"][-1]["combat"]["owner"] > 0, "Stop was not exercised during sampled combat ownership")
                stop_commands = [command for command in commands["quiet"] if re.search(r"\bagpb_bot_stop\s+" + str(metadata.get("subject_index", 0)) + r"\b", command)]
                require(stop_commands and quiet[0]["core"]["reset"] == "stop", "No explicit native stop of the fighting subject")
                no_attack(quiet, quiet[0], [row for row in subject_events if row["tick"] >= quiet[0]["core"]["tick"]])
                for frame in quiet:
                    lifecycle.clean(frame["core"])
                    require(frame["combat"]["ent"] == -1 and frame["combat"]["owner"] == 0 and frame["combat"]["state"] == "stopped", "Stop retained combat state")
                require(not any(row["event"] == "weapon_fire" and row["tick"] >= quiet[0]["core"]["tick"] for row in subject_events), "Real fire continued after stop")
        result.update(status="passed", samples={name: len(stage) for name, stage in stages.items()}, real_fire_events=sum(row["event"] == "weapon_fire" for row in subject_events), guard_scope="raw_active_intact_and_declared_server_hash")
    except (ValueError, KeyError, TypeError, OverflowError, IndexError) as exc:
        result["error"] = str(exc)
    return result


def evaluate(document):
    require(isinstance(document, dict) and document.get("schema_version") == VERSION, "Unknown combat evidence schema")
    results = [evaluate_case(case, document["metadata"]) for case in document.get("cases", [])]
    missing = sorted(set(KINDS) - {row["kind"] for row in results if row["status"] == "passed"})
    return {"schema_version": VERSION, "scope": "sampled_minimum_combat_only", "status": "passed" if not missing and all(row["status"] == "passed" for row in results) else "incomplete", "pending_kinds": missing, "cases": results}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("evidence", type=Path)
    args = ap.parse_args()
    try:
        report = evaluate(json.loads(args.evidence.read_text(encoding="utf-8-sig")))
        print(json.dumps(report, ensure_ascii=True))
        return 0 if report["status"] == "passed" else 1
    except (ValueError, KeyError, TypeError, OSError) as exc:
        print(json.dumps({"status": "incomplete", "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
