#!/usr/bin/env python3
"""Offline M3b clearance acceptance from original console receipts only.

API: parse(log), evaluate_case(case, metadata), evaluate(document).
Success kinds use before/run stages; bounded_exit and interrupt kinds also use
quiet. Each stage contains unedited ConsoleSender receipts. A case specifies
kind, seven-hash binding, goal, original target_refs, and target endpoint data.
The waypoint snapshot describes existing AGPW; it is not an authored new plan.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

import stop_policy
import verify_lifecycle as lifecycle

VERSION = 1
KINDS = ("gun", "knife", "chain", "bounded_exit", "stop_interrupt", "death_interrupt")
HASHES = tuple(name + "_sha256" for name in ("waypoint_snapshot", "agpw", "bsp", "dll", "player_probe", "fixture", "clearance_probe"))
TOKEN = re.compile(r'([a-zA-Z_][a-zA-Z_0-9]*)=(\([^)]*\)|"[^"\r\n]*"|[^\s]+)')
# Exact schemas reject unknown/new diagnostics until their policy is reviewed.
SPECS = {
    "clearance": ("v edict userid tick ent route goal owner index node next breaktoken shots nodamage besthealth near release knifepending clip ammotype reserve cmdnum cmdtick weaponselect attackcmds lastattacktick", "time acquired deadline nextattack playerattack", "map class target trackedweapon weapon buttons", {"move": 3, "view": 2}),
    "targets": ("v epoch map_serial tick watch generation ent ref exists damage_hooked initial_health_known initial_health health_known health broken_known broken pos_known damage_seq break_seq destroy_seq", "t", "map class name model", {"pos": 3}),
    "weapons": ("v epoch map_serial tick slot userid serial fake alive active_ent active_ref clip_known clip ammotype_known ammotype reserve_known reserve reload_known reload primary_known secondary_known player_attack_known smack_known client_fires client_damages", "t primary secondary player_attack smack_time", "map class", {}),
    "counts": ("v epoch map_serial tick seq damage_calls breaks destroys fires watched cached break_prop_hooked break_breakable_hooked weapon_fire_hooked", "t", "map", {}),
    "clearance_events": ("v epoch map_serial tick seq watch generation ent ref damage_id context_valid attacker_slot attacker_userid attacker_serial attacker_fake weapon_ref active_ref smack_known damage_known damagetype health_before_known health_before health_after_known health_after replay", "t smack_time damage", "map event class attribution weapon_class active_class event_weapon", {}),
}
TAGS = {"[AgPB clearance] ": "clearance", **{"[WP_CLEARANCE_" + tag + "] ": key for tag, key in (("TARGET", "targets"), ("WEAPON", "weapons"), ("COUNTS", "counts"), ("EVENT", "clearance_events"))}}
require = lifecycle.require


FIXTURE_BASE = ("v arm tick slot userid serial human_slot human_userid human_serial core_idx", "t", "action mode")
FIXTURE_EXTRA = {
    "armed": ("", "deadline timeout", "trigger dispatch"),
    "triggered": ("arm_tick", "", "event weapon"),
    "before": ("arm_tick trigger_tick", "", "weapon"),
    "done": ("arm_tick trigger_tick snapshot_tick alive", "", "weapon"),
    "cancelled": ("arm_tick trigger_tick", "", "reason"),
}
IDENTITY_KEYS = {"map", "bot_slot", "bot_userid", "bot_name", "human_slot", "human_userid"}


def decode(text, kind):
    name = None
    if kind == "clearance":
        match = re.fullmatch(r"v=(\d+) name=(.+?) edict=(.*)", text)
        require(match is not None, "Malformed clearance identity")
        text, name = "v=" + match[1] + " edict=" + match[3], match[2]
    raw, cursor = {}, 0
    for match in TOKEN.finditer(text):
        require(not text[cursor:match.start()].strip() and match[1] not in raw, "Malformed/duplicate clearance field")
        raw[match[1]], cursor = match[2], match.end()
    require(not text[cursor:].strip(), "Malformed clearance suffix")
    if kind == "fixture":
        require(raw.get("action") in FIXTURE_EXTRA, "Unknown clearance fixture action")
        extra = FIXTURE_EXTRA[raw["action"]]
        ints, floats, strings = [base + " " + suffix for base, suffix in zip(FIXTURE_BASE, extra)]
        vectors = {}
    else:
        ints, floats, strings, vectors = SPECS[kind]
    lifecycle.exact_keys(raw, set(ints.split() + floats.split() + strings.split()) | set(vectors), kind)
    row = {key: int(raw[key]) for key in ints.split()}
    row.update({key: lifecycle.number(raw[key]) for key in floats.split()})
    row.update({key: raw[key][1:-1] if raw[key].startswith('"') else raw[key] for key in strings.split()})
    row.update({key: lifecycle.vector(raw[key], count) for key, count in vectors.items()})
    require(row["v"] == VERSION, "Unsupported clearance diagnostic version")
    for key in row:
        if key.endswith(("_known", "_hooked")) or key in ("exists", "context_valid", "near", "release", "knifepending", "replay"):
            require(row[key] in (0, 1), "Unknown boolean: " + key)
    if kind == "clearance_events":
        expected = {"damage_post": "damage_callback", "destroy": "destruction_damage_context", "weapon_fire": "game_event", "break_prop": "game_event", "break_breakable": "game_event"}
        require(row["event"] in expected and row["attribution"] == expected[row["event"]], "Unknown clearance event/attribution")
    if kind not in ("clearance", "fixture"):
        require(row["epoch"] > 0 and row["map_serial"] > 0 and row["tick"] >= 0, "Unknown clearance probe identity/time")
    if kind == "clearance":
        row["name"] = name
        for key in ("target", "trackedweapon", "weapon", "buttons"):
            require(re.fullmatch(r"0x[0-9A-Fa-f]+", row[key]), "Malformed opaque handle/buttons")
        row["buttons"] = int(row["buttons"], 16)
        require(row["attackcmds"] >= 0 and 0 <= row["tick"] - row["cmdtick"] <= 2, "Unknown/stale submitted command")
    return row


def parse(log):
    """Retain the unchanged lifecycle parser and add separate clearance tags."""
    result = lifecycle.parse(log)
    result.update({key: [] for key in SPECS})
    result["fixture"] = []
    for line in log.splitlines():
        if line.strip().startswith("[AGPB_REG_CLEARANCE] "):
            result["fixture"].append(decode(line.strip()[len("[AGPB_REG_CLEARANCE] "):], "fixture"))
        for prefix, kind in TAGS.items():
            if line.strip().startswith(prefix):
                result[kind].append(decode(line.strip()[len(prefix):], kind))
                break
    return result


def snapshot_target(data, metadata, index):
    """Get an endpoint from the hash-bound read-only AGPW snapshot bytes."""
    require(hashlib.sha256(data).hexdigest() == metadata["waypoint_snapshot_sha256"], "Waypoint snapshot hash differs")
    snapshot = json.loads(data.decode("utf-8-sig"))
    require(snapshot.get("source_kind") == "existing_agpw_readonly_observation" and snapshot.get("manual_authorship") is False,
            "Waypoint data is not the declared existing-AGPW observation")
    require(snapshot["map"] == metadata["map"] and snapshot["source_sha256"] == metadata["agpw_sha256"], "Snapshot map/AGPW version differs")
    node = snapshot["nodes"][index]
    require(node["index"] == index, "Snapshot node index differs")
    return {"index": index, **{key: node[key] for key in ("x", "y", "z", "radius")}}


def collect(receipts, metadata, seen, offset):
    frames, events, logs, fixture = [], {}, [], []
    for receipt in receipts:
        log, marker, finish = lifecycle.receipt_log(receipt)
        require(marker not in seen and finish > offset[0], "Receipt was reused/reordered")
        seen.add(marker)
        if "observed_log" in receipt:
            start = receipt.get("observed_start_offset")
            end = receipt.get("observed_end_offset")
            require(type(start) is int and type(end) is int and offset[0] <= start <= finish <= end, "Observed offsets are missing/reused")
            offset[0] = end
        else:
            offset[0] = finish
        data = parse(log)
        logs.append(log)
        fixture.extend(data["fixture"])
        for event in data["clearance_events"]:
            key = event["epoch"], event["seq"]
            plain = {k: v for k, v in event.items() if k != "replay"}
            require(key not in events or events[key] == plain, "Replayed event sequence changed contents")
            events[key] = plain
        # The fire-trigger fixture can emit a complete before snapshot outside
        # ACK immediately before another complete native snapshot. Preserve the
        # receipt once, pairing each original contiguous block independently.
        blocks = re.split(r"(?m)(?=^\[AgPB lifecycle\] )", log)
        for block in blocks:
            data = parse(block)
            if not data["core"]:
                continue
            require(len(data["core"]) == 1, "Ambiguous lifecycle snapshot block")
            life = data["core"][0]
            require((life["edict"], life["userid"], life["name"], life["map"]) == (metadata["bot_slot"], metadata["bot_userid"], metadata["bot_name"], metadata["map"]), "Lifecycle identity changed")
            players = {p["slot"]: p for p in data["players"]}
            require(len(players) == len(data["players"]), "Ambiguous players in snapshot block")
            bot, human = players.get(metadata["bot_slot"]), players.get(metadata["human_slot"])
            require(bot and human and bot["fake"] == 1 and human["fake"] == 0 and bot["userid"] == metadata["bot_userid"] and human["userid"] == metadata["human_userid"], "Independent player identity changed")
            require(human["team"] in (2, 3) and all(p["team"] == human["team"] for p in players.values() if p["fake"] and p["team"] in (2, 3)), "Every playing bot must share the human team")
            require((bot["epoch"], bot["map_serial"]) == (human["epoch"], human["map_serial"]) and abs(bot["tick"] - life["tick"]) <= 2, "Player snapshot scene/tick differs")
            require(bot["team"] == life["team"] and bot["alive"] == life["alive"] and bot["life_state"] == life["lifestate"] and bool(bot["flags"] & 1) == bool(life["flags"] & 1) and bot["frozen"] == int(bool(life["flags"] & lifecycle.CORE_FL_FROZEN)), "Core and independent player probe disagree")
            require(math.dist(bot["pos"], life["pos"]) <= 1 and life["auto"] == life["roam"] == 0, "Player position/manual-navigation isolation differs")
            pair = dict(core=life, bot=bot, human=human)
            rows = [row for row in data["clearance"] if row["tick"] == life["tick"]]
            require(len(rows) == 1, "Each lifecycle sample needs one matching clearance snapshot")
            clear = rows[0]
            require((clear["edict"], clear["userid"], clear["name"], clear["map"]) == (life["edict"], life["userid"], life["name"], life["map"]), "Clearance bot identity changed")
            require(clear["ent"] == life["break"], "Clearance target differs from lifecycle break handle")
            for key in ("route", "goal", "owner", "index", "node", "breaktoken", "cmdnum", "cmdtick", "buttons", "move", "view"):
                require(clear[key] == life[key], "Clearance/lifecycle disagree: " + key)
            weapons = [w for w in data["weapons"] if abs(w["tick"] - life["tick"]) <= 2]
            counts = [w for w in data["counts"] if abs(w["tick"] - life["tick"]) <= 2]
            require(len(weapons) == len(counts) == 1, "Missing/ambiguous independent weapon/counter snapshot")
            weapon, count = weapons[0], counts[0]
            require((weapon["slot"], weapon["userid"], weapon["serial"], weapon["fake"], weapon["alive"]) == (life["edict"], life["userid"], pair["bot"]["serial"], 1, life["alive"]), "Independent weapon actor changed")
            require((weapon["epoch"], weapon["map_serial"], weapon["map"]) == (count["epoch"], count["map_serial"], metadata["map"]), "Clearance probe scene changed")
            no_weapon = weapon["active_ent"] <= 0 and weapon["active_ref"] == -1 and clear["class"] == "none" and weapon["class"] == "unknown"
            require(no_weapon or clear["class"] == weapon["class"], "Core/probe weapon classes disagree")
            targets = {t["ref"]: t for t in data["targets"] if abs(t["tick"] - life["tick"]) <= 2}
            require(count["seq"] >= 0 and all(0 <= t[key] <= count["seq"] for t in targets.values() for key in ("damage_seq", "break_seq", "destroy_seq")), "Target event sequence contradicts probe counters")
            require(all((t["epoch"], t["map_serial"], t["map"]) == (weapon["epoch"], weapon["map_serial"], weapon["map"]) for t in targets.values()), "Target probe scene differs")
            frames.append(dict(pair, clearance=clear, weapon=weapon, counts=count, targets=targets))
    return frames, list(events.values()), logs, fixture


def actual_actor(event, frame):
    return (event["attacker_slot"], event["attacker_userid"], event["attacker_serial"], event["attacker_fake"]) == (frame["bot"]["slot"], frame["bot"]["userid"], frame["bot"]["serial"], 1)


def applied(event, events):
    # SDKHooks can lose the Post callback when TakeDamage deletes its victim.
    # Require the saved Pre context plus a same-call real break with a measured
    # health transition; neither destruction nor a break event alone suffices.
    if event["event"] == "destroy" and event["context_valid"] and event["attribution"] == "destruction_damage_context" and event["damage_id"] > 0 and event["health_before_known"] and event["health_before"] > 0:
        actor = tuple(event[key] for key in ("attacker_slot", "attacker_userid", "attacker_serial", "attacker_fake"))
        return any(e["event"] in ("break_prop", "break_breakable") and e["ref"] == event["ref"] and e["damage_id"] == event["damage_id"] and e["context_valid"] and e["health_after_known"] and e["health_after"] <= 0 and e["health_after"] < event["health_before"] and tuple(e[key] for key in ("attacker_slot", "attacker_userid", "attacker_serial", "attacker_fake")) == actor for e in events)
    if event["event"] != "damage_post" or not event["context_valid"] or not event["damage_known"] or event["damage"] <= 0:
        return False
    if event["health_before_known"] and event["health_after_known"] and event["health_after"] < event["health_before"]:
        return True
    return event["damage_id"] > 0 and any(e["ref"] == event["ref"] and e["damage_id"] == event["damage_id"] and e["event"] in ("break_prop", "break_breakable", "destroy") and e["context_valid"] for e in events)


def endpoint(target, frames):
    samples = [{"time": f["core"]["time"], "slot": f["bot"]["slot"], "userid": f["bot"]["userid"], "flags": f["bot"]["flags"], "position": f["bot"]["pos"], "velocity": f["bot"]["vel"]} for f in frames]
    return stop_policy.evaluate(target, samples, samples[-1])


def quiet_window(frames, pending):
    require(frames, "No post-exit samples")
    attack_count = frames[0]["clearance"]["attackcmds"]
    for frame in frames:
        lifecycle.clean(frame["core"])
        clear = frame["clearance"]
        require(frame["bot"]["buttons"] == 0 and clear["ent"] == -1 and clear["breaktoken"] == clear["shots"] == clear["nodamage"] == 0 and clear["deadline"] == 0 and clear["acquired"] == -1,
                "Exit retained obstacle/input state")
        require(clear["attackcmds"] == attack_count and clear["lastattacktick"] <= frames[0]["core"]["tick"], "Attack commands continued after exit")
    settled = [f for f in frames if f["core"]["time"] >= pending]
    lifecycle.continuous(settled, 0.8)
    return settled[0]["core"]["tick"]


def fixture_transition(rows, mode, baseline, frames, events):
    """Verify original next-frame fixture receipts, never its armed promise."""
    completed = [row for row in rows if row["action"] == "done" and row["mode"] == mode]
    require(len(completed) == 1, "Missing/ambiguous completed fire-trigger fixture")
    done = completed[0]
    group = [row for row in rows if row["arm"] == done["arm"]]
    found = {}
    for action in ("armed", "triggered", "before", "done"):
        values = [row for row in group if row["action"] == action]
        require(len(values) == 1, "Fixture sequence lacks unique " + action)
        found[action] = values[0]
    require(not any(row["action"] == "cancelled" for row in group), "Fire-trigger fixture was cancelled")
    actor = (baseline["bot"]["slot"], baseline["bot"]["userid"], baseline["bot"]["serial"], baseline["human"]["slot"], baseline["human"]["userid"], baseline["human"]["serial"])
    require(all((row["slot"], row["userid"], row["serial"], row["human_slot"], row["human_userid"], row["human_serial"]) == actor and row["mode"] == mode and row["core_idx"] == 0 for row in group), "Fixture participant identity changed")
    armed, trigger, before = found["armed"], found["triggered"], found["before"]
    require(armed["trigger"] == trigger["event"] == "weapon_fire" and armed["dispatch"] == "RequestFrame", "Unknown fixture dispatch")
    require(armed["tick"] <= trigger["tick"] < before["tick"] == done["snapshot_tick"] == done["tick"] and trigger["arm_tick"] == before["arm_tick"] == done["arm_tick"] == armed["tick"] and before["trigger_tick"] == done["trigger_tick"] == trigger["tick"] and done["t"] <= armed["deadline"], "Fixture action was not after the original fire tick")
    require(any(e["event"] == "weapon_fire" and e["tick"] == trigger["tick"] and e["event_weapon"] == trigger["weapon"] and actual_actor(e, baseline) for e in events), "Fixture trigger lacks independent real weapon_fire")
    snapshots = [f for f in frames if f["core"]["tick"] == before["tick"] and f["clearance"]["ent"] >= 0 and f["clearance"]["breaktoken"] > 0]
    require(len(snapshots) == 1, "Fire-trigger snapshot was not an active obstacle attack")
    snapshot = snapshots[0]
    clear = snapshot["clearance"]
    exists = any(t["ent"] == clear["ent"] and t["exists"] for t in snapshot["targets"].values())
    original = {ref for ref, target in baseline["targets"].items() if target["ent"] == clear["ent"] and target["exists"]}
    # Hitscan can delete the prop before the next-frame interruption. The core
    # still owns that break and its last submitted IN_ATTACK awaits release.
    # Require the real preceding shot/application, not the fixture's promise.
    pending_release = bool(clear["buttons"] & 1) and clear["release"] == 1 and clear["cmdtick"] == clear["lastattacktick"] == trigger["tick"] and any(e["ref"] in original and e["tick"] == trigger["tick"] and actual_actor(e, baseline) and applied(e, events) for e in events)
    require(exists or pending_release, "Fire-trigger snapshot lacks a surviving target or verified attack awaiting release")
    return done, snapshot


def evaluate_case(case, metadata):
    if not isinstance(case, dict) or not isinstance(metadata, dict):
        return {"kind": None, "status": "failed", "error": "Case/metadata must be objects"}
    result = {"kind": case.get("kind"), "status": "failed", "scope": "sampled_clearance_only", "policy_version": VERSION}
    try:
        identity = case.get("identity", {})
        require(isinstance(identity, dict) and set(identity) <= IDENTITY_KEYS, "Unknown case identity override")
        metadata = dict(metadata, **identity)
        kind = case["kind"]
        require(kind in KINDS, "Unknown clearance case kind")
        require(all(isinstance(metadata.get(key), str) and re.fullmatch(r"[a-f0-9]{64}", metadata[key]) and case.get("binding", {}).get(key) == metadata[key] for key in HASHES), "Missing/mixed clearance artifact binding")
        stages = case["stages"]
        success = kind in ("gun", "knife", "chain")
        require(set(stages) == ({"before", "run"} if success else {"before", "run", "quiet"}), "Missing/unknown clearance stages")
        seen, offset, parsed, all_events, logs, fixture_rows = set(), [0], {}, {}, [], []
        setup = case.get("setup")
        setup_frames = []
        if setup:
            require(kind == "bounded_exit" and case.get("expected_exit") == "timeout", "Diagnostic setup is only supported for timeout evidence")
            setup_frames, setup_events, setup_logs, _ = collect(setup["command_receipts"], metadata, seen, offset)
            logs.extend(setup_logs)
        for name in ("before", "run", "quiet"):
            if name not in stages:
                continue
            frames, events, text, fixture = collect(stages[name], metadata, seen, offset)
            fixture_rows.extend(fixture)
            parsed[name] = frames
            logs.extend(text)
            for e in events:
                key = e["epoch"], e["seq"]
                require(key not in all_events or all_events[key] == e, "Event replay changed across stages")
                all_events[key] = e
        require(parsed["before"] and parsed["run"], "Missing actual before/run samples")
        fixture_done, fixture_before = None, None
        if kind.endswith("interrupt") and fixture_rows:
            combined = parsed["run"] + parsed["quiet"]
            fixture_done, fixture_before = fixture_transition(fixture_rows, "stop" if kind == "stop_interrupt" else "death", parsed["before"][-1], combined, list(all_events.values()))
            boundary = next(i for i, frame in enumerate(combined) if frame is fixture_before)
            parsed["run"] = combined[:boundary + 1]
            parsed["quiet"] = [f for f in combined[boundary + 1:] if f["clearance"]["cmdnum"] > fixture_before["clearance"]["cmdnum"]]
            result["transition_samples"] = len(combined) - len(parsed["run"]) - len(parsed["quiet"])
        if kind == "bounded_exit":
            combined = parsed["run"] + parsed["quiet"]
            attempts = [i for i, f in enumerate(combined) if f["clearance"]["ent"] >= 0 and f["clearance"]["breaktoken"] > 0]
            if attempts:
                cleared = next((i for i in range(attempts[-1] + 1, len(combined)) if combined[i]["core"]["route"] == 0 and combined[i]["clearance"]["ent"] == -1), None)
                if cleared is not None:
                    parsed["run"], parsed["quiet"] = combined[:cleared], combined[cleared:]
        for values in parsed.values():
            lifecycle.continuous(values)
        baseline, final = parsed["before"][-1], parsed["quiet" if not success else "run"][-1]
        frames = [f for name in ("before", "run", "quiet") for f in parsed.get(name, [])]
        require(baseline["core"]["alive"] and baseline["human"]["alive"], "Baseline players must be alive")
        for previous, frame in zip(frames, frames[1:]):
            require(frame["core"]["tick"] >= previous["core"]["tick"] and frame["clearance"]["cmdnum"] > previous["clearance"]["cmdnum"] and frame["clearance"]["attackcmds"] >= previous["clearance"]["attackcmds"], "Reversed samples/attack counters")
        for frame in frames:
            require((frame["bot"]["serial"], frame["bot"]["epoch"], frame["bot"]["map_serial"], frame["weapon"]["epoch"], frame["weapon"]["map_serial"]) == (baseline["bot"]["serial"], baseline["bot"]["epoch"], baseline["bot"]["map_serial"], baseline["weapon"]["epoch"], baseline["weapon"]["map_serial"]), "Player/probe/map generation changed")
        if setup:
            value = setup.get("controlled_player_cooldown")
            require(stop_policy.finite_number(value) and value > final["core"]["time"], "Controlled cooldown must remain a finite future time")
            writes = [match for r in setup["command_receipts"] for match in re.finditer(r"\bagpb_netwrite\s+(\d+)\s+m_flNextAttack\s+([-+0-9.eE]+)(?:\s*;|\s*$)", r["command"])]
            require(len(writes) == 1 and int(writes[0][1]) == metadata.get("bot_index", 0) and float(writes[0][2]) == value and sum(len(re.findall(r"\bagpb_netwrite\b", r["command"])) for r in setup["command_receipts"]) == 1, "Diagnostic cooldown requires one original player-only netwrite")
            require(setup_frames and all(f["core"]["route"] == 0 and f["core"]["alive"] and f["bot"]["serial"] == baseline["bot"]["serial"] and f["weapon"]["epoch"] == baseline["weapon"]["epoch"] and f["weapon"]["map_serial"] == baseline["weapon"]["map_serial"] for f in setup_frames), "Cooldown was not prepared on the same alive player without a route")
            reads = [setup_frames[-1], *frames]
            require(all(f["weapon"]["player_attack_known"] and abs(f["weapon"]["player_attack"] - value) <= 0.001 and abs(f["clearance"]["playerattack"] - value) <= 0.001 for f in reads), "Independent cooldown readback changed/missing")
            require(final["clearance"]["attackcmds"] == baseline["clearance"]["attackcmds"], "Controlled future cooldown unexpectedly submitted an attack")
            result.update(scenario_scope="diagnostic_player_attack_cooldown_timeout", controlled_player_cooldown=value)
        refs = case["target_refs"]
        require(isinstance(refs, list) and refs and len(refs) == len(set(refs)) and all(type(ref) is int and ref != -1 for ref in refs), "Unknown/duplicate target references")
        require(all(ref in baseline["targets"] and baseline["targets"][ref]["exists"] and baseline["targets"][ref]["damage_hooked"] for ref in refs), "Original watched targets are missing/unhooked")
        new_events = [e for e in all_events.values() if e["epoch"] == baseline["weapon"]["epoch"] and e["map_serial"] == baseline["weapon"]["map_serial"] and e["map"] == metadata["map"] and baseline["counts"]["seq"] < e["seq"] <= final["counts"]["seq"]]
        require({e["seq"] for e in new_events} == set(range(baseline["counts"]["seq"] + 1, final["counts"]["seq"] + 1)), "Real event sequence is truncated/incomplete")
        require(all(baseline["core"]["tick"] <= e["tick"] <= final["core"]["tick"] + 2 for e in new_events), "New event sequence has stale/future ticks")
        for counter, names in (("damage_calls", ("damage_post",)), ("breaks", ("break_prop", "break_breakable")), ("destroys", ("destroy",)), ("fires", ("weapon_fire",))):
            require(final["counts"][counter] - baseline["counts"][counter] == sum(e["event"] in names for e in new_events), "Real event counter contradicts retained events: " + counter)
        for ref in refs:
            old = baseline["targets"][ref]
            for frame in frames:
                require(ref in frame["targets"] and (frame["targets"][ref]["ent"], frame["targets"][ref]["generation"], frame["targets"][ref]["class"]) == (old["ent"], old["generation"], old["class"]), "Watched entity identity was replaced/reused")
            require(all((e["ent"], e["generation"], e["class"]) == (old["ent"], old["generation"], old["class"]) for e in new_events if e["ref"] == ref), "Damage target identity contradicts original watch")
        goal = case["goal"]
        active = [f for f in parsed["run"] if f["core"]["route"] > 0]
        require(active, "No owned route was sampled")
        owner = active[0]["core"]["owner"]
        require(owner > 0 and all(f["core"]["goal"] == goal and f["core"]["owner"] == owner for f in active), "Original route goal/owner changed")
        require(not any(re.search(r"\bagpb_netwrite\b", r["command"]) for name in stages for r in stages[name]), "Netwrite during acceptance is not a one-time diagnostic setup")
        commands = [r["command"] for name in ("before", "run") for r in stages[name]]
        gotos = re.findall(r"\bagpb_bot_goto\s+\d+\s+(\d+)", ";".join(commands))
        require(gotos == [str(goal)], "One original goto is required; route replacement cannot prove resumption")
        require(not any(re.search(r"\b(?:ent_fire|ent_remove|ent_create|agpb_bot_testweapon|agpb_wp_load|agpb_wp_clear)\b", r["command"]) for r in stages["run"]), "External scene/weapon mutation during acceptance")
        if kind != "bounded_exit" or case.get("expected_exit") != "timeout":
            require(final["clearance"]["attackcmds"] > baseline["clearance"]["attackcmds"], "No real submitted IN_ATTACK commands")
        bot_events = [e for e in new_events if actual_actor(e, baseline)]
        damage = [e for e in bot_events if e["ref"] in refs and applied(e, new_events)]
        if success or kind.endswith("interrupt"):
            require(damage, "No independent applied damage attributed to the original bot")
        if kind.endswith("interrupt"):
            interrupted = parsed["run"][-1]
            require(interrupted["clearance"]["ent"] >= 0 and interrupted["clearance"]["breaktoken"] > 0 and (fixture_before is interrupted or any(t["ent"] == interrupted["clearance"]["ent"] and t["exists"] for t in interrupted["targets"].values())), "Interruption was not exercised during an active obstacle attack")
        if kind in ("gun", "knife"):
            knife = kind == "knife"
            weapon_classes = [e["weapon_class"] if e["weapon_class"] != "unknown" else e["active_class"] for e in damage]
            require(any(name == "weapon_knife" if knife else name.startswith("weapon_") and name not in ("weapon_knife", "weapon_hegrenade", "weapon_flashbang", "weapon_smokegrenade") for name in weapon_classes), "Real damage weapon does not match case")
            if knife:
                require(any(e["weapon_class"] == "weapon_knife" or (e["weapon_class"] == "unknown" and e["active_class"] == "weapon_knife" and e["smack_known"] and baseline["core"]["time"] <= e["smack_time"] <= e["t"] + 0.02) for e in damage), "Unknown knife DamageInfo weapon requires real active-knife Smack context")
            if not knife:
                require(any(e["event"] == "weapon_fire" and e["event_weapon"] not in ("knife", "unknown") for e in bot_events), "No real firearm fire event")
        text = "\n".join(logs)
        if success:
            require(kind != "chain" or len(refs) >= 2, "Continuous clearance needs two original distinct targets")
            resolutions = []
            for ref in refs:
                old = baseline["targets"][ref]
                hits = [e for e in damage if e["ref"] == ref]
                breaks = [e for e in bot_events if e["ref"] == ref and e["event"] in ("break_prop", "break_breakable")]
                after = final["targets"][ref]
                require(hits and breaks and (not after["exists"] or (after["broken_known"] and after["broken"]) or (after["health_known"] and after["health"] <= 0)), "No real bot destruction of original target " + str(ref))
                acquired = re.search(re.escape("[AgPB] " + metadata["bot_name"]) + rf": breaking obstacle ent={old['ent']}; preserving hop \d+ and goal #{goal}\b", text)
                require(acquired is not None, "Missing same-route acquisition of original target")
                current = [f["clearance"] for f in active if f["clearance"]["ent"] == old["ent"]]
                require(not current or len({row["target"] for row in current}) == 1, "Core target handle changed within this watched entity")
                resolutions.append(min(e["tick"] for e in breaks))
            require(len(set(resolutions)) == len(resolutions), "Continuous targets were not individually resolved")
            last = max(resolutions)
            continued = [f for f in parsed["run"] if f["core"]["tick"] > last]
            require(len(continued) >= 2 and any(not lifecycle.released(f["core"]) for f in continued) and any(math.dist(continued[0]["bot"]["pos"], f["bot"]["pos"]) > 1 for f in continued[1:]), "No real locomotion after clearance")
            target = case["target"]
            require(target["index"] == goal, "Endpoint differs from original goal")
            result["endpoint"] = endpoint(target, parsed["run"])
            require(result["endpoint"]["precise_stop_passed"], "Final precise endpoint window was not established")
            require(re.search(re.escape("[AgPB] " + metadata["bot_name"]) + rf": arrived at waypoint #{goal}\b", text), "No real original-goal arrival")
        else:
            pending = max([f["weapon"]["smack_time"] for f in parsed["run"] if f["weapon"]["class"] == "weapon_knife" and f["weapon"]["smack_known"]] + [0])
            quiet_tick = quiet_window(parsed["quiet"], pending)
            if fixture_done:
                require(all(f["clearance"]["attackcmds"] == fixture_before["clearance"]["attackcmds"] and f["clearance"]["lastattacktick"] <= fixture_done["tick"] for f in parsed["quiet"]), "New IN_ATTACK after the fixture interruption")
            require(not any(e["tick"] >= quiet_tick and e["event"] in ("weapon_fire", "damage_post") for e in bot_events), "New attack/damage after the settled exit window")
            if kind == "bounded_exit":
                attempts = [f["clearance"] for f in active if f["clearance"]["ent"] >= 0 and f["clearance"]["breaktoken"] > 0 and f["clearance"]["acquired"] >= 0]
                require(attempts, "No acquired bounded obstacle attempt")
                attempt = attempts[-1]
                require(abs(attempt["deadline"] - attempt["acquired"] - 10.0) <= 0.002 and any(baseline["targets"][ref]["ent"] == attempt["ent"] for ref in refs), "Budget attempt is not a ten-second acquisition of an original watched target")
                failure = re.search(re.escape("[AgPB] " + metadata["bot_name"]) + rf": obstacle break failed: ([^\r\n]+) \(ent={attempt['ent']}, shots=(\d+), goal=#{goal}\) - stopped", text)
                require(failure is not None, "No native bounded obstacle failure")
                result["budget"] = dict(acquired=attempt["acquired"], deadline=attempt["deadline"],
                    last_active_time=parsed["run"][-1]["core"]["time"], first_clear_time=parsed["quiet"][0]["core"]["time"],
                    elapsed_upper_bound=parsed["quiet"][0]["core"]["time"] - attempt["acquired"], reason=failure[1], confirmed_attacks=int(failure[2]))
                require(parsed["quiet"][0]["core"]["time"] <= attempt["acquired"] + 11.5, "Failure exceeded its ten-second budget and sampling allowance")
                if case.get("expected_exit") == "timeout":
                    require(failure[1] == "10 second timeout" and parsed["quiet"][0]["core"]["time"] >= attempt["deadline"], "No real deadline timeout")
                else:
                    attempted_ref = next(ref for ref in refs if baseline["targets"][ref]["ent"] == attempt["ent"])
                    last_damage = max((e for e in damage if e["ref"] == attempted_ref), key=lambda e: e["seq"], default=None)
                    cutoff = last_damage["seq"] if last_damage else baseline["counts"]["seq"]
                    fires = [e for e in bot_events if e["event"] == "weapon_fire" and e["seq"] > cutoff and attempt["acquired"] - 0.002 <= e["t"] <= parsed["quiet"][0]["core"]["time"]]
                    readings = [f["targets"][attempted_ref] for f in frames if last_damage is None or f["core"]["tick"] > last_damage["tick"]]
                    known_unchanged = readings and all(t["exists"] and t["health_known"] for t in readings) and len({t["health"] for t in readings}) == 1
                    require(case.get("expected_exit") == "no_damage_budget" and failure[1] == "six attacks without confirmed damage/destruction" and int(failure[2]) >= 6 and len(fires) >= 6 and known_unchanged, "Six real unsuccessful attacks with known unchanged health were not demonstrated")
            elif kind == "stop_interrupt":
                require((fixture_done is not None or any(re.search(r"\bagpb_bot_stop\s+\d+", r["command"]) for name in ("run", "quiet") for r in stages[name])) and parsed["quiet"][0]["core"]["reset"] == "stop", "No explicit stop of the attacking bot")
            else:
                after = parsed["quiet"][-1]
                require(not after["core"]["alive"] and after["bot"]["client_deaths"] > baseline["bot"]["client_deaths"], "No real death of the attacking identity")
                events = [e for name in ("run", "quiet") for r in stages[name] for e in lifecycle.parse(lifecycle.receipt_log(r)[0])["events"]]
                lifecycle.real_event({"events": events}, "player_death", baseline, after, baseline["bot"]["lifecycle_seq"])
        result.update(status="passed", goal=goal, route_owner=owner, target_refs=refs, applied_damage_events=len(damage), submitted_attacks=final["clearance"]["attackcmds"] - baseline["clearance"]["attackcmds"], samples={name: len(values) for name, values in parsed.items()})
    except (ValueError, KeyError, TypeError, OverflowError) as exc:
        result["error"] = str(exc)
    return result


def evaluate(document):
    require(isinstance(document, dict) and document.get("schema_version") == VERSION, "Unsupported clearance evidence schema")
    results = [evaluate_case(case, document["metadata"]) for case in document.get("cases", [])]
    missing = sorted(set(KINDS) - {row["kind"] for row in results if row["status"] == "passed"})
    return {"schema_version": VERSION, "scope": "sampled_clearance_only", "status": "passed" if not missing and all(row["status"] == "passed" for row in results) else "incomplete", "pending_kinds": missing, "cases": results}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("evidence", type=Path)
    args = ap.parse_args()
    try:
        report = evaluate(json.loads(args.evidence.read_text(encoding="utf-8-sig")))
        print(json.dumps(report, ensure_ascii=True))
        return 0 if report["status"] == "passed" else 1
    except (ValueError, KeyError, TypeError, OSError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
