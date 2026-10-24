// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

// Opt-in combat scene preparation, never part of the AgPB package.
// The operator must independently verify the Radio guard and stop BOTH local
// bot cores before preparing. The literal "stopped" is an external evidence
// acknowledgement, not a claim that this plugin can inspect the core's state.
// Only the target's actionable CUserCmd fields are locked. Subject commands,
// human state and real damage handling are never intercepted or rewritten.
// Equipment/health/ammo are initialized ONCE per explicit prepare, never on a
// timer, frame, spawn, hide/show transition or combat event.
public Plugin myinfo = {
    name = "AgPB locomotion combat fixture",
    author = "AgPB",
    description = "One-shot equipment and target-only controlled visibility scene",
    version = "1.0"
};

enum struct LocoIdentity
{
    int slot;
    int userid;
    int serial;
    int team;
}

LocoIdentity subject, human, locoTarget;
bool bound, hidden, mapEnding;
int generation, sequence, mapSerial, boundMapSerial, targetMoveSequence;
char epoch[40], boundMap[PLATFORM_MAX_PATH], preparedWeapon[64];
float targetVisible[3], targetHidden[3], targetAngles[3], targetVisibleAngles[3];
ConVar cheats, guard;
Handle locoSampleTimer;
bool locoSampling;
int locoSampleToken, locoSampleCore, locoSampleTotal, locoSampleSent, locoSampleGeneration;
float locoSampleInterval;
Handle locoSequenceTimer;
bool locoSequencing;
int locoSequenceToken, locoSequenceGeneration, locoSequenceCore, locoSequenceStage, locoSequenceSample, locoSequenceTotal;
float locoSequenceStarted, locoSequenceShow[3], locoSequenceYaw;
char locoSequenceNonce[64];

bool LocoValid(int client)
{
    return client >= 1 && client <= MaxClients && IsClientInGame(client);
}

bool LocoFinite(float value)
{
    return (view_as<int>(value) & 0x7F800000) != 0x7F800000;
}

int LocoReadInt(int entity, const char[] property)
{
    if (entity <= 0 || !IsValidEntity(entity)) return -1;
    if (HasEntProp(entity, Prop_Send, property)) return GetEntProp(entity, Prop_Send, property);
    if (HasEntProp(entity, Prop_Data, property)) return GetEntProp(entity, Prop_Data, property);
    return -1;
}

bool LocoActive(int client)
{
    return LocoValid(client) && IsPlayerAlive(client) && GetClientTeam(client) >= 2 &&
        GetClientTeam(client) <= 3 && LocoReadInt(client, "m_iPlayerState") == 0;
}

bool LocoPrerequisites()
{
    // Enabled is only a prerequisite. Guard active/intact is verified externally.
    return !mapEnding && cheats != null && cheats.IntValue == 1 && guard != null && guard.BoolValue;
}

void LocoCapture(LocoIdentity identity, int client)
{
    identity.slot = client; identity.userid = GetClientUserId(client);
    identity.serial = GetClientSerial(client); identity.team = GetClientTeam(client);
}

bool LocoMatches(LocoIdentity identity)
{
    return LocoValid(identity.slot) && GetClientUserId(identity.slot) == identity.userid &&
        GetClientSerial(identity.slot) == identity.serial && GetClientTeam(identity.slot) == identity.team;
}

bool LocoSetupValid(int bot, int observer, int opponent, char[] reason, int size)
{
    if (!LocoPrerequisites())
    { strcopy(reason, size, "require_sv_cheats_1_and_enabled_guard_external_intact_verification"); return false; }
    if (bot == observer || bot == opponent || observer == opponent ||
        !LocoActive(bot) || !LocoActive(observer) || !LocoActive(opponent))
    { strcopy(reason, size, "three_distinct_alive_ACTIVE_players_required"); return false; }
    if (!IsFakeClient(bot) || !IsFakeClient(opponent) || IsFakeClient(observer) ||
        IsClientSourceTV(bot) || IsClientSourceTV(opponent) || IsClientSourceTV(observer) || observer >= bot || observer >= opponent)
    { strcopy(reason, size, "two_fakes_and_lower_slot_real_human_required"); return false; }
    if (GetClientTeam(bot) != GetClientTeam(observer) || GetClientTeam(opponent) == GetClientTeam(observer))
    { strcopy(reason, size, "subject_and_human_same_team_target_opposing_required"); return false; }
    for (int client = 1; client <= MaxClients; ++client)
    {
        if (IsClientConnected(client) && IsFakeClient(client) && client != bot && client != opponent)
        { strcopy(reason, size, "unbound_fake_client_present"); return false; }
    }
    return true;
}

bool LocoBindingValid(char[] reason, int size)
{
    if (!bound) { strcopy(reason, size, "no_prepared_scene"); return false; }
    if (!LocoMatches(subject) || !LocoMatches(human) || !LocoMatches(locoTarget))
    { strcopy(reason, size, "bound_identity_or_team_changed"); return false; }
    char map[PLATFORM_MAX_PATH]; GetCurrentMap(map, sizeof(map));
    if (boundMapSerial != mapSerial || !StrEqual(map, boundMap))
    { strcopy(reason, size, "map_changed"); return false; }
    return LocoSetupValid(subject.slot, human.slot, locoTarget.slot, reason, size);
}

