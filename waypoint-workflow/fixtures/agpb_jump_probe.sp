// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#pragma semicolon 1
#pragma newdecls required

// Read-only, explicitly armed observation. No property/input/position writes,
// client/entity creation, damage, commands to clients or synthetic game events.
public Plugin myinfo = {
    name = "AgPB read-only jump probe",
    author = "AgPB",
    description = "Bound PostThink jump velocity, posture and native event observations",
    version = "1.3"
};

#define SAMPLE_LIMIT 512

bool watching, subjectHooked, jumpEventHooked, previousSample;
int subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam;
int mapSerial, boundMapSerial, generation, sequence, samples, jumps, lastTick, previousFlags;
float deadline;
char epoch[40], boundMap[PLATFORM_MAX_PATH];
int jumpSweepSequence;

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

bool ReadFloat(int client, const char[] property, float &value)
{
    if (HasEntProp(client, Prop_Send, property)) { value = GetEntPropFloat(client, Prop_Send, property); return true; }
    if (HasEntProp(client, Prop_Data, property)) { value = GetEntPropFloat(client, Prop_Data, property); return true; }
    return false;
}

bool ReadVector(int client, const char[] property, float value[3])
{
    if (HasEntProp(client, Prop_Send, property)) { GetEntPropVector(client, Prop_Send, property, value); return true; }
    if (HasEntProp(client, Prop_Data, property)) { GetEntPropVector(client, Prop_Data, property, value); return true; }
    return false;
}

// Keep the raw EHANDLE separate from SourceMod's resolved entity/EntRef.
// Offsets are resolved from public property metadata; no fixed layout is used.
bool ReadHandle(int client, const char[] property, int &entity, int &handle)
{
    int offset = -1;
    if (HasEntProp(client, Prop_Send, property))
    {
        offset = GetEntSendPropOffs(client, property, true);
        entity = GetEntPropEnt(client, Prop_Send, property);
    }
    else if (HasEntProp(client, Prop_Data, property))
    {
        offset = FindDataMapInfo(client, property);
        entity = GetEntPropEnt(client, Prop_Data, property);
    }
    if (offset <= 0) return false;
    handle = GetEntData(client, offset, 4);
    return true;
}

bool Finite(float value)
{
    return (view_as<int>(value) & 0x7F800000) != 0x7F800000;
}

bool FiniteVector(const float value[3])
{
    return Finite(value[0]) && Finite(value[1]) && Finite(value[2]);
}

bool ActivePlayer(int client)
{
    if (!ClientValid(client) || !IsPlayerAlive(client) || GetClientTeam(client) < 2) return false;
    int state;
    return ReadInt(client, "m_iPlayerState", state) && state == 0;
}

bool IdentityMatches(int client, int userid, int serial)
{
    return ClientValid(client) && GetClientUserId(client) == userid && GetClientSerial(client) == serial;
}

