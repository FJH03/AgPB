// SPDX-License-Identifier: GPL-3.0-only
// Optional navigation-only fixture; never packaged with AgPB.
// Property/identity helpers follow agpb_jump_fixture.sp/agpb_player_fixture.sp.
// Explicit existing entity only; no entity search, creation, damage, health,
// collision-mode changes, player writes or synthetic events. The two explicit
// mutation commands each call TeleportEntity on the selected prop once.
// Stop the core and retain raw agpb_list/agpb_bot_info before preparation or
// restoration. RunCmdPost is observation only. No runtime correction/restore.
//
// agpb_prop_state <ent> <bot-slot> <human-slot>
// agpb_prop_prepare <ent> <bot-slot> <human-slot> x y z [pitch yaw roll]
// agpb_prop_watch <ent> <bot-slot> <human-slot> [seconds=5, max20]
// agpb_prop_watch_stop
// agpb_prop_restore
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
    name = "AgPB navigation prop probe", author = "AgPB",
    description = "Existing-prop one-shot preparation, explicit restoration and read-only navigation samples",
    version = "1.0"
};

#define WATCH_INTERVAL 0.05
#define WATCH_SECONDS_MAX 20.0
#define WATCH_SAMPLE_MAX 400
#define CMD_AGE_MAX 0.25
#define RELEASE_BUTTONS (IN_ATTACK | IN_ATTACK2 | IN_JUMP | IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_LEFT | IN_RIGHT)

enum struct Pair {
    int bot;
    int botUserid;
    int botSerial;
    int botLifecycle;
    int human;
    int humanUserid;
    int humanSerial;
    int humanLifecycle;
    int team;
    int mapSerial;
    int roundSerial;
    char map[PLATFORM_MAX_PATH];
}
enum struct PropState {
    int entity;
    int reference;
    int moveType;
    bool healthKnown;
    int health;
    bool damageKnown;
    int damage;
    bool groupKnown;
    int group;
    bool angularKnown;
    float position[3];
    float angles[3];
    float velocity[3];
    float angular[3];
    char classname[64];
    char model[160];
}

char epoch[40];
int mapSerial = 1, roundSerial = 1, sequence, operation, watchGeneration;
bool mapEnding;
int lifecycle[MAXPLAYERS + 1], cmdSerial[MAXPLAYERS + 1], cmdUserid[MAXPLAYERS + 1];
int cmdNumber[MAXPLAYERS + 1], cmdTick[MAXPLAYERS + 1], cmdButtons[MAXPLAYERS + 1];
float cmdTime[MAXPLAYERS + 1], cmdMove[MAXPLAYERS + 1][3];
bool cmdKnown[MAXPLAYERS + 1];

bool saved, readbackPending, readbackRestore;
int savedEntity = -1, savedReference = INVALID_ENT_REFERENCE;
float originalPosition[3], originalAngles[3], originalVelocity[3];
char savedClass[64], savedModel[160];
Pair savedPair;

Handle watchTimer;
bool watching, previousPosition;
Pair watchPair;
int watchEntity = -1, watchReference = INVALID_ENT_REFERENCE, watchSamples;
float watchDeadline, previousTime, previousPropPosition[3];
char watchClass[64], watchModel[160];