void LocoClear(const char[] reason)
{
    LocoCancelSequence(reason);
    LocoCancelSampling(reason);
    if (!bound) return;
    PrintToServer("[AGPB_LOCO] v=1 epoch=%s seq=%d generation=%d action=cleared reason=%s subject_slot=%d subject_userid=%d subject_serial=%d human_slot=%d human_userid=%d human_serial=%d target_slot=%d target_userid=%d target_serial=%d target_input_lock=0 human_writes=0 restoration_writes=0",
        epoch, ++sequence, generation, reason, subject.slot, subject.userid, subject.serial,
        human.slot, human.userid, human.serial, locoTarget.slot, locoTarget.userid, locoTarget.serial);
    bound = false; hidden = false; ++generation; ++targetMoveSequence;
}

bool LocoCheckBinding()
{
    char reason[128];
    if (LocoBindingValid(reason, sizeof(reason))) return true;
    LocoClear(reason);
    return false;
}

bool LocoArgument(int index, char[] text, int size)
{
    int written = GetCmdArg(index, text, size);
    return written > 0 && written < size - 1;
}

bool LocoIntArgument(int index, int minimum, int maximum, int &value)
{
    char text[32];
    if (!LocoArgument(index, text, sizeof(text))) return false;
    value = 0;
    for (int cursor = 0; cursor < strlen(text); ++cursor)
    {
        if (text[cursor] < '0' || text[cursor] > '9') return false;
        int digit = text[cursor] - '0';
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    return value >= minimum && value <= maximum;
}

bool LocoFloatArgument(int index, float &value, bool coordinate = true)
{
    char text[128];
    return LocoArgument(index, text, sizeof(text)) && StringToFloatEx(text, value) == strlen(text) &&
        LocoFinite(value) && (!coordinate || (value >= -16384.0 && value <= 16384.0));
}

bool LocoGunArgument(int index, char[] classname, int size)
{
    char text[64], alias[64];
    if (!LocoArgument(index, text, sizeof(text))) return false;
    for (int cursor = 0; cursor < strlen(text); ++cursor)
    {
        if (text[cursor] >= 'A' && text[cursor] <= 'Z') text[cursor] += 'a' - 'A';
        if (!(text[cursor] >= 'a' && text[cursor] <= 'z') && !(text[cursor] >= '0' && text[cursor] <= '9') && text[cursor] != '_')
            return false;
    }
    if (StrContains(text, "weapon_", true) == 0) strcopy(alias, sizeof(alias), text[7]);
    else strcopy(alias, sizeof(alias), text);
    if (StrEqual(alias, "m4")) strcopy(alias, sizeof(alias), "m4a1");
    else if (StrEqual(alias, "mp5")) strcopy(alias, sizeof(alias), "mp5navy");
    else if (StrEqual(alias, "ak")) strcopy(alias, sizeof(alias), "ak47");
    // All native CSS firearm classes; no grenades, C4, knife or arbitrary entity.
    static const char guns[][] = {
        "ak47", "aug", "awp", "deagle", "elite", "famas", "fiveseven", "g3sg1", "galil",
        "glock", "m249", "m3", "m4a1", "mac10", "mp5navy", "p228", "p90", "scout",
        "sg550", "sg552", "tmp", "ump45", "usp", "xm1014"
    };
    for (int gun = 0; gun < sizeof(guns); ++gun)
    {
        if (!StrEqual(alias, guns[gun])) continue;
        Format(classname, size, "weapon_%s", alias);
        return true;
    }
    return false;
}

bool LocoRequiredProperties(int client)
{
    return HasEntProp(client, Prop_Send, "m_ArmorValue") && HasEntProp(client, Prop_Send, "m_iAmmo") &&
        HasEntProp(client, Prop_Send, "m_hActiveWeapon") && HasEntProp(client, Prop_Data, "m_iHealth");
}

bool LocoRemoveGuns(int client)
{
    // Preserve knife, grenades and map objectives. Only firearm slots are reset.
    for (int slot = 0; slot <= 1; ++slot)
    {
        for (int count = 0; count < 16; ++count)
        {
            int item = GetPlayerWeaponSlot(client, slot);
            if (item <= MaxClients || !IsValidEntity(item)) break;
            if (!RemovePlayerItem(client, item)) return false;
            AcceptEntityInput(item, "Kill");
            if (count == 15 && GetPlayerWeaponSlot(client, slot) > MaxClients) return false;
        }
    }
    return true;
}

bool LocoEquipSubject(int client, const char[] classname, int reserve)
{
    if (!LocoRemoveGuns(client)) return false;
    int weapon = GivePlayerItem(client, classname);
    if (weapon <= MaxClients || !IsValidEntity(weapon) || !HasEntProp(weapon, Prop_Send, "m_iClip1")) return false;
    int clip = GetEntProp(weapon, Prop_Send, "m_iClip1");
    int ammoType = LocoReadInt(weapon, "m_iPrimaryAmmoType");
    if (clip <= 0 || ammoType < 0 || ammoType >= GetEntPropArraySize(client, Prop_Send, "m_iAmmo")) return false;
    // A fresh native firearm supplies its own full spawn magazine. Preserve its
    // game-defined clip size; reserve is explicit and initialized exactly once.
    // GivePlayerItem already equips ownership. Re-equipping the same firearm
    // would drop it, transfer reserve into ExtraAmmo and zero player reserve.
    FakeClientCommand(client, "use %s", classname);
    SetEntProp(weapon, Prop_Send, "m_iClip1", clip);
    SetEntProp(client, Prop_Send, "m_iAmmo", reserve, _, ammoType);
    return true;
}

void LocoPlayerSnapshot(int client, const char[] role, const char[] phase)
{
    float origin[3], eye[3], velocity[3];
    GetClientAbsOrigin(client, origin); GetClientEyeAngles(client, eye);
    if (HasEntProp(client, Prop_Data, "m_vecVelocity")) GetEntPropVector(client, Prop_Data, "m_vecVelocity", velocity);
    else if (HasEntProp(client, Prop_Send, "m_vecVelocity")) GetEntPropVector(client, Prop_Send, "m_vecVelocity", velocity);
    int weapon = GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
    char classname[64] = "none", name[MAX_NAME_LENGTH];
    int reference = INVALID_ENT_REFERENCE, ammoType = -1, reserve = -1;
    if (weapon > MaxClients && IsValidEntity(weapon))
    {
        GetEntityClassname(weapon, classname, sizeof(classname)); reference = EntIndexToEntRef(weapon);
        ammoType = LocoReadInt(weapon, "m_iPrimaryAmmoType");
        if (ammoType >= 0 && HasEntProp(client, Prop_Send, "m_iAmmo") && ammoType < GetEntPropArraySize(client, Prop_Send, "m_iAmmo"))
            reserve = GetEntProp(client, Prop_Send, "m_iAmmo", _, ammoType);
    }
    GetClientName(client, name, sizeof(name));
    PrintToServer("[AGPB_LOCO_PLAYER] v=1 epoch=%s seq=%d generation=%d phase=%s tick=%d t=%.6f role=%s slot=%d userid=%d serial=%d name=\"%s\" fake=%d team=%d alive=%d state=%d origin=(%.4f,%.4f,%.4f) eye=(%.4f,%.4f,%.4f) velocity=(%.4f,%.4f,%.4f) hp=%d armor=%d active=%s active_ref=%d clip=%d ammo_type=%d reserve=%d buttons=0x%X target_input_lock=%d",
        epoch, ++sequence, generation, phase, GetGameTickCount(), GetGameTime(), role, client, GetClientUserId(client),
        GetClientSerial(client), name, IsFakeClient(client) ? 1 : 0, GetClientTeam(client), IsPlayerAlive(client) ? 1 : 0,
        LocoReadInt(client, "m_iPlayerState"), origin[0], origin[1], origin[2], eye[0], eye[1], eye[2],
        velocity[0], velocity[1], velocity[2], LocoReadInt(client, "m_iHealth"), LocoReadInt(client, "m_ArmorValue"),
        classname, reference, LocoReadInt(weapon, "m_iClip1"), ammoType, reserve, GetClientButtons(client),
        bound && client == locoTarget.slot && LocoMatches(locoTarget) ? 1 : 0);
}

void LocoSnapshot(const char[] phase)
{
    if (!LocoCheckBinding()) return;
    PrintToServer("[AGPB_LOCO] v=1 epoch=%s seq=%d generation=%d action=snapshot phase=%s map_serial=%d map=%s tick=%d hidden=%d requested_weapon=%s target_input_lock=1 subject_cmd_writes=0 human_writes=0 guard_intact_external_verification_required=1",
        epoch, ++sequence, generation, phase, boundMapSerial, boundMap, GetGameTickCount(), hidden ? 1 : 0, preparedWeapon);
    LocoPlayerSnapshot(subject.slot, "subject", phase);
    LocoPlayerSnapshot(human.slot, "human", phase);
    LocoPlayerSnapshot(locoTarget.slot, "target", phase);
}

public void OnPluginStart()
{
    Format(epoch, sizeof(epoch), "%d-%08x", GetTime(), GetURandomInt());
    cheats = FindConVar("sv_cheats"); guard = FindConVar("sm_agpb_fakeclient_guard");
    if (cheats != null) cheats.AddChangeHook(LocoPrerequisiteChanged);
    if (guard != null) guard.AddChangeHook(LocoPrerequisiteChanged);
    RegServerCmd("agpb_loco_prepare", LocoPrepareCommand);
    RegServerCmd("agpb_loco_hide", LocoHideCommand);
    RegServerCmd("agpb_loco_show", LocoShowCommand);
    RegServerCmd("agpb_loco_state", LocoStateCommand);
    RegServerCmd("agpb_loco_clear", LocoClearCommand);
    RegServerCmd("agpb_loco_sample", LocoSampleCommand);
    RegServerCmd("agpb_loco_sequence", LocoSequenceCommand);
    HookEventEx("player_death", LocoLifecycle, EventHookMode_Post);
    HookEventEx("player_spawn", LocoLifecycle, EventHookMode_Post);
    HookEventEx("player_team", LocoLifecycle, EventHookMode_Post);
    HookEventEx("round_start", LocoRoundTransition, EventHookMode_Post);
    HookEventEx("round_end", LocoRoundTransition, EventHookMode_Post);
    PrintToServer("[AGPB_LOCO] loaded epoch=%s disabled=1 preparation_once=1 target_only_input_lock=1 damage_changes=0", epoch);
}

public void OnMapStart() { LocoClear("map_start"); ++mapSerial; mapEnding = false; }
public void OnMapEnd() { mapEnding = true; LocoClear("map_end"); }
public void OnPluginEnd() { LocoClear("plugin_end"); }
public void OnGameFrame() { if (bound) LocoCheckBinding(); }

public void LocoPrerequisiteChanged(ConVar convar, const char[] oldValue, const char[] newValue)
{
    if (bound && !LocoPrerequisites()) LocoClear("prerequisite_changed");
}

public void OnClientDisconnect(int client)
{
    if (bound && (client == subject.slot || client == human.slot || client == locoTarget.slot)) LocoClear("bound_disconnect");
}

public void OnClientPutInServer(int client)
{
    if (bound && IsFakeClient(client) && client != subject.slot && client != locoTarget.slot) LocoClear("unbound_fake_joined");
}

public Action LocoPrepareCommand(int args)
{
    if (args < 16 || args > 20)
    { PrintToServer("[AGPB_LOCO] usage: agpb_loco_prepare subject human target gun sx sy sz syaw tx ty tz tyaw hidex hidey hidez stopped [subject_hp=100] [armor=100] [target_hp=2000] [reserve=200]"); return Plugin_Handled; }
    int bot, observer, opponent;
    if (!LocoIntArgument(1, 1, MaxClients, bot) || !LocoIntArgument(2, 1, MaxClients, observer) || !LocoIntArgument(3, 1, MaxClients, opponent))
    { PrintToServer("[AGPB_LOCO] rejected: invalid_player_slot"); return Plugin_Handled; }
    char reason[128], classname[64], ack[32];
    if (!LocoSetupValid(bot, observer, opponent, reason, sizeof(reason)))
    { PrintToServer("[AGPB_LOCO] rejected: %s", reason); return Plugin_Handled; }
    if (!LocoArgument(16, ack, sizeof(ack)) || !StrEqual(ack, "stopped") || GetClientButtons(bot) != 0)
    { PrintToServer("[AGPB_LOCO] rejected: external_core_stop_and_zero_input_record_required_then_literal_stopped_and_released_subject_buttons"); return Plugin_Handled; }
    if (!LocoGunArgument(4, classname, sizeof(classname)))
    { PrintToServer("[AGPB_LOCO] rejected: specify_native_firearm_alias_or_weapon_class"); return Plugin_Handled; }
    float botPosition[3], botAngles[3], visible[3], hiding[3], opponentAngles[3], zero[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!LocoFloatArgument(axis + 5, botPosition[axis]) || !LocoFloatArgument(axis + 9, visible[axis]) ||
            !LocoFloatArgument(axis + 13, hiding[axis]))
        { PrintToServer("[AGPB_LOCO] rejected: coordinates_require_finite_values_within_16384"); return Plugin_Handled; }
    }
    if (!LocoFloatArgument(8, botAngles[1], false) || !LocoFloatArgument(12, opponentAngles[1], false))
    { PrintToServer("[AGPB_LOCO] rejected: finite_yaws_required"); return Plugin_Handled; }
    int botHealth = 100, armor = 100, targetHealth = 2000, reserve = 200;
    if ((args >= 17 && !LocoIntArgument(17, 1, 10000, botHealth)) || (args >= 18 && !LocoIntArgument(18, 0, 100, armor)) ||
        (args >= 19 && !LocoIntArgument(19, 1, 10000, targetHealth)) || (args >= 20 && !LocoIntArgument(20, 0, 10000, reserve)))
    { PrintToServer("[AGPB_LOCO] rejected: hp_1_to_10000_armor_0_to_100_reserve_0_to_10000_required"); return Plugin_Handled; }
    if (!LocoRequiredProperties(bot) || !LocoRequiredProperties(opponent) || !HasEntProp(observer, Prop_Send, "m_hActiveWeapon"))
    { PrintToServer("[AGPB_LOCO] rejected: required_public_properties_unavailable"); return Plugin_Handled; }
    LocoClear("explicit_reprepare");
    LocoCapture(subject, bot); LocoCapture(human, observer); LocoCapture(locoTarget, opponent);
    for (int axis = 0; axis < 3; ++axis)
    { targetVisible[axis] = visible[axis]; targetHidden[axis] = hiding[axis]; targetAngles[axis] = opponentAngles[axis]; targetVisibleAngles[axis] = opponentAngles[axis]; }
    boundMapSerial = mapSerial; GetCurrentMap(boundMap, sizeof(boundMap));
    strcopy(preparedWeapon, sizeof(preparedWeapon), classname);
    ++generation; bound = true; hidden = false;
    if (!LocoEquipSubject(bot, classname, reserve) || !LocoRemoveGuns(opponent))
    { PrintToServer("[AGPB_LOCO] prepare_failed reason=native_equipment_operation prior_equipment_changes_may_remain=1"); LocoClear("equipment_failed"); return Plugin_Handled; }
    FakeClientCommand(opponent, "use weapon_knife");
    SetEntityHealth(bot, botHealth); SetEntProp(bot, Prop_Send, "m_ArmorValue", armor);
    SetEntityHealth(opponent, targetHealth); SetEntProp(opponent, Prop_Send, "m_ArmorValue", 0);
    TeleportEntity(bot, botPosition, botAngles, zero);
    TeleportEntity(opponent, visible, opponentAngles, zero);
    PrintToServer("[AGPB_LOCO] v=1 epoch=%s seq=%d generation=%d action=prepared map=%s subject_slot=%d subject_userid=%d subject_serial=%d human_slot=%d human_userid=%d human_serial=%d target_slot=%d target_userid=%d target_serial=%d subject_hp_requested=%d armor_requested=%d target_hp_requested=%d reserve_requested=%d hide=(%.4f,%.4f,%.4f) stop_ack=external_raw_record_required target_core_stop=external target_input_lock=1 preparation_once=1 damage_changes=0 human_writes=0",
        epoch, ++sequence, generation, boundMap, subject.slot, subject.userid, subject.serial, human.slot, human.userid, human.serial,
        locoTarget.slot, locoTarget.userid, locoTarget.serial, botHealth, armor, targetHealth, reserve, hiding[0], hiding[1], hiding[2]);
    LocoSnapshot("prepared");
    RequestFrame(LocoPrepareReadback, generation);
    return Plugin_Handled;
}

