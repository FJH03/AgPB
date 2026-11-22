#!/usr/bin/env python3
r"""Bounded listen-server console client for waypoint goals and optional LLM chat.

Example (one console operator, existing running game/session, current bot userid):
  py tools/agpb_agent.py --run --session-name agpb_wp_agent \
    --session-directory "%TEMP%\agpb_wp_agent" --userid 8 \
    --mode camp --waypoint 60 --ttl 3 --duration 10 --renew-interval 1

The server starts with both external overrides off. Set
agpb_agent_combat_override 1 for waypoint-goal submissions and
agpb_agent_chat_override 1 for provider decisions.
Pass --chat-provider last, followed by the executable and its arguments.
Include --chat-only to watch chat without a waypoint goal. The provider reads
one UTF-8 JSON line {"version":1,"event":{...,"text":"..."}} on stdin and
writes one JSON object to stdout. Use {"reply":"some text"} to speak,
{"reply":null} to defer to local chat, or {"suppress":true} to skip the event.
The native deadline governs fallback.

The CLI uses the existing listen-server console transport. Other harnesses
can use agpb_agent_chat_events and agpb_agent_chat_reply through an authorized
server console transport.

Goal renewal leaves existing leases alone and gives control back at the
native game-time deadline. Console receipts and the summary go to system TEMP.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "waypoint-workflow"))
from verify_routes import ConsoleSender, CommandFailure, temp_child

READ_STATE = "agpb_agent_state\nagpb_bot_info all"
READ_CHAT = "agpb_agent_chat_events"
AGENT_SCOPE = "[AgPB agent] v=1 "
COMBAT_DISABLED = "[AgPB] agent intent rejected: combat override disabled"
CHAT_CUES = {"reply", "join", "kill", "death", "plant", "idle"}
CHAT_REPLY_BYTES = 120
CHAT_REPLY_RESERVE = 0.35
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
        if line.startswith(AGENT_SCOPE):
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


def combat_override_enabled(log):
    scope = [fields(line) for line in log.splitlines() if line.strip().startswith(AGENT_SCOPE)]
    if len(scope) != 1:
        raise ValueError("Missing or ambiguous agent state scope")
    value = scope[0].get("combat_override", "0")
    if value not in ("0", "1"):
        raise ValueError("Invalid combat override in agent state")
    return value == "1"


def same_lease(current, owned):
    return owned is not None and all(current[key] == owned[key] for key in
        ("mapserial", "round", "token", "mode", "wp", "lease-map", "lease-round")) and \
        abs(current["deadline"] - owned["deadline"]) <= 0.0005


def parse_chat_events(log):
    """Validate the complete native batch before using any pending event."""
    events = []
    seen = set()
    ended = False
    for line in log.splitlines():
        line = line.strip()
        if line == "[AgPB] chat-events end":
            if ended:
                raise ValueError("Duplicate chat event batch terminator")
            ended = True
        elif line.startswith("[AgPB] chat-event "):
            if ended:
                raise ValueError("Chat event follows its batch terminator")
            row = fields(line)
            event = {}
            for key, minimum, maximum in (
                ("id", 1, 2**64 - 1), ("map", 1, 2**32 - 1), ("round", 0, 2**32 - 1),
                ("userid", 0, 2**31 - 1), ("bot", 1, 2**31 - 1), ("botuserid", 1, 2**31 - 1),
                ("team", 2, 3), ("teamonly", 0, 1),
            ):
                value = row[key]
                if not re.fullmatch(r"[0-9]+", value) or not minimum <= int(value) <= maximum:
                    raise ValueError("Invalid chat event field " + key)
                event[key] = int(value)
            event["cue"] = row["cue"]
            event["ttl"] = float(row["ttl"])
            if event["cue"] not in CHAT_CUES or not math.isfinite(event["ttl"]) or event["ttl"] < 0:
                raise ValueError("Invalid chat event cue or TTL")
            encoded = row.get("text_hex", "")
            if encoded != "-" and not re.fullmatch(r"(?:[0-9a-fA-F]{2}){0,127}", encoded):
                raise ValueError("Invalid chat event text encoding")
            event["text"] = bytes.fromhex(encoded).decode("utf-8", errors="replace") if encoded != "-" else ""
            if event["id"] in seen:
                raise ValueError("Duplicate chat event ID")
            seen.add(event["id"])
            events.append(event)
    if not ended:
        raise ValueError("Missing chat event batch terminator")
    return events


def chat_payload(response):
    if response == {"suppress": True} and response["suppress"] is True:
        return "-"
    if isinstance(response, dict) and response.keys() == {"reply"}:
        reply = response["reply"]
        if reply is None:
            return None
        if isinstance(reply, str):
            if not reply.strip() or any(ord(char) < 32 or ord(char) == 127 or char in '";\\' for char in reply):
                raise ValueError("Invalid chat reply text")
            encoded = reply.encode("utf-8")
            if len(encoded) <= CHAT_REPLY_BYTES:
                return encoded.hex()
    raise ValueError("Invalid chat provider response")


def run_chat_provider(argv, event, timeout):
    request = json.dumps({"version": 1, "event": event}, ensure_ascii=False) + "\n"
    result = subprocess.run(argv, input=request, capture_output=True, check=True,
                            text=True, encoding="utf-8", timeout=timeout,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return chat_payload(json.loads(result.stdout))


def parse_chat_reply(log, event_id):
    replies = [fields(line) for line in log.splitlines() if line.strip().startswith("[AgPB] chat-reply ")]
    if len(replies) != 1:
        raise ValueError("Missing or ambiguous native chat reply acknowledgement")
    reply = replies[0]
    if reply.get("id") != str(event_id) or reply.get("accepted") not in ("0", "1"):
        raise ValueError("Native chat reply acknowledgement has the wrong scope")
    return reply["accepted"] == "1"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run", action="store_true", help="Explicitly allow bounded native submissions")
    parser.add_argument("--session-name", required=True)
    parser.add_argument("--session-directory", required=True, type=Path)
    parser.add_argument("--userid", type=int)
    parser.add_argument("--mode", choices=("move", "camp"))
    parser.add_argument("--waypoint", type=int)
    parser.add_argument("--ttl", type=float, default=3.0, help="Native game seconds, 0.5..30 (default 3)")
    parser.add_argument("--duration", type=float, default=10.0, help="Client wall seconds, 0.5..120 (default 10)")
    parser.add_argument("--renew-interval", type=float, default=1.0)
    parser.add_argument("--chat-only", action="store_true", help="Poll chat without a waypoint goal")
    parser.add_argument("--chat-interval", type=float, default=0.2, help="Chat poll interval in wall seconds")
    parser.add_argument("--chat-timeout", type=float, default=1.5, help="Maximum wall seconds per chat provider call")
    parser.add_argument("--powershell", default=shutil.which("pwsh") or shutil.which("powershell") or "powershell.exe")
    parser.add_argument("--chat-provider", nargs=argparse.REMAINDER,
                        help="Chat provider executable and arguments, supplied last")
    args = parser.parse_args(argv)
    if not args.run:
        parser.error("No game commands are sent without --run")
    if not re.fullmatch(r"agpb_wp_[A-Za-z0-9_]+", args.session_name):
        parser.error("Invalid existing native console session name")
    goal = (args.userid, args.mode, args.waypoint)
    if args.chat_only:
        if not args.chat_provider or any(value is not None for value in goal):
            parser.error("--chat-only requires --chat-provider and no goal arguments")
    elif any(value is None for value in goal) or args.userid <= 0 or args.waypoint < 0:
        parser.error("Goals require positive --userid, --mode, and nonnegative --waypoint")
    if args.chat_provider == []:
        parser.error("--chat-provider requires an executable")
    if not all(math.isfinite(x) for x in (args.ttl, args.duration, args.renew_interval)) or \
       not 0.5 <= args.ttl <= 30 or not 0.5 <= args.duration <= 120 or not 0.05 <= args.renew_interval < args.ttl:
        parser.error("Require TTL 0.5..30, duration 0.5..120, and 0.05 <= renew interval < TTL")
    if not 0.05 <= args.chat_interval <= 1 or not 0.05 <= args.chat_timeout <= 10:
        parser.error("Require chat interval 0.05..1 and provider timeout 0.05..10")
    try:
        session = temp_child(args.session_directory)
    except ValueError as exc:
        parser.error(str(exc))
    output = Path(tempfile.mkdtemp(prefix="agpb_agent_"))
    print("Receipts: " + str(output), flush=True)
    sender = ConsoleSender(args.powershell, args.session_name, session, ack_timeout=2)
    stop_at = time.monotonic() + args.duration
    owned, identity, sequence, exit_code = None, None, 0, 0
    summary = {"client": "chat_harness" if args.chat_only else "deterministic_goal_lease",
               "external_llm": bool(args.chat_provider),
               "arguments": {key: value for key, value in vars(args).items() if key != "chat_provider"},
               "receipts": str(output), "renewals": 0, "automatic_clear": False}
    summary["arguments"]["session_directory"] = str(session)
    attempted_chat = set()
    chat = dict(accepted=0, rejected=0, declined=0, provider_errors=0, expired=0)
    if args.chat_provider:
        summary["chat"] = chat

    def send(command):
        nonlocal sequence
        remaining = stop_at - time.monotonic()
        if remaining <= 0.75:
            raise TimeoutError("Client duration ended before another console request")
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

    def poll_chat(work_until):
        # Each provider call starts from a fresh batch. Disabled chat produces an empty batch.
        requested_at = time.monotonic()
        events = parse_chat_events(send(READ_CHAT))
        attempted_chat.intersection_update((event["map"], event["id"]) for event in events)
        for event in events:
            key = event["map"], event["id"]
            if key in attempted_chat:
                continue
            native_deadline = requested_at + event["ttl"]
            deadline = min(work_until, native_deadline)
            now = time.monotonic()
            budget = deadline - now - CHAT_REPLY_RESERVE
            if budget <= 0.0:
                if now + CHAT_REPLY_RESERVE >= native_deadline:
                    attempted_chat.add(key)
                    chat["expired"] += 1
                continue
            attempted_chat.add(key)
            try:
                payload = run_chat_provider(args.chat_provider, event, min(args.chat_timeout, budget))
            except (OSError, ValueError, subprocess.SubprocessError):
                chat["provider_errors"] += 1
                return
            if payload is None:
                chat["declined"] += 1
                return
            if time.monotonic() >= deadline:
                chat["expired"] += 1
                return
            command = "agpb_agent_chat_reply %d %d %d %d %d %s" % (
                event["id"], event["map"], event["round"], event["userid"], event["botuserid"], payload)
            accepted = parse_chat_reply(send(command), event["id"])
            chat["accepted" if accepted else "rejected"] += 1
            return

    def renew_goal():
        nonlocal identity, owned
        log = send(READ_STATE)
        enabled = combat_override_enabled(log)
        if summary.get("combat_override") != enabled:
            print("External combat override %s" % ("enabled" if enabled else "disabled"), flush=True)
        summary["combat_override"] = enabled
        if not enabled:
            owned = None
            return True
        state = parse_state(log, args.userid)
        if identity is None:
            identity = state["identity"]
        elif state["identity"] != identity:
            raise ValueError("Bot identity, map, round or team changed; renewal stopped")
        if owned is None:
            if state["token"] != 0:
                raise ValueError("An existing lease is present; refusing to take it over")
        elif state["state"] not in ("active", "suspended", "camping", "clearing") or not same_lease(state, owned):
            summary["reason"] = "Lease ended/replaced/renewed by another writer; left untouched"
            return False
        command = "agpb_agent_intent %d %d %d %s %d %.3f" % (
            state["mapserial"], state["round"], args.userid, args.mode, args.waypoint, args.ttl)
        log = send(command)
        if any(line.strip() == COMBAT_DISABLED for line in log.splitlines()):
            owned = None
            summary["combat_override"] = False
            print("External combat override disabled", flush=True)
            return True
        accepted = parse_state(log, args.userid, need_header=False)
        if accepted["state"] not in ("active", "suspended", "camping", "clearing") or accepted["token"] == 0 or \
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
        return True

    try:
        next_goal_at = stop_at if args.chat_only else time.monotonic()
        next_chat_at = time.monotonic() if args.chat_provider else stop_at
        while (now := time.monotonic()) < stop_at:
            if now >= next_goal_at:
                if not renew_goal():
                    if not args.chat_provider:
                        break
                    next_goal_at = stop_at
                else:
                    next_goal_at = min(stop_at, time.monotonic() + args.renew_interval)
            elif now >= next_chat_at and now + CHAT_REPLY_RESERVE < next_goal_at:
                poll_chat(next_goal_at)
                next_chat_at = min(stop_at, time.monotonic() + args.chat_interval)
            else:
                chat_fits = max(now, next_chat_at) + CHAT_REPLY_RESERVE < next_goal_at
                wake_at = next_chat_at if chat_fits else next_goal_at
                time.sleep(max(0.0, wake_at - now))
        summary.setdefault("reason", "Bounded duration ended. Native fallback remains active.")
    except (KeyboardInterrupt, TimeoutError):
        summary["reason"] = "Client stopped. Native fallback remains active."
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
