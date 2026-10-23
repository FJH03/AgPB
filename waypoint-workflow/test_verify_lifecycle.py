"""Offline evidence-policy regression. No game, deployment or fixture execution."""
import copy
import itertools
import re
import unittest

from verify_lifecycle import HASHES, evaluate, evaluate_case, parse, receipt_log

META = dict(map="de_dust2", bot_slot=2, bot_userid=102, bot_name="Test_Bot", human_slot=1, human_userid=101)
META.update({key: "a" * 64 for key in HASHES})
SEQ = itertools.count(1)
CORE = ("[AgPB lifecycle] v=1 name=Test_Bot edict=2 userid=102 map=de_dust2 epoch=1 tick=1000 time=10.000 "
        "alive=1 wasalive=1 lifestate=0 team=2 desired=2 auto=0 roam=0 roundfrozen=0 flags=0x301 hp=100 "
        "pos=(100,0,0) vel=(0,0,0) velknown=1 eye=(0,90) route=2 index=0 node=3 goal=4 owner=5 arrived=0 "
        "settle=-1 settleage=-1 airborne=0 progress=10 stuck=12 recovery=0 task=move_to_position token=5 tasks=2 "
        "break=-1 breaktoken=0 use=-1 action=0 pendingvel=0 resets=5 reset=stop cmdnum=1000 cmdtick=1000 "
        "buttons=0x8 move=(100,0,0) view=(0,90)")
PLAYER = ("[WP_PROBE_STATE] t=10.000 map=de_dust2 slot=2 userid=102 fake=1 team=2 alive=1 flags=65921 "
          "pos=(100 0 0) vel=(0 0 0) zone=0 defusing=0 v=1 epoch=100 map_serial=1 tick=1000 serial=258 frozen=0 "
          "view_ent=-1 eye=(0 90 0) buttons=8 player_state=0 life_state=0 observer=0 deaths=0 spawns=1 teams=1 "
          "round_starts=0 round_ends=0 lifecycle_seq=1 client_deaths=0 client_spawns=1 client_teams=1")
CLEAR = dict(route=0, index=0, node=-1, goal=-1, owner=0, arrived=0, settle=-1, settleage=-1,
             airborne=0, progress=-1, recovery=0, task="normal", tasks=1, action=0,
             buttons="0x0", move="(0,0,0)", resets=6)
FROZEN = dict(flags="0x341", buttons="0x0", move="(0,0,0)", settle=-1, settleage=-1, progress=-1)


def change(text, values):
    for key, value in values.items():
        text, count = re.subn(r"(?<!\w)" + re.escape(key) + r"=(?:\([^)]*\)|\S+)", lambda m: key + "=" + str(value), text)
        if count != 1:
            raise AssertionError("Unknown test protocol field: " + key)
    return text


def framed(text, command="agpb_bot_info 0; agpb_wp_probe_state; agpb_wp_probe_lifecycle"):
    nonce = str(next(SEQ))
    begin, end = "AGPB_SESSION_BEGIN_" + nonce, "AGPB_SESSION_END_" + nonce
    log = begin + " \n" + text + "\n" + end + "\n"
    return {"returncode": 0, "command": command, "log": log, "ack": {"beginMarker": begin, "endMarker": end,
            "logResetCount": 0, "endByteOffset": int(nonce) * 10000}}


def sample(t, c=None, p=None, pos="100,0,0", command=None, extra="", human_team=2):
    tick = round(t * 100)
    cv = dict(tick=tick, cmdtick=tick, cmdnum=tick, time=f"{t:.3f}", pos="(" + pos + ")")
    cv.update(c or {})
    pv = dict(t=f"{t:.3f}", tick=tick, pos="(" + pos.replace(",", " ") + ")")
    pv.update(p or {})
    core = change(CORE, cv)
    bot = change(PLAYER, pv)
    human = change(PLAYER, dict(t=f"{t:.3f}", tick=tick, slot=1, userid=101, serial=1, fake=0, flags=65665, pos="(-100 0 0)", buttons=0, team=human_team))
    human = change(human, {key: pv[key] for key in ("map_serial", "epoch", "map") if key in pv})
    return framed(core + "\n" + human + "\n" + bot + ("\n" + extra if extra else ""), command or "agpb_bot_info 0; agpb_wp_probe_state; agpb_wp_probe_lifecycle")