public void LocoPrepareReadback(any token)
{
    if (bound && token == generation) LocoSnapshot("prepared_next_frame");
}

Action LocoMoveTargetCommand(int args, bool hide)
{
    if (locoSequencing) { PrintToServer("[AGPB_LOCO] rejected: finite_sequence_in_progress"); return Plugin_Handled; }
    if ((hide && args != 0) || (!hide && args != 0 && args != 4))
    { PrintToServer("[AGPB_LOCO] usage: agpb_loco_hide or agpb_loco_show [x y z yaw]"); return Plugin_Handled; }
    if (!LocoCheckBinding()) { PrintToServer("[AGPB_LOCO] rejected: no_valid_prepared_scene"); return Plugin_Handled; }
    float position[3], angles[3], zero[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        position[axis] = hide ? targetHidden[axis] : targetVisible[axis];
        angles[axis] = hide ? targetAngles[axis] : targetVisibleAngles[axis];
    }
    if (!hide && args == 4)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!LocoFloatArgument(axis + 1, position[axis]))
            { PrintToServer("[AGPB_LOCO] rejected: show_coordinates_require_finite_values_within_16384"); return Plugin_Handled; }
        }
        if (!LocoFloatArgument(4, angles[1], false))
        { PrintToServer("[AGPB_LOCO] rejected: finite_show_yaw_required"); return Plugin_Handled; }
    }
    TeleportEntity(locoTarget.slot, position, angles, zero);
    for (int axis = 0; axis < 3; ++axis) targetAngles[axis] = angles[axis];
    hidden = hide;
    PrintToServer("[AGPB_LOCO] v=1 epoch=%s seq=%d generation=%d action=%s tick=%d t=%.6f target_slot=%d target_userid=%d target_serial=%d requested_origin=(%.4f,%.4f,%.4f) requested_yaw=%.4f custom_show=%d target_teleports=1 subject_writes=0 health_writes=0 ammo_writes=0 visibility_requires_real_observation=1",
        epoch, ++sequence, generation, hide ? "hide" : "show", GetGameTickCount(), GetGameTime(),
        locoTarget.slot, locoTarget.userid, locoTarget.serial, position[0], position[1], position[2], angles[1], args == 4 ? 1 : 0);
    LocoSnapshot(hide ? "hidden" : "shown");
    RequestFrame(LocoTargetReadback, ++targetMoveSequence);
    return Plugin_Handled;
}

