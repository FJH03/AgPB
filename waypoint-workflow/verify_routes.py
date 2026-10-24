#!/usr/bin/env python3
"""Verify explicit routes using an already running game and existing same-team players.

Only agpb_list, agpb_wp_list, the read-only probe, agpb_bot_goto and
agpb_bot_stop are sent. Compile/load fixtures/agpb_waypoint_probe.sp manually
before running. This tool never launches a server, places players or edits links.
"""
import argparse
import hashlib
import json
import math
import os
import queue
from pathlib import Path
import re
import shutil
import subprocess
import time
import threading

from export_manual_plan import export, jump_velocity
from audit_waypoints import compare_agpw
import stop_policy

STATE = re.compile(
    r"\[WP_PROBE_STATE\] t=([\d.]+) map=(\S+) slot=(\d+) userid=(\d+) "
    r"fake=([01]) team=(\d+) alive=([01]) flags=(-?\d+) "
    r"pos=\(([^)]+)\) vel=\(([^)]+)\)(?: zone=(-?\d+) defusing=(-?\d+))?")
BOT = re.compile(r"^\s*\[(\d+)\] (.+) slot=(\d+) team=(-?\d+) want=(-?\d+) hp=(-?\d+) wpn=", re.M)
POINT = re.compile(r"^\s*\[\s*(\d+)\] (-?\d+) (-?\d+) (-?\d+)\s+flags=0x([0-9A-Fa-f]+)\s+r=(\d+)\s+(.*)$", re.M)
EDGE = re.compile(r"(\d+)/0x([0-9A-Fa-f]+)(?:\s+velocity=(\([^)]*\)))?")
TEAM = {"T": 2, "CT": 3}
SETTLE_SPEED = stop_policy.MAX_SPEED
SETTLE_DURATION = stop_policy.MIN_DURATION
SETTLE_MAX_GAP = stop_policy.MAX_SAMPLE_GAP


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def temp_child(path):
    path = Path(path).resolve()
    root = Path(os.environ.get("TEMP", os.environ.get("TMP", "/tmp"))).resolve()
    if path == root or root not in path.parents:
        raise ValueError("Output/session path must be a child of system TEMP: " + str(path))
    return path


class CommandFailure(RuntimeError):
    def __init__(self, message, receipt):
        super().__init__(message)
        self.receipt = receipt


