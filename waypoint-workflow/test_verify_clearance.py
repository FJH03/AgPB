"""Evidence-policy tests: no game control and no mocked clearance AI."""
import hashlib
import json
import re
import unittest

from test_verify_lifecycle import CLEAR, change, event as lifecycle_event, framed, sample
from verify_clearance import HASHES, applied, evaluate, evaluate_case, parse, snapshot_target

META = dict(map="de_dust2", bot_slot=2, bot_userid=102, bot_name="Test_Bot", human_slot=1, human_userid=101)
META.update({key: "a" * 64 for key in HASHES})
CLEARANCE = ("[AgPB clearance] v=1 name=Test_Bot edict=2 userid=102 map=de_dust2 tick=1000 time=10.000 "
    "target=0xFFFFFFFF ent=-1 route=0 goal=-1 owner=0 index=0 node=-1 next=-1 breaktoken=0 acquired=-1 deadline=0 "
    "shots=0 nodamage=0 besthealth=-1 near=0 release=0 knifepending=0 trackedweapon=0xFFFFFFFF weapon=0x1234 class=weapon_usp "
    "clip=12 ammotype=8 reserve=100 nextattack=0 playerattack=0 cmdnum=1000 cmdtick=1000 buttons=0x0 weaponselect=0 "
    "move=(0,0,0) view=(0,90) attackcmds=0 lastattacktick=-1")
TARGET = ("[WP_CLEARANCE_TARGET] v=1 epoch=1000 map_serial=1 tick=1000 t=10.000 map=de_dust2 watch=0 generation=1 "
    "ent=713 ref=-1001 exists=1 class=prop_physics_multiplayer name=\"radio\" model=\"models/office radio.mdl\" "
    "damage_hooked=1 initial_health_known=1 initial_health=10 health_known=1 health=10 broken_known=0 broken=-1 "
    "pos_known=1 pos=(100 0 0) damage_seq=0 break_seq=0 destroy_seq=0")
WEAPON = ("[WP_CLEARANCE_WEAPON] v=1 epoch=1000 map_serial=1 tick=1000 t=10.000 map=de_dust2 slot=2 userid=102 serial=258 "
    "fake=1 alive=1 active_ent=114 active_ref=-2001 class=weapon_usp clip_known=1 clip=12 ammotype_known=1 ammotype=8 "
    "reserve_known=1 reserve=100 reload_known=1 reload=0 primary_known=1 primary=0 secondary_known=1 secondary=0 "
    "player_attack_known=1 player_attack=0 smack_known=0 smack_time=-1 client_fires=0 client_damages=0")
COUNTS = ("[WP_CLEARANCE_COUNTS] v=1 epoch=1000 map_serial=1 tick=1000 t=10.000 map=de_dust2 seq=0 damage_calls=0 "
    "breaks=0 destroys=0 fires=0 watched=1 cached=0 break_prop_hooked=1 break_breakable_hooked=1 weapon_fire_hooked=1")
EVENT = ("[WP_CLEARANCE_EVENT] v=1 epoch=1000 map_serial=1 tick=1120 t=11.200 map=de_dust2 seq=1 event=damage_post watch=0 "
    "generation=1 ent=713 ref=-1001 class=prop_physics_multiplayer damage_id=1 context_valid=1 attribution=damage_callback "
    "attacker_slot=2 attacker_userid=102 attacker_serial=258 attacker_fake=1 weapon_ref=-2001 weapon_class=weapon_usp "
    "active_ref=-2001 active_class=weapon_usp smack_known=0 smack_time=-1 damage_known=1 damage=15 damagetype=2 "
    "health_before_known=1 health_before=10 health_after_known=1 health_after=-5 event_weapon=usp replay=1")


def probe_change(text, updates):
    # These fields contain quoted spaces; the production parser must preserve them.
    for key, value in updates.items():
        text, count = re.subn(r"(?<!\w)" + re.escape(key) + r'=(?:\([^)]*\)|"[^"\r\n]*"|\S+)', lambda match: key + "=" + str(value), text)
        if count != 1:
            raise AssertionError(key)
    return text