bool Finite(float value) { return (view_as<int>(value) & 0x7F800000) != 0x7F800000; }
bool FiniteVector(const float value[3]) { return Finite(value[0]) && Finite(value[1]) && Finite(value[2]); }
bool ClientValid(int client) { return client >= 1 && client <= MaxClients && IsClientInGame(client); }
bool ReadInt(int entity, const char[] property, int &value, int bytes = 4) {
    value = -1;
    if (HasEntProp(entity, Prop_Send, property)) { value = GetEntProp(entity, Prop_Send, property, bytes); return true; }
    if (HasEntProp(entity, Prop_Data, property)) { value = GetEntProp(entity, Prop_Data, property, bytes); return true; }
    return false;
}
bool ReadVector(int entity, const char[] property, float value[3]) {
    for (int axis = 0; axis < 3; ++axis) value[axis] = 0.0;
    if (HasEntProp(entity, Prop_Send, property)) { GetEntPropVector(entity, Prop_Send, property, value); return FiniteVector(value); }
    if (HasEntProp(entity, Prop_Data, property)) { GetEntPropVector(entity, Prop_Data, property, value); return FiniteVector(value); }
    return false;
}
void ReadModel(int entity, char[] model, int size) {
    strcopy(model, size, "unknown");
    if (HasEntProp(entity, Prop_Data, "m_ModelName")) GetEntPropString(entity, Prop_Data, "m_ModelName", model, size);
    else if (HasEntProp(entity, Prop_Send, "m_ModelName")) GetEntPropString(entity, Prop_Send, "m_ModelName", model, size);
}
bool ActivePlayer(int client) {
    if (!ClientValid(client) || !IsPlayerAlive(client) || GetClientTeam(client) < 2 || GetClientTeam(client) > 3) return false;
    int state;
    return ReadInt(client, "m_iPlayerState", state) && state == 0;
}
bool PairValid(int bot, int human, char[] reason, int size) {
    if (mapEnding) { strcopy(reason, size, "map_ending"); return false; }
    if (!ActivePlayer(bot) || !IsFakeClient(bot) || IsClientSourceTV(bot))
    { strcopy(reason, size, "alive_active_AgPB_fake_required"); return false; }
    char name[MAX_NAME_LENGTH]; GetClientName(bot, name, sizeof(name));
    if (StrContains(name, "AgPB_", true) != 0) { strcopy(reason, size, "AgPB_name_prefix_required"); return false; }
    if (!ActivePlayer(human) || IsFakeClient(human) || IsClientSourceTV(human) || human >= bot)
    { strcopy(reason, size, "lower_slot_alive_active_human_required"); return false; }
    if (GetClientTeam(bot) != GetClientTeam(human)) { strcopy(reason, size, "team_mismatch"); return false; }
    int count;
    for (int client = 1; client <= MaxClients; ++client) {
        if (!IsClientConnected(client) || !IsFakeClient(client)) continue;
        ++count;
        if (client != bot) { strcopy(reason, size, "additional_fake_client"); return false; }
    }
    if (count != 1) { strcopy(reason, size, "exactly_one_fake_required"); return false; }
    return true;
}
void BindPair(Pair pair, int bot, int human) {
    pair.bot = bot; pair.botUserid = GetClientUserId(bot); pair.botSerial = GetClientSerial(bot); pair.botLifecycle = lifecycle[bot];
    pair.human = human; pair.humanUserid = GetClientUserId(human); pair.humanSerial = GetClientSerial(human); pair.humanLifecycle = lifecycle[human];
    pair.team = GetClientTeam(bot); pair.mapSerial = mapSerial; pair.roundSerial = roundSerial;
    GetCurrentMap(pair.map, sizeof(pair.map));
}
bool BindingValid(Pair pair, char[] reason, int size) {
    char map[PLATFORM_MAX_PATH]; GetCurrentMap(map, sizeof(map));
    if (pair.mapSerial != mapSerial || pair.roundSerial != roundSerial || !StrEqual(map, pair.map))
    { strcopy(reason, size, "map_or_round_changed"); return false; }
    if (!PairValid(pair.bot, pair.human, reason, size)) return false;
    if (GetClientUserId(pair.bot) != pair.botUserid || GetClientSerial(pair.bot) != pair.botSerial ||
        GetClientUserId(pair.human) != pair.humanUserid || GetClientSerial(pair.human) != pair.humanSerial ||
        lifecycle[pair.bot] != pair.botLifecycle || lifecycle[pair.human] != pair.humanLifecycle ||
        GetClientTeam(pair.bot) != pair.team || GetClientTeam(pair.human) != pair.team)
    { strcopy(reason, size, "bound_identity_lifecycle_or_team_changed"); return false; }
    return true;
}
bool CheatsEnabled() {
    ConVar cheats = FindConVar("sv_cheats");
    return cheats != null && cheats.IntValue == 1;
}
bool ReleasedBot(int bot, char[] reason, int size) {
    float age = GetGameTime() - cmdTime[bot], velocity[3];
    if (!cmdKnown[bot] || cmdSerial[bot] != GetClientSerial(bot) || cmdUserid[bot] != GetClientUserId(bot) ||
        age < 0.0 || age > CMD_AGE_MAX || !FiniteVector(cmdMove[bot]))
    { strcopy(reason, size, "fresh_read_only_RunCmdPost_observation_required"); return false; }
    if ((GetClientButtons(bot) & RELEASE_BUTTONS) != 0 || (cmdButtons[bot] & RELEASE_BUTTONS) != 0 ||
        GetVectorLength(cmdMove[bot]) > 0.01 || !ReadVector(bot, "m_vecVelocity", velocity) || GetVectorLength(velocity) > 5.0)
    { strcopy(reason, size, "stop_core_first_release_move_attack_jump_and_wait_for_speed_at_most5"); return false; }
    return true;
}
bool ReadProp(int entity, PropState state, char[] reason, int size) {
    if (entity <= MaxClients || !IsValidEntity(entity)) { strcopy(reason, size, "existing_nonplayer_entity_required"); return false; }
    GetEntityClassname(entity, state.classname, sizeof(state.classname));
    if (StrContains(state.classname, "prop_physics", true) != 0 || GetEntityMoveType(entity) != MOVETYPE_VPHYSICS)
    { strcopy(reason, size, "explicit_existing_prop_physics_VPHYSICS_required"); return false; }
    // Refuse parent-local transforms: captured origin/angles must be world
    // coordinates for the one permitted native TeleportEntity restoration.
    if (HasEntProp(entity, Prop_Data, "m_hMoveParent") && GetEntPropEnt(entity, Prop_Data, "m_hMoveParent") != -1)
    { strcopy(reason, size, "unparented_prop_required"); return false; }
    if (!ReadVector(entity, "m_vecOrigin", state.position) || !ReadVector(entity, "m_angRotation", state.angles) ||
        !ReadVector(entity, "m_vecVelocity", state.velocity))
    { strcopy(reason, size, "finite_origin_angles_linear_velocity_reflection_required"); return false; }
    state.entity = entity; state.reference = EntIndexToEntRef(entity); state.moveType = view_as<int>(GetEntityMoveType(entity));
    ReadModel(entity, state.model, sizeof(state.model));
    state.healthKnown = ReadInt(entity, "m_iHealth", state.health);
    state.damageKnown = ReadInt(entity, "m_takedamage", state.damage, 1);
    state.groupKnown = ReadInt(entity, "m_CollisionGroup", state.group);
    state.angularKnown = ReadVector(entity, "m_vecAngVelocity", state.angular);
    return true;
}
bool SameProp(int entity, int reference, const char[] classname, const char[] model, PropState state, char[] reason, int size) {
    if (EntRefToEntIndex(reference) != entity || !ReadProp(entity, state, reason, size) || state.reference != reference)
    { strcopy(reason, size, "original_EntRef_missing_or_reused"); return false; }
    if (!StrEqual(classname, state.classname) || !StrEqual(model, state.model))
    { strcopy(reason, size, "original_class_or_model_changed"); return false; }
    return true;
}
bool Argument(int index, char[] text, int size) {
    int written = GetCmdArg(index, text, size);
    return written > 0 && written < size - 1;
}
bool IntegerArgument(int index, int low, int high, int &value) {
    char text[32]; if (!Argument(index, text, sizeof(text))) return false;
    value = 0;
    for (int i = 0; i < strlen(text); ++i) {
        int digit = text[i] - '0';
        if (digit < 0 || digit > 9 || value > (high - digit) / 10) return false;
        value = value * 10 + digit;
        if (value > high) return false;
    }
    return value >= low;
}
bool FloatArgument(int index, float &value) {
    char text[128];
    return Argument(index, text, sizeof(text)) && StringToFloatEx(text, value) == strlen(text) && Finite(value);
}
bool Selection(int &entity, int &bot, int &human, char[] reason, int size) {
    if (!IntegerArgument(1, MaxClients + 1, GetMaxEntities() - 1, entity) ||
        !IntegerArgument(2, 1, MaxClients, bot) || !IntegerArgument(3, 1, MaxClients, human))
    { strcopy(reason, size, "invalid_explicit_entity_or_player_slot"); return false; }
    return PairValid(bot, human, reason, size);
}
void LogState(const char[] phase, int sample, Pair pair, PropState state, bool derivedKnown = false, const float derived[3] = NULL_VECTOR) {
    float position[3], velocity[3]; GetClientAbsOrigin(pair.bot, position);
    bool velocityKnown = ReadVector(pair.bot, "m_vecVelocity", velocity);
    float now = GetGameTime();
    char angularText[96];
    if (state.angularKnown) Format(angularText, sizeof(angularText), "(%.6f,%.6f,%.6f)", state.angular[0], state.angular[1], state.angular[2]);
    else strcopy(angularText, sizeof(angularText), "unknown");
    PrintToServer("[AGPB_PROP_STATE] v=1 epoch=%s seq=%d operation=%d watch=%d sample=%d phase=%s map_serial=%d round_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d ent=%d ref=%d class=%s pos=(%.6f,%.6f,%.6f) angles=(%.6f,%.6f,%.6f) linear_velocity=(%.6f,%.6f,%.6f) linear_source=m_vecVelocity_reflection health_known=%d health=%d takedamage_known=%d takedamage=%d collisiongroup_known=%d collisiongroup=%d movetype=%d writes=0",
        epoch, ++sequence, operation, watchGeneration, sample, phase, pair.mapSerial, pair.roundSerial, pair.map,
        GetGameTickCount(), now, pair.bot, pair.botUserid, pair.botSerial, pair.human, pair.humanUserid, pair.humanSerial, pair.team,
        state.entity, state.reference, state.classname, state.position[0], state.position[1], state.position[2],
        state.angles[0], state.angles[1], state.angles[2], state.velocity[0], state.velocity[1], state.velocity[2],
        state.healthKnown, state.health, state.damageKnown, state.damage, state.groupKnown, state.group, state.moveType);
    PrintToServer("[AGPB_PROP_PHYSICS] v=1 epoch=%s seq=%d operation=%d watch=%d sample=%d phase=%s map_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d model=\"%s\" angular_known=%d angular_reflection=%s angular_source=m_vecAngVelocity_reflection observed_delta_velocity_known=%d observed_delta_velocity=(%.6f,%.6f,%.6f) derived_scope=adjacent_real_position_samples full_physics_restore_claim=0 writes=0",
        epoch, ++sequence, operation, watchGeneration, sample, phase, pair.mapSerial, pair.map, GetGameTickCount(), now,
        state.entity, state.reference, state.model, state.angularKnown, angularText,
        derivedKnown, derived[0], derived[1], derived[2]);
    PrintToServer("[AGPB_PROP_PLAYER] v=1 epoch=%s seq=%d operation=%d watch=%d sample=%d phase=%s map_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d bot_alive=1 bot_fake=1 human_alive=1 human_fake=0 pos=(%.6f,%.6f,%.6f) velocity_known=%d velocity=(%.6f,%.6f,%.6f) flags=0x%X buttons=0x%X cmd_known=%d cmd_age=%.6f cmdnum=%d cmdtick=%d cmd_buttons=0x%X cmd_move=(%.6f,%.6f,%.6f) core_owner_goal_requires_agpb_bot_info=1 writes=0",
        epoch, ++sequence, operation, watchGeneration, sample, phase, pair.mapSerial, pair.map, GetGameTickCount(), now,
        state.entity, state.reference, pair.bot, pair.botUserid, pair.botSerial, pair.human, pair.humanUserid, pair.humanSerial, pair.team,
        position[0], position[1], position[2], velocityKnown, velocity[0], velocity[1], velocity[2], GetEntityFlags(pair.bot), GetClientButtons(pair.bot),
        cmdKnown[pair.bot], now - cmdTime[pair.bot], cmdNumber[pair.bot], cmdTick[pair.bot], cmdButtons[pair.bot],
        cmdMove[pair.bot][0], cmdMove[pair.bot][1], cmdMove[pair.bot][2]);
}
void StopWatch(const char[] reason, bool cancelTimer = true) {
    if (cancelTimer && watchTimer != null) delete watchTimer;
    watchTimer = null;
    if (!watching) return;
    watching = false;
    PrintToServer("[AGPB_PROP_END] v=1 epoch=%s seq=%d watch=%d map_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d bot_slot=%d bot_userid=%d samples=%d reason=%s writes=0 automatic_restore=0",
        epoch, ++sequence, watchGeneration, watchPair.mapSerial, watchPair.map, GetGameTickCount(), GetGameTime(),
        watchEntity, watchReference, watchPair.bot, watchPair.botUserid, watchSamples, reason);
}
public void OnPluginStart() {
    Format(epoch, sizeof(epoch), "%d_%08x", GetTime(), GetURandomInt());
    RegServerCmd("agpb_prop_state", StateCommand);
    RegServerCmd("agpb_prop_prepare", PrepareCommand);
    RegServerCmd("agpb_prop_restore", RestoreCommand);
    RegServerCmd("agpb_prop_watch", WatchCommand);
    RegServerCmd("agpb_prop_watch_stop", StopCommand);
    HookEventEx("player_death", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_spawn", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_team", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("round_start", RoundTransition, EventHookMode_PostNoCopy);
    HookEventEx("round_end", RoundTransition, EventHookMode_PostNoCopy);
    PrintToServer("[AGPB_PROP] loaded version=1.0 navigation_only=1 existing_entity_only=1 observation_interval=0.05 max_seconds=20 automatic_restore=0 player_writes=0");
}
public void OnMapStart() { StopWatch("map_start"); ++mapSerial; ++roundSerial; mapEnding = false; saved = false; readbackPending = false; }
public void OnMapEnd() {
    mapEnding = true; StopWatch("map_end");
    if (saved) PrintToServer("[AGPB_PROP] captured_scene_invalidated=map_end ref=%d automatic_restore=0", savedReference);
    saved = false; readbackPending = false;
}
public void OnPluginEnd() {
    StopWatch("plugin_end");
    if (saved) PrintToServer("[AGPB_PROP] captured_scene_left_unchanged=plugin_end ref=%d automatic_restore=0", savedReference);
}
public void OnClientDisconnect(int client) {
    cmdKnown[client] = false; ++lifecycle[client];
    if (watching && (client == watchPair.bot || client == watchPair.human)) StopWatch("bound_disconnect");
}
public void PlayerLifecycle(Event event, const char[] name, bool dontBroadcast) {
    int client = GetClientOfUserId(event.GetInt("userid", 0));
    if (client > 0) ++lifecycle[client];
    if (watching && (client == watchPair.bot || client == watchPair.human)) StopWatch(name);
}
public void RoundTransition(Event event, const char[] name, bool dontBroadcast) { ++roundSerial; StopWatch(name); }
public void OnPlayerRunCmdPost(int client, int buttons, int impulse, const float vel[3], const float angles[3], int weapon,
    int subtype, int cmdnum, int tickcount, int seed, const int mouse[2]) {
    if (!ClientValid(client) || !IsFakeClient(client)) return;
    cmdKnown[client] = true; cmdSerial[client] = GetClientSerial(client); cmdUserid[client] = GetClientUserId(client);
    cmdTime[client] = GetGameTime(); cmdNumber[client] = cmdnum; cmdTick[client] = tickcount; cmdButtons[client] = buttons;
    for (int axis = 0; axis < 3; ++axis) cmdMove[client][axis] = vel[axis];
}
public Action StateCommand(int args) {
    if (args != 3) { PrintToServer("[AGPB_PROP] usage: agpb_prop_state <ent> <bot-slot> <human-slot>"); return Plugin_Handled; }
    int entity, bot, human; char reason[128]; PropState state; Pair pair;
    if (!Selection(entity, bot, human, reason, sizeof(reason)) || !ReadProp(entity, state, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PROP] rejected: %s writes=0", reason); return Plugin_Handled; }
    BindPair(pair, bot, human); LogState("state", 0, pair, state);
    return Plugin_Handled;
}
public Action PrepareCommand(int args) {
    if (args != 6 && args != 9) { PrintToServer("[AGPB_PROP] usage: agpb_prop_prepare <ent> <bot-slot> <human-slot> x y z [pitch yaw roll]"); return Plugin_Handled; }
    if (!CheatsEnabled()) { PrintToServer("[AGPB_PROP] rejected: sv_cheats_must_equal1 writes=0"); return Plugin_Handled; }
    if (saved || readbackPending) { PrintToServer("[AGPB_PROP] rejected: captured_scene_exists_restore_explicitly_first writes=0"); return Plugin_Handled; }
    int entity, bot, human; char reason[128]; PropState state;
    if (!Selection(entity, bot, human, reason, sizeof(reason)) || !ReleasedBot(bot, reason, sizeof(reason)) || !ReadProp(entity, state, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PROP] rejected: %s writes=0", reason); return Plugin_Handled; }
    float requested[3], angles[3], zero[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!FloatArgument(axis + 4, requested[axis]) || FloatAbs(requested[axis]) > 16384.0)
        { PrintToServer("[AGPB_PROP] rejected: finite_coordinates_within16384_required writes=0"); return Plugin_Handled; }
        angles[axis] = state.angles[axis];
        if (args == 9 && (!FloatArgument(axis + 7, angles[axis]) || FloatAbs(angles[axis]) > 3600.0))
        { PrintToServer("[AGPB_PROP] rejected: finite_angles_within3600_required writes=0"); return Plugin_Handled; }
    }
    StopWatch("explicit_preparation"); BindPair(savedPair, bot, human);
    savedEntity = entity; savedReference = state.reference;
    strcopy(savedClass, sizeof(savedClass), state.classname); strcopy(savedModel, sizeof(savedModel), state.model);
    for (int axis = 0; axis < 3; ++axis) {
        originalPosition[axis] = state.position[axis]; originalAngles[axis] = state.angles[axis]; originalVelocity[axis] = state.velocity[axis];
    }
    saved = true; ++operation;
    LogState("captured_original", 0, savedPair, state);
    // One explicit prop-only preparation. No player, health, damage mode,
    // collision mode, angular velocity or future physics state is changed here.
    TeleportEntity(entity, requested, angles, zero);
    PrintToServer("[AGPB_PROP_PREPARE] v=1 epoch=%s seq=%d operation=%d map_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d bot_slot=%d bot_userid=%d human_slot=%d human_userid=%d requested_pos=(%.6f,%.6f,%.6f) requested_angles=(%.6f,%.6f,%.6f) requested_linear_velocity=(0,0,0) writes=one_prop_teleport player_writes=0 original_health_damage_collision_not_rewritten=1 full_physics_restore_claim=0",
        epoch, ++sequence, operation, mapSerial, savedPair.map, GetGameTickCount(), GetGameTime(), entity, savedReference,
        bot, savedPair.botUserid, human, savedPair.humanUserid, requested[0], requested[1], requested[2], angles[0], angles[1], angles[2]);
    readbackPending = true; readbackRestore = false; RequestFrame(MutationReadback, operation);
    return Plugin_Handled;
}
public Action RestoreCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_PROP] usage: agpb_prop_restore"); return Plugin_Handled; }
    if (!CheatsEnabled() || !saved || readbackPending)
    { PrintToServer("[AGPB_PROP] rejected: cheats_and_original_capture_and_completed_readback_required writes=0"); return Plugin_Handled; }
    char reason[128]; PropState state;
    if (!BindingValid(savedPair, reason, sizeof(reason)) || !ReleasedBot(savedPair.bot, reason, sizeof(reason)) ||
        !SameProp(savedEntity, savedReference, savedClass, savedModel, state, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PROP] rejected_restore: %s original_ref=%d writes=0", reason, savedReference); return Plugin_Handled; }
    StopWatch("explicit_restore"); ++operation;
    LogState("before_restore", 0, savedPair, state);
    // The only restoration write. Never find a substitute entity or restore
    // health, damage mode, collision state, angular velocity or sleeping state.
    TeleportEntity(savedEntity, originalPosition, originalAngles, originalVelocity);
    saved = false;
    PrintToServer("[AGPB_PROP_RESTORE] v=1 epoch=%s seq=%d operation=%d map_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d requested_pos=(%.6f,%.6f,%.6f) requested_angles=(%.6f,%.6f,%.6f) requested_linear_velocity=(%.6f,%.6f,%.6f) linear_source=captured_m_vecVelocity_reflection writes=one_prop_teleport restoration_scope=position_angles_linear_velocity full_physics_restore_claim=0 player_writes=0",
        epoch, ++sequence, operation, mapSerial, savedPair.map, GetGameTickCount(), GetGameTime(), savedEntity, savedReference,
        originalPosition[0], originalPosition[1], originalPosition[2], originalAngles[0], originalAngles[1], originalAngles[2], originalVelocity[0], originalVelocity[1], originalVelocity[2]);
    readbackPending = true; readbackRestore = true; RequestFrame(MutationReadback, operation);
    return Plugin_Handled;
}
public void MutationReadback(any token) {
    if (!readbackPending || token != operation) return;
    readbackPending = false;
    char reason[128]; PropState state;
    if (!BindingValid(savedPair, reason, sizeof(reason)) || !SameProp(savedEntity, savedReference, savedClass, savedModel, state, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PROP] readback_rejected=%s original_ref=%d writes=0", reason, savedReference); return; }
    LogState(readbackRestore ? "restore_readback" : "prepare_readback", 0, savedPair, state);
}
public Action WatchCommand(int args) {
    if (args != 3 && args != 4) { PrintToServer("[AGPB_PROP] usage: agpb_prop_watch <ent> <bot-slot> <human-slot> [seconds=5,max20]"); return Plugin_Handled; }
    int entity, bot, human; char reason[128]; PropState state;
    float seconds = 5.0;
    if (args == 4 && (!FloatArgument(4, seconds) || seconds <= 0.0 || seconds > WATCH_SECONDS_MAX))
    { PrintToServer("[AGPB_PROP] rejected: observation_seconds_must_be_finite_positive_at_most20 writes=0"); return Plugin_Handled; }
    if (!Selection(entity, bot, human, reason, sizeof(reason)) || !ReadProp(entity, state, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PROP] rejected: %s writes=0", reason); return Plugin_Handled; }
    if (saved && entity == savedEntity && state.reference != savedReference)
    { PrintToServer("[AGPB_PROP] rejected: captured_entity_index_was_reused writes=0"); return Plugin_Handled; }
    StopWatch("explicit_observation_replaced"); BindPair(watchPair, bot, human);
    watchEntity = entity; watchReference = state.reference;
    strcopy(watchClass, sizeof(watchClass), state.classname); strcopy(watchModel, sizeof(watchModel), state.model);
    ++watchGeneration; watchSamples = 0; previousPosition = false; watching = true; watchDeadline = GetGameTime() + seconds;
    watchTimer = CreateTimer(WATCH_INTERVAL, SampleTimer, watchGeneration, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
    if (watchTimer == null) { StopWatch("timer_creation_failed"); return Plugin_Handled; }
    PrintToServer("[AGPB_PROP_ARM] v=1 epoch=%s seq=%d watch=%d map_serial=%d round_serial=%d map=%s tick=%d t=%.6f ent=%d ref=%d bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d interval=0.05 seconds=%.6f deadline=%.6f sample_limit=400 core_owner_goal_requires_agpb_bot_info=1 writes=0",
        epoch, ++sequence, watchGeneration, watchPair.mapSerial, watchPair.roundSerial, watchPair.map, GetGameTickCount(), GetGameTime(),
        entity, watchReference, bot, watchPair.botUserid, watchPair.botSerial, human, watchPair.humanUserid, watchPair.humanSerial, watchPair.team, seconds, watchDeadline);
    return Plugin_Handled;
}
public Action SampleTimer(Handle timer, any token) {
    if (!watching || timer != watchTimer || token != watchGeneration) return Plugin_Stop;
    if (GetGameTime() > watchDeadline || watchSamples >= WATCH_SAMPLE_MAX) { StopWatch("bounded_observation_complete", false); return Plugin_Stop; }
    char reason[128]; PropState state;
    if (!BindingValid(watchPair, reason, sizeof(reason)) || !SameProp(watchEntity, watchReference, watchClass, watchModel, state, reason, sizeof(reason)))
    { StopWatch(reason, false); return Plugin_Stop; }
    float now = GetGameTime(), delta[3];
    bool known = previousPosition && now > previousTime;
    for (int axis = 0; axis < 3; ++axis) {
        if (known) delta[axis] = (state.position[axis] - previousPropPosition[axis]) / (now - previousTime);
        previousPropPosition[axis] = state.position[axis];
    }
    previousTime = now; previousPosition = true;
    LogState("watch", ++watchSamples, watchPair, state, known, delta);
    return Plugin_Continue;
}
public Action StopCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_PROP] usage: agpb_prop_watch_stop"); return Plugin_Handled; }
    StopWatch("explicit_observation_stop");
    PrintToServer("[AGPB_PROP] observation_stopped captured_restore_available=%d writes=0 automatic_restore=0", saved);
    return Plugin_Handled;
}