class ConsoleSender:
    """Serialize requests through one shared -Listen helper, with READY framing."""
    READY = "AGPB_NATIVE_SESSION_READY"
    ERROR = "AGPB_NATIVE_SESSION_ERROR:"

    def __init__(self, powershell, session_name, session_directory, ack_timeout=15):
        self.powershell = powershell
        self.session_name = session_name
        self.session_directory = session_directory
        self.ack_timeout = ack_timeout
        self.frame_timeout = ack_timeout + 20
        self.startup_timeout = 15
        self.log_path = Path(session_directory) / "console.log"
        self.script = Path(__file__).with_name("Invoke-GameConsole.ps1")
        self.anchors = {}
        self.process = None
        self.reader = None
        self.lines = None
        self.closed = False
        self.request_lock = threading.Lock()

    def _read_frame(self, receipt, timeout, initial=False):
        deadline = time.monotonic() + timeout
        output = []
        while True:
            remaining = deadline - time.monotonic()
            try:
                if remaining <= 0:
                    raise queue.Empty
                line = self.lines.get(timeout=remaining)
            except queue.Empty as exc:
                receipt["stdout"] = "".join(output)
                raise CommandFailure("Console listener timed out before READY", receipt) from exc
            if line is None:
                receipt["stdout"] = "".join(output)
                raise CommandFailure("Console listener exited before READY", receipt)
            output.append(line)
            text = line.rstrip("\r\n")
            if text == self.READY or (initial and text.startswith(self.READY + ": ")):
                receipt["stdout"] = "".join(output)
                return

    def _start(self, receipt):
        if self.closed:
            raise CommandFailure("Console sender is closed", receipt)
        if self.process is not None:
            return
        args = [self.powershell, "-NoProfile", "-File", str(self.script), "-Listen",
                "-SessionName", self.session_name, "-SessionDirectory", str(self.session_directory),
                "-DelayMilliseconds", "100", "-AckTimeoutSeconds", str(self.ack_timeout)]
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                        errors="replace", bufsize=1,
                                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.lines = queue.Queue()
        process, lines = self.process, self.lines
        def read_output():
            try:
                for line in iter(process.stdout.readline, ""):
                    lines.put(line)
            finally:
                lines.put(None)
        self.reader = threading.Thread(target=read_output, name="agpb-console-listener", daemon=True)
        self.reader.start()
        startup = {"stdout": ""}
        try:
            self._read_frame(startup, self.startup_timeout, initial=True)
        except CommandFailure as exc:
            receipt["stdout"] = startup["stdout"]
            raise CommandFailure(str(exc), receipt) from exc
        finally:
            receipt["helper_startup_stdout"] = startup["stdout"]

    def _close_helper(self, graceful=True):
        process, self.process = self.process, None
        if process is None:
            return
        try:
            if graceful and process.poll() is None:
                try:
                    process.stdin.write("END\n")
                    process.stdin.flush()
                except (OSError, ValueError):
                    pass
            try:
                process.wait(timeout=3 if graceful else 0.1)
            except subprocess.TimeoutExpired:
                # These handles belong only to the helper we started, never the game.
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        finally:
            try:
                process.stdin.close()
            except (OSError, ValueError):
                pass
            if self.reader is not None:
                self.reader.join(timeout=0.2)
            process.stdout.close()
            self.reader = None

    def close(self):
        self.closed = True
        self._close_helper()

    def send(self, command):
        receipt = {"command": command, "stdout": "", "stderr": "", "log": "",
                   "transport": "persistent_listener", "stderr_merged": True}
        if not self.request_lock.acquire(blocking=False):
            raise CommandFailure("Concurrent console requests are prohibited; use one operator", receipt)
        framed = False
        try:
            self._start(receipt)
            request = {"commands": command, "delay": 100, "ackTimeoutSeconds": self.ack_timeout}
            self.process.stdin.write(json.dumps(request, ensure_ascii=True) + "\n")
            self.process.stdin.flush()
            self._read_frame(receipt, self.frame_timeout)
            framed = True
            if re.search(r"^" + self.ERROR, receipt["stdout"], re.M):
                receipt["returncode"] = 1
                raise CommandFailure("Console listener reported a request error", receipt)
            matches = list(re.finditer(r"^CONSOLE_ACK:\s*(\{.*\})\s*$", receipt["stdout"], re.M))
            if len(matches) != 1:
                raise CommandFailure("Console listener did not return exactly one CONSOLE_ACK", receipt)
            ack = json.loads(matches[0][1])
            receipt["ack"] = ack
            begin, end = ack["beginMarker"], ack["endMarker"]
            start = receipt["stdout"].find(begin)
            finish = receipt["stdout"].find(end, start + len(begin))
            if start < 0 or finish < start or ack.get("logResetCount", 0):
                raise CommandFailure("Console receipt missing markers or log was reset", receipt)
            receipt["log"] = receipt["stdout"][start:finish + len(end)] + "\n"
            receipt["end_offset"] = int(ack["endByteOffset"])
            receipt["returncode"] = 0
            return receipt
        except (OSError, ValueError, KeyError) as exc:
            receipt["stderr"] = str(exc)
            raise CommandFailure("Console listener failed: " + str(exc), receipt) from exc
        finally:
            try:
                if not framed:
                    # Partial frames cannot be reused; a subsequent best-effort stop
                    # may start a fresh helper instead of consuming stale output.
                    self._close_helper(graceful=False)
            finally:
                self.request_lock.release()

    def cursor(self):
        data = self.log_path.read_bytes() if self.log_path.exists() else b""
        offset = len(data)
        self.anchors[offset] = sha256(data[max(0, offset - 64):offset])
        return offset

    def tail(self, offset):
        data = self.log_path.read_bytes()
        if len(data) < offset or (offset in self.anchors and sha256(data[max(0, offset - 64):offset]) != self.anchors[offset]):
            raise RuntimeError("Console log was truncated/replaced during route verification")
        return data[offset:].decode("utf-8", errors="replace")


def states(log, map_name):
    result = {}
    for match in STATE.finditer(log):
        if match[2] != map_name:
            raise ValueError("Probe map does not match plan")
        position = [float(value) for value in match[9].split()]
        velocity = [float(value) for value in match[10].split()]
        if len(position) != 3 or len(velocity) != 3 or not all(map(math.isfinite, position + velocity)):
            raise ValueError("Invalid probe vectors")
        slot = int(match[3])
        flags = stop_policy.normalize_flags(int(match[8]))
        if flags is None:
            raise ValueError("Probe flags are outside int32/uint32 range")
        result[slot] = {"time": float(match[1]), "slot": slot, "userid": int(match[4]),
                        "fake": int(match[5]), "team": int(match[6]), "alive": int(match[7]),
                        "flags": flags, "position": position, "velocity": velocity,
                        "zone": int(match[11]) if match[11] is not None else -1,
                        "defusing": int(match[12]) if match[12] is not None else -1}
    return result