def event_rows(destroyed=True):
    fire = probe_change(EVENT, dict(seq=1, event="weapon_fire", watch=-1, generation=-1, ent=-1, ref=-1,
                                    damage_id=0, damage_known=0, damage=-1, attribution="game_event"))
    damage = probe_change(EVENT, dict(seq=2, health_after=-5 if destroyed else 5))
    result = [fire, damage]
    if destroyed:
        result += [probe_change(EVENT, dict(seq=3, event="break_prop", attribution="game_event", damage_known=0)),
                   probe_change(EVENT, dict(seq=4, event="destroy", attribution="destruction_damage_context", context_valid=0, damage_known=0))]
    return result


def frame(t, active=False, breaking=False, pos="100,0,0", attacks=0, destroyed=False, events=(), extra="", dead=False):
    tick = round(t * 100)
    life = dict(CLEAR, resets=5, eye="(0,90)", view="(0,90)")
    clear = dict(tick=tick, time=f"{t:.3f}", cmdnum=tick, cmdtick=tick, attackcmds=attacks, lastattacktick=1120 if attacks else -1)
    if active:
        for key, value in dict(route=2, goal=4, owner=5, index=0, node=3, task="move_to_position", token=5, tasks=2, move="(100,0,0)").items():
            life[key] = value
        clear.update(route=2, goal=4, owner=5, index=0, node=3, next=4, move="(100,0,0)")
    if breaking:
        life.update(task="destroy_breakable", token=7, tasks=3, **{"break": 713, "breaktoken": 7}, move="(0,0,0)")
        clear.update(target="0xA003", ent=713, breaktoken=7, acquired=11, deadline=21, besthealth=5 if attacks else 10,
                     trackedweapon="0x1234", shots=attacks, move="(0,0,0)")
    p = {"buttons": 0}
    if dead:
        life.update(alive=0, wasalive=0, lifestate=1, resets=6, reset="dead")
        p.update(alive=0, life_state=1, client_deaths=1, lifecycle_seq=2)
    rec = sample(t, life, p, pos=pos)
    common = dict(tick=tick, t=f"{t:.3f}")
    def latest(*names):
        return max([int(re.search(r" seq=(\d+)", e)[1]) for e in events if any("event="+name+" " in e for name in names)]+[0])
    target = probe_change(TARGET, dict(common, exists=int(not destroyed), health_known=int(not destroyed), health=-1 if destroyed else 5 if attacks else 10,
                                      damage_seq=latest("damage_post"), break_seq=latest("break_prop","break_breakable"), destroy_seq=latest("destroy")))
    damage_count=sum("event=damage_post " in e for e in events)
    fire_count=sum("event=weapon_fire " in e for e in events)
    weapon = change(WEAPON, dict(common, alive=int(not dead), clip=12-attacks, client_fires=fire_count, client_damages=damage_count))
    count = change(COUNTS, dict(common, seq=len(events), damage_calls=damage_count, breaks=int(destroyed), destroys=int(destroyed), fires=fire_count, cached=len(events)))
    added = "\n".join([change(CLEARANCE, clear), target, weapon, count, *events, extra]) + "\n"
    rec["log"] = rec["log"].replace(rec["ack"]["endMarker"], added + rec["ack"]["endMarker"])
    return rec