bool SetupValid(int bot, int observer, char[] reason, int size)
{
    if (!ActivePlayer(bot) || !IsFakeClient(bot)) { strcopy(reason, size, "subject_not_alive_active_fake"); return false; }
    if (!ActivePlayer(observer) || IsFakeClient(observer) || IsClientSourceTV(observer) || observer >= bot)
    { strcopy(reason, size, "lower_slot_alive_active_human_required"); return false; }
    if (GetClientTeam(bot) != GetClientTeam(observer)) { strcopy(reason, size, "team_mismatch"); return false; }
    int fakeCount;
    for (int client = 1; client <= MaxClients; ++client)
    {
        if (IsClientConnected(client) && IsFakeClient(client))
        {
            ++fakeCount;
            if (client != bot) { strcopy(reason, size, "additional_fake_client"); return false; }
        }
    }
    if (fakeCount != 1) { strcopy(reason, size, "exactly_one_fake_required"); return false; }
    return true;
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

void StopWatch(const char[] reason)
{
    if (!watching) return;
    watching = false;
    if (subjectHooked && IdentityMatches(subject, subjectUserid, subjectSerial))
        SDKUnhook(subject, SDKHook_PostThinkPost, SubjectPostThink);
    subjectHooked = false;
    PrintToServer("[AGPB_JUMP_END] v=1 epoch=%s generation=%d seq=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d reason=%s samples=%d jumps=%d",
        epoch, generation, ++sequence, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(),
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam, reason, samples, jumps);
}

bool CheckWatch()
{
    if (!watching) return false;
    char reason[96];
    if (!BindingValid(reason, sizeof(reason))) { StopWatch(reason); return false; }
    if (GetGameTime() >= deadline) { StopWatch("deadline"); return false; }
    if (samples >= SAMPLE_LIMIT) { StopWatch("sample_limit"); return false; }
    return true;
}

public void OnPluginStart()
{
    Format(epoch, sizeof(epoch), "%d_%d", GetTime(), GetGameTickCount());
    RegConsoleCmd("agpb_jump_probe_arm", ArmCommand);
    RegConsoleCmd("agpb_jump_probe_clear", ClearCommand);
    RegConsoleCmd("agpb_jump_probe_sweep", JumpSweepCommand);
    jumpEventHooked = HookEventEx("player_jump", PlayerJump, EventHookMode_Post);
    HookEventEx("player_death", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_team", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("player_spawn", PlayerLifecycle, EventHookMode_Post);
    HookEventEx("round_start", RoundTransition, EventHookMode_Post);
    HookEventEx("round_end", RoundTransition, EventHookMode_Post);
    PrintToServer("[AGPB_JUMP_PROBE] loaded epoch=%s player_jump_hooked=%d read_only=1", epoch, jumpEventHooked ? 1 : 0);
}

public void OnMapStart() { StopWatch("map_start"); ++mapSerial; }
public void OnMapEnd() { StopWatch("map_end"); }
public void OnPluginEnd() { StopWatch("plugin_end"); }
public void OnGameFrame() { CheckWatch(); }

public void OnClientDisconnect(int client)
{
    if (watching && (client == subject || client == human)) StopWatch("bound_disconnect");
}

public void OnClientPutInServer(int client)
{
    if (watching && client != subject && IsFakeClient(client)) StopWatch("additional_fake_client");
}

public Action ArmCommand(int client, int args)
{
    if (client != 0) { ReplyToCommand(client, "[AGPB_JUMP_PROBE] Server console only."); return Plugin_Handled; }
    if (args < 2 || args > 3)
    { PrintToServer("[AGPB_JUMP_PROBE] usage: agpb_jump_probe_arm <bot_slot> <human_slot> [seconds=5]"); return Plugin_Handled; }
    char text[64], reason[96];
    int bot, observer;
    GetCmdArg(1, text, sizeof(text));
    if (strlen(text) == 0 || StringToIntEx(text, bot) != strlen(text)) { PrintToServer("[AGPB_JUMP_PROBE] rejected: invalid_bot_slot"); return Plugin_Handled; }
    GetCmdArg(2, text, sizeof(text));
    if (strlen(text) == 0 || StringToIntEx(text, observer) != strlen(text)) { PrintToServer("[AGPB_JUMP_PROBE] rejected: invalid_human_slot"); return Plugin_Handled; }
    float seconds = 5.0;
    if (args == 3)
    {
        GetCmdArg(3, text, sizeof(text));
        if (strlen(text) == 0 || StringToFloatEx(text, seconds) != strlen(text))
        { PrintToServer("[AGPB_JUMP_PROBE] rejected: invalid_seconds"); return Plugin_Handled; }
    }
    if (!Finite(seconds) || seconds <= 0.0 || seconds > 10.0)
    { PrintToServer("[AGPB_JUMP_PROBE] rejected: seconds_must_be_within_0_exclusive_and_10_inclusive"); return Plugin_Handled; }
    if (!jumpEventHooked) { PrintToServer("[AGPB_JUMP_PROBE] rejected: native_player_jump_event_unavailable"); return Plugin_Handled; }
    if (!SetupValid(bot, observer, reason, sizeof(reason)))
    { PrintToServer("[AGPB_JUMP_PROBE] rejected: %s", reason); return Plugin_Handled; }
    float velocity[3], base[3], maxspeed;
    int flags;
    if (!ReadVector(bot, "m_vecVelocity", velocity) || !ReadVector(bot, "m_vecBaseVelocity", base) ||
        !ReadFloat(bot, "m_flMaxspeed", maxspeed) || !ReadInt(bot, "m_fFlags", flags))
    { PrintToServer("[AGPB_JUMP_PROBE] rejected: required_properties_unavailable"); return Plugin_Handled; }
    StopWatch("rearmed");
    subject = bot; subjectUserid = GetClientUserId(bot); subjectSerial = GetClientSerial(bot);
    human = observer; humanUserid = GetClientUserId(observer); humanSerial = GetClientSerial(observer);
    boundTeam = GetClientTeam(bot); boundMapSerial = mapSerial; GetCurrentMap(boundMap, sizeof(boundMap));
    ++generation; samples = 0; jumps = 0; lastTick = -1; previousSample = false;
    deadline = GetGameTime() + seconds;
    watching = true;
    subjectHooked = SDKHookEx(subject, SDKHook_PostThinkPost, SubjectPostThink);
    if (!subjectHooked) { StopWatch("postthink_hook_failed"); return Plugin_Handled; }
    PrintToServer("[AGPB_JUMP_ARM] v=1 epoch=%s generation=%d seq=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d seconds=%.3f deadline=%.6f limit=%d phase=postthink_post read_only=1",
        epoch, generation, ++sequence, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(),
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam, seconds, deadline, SAMPLE_LIMIT);
    return Plugin_Handled;
}

public Action ClearCommand(int client, int args)
{
    if (client != 0) { ReplyToCommand(client, "[AGPB_JUMP_PROBE] Server console only."); return Plugin_Handled; }
    if (watching) StopWatch("cleared");
    else PrintToServer("[AGPB_JUMP_PROBE] idle epoch=%s generation=%d", epoch, generation);
    return Plugin_Handled;
}

public void SubjectPostThink(int client)
{
    if (!watching || client != subject || !CheckWatch()) return;
    int tick = GetGameTickCount();
    if (tick == lastTick) return;
    float origin[3], velocity[3], view[3], base[3], maxspeed;
    int flags;
    GetClientAbsOrigin(client, origin); GetClientEyeAngles(client, view);
    if (!ReadVector(client, "m_vecVelocity", velocity) || !ReadVector(client, "m_vecBaseVelocity", base) ||
        !ReadFloat(client, "m_flMaxspeed", maxspeed) || !ReadInt(client, "m_fFlags", flags))
    { StopWatch("sample_property_unavailable"); return; }
    if (!FiniteVector(origin) || !FiniteVector(velocity) || !FiniteVector(view) || !FiniteVector(base) || !Finite(maxspeed))
    { StopWatch("sample_nonfinite"); return; }
    int groundEntity = -1, groundHandle = -1, ladderEntity = -1, ladderHandle = -1;
    bool groundKnown = ReadHandle(client, "m_hGroundEntity", groundEntity, groundHandle);
    bool ladderKnown = ReadHandle(client, "m_hLadder", ladderEntity, ladderHandle);
    float ladderNormal[3];
    bool ladderNormalKnown = ReadVector(client, "m_vecLadderNormal", ladderNormal) && FiniteVector(ladderNormal);
    lastTick = tick; ++samples;
    PrintToServer("[AGPB_JUMP_SAMPLE] v=1 epoch=%s generation=%d seq=%d map_serial=%d map=%s tick=%d t=%.6f sample=%d phase=postthink_post bot_slot=%d bot_userid=%d bot_serial=%d bot_fake=1 bot_alive=1 human_slot=%d human_userid=%d human_serial=%d human_fake=0 human_alive=1 team=%d pos=(%.4f,%.4f,%.4f) velocity=(%.4f,%.4f,%.4f) view=(%.4f,%.4f,%.4f) buttons=0x%X flags=0x%X maxspeed=%.4f base=(%.4f,%.4f,%.4f) jumps=%d movetype=%d ground_known=%d ground_ent=%d ground_handle=0x%X ladder_known=%d ladder_ent=%d ladder_handle=0x%X ladder_normal_known=%d ladder_normal=(%.4f,%.4f,%.4f)",
        epoch, generation, ++sequence, boundMapSerial, boundMap, tick, GetGameTime(), samples,
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam,
        origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2], view[0], view[1], view[2],
        GetClientButtons(client), flags, maxspeed, base[0], base[1], base[2], jumps,
        view_as<int>(GetEntityMoveType(client)), groundKnown ? 1 : 0, groundEntity, groundHandle,
        ladderKnown ? 1 : 0, ladderEntity, ladderHandle, ladderNormalKnown ? 1 : 0,
        ladderNormal[0], ladderNormal[1], ladderNormal[2]);
    if (previousSample && (previousFlags & FL_ONGROUND) != 0 && (flags & FL_ONGROUND) == 0)
        Transition("airborne", tick);
    else if (previousSample && (previousFlags & FL_ONGROUND) == 0 && (flags & FL_ONGROUND) != 0)
        Transition("landing", tick);
    previousFlags = flags; previousSample = true;
    if (samples >= SAMPLE_LIMIT) StopWatch("sample_limit");
}

void Transition(const char[] name, int tick)
{
    PrintToServer("[AGPB_JUMP_TRANSITION] v=1 epoch=%s generation=%d seq=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d kind=%s source=postthink_flags sample=%d",
        epoch, generation, ++sequence, boundMapSerial, boundMap, tick, GetGameTime(),
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam, name, samples);
}

public void PlayerJump(Event event, const char[] name, bool dontBroadcast)
{
    if (!watching || event.GetInt("userid", 0) != subjectUserid || !CheckWatch()) return;
    ++jumps;
    PrintToServer("[AGPB_JUMP_EVENT] v=1 epoch=%s generation=%d seq=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d human_slot=%d human_userid=%d human_serial=%d team=%d event=player_jump source=native_event jumps=%d",
        epoch, generation, ++sequence, boundMapSerial, boundMap, GetGameTickCount(), GetGameTime(),
        subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial, boundTeam, jumps);
}

public void PlayerLifecycle(Event event, const char[] name, bool dontBroadcast)
{
    if (!watching) return;
    int userid = event.GetInt("userid", 0);
    if (userid == subjectUserid || userid == humanUserid) StopWatch(name);
}

public void RoundTransition(Event event, const char[] name, bool dontBroadcast)
{
    StopWatch(name);
}

// Independent, read-only geometry diagnostics. These commands never arm,
// clear, sample or advance the existing observation sequence.
bool JumpSweepCoordinate(float value)
{
    return Finite(value) && value >= -16384.0 && value <= 16384.0;
}

bool JumpSweepFloatArgument(int index, float &value)
{
    char text[128];
    int written = GetCmdArg(index, text, sizeof(text));
    return written > 0 && written < sizeof(text) - 1 &&
        StringToFloatEx(text, value) == strlen(text) && JumpSweepCoordinate(value);
}

bool JumpSweepSlotArgument(int &slot)
{
    char text[32];
    int written = GetCmdArg(1, text, sizeof(text));
    if (written <= 0 || written >= sizeof(text) - 1) return false;
    slot = 0;
    for (int position = 0; position < strlen(text); ++position)
    {
        if (text[position] < '0' || text[position] > '9') return false;
        slot = slot * 10 + text[position] - '0';
        if (slot > MaxClients) return false;
    }
    return slot >= 1;
}

public bool JumpSweepIgnoreSelf(int entity, int contentsMask, any data)
{
    return entity != data;
}

void JumpSweepLogResult(Handle trace, int bot, int sweep, const char[] kind, int heightPercent,
    const float start[3], const float target[3], const float mins[3], const float maxs[3],
    const char[] hullSource, float requestedHeight, float actualPlayerHeight)
{
    if (trace == null)
    {
        PrintToServer("[AGPB_JUMP_SWEEP] v=1 epoch=%s sweep=%d kind=%s height_pct=%d error=trace_unavailable read_only=1 hull_source=%s requested_height=%.6f actual_player_height=%.6f",
            epoch, sweep, kind, heightPercent, hullSource, requestedHeight, actualPlayerHeight);
        return;
    }
    float hit[3], planeNormal[3];
    TR_GetEndPosition(hit, trace);
    TR_GetPlaneNormal(trace, planeNormal);
    int entity = TR_GetEntityIndex(trace);
    char classname[128], map[PLATFORM_MAX_PATH], surfaceName[128];
    strcopy(classname, sizeof(classname), "none");
    if (entity >= 0 && IsValidEntity(entity)) GetEntityClassname(entity, classname, sizeof(classname));
    TR_GetSurfaceName(trace, surfaceName, sizeof(surfaceName));
    // SourceMod exposes point contents, not trace.contents. A hull's end is
    // its reference origin, so do not present this as the hit brush contents.
    int pointEntity = -1;
    int pointContents = TR_GetPointContents(hit, pointEntity);
    GetCurrentMap(map, sizeof(map));
    PrintToServer("[AGPB_JUMP_SWEEP] v=1 epoch=%s sweep=%d map_serial=%d map=%s tick=%d t=%.6f bot_slot=%d bot_userid=%d bot_serial=%d kind=%s height_pct=%d mask=0x%X start=(%.4f,%.4f,%.4f) target=(%.4f,%.4f,%.4f) mins=(%.4f,%.4f,%.4f) maxs=(%.4f,%.4f,%.4f) startsolid=%d allsolid=%d fraction=%.6f end=(%.4f,%.4f,%.4f) entity=%d class=%s read_only=1 plane_normal=(%.4f,%.4f,%.4f) surface_name=\"%s\" surface_flags=0x%X surface_props=%d end_point_contents=0x%X end_point_contents_ent=%d hull_source=%s requested_height=%.6f actual_player_height=%.6f",
        epoch, sweep, mapSerial, map, GetGameTickCount(), GetGameTime(), bot, GetClientUserId(bot), GetClientSerial(bot),
        kind, heightPercent, MASK_PLAYERSOLID, start[0], start[1], start[2], target[0], target[1], target[2],
        mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2], TR_StartSolid(trace) ? 1 : 0,
        TR_AllSolid(trace) ? 1 : 0, TR_GetFraction(trace), hit[0], hit[1], hit[2], entity, classname,
        planeNormal[0], planeNormal[1], planeNormal[2], surfaceName, TR_GetSurfaceFlags(trace),
        TR_GetSurfaceProps(trace), pointContents, pointEntity, hullSource, requestedHeight, actualPlayerHeight);
    CloseHandle(trace);
}