public Action LocoHideCommand(int args) { return LocoMoveTargetCommand(args, true); }
public Action LocoShowCommand(int args) { return LocoMoveTargetCommand(args, false); }

public void LocoTargetReadback(any token)
{
    if (bound && token == targetMoveSequence) LocoSnapshot(hidden ? "hidden_next_frame" : "shown_next_frame");
}

public Action LocoStateCommand(int args)
{
    if (args != 0) { PrintToServer("[AGPB_LOCO] usage: agpb_loco_state"); return Plugin_Handled; }
    if (bound) LocoSnapshot("state");
    else PrintToServer("[AGPB_LOCO] v=1 epoch=%s seq=%d generation=%d action=idle target_input_lock=0", epoch, ++sequence, generation);
    return Plugin_Handled;
}

public Action LocoClearCommand(int args)
{
    if (args != 0) { PrintToServer("[AGPB_LOCO] usage: agpb_loco_clear"); return Plugin_Handled; }
    LocoClear("explicit_clear");
    PrintToServer("[AGPB_LOCO] binding_cleared target_input_lock=0 human_writes=0 equipment_and_positions_left_unchanged=1");
    return Plugin_Handled;
}

void LocoCancelSampling(const char[] reason)
{
    if (!locoSampling && locoSampleTimer == null) return;
    locoSampling = false; ++locoSampleToken;
    if (locoSampleTimer != null) { delete locoSampleTimer; locoSampleTimer = null; }
    PrintToServer("[AGPB_LOCO_SAMPLE] v=1 epoch=%s action=cancelled generation=%d reason=%s sent=%d total=%d input_writes=0",
        epoch, locoSampleGeneration, reason, locoSampleSent, locoSampleTotal);
}

