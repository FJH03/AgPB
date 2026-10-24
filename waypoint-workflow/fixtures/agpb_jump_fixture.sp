// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

// Test-only, one-shot start-position preparation. Never package with AgPB.
// The caller must first stop the local bot and verify zero input / zero route.
// This fixture neither issues bot inputs nor changes health, equipment, gravity,
// ground state, waypoint data or the selected route. Only TeleportEntity writes.
public Plugin myinfo = {
    name = "AgPB jump start fixture",
    author = "AgPB",
    description = "Cheat-gated one-shot start placement with bound frame readback",
    version = "1.0"
};

bool pending, mapEnding;
int subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial;
int boundTeam, mapSerial, boundMapSerial, generation, frameNumber;
char boundMap[PLATFORM_MAX_PATH];

bool Finite(float value)
{
    return (view_as<int>(value) & 0x7F800000) != 0x7F800000;
}

bool FiniteVector(const float value[3])
{
    return Finite(value[0]) && Finite(value[1]) && Finite(value[2]);
}

bool ClientValid(int client)
{
    return client >= 1 && client <= MaxClients && IsClientInGame(client);
}

bool ReadInt(int client, const char[] property, int &value)
{
    if (HasEntProp(client, Prop_Send, property)) { value = GetEntProp(client, Prop_Send, property); return true; }
    if (HasEntProp(client, Prop_Data, property)) { value = GetEntProp(client, Prop_Data, property); return true; }
    return false;
}

bool ReadVector(int client, const char[] property, float value[3])
{
    if (HasEntProp(client, Prop_Send, property)) { GetEntPropVector(client, Prop_Send, property, value); return true; }
    if (HasEntProp(client, Prop_Data, property)) { GetEntPropVector(client, Prop_Data, property, value); return true; }
    return false;
}

bool ReadGround(int client, int &entity, int &reference)
{
    if (HasEntProp(client, Prop_Send, "m_hGroundEntity"))
        entity = GetEntPropEnt(client, Prop_Send, "m_hGroundEntity");
    else if (HasEntProp(client, Prop_Data, "m_hGroundEntity"))
        entity = GetEntPropEnt(client, Prop_Data, "m_hGroundEntity");
    else return false;
    reference = entity >= 0 && IsValidEntity(entity) ? EntIndexToEntRef(entity) : INVALID_ENT_REFERENCE;
    return true;
}

bool CheatsEnabled()
{
    ConVar cheats = FindConVar("sv_cheats");
    return cheats != null && cheats.IntValue == 1;
}

bool ActivePlayer(int client)
{
    if (!ClientValid(client) || !IsPlayerAlive(client) || GetClientTeam(client) < 2 || GetClientTeam(client) > 3)
        return false;
    int state;
    return ReadInt(client, "m_iPlayerState", state) && state == 0;
}

bool SetupValid(int bot, int observer, char[] reason, int size)
{
    if (!CheatsEnabled()) { strcopy(reason, size, "sv_cheats_must_equal_1"); return false; }
    if (mapEnding) { strcopy(reason, size, "map_ending"); return false; }
    if (!ActivePlayer(bot) || !IsFakeClient(bot) || IsClientSourceTV(bot))
    { strcopy(reason, size, "subject_not_alive_active_fake"); return false; }
    char name[MAX_NAME_LENGTH]; GetClientName(bot, name, sizeof(name));
    if (StrContains(name, "AgPB_", true) != 0)
    { strcopy(reason, size, "subject_requires_AgPB_name_prefix"); return false; }
    if (!ActivePlayer(observer) || IsFakeClient(observer) || IsClientSourceTV(observer) || observer >= bot)
    { strcopy(reason, size, "lower_slot_alive_active_human_required"); return false; }
    if (GetClientTeam(bot) != GetClientTeam(observer))
    { strcopy(reason, size, "team_mismatch"); return false; }
    int fakeCount;
    for (int client = 1; client <= MaxClients; ++client)
    {
        if (!IsClientConnected(client) || !IsFakeClient(client)) continue;
        ++fakeCount;
        if (client != bot) { strcopy(reason, size, "additional_fake_client"); return false; }
    }
    if (fakeCount != 1) { strcopy(reason, size, "exactly_one_fake_required"); return false; }
    if ((GetClientButtons(bot) & (IN_ATTACK | IN_ATTACK2 | IN_JUMP)) != 0)
    { strcopy(reason, size, "attack_or_jump_input_not_released"); return false; }
    return true;
}