def gun_case():
    before = frame(10)
    goto = framed("[AgPB] Test_Bot: walking to #4, 2 hop(s): 3 4\n[AgPB] Test_Bot: breaking obstacle ent=713; preserving hop 0 and goal #4", "agpb_bot_goto 0 4")
    events = event_rows()
    run = [goto, frame(11, active=True, breaking=True), frame(11.5, active=True, pos="110,0,0", attacks=1, destroyed=True, events=events),
           frame(12, active=True, pos="120,0,0", attacks=1, destroyed=True, events=events),
           frame(12.5, pos="130,0,0", attacks=1, destroyed=True, events=events, extra="[AgPB] Test_Bot: arrived at waypoint #4 (2 hop(s) walked)"),
           frame(13, pos="130,0,0", attacks=1, destroyed=True, events=events), frame(13.5, pos="130,0,0", attacks=1, destroyed=True, events=events), frame(14, pos="130,0,0", attacks=1, destroyed=True, events=events)]
    return dict(kind="gun", binding={key:META[key] for key in HASHES}, goal=4, target_refs=[-1001], target=dict(index=4,x=130,y=0,z=0,radius=4), stages=dict(before=[before],run=run))


def interrupt_case(death=False):
    before = frame(10)
    goto = framed("[AgPB] Test_Bot: walking to #4, 2 hop(s): 3 4", "agpb_bot_goto 0 4")
    events = event_rows(False)
    run = [goto, frame(11, active=True, breaking=True), frame(11.5, active=True, breaking=True, attacks=1, events=events)]
    trigger = framed("", "agpb_reg_lifecycle_kill 2 1" if death else "agpb_bot_stop 0")
    quiet = [trigger] + [frame(t, attacks=1, events=events, dead=death, extra=lifecycle_event("player_death", 11.8, 2, alive=0) if death else "") for t in (12,12.5,13)]
    return dict(kind="death_interrupt" if death else "stop_interrupt", binding={key:META[key] for key in HASHES}, goal=4, target_refs=[-1001], stages=dict(before=[before],run=run,quiet=quiet))


def bounded_case(timeout=True, fire=True):
    before=frame(10)
    goto=framed("[AgPB] Test_Bot: walking to #4, 2 hop(s): 3 4", "agpb_bot_goto 0 4")
    fire_event=event_rows(False)[0]
    count=(1 if timeout else 6) if fire else 0
    events=[probe_change(fire_event,dict(seq=i+1,tick=1120+i*40,t=f"{11.2+i*.4:.3f}")) for i in range(count)]
    run=[goto,frame(11,active=True,breaking=True),frame(20.5 if timeout else 13.5,active=True,breaking=True,attacks=count,events=events)]
    reason="10 second timeout" if timeout else "six attacks without confirmed damage/destruction"
    trigger=framed(f"[AgPB] Test_Bot: obstacle break failed: {reason} (ent=713, shots={count}, goal=#4) - stopped")
    quiet=[trigger]+[frame(t,attacks=count,events=events) for t in ((21.1,21.6,22.1) if timeout else (14,14.5,15))]
    for receipt in run+quiet:
        receipt["log"]="\n".join(probe_change(line,{"health":10}) if line.startswith("[WP_CLEARANCE_TARGET]") else line for line in receipt["log"].splitlines())+"\n"
    return dict(kind="bounded_exit",expected_exit="timeout" if timeout else "no_damage_budget",binding={key:META[key] for key in HASHES},goal=4,target_refs=[-1001],stages=dict(before=[before],run=run,quiet=quiet))


def fixture_case():
    item=interrupt_case()
    common="arm=7 mode=stop slot=2 userid=102 serial=258 human_slot=1 human_userid=101 human_serial=1 core_idx=0"
    rows=["[AGPB_REG_CLEARANCE] v=1 action=armed "+common+" t=10.500 tick=1050 deadline=20.500 timeout=10.000 trigger=weapon_fire dispatch=RequestFrame",
          "[AGPB_REG_CLEARANCE] v=1 action=triggered "+common+" t=11.200 tick=1120 arm_tick=1050 event=weapon_fire weapon=usp",
          "[AGPB_REG_CLEARANCE] v=1 action=before "+common+" t=11.500 tick=1150 arm_tick=1050 trigger_tick=1120 weapon=usp",
          "[AGPB_REG_CLEARANCE] v=1 action=done "+common+" t=11.500 tick=1150 arm_tick=1050 trigger_tick=1120 snapshot_tick=1150 alive=1 weapon=usp"]
    receipt=item["stages"]["run"][-1]
    lines=receipt["log"].splitlines()
    quiet=item["stages"]["quiet"].pop(1)
    receipt["log"]="\n".join([lines[0],*rows[:3],*lines[1:-1],rows[3],*quiet["log"].splitlines()[1:-1],lines[-1]])+"\n"
    item["stages"]["quiet"][0]["command"]="agpb_bot_info 0"
    return item


