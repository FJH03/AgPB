"""Meaningful offline combat-evidence regressions; never controls a game."""
import copy
import itertools
import re
import unittest

from verify_combat import HASHES, LOCOMOTION_CONTRACT, LOCOMOTION_KIND, evaluate, evaluate_case, parse
from test_verify_lifecycle import CORE, CLEAR, change

NONCE = itertools.count(1)
EPOCH = "2000-abcd1234-1234abcd"
TARGET = dict(slot=3, userid=103, serial=259)


def receipt(text, command="agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat"):
    n = next(NONCE)
    begin, end = f"AGPB_SESSION_BEGIN_C{n}", f"AGPB_SESSION_END_C{n}"
    return dict(returncode=0, command=command, log=begin+"\n"+text+"\n"+end+"\n",
                ack=dict(beginMarker=begin, endMarker=end, logResetCount=0, endByteOffset=n*100000))


def metadata():
    value = dict(map="de_dust2", subject_slot=2, subject_userid=102, subject_name="Test_Bot", human_slot=1, human_userid=101,
                 controlled_enemy_authorized=True, guard_expected_crc="770BAE64")
    value.update({key: "a"*64 for key in HASHES})
    value["prelude"] = [receipt("[AgPB Guard] active=1 intact=1 null-bot=0 null-profile=0 native-path=0 freeze-ends=2", "agpb_guard_status")]
    return value


def encode(values):
    def raw(value):
        return "("+" ".join(str(v) for v in value)+")" if isinstance(value, list) else str(value)
    return " ".join(key+"="+raw(value) for key, value in values.items())


def row(kind, t, seq=None, **changes):
    tick = round(t*100)
    values = dict(v=1, epoch=EPOCH, map_serial=1, tick=tick, t=t, map="de_dust2", seq=seq or tick*100+1,
                  sample=tick, generation=1, replay=0)
    values.update(changes)
    return "[WP_COMBAT_"+kind.upper()+"] "+encode(values)


def events(friendly=False, victim_serial=259, health=70):
    common = dict(actor_slot=2, actor_userid=102, actor_serial=258, actor_fake=1, actor_team=2, actor_alive=1,
                  victim_slot=3, victim_userid=103, victim_serial=victim_serial, victim_fake=1,
                  victim_team=2 if friendly else 3, victim_alive=1, weapon="usp", active_ref=-2001,
                  damage_id=1, dmg_health=30, dmg_armor=0, health=health, armor=0, hitgroup=2, shots=-1, headshot=-1, inflictor=-1)
    return [row("event", 11.2, seq=112001, event="weapon_fire", **dict(common, victim_slot=-1, victim_userid=0, victim_serial=-1, victim_team=-1, health=-1)),
            row("event", 11.2, seq=112002, event="player_hurt", **common)]