def validate_pair(samples, human_slot, bot_slot, team, identity=None):
    if human_slot == bot_slot:
        raise ValueError("Human and bot slots must differ")
    human, bot = samples.get(human_slot), samples.get(bot_slot)
    if not human or not bot:
        raise ValueError("Probe did not report both configured slots")
    if human["fake"] or not bot["fake"]:
        raise ValueError("Configured human/bot slots have incorrect fake-client types")
    if human["team"] != TEAM[team] or bot["team"] != TEAM[team]:
        raise ValueError("Human and bot must both be on the requested team")
    if not human["alive"] or not bot["alive"]:
        raise ValueError("Human and bot must both be alive")
    # The known Radio/freezetime crash applies to every test bot, including bots
    # outside this run. Reject any playing fake client on the opposing team.
    if any(sample["fake"] and sample["team"] in (2, 3) and sample["team"] != TEAM[team] for sample in samples.values()):
        raise ValueError("Every active-team bot must share the configured human team (AGENTS.md test rule)")
    if identity and (human["userid"], bot["userid"]) != identity:
        raise ValueError("Configured player identity changed during verification")
    return human, bot


def validate_waypoints(log, plan):
    current_map = re.search(r"\[AgPB\] map=(\S+) file=", log)
    count = re.search(r"\[AgPB\] (\d+) waypoint\(s\),", log)
    if not current_map or current_map[1] != plan["map"] or not count or int(count[1]) != len(plan["nodes"]):
        raise ValueError("Current map/waypoint count does not match plan")
    rows = {int(row[1]): row for row in POINT.finditer(log)}
    if set(rows) != set(range(len(plan["nodes"]))):
        raise ValueError("agpb_wp_list 0 did not report every waypoint")
    labels = {node["label"]: i for i, node in enumerate(plan["nodes"])}
    edges = [dict() for _ in plan["nodes"]]
    for edge in plan["directed_edges"]:
        edges[labels[edge["from"]]][labels[edge["to"]]] = (edge.get("flags", 0), jump_velocity(edge.get("jumpVelocity", [0.0, 0.0, 0.0])))
    for index, node in enumerate(plan["nodes"]):
        row = rows[index]
        if any(abs(float(row[j + 2]) - float(node[key])) > 0.501 for j, key in enumerate(("x", "y", "z"))):
            raise ValueError("In-memory waypoint coordinates differ from plan at #" + str(index))
        edge_rows = list(EDGE.finditer(row[7]))
        links = {int(edge[1]): int(edge[2], 16) for edge in edge_rows}
        if len(edge_rows) != len(links) or int(row[5], 16) != node.get("flags", 0) or int(row[6]) != node["radius"] or links != {target: value[0] for target, value in edges[index].items()}:
            raise ValueError("In-memory waypoint flags/radius/links differ at #" + str(index))
        if len(re.findall(r"\bvelocity=", row[7])) != sum(edge[3] is not None for edge in edge_rows):
            raise ValueError("Malformed/ambiguous in-memory jumpVelocity at #" + str(index))
        for edge in edge_rows:
            expected_velocity = edges[index][int(edge[1])][1]
            if edge[3] is None:
                if any(component != 0 for component in expected_velocity):
                    raise ValueError("Missing in-memory jumpVelocity at #" + str(index))
                continue
            try:
                actual_velocity = [float(component) for component in re.split(r"[,\s]+", edge[3][1:-1].strip())]
            except ValueError as exc:
                raise ValueError("Invalid in-memory jumpVelocity at #" + str(index)) from exc
            # Native list prints six decimals; disk comparison remains bit-exact f32.
            if len(actual_velocity) != 3 or not all(math.isfinite(component) and abs(component) <= 2000 for component in actual_velocity) or any(abs(actual - expected) > 0.000000501 for actual, expected in zip(actual_velocity, expected_velocity)):
                raise ValueError("In-memory jumpVelocity differs from plan at #" + str(index))


