// SPDX-License-Identifier: GPL-3.0-only
// Optional navigation-only regression fixture, never packaged with AgPB.
// Finite/active/argument/property helpers follow agpb_jump_fixture.sp.
// Preparation writes exactly two one-shot fake-client teleports. Sampling is
// read-only: no RunCmd hooks, player input/view writes, damage hooks, health,
// inventory, teams, waypoint changes, continuous placement or synthetic events.
// Caller must stop both core bots first and retain raw zero-command/route proof.
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
    name = "AgPB same-team player fixture", author = "AgPB",
    description = "Two-fake one-shot preparation and finite read-only navigation samples", version = "1.0"
};

Handle sampleTimer;
bool watching, mapEnding, preparedOrder;
int boundClients[3], userids[3], serials[3], coreIndices[2];
int preparedSlots[3], preparedUserids[3], preparedSerials[3], preparedMapSerial;
int mapSerial = 1, boundMapSerial, generation, sequence, samples, sampleLimit;
char epoch[40], boundMap[PLATFORM_MAX_PATH];

bool Finite(float value) { return (view_as<int>(value) & 0x7F800000) != 0x7F800000; }
bool FiniteVector(const float value[3]) { return Finite(value[0]) && Finite(value[1]) && Finite(value[2]); }
bool ClientValid(int client) { return client >= 1 && client <= MaxClients && IsClientInGame(client); }
bool ReadInt(int client, const char[] property, int &value) {
    if (HasEntProp(client, Prop_Send, property)) { value = GetEntProp(client, Prop_Send, property); return true; }
    if (HasEntProp(client, Prop_Data, property)) { value = GetEntProp(client, Prop_Data, property); return true; }
    return false;
}
bool ReadVector(int client, const char[] property, float value[3]) {
    if (HasEntProp(client, Prop_Send, property)) { GetEntPropVector(client, Prop_Send, property, value); return true; }
    if (HasEntProp(client, Prop_Data, property)) { GetEntPropVector(client, Prop_Data, property, value); return true; }
    return false;
}
bool ActivePlayer(int client) {
    if (!ClientValid(client) || !IsPlayerAlive(client) || GetClientTeam(client) != 3) return false;
    int state;
    return ReadInt(client, "m_iPlayerState", state) && state == 0;
}
bool IdentityMatches(int client, int userid, int serial) {
    return ClientValid(client) && GetClientUserId(client) == userid && GetClientSerial(client) == serial;
}
bool AgPBPlayer(int client) {
    if (!ActivePlayer(client) || !IsFakeClient(client) || IsClientSourceTV(client)) return false;
    char name[MAX_NAME_LENGTH]; GetClientName(client, name, sizeof(name));
    return StrContains(name, "AgPB_", true) == 0;
}
bool CheatsEnabled() {
    ConVar cheats = FindConVar("sv_cheats");
    return cheats != null && cheats.IntValue == 1;
}
bool SceneValid(int a, int b, int observer, char[] reason, int size) {
    if (mapEnding) { strcopy(reason, size, "map_ending"); return false; }
    if (a == b || a == observer || b == observer || !AgPBPlayer(a) || !AgPBPlayer(b))
    { strcopy(reason, size, "two_distinct_alive_active_AgPB_CT_fakes_required"); return false; }
    if (!ActivePlayer(observer) || IsFakeClient(observer) || IsClientSourceTV(observer) || observer >= a || observer >= b)
    { strcopy(reason, size, "lower_slot_alive_active_CT_human_required"); return false; }
    int count;
    for (int client = 1; client <= MaxClients; ++client) {
        if (!IsClientConnected(client) || !IsFakeClient(client)) continue;
        ++count;
        if (client != a && client != b) { strcopy(reason, size, "additional_fake_client"); return false; }
    }
    if (count != 2) { strcopy(reason, size, "exactly_two_fakes_required"); return false; }
    return true;
}
bool Argument(int index, char[] text, int size) {
    int written = GetCmdArg(index, text, size);
    return written > 0 && written < size - 1;
}
bool IntegerArgument(int index, int minimum, int maximum, int &value) {
    char text[32];
    if (!Argument(index, text, sizeof(text))) return false;
    value = 0;
    for (int position = 0; position < strlen(text); ++position) {
        int digit = text[position] - '0';
        if (digit < 0 || digit > 9 || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
        if (value > maximum) return false;
    }
    return value >= minimum;
}
bool FloatArgument(int index, float &value) {
    char text[128];
    return Argument(index, text, sizeof(text)) && StringToFloatEx(text, value) == strlen(text) && Finite(value);
}
void StopSampling(const char[] reason, bool cancelTimer = true) {
    if (cancelTimer && sampleTimer != null) delete sampleTimer;
    sampleTimer = null;
    if (!watching) return;
    watching = false;
    PrintToServer("[AGPB_PLAYERS_END] v=1 epoch=%s seq=%d generation=%d map_serial=%d map=%s tick=%d t=%.6f samples=%d limit=%d a_slot=%d a_userid=%d a_serial=%d b_slot=%d b_userid=%d b_serial=%d human_slot=%d human_userid=%d human_serial=%d team=3 reason=%s writes=0",
        epoch, ++sequence, generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(), samples, sampleLimit,
        boundClients[0], userids[0], serials[0], boundClients[1], userids[1], serials[1], boundClients[2], userids[2], serials[2], reason);
}
bool BindingValid(char[] reason, int size) {
    for (int i = 0; i < 3; ++i) {
        if (!IdentityMatches(boundClients[i], userids[i], serials[i]))
        { strcopy(reason, size, "identity_changed"); return false; }
    }
    char map[PLATFORM_MAX_PATH]; GetCurrentMap(map, sizeof(map));
    if (mapSerial != boundMapSerial || !StrEqual(map, boundMap))
    { strcopy(reason, size, "map_changed"); return false; }
    return SceneValid(boundClients[0], boundClients[1], boundClients[2], reason, size);
}
bool PreparedOrderValid(int observer) {
    if (!preparedOrder || preparedMapSerial != mapSerial || preparedSlots[2] != observer) return false;
    for (int i = 0; i < 3; ++i)
        if (!IdentityMatches(preparedSlots[i], preparedUserids[i], preparedSerials[i])) return false;
    return true;
}
public void OnPluginStart() {
    Format(epoch, sizeof(epoch), "%d_%08x", GetTime(), GetURandomInt());
    RegServerCmd("agpb_players_prepare", PrepareCommand);
    RegServerCmd("agpb_players_sample", SampleCommand);
    RegServerCmd("agpb_players_clear", ClearCommand);
    HookEventEx("player_death", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_spawn", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_team", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("round_start", RoundTransition, EventHookMode_PostNoCopy);
    HookEventEx("round_end", RoundTransition, EventHookMode_PostNoCopy);
    PrintToServer("[AGPB_PLAYERS] loaded server_console_only=1 preparation_once=1 navigation_only=1 human_writes=0");
}
public void OnMapStart() { StopSampling("map_start"); ++mapSerial; mapEnding = false; preparedOrder = false; }
public void OnMapEnd() { mapEnding = true; StopSampling("map_end"); preparedOrder = false; }
public void OnPluginEnd() { StopSampling("plugin_end"); }
public void OnClientDisconnect(int client) {
    if (watching && (client == boundClients[0] || client == boundClients[1] || client == boundClients[2])) StopSampling("bound_disconnect");
}
public void OnClientPutInServer(int client) {
    if (watching && IsFakeClient(client) && client != boundClients[0] && client != boundClients[1]) StopSampling("additional_fake_client");
}
public void OnGameFrame() {
    if (!watching) return;
    char reason[96];
    if (!BindingValid(reason, sizeof(reason))) StopSampling(reason);
}
public void PlayerLifecycle(Event event, const char[] name, bool dontBroadcast) {
    if (!watching) return;
    int userid = event.GetInt("userid", 0);
    for (int i = 0; i < 3; ++i) if (userid == userids[i]) { StopSampling(name); return; }
}
public void RoundTransition(Event event, const char[] name, bool dontBroadcast) { StopSampling(name); }

public Action PrepareCommand(int args) {
    if (args != 11) {
        PrintToServer("[AGPB_PLAYERS] usage: agpb_players_prepare <slotA> <slotB> <human> ax ay az ayaw bx by bz byaw");
        return Plugin_Handled;
    }
    if (!CheatsEnabled()) { PrintToServer("[AGPB_PLAYERS] rejected: sv_cheats_must_equal_1"); return Plugin_Handled; }
    int selected[3];
    for (int i = 0; i < 3; ++i) if (!IntegerArgument(i + 1, 1, MaxClients, selected[i]))
    { PrintToServer("[AGPB_PLAYERS] rejected: invalid_slot"); return Plugin_Handled; }
    char reason[96];
    if (!SceneValid(selected[0], selected[1], selected[2], reason, sizeof(reason)))
    { PrintToServer("[AGPB_PLAYERS] rejected: %s", reason); return Plugin_Handled; }
    float positions[2][3], eyes[2][3], zero[3], velocity[3];
    for (int bot = 0; bot < 2; ++bot) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!FloatArgument(4 + bot * 4 + axis, positions[bot][axis]) || FloatAbs(positions[bot][axis]) > 16384.0)
            { PrintToServer("[AGPB_PLAYERS] rejected: finite_coordinates_within_16384_required"); return Plugin_Handled; }
        }
        if (!FloatArgument(7 + bot * 4, eyes[bot][1]))
        { PrintToServer("[AGPB_PLAYERS] rejected: finite_yaw_required"); return Plugin_Handled; }
        if (GetClientButtons(selected[bot]) != 0 || !ReadVector(selected[bot], "m_vecVelocity", velocity) ||
            !FiniteVector(velocity) || GetVectorLength(velocity) > 5.0)
        { PrintToServer("[AGPB_PLAYERS] rejected: both_fakes_require_zero_buttons_and_speed_at_most_5"); return Plugin_Handled; }
    }
    StopSampling("preparation_replaced");
    ++generation;
    preparedOrder = true; preparedMapSerial = mapSerial;
    char map[PLATFORM_MAX_PATH]; GetCurrentMap(map, sizeof(map));
    for (int i = 0; i < 3; ++i) {
        preparedSlots[i] = selected[i]; preparedUserids[i] = GetClientUserId(selected[i]); preparedSerials[i] = GetClientSerial(selected[i]);
    }
    // The only two writes in this fixture. There is no timer/frame placement.
    TeleportEntity(selected[0], positions[0], eyes[0], zero);
    TeleportEntity(selected[1], positions[1], eyes[1], zero);
    PrintToServer("[AGPB_PLAYERS_PREPARE] v=1 epoch=%s seq=%d generation=%d map_serial=%d map=%s tick=%d t=%.6f a_slot=%d a_userid=%d a_serial=%d b_slot=%d b_userid=%d b_serial=%d human_slot=%d human_userid=%d human_serial=%d team=3 alive_active=1 human_fake=0 a_requested=(%.4f,%.4f,%.4f) a_yaw=%.4f b_requested=(%.4f,%.4f,%.4f) b_yaw=%.4f velocity=(0,0,0) writes=two_preparation_teleports human_writes=0 core_stop_requires_raw_verification=1",
        epoch, ++sequence, generation, mapSerial, map, GetGameTickCount(), GetGameTime(),
        preparedSlots[0], preparedUserids[0], preparedSerials[0], preparedSlots[1], preparedUserids[1], preparedSerials[1],
        preparedSlots[2], preparedUserids[2], preparedSerials[2], positions[0][0], positions[0][1], positions[0][2], eyes[0][1],
        positions[1][0], positions[1][1], positions[1][2], eyes[1][1]);
    return Plugin_Handled;
}
public Action SampleCommand(int args) {
    if (args != 5) {
        PrintToServer("[AGPB_PLAYERS] usage: agpb_players_sample <coreIdxA> <coreIdxB> <humanSlot> <count:1..160> <interval:.05..1>");
        return Plugin_Handled;
    }
    int coreA, coreB, observer, limit;
    float interval;
    if (!IntegerArgument(1, 0, MaxClients - 1, coreA) || !IntegerArgument(2, 0, MaxClients - 1, coreB) || coreA == coreB ||
        !IntegerArgument(3, 1, MaxClients, observer) || !IntegerArgument(4, 1, 160, limit) ||
        !FloatArgument(5, interval) || interval < 0.05 || interval > 1.0)
    { PrintToServer("[AGPB_PLAYERS] rejected: invalid_sample_arguments"); return Plugin_Handled; }
    int a, b;
    bool usePrepared = PreparedOrderValid(observer);
    if (usePrepared) { a = preparedSlots[0]; b = preparedSlots[1]; }
    else {
        for (int client = 1; client <= MaxClients; ++client) {
            if (!ClientValid(client) || !IsFakeClient(client)) continue;
            if (a == 0) a = client;
            else if (b == 0) b = client;
        }
    }
    char reason[96];
    if (!SceneValid(a, b, observer, reason, sizeof(reason)))
    { PrintToServer("[AGPB_PLAYERS] rejected: %s", reason); return Plugin_Handled; }
    StopSampling("replaced");
    boundClients[0] = a; boundClients[1] = b; boundClients[2] = observer;
    for (int i = 0; i < 3; ++i) { userids[i] = GetClientUserId(boundClients[i]); serials[i] = GetClientSerial(boundClients[i]); }
    coreIndices[0] = coreA; coreIndices[1] = coreB;
    boundMapSerial = mapSerial; GetCurrentMap(boundMap, sizeof(boundMap));
    ++generation; samples = 0; sampleLimit = limit; watching = true;
    sampleTimer = CreateTimer(interval, SampleTimer, generation, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
    if (sampleTimer == null) { StopSampling("timer_creation_failed"); return Plugin_Handled; }
    PrintToServer("[AGPB_PLAYERS_ARM] v=1 epoch=%s seq=%d generation=%d map_serial=%d map=%s tick=%d t=%.6f a_slot=%d a_userid=%d a_serial=%d b_slot=%d b_userid=%d b_serial=%d human_slot=%d human_userid=%d human_serial=%d team=3 core_a=%d core_b=%d count=%d interval=%.4f prepared_order=%d core_mapping_requires_raw_verification=1 writes=0",
        epoch, ++sequence, generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(),
        boundClients[0], userids[0], serials[0], boundClients[1], userids[1], serials[1], boundClients[2], userids[2], serials[2],
        coreA, coreB, limit, interval, usePrepared ? 1 : 0);
    return Plugin_Handled;
}
public Action SampleTimer(Handle timer, any token) {
    if (!watching || timer != sampleTimer || token != generation) return Plugin_Stop;
    char reason[96];
    if (!BindingValid(reason, sizeof(reason))) { StopSampling(reason, false); return Plugin_Stop; }
    float positions[3][3], velocities[3][3], eyes[3][3];
    int health[3], states[3];
    for (int i = 0; i < 3; ++i) {
        GetClientAbsOrigin(boundClients[i], positions[i]); GetClientEyeAngles(boundClients[i], eyes[i]);
        if (!ReadVector(boundClients[i], "m_vecVelocity", velocities[i]) || !FiniteVector(positions[i]) ||
            !FiniteVector(velocities[i]) || !FiniteVector(eyes[i]) || !ReadInt(boundClients[i], "m_iHealth", health[i]) ||
            !ReadInt(boundClients[i], "m_iPlayerState", states[i]))
        { StopSampling("required_finite_readback_unavailable", false); return Plugin_Stop; }
    }
    ++samples;
    PrintToServer("[AGPB_PLAYERS_SAMPLE] v=1 epoch=%s seq=%d generation=%d map_serial=%d map=%s tick=%d t=%.6f sample=%d limit=%d core_a=%d core_b=%d writes=0",
        epoch, ++sequence, generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(), samples, sampleLimit, coreIndices[0], coreIndices[1]);
    for (int i = 0; i < 3; ++i) {
        char role[8];
        if (i == 0) strcopy(role, sizeof(role), "A");
        else if (i == 1) strcopy(role, sizeof(role), "B");
        else strcopy(role, sizeof(role), "human");
        PrintToServer("[AGPB_PLAYERS_PLAYER] v=1 epoch=%s seq=%d generation=%d map_serial=%d map=%s tick=%d t=%.6f sample=%d role=%s slot=%d userid=%d serial=%d team=%d fake=%d alive=%d state=%d hp=%d pos=(%.4f,%.4f,%.4f) velocity=(%.4f,%.4f,%.4f) eye=(%.4f,%.4f,%.4f) buttons=0x%X flags=0x%X writes=0",
            epoch, ++sequence, generation, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(), samples, role,
            boundClients[i], userids[i], serials[i], GetClientTeam(boundClients[i]), IsFakeClient(boundClients[i]) ? 1 : 0, IsPlayerAlive(boundClients[i]) ? 1 : 0,
            states[i], health[i], positions[i][0], positions[i][1], positions[i][2], velocities[i][0], velocities[i][1], velocities[i][2],
            eyes[i][0], eyes[i][1], eyes[i][2], GetClientButtons(boundClients[i]), GetEntityFlags(boundClients[i]));
    }
    // Only read-only commands. Indices are strict integers; this cannot submit
    // movement, task changes, client commands or test preparation implicitly.
    ServerCommand("agpb_bot_info %d;agpb_bot_info %d;sm_agpb_guard_status", coreIndices[0], coreIndices[1]);
    ServerExecute();
    if (samples >= sampleLimit) { StopSampling("sample_limit", false); return Plugin_Stop; }
    return Plugin_Continue;
}
public Action ClearCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_PLAYERS] usage: agpb_players_clear"); return Plugin_Handled; }
    StopSampling("clear"); preparedOrder = false; ++generation;
    PrintToServer("[AGPB_PLAYERS] cleared writes=0 restoration_writes=0 positions_left_unchanged=1 human_writes=0");
    return Plugin_Handled;
}