def frame(t, *, enemy=True, owned=False, attacks=0, health=100, pos=100, stopped=False, settled=False, retained=(), extra="", command=None,
          position=None, movement=None, velocity=None, view=None):
    tick = round(t*100)
    core_changes = dict(tick=tick, time=f"{t:.3f}", cmdnum=tick, cmdtick=tick,
                        pos=f"({pos},0,0)", vel="(0,0,0)" if owned or stopped or settled else "(10,0,0)",
                        move="(0,0,0)" if owned else "(100,0,0)", buttons="0x1" if owned and attacks else "0x0")
    position = position if position is not None else [pos, 0, 0]
    core_changes["pos"] = "(" + ",".join(map(str, position)) + ")"
    for key, value in (("move", movement), ("vel", velocity), ("view", view)):
        if value is not None:
            core_changes[key] = "(" + ",".join(map(str, value)) + ")"
    if stopped or settled:
        core_changes.update(CLEAR)
        core_changes.update(tick=tick, time=f"{t:.3f}", cmdnum=tick, cmdtick=tick,
                            pos="(" + ",".join(map(str, position)) + ")", reset="stop" if stopped else "arrived")
    core = change(CORE, core_changes)
    route = 0 if stopped or settled else 2
    combat = dict(v=1, enabled=1, global_=1, state="stopped" if stopped else "aiming" if owned else "scanning",
                  enemy="0x1234" if owned else "0xFFFFFFFF", ent=3 if owned else -1, userid=103 if owned else 0,
                  enemyhp=health if owned else -1, weapon="0x5678", clip=12-attacks, aimerror=.2,
                  attackcmds=attacks, shots=attacks, release=0, task="normal" if not route else "move_to_position",
                  token=1 if not route else 5, owner=5 if owned else 0, routeowner=5 if route else 0,
                  goal=4 if route else -1, hop=0, node=3 if route else -1)
    combat["global"] = combat.pop("global_")
    parts = [core, "[AgPB] Test_Bot: combat "+encode(combat)]
    scene = dict(watching=1, subject_slot=2, subject_userid=102, subject_serial=258, subject_present=1,
                 human_slot=1, human_userid=101, human_serial=1, human_present=1,
                 target_slot=3, target_userid=103, target_serial=259, target_present=1,
                 subject_human_ok=1, human_slot_lower=1, opposing_fake_count=int(enemy), unbound_opposing_count=0,
                 target_opposing=int(enemy), scene_binding_ok=1, guard_verified=-1, external_authorization_verified=-1)
    parts.append(row("scene",t,**scene))
    for slot, userid, serial, fake, team, hp, x, fires in ((1,101,1,0,2,100,-100,0),(2,102,258,1,2,100,pos,attacks),(3,103,259,1,3 if enemy else 2,health,200,0)):
        parts.append(row("player",t,seq=tick*100+slot+1,slot=slot,userid=userid,serial=serial,fake=fake,team=team,alive=1,
                         health_known=1,health=hp,state_known=1,player_state=0,life_state=0,pos=position if slot==2 else [x,0,0],fires=fires,
                         hurts_dealt=attacks if slot==2 else 0,hurts_taken=attacks if slot==3 else 0))
        parts.append(row("weapon",t,seq=tick*100+slot+11,slot=slot,userid=userid,serial=serial,active_ref=-2001-slot,
                         active_ent=114+slot,**{"class":"weapon_usp"},clip_known=1,clip=12-attacks if slot==2 else 12,
                         reserve_known=1,reserve=100,primary_known=1,primary=0,player_attack_known=1,player_attack=0,reload_known=1,reload=0))
    parts.append(row("trace",t,seq=tick*100+30,shooter_slot=2,shooter_userid=102,shooter_serial=258,
                     target_slot=3,target_userid=103,target_serial=259,mask=1174421507,endpoint="eye",did_hit=1,
                     hit_ent=0 if enemy else 3,unobstructed=0 if enemy else 1))
    parts.append(row("counts",t,seq=tick*100+99,weapon_fire_hooked=1,player_hurt_hooked=1,player_death_hooked=1,damage_overflows=0))
    parts.extend(retained)
    if extra: parts.append(extra)
    return receipt("\n".join(parts), command or "agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat")


def make_case(kind):
    meta = metadata()
    enemy = kind != "friendly_rejection"
    item = dict(kind=kind, binding={key:meta[key] for key in HASHES}, target_identity=TARGET.copy())
    before = frame(10,enemy=enemy,command="agpb_bot_goto 0 4; agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat")
    if kind.endswith("rejection"):
        stages = dict(before=[before],run=[frame(t,enemy=enemy) for t in (11,11.5,12)])
    else:
        shots = events()
        run = [frame(11,owned=True),frame(11.5,owned=True,attacks=1,health=70,retained=shots)]
        stages = dict(before=[before],run=run)
        if kind == "visible_damage_resume":
            stages["resumed"] = [frame(12,attacks=1,health=70,pos=110,retained=shots,
                extra="[AgPB] Test_Bot: combat released ent=3; route goal=#4 hop=0 owner=5"),
                frame(12.5,attacks=1,health=70,pos=120,retained=shots),
                *[frame(t,attacks=1,health=70,pos=130,settled=True,retained=shots) for t in (13,13.5,14,14.5)]]
            item.update(goal=4,target=dict(index=4,x=130,y=0,z=0,radius=4))
        else:
            stages["quiet"] = [frame(12,stopped=True,attacks=1,health=70,retained=shots,command="agpb_bot_stop 0; agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat"),
                               frame(12.5,stopped=True,attacks=1,health=70,retained=shots),frame(13,stopped=True,attacks=1,health=70,retained=shots)]
    item["stages"] = stages
    return meta,item


def edit_log(receipt_, tag, changes):
    lines=[]
    for line in receipt_["log"].splitlines():
        lines.append(change(line,changes) if tag in line else line)
    receipt_["log"]="\n".join(lines)+"\n"