bool IdentityMatches(int client, int userid, int serial)
{
    return ClientValid(client) && GetClientUserId(client) == userid && GetClientSerial(client) == serial;
}

bool BindingValid(char[] reason, int size)
{
    if (!IdentityMatches(subject, subjectUserid, subjectSerial) || !IdentityMatches(human, humanUserid, humanSerial))
    { strcopy(reason, size, "identity_changed"); return false; }
    char map[PLATFORM_MAX_PATH]; GetCurrentMap(map, sizeof(map));
    if (mapSerial != boundMapSerial || !StrEqual(map, boundMap))
    { strcopy(reason, size, "map_changed"); return false; }
    if (!SetupValid(subject, human, reason, size)) return false;
    if (GetClientTeam(subject) != boundTeam || GetClientTeam(human) != boundTeam)
    { strcopy(reason, size, "bound_team_changed"); return false; }
    return true;
}

void CancelPrepare(const char[] reason)
{
    if (!pending) return;
    pending = false;
    PrintToServer("[AGPB_JUMP_PREPARE] v=1 action=cancelled generation=%d map_serial=%d map=%s bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d frame=%d reason=%s",
        generation, boundMapSerial, boundMap, subject, subjectUserid, subjectSerial,
        human, humanUserid, humanSerial, boundTeam, frameNumber, reason);
}

bool Argument(int index, char[] text, int size)
{
    int written = GetCmdArg(index, text, size);
    return written > 0 && written < size - 1;
}

bool SlotArgument(int index, int &slot)
{
    char text[32];
    if (!Argument(index, text, sizeof(text))) return false;
    slot = 0;
    for (int position = 0; position < strlen(text); ++position)
    {
        if (text[position] < '0' || text[position] > '9') return false;
        slot = slot * 10 + text[position] - '0';
        if (slot > MaxClients) return false;
    }
    return slot >= 1;
}

bool FloatArgument(int index, float &value)
{
    char text[128];
    return Argument(index, text, sizeof(text)) && StringToFloatEx(text, value) == strlen(text) && Finite(value);
}

