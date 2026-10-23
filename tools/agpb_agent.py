#!/usr/bin/env python3
"""Bounded deterministic waypoint-goal lease client; no external LLM is connected.

Example (one console operator, existing running game/session, current bot userid):
  py tools/agpb_agent.py --run --session-name agpb_wp_agent \
    --session-directory "%TEMP%\agpb_wp_agent" --userid 8 \
    --mode camp --waypoint 60 --ttl 3 --duration 10 --renew-interval 1

The client never enables autonomy, moves a player directly, or clears a lease.
Stopping/losing this process stops renewal; the native game-time TTL returns
control to the local SyPB/EBot core. Existing leases are never taken over.
Raw console receipts and the final summary are retained in a fresh TEMP folder.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "waypoint-workflow"))
from verify_routes import ConsoleSender, CommandFailure, temp_child

READ_STATE = "agpb_agent_state\nagpb_bot_info all"
FIELDS = re.compile(r"([A-Za-z_][A-Za-z_0-9-]*)=(\([^)]*\)|[^\s]+)")


def fields(text):
    result = {}
    for match in FIELDS.finditer(text):
        if match[1] in result:
            raise ValueError("Duplicate state field: " + match[1])
        result[match[1]] = match[2]
    return result


def parse_state(log, userid, need_header=True):
    headers, bots, lives = [], [], []
    for line in log.splitlines():
        line = line.strip()
        if line.startswith("[AgPB agent] v=1 "):
            headers.append(fields(line))
        match = re.fullmatch(r"\[AgPB\] (.+?): agent v=1 (.*)", line)
        if match:
            row = fields(match[2])
            if int(row["userid"]) == userid:
                row["name"] = match[1]
                bots.append(row)
        match = re.fullmatch(r"\[AgPB lifecycle\] v=1 name=(.+?) edict=(\d+) userid=(\d+) (.*)", line)
        if match and int(match[3]) == userid:
            row = fields(match[4])
            row.update(name=match[1], edict=match[2], userid=match[3])
            lives.append(row)
    if len(bots) != 1 or need_header and (len(headers) != 1 or len(lives) != 1):
        raise ValueError("Missing/ambiguous current bot identity or scope")
    bot = bots[0]
    for key in ("mapserial", "round", "token", "wp", "auto", "lease-map", "lease-round"):
        bot[key] = int(bot[key])
    bot["deadline"] = float(bot["deadline"])
    if not math.isfinite(bot["deadline"]) or bot["auto"] != 1:
        raise ValueError("Local autonomy disabled or invalid deadline; no commands sent")
    if need_header:
        head, life = headers[0], lives[0]
        if (int(head["mapserial"]), int(head["round"])) != (bot["mapserial"], bot["round"]):
            raise ValueError("Incoherent map/round state")
        if life["alive"] != "1" or life["lifestate"] != "0" or life["auto"] != "1":
            raise ValueError("Bot stopped/dead; local controls are not overridden")
        if (life["map"], int(life["epoch"])) != (head["map"], bot["round"]):
            raise ValueError("Lifecycle and lease scope differ")
        bot["identity"] = (head["map"], bot["mapserial"], bot["round"], userid,
                           int(life["edict"]), life["name"], int(life["team"]), int(life["desired"]))
    return bot


def same_lease(current, owned):
    return owned is not None and all(current[key] == owned[key] for key in
        ("mapserial", "round", "token", "mode", "wp", "lease-map", "lease-round")) and \
        abs(current["deadline"] - owned["deadline"]) <= 0.0005


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run", action="store_true", help="Explicitly allow bounded native goal submission")
    parser.add_argument("--session-name", required=True)
    parser.add_argument("--session-directory", required=True, type=Path)
    parser.add_argument("--userid", required=True, type=int)
    parser.add_argument("--mode", required=True, choices=("move", "camp"))
    parser.add_argument("--waypoint", required=True, type=int)
    parser.add_argument("--ttl", type=float, default=3.0, help="Native game seconds, 0.5..30 (default 3)")
    parser.add_argument("--duration", type=float, default=10.0, help="Renewal wall seconds, 0.5..120 (default 10)")
    parser.add_argument("--renew-interval", type=float, default=1.0)
    parser.add_argument("--powershell", default=shutil.which("pwsh") or shutil.which("powershell") or "powershell.exe")
    args = parser.parse_args(argv)
    if not args.run:
        parser.error("No game commands are sent without --run")
    if not re.fullmatch(r"agpb_wp_[A-Za-z0-9_]+", args.session_name):
        parser.error("Invalid existing native console session name")
    if args.userid <= 0 or args.waypoint < 0:
        parser.error("userid must be positive and waypoint must be nonnegative")
    if not all(math.isfinite(x) for x in (args.ttl, args.duration, args.renew_interval)) or \
       not 0.5 <= args.ttl <= 30 or not 0.5 <= args.duration <= 120 or not 0.05 <= args.renew_interval < args.ttl:
        parser.error("Require TTL 0.5..30, duration 0.5..120, and 0.05 <= renew interval < TTL")
    try:
        session = temp_child(args.session_directory)
    except ValueError as exc:
        parser.error(str(exc))
    output = Path(tempfile.mkdtemp(prefix="agpb_agent_"))
    print("Receipts: " + str(output), flush=True)
    sender = ConsoleSender(args.powershell, args.session_name, session, ack_timeout=2)
    stop_at = time.monotonic() + args.duration
    owned, identity, sequence, exit_code = None, None, 0, 0
    summary = {"client": "deterministic_goal_lease", "external_llm": False, "arguments": vars(args).copy(),
               "receipts": str(output), "renewals": 0, "automatic_clear": False}
    summary["arguments"]["session_directory"] = str(session)

    def send(command):
        nonlocal sequence
        remaining = stop_at - time.monotonic()
        if remaining <= 0.75:
            raise TimeoutError("Bounded renewal duration ended before another console request")
        # Existing helper owns its single-operator mutex. Bound listener waits;
        # shutdown can take its existing finite helper-close grace period.
        sender.startup_timeout = min(3.0, remaining / 2)
        frame_budget = remaining if sender.process is not None else remaining - sender.startup_timeout
        if frame_budget <= 0.75:
            raise TimeoutError("Insufficient bounded startup/console latency budget")
        sender.frame_timeout = min(4.0, frame_budget)
        sequence += 1
        try:
            receipt = sender.send(command)
        except CommandFailure as exc:
            receipt = exc.receipt
            raise
        finally:
            if "receipt" in locals():
                (output / ("receipt_%04d.json" % sequence)).write_text(
                    json.dumps(receipt, ensure_ascii=False, indent=2), encoding="utf-8")
        return receipt["log"]

    try:
        state = parse_state(send(READ_STATE), args.userid)
        identity = state["identity"]
        if state["token"] != 0:
            raise ValueError("An existing lease is present; refusing to take it over")
        while time.monotonic() < stop_at:
            if owned is not None:
                state = parse_state(send(READ_STATE), args.userid)
                if state["identity"] != identity:
                    raise ValueError("Bot identity, map, round or team changed; renewal stopped")
                if state["state"] not in ("active", "suspended", "camping") or not same_lease(state, owned):
                    summary["reason"] = "Lease ended/replaced/renewed by another writer; left untouched"
                    break
            command = "agpb_agent_intent %d %d %d %s %d %.3f" % (
                state["mapserial"], state["round"], args.userid, args.mode, args.waypoint, args.ttl)
            accepted = parse_state(send(command), args.userid, need_header=False)
            if accepted["state"] not in ("active", "suspended", "camping") or accepted["token"] == 0 or \
               accepted["mode"] != args.mode or accepted["wp"] != args.waypoint or \
               (accepted["mapserial"], accepted["round"]) != (identity[1], identity[2]) or \
               (accepted["lease-map"], accepted["lease-round"]) != (identity[1], identity[2]):
                raise ValueError("Native core did not accept the scoped goal")
            if owned is not None and accepted["token"] != owned["token"]:
                raise ValueError("Renewal unexpectedly replaced task token; stopping")
            owned = accepted
            summary["renewals"] += 1
            summary["last_accepted_lease"] = owned
            print("%s waypoint=%d token=%d deadline=%.3f" %
                  (args.mode, args.waypoint, owned["token"], owned["deadline"]), flush=True)
            time.sleep(max(0.0, min(args.renew_interval, stop_at - time.monotonic())))
        summary.setdefault("reason", "Bounded duration ended; native TTL fallback remains active")
    except (KeyboardInterrupt, TimeoutError):
        summary["reason"] = "Client stopped; no further renewal; native TTL fallback remains active"
    except (CommandFailure, ValueError, KeyError, OSError) as exc:
        summary["reason"], exit_code = str(exc), 1
    finally:
        sender.close()
        summary["owned_lease_last_seen"] = owned
        summary["exit_code"] = exit_code
        (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        print(summary["reason"], flush=True)
        if owned:
            print("No automatic clear. Last accepted game-time deadline: %.3f" % owned["deadline"], flush=True)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