def hitscan_fixture_case():
    item=interrupt_case()
    events=event_rows()
    common="arm=7 mode=stop slot=2 userid=102 serial=258 human_slot=1 human_userid=101 human_serial=1 core_idx=0"
    rows=["[AGPB_REG_CLEARANCE] v=1 action=armed "+common+" t=10.500 tick=1050 deadline=20.500 timeout=10.000 trigger=weapon_fire dispatch=RequestFrame",
          "[AGPB_REG_CLEARANCE] v=1 action=triggered "+common+" t=11.200 tick=1120 arm_tick=1050 event=weapon_fire weapon=usp",
          "[AGPB_REG_CLEARANCE] v=1 action=before "+common+" t=11.210 tick=1121 arm_tick=1050 trigger_tick=1120 weapon=usp",
          "[AGPB_REG_CLEARANCE] v=1 action=done "+common+" t=11.210 tick=1121 arm_tick=1050 trigger_tick=1120 snapshot_tick=1121 alive=1 weapon=usp"]
    before=frame(11.21,active=True,breaking=True,attacks=1,destroyed=True,events=events)
    fresh=frame(11.21,attacks=1,destroyed=True,events=events)
    # The pre-action last command is the previous tick's actual attack. A new
    # zero command may be submitted later in the same engine tick.
    before["log"]="\n".join(line.replace("cmdtick=1121", "cmdtick=1120").replace("buttons=0x0", "buttons=0x1").replace("release=0", "release=1") if line.startswith(("[AgPB lifecycle]", "[AgPB clearance]")) else line for line in before["log"].splitlines())+"\n"
    fresh["log"]=fresh["log"].replace("cmdnum=1121", "cmdnum=1122")
    lines=before["log"].splitlines()
    before["log"]="\n".join([lines[0],*rows[:3],*lines[1:-1],rows[3],*fresh["log"].splitlines()[1:-1],lines[-1]])+"\n"
    item["stages"]["run"][-1]=before
    item["stages"]["quiet"]=[frame(t,attacks=1,destroyed=True,events=events) for t in (12,12.5)]
    return item