public Action LocoSampleCommand(int args)
{
    if (locoSequencing) { PrintToServer("[AGPB_LOCO_SAMPLE] rejected: finite_sequence_in_progress"); return Plugin_Handled; }
    if (args != 4)
    { PrintToServer("[AGPB_LOCO_SAMPLE] usage: agpb_loco_sample core_index delay count interval"); return Plugin_Handled; }
    int core, count;
    float delay, interval;
    if (!LocoIntArgument(1, 0, MaxClients - 1, core) || !LocoFloatArgument(2, delay, false) ||
        !LocoIntArgument(3, 1, 12, count) || !LocoFloatArgument(4, interval, false) ||
        delay < 0.0 || delay > 5.0 || interval < 0.05 || interval > 1.0)
    { PrintToServer("[AGPB_LOCO_SAMPLE] rejected: core_index_valid_delay_0_to_5_count_1_to_12_interval_0.05_to_1_required"); return Plugin_Handled; }
    if (!LocoCheckBinding())
    { PrintToServer("[AGPB_LOCO_SAMPLE] rejected: valid_prepared_identity_binding_required"); return Plugin_Handled; }
    LocoCancelSampling("replaced");
    locoSampling = true; ++locoSampleToken;
    locoSampleCore = core; locoSampleTotal = count; locoSampleSent = 0;
    locoSampleGeneration = generation; locoSampleInterval = interval;
    locoSampleTimer = CreateTimer(delay, LocoSampleTimerFired, locoSampleToken, TIMER_FLAG_NO_MAPCHANGE);
    PrintToServer("[AGPB_LOCO_SAMPLE] v=1 epoch=%s action=armed generation=%d core_index=%d delay=%.4f count=%d interval=%.4f subject_slot=%d subject_userid=%d subject_serial=%d target_slot=%d target_userid=%d target_serial=%d input_writes=0",
        epoch, generation, core, delay, count, interval, subject.slot, subject.userid, subject.serial,
        locoTarget.slot, locoTarget.userid, locoTarget.serial);
    return Plugin_Handled;
}