def event(name, t, seq, **changes):
    text = (f"[WP_PROBE_LIFECYCLE] v=1 epoch=100 map_serial=1 tick={round(t*100)} t={t:.3f} map=de_dust2 seq={seq} "
            f"event={name} userid=102 slot=2 serial=258 fake=1 team=2 alive=1 oldteam=-1 newteam=-1 deaths=1 spawns=2 teams=3 "
            "round_starts=1 round_ends=1 winner=-1 reason=-1 replay=1")
    return change(text, changes)


def case(kind, stages, **extras):
    return dict(kind=kind, binding={key: META[key] for key in HASHES}, stages=stages, **extras)


def stop_case():
    return case("stop", {"before": [sample(10)], "stopped": [sample(t, dict(CLEAR, reset="stop"), {"buttons": 0}) for t in (11, 11.5, 12)]})


def freeze_case(settle=False):
    before = sample(10, {"settle": 9.8, "settleage": .2} if settle else {})
    frozen = [sample(t, FROZEN, {"flags": 65953, "frozen": 1, "buttons": 0}) for t in (11, 11.5, 12)]
    if settle:
        resumed = [sample(12.2, {"settle": 12.1, "settleage": .1}), sample(12.6, {"settle": 12.1, "settleage": .5}),
                   sample(13.1, dict(CLEAR, resets=5), {"buttons": 0}, extra="[AgPB] Test_Bot: arrived at waypoint #4 (1 hop(s) walked)")]
    else:
        resumed = [sample(12.2), sample(12.6, pos="110,0,0")]
    return case("freeze_settle" if settle else "freeze_resume", {"before": [before], "frozen": frozen, "resumed": resumed})


def reset_case(kind):
    stages = {"before": [sample(10)]}
    if kind == "death_respawn":
        dead = {"alive": 0, "life_state": 1, "client_deaths": 1, "deaths": 1, "lifecycle_seq": 2, "buttons": 0}
        spawned = dict(dead, alive=1, life_state=0, client_spawns=2, spawns=2, lifecycle_seq=3)
        stages["dead"] = [sample(11, dict(CLEAR, alive=0, wasalive=0, lifestate=1, reset="dead"), dead, extra=event("player_death", 10.8, 2, alive=0))]
        stages["respawned"] = [sample(12, dict(CLEAR, reset="dead"), spawned, extra=event("player_spawn", 11.8, 3))]
        recoverprobe = spawned
    elif kind == "team":
        spec = dict(team=1, alive=0, client_teams=2, teams=2, lifecycle_seq=2, buttons=0)
        returned = dict(spec, team=2, alive=1, client_teams=4, teams=4, lifecycle_seq=4)
        stages["changed"] = [sample(11, dict(CLEAR, team=1, alive=0, reset="team-change"), spec, extra=event("player_team", 10.8, 2, oldteam=2, newteam=1))]
        stages["returned"] = [sample(11.7, dict(CLEAR, team=3, reset="team-change"), dict(returned, team=3, client_teams=3, lifecycle_seq=3), human_team=3, extra=event("player_team", 11.5, 3, oldteam=1, newteam=3)),
                              sample(12.5, dict(CLEAR, reset="team-change"), returned, extra=event("player_team", 12.2, 4, oldteam=1, newteam=2))]
        recoverprobe = returned
    else:
        stages["before"][0]["command"] += "; endround"
        recoverprobe = dict(round_starts=1, round_ends=1, lifecycle_seq=3, buttons=0)
        stages["cleared"] = [sample(11, dict(CLEAR, epoch=2, reset="round-start"), recoverprobe, extra=event("round_end", 10, 2, userid=0, slot=-1, serial=-1, fake=-1) + "\n" + event("round_start", 10.8, 3, userid=0, slot=-1, serial=-1, fake=-1))]
    stages["recovered"] = [sample(13, {"resets": 6, "epoch": 2 if kind == "round" else 1}, recoverprobe, command="agpb_bot_goto 0 4; agpb_bot_info 0; agpb_wp_probe_state; agpb_wp_probe_lifecycle"),
                           sample(13.5, {"resets": 6, "epoch": 2 if kind == "round" else 1}, recoverprobe, pos="110,0,0")]
    return case(kind, stages)