def disk_evidence(plan, expected):
    """Read the deployment destination from AGENTS; allow native-save author changes."""
    requirements = (Path(__file__).resolve().parents[1] / "AGENTS.md").read_text(encoding="utf-8-sig")
    match = re.search(r"(?m)^- 部署目标：\s*`([^`]+)`", requirements)
    if not match:
        raise ValueError("Deployment target is missing from AGENTS.md")
    game_dir = Path(match[1])
    path = game_dir / "addons" / "AgPB" / "waypoints" / (plan["map"] + ".agpw")
    actual = path.read_bytes()
    comparison = compare_agpw(expected, actual, allow_author_change=True)
    if not comparison['fields_match']:
        raise ValueError("Deployed AGPW data/map differs from the explicit plan: " + str(path))
    result = {"agpw_file": str(path), "agpw_sha256": sha256(actual),
               "agpw_author_differs": actual[44:76] != expected[44:76],
               "agpw_version": comparison['actual_agpw_version']}
    for name, file in (("bsp", game_dir / "maps" / (plan["map"] + ".bsp")),
                       ("dll", game_dir / "addons" / "AgPB" / "bin" / "win64" / "agpb_mm.dll")):
        if file.is_file():
            result[name + "_sha256"] = sha256(file.read_bytes())
            result[name + "_file"] = str(file)
    return result