public Action LocoSampleTimerFired(Handle timer, any token)
{
    if (timer != locoSampleTimer) return Plugin_Stop;
    locoSampleTimer = null;
    if (!locoSampling || token != locoSampleToken) return Plugin_Stop;
    if (generation != locoSampleGeneration || !LocoCheckBinding())
    { LocoCancelSampling("binding_changed"); return Plugin_Stop; }
    RequestFrame(LocoSampleReadOnlyFrame, token);
    return Plugin_Stop;
}

public void LocoSampleReadOnlyFrame(any token)
{
    if (!locoSampling || token != locoSampleToken) return;
    if (generation != locoSampleGeneration || !LocoCheckBinding())
    { LocoCancelSampling("binding_changed"); return; }
    ++locoSampleSent;
    PrintToServer("[AGPB_LOCO_SAMPLE] v=1 epoch=%s action=begin generation=%d sample=%d total=%d core_index=%d tick=%d t=%.6f input_writes=0",
        epoch, generation, locoSampleSent, locoSampleTotal, locoSampleCore, GetGameTickCount(), GetGameTime());
    // Only these four read-only diagnostics are queued; no input, equipment,
    // position, health or other game command is issued by the sampler.
    ServerCommand("agpb_bot_info %d;agpb_bot_combat_info %d;agpb_wp_probe_combat;sm_agpb_guard_status", locoSampleCore, locoSampleCore);
    ServerExecute();
    PrintToServer("[AGPB_LOCO_SAMPLE] v=1 epoch=%s action=end generation=%d sample=%d total=%d tick=%d input_writes=0",
        epoch, locoSampleGeneration, locoSampleSent, locoSampleTotal, GetGameTickCount());
    if (!locoSampling || token != locoSampleToken) return;
    if (locoSampleSent >= locoSampleTotal)
    {
        locoSampling = false;
        PrintToServer("[AGPB_LOCO_SAMPLE] v=1 epoch=%s action=complete generation=%d sent=%d input_writes=0",
            epoch, locoSampleGeneration, locoSampleSent);
        return;
    }
    locoSampleTimer = CreateTimer(locoSampleInterval, LocoSampleTimerFired, token, TIMER_FLAG_NO_MAPCHANGE);
}

void LocoCancelSequence(const char[] reason)
{
    if (!locoSequencing && locoSequenceTimer == null) return;
    locoSequencing = false; ++locoSequenceToken;
    if (locoSequenceTimer != null) { delete locoSequenceTimer; locoSequenceTimer = null; }
    PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=cancelled generation=%d stage=%d samples=%d reason=%s",
        epoch, locoSequenceNonce, locoSequenceGeneration, locoSequenceStage, locoSequenceTotal, reason);
}

bool LocoSequenceValid(int token)
{
    if (!locoSequencing || token != locoSequenceToken) return false;
    if (generation != locoSequenceGeneration || !LocoCheckBinding())
    { LocoCancelSequence("binding_changed"); return false; }
    return true;
}

void LocoSequenceMarker(bool begin)
{
    static const char phases[][] = {"before", "run", "resumed", "reengaged", "quiet"};
    // These are actual native console marker lines, never fabricated receipts.
    PrintToServer("AGPB_SESSION_%s_%s%s", begin ? "BEGIN" : "END", locoSequenceNonce, phases[locoSequenceStage]);
}