def map_case():
    before = sample(10)
    middle = change(PLAYER, dict(map="cs_office", map_serial=2, slot=1, userid=101, serial=1, fake=0, lifecycle_seq=3, buttons=0))
    map_line = "[WP_PROBE_MAP] v=1 epoch=100 map_serial=2 tick=1000 t=10.000 map=cs_office seq=3 event=map_start replay=1"
    returned = "[WP_PROBE_MAP] v=1 epoch=100 map_serial=3 tick=1000 t=10.000 map=de_dust2 seq=5 event=map_start replay=1"
    absent = framed(middle + "\n" + map_line, "agpb_kick all; changelevel cs_office")
    returned_empty = framed(change(middle, {"map": "de_dust2", "map_serial": 3}) + "\n" + returned)
    cp = dict(CLEAR, name="Test_New", userid=103)
    pp = dict(userid=103, serial=259, map_serial=3, lifecycle_seq=5, buttons=0)
    recovered = [sample(10, cp, pp, extra=returned),
                 sample(11, dict(name="Test_New", userid=103), pp, command="agpb_bot_goto 0 4"),
                 sample(11.5, dict(name="Test_New", userid=103), pp, pos="110,0,0")]
    return case("map", {"before": [before], "absent": [absent, returned_empty], "recovered": recovered},
                recovery_identity=dict(bot_userid=103, bot_name="Test_New"))