class RouteVerifier:
    def __init__(self, plan, metadata, sender, timeout=45, clock=time.monotonic, sleep=time.sleep, save=None, require_precise_stop=False):
        self.plan, self.metadata, self.sender = plan, metadata, sender
        self.timeout, self.clock, self.sleep, self.save_callback = timeout, clock, sleep, save
        self.require_precise_stop = require_precise_stop
        self.metadata["scope"] = "route_traversal"
        self.metadata["stop_policy"] = stop_policy.description()
        self.metadata["require_precise_stop"] = require_precise_stop
        self.result = {"schema_version": 1, "metadata": metadata, "records": [], "preflight_receipts": [], "status": "running"}
        self.active = None
        self.identity = None
        self.bot_name = None

    def save(self):
        if self.save_callback:
            self.save_callback(self.result)

    def send(self, command, receipts):
        try:
            receipt = self.sender.send(command)
        except CommandFailure as exc:
            receipts.append(exc.receipt)
            raise
        receipts.append(receipt)
        return receipt

    def pair(self, log):
        return validate_pair(states(log, self.plan["map"]), self.metadata["human_slot"],
                             self.metadata["bot_slot"], self.metadata["team"], self.identity)

    def check_mapping(self, log):
        bots = list(BOT.finditer(log))
        selected = [row for row in bots if int(row[1]) == self.metadata["bot_index"]]
        if len(selected) != 1 or int(selected[0][3]) != self.metadata["bot_slot"] or int(selected[0][4]) != TEAM[self.metadata["team"]]:
            raise ValueError("agpb_list does not map bot-index to the requested slot/team")
        name = selected[0][2]
        if self.bot_name is not None and name != self.bot_name:
            raise ValueError("Bot-index identity changed during verification")
        if sum(row[2] == name for row in bots) != 1:
            raise ValueError("Bot names are ambiguous; route messages cannot be attributed safely")
        self.bot_name = name

    def preflight(self):
        receipt = self.send("agpb_list; agpb_wp_list 0; agpb_wp_probe_state", self.result["preflight_receipts"])
        log = receipt["log"]
        self.check_mapping(log)
        validate_waypoints(log, self.plan)
        human, bot = self.pair(log)
        self.identity = human["userid"], bot["userid"]
        self.metadata["bot_name"] = self.bot_name
        self.result["preflight_state"] = {"human": human, "bot": bot}
        self.save()

    def sample(self, record, check_mapping=False):
        command = "agpb_list; agpb_wp_probe_state" if check_mapping else "agpb_wp_probe_state"
        receipt = self.send(command, record["command_receipts"])
        if check_mapping:
            self.check_mapping(receipt["log"])
        human, bot = self.pair(receipt["log"])
        record["samples"].append({"human": human, "bot": bot, "log": receipt["log"]})
        return bot

    def stop(self, record):
        # Resolve the same existing client again: another bot may have left and
        # shifted the list index. Never send stop to a replacement client.
        receipt = self.send("agpb_list; agpb_wp_probe_state", record["command_receipts"])
        bot = states(receipt["log"], self.plan["map"]).get(self.metadata["bot_slot"])
        if not bot or not bot["fake"] or (self.identity and bot["userid"] != self.identity[1]):
            raise ValueError("Cannot safely identify the original bot for stop")
        rows = [row for row in BOT.finditer(receipt["log"]) if int(row[3]) == bot["slot"] and row[2] == self.bot_name]
        if len(rows) != 1:
            raise ValueError("Cannot safely map the original bot for stop")
        record["stop_bot_index"] = int(rows[0][1])
        stopped = self.send("agpb_bot_stop " + rows[0][1], record["command_receipts"])
        expected = re.escape("[AgPB] " + self.bot_name + ": route stopped (roam off)")
        if not re.search(r"^" + expected + r"\s*$", stopped["log"], re.M):
            raise ValueError("AgPB did not acknowledge stopping the original bot")

    def settle(self, record):
        deadline = self.clock() + self.timeout
        node = self.plan["nodes"][record["index"]]
        # Navigation samples never establish an endpoint stop window.
        record["stop_samples"] = []
        while self.clock() < deadline:
            sample = self.sample(record)
            record["after"] = sample
            record["stop_samples"].append(sample)
            measured = stop_policy.evaluate(node, record["stop_samples"], sample)
            record.update(measured)
            if measured["precise_stop_passed"] or (measured["settled"] and
                    (not measured["within_radius"] or not measured["within_vertical"])):
                return
            self.sleep(0.15)

    def navigate(self, record):
        index = record["index"]
        record["console_start_offset"] = self.sender.cursor()
        receipt = self.send(f"agpb_bot_goto {self.metadata['bot_index']} {index}", record["command_receipts"])
        prefix = re.escape("[AgPB] " + self.bot_name + ": ")
        walking = re.compile(prefix + rf"walking to #{index}, \d+ hop\(s\): ([0-9 ]+)")
        arrival = re.compile(prefix + rf"arrived at waypoint #{index}\b")
        failure = re.compile(prefix + r"[^\r\n]*(?:failed:|stuck|could not|no walkable|cannot|unable to|route aborted| - stopped)", re.I)
        deadline = self.clock() + self.timeout
        while True:
            record["log"] = self.sender.tail(record["console_start_offset"])
            # A walking message must belong to this goto's acknowledged CFG;
            # unrelated output before its BEGIN marker cannot establish a route.
            log = receipt["log"] + self.sender.tail(receipt["end_offset"])
            walk = walking.search(receipt["log"])
            if walk:
                record["route"] = walk[1].strip()
                following = log[walk.end():]
                arrived, failed = arrival.search(following), failure.search(following)
                if failed and (not arrived or failed.start() < arrived.start()):
                    record["status"] = "failed"
                    return
                if arrived:
                    record["status"] = "arrived"
                    return
            else:
                record["status"] = "failed"
                record["error"] = "No matching walking response in the goto command receipt"
                return
            if self.clock() >= deadline:
                record["status"] = "timeout"
                return
            # Check actual players throughout the walk; receipt lines are retained.
            self.sample(record)
            self.sleep(0.15)

    def run(self, targets):
        try:
            self.save()
            self.preflight()
            for label, index in targets:
                record = {"target": label, "index": index, "status": "running", "route": None,
                          "before": None, "after": None, "stopped_distance": None,
                          "target_radius": self.plan["nodes"][index]["radius"],
                          "settled": False, "within_radius": False, "stop_status": "unsettled",
                          "stop_policy_version": stop_policy.VERSION, "stop_scope": "sampled_endpoint_only",
                          "raw_radius": self.plan["nodes"][index]["radius"],
                          "effective_radius": max(self.plan["nodes"][index]["radius"], stop_policy.MIN_XY_RADIUS),
                          "xy_distance": None, "vertical_error": None, "precise_stop_passed": False,
                          "log": "", "command_receipts": [], "samples": [], "stop_samples": []}
                self.result["records"].append(record)
                start = self.clock()
                record["before"] = self.sample(record, check_mapping=True)
                # Once the goto is submitted, every failure path must try to stop it.
                self.active = record
                self.navigate(record)
                self.stop(record)
                self.settle(record)
                record.update(stop_policy.evaluate(self.plan["nodes"][index], record["stop_samples"], record["after"]))
                offset = record.get("console_start_offset")
                if offset is not None:
                    record["log"] = self.sender.tail(offset)
                record["seconds"] = round(self.clock() - start, 3)
                self.active = None
                self.save()
                if record["status"] != "arrived" or (self.require_precise_stop and record["stop_status"] != "settled_in_radius"):
                    self.result["status"] = "failed"
                    break
            else:
                self.result["status"] = "passed"
        except (Exception, KeyboardInterrupt) as exc:
            interrupted = isinstance(exc, KeyboardInterrupt)
            self.result.update(status="interrupted" if interrupted else "failed", error=str(exc) or "KeyboardInterrupt")
            if self.result["records"] and self.result["records"][-1]["status"] == "running":
                self.result["records"][-1].update(status="interrupted" if interrupted else "failed", error=self.result["error"])
            if self.active:
                record = self.active
                record.update(status="interrupted" if interrupted else "failed", error=self.result["error"], settled=False, precise_stop_passed=False, stop_status="unsettled")
                try:
                    self.stop(record)
                except (Exception, KeyboardInterrupt) as stop_error:
                    record["stop_error"] = str(stop_error) or "KeyboardInterrupt"
                try:
                    if "console_start_offset" in record:
                        record["log"] = self.sender.tail(record["console_start_offset"])
                except Exception as log_error:
                    record["log_error"] = str(log_error)
        records = self.result["records"]
        precise = sum(record.get("stop_status") == "settled_in_radius" for record in records)
        self.result["precise_stop_summary"] = {
            "status": "passed" if len(records) == len(targets) and precise == len(targets) else "not_passed",
            "settled_in_radius": precise,
            "out_of_radius": sum(record.get("stop_status") == "out_of_radius" for record in records),
            "unsettled": sum(record.get("stop_status") == "unsettled" for record in records),
            "requested": len(targets), "scope": "sampled_endpoint_stop"}
        self.save()
        return self.result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("plan", "session-name", "session-directory", "bot-index", "bot-slot", "human-slot", "team", "targets", "output"):
        parser.add_argument("--" + name, required=True, type=int if name in ("bot-index", "bot-slot", "human-slot") else str)
    parser.add_argument("--timeout", type=float, default=45, help="Per-navigation and per-settling deadline in seconds")
    parser.add_argument("--require-precise-stop", action="store_true", help="Stop the run and fail if an endpoint is unsettled or outside its radius")
    parser.add_argument("--powershell", default=shutil.which("pwsh") or shutil.which("powershell") or "powershell.exe")
    args = parser.parse_args()
    output = temp_child(args.output)
    session = temp_child(args.session_directory)
    if not re.fullmatch(r"agpb_wp_[A-Za-z0-9_]+", args.session_name):
        parser.error("Invalid session name")
    if args.team not in TEAM or args.bot_index < 0 or min(args.bot_slot, args.human_slot) < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("Invalid team, indices or timeout")
    plan_file = Path(args.plan).resolve()
    if output == plan_file or output == session / "console.log":
        parser.error("Output must not overwrite the plan or session console log")
    original = plan_file.read_bytes()
    plan = json.loads(original.decode("utf-8-sig"))
    expected = export(plan)  # Validate explicit nodes/links; never infer a connection.
    if not re.fullmatch(r"[A-Za-z0-9_-]+", plan["map"]):
        parser.error("Invalid map name")
    labels = {node["label"]: i for i, node in enumerate(plan["nodes"])}
    try:
        targets = [(label, labels[label]) for label in args.targets.split(",")]
    except KeyError as exc:
        parser.error("Unknown target label: " + str(exc))
    metadata = {"plan_sha256": sha256(original), "plan_version": 1, "map": plan["map"],
                "plan_file": str(plan_file), "scope": "route_traversal", "team": args.team, "human_slot": args.human_slot,
                "bot_slot": args.bot_slot, "bot_index": args.bot_index,
                "stop_policy": stop_policy.description(), "settle_speed": SETTLE_SPEED, "settle_duration": SETTLE_DURATION, "settle_max_sample_gap": SETTLE_MAX_GAP}
    output.parent.mkdir(parents=True, exist_ok=True)
    def save(result):
        output.write_text(json.dumps(result, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    try:
        metadata.update(disk_evidence(plan, expected))
    except Exception as exc:
        save({"schema_version": 1, "metadata": metadata, "records": [], "status": "failed", "error": str(exc)})
        print(str(exc))
        return 1
    sender = ConsoleSender(args.powershell, args.session_name, session)
    try:
        result = RouteVerifier(plan, metadata, sender, args.timeout, save=save, require_precise_stop=args.require_precise_stop).run(targets)
    finally:
        sender.close()
    print(json.dumps({"status": result["status"], "records": len(result["records"]), "output": str(output), "error": result.get("error"), "scope": "route_traversal", "precise_stop_summary": result.get("precise_stop_summary")}))
    return 0 if result["status"] == "passed" else 130 if result["status"] == "interrupted" else 1


if __name__ == "__main__":
    raise SystemExit(main())