void LocoSequencePhase(char[] phase, int size)
{
    static const char phases[][] = {"before", "run", "resumed", "reengaged", "quiet"};
    strcopy(phase, size, phases[locoSequenceStage]);
}

void LocoSequenceSchedule(float offset)
{
    float delay = locoSequenceStarted + offset - GetGameTime();
    if (delay < 0.0) delay = 0.0;
    locoSequenceTimer = CreateTimer(delay, LocoSequenceTimerFired, locoSequenceToken, TIMER_FLAG_NO_MAPCHANGE);
}

bool LocoSequenceSnapshot(int token)
{
    if (!LocoSequenceValid(token)) return false;
    ++locoSequenceSample; ++locoSequenceTotal;
    PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=sample generation=%d stage=%d sample=%d total_samples=%d core_index=%d tick=%d t=%.6f stage_elapsed=%.6f",
        epoch, locoSequenceNonce, generation, locoSequenceStage, locoSequenceSample, locoSequenceTotal,
        locoSequenceCore, GetGameTickCount(), GetGameTime(), GetGameTime() - locoSequenceStarted);
    char phase[16]; LocoSequencePhase(phase, sizeof(phase));
    PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=execute source_phase=%s generation=%d sample=%d tick=%d command=\"agpb_bot_info %d;agpb_bot_combat_info %d;agpb_wp_probe_combat;sm_agpb_guard_status\" executed_via=ServerCommand_ServerExecute",
        epoch, locoSequenceNonce, phase, generation, locoSequenceSample, GetGameTickCount(), locoSequenceCore, locoSequenceCore);
    ServerCommand("agpb_bot_info %d;agpb_bot_combat_info %d;agpb_wp_probe_combat;sm_agpb_guard_status", locoSequenceCore, locoSequenceCore);
    ServerExecute();
    return LocoSequenceValid(token);
}

bool LocoSequenceTargetMove(int token, bool hide, bool custom)
{
    if (!LocoSequenceValid(token)) return false;
    float position[3], angles[3], zero[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        position[axis] = hide ? targetHidden[axis] : (custom ? locoSequenceShow[axis] : targetVisible[axis]);
        angles[axis] = targetVisibleAngles[axis];
    }
    if (custom) angles[1] = locoSequenceYaw;
    TeleportEntity(locoTarget.slot, position, angles, zero);
    for (int axis = 0; axis < 3; ++axis) targetAngles[axis] = angles[axis];
    hidden = hide;
    PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=%s source_phase=%s generation=%d target_slot=%d target_userid=%d target_serial=%d requested_origin=(%.4f,%.4f,%.4f) requested_yaw=%.4f command=\"TeleportEntity target_slot=%d origin=(%.4f,%.4f,%.4f) eye=(%.4f,%.4f,%.4f) velocity=(0,0,0)\" executed_via=SDKTools subject_writes=0 health_writes=0 ammo_writes=0 tick=%d t=%.6f",
        epoch, locoSequenceNonce, hide ? "hide" : "show", hide ? "resumed" : (custom ? "reengaged" : "run"),
        generation, locoTarget.slot, locoTarget.userid, locoTarget.serial, position[0], position[1], position[2], angles[1],
        locoTarget.slot, position[0], position[1], position[2], angles[0], angles[1], angles[2], GetGameTickCount(), GetGameTime());
    LocoSnapshot(hide ? "sequence_hidden" : "sequence_shown");
    return LocoSequenceValid(token);
}

void LocoSequenceBeginPhase(int stage)
{
    locoSequenceStage = stage; locoSequenceSample = 0; locoSequenceStarted = GetGameTime();
    LocoSequenceMarker(true);
}