def locomotion_case(motion="approach"):
    meta = metadata()
    meta["acceptance_contract"] = LOCOMOTION_CONTRACT
    item = dict(kind=LOCOMOTION_KIND, locomotion_policy="sypb_ground_v1", goal=4,
                binding={key:meta[key] for key in HASHES}, target_identity=TARGET.copy(),
                locomotion=dict(expected_motion=motion, weapon_class="weapon_usp",
                                distance_range=[20, 300], movement_context="ground_walk"))
    move = {"approach":[100,0,0], "retreat":[-100,0,0],
            "strafe":[0,-100,0], "precision_stop":[0,0,0]}[motion]
    def combat(t, step, attacks=1, retained=()):
        position = ([100 + step*10,0,0] if motion == "approach" else
                    [100 - step*10,0,0] if motion == "retreat" else
                    [100,step*10,0] if motion == "strafe" else [100,0,0])
        return frame(t, owned=True, attacks=attacks, health=100-30*attacks,
                     position=position, movement=move, view=[0,0],
                     velocity=[0,0,0] if motion == "precision_stop" else [move[0]/5,-move[1]/5,0], retained=retained)
    shots = events()
    stages = dict(before=[frame(10,command="agpb_bot_goto 0 4; agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat")],
                  run=[combat(11,0,0),combat(11.5,1,retained=shots),combat(12,2,retained=shots)],
                  resumed=[frame(12.5,attacks=1,health=70,pos=125,retained=shots,
                                 extra="[AgPB] Test_Bot: combat released ent=3; route goal=#4 hop=0 owner=5"),
                           frame(13,attacks=1,health=70,pos=130,retained=shots)],
                  reengaged=[combat(t,i+3,retained=shots) for i,t in enumerate((13.5,14,14.5))],
                  quiet=[frame(t,stopped=True,attacks=1,health=70,retained=shots,
                               command="agpb_bot_stop 0; agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat" if t==15 else None)
                         for t in (15,15.5,16)])
    if motion == "precision_stop":
        second = [change(line, {"tick":1370, "t":13.7, "seq":137001+i})
                  for i,line in enumerate(events(health=40))]
        stages["reengaged"] = [combat(13.5,3,retained=shots),
                               combat(14,4,attacks=2,retained=shots+second),
                               combat(14.5,5,attacks=2,retained=shots+second)]
        stages["quiet"] = [frame(t,stopped=True,attacks=2,health=40,retained=shots+second,
                                 command="agpb_bot_stop 0; agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat" if t==15 else None)
                           for t in (15,15.5,16)]
    item["stages"] = stages
    return meta,item