public void OnPluginStart()
{
    RegConsoleCmd("agpb_jump_prepare", PrepareCommand);
    HookEventEx("player_death", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_spawn", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_team", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("round_start", RoundTransition, EventHookMode_Post);
    HookEventEx("round_end", RoundTransition, EventHookMode_Post);
    PrintToServer("[AGPB_JUMP_FIXTURE] loaded one_shot=1 server_console_only=1 sv_cheats_required=1");
}

public void OnMapStart() { CancelPrepare("map_start"); ++mapSerial; mapEnding = false; }
public void OnMapEnd() { mapEnding = true; CancelPrepare("map_end"); }
public void OnPluginEnd() { CancelPrepare("plugin_end"); }

public void OnClientDisconnect(int client)
{
    if (pending && (client == subject || client == human)) CancelPrepare("bound_disconnect");
}

public void OnClientPutInServer(int client)
{
    if (pending && client != subject && IsFakeClient(client)) CancelPrepare("additional_fake_client");
}

public Action PrepareCommand(int client, int args)
{
    if (client != 0) { ReplyToCommand(client, "[AGPB_JUMP_FIXTURE] Server console only."); return Plugin_Handled; }
    if (!CheatsEnabled()) { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: sv_cheats_must_equal_1"); return Plugin_Handled; }
    if (args != 5 && args != 6)
    { PrintToServer("[AGPB_JUMP_FIXTURE] usage: agpb_jump_prepare <bot_slot> <human_slot> <x> <y> <z> [yaw]"); return Plugin_Handled; }
    int bot, observer;
    if (!SlotArgument(1, bot) || !SlotArgument(2, observer))
    { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: invalid_slot"); return Plugin_Handled; }
    float origin[3], angles[3], zero[3], velocity[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!FloatArgument(axis + 3, origin[axis]) || origin[axis] < -16384.0 || origin[axis] > 16384.0)
        { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: coordinates_require_finite_values_within_16384"); return Plugin_Handled; }
    }
    char reason[96];
    if (!SetupValid(bot, observer, reason, sizeof(reason)))
    { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: %s", reason); return Plugin_Handled; }
    GetClientEyeAngles(bot, angles);
    if (args == 6 && !FloatArgument(6, angles[1]))
    { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: invalid_yaw"); return Plugin_Handled; }
    int ground, groundReference;
    if (!FiniteVector(angles) || !ReadVector(bot, "m_vecVelocity", velocity) || !FiniteVector(velocity) ||
        !ReadGround(bot, ground, groundReference))
    { PrintToServer("[AGPB_JUMP_FIXTURE] rejected: required_finite_state_unavailable"); return Plugin_Handled; }
    CancelPrepare("replaced");
    subject = bot; subjectUserid = GetClientUserId(bot); subjectSerial = GetClientSerial(bot);
    human = observer; humanUserid = GetClientUserId(observer); humanSerial = GetClientSerial(observer);
    boundTeam = GetClientTeam(bot); boundMapSerial = mapSerial; GetCurrentMap(boundMap, sizeof(boundMap));
    ++generation; frameNumber = 0; pending = true;
    // One explicit start placement. Subsequent frames are observation only.
    TeleportEntity(subject, origin, angles, zero);
    PrintToServer("[AGPB_JUMP_PREPARE] v=1 action=placed generation=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d requested_origin=(%.4f,%.4f,%.4f) requested_eye=(%.4f,%.4f,%.4f) requested_velocity=(0,0,0) writes=one_teleport",
        generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(), subject, subjectUserid, subjectSerial,
        human, humanUserid, humanSerial, boundTeam, origin[0], origin[1], origin[2], angles[0], angles[1], angles[2]);
    RequestFrame(ReportFrame, generation);
    return Plugin_Handled;
}

public void ReportFrame(any token)
{
    if (!pending || token != generation) return;
    char reason[96];
    if (!BindingValid(reason, sizeof(reason))) { CancelPrepare(reason); return; }
    float origin[3], velocity[3], eye[3];
    GetClientAbsOrigin(subject, origin); GetClientEyeAngles(subject, eye);
    int ground, groundReference;
    if (!ReadVector(subject, "m_vecVelocity", velocity) || !ReadGround(subject, ground, groundReference) ||
        !FiniteVector(origin) || !FiniteVector(velocity) || !FiniteVector(eye))
    { CancelPrepare("readback_state_unavailable_or_nonfinite"); return; }
    ++frameNumber;
    PrintToServer("[AGPB_JUMP_PREPARE] v=1 action=readback generation=%d map_serial=%d map=%s tick=%d t=%.6f frame=%d bot_slot=%d bot_userid=%d bot_serial=%d bot_fake=1 bot_alive=1 human_slot=%d human_userid=%d human_serial=%d human_fake=0 human_alive=1 team=%d origin=(%.4f,%.4f,%.4f) velocity=(%.4f,%.4f,%.4f) eye=(%.4f,%.4f,%.4f) buttons=0x%X flags=0x%X ground_entity=%d ground_ref=%d writes=0 final=%d",
        generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(), frameNumber,
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam,
        origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2], eye[0], eye[1], eye[2],
        GetClientButtons(subject), GetEntityFlags(subject), ground, groundReference, frameNumber == 2 ? 1 : 0);
    if (frameNumber >= 2) pending = false;
    else RequestFrame(ReportFrame, generation);
}

public void PlayerLifecycle(Event event, const char[] name, bool dontBroadcast)
{
    if (!pending) return;
    int userid = event.GetInt("userid", 0);
    if (userid == subjectUserid || userid == humanUserid) CancelPrepare(name);
}

public void RoundTransition(Event event, const char[] name, bool dontBroadcast)
{
    CancelPrepare(name);
}