class LifecycleEvidenceTests(unittest.TestCase):
    def verdict(self, item, expected="passed", reason=None):
        result = evaluate_case(item, META)
        self.assertEqual(result["status"], expected, result)
        if reason:
            self.assertIn(reason, result["error"])
        return result

    def test_intermediate_empty_map_and_clean_new_bot_on_return(self):
        self.verdict(map_case())
        item = map_case()
        item["stages"]["recovered"][0]["log"] = item["stages"]["recovered"][0]["log"].replace("map_serial=3", "map_serial=1")
        self.verdict(item, "failed")

    def test_round_requires_native_endround_and_new_real_end_then_start(self):
        item = reset_case("round")
        item["stages"]["before"][0]["command"] = "mp_restartgame 1"
        self.verdict(item, "failed", "endround request")
        item = reset_case("round")
        receipt = item["stages"]["cleared"][0]
        receipt["log"] = "\n".join(line for line in receipt["log"].splitlines() if "event=round_end " not in line) + "\n"
        self.verdict(item, "failed", "round_end")

    def test_spectator_and_back_without_other_faction_is_insufficient(self):
        item = reset_case("team")
        item["stages"]["returned"] = item["stages"]["returned"][1:]
        self.verdict(item, "failed", "other playing faction")

    def test_same_tick_event_after_state_is_ordered_by_new_sequence(self):
        item = reset_case("death_respawn")
        receipt = item["stages"]["dead"][0]
        receipt["log"] = receipt["log"].replace("tick=1080 t=10.800", "tick=1000 t=10.000")
        self.verdict(item)

    def test_frozen_task_preserves_its_nonzero_entry_clock(self):
        item = freeze_case()
        for name in item["stages"]:
            for receipt in item["stages"][name]:
                receipt["log"] = receipt["log"].replace("action=0 ", "action=9.5 ")
        self.verdict(item)
        item["stages"]["frozen"][0]["log"] = item["stages"]["frozen"][0]["log"].replace("action=9.5 ", "action=0 ")
        self.verdict(item, "failed", "task entry clock")

    def test_intermediate_spectator_event_cannot_mask_the_later_real_team_restore(self):
        item = reset_case("team")
        receipt = item["stages"]["returned"][-1]
        receipt["log"] = receipt["log"].replace("lifecycle_seq=4", "lifecycle_seq=5").replace("client_teams=4", "client_teams=5").replace("seq=4 event=player_team", "seq=5 event=player_team")
        receipt["log"] = receipt["log"].replace(receipt["ack"]["endMarker"], event("player_team", 12.1, 4, oldteam=3, newteam=1) + "\n" + receipt["ack"]["endMarker"])
        self.verdict(item)

    def test_stop_releases_input_and_holds_a_nonzero_heading(self):
        self.verdict(stop_case())

    def test_real_frozen_layouts_differ_and_automatic_route_resume_passes(self):
        self.verdict(freeze_case())

    def test_pre_freeze_settle_must_be_restarted_after_unfreeze(self):
        item = freeze_case(True)
        self.verdict(item)
        item["stages"]["resumed"][0]["log"] = change(item["stages"]["resumed"][0]["log"], {"settle": 9.8})
        item["stages"]["resumed"][1]["log"] = change(item["stages"]["resumed"][1]["log"], {"settle": 9.8})
        self.verdict(item, "failed", "newly started")

    def test_dying_is_not_alive_and_missing_new_lifestate_cannot_pass(self):
        for replacement in ("lifestate=1", ""):
            item = stop_case()
            item["stages"]["before"][0]["log"] = item["stages"]["before"][0]["log"].replace("lifestate=0", replacement)
            self.verdict(item, "failed")

    def test_route_can_advance_until_freeze_applies_then_must_stay_fixed(self):
        item = freeze_case()
        for receipt in item["stages"]["frozen"]:
            receipt["log"] = receipt["log"].replace("index=0", "index=1").replace("node=3", "node=4")
        self.verdict(item)
        item["stages"]["frozen"][1]["log"] = item["stages"]["frozen"][1]["log"].replace("index=1", "index=0")
        self.verdict(item, "failed", "retain")

    def test_real_actor_events_and_per_identity_counters_prove_lifecycles(self):
        for kind in ("death_respawn", "team", "round"):
            with self.subTest(kind=kind):
                self.verdict(reset_case(kind))

    def test_global_death_counter_or_replayed_old_event_cannot_certify_target_death(self):
        item = reset_case("death_respawn")
        item["stages"]["dead"][0]["log"] = item["stages"]["dead"][0]["log"].replace("client_deaths=1", "client_deaths=0")
        self.verdict(item, "failed", "real death")
        item = reset_case("death_respawn")
        item["stages"]["dead"][0]["log"] = item["stages"]["dead"][0]["log"].replace("seq=2 event=player_death", "seq=1 event=player_death")
        self.verdict(item, "failed", "real event")

    def test_movement_type_sm_freeze_and_round_freeze_are_not_real_frozen(self):
        item = freeze_case()
        for receipt in item["stages"]["frozen"]:
            receipt["log"] = receipt["log"].replace("flags=0x341", "flags=0x301").replace("flags=65953", "flags=65921").replace("frozen=1", "frozen=0")
        self.verdict(item, "failed", "real FL_FROZEN")
        item = freeze_case()
        item["stages"]["frozen"][0]["log"] = item["stages"]["frozen"][0]["log"].replace("roundfrozen=0", "roundfrozen=1")
        self.verdict(item, "failed", "real FL_FROZEN")

    def test_unknown_versions_missing_velocity_and_stale_cmd_are_rejected(self):
        mutations = (("velknown=1", "velknown=0"), ("v=1 name=", "v=2 name="), ("cmdtick=1100", "cmdtick=1000"), ("velknown=1", ""))
        for old, new in mutations:
            with self.subTest(old=old, new=new):
                item = stop_case()
                item["stages"]["stopped"][0]["log"] = item["stages"]["stopped"][0]["log"].replace(old, new)
                self.verdict(item, "failed")

    def test_stop_heading_reset_and_leftover_attack_fail(self):
        for key, value in (("view", "(0,0)"), ("buttons", "0x1"), ("pendingvel", "1"), ("use", "55")):
            with self.subTest(field=key):
                item = stop_case()
                item["stages"]["stopped"][1]["log"] = re.sub(r"(?<!\w)" + key + r"=(?:\([^)]*\)|\S+)", key + "=" + value, item["stages"]["stopped"][1]["log"], count=1)
                self.verdict(item, "failed")

    def test_reissued_goto_and_changed_ownership_do_not_prove_automatic_resume(self):
        item = freeze_case()
        item["stages"]["resumed"][0]["command"] = "agpb_bot_goto 0 4"
        self.verdict(item, "failed", "explicitly")
        item = freeze_case()
        item["stages"]["frozen"][0]["log"] = item["stages"]["frozen"][0]["log"].replace("owner=5", "owner=6")
        self.verdict(item, "failed", "Original route")

    def test_wrong_player_serial_enemy_team_and_mixed_dll_refuse(self):
        item = stop_case()
        item["stages"]["stopped"][0]["log"] = item["stages"]["stopped"][0]["log"].replace("serial=258", "serial=259")
        self.verdict(item, "failed", "generation changed")
        item = stop_case()
        receipt = item["stages"]["before"][0]
        receipt["log"] = receipt["log"].replace(receipt["ack"]["endMarker"], change(PLAYER, {"slot": 3, "userid": 103, "serial": 259, "team": 3}) + "\n" + receipt["ack"]["endMarker"])
        self.verdict(item, "failed", "share the human team")
        item = stop_case()
        item["binding"]["dll_sha256"] = "b" * 64
        self.verdict(item, "failed", "artifact version")

    def test_short_window_duplicate_receipt_and_claimed_passed_are_not_trusted(self):
        item = stop_case()
        item["stages"]["stopped"] = item["stages"]["stopped"][:2]
        item["passed"] = True
        self.verdict(item, "failed", "Too few")
        item = stop_case()
        item["stages"]["stopped"][1] = copy.deepcopy(item["stages"]["stopped"][0])
        self.verdict(item, "failed", "reused")
        document = {"schema_version": 1, "metadata": META, "cases": [stop_case()]}
        self.assertIn("map", evaluate(document)["missing_kinds"])
        self.assertEqual(evaluate(document)["status"], "failed")

    def test_unknown_input_sentinels_and_malformed_document_refuse(self):
        item = stop_case()
        item["stages"]["before"][0]["log"] = item["stages"]["before"][0]["log"].replace("flags=65921", "flags=-1")
        self.verdict(item, "failed", "Unknown player flags")
        self.assertEqual(evaluate_case(None, META)["status"], "failed")
        with self.assertRaisesRegex(ValueError, "must be an object"):
            evaluate([])

    def test_observed_log_requires_its_original_nonoverlapping_byte_offsets(self):
        item = stop_case()
        receipt = item["stages"]["stopped"][0]
        receipt["observed_log"] = receipt["log"]
        self.verdict(item, "failed", "offsets")
        receipt["observed_start_offset"] = item["stages"]["before"][0]["ack"]["endByteOffset"]
        receipt["observed_end_offset"] = receipt["ack"]["endByteOffset"]
        self.verdict(item)
        receipt["observed_start_offset"] = 0
        self.verdict(item, "failed", "offsets")

    def test_observed_events_outside_ack_preserve_original_marker_spacing(self):
        receipt = sample(10)
        receipt["observed_log"] = event("player_spawn", 9.8, 1) + "\r\n" + receipt["log"].replace("\n", "\r\n").replace("AGPB_SESSION_END_", "AGPB_SESSION_END_")
        receipt["observed_log"] = receipt["observed_log"].replace(receipt["ack"]["endMarker"] + "\r\n", receipt["ack"]["endMarker"] + " \r\n")
        log, _, _ = receipt_log(receipt)
        self.assertEqual(len(parse(log)["events"]), 1)
        receipt["observed_log"] = "unrelated new log"
        with self.assertRaisesRegex(ValueError, "unaltered ACK"):
            receipt_log(receipt)


if __name__ == "__main__":
    unittest.main()