public Action JumpSweepCommand(int client, int args)
{
    if (client != 0) { ReplyToCommand(client, "[AGPB_JUMP_SWEEP] Server console only."); return Plugin_Handled; }
    if (args != 7 && args != 8)
    { PrintToServer("[AGPB_JUMP_SWEEP] usage: agpb_jump_probe_sweep <bot_slot> sx sy sz tx ty tz [height]"); return Plugin_Handled; }
    int bot;
    if (!JumpSweepSlotArgument(bot) || !ClientValid(bot) || !IsFakeClient(bot))
    { PrintToServer("[AGPB_JUMP_SWEEP] rejected: in_game_fake_client_required"); return Plugin_Handled; }
    float start[3], target[3], mins[3], maxs[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!JumpSweepFloatArgument(axis + 2, start[axis]) || !JumpSweepFloatArgument(axis + 5, target[axis]))
        { PrintToServer("[AGPB_JUMP_SWEEP] rejected: coordinates_require_finite_values_within_16384"); return Plugin_Handled; }
    }
    GetClientMins(bot, mins); GetClientMaxs(bot, maxs);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!JumpSweepCoordinate(mins[axis]) || !JumpSweepCoordinate(maxs[axis]) || mins[axis] > maxs[axis])
        { PrintToServer("[AGPB_JUMP_SWEEP] rejected: invalid_actual_player_hull"); return Plugin_Handled; }
    }
    float actualPlayerHeight = maxs[2] - mins[2];
    if (!Finite(actualPlayerHeight) || actualPlayerHeight <= 0.0)
    { PrintToServer("[AGPB_JUMP_SWEEP] rejected: invalid_actual_player_height"); return Plugin_Handled; }
    float height = actualPlayerHeight;
    char hullSource[40] = "actual_player_hull";
    if (args == 8)
    {
        if (!JumpSweepFloatArgument(8, height) || height <= 0.0 || height > actualPlayerHeight)
        { PrintToServer("[AGPB_JUMP_SWEEP] rejected: explicit_height_requires_finite_positive_value_not_above_actual_player_height"); return Plugin_Handled; }
        // A read-only hypothesis: only this local trace box changes. It does
        // not set player posture, collision properties, origin or movement.
        maxs[2] = mins[2] + height;
        if (!JumpSweepCoordinate(maxs[2]))
        { PrintToServer("[AGPB_JUMP_SWEEP] rejected: derived_hypothesis_max_z_outside_16384"); return Plugin_Handled; }
        strcopy(hullSource, sizeof(hullSource), "explicit_height_hypothesis");
    }
    int percentages[5] = {5, 25, 50, 75, 95};
    float rayStarts[5][3], rayTargets[5][3];
    for (int ray = 0; ray < 5; ++ray)
    {
        float offset = mins[2] + height * float(percentages[ray]) / 100.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            rayStarts[ray][axis] = start[axis] + (axis == 2 ? offset : 0.0);
            rayTargets[ray][axis] = target[axis] + (axis == 2 ? offset : 0.0);
            if (!JumpSweepCoordinate(rayStarts[ray][axis]) || !JumpSweepCoordinate(rayTargets[ray][axis]))
            { PrintToServer("[AGPB_JUMP_SWEEP] rejected: derived_ray_coordinates_outside_16384"); return Plugin_Handled; }
        }
    }
    int sweep = ++jumpSweepSequence;
    Handle trace = TR_TraceHullFilterEx(start, target, mins, maxs, MASK_PLAYERSOLID, JumpSweepIgnoreSelf, bot);
    JumpSweepLogResult(trace, bot, sweep, "hull", -1, start, target, mins, maxs, hullSource, height, actualPlayerHeight);
    for (int ray = 0; ray < 5; ++ray)
    {
        trace = TR_TraceRayFilterEx(rayStarts[ray], rayTargets[ray], MASK_PLAYERSOLID, RayType_EndPoint, JumpSweepIgnoreSelf, bot);
        JumpSweepLogResult(trace, bot, sweep, "ray", percentages[ray], rayStarts[ray], rayTargets[ray], mins, maxs, hullSource, height, actualPlayerHeight);
    }
    return Plugin_Handled;
}
