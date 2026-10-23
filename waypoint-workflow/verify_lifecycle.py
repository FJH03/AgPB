#!/usr/bin/env python3
"""Re-evaluate sampled M3a lifecycle evidence without controlling the game.

API: evaluate_case(case, metadata), evaluate(document), parse(log).
A case has kind, binding (six artifact SHA256s), and stages mapping names to
unaltered ConsoleSender receipts. observed_log may retain events outside ACK;
it must contain the complete original framed log. No recorded passed flag is
trusted. This certifies only the supplied samples, not unsampled game time.
"""
import argparse
import json
import math
from pathlib import Path
import re

import stop_policy

VERSION = 1
KINDS = ("stop", "freeze_resume", "freeze_settle", "death_respawn", "team", "round", "map")
HASHES = tuple(name + "_sha256" for name in ("plan", "agpw", "bsp", "dll", "probe", "fixture"))
# Raw SDK m_fFlags and SourceMod GetEntityFlags use different flag layouts.
# const.h (multiplayer branch) and entity_prop_stocks.inc define these separately.
CORE_FL_FROZEN = 1 << 6
PROBE_FL_FROZEN = 1 << 5
FIELDS = re.compile(r"([a-zA-Z_][a-zA-Z_0-9]*)=(\([^)]*\)|[^\s]+)")
CORE_INTS = set("v edict userid epoch tick alive wasalive lifestate team desired auto roam roundfrozen hp route index node goal owner arrived airborne pendingvel resets cmdnum cmdtick tasks token break breaktoken use velknown".split())
CORE_FLOATS = set("time settle settleage progress stuck recovery action".split())
CORE_VECTORS = {"pos": 3, "vel": 3, "eye": 2, "move": 3, "view": 2}
CORE_STRINGS = {"map", "task", "reset"}
CORE_KEYS = CORE_INTS | CORE_FLOATS | set(CORE_VECTORS) | CORE_STRINGS | {"flags", "buttons"}
PROBE_INTS = set("slot userid fake team alive flags zone defusing v epoch map_serial tick serial frozen view_ent buttons player_state life_state observer deaths spawns teams round_starts round_ends lifecycle_seq client_deaths client_spawns client_teams".split())
PROBE_KEYS = PROBE_INTS | {"t", "map", "pos", "vel", "eye"}
EVENT_INTS = set("v epoch map_serial tick seq userid slot serial fake team alive oldteam newteam deaths spawns teams round_starts round_ends winner reason replay".split())
EVENT_KEYS = EVENT_INTS | {"t", "map", "event"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    value = float(value)
    require(math.isfinite(value), "Nonfinite numeric evidence")
    return value


def vector(value, count):
    require(value.startswith("(") and value.endswith(")"), "Malformed vector")
    values = [number(item) for item in re.split(r"[,\s]+", value[1:-1].strip())]
    require(len(values) == count, "Wrong vector length")
    return values


def fields(text):
    result, cursor = {}, 0
    for match in FIELDS.finditer(text):
        require(not text[cursor:match.start()].strip(), "Unrecognized diagnostic text")
        require(match[1] not in result, "Duplicate diagnostic field: " + match[1])
        result[match[1]] = match[2]
        cursor = match.end()
    require(not text[cursor:].strip(), "Unrecognized diagnostic suffix")
    return result


def exact_keys(values, expected, label):
    require(set(values) == expected,
            label + " missing/unknown fields: " + ",".join(sorted(set(values) ^ expected)))


def core(text):
    match = re.fullmatch(r"v=(\d+) name=(.+?) edict=(.*)", text)
    require(match is not None, "Malformed lifecycle identity")
    values = fields("v=" + match[1] + " edict=" + match[3])
    exact_keys(values, CORE_KEYS, "Core lifecycle")
    result = {key: int(values[key]) for key in CORE_INTS}
    result.update({key: number(values[key]) for key in CORE_FLOATS})
    result.update({key: vector(values[key], count) for key, count in CORE_VECTORS.items()})
    result.update({key: values[key] for key in CORE_STRINGS})
    for key in ("flags", "buttons"):
        raw = int(values[key], 0)
        require(-(1 << 31) <= raw < (1 << 32) and raw != -1, "Unknown/out-of-range core " + key)
        result[key] = raw & 0xffffffff
    result["name"] = match[2]
    require(result["v"] == VERSION, "Unsupported core lifecycle version")
    require(result["velknown"] == 1, "Velocity is unknown")
    require(result["lifestate"] in (0, 1, 2, 3, 4), "Unknown reflected life state")
    require(not result["alive"] or result["lifestate"] == 0, "Non-alive life state was reported alive")
    require(result["edict"] > 0 and result["userid"] > 0, "Unknown bot identity")
    for key in ("alive", "wasalive", "auto", "roam", "roundfrozen", "arrived", "airborne", "pendingvel"):
        require(result[key] in (0, 1), "Unknown core boolean: " + key)
    require(result["cmdnum"] > 0 and 0 <= result["tick"] - result["cmdtick"] <= 2,
            "Actual submitted CUserCmd is missing or stale")
    return result


def probe(text):
    values = fields(text)
    exact_keys(values, PROBE_KEYS, "Player probe")
    result = {key: int(values[key]) for key in PROBE_INTS}
    result.update(t=number(values["t"]), map=values["map"],
                  pos=vector(values["pos"], 3), vel=vector(values["vel"], 3), eye=vector(values["eye"], 3))
    flags = stop_policy.normalize_flags(result["flags"])
    require(flags is not None and result["flags"] != -1, "Unknown player flags")
    require(result["buttons"] >= 0, "Unknown player buttons")
    result["flags"] = flags
    require(result["v"] == VERSION, "Unsupported player probe version")
    require(min(result[key] for key in ("slot", "userid", "serial", "epoch", "map_serial")) > 0,
            "Missing player/map identity")
    for key in ("fake", "alive", "frozen"):
        require(result[key] in (0, 1), "Unknown player boolean: " + key)
    require(result["frozen"] == int(bool(flags & PROBE_FL_FROZEN)), "Probe frozen contradicts real FL_FROZEN")
    require(all(result[key] >= 0 for key in ("client_deaths", "client_spawns", "client_teams", "lifecycle_seq", "deaths", "spawns", "teams", "round_starts", "round_ends")),
            "Unknown real event counters")
    return result


def event(text):
    values = fields(text)
    exact_keys(values, EVENT_KEYS, "Lifecycle event")
    result = {key: int(values[key]) for key in EVENT_INTS}
    result.update(t=number(values["t"]), map=values["map"], event=values["event"])
    require(result["v"] == VERSION and result["seq"] > 0 and result["replay"] in (0, 1),
            "Unknown lifecycle event version/sequence")
    return result


def parse(log):
    """Parse recognized real diagnostics; never infer a default for missing data."""
    result = {"core": [], "players": [], "events": [], "maps": []}
    for line in log.splitlines():
        line = line.strip()
        if line.startswith("[AgPB lifecycle] "):
            result["core"].append(core(line[len("[AgPB lifecycle] "):]))
        elif line.startswith("[WP_PROBE_STATE] "):
            result["players"].append(probe(line[len("[WP_PROBE_STATE] "):]))
        elif line.startswith("[WP_PROBE_LIFECYCLE] "):
            result["events"].append(event(line[len("[WP_PROBE_LIFECYCLE] "):]))
        elif line.startswith("[WP_PROBE_MAP] "):
            values = fields(line[len("[WP_PROBE_MAP] "):])
            # Map forward format is deliberately small and independent of player state.
            exact_keys(values, {"v", "epoch", "map_serial", "tick", "t", "map", "seq", "event", "replay"}, "Map forward")
            require(values.get("v") == str(VERSION), "Unknown map probe version")
            require(values.get("event") in ("map_start", "map_end"), "Unknown map forward")
            for key in ("epoch", "map_serial", "tick", "seq", "replay"):
                require(key in values, "Map forward missing " + key)
                values[key] = int(values[key])
            require("map" in values and "t" in values, "Map forward lacks map/time")
            values["t"] = number(values["t"])
            result["maps"].append(values)
    return result


def receipt_log(receipt):
    require(isinstance(receipt, dict) and type(receipt.get("returncode")) is int and receipt["returncode"] == 0, "Failed/missing command receipt")
    require(isinstance(receipt.get("command"), str), "Missing original command")
    ack = receipt.get("ack")
    require(isinstance(ack, dict) and ack.get("logResetCount") == 0, "Missing/reset console acknowledgement")
    begin, end = ack.get("beginMarker"), ack.get("endMarker")
    require(isinstance(begin, str) and re.fullmatch(r"AGPB_SESSION_BEGIN_[A-Za-z0-9]+", begin), "Invalid BEGIN marker")
    require(end == begin.replace("_BEGIN_", "_END_"), "Mismatched console markers")
    log = receipt.get("log", "").replace("\r\n", "\n")
    lines = [line.rstrip(" \t") for line in log.splitlines()]
    require(lines and lines[0] == begin and lines[-1] == end and lines.count(begin) == 1 and lines.count(end) == 1,
            "Receipt is not one complete original BEGIN/END frame")
    observed = receipt.get("observed_log", receipt["log"]).replace("\r\n", "\n")
    require("\n".join(line.rstrip(" \t") for line in log.splitlines()) in "\n".join(line.rstrip(" \t") for line in observed.splitlines()), "Observed log does not include the unaltered ACK frame")
    require(type(ack.get("endByteOffset")) is int and ack["endByteOffset"] > 0, "Missing console byte offset")
    return observed, begin, ack["endByteOffset"]


def heading(a, b):
    return max(abs((x - y + 180.0) % 360.0 - 180.0) for x, y in zip(a, b))


def released(sample):
    return sample["buttons"] == 0 and all(abs(value) <= 0.001 for value in sample["move"])


def clean(sample):
    expected = {"route": 0, "index": 0, "node": -1, "goal": -1, "owner": 0,
                "arrived": 0, "settle": -1, "settleage": -1, "airborne": 0,
                "progress": -1, "recovery": 0, "break": -1, "breaktoken": 0,
                "use": -1, "action": 0, "pendingvel": 0, "task": "normal", "tasks": 1}
    require(all(sample[key] == value for key, value in expected.items()), "Lifecycle left route/task/action state behind")
    require(released(sample), "Lifecycle did not release actual CUserCmd buttons/movement")


def binding(metadata, case):
    for key in HASHES:
        require(isinstance(metadata.get(key), str) and re.fullmatch(r"[a-f0-9]{64}", metadata[key]), "Missing artifact version: " + key)
        require(case.get("binding", {}).get(key) == metadata[key], "Case artifact version differs: " + key)
    require(metadata.get("lifecycle_policy_version", VERSION) == VERSION, "Unsupported lifecycle policy")


def stage_frames(receipts, identity, seen, previous_offset):
    frames, all_data = [], {"events": [], "maps": [], "logs": [], "players": []}
    for receipt in receipts:
        log, marker, offset = receipt_log(receipt)
        require(marker not in seen, "A console receipt was reused")
        require(offset > previous_offset[0], "Console receipts are reordered/reused")
        seen.add(marker)
        if "observed_log" in receipt:
            start, finish = receipt.get("observed_start_offset"), receipt.get("observed_end_offset")
            require(type(start) is int and type(finish) is int and previous_offset[0] <= start <= offset <= finish,
                    "Observed log offsets are missing/reused/reversed")
            previous_offset[0] = finish
        else:
            previous_offset[0] = offset
        data = parse(log)
        all_data["logs"].append(log)
        for key in ("events", "maps", "players"):
            all_data[key].extend(data[key])
        for sample in data["core"]:
            require((sample["edict"], sample["userid"], sample["name"], sample["map"]) ==
                    (identity["bot_slot"], identity["bot_userid"], identity["bot_name"], identity["map"]),
                    "Bot identity/map changed unexpectedly")
            players = [p for p in data["players"] if p["map"] == sample["map"] and abs(p["tick"] - sample["tick"]) <= 2]
            by_slot = {p["slot"]: p for p in players}
            require(len(players) == len(by_slot), "Ambiguous player snapshots in one receipt")
            bot, human = by_slot.get(identity["bot_slot"]), by_slot.get(identity["human_slot"])
            require(bot is not None and human is not None, "Each lifecycle sample needs both real player probes")
            require(bot["fake"] == 1 and human["fake"] == 0 and bot["userid"] == identity["bot_userid"] and human["userid"] == identity["human_userid"],
                    "Player identity/fake-client type changed")
            require((human["epoch"], human["map_serial"]) == (bot["epoch"], bot["map_serial"]), "Human and bot probes come from different map generations")
            require(human["team"] in (2, 3), "Human must remain on a playing team")
            require(all(p["team"] == human["team"] for p in players if p["fake"] and p["team"] in (2, 3)),
                    "Every playing bot must share the human team")
            require(bot["team"] == sample["team"] and bot["alive"] == sample["alive"] and bot["life_state"] == sample["lifestate"] and bool(bot["flags"] & 1) == bool(sample["flags"] & 1) and bot["frozen"] == int(bool(sample["flags"] & CORE_FL_FROZEN)),
                    "Core and independent player probe disagree")
            require(math.dist(bot["pos"], sample["pos"]) <= 1.0, "Core/player positions disagree")
            require(sample["auto"] == 0 and sample["roam"] == 0, "Lifecycle test must isolate manual navigation")
            frames.append({"core": sample, "bot": bot, "human": human})
    return frames, all_data


def continuous(frames, minimum=0):
    require(len(frames) >= (3 if minimum else 1), "Too few lifecycle samples")
    first = frames[0]
    previous = None
    for frame in frames:
        bot, sample = frame["bot"], frame["core"]
        require((bot["serial"], bot["epoch"], bot["map_serial"]) ==
                (first["bot"]["serial"], first["bot"]["epoch"], first["bot"]["map_serial"]), "Player/map generation changed within stage")
        if previous:
            dt = sample["time"] - previous["core"]["time"]
            require(dt > 0 and (not minimum or dt <= 1.5) and sample["tick"] > previous["core"]["tick"] and sample["cmdnum"] > previous["core"]["cmdnum"],
                    "Stale/reversed/gapped lifecycle samples")
        previous = frame
    require(frames[-1]["core"]["time"] - first["core"]["time"] >= minimum, "Continuous sample window is too short")


def moving(frames, goal):
    require(any(f["core"]["route"] > 0 and f["core"]["goal"] == goal and not released(f["core"]) for f in frames),
            "No active actual CUserCmd on the original goal")
    require(any(math.dist(frames[0]["core"]["pos"], f["core"]["pos"]) > 1.0 for f in frames[1:]),
            "No real post-transition movement")


def real_event(data, name, before, after, minimum_seq, newteam=None):
    """Use the hook's original sequence, even when the probe replays its cache."""
    for item in data["events"]:
        same_scene = (item["epoch"], item["map_serial"], item["map"]) == (after["bot"]["epoch"], after["bot"]["map_serial"], after["core"]["map"])
        actor = name.startswith("round_") or (item["userid"], item["slot"], item["serial"], item["fake"]) == (after["bot"]["userid"], after["bot"]["slot"], after["bot"]["serial"], 1)
        team_matches = newteam is None or item["newteam"] == newteam
        if same_scene and actor and team_matches and item["event"] == name and minimum_seq < item["seq"] <= after["bot"]["lifecycle_seq"] and before["core"]["tick"] <= item["tick"] <= after["core"]["tick"]:
            return item
    raise ValueError("Missing newly observed real event for the original identity: " + name)


def evaluate_case(case, metadata):
    """Return a new verdict. Invalid/unknown evidence is a failure, never a skip."""
    if not isinstance(case, dict) or not isinstance(metadata, dict):
        return {"kind": None, "status": "failed", "error": "Case and metadata must be objects", "policy_version": VERSION}
    result = {"kind": case.get("kind"), "status": "failed", "scope": "sampled_lifecycle_only", "policy_version": VERSION}
    try:
        kind = case.get("kind")
        require(kind in KINDS, "Unknown lifecycle kind")
        binding(metadata, case)
        identities = {key: metadata[key] for key in ("map", "bot_slot", "bot_userid", "bot_name", "human_slot", "human_userid")}
        required = {"stop": ("before", "stopped"), "freeze_resume": ("before", "frozen", "resumed"),
                    "freeze_settle": ("before", "frozen", "resumed"),
                    "death_respawn": ("before", "dead", "respawned", "recovered"),
                    "team": ("before", "changed", "returned", "recovered"),
                    "round": ("before", "cleared", "recovered"), "map": ("before", "absent", "recovered")}[kind]
        stages = case.get("stages", {})
        require(set(stages) == set(required), "Missing/unknown lifecycle stages")
        parsed, extra, seen, previous_offset = {}, {}, set(), [0]
        for stage in required:
            identity = dict(identities)
            if kind == "map" and stage == "recovered":
                identity.update(case.get("recovery_identity", {}))
            if kind == "map" and stage == "absent":
                # No AgPB object may survive the required pre-map kick.
                identity["map"] = "__no_bot_expected__"
            parsed[stage], extra[stage] = stage_frames(stages[stage], identity, seen, previous_offset)
            if stage != "absent":
                continuous(parsed[stage], 0.8 if stage in ("stopped", "frozen") else 0)
        before = parsed["before"][-1]
        base, basebot = before["core"], before["bot"]
        require(base["alive"] and before["human"]["alive"] and base["team"] == before["human"]["team"], "Baseline needs alive same-team players")
        require(base["route"] > 0 and base["goal"] >= 0 and base["owner"] > 0, "Baseline has no owned active route")
        goal = base["goal"]
        # Across a non-map lifecycle the same edict/userid must also retain the
        # independent SourceMod serial and map generation, including respawn.
        if kind != "map":
            sequence = [frame for name in required for frame in parsed[name]]
            for frame in sequence:
                b = frame["bot"]
                require((b["serial"], b["epoch"], b["map_serial"], frame["human"]["serial"]) == (basebot["serial"], basebot["epoch"], basebot["map_serial"], before["human"]["serial"]), "Lifecycle replaced the client/map identity")
            for a, b in zip(sequence, sequence[1:]):
                require(b["core"]["tick"] > a["core"]["tick"] and b["core"]["time"] > a["core"]["time"], "Stages do not follow real game time")
        if kind == "stop":
            stopped = parsed["stopped"]
            require(any(not released(f["core"]) for f in parsed["before"]), "Stop was not exercised during active input")
            require(abs((base["eye"][1] + 180) % 360 - 180) >= 10, "Stop heading baseline is too close to zero")
            anchor = stopped[0]["core"]["eye"]
            require(heading(anchor, base["eye"]) <= 5, "Stop changed the previous heading")
            for frame in stopped:
                sample = frame["core"]
                clean(sample)
                require(sample["resets"] > base["resets"] and sample["reset"] == "stop", "No explicit stop reset was observed")
                require(heading(sample["view"], anchor) <= 0.5 and heading(sample["eye"], anchor) <= 0.5 and heading(frame["bot"]["eye"][:2], anchor) <= 0.5,
                        "Stopped view/heading drifted")
                require(frame["bot"]["buttons"] == 0, "Independent probe still observes held buttons")
        elif kind.startswith("freeze_"):
            frozen, resumed = parsed["frozen"], parsed["resumed"]
            anchor = frozen[0]["core"]
            require((anchor["route"], anchor["goal"], anchor["owner"]) == (base["route"], base["goal"], base["owner"]) and base["index"] <= anchor["index"] < anchor["route"], "Original route was changed before real freeze")
            original = tuple(anchor[key] for key in ("route", "index", "node", "goal", "owner", "airborne"))
            for frame in frozen:
                sample = frame["core"]
                require(sample["flags"] & CORE_FL_FROZEN and frame["bot"]["frozen"] == 1 and sample["roundfrozen"] == 0,
                        "Freeze must be real FL_FROZEN, independently sampled outside round freeze")
                require(tuple(sample[key] for key in ("route", "index", "node", "goal", "owner", "airborne")) == original,
                        "Freeze did not retain the original route/task ownership")
                require(released(sample) and frame["bot"]["buttons"] == 0, "Frozen actual input was not released")
                require(all(sample[key] == value for key, value in {"settle": -1, "settleage": -1, "progress": -1, "recovery": 0, "break": -1, "breaktoken": 0, "use": -1, "pendingvel": 0}.items()),
                        "Freeze left settle/break/use/recovery state behind")
                # action is the owning task's entry clock, not a held-use timer.
                # Pausing preserves it; a true lifecycle reset still requires 0.
                require(sample["action"] == base["action"], "Freeze changed the owning task entry clock")
                require(sample["resets"] == base["resets"], "Freeze destroyed/reset the owning task")
            require(all(not (f["core"]["flags"] & CORE_FL_FROZEN) and f["core"]["roundfrozen"] == 0 for f in resumed), "Unfreeze was not observed")
            require(not any(re.search(r"\bagpb_bot_(?:goto|stop|auto|roam)\b", r["command"]) for name in ("frozen", "resumed") for r in stages[name]),
                    "Frozen route was explicitly replaced/restarted")
            active = [f for f in resumed if f["core"]["route"] > 0]
            require(active and all(f["core"]["goal"] == goal and f["core"]["owner"] == base["owner"] for f in active), "Original route did not automatically resume")
            if kind == "freeze_resume":
                moving(resumed, goal)
            else:
                require(base["settle"] >= 0 and 0 <= base["settleage"] < 0.8, "No positive pre-freeze settle timer was exercised")
                restarted = [f["core"] for f in active if f["core"]["settle"] >= frozen[-1]["core"]["time"] and f["core"]["settleage"] >= 0]
                require(restarted, "No newly started post-freeze settle timer was sampled")
                arrival = re.compile(r"\[AgPB\] " + re.escape(base["name"]) + rf": arrived at waypoint #{goal}\b")
                require(any(arrival.search(log) for log in extra["resumed"]["logs"]), "No real arrival on the original target")
                require(resumed[-1]["core"]["time"] - restarted[0]["settle"] >= 0.8, "Arrival reused pre-freeze settle time")
        elif kind in ("death_respawn", "team", "round"):
            clearstage = {"death_respawn": "dead", "team": "changed", "round": "cleared"}[kind]
            cleared = parsed[clearstage]
            for frame in cleared:
                clean(frame["core"])
                require(frame["bot"]["buttons"] == 0, "Independent probe still observes held lifecycle input")
                require(frame["core"]["resets"] > base["resets"], "Lifecycle task reset was not observed")
            if kind == "death_respawn":
                dead, respawned = cleared[-1], parsed["respawned"][-1]
                require(not dead["core"]["alive"] and dead["bot"]["client_deaths"] > basebot["client_deaths"], "No real death of the original bot")
                require(respawned["core"]["alive"] and respawned["bot"]["client_spawns"] > dead["bot"]["client_spawns"], "No real respawn of the original bot")
                real_event(extra["dead"], "player_death", before, dead, basebot["lifecycle_seq"])
                real_event(extra["respawned"], "player_spawn", dead, respawned, dead["bot"]["lifecycle_seq"])
                clean(respawned["core"])
            elif kind == "team":
                changed, returned = cleared[-1], parsed["returned"][-1]
                require(changed["core"]["team"] == 1 and changed["bot"]["client_teams"] > basebot["client_teams"], "No real spectator team transition")
                require(returned["core"]["team"] == base["team"] and returned["bot"]["client_teams"] > changed["bot"]["client_teams"] and returned["core"]["alive"], "No real return to the original human team")
                real_event(extra["changed"], "player_team", before, changed, basebot["lifecycle_seq"])
                real_event(extra["returned"], "player_team", changed, returned, changed["bot"]["lifecycle_seq"])
                for frame in parsed["returned"]:
                    clean(frame["core"])
                    require(frame["bot"]["buttons"] == 0, "Returned team retained held input")
                alternate = [frame for frame in parsed["returned"] if frame["core"]["alive"] and frame["core"]["team"] in (2, 3) and frame["core"]["team"] != base["team"]]
                require(alternate, "No real same-team switch to the other playing faction")
                other = alternate[-1]
                other_event = real_event(extra["returned"], "player_team", changed, other, changed["bot"]["lifecycle_seq"], newteam=other["core"]["team"])
                require(other_event["newteam"] == other["core"]["team"] and other["bot"]["client_teams"] > changed["bot"]["client_teams"], "Alternate playing-team event does not match actual state")
                return_event = real_event(extra["returned"], "player_team", other, returned, other["bot"]["lifecycle_seq"], newteam=base["team"])
                require(return_event["newteam"] == base["team"] and returned["bot"]["client_teams"] > other["bot"]["client_teams"], "Original playing team was not restored after the alternate faction")
            else:
                after = cleared[-1]
                require(after["bot"]["round_starts"] > basebot["round_starts"] and after["core"]["epoch"] > base["epoch"], "No real new round in probe/core")
                require(after["core"]["reset"] in ("round-start", "round-end", "dead"), "Cleared state is not tied to the round lifecycle")
                commands = [r["command"] for stage in ("before", "cleared") for r in stages[stage]]
                require(any(re.search(r"(?:^|;)\s*endround\s*(?:;|$)", command) for command in commands), "Native endround request is missing")
                require(after["bot"]["round_ends"] > basebot["round_ends"], "No real round_end counter increase")
                end = real_event(extra["cleared"], "round_end", before, after, basebot["lifecycle_seq"])
                start = real_event(extra["cleared"], "round_start", before, after, end["seq"])
                require(end["tick"] <= start["tick"], "Round start preceded the real round end")
            recovered = parsed["recovered"]
            require(all(f["core"]["alive"] and f["core"]["team"] == before["human"]["team"] for f in recovered), "Recovery player is not alive on the original team")
            require(any(re.search(rf"\bagpb_bot_goto\s+\d+\s+{goal}(?:\s|;|$)", r["command"]) for r in stages["recovered"]), "Recovery did not explicitly request the original target")
            moving(recovered, goal)
        else:
            absent = extra["absent"]
            require(absent["players"] and not any(p["fake"] for p in absent["players"]), "Map transition lacks a real zero-bot intermediate state")
            require(any(p["epoch"] == basebot["epoch"] and p["map_serial"] > basebot["map_serial"] and p["map"] != base["map"] for p in absent["players"]), "No intermediate different map generation")
            recovered = parsed["recovered"]
            new = recovered[0]
            clean(new["core"])
            require(new["bot"]["buttons"] == 0, "New-map player retained input")
            require(new["bot"]["epoch"] == basebot["epoch"] and new["bot"]["map_serial"] > max(p["map_serial"] for p in absent["players"] if p["map"] != base["map"]), "No return map generation after the empty intermediate map")
            require((new["bot"]["userid"], new["bot"]["serial"]) != (basebot["userid"], basebot["serial"]), "Map recovery reused the old client identity")
            require(new["core"]["map"] == base["map"], "Map recovery did not return to the original map")
            require(any(re.search(r"\bagpb_kick\s+all\b", r["command"]) for r in stages["absent"]), "Required pre-map kick is missing")
            require(any(item["event"] == "map_start" and item["epoch"] == basebot["epoch"] and item["map_serial"] == new["bot"]["map_serial"] and item["map"] == base["map"] and item["seq"] > basebot["lifecycle_seq"] for item in extra["recovered"]["maps"]), "Missing real return map_start forward")
            require(any(item["event"] == "map_start" and item["epoch"] == basebot["epoch"] and item["map_serial"] > basebot["map_serial"] and item["map"] != base["map"] and item["seq"] > basebot["lifecycle_seq"] for item in absent["maps"]), "Missing real intermediate map_start forward")
            require(any(re.search(rf"\bagpb_bot_goto\s+\d+\s+{goal}(?:\s|;|$)", r["command"]) for r in stages["recovered"]), "New map bot was not given the original target")
            moving(recovered, goal)
        if "target" in case:
            target = case["target"]
            require(target["index"] == goal, "Endpoint target does not match the original goal")
            final = parsed["stopped" if kind == "stop" else "resumed" if kind.startswith("freeze_") else "recovered"]
            samples = [{"time": f["core"]["time"], "slot": f["bot"]["slot"], "userid": f["bot"]["userid"], "flags": f["bot"]["flags"], "position": f["bot"]["pos"], "velocity": f["bot"]["vel"]} for f in final]
            endpoint = stop_policy.evaluate(target, samples, samples[-1])
            result["endpoint"] = endpoint
            require(endpoint["precise_stop_passed"], "Final precise endpoint window was not established")
        result.update(status="passed", goal=goal, sample_counts={name: len(frames) for name, frames in parsed.items()})
    except (ValueError, TypeError, KeyError, OverflowError) as exc:
        result["error"] = str(exc)
    return result


def evaluate(document):
    """Recompute all cases; full acceptance needs every lifecycle kind."""
    require(isinstance(document, dict), "Lifecycle evidence must be an object")
    require(document.get("schema_version") == VERSION, "Unsupported lifecycle evidence schema")
    metadata = document.get("metadata", {})
    cases = document.get("cases", [])
    require(isinstance(cases, list) and cases, "No lifecycle cases")
    require(isinstance(metadata, dict), "Lifecycle metadata must be an object")
    results = [evaluate_case(case, metadata) for case in cases]
    covered = {result["kind"] for result in results if result["status"] == "passed"}
    missing = sorted(set(KINDS) - covered)
    return {"schema_version": VERSION, "policy_version": VERSION, "scope": "sampled_lifecycle_only",
            "status": "passed" if not missing and all(r["status"] == "passed" for r in results) else "failed",
            "missing_kinds": missing, "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--output", type=Path, help="New report under system TEMP; evidence is never overwritten")
    args = parser.parse_args()
    try:
        report = evaluate(json.loads(args.evidence.read_text(encoding="utf-8-sig")))
        if args.output:
            from verify_routes import temp_child
            output = temp_child(args.output)
            require(not output.exists(), "Report output already exists")
            output.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=True))
        return 0 if report["status"] == "passed" else 1
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