class ClearanceEvidenceTests(unittest.TestCase):
    def verdict(self, item, status="passed", error=None):
        result=evaluate_case(item,META)
        self.assertEqual(status,result["status"],result)
        if error:
            self.assertIn(error,result["error"])
        return result

    def test_timeout_and_six_real_no_damage_attacks_leave_quiet_clean_state(self):
        self.verdict(bounded_case())
        self.verdict(bounded_case(False))

    def test_claimed_attack_budget_without_six_real_fires_and_late_exit_refuse(self):
        item=bounded_case(False)
        for receipt in item["stages"]["run"]+item["stages"]["quiet"]:
            receipt["log"]=receipt["log"].replace("event=weapon_fire ","event=damage_post ")
        self.verdict(item,"failed")
        item=bounded_case()
        # A source timestamp cannot be moved beyond the budget just by claiming
        # timeout in the human-readable failure line.
        for receipt in item["stages"]["run"]:
            receipt["log"]=receipt["log"].replace("acquired=11 ","acquired=1 ").replace("deadline=21 ","deadline=11 ")
        self.verdict(item,"failed","budget")

    def test_knife_unknown_damageinfo_weapon_uses_real_active_smack_context(self):
        item=gun_case();item["kind"]="knife"
        for stage in item["stages"].values():
            for receipt in stage:
                receipt["log"]=receipt["log"].replace("weapon_usp","weapon_knife").replace("event_weapon=usp", "event_weapon=knife")
                receipt["log"]="\n".join(probe_change(line,dict(weapon_ref=-1,weapon_class="unknown",smack_known=1,smack_time=11.19)) if line.startswith("[WP_CLEARANCE_EVENT]") else line for line in receipt["log"].splitlines())+"\n"
        self.verdict(item)
        for receipt in item["stages"]["run"]:
            receipt["log"]=receipt["log"].replace("smack_known=1 smack_time=11.19", "smack_known=0 smack_time=-1")
        self.verdict(item,"failed","Smack")

    def test_existing_agpw_snapshot_does_not_claim_new_manual_authorship(self):
        source=dict(source_kind="existing_agpw_readonly_observation",manual_authorship=False,map=META["map"],source_sha256=META["agpw_sha256"],nodes=[dict(index=0,x=1,y=2,z=3,radius=4)])
        data=json.dumps(source).encode()
        meta=dict(META,waypoint_snapshot_sha256=hashlib.sha256(data).hexdigest())
        self.assertEqual(snapshot_target(data,meta,0)["x"],1)
        source["manual_authorship"]=True
        altered=json.dumps(source).encode()
        meta["waypoint_snapshot_sha256"]=hashlib.sha256(altered).hexdigest()
        with self.assertRaisesRegex(ValueError,"existing-AGPW"):
            snapshot_target(altered,meta,0)

    def test_deleted_in_damage_uses_matching_pre_context_and_real_break_health(self):
        post=parse(EVENT)["clearance_events"][0]
        destroy=dict(post,event="destroy",attribution="destruction_damage_context",damage_known=0,damage=-1,health_after_known=0)
        broken=dict(post,event="break_prop",attribution="game_event",damage_known=0,health_before_known=0,health_after=-5)
        self.assertTrue(applied(destroy,[broken,destroy]))
        self.assertFalse(applied(destroy,[destroy]))
        for key,value in (("damage_id",2),("ref",-1002),("health_after",10),("attacker_userid",999)):
            self.assertFalse(applied(destroy,[dict(broken,**{key:value}),destroy]))

    def test_fire_trigger_requires_real_cross_tick_action_and_keeps_both_original_blocks(self):
        item=fixture_case()
        result=self.verdict(item)
        self.assertEqual(result["samples"]["run"],2)
        self.assertEqual(result["samples"]["quiet"],3)
        item=fixture_case()
        receipt=item["stages"]["run"][-1]
        receipt["log"]=receipt["log"].replace("t=11.500 tick=1150 arm_tick=1050 trigger_tick=1120", "t=11.200 tick=1120 arm_tick=1050 trigger_tick=1120").replace("snapshot_tick=1150", "snapshot_tick=1120")
        self.verdict(item,"failed","after the original fire tick")

    def test_armed_or_done_fixture_promises_do_not_replace_real_active_before_state(self):
        item=fixture_case()
        receipt=item["stages"]["run"][-1]
        receipt["log"]="\n".join(line for line in receipt["log"].splitlines() if "action=done " not in line)+"\n"
        self.verdict(item,"failed","completed fire-trigger")
        item=fixture_case()
        receipt=item["stages"]["run"][-1]
        receipt["log"]="\n".join(line.replace("exists=1", "exists=0") if line.startswith("[WP_CLEARANCE_TARGET]") else line for line in receipt["log"].splitlines())+"\n"
        self.verdict(item,"failed","surviving target")

    def test_case_identity_can_follow_scene_but_cannot_override_artifact_hashes(self):
        item=gun_case();item["identity"]=dict(bot_userid=402)
        for stage in item["stages"].values():
            for receipt in stage:
                receipt["log"]=receipt["log"].replace("userid=102", "userid=402")
        self.verdict(item)
        item["identity"]["dll_sha256"]="b"*64
        self.verdict(item,"failed","identity override")

    def test_deleted_hitscan_target_still_has_a_real_attack_command_to_release(self):
        item=hitscan_fixture_case()
        self.verdict(item)
        receipt=item["stages"]["run"][-1]
        receipt["log"]=receipt["log"].replace("buttons=0x1", "buttons=0x0")
        self.verdict(item,"failed","awaiting release")

    def test_timeout_can_release_an_acquired_target_without_an_attack_opportunity(self):
        item=bounded_case(fire=False)
        result=self.verdict(item)
        self.assertEqual(result["submitted_attacks"],0)
        # The timeout must still have an actual active break/task acquisition.
        for receipt in item["stages"]["run"]:
            receipt["log"]=receipt["log"].replace("breaktoken=7", "breaktoken=0")
        self.verdict(item,"failed")

    def test_six_attack_budget_cannot_treat_unknown_health_as_no_damage(self):
        item=bounded_case(False)
        for receipt in item["stages"]["run"]+item["stages"]["quiet"]:
            receipt["log"]="\n".join(probe_change(line,dict(health_known=0,health=-1)) if line.startswith("[WP_CLEARANCE_TARGET]") else line for line in receipt["log"].splitlines())+"\n"
        self.verdict(item,"failed","known unchanged health")

    def test_diagnostic_cooldown_is_single_idle_setup_and_has_independent_readback(self):
        setup=frame(9.5)
        after=frame(9.5)
        def cooldown(log):
            return "\n".join(change(line,dict(playerattack=1000000)) if line.startswith("[AgPB clearance]") else change(line,dict(player_attack=1000000)) if line.startswith("[WP_CLEARANCE_WEAPON]") else line for line in log.splitlines())+"\n"
        lines=setup["log"].splitlines()
        setup["log"]="\n".join([*lines[:-1],*cooldown(after["log"]).splitlines()[1:-1],lines[-1]])+"\n"
        setup["command"]="agpb_bot_info 0; agpb_netwrite 0 m_flNextAttack 1000000; agpb_bot_info 0"
        item=bounded_case(fire=False)
        item["setup"]=dict(controlled_player_cooldown=1000000,command_receipts=[setup])
        for stage in item["stages"].values():
            for receipt in stage:
                receipt["log"]=cooldown(receipt["log"])
        result=self.verdict(item)
        self.assertEqual(result["scenario_scope"],"diagnostic_player_attack_cooldown_timeout")
        item["stages"]["run"][-1]["command"] += "; agpb_netwrite 0 m_flNextAttack 1000000"
        self.verdict(item,"failed","one-time diagnostic setup")

    def test_timeout_exit_uses_first_real_clear_state_even_inside_run_receipts(self):
        item=bounded_case()
        item["stages"]["run"] += item["stages"]["quiet"][:3]
        item["stages"]["quiet"] = item["stages"]["quiet"][3:]
        result=self.verdict(item)
        self.assertEqual(result["budget"]["first_clear_time"],21.1)
        self.assertEqual(result["samples"]["quiet"],3)

    def test_original_bot_real_damage_destruction_and_same_route_arrival(self):
        result=self.verdict(gun_case())
        self.assertTrue(result["endpoint"]["precise_stop_passed"])
        self.assertEqual(result["applied_damage_events"],1)

    def test_stop_and_death_interrupt_real_active_attacks(self):
        self.verdict(interrupt_case())
        self.verdict(interrupt_case(True))

    def test_damage_parameter_alone_and_unrelated_destroy_do_not_prove_applied_damage(self):
        event=parse(EVENT)["clearance_events"][0]
        event.update(health_before_known=0,health_after_known=0)
        self.assertFalse(applied(event,[]))
        destroy=dict(event,event="destroy",ref=-1002)
        self.assertFalse(applied(event,[destroy]))
        event.update(health_before_known=1,health_before=10,health_after_known=1,health_after=10)
        self.assertFalse(applied(event,[]))

    def test_unknown_fields_and_quoted_model_are_handled_strictly(self):
        self.assertEqual(parse(TARGET)["targets"][0]["model"],"models/office radio.mdl")
        with self.assertRaisesRegex(ValueError,"missing/unknown"):
            parse(CLEARANCE+" invented=1")
        with self.assertRaisesRegex(ValueError,"version"):
            parse(CLEARANCE.replace("v=1 ","v=2 "))
        with self.assertRaisesRegex(ValueError,"event/attribution"):
            parse(EVENT.replace("event=damage_post", "event=made_up"))

    def test_foreign_damage_actor_or_reused_entity_identity_refuses(self):
        item=gun_case()
        for receipt in item["stages"]["run"]:
            receipt["log"]=receipt["log"].replace("attacker_userid=102","attacker_userid=999")
        self.verdict(item,"failed","applied damage")
        item=gun_case()
        item["stages"]["run"][2]["log"]="\n".join(line.replace("generation=1 ent=713","generation=2 ent=713") if line.startswith("[WP_CLEARANCE_TARGET]") else line for line in item["stages"]["run"][2]["log"].splitlines())+"\n"
        self.verdict(item,"failed","identity")

    def test_replayed_old_events_do_not_certify_new_case_and_counter_only_is_insufficient(self):
        item=gun_case()
        item["stages"]["before"][0]["log"]=item["stages"]["before"][0]["log"].replace("seq=0 damage_calls=", "seq=4 damage_calls=")
        self.verdict(item,"failed","counter")
        item=gun_case()
        for receipt in item["stages"]["run"]:
            receipt["log"]="\n".join(line for line in receipt["log"].splitlines() if "[WP_CLEARANCE_EVENT]" not in line)+"\n"
        self.verdict(item,"failed","truncated")

    def test_route_replacement_and_analog_only_movement_cannot_certify_clearance(self):
        item=gun_case()
        item["stages"]["run"][2]["command"]="agpb_bot_goto 0 4; agpb_bot_info 0"
        self.verdict(item,"failed","route replacement")
        item=gun_case()
        for receipt in item["stages"]["run"]:
            receipt["log"]=receipt["log"].replace("attackcmds=1","attackcmds=0")
        self.verdict(item,"failed","IN_ATTACK")

    def test_clear_after_attack_finished_does_not_prove_mid_attack_interruption(self):
        item=interrupt_case()
        item["stages"]["run"][-1]["log"]=item["stages"]["run"][-1]["log"].replace("ent=713 route=", "ent=-1 route=").replace("breaktoken=7 acquired=", "breaktoken=0 acquired=")
        self.verdict(item,"failed")

    def test_post_stop_attacks_or_stale_use_handle_refuse(self):
        item=interrupt_case()
        item["stages"]["quiet"][-1]["log"]=item["stages"]["quiet"][-1]["log"].replace("attackcmds=1","attackcmds=2")
        self.verdict(item,"failed","continued after exit")
        item=interrupt_case()
        item["stages"]["quiet"][1]["log"]=item["stages"]["quiet"][1]["log"].replace("use=-1", "use=123")
        self.verdict(item,"failed","state behind")

    def test_missing_precise_endpoint_or_second_target_cannot_pass(self):
        item=gun_case()
        item["target"]["z"]=10
        self.verdict(item,"failed","endpoint")
        item=gun_case();item["kind"]="chain"
        self.verdict(item,"failed","two original")

    def test_version_mismatch_and_missing_kinds_are_not_silently_accepted(self):
        item=gun_case();item["binding"]["dll_sha256"]="b"*64
        self.verdict(item,"failed","artifact")
        report=evaluate(dict(schema_version=1,metadata=META,cases=[gun_case()]))
        self.assertEqual(report["status"],"incomplete")
        self.assertIn("bounded_exit",report["pending_kinds"])


if __name__ == "__main__":
    unittest.main()