class CombatEvidenceTests(unittest.TestCase):
    def verdict(self, meta, item, expected="passed", message=None):
        result=evaluate_case(item,meta)
        self.assertEqual(result["status"],expected,result)
        if message: self.assertIn(message,result.get("error",""))
        return result

    def test_four_real_evidence_shapes_and_partial_report(self):
        cases=[]
        first_meta=None
        for kind in ("friendly_rejection","occluded_rejection","visible_damage_resume","stop_interrupt"):
            meta,item=make_case(kind)
            first_meta=first_meta or meta
            self.verdict(meta,item)
            cases.append(item)
        self.assertEqual(evaluate(dict(schema_version=1,metadata=first_meta,cases=cases))["status"],"passed")
        report=evaluate(dict(schema_version=1,metadata=first_meta,cases=cases[:1]))
        self.assertEqual(report["status"],"incomplete")
        self.assertIn("visible_damage_resume",report["pending_kinds"])

    def test_claimed_pass_and_attack_commands_cannot_replace_real_fire(self):
        meta,item=make_case("visible_damage_resume");item["passed"]=True
        for stage in item["stages"].values():
            for rec in stage:
                rec["log"]="\n".join(line for line in rec["log"].splitlines() if "event=weapon_fire " not in line)+"\n"
        self.verdict(meta,item,"pending","Fire counter")

    def test_amount_without_measured_health_change_cannot_pass(self):
        meta,item=make_case("visible_damage_resume")
        for stage in item["stages"].values():
            for rec in stage: edit_log(rec,"[WP_COMBAT_PLAYER]",{"health":100})
        self.verdict(meta,item,"pending","health decrease")

    def test_wrong_target_serial_and_changed_replays_cannot_pass(self):
        meta,item=make_case("visible_damage_resume")
        for stage in item["stages"].values():
            for rec in stage: edit_log(rec,"event=player_hurt ",{"victim_serial":999})
        self.verdict(meta,item,"pending","health decrease")
        meta,item=make_case("visible_damage_resume")
        edit_log(item["stages"]["resumed"][0],"event=player_hurt ",{"health":60})
        self.verdict(meta,item,"pending","Replay sequence")

    def test_friendly_damage_and_hidden_ammunition_loss_rejected(self):
        meta,item=make_case("friendly_rejection")
        rec=item["stages"]["run"][-1]
        rec["log"]=rec["log"].replace(rec["ack"]["endMarker"],"\n".join(events(friendly=True))+"\n"+rec["ack"]["endMarker"])
        edit_log(rec,"[WP_COMBAT_PLAYER]",{"fires":1,"hurts_dealt":1})
        self.verdict(meta,item,"pending","same-team")
        meta,item=make_case("friendly_rejection")
        edit_log(item["stages"]["run"][1],"[WP_COMBAT_WEAPON]",{"reserve":99})
        self.verdict(meta,item,"pending","ammunition")

    def test_rejection_needs_complete_increasing_window_and_blocked_eye_trace(self):
        meta,item=make_case("occluded_rejection")
        edit_log(item["stages"]["run"][1],"[WP_COMBAT_TRACE]",{"unobstructed":1})
        self.verdict(meta,item,"pending","blocked eye")
        meta,item=make_case("friendly_rejection");item["stages"]["run"].pop()
        self.verdict(meta,item,"pending","Too few")
        meta,item=make_case("friendly_rejection");item["stages"]["run"][1]=copy.deepcopy(item["stages"]["run"][0])
        self.verdict(meta,item,"pending","reused")

    def test_route_preservation_resumption_and_strict_endpoint_are_independent(self):
        meta,item=make_case("visible_damage_resume")
        edit_log(item["stages"]["run"][1],"[AgPB lifecycle]",{"index":1})
        edit_log(item["stages"]["run"][1],"[AgPB] Test_Bot: combat",{"hop":1})
        self.verdict(meta,item,"pending","owner/goal/hop")
        meta,item=make_case("visible_damage_resume")
        item["stages"]["resumed"][0]["command"]+="; agpb_bot_goto 0 4"
        self.verdict(meta,item,"pending","replacement goto")
        meta,item=make_case("visible_damage_resume");item["target"]["z"]=10
        self.verdict(meta,item,"pending","strict endpoint")

    def test_stop_needs_native_stop_prior_shot_and_continuous_released_commands(self):
        meta,item=make_case("stop_interrupt")
        item["stages"]["quiet"][0]["command"]="agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat"
        self.verdict(meta,item,"pending","explicit native stop")
        meta,item=make_case("stop_interrupt")
        edit_log(item["stages"]["quiet"][1],"[AgPB lifecycle]",{"move":"(10,0,0)"})
        self.verdict(meta,item,"pending","release actual")

    def test_guard_authorization_hash_and_immutable_observed_receipts(self):
        meta,item=make_case("occluded_rejection");meta["controlled_enemy_authorized"]=False
        self.verdict(meta,item,"pending","authorization")
        meta,item=make_case("occluded_rejection")
        edit_log(meta["prelude"][0],"[AgPB Guard]",{"intact":0})
        self.verdict(meta,item,"pending","active and intact")
        meta,item=make_case("friendly_rejection");item["binding"]["dll_sha256"]="b"*64
        self.verdict(meta,item,"pending","artifact version")
        meta,item=make_case("friendly_rejection")
        item["stages"]["run"][1]["observed_log"]="edited"
        self.verdict(meta,item,"pending","unaltered ACK")

    def test_idle_or_dead_subject_cannot_certify_rejection(self):
        meta,item=make_case("friendly_rejection")
        edit_log(item["stages"]["run"][1],"[AgPB] Test_Bot: combat",{"state":"stopped"})
        self.verdict(meta,item,"pending","scanning intent")
        meta,item=make_case("friendly_rejection")
        edit_log(item["stages"]["run"][1],"[WP_COMBAT_WEAPON]",{"player_attack":999})
        self.verdict(meta,item,"pending","ready firearm")

    def test_events_outside_ack_are_preserved_without_editing_original_frame(self):
        meta,item=make_case("visible_damage_resume")
        rec=item["stages"]["run"][0]
        rec["observed_log"]=rec["log"]+"\n".join(events())+"\n"
        rec["observed_start_offset"]=rec["ack"]["endByteOffset"]-1000
        rec["observed_end_offset"]=rec["ack"]["endByteOffset"]+1000
        self.verdict(meta,item)
        rec["observed_start_offset"]=0
        self.verdict(meta,item,"pending","Observed offsets")
    def test_startup_marker_is_replayable_but_cannot_be_an_active_map(self):
        marker=row("event",1,seq=1,event="plugin_start",map_serial=0)
        self.assertEqual(parse(marker)["event"][0]["map_serial"],0)
        with self.assertRaisesRegex(ValueError,"map identity"):
            parse(change(marker,{"event":"player_hurt"}))

    def test_disabled_before_baseline_does_not_replace_enabled_rejection_window(self):
        meta,item=make_case("friendly_rejection")
        edit_log(item["stages"]["before"][0],"[AgPB] Test_Bot: combat",{"enabled":0})
        self.verdict(meta,item)
        edit_log(item["stages"]["run"][1],"[AgPB] Test_Bot: combat",{"enabled":0})
        self.verdict(meta,item,"pending","active acceptance")

    def test_natural_hop_progress_before_acquisition_is_not_a_combat_route_change(self):
        meta,item=make_case("visible_damage_resume")
        edit_log(item["stages"]["before"][0],"[AgPB lifecycle]",{"index":0,"node":2})
        edit_log(item["stages"]["before"][0],"[AgPB] Test_Bot: combat",{"hop":0,"node":2})
        prior=frame(10.5)
        item["stages"]["before"].append(prior)
        # This original receipt was created later in the test helper only;
        # rebuild offsets/markers in chronological order without editing logs.
        ordered=[r for stage in item["stages"].values() for r in stage]
        for index,r in enumerate(ordered):
            r["ack"]["endByteOffset"]=(index+next(NONCE))*100000
        self.verdict(meta,item)
    def test_real_shot_between_last_engaged_snapshot_and_first_resumed_snapshot(self):
        meta,item=make_case("visible_damage_resume")
        rec=item["stages"]["run"][-1]
        edit_log(rec,"[AgPB] Test_Bot: combat",{"attackcmds":0,"shots":0})
        edit_log(rec,"[WP_COMBAT_PLAYER]",{"fires":0,"hurts_dealt":0,"hurts_taken":0,"health":100})
        edit_log(rec,"[WP_COMBAT_WEAPON]",{"clip":12})
        rec["log"]="\n".join(line for line in rec["log"].splitlines() if not line.startswith("[WP_COMBAT_EVENT]"))+"\n"
        for later in item["stages"]["resumed"]:
            edit_log(later,"[WP_COMBAT_EVENT]",{"tick":1170,"t":11.7})
        late=[change(line,{"tick":1170,"t":11.7}) for line in events()]
        rec["observed_log"]=rec["log"]+"\n".join(late)+"\n"
        rec["observed_start_offset"]=rec["ack"]["endByteOffset"]-1000
        rec["observed_end_offset"]=rec["ack"]["endByteOffset"]+1000
        self.verdict(meta,item)
    def test_stop_only_original_ack_needs_no_fabricated_quiet_frame(self):
        meta,item=make_case("stop_interrupt")
        item["stages"]["quiet"][0]["command"]="agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat"
        control=receipt("[AgPB] Test_Bot: route stopped (roam off)","agpb_bot_stop 0")
        item["stages"]["quiet"].insert(0,control)
        # Synthetic test receipts get increasing file offsets in their assigned
        # order; the original stop ACK itself contains no snapshot or edits.
        for stage in item["stages"].values():
            for r in stage:r["ack"]["endByteOffset"]=next(NONCE)*100000
        result=self.verdict(meta,item)
        self.assertEqual(result["samples"]["quiet"],3)
        self.assertFalse(parse(control["log"])["core"])
    def test_source_skill_reaction_and_recoil_pause_still_own_original_route(self):
        for kind in ("visible_damage_resume","stop_interrupt"):
            meta,item=make_case(kind)
            edit_log(item["stages"]["run"][0],"[AgPB] Test_Bot: combat",{"state":"reacting"})
            edit_log(item["stages"]["run"][1],"[AgPB] Test_Bot: combat",{"state":"recoil-pause"})
            self.verdict(meta,item)
        meta,item=make_case("visible_damage_resume")
        edit_log(item["stages"]["run"][0],"[AgPB] Test_Bot: combat",{"state":"unknown-new-policy"})
        self.verdict(meta,item,"pending","combat ownership")
    def test_per_case_identity_override_does_not_override_assets(self):
        meta,item=make_case("friendly_rejection")
        item["identity"]=dict(subject_userid=202,subject_serial=258)
        for stage in item["stages"].values():
            for rec in stage:
                rec["log"]=rec["log"].replace("userid=102 ","userid=202 ").replace("subject_userid=102 ","subject_userid=202 ")
        self.verdict(meta,item)
        item["identity"]["dll_sha256"]="b"*64
        self.verdict(meta,item,"pending","Unknown per-case")

    def test_locomotion_contract_certifies_only_one_explicit_sample(self):
        for motion in ("approach", "retreat", "strafe", "precision_stop"):
            with self.subTest(motion=motion):
                meta,item=locomotion_case(motion)
                result=self.verdict(meta,item)
                self.assertEqual(result["locomotion"]["expected_motion"],motion)
                report=evaluate(dict(schema_version=1,metadata=meta,cases=[item]))
                self.assertEqual(report["status"],"passed",report)
                self.assertFalse(report["pending_kinds"])
                self.assertIn("sampled_ground",report["scope"])
        meta,item=locomotion_case()
        meta.pop("acceptance_contract")
        report=evaluate(dict(schema_version=1,metadata=meta,cases=[item]))
        self.assertEqual(report["status"],"incomplete")
        self.assertEqual(set(report["pending_kinds"]),set(("friendly_rejection","occluded_rejection","visible_damage_resume","stop_interrupt")))

    def test_legacy_stationary_assertion_is_preserved_and_policy_is_explicit(self):
        meta,item=make_case("visible_damage_resume")
        edit_log(item["stages"]["run"][0],"[AgPB lifecycle]",{"move":"(100,0,0)"})
        self.verdict(meta,item,"pending","suspend actual locomotion")
        meta,item=locomotion_case();item.pop("locomotion_policy")
        self.verdict(meta,item,"pending","locomotion policy")
        meta,item=locomotion_case();item["locomotion"].pop("distance_range")
        self.verdict(meta,item,"pending","scene boundaries")

    def test_locomotion_commands_without_real_displacement_and_wrong_direction_fail(self):
        meta,item=locomotion_case()
        for rec in item["stages"]["run"]:
            edit_log(rec,"[AgPB lifecycle]",{"pos":"(100,0,0)"})
            edit_log(rec,"slot=2 userid=102 serial=258 fake=1",{"pos":"(100,0,0)"})
        self.verdict(meta,item,"pending","actual displacement")
        meta,item=locomotion_case();item["locomotion"]["expected_motion"]="retreat"
        self.verdict(meta,item,"pending","actual displacement")

    def test_locomotion_displacement_without_commands_and_reversed_world_axis_fail(self):
        meta,item=locomotion_case()
        for rec in item["stages"]["run"]:edit_log(rec,"[AgPB lifecycle]",{"move":"(0,0,0)"})
        self.verdict(meta,item,"pending","actual displacement")
        meta,item=locomotion_case()
        for rec in item["stages"]["run"]:edit_log(rec,"[AgPB lifecycle]",{"view":"(0,180)"})
        self.verdict(meta,item,"pending","actual displacement")

    def test_locomotion_needs_grounded_fresh_window_and_actual_scene_boundaries(self):
        for field,value in (("airborne",1),("flags","0x300"),("roundfrozen",1),("move","(100,0,1)"),("buttons","0x2")):
            meta,item=locomotion_case()
            edit_log(item["stages"]["run"][1],"[AgPB lifecycle]",{field:value})
            self.verdict(meta,item,"pending","remain grounded")
        meta,item=locomotion_case();item["stages"]["run"].pop()
        self.verdict(meta,item,"pending","Too few")
        meta,item=locomotion_case();item["locomotion"]["distance_range"]=[0,50]
        self.verdict(meta,item,"pending","distance is outside")
        meta,item=locomotion_case();item["locomotion"]["weapon_class"]="weapon_awp"
        self.verdict(meta,item,"pending","weapon/target changed")

    def test_locomotion_real_damage_route_resume_and_combat_stop_remain_required(self):
        meta,item=locomotion_case()
        for stage in item["stages"].values():
            for rec in stage:edit_log(rec,"[WP_COMBAT_PLAYER]",{"health":100})
        self.verdict(meta,item,"pending","health decrease")
        meta,item=locomotion_case()
        edit_log(item["stages"]["run"][1],"[AgPB lifecycle]",{"index":1})
        edit_log(item["stages"]["run"][1],"[AgPB] Test_Bot: combat",{"hop":1})
        self.verdict(meta,item,"pending","owner/goal/hop")
        meta,item=locomotion_case();item["stages"]["resumed"][0]["command"]+="; agpb_bot_goto 0 4"
        self.verdict(meta,item,"pending","replacement goto")
        meta,item=locomotion_case()
        edit_log(item["stages"]["quiet"][1],"[AgPB lifecycle]",{"move":"(10,0,0)"})
        self.verdict(meta,item,"pending","release actual")

    def test_locomotion_reengagement_preserves_resumed_hop_and_moves_before_stop(self):
        meta,item=locomotion_case()
        for rec in item["stages"]["reengaged"]:
            edit_log(rec,"[AgPB lifecycle]",{"index":1})
            edit_log(rec,"[AgPB] Test_Bot: combat",{"hop":1})
        self.verdict(meta,item,"pending","owner/goal/hop")
        meta,item=locomotion_case()
        for rec in item["stages"]["reengaged"]:edit_log(rec,"[AgPB lifecycle]",{"move":"(0,0,0)"})
        self.verdict(meta,item,"pending","actual displacement")
        meta,item=locomotion_case();item["stages"]["quiet"][0]["command"]="agpb_bot_info 0; agpb_bot_combat_info 0; agpb_wp_probe_combat"
        self.verdict(meta,item,"pending","explicit native stop")

    def test_locomotion_rejects_external_position_change_and_precision_stop_drift(self):
        meta,item=locomotion_case();item["stages"]["run"][1]["command"]+="; setpos 110 0 0"
        self.verdict(meta,item,"pending","External movement")
        meta,item=locomotion_case("precision_stop")
        edit_log(item["stages"]["run"][1],"[AgPB lifecycle]",{"vel":"(10,0,0)"})
        self.verdict(meta,item,"pending","Precision-stop")

    def test_locomotion_contract_does_not_silently_accept_legacy_cases(self):
        meta,item=make_case("visible_damage_resume");meta["acceptance_contract"]=LOCOMOTION_CONTRACT
        with self.assertRaisesRegex(ValueError,"only its explicit"):
            evaluate(dict(schema_version=1,metadata=meta,cases=[item]))

    def test_locomotion_cannot_use_damage_from_later_reengagement(self):
        meta,item=locomotion_case()
        for stage in item["stages"].values():
            for rec in stage:
                edit_log(rec,"event=weapon_fire ",{"tick":1400,"t":14})
                edit_log(rec,"event=player_hurt ",{"tick":1400,"t":14})
        self.verdict(meta,item,"pending","Shot is not before")

    def test_locomotion_accepts_real_braking_and_posture_transitions_in_both_engagements(self):
        for stage in ("run", "reengaged"):
            for state in ("braking", "posture-settling"):
                with self.subTest(stage=stage, state=state):
                    meta,item=locomotion_case("precision_stop")
                    rec=item["stages"][stage][0]
                    edit_log(rec,"[AgPB] Test_Bot: combat",{"state":state})
                    edit_log(rec,"[AgPB lifecycle]",{"buttons":"0x0"})
                    self.verdict(meta,item)

    def test_legacy_owned_state_allowlist_is_unchanged(self):
        for kind in ("visible_damage_resume", "stop_interrupt"):
            for state in ("braking", "posture-settling"):
                with self.subTest(kind=kind, state=state):
                    meta,item=make_case(kind)
                    edit_log(item["stages"]["run"][0],"[AgPB] Test_Bot: combat",{"state":state})
                    self.verdict(meta,item,"pending","combat ownership")

    def test_locomotion_still_rejects_unknown_owned_states(self):
        for stage in ("run", "reengaged"):
            meta,item=locomotion_case("precision_stop")
            edit_log(item["stages"][stage][0],"[AgPB] Test_Bot: combat",{"state":"unknown-new-policy"})
            self.verdict(meta,item,"pending","combat ownership" if stage=="run" else "Reengagement")

    def test_transition_states_do_not_relax_motion_or_measured_damage(self):
        meta,item=locomotion_case("precision_stop")
        edit_log(item["stages"]["run"][0],"[AgPB] Test_Bot: combat",{"state":"braking"})
        edit_log(item["stages"]["run"][1],"[AgPB lifecycle]",{"move":"(10,0,0)"})
        self.verdict(meta,item,"pending","Precision-stop")
        meta,item=locomotion_case("precision_stop")
        edit_log(item["stages"]["run"][0],"[AgPB] Test_Bot: combat",{"state":"posture-settling"})
        for stage in item["stages"].values():
            for rec in stage:edit_log(rec,"[WP_COMBAT_PLAYER]",{"health":100})
        self.verdict(meta,item,"pending","health decrease")

    def test_precision_snapshot_aliasing_accepts_counter_with_native_fire_and_damage(self):
        meta,item=locomotion_case("precision_stop")
        for stage in ("run", "reengaged"):
            for rec in item["stages"][stage]:
                edit_log(rec,"[AgPB lifecycle]",{"buttons":"0x0"})
        result=self.verdict(meta,item)
        for key in ("locomotion", "reengaged_locomotion"):
            self.assertEqual(result[key]["attack_command_proof"],"in_window_counter_increase")
        for window in result["precision_fire_windows"].values():
            self.assertEqual(window["real_fire_events"],1)
            self.assertEqual(window["real_hurt_events"],1)

    def test_precision_stale_counter_cannot_replace_missed_attack_bit(self):
        for stage,attacks in (("run",0),("reengaged",1)):
            with self.subTest(stage=stage):
                meta,item=locomotion_case("precision_stop")
                for rec in item["stages"][stage]:
                    edit_log(rec,"[AgPB lifecycle]",{"buttons":"0x0"})
                    edit_log(rec,"[AgPB] Test_Bot: combat",{"attackcmds":attacks})
                self.verdict(meta,item,"pending","submitted primary attack")

    def test_precision_counter_needs_native_fire_inside_each_owned_window(self):
        for fire,hurt,t,tick in ((112001,112002,12.2,1220),(137001,137002,14.8,1480)):
            with self.subTest(fire=fire):
                meta,item=locomotion_case("precision_stop")
                for stage in item["stages"].values():
                    for rec in stage:
                        edit_log(rec,f"seq={fire} ",{"tick":tick,"t":t})
                        edit_log(rec,f"seq={hurt} ",{"tick":tick,"t":t})
                self.verdict(meta,item,"pending","window lacks matching native weapon_fire")

    def test_precision_cannot_borrow_damage_after_owned_last_tick(self):
        for fire,hurt,fire_t,fire_tick,hurt_t,hurt_tick in (
                (112001,112002,12,1200,12.2,1220),
                (137001,137002,14.5,1450,14.7,1470)):
            with self.subTest(hurt=hurt):
                meta,item=locomotion_case("precision_stop")
                for stage in item["stages"].values():
                    for rec in stage:
                        edit_log(rec,f"seq={fire} ",{"tick":fire_tick,"t":fire_t})
                        edit_log(rec,f"seq={hurt} ",{"tick":hurt_tick,"t":hurt_t})
                self.verdict(meta,item,"pending","window lacks matching native hurt")

    def test_precision_needs_independent_health_change_in_each_owned_window(self):
        for stage,health in (("run",100),("reengaged",70)):
            with self.subTest(stage=stage):
                meta,item=locomotion_case("precision_stop")
                for rec in item["stages"][stage]:
                    edit_log(rec,"slot=3 userid=103 serial=259 fake=1",{"health":health})
                self.verdict(meta,item,"pending","window lacks matching native hurt")

    def test_precision_native_fire_must_match_declared_weapon(self):
        meta,item=locomotion_case("precision_stop")
        for stage in item["stages"].values():
            for rec in stage:
                edit_log(rec,"seq=112001 ",{"weapon":"m4a1"})
                edit_log(rec,"seq=112002 ",{"weapon":"m4a1"})
        self.verdict(meta,item,"pending","window lacks matching native weapon_fire")


if __name__ == "__main__":
    unittest.main()