public Action LocoSequenceCommand(int args)
{
    if (locoSequencing || locoSampling)
    { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: sequence_or_sampler_already_running"); return Plugin_Handled; }
    if (args != 6)
    { PrintToServer("[AGPB_LOCO_SEQUENCE] usage: agpb_loco_sequence core_index nonce_alphanumeric showx showy showz showyaw"); return Plugin_Handled; }
    int core; char nonce[64]; float position[3], yaw;
    if (!LocoIntArgument(1, 0, MaxClients - 1, core) || !LocoArgument(2, nonce, sizeof(nonce)))
    { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: invalid_core_index_or_nonce"); return Plugin_Handled; }
    for (int cursor = 0; cursor < strlen(nonce); ++cursor)
    {
        if (!(nonce[cursor] >= 'a' && nonce[cursor] <= 'z') && !(nonce[cursor] >= 'A' && nonce[cursor] <= 'Z') &&
            !(nonce[cursor] >= '0' && nonce[cursor] <= '9'))
        { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: nonce_requires_ASCII_letters_and_digits_only"); return Plugin_Handled; }
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!LocoFloatArgument(axis + 3, position[axis]))
        { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: show_coordinates_require_finite_values_within_16384"); return Plugin_Handled; }
    }
    if (!LocoFloatArgument(6, yaw, false) || !LocoCheckBinding())
    { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: finite_yaw_and_valid_prepared_identity_binding_required"); return Plugin_Handled; }
    if (!hidden)
    { PrintToServer("[AGPB_LOCO_SEQUENCE] rejected: hide_target_before_starting_navigation_sequence"); return Plugin_Handled; }
    strcopy(locoSequenceNonce, sizeof(locoSequenceNonce), nonce);
    for (int axis = 0; axis < 3; ++axis) locoSequenceShow[axis] = position[axis];
    locoSequenceYaw = yaw; locoSequenceCore = core; locoSequenceGeneration = generation;
    locoSequenceStage = 0; locoSequenceSample = 0; locoSequenceTotal = 0;
    locoSequenceStarted = GetGameTime(); locoSequencing = true; ++locoSequenceToken;
    LocoSequenceSchedule(0.35);
    PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=armed generation=%d core_index=%d initial_delay=0.35 expected_samples=26 subject_slot=%d subject_userid=%d subject_serial=%d target_slot=%d target_userid=%d target_serial=%d navigation_commands=0 final_stop_commands=1",
        epoch, locoSequenceNonce, generation, core, subject.slot, subject.userid, subject.serial,
        locoTarget.slot, locoTarget.userid, locoTarget.serial);
    return Plugin_Handled;
}

public Action LocoSequenceTimerFired(Handle timer, any token)
{
    if (timer != locoSequenceTimer) return Plugin_Stop;
    locoSequenceTimer = null;
    if (LocoSequenceValid(token)) RequestFrame(LocoSequenceFrame, token);
    return Plugin_Stop;
}

public void LocoSequenceFrame(any token)
{
    if (!LocoSequenceValid(token)) return;
    if (locoSequenceStage == 0)
    {
        LocoSequenceMarker(true);
        if (!LocoSequenceSnapshot(token)) return;
        LocoSequenceMarker(false);
        if (!LocoSequenceTargetMove(token, false, false)) return;
        LocoSequenceBeginPhase(1); LocoSequenceSchedule(0.6);
        return;
    }
    if (locoSequenceStage == 1 || locoSequenceStage == 3)
    {
        if (locoSequenceSample < 7)
        {
            if (!LocoSequenceSnapshot(token)) return;
            LocoSequenceSchedule(locoSequenceSample < 7 ? 0.6 + float(locoSequenceSample) * 0.2 : 2.0);
            return;
        }
        LocoSequenceMarker(false);
        if (locoSequenceStage == 1)
        {
            if (!LocoSequenceTargetMove(token, true, false)) return;
            LocoSequenceBeginPhase(2); LocoSequenceSchedule(0.2);
        }
        else
        {
            LocoSequenceBeginPhase(4);
            // The sequence's only subject control command: the final real stop.
            PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=execute source_phase=quiet generation=%d tick=%d command=\"agpb_bot_stop %d\" executed_via=ServerCommand_ServerExecute",
                epoch, locoSequenceNonce, generation, GetGameTickCount(), locoSequenceCore);
            ServerCommand("agpb_bot_stop %d", locoSequenceCore); ServerExecute();
            if (!LocoSequenceValid(token)) return;
            locoSequenceStarted = GetGameTime(); LocoSequenceSchedule(0.1);
        }
        return;
    }
    if (locoSequenceStage == 2)
    {
        if (!LocoSequenceSnapshot(token)) return;
        if (locoSequenceSample < 4)
        {
            float offsets[4] = {0.2, 0.5, 0.8, 1.0};
            LocoSequenceSchedule(offsets[locoSequenceSample]);
        }
        else
        {
            LocoSequenceMarker(false);
            if (!LocoSequenceTargetMove(token, false, true)) return;
            LocoSequenceBeginPhase(3); LocoSequenceSchedule(0.6);
        }
        return;
    }
    if (!LocoSequenceSnapshot(token)) return;
    if (locoSequenceSample < 7) LocoSequenceSchedule(0.1 + float(locoSequenceSample) * 0.2);
    else
    {
        LocoSequenceMarker(false); locoSequencing = false;
        PrintToServer("[AGPB_LOCO_SEQUENCE] v=1 epoch=%s nonce=%s action=complete generation=%d samples=%d final_stop_commands=1 navigation_commands=0 health_writes=0 ammo_writes=0",
            epoch, locoSequenceNonce, generation, locoSequenceTotal);
    }
}

public Action OnPlayerRunCmd(int client, int &buttons, int &impulse, float vel[3], float angles[3], int &weapon, int &subtype,
    int &cmdnum, int &tickcount, int &seed, int mouse[2])
{
    // This early return is the subject/human isolation boundary. No write may
    // precede it. Identity and scene safety are rechecked before target writes.
    if (!bound || client != locoTarget.slot || client == subject.slot || client == human.slot) return Plugin_Continue;
    if (!LocoCheckBinding()) return Plugin_Continue;
    buttons = 0; impulse = 0; weapon = 0; subtype = 0; mouse[0] = 0; mouse[1] = 0;
    for (int axis = 0; axis < 3; ++axis) { vel[axis] = 0.0; angles[axis] = targetAngles[axis]; }
    // Preserve command identity/tick/seed so ordinary engine physics still runs.
    return Plugin_Changed;
}

public void LocoLifecycle(Event event, const char[] name, bool dontBroadcast)
{
    if (!bound) return;
    int userid = event.GetInt("userid", 0);
    if (userid == subject.userid || userid == human.userid || userid == locoTarget.userid) LocoClear(name);
}

public void LocoRoundTransition(Event event, const char[] name, bool dontBroadcast)
{
    LocoClear(name);
}
