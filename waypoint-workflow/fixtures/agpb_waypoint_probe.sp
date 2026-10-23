#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

// Optional read-only diagnostics. No placement, inventory, input or team writes.
public Plugin myinfo = {
    name = "AgPB read-only waypoint probe",
    author = "AgPB",
    description = "Read-only player state and real bomb/lifecycle event evidence",
    version = "1.1"
};
int plantedCount;
int defusedCount;
int explodedCount;
int plantedUserId;
int defusedUserId;
int explodedUserId;
int probeEpoch;
int mapSerial;
int lifecycleSequence;
int deathCount, spawnCount, teamCount, roundStartCount, roundEndCount;
int clientDeaths[MAXPLAYERS + 1], clientSpawns[MAXPLAYERS + 1], clientTeams[MAXPLAYERS + 1];
int deathUserId, deathSlot = -1, deathSerial = -1, deathSequence;
int spawnUserId, spawnSlot = -1, spawnSerial = -1, spawnSequence;
int teamUserId, teamSlot = -1, teamSerial = -1, teamSequence;
#define EVENT_CACHE_SIZE 64
#define EVENT_LINE_SIZE 768
char lifecycleCache[EVENT_CACHE_SIZE][EVENT_LINE_SIZE];
int cacheWrite;
int cacheCount;

void RememberEvent(const char[] line)
{
    strcopy(lifecycleCache[cacheWrite], EVENT_LINE_SIZE, line);
    cacheWrite = (cacheWrite + 1) % EVENT_CACHE_SIZE;
    if (cacheCount < EVENT_CACHE_SIZE) cacheCount++;
    PrintToServer("%s replay=0", line);
}

int ReadPlayerInt(int client, const char[] property)
{
    if (HasEntProp(client, Prop_Send, property)) return GetEntProp(client, Prop_Send, property);
    if (HasEntProp(client, Prop_Data, property)) return GetEntProp(client, Prop_Data, property);
    return -1;
}

void MapEvent(const char[] event)
{
    char map[64], line[EVENT_LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    lifecycleSequence++;
    Format(line, sizeof(line), "[WP_PROBE_MAP] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s seq=%d event=%s",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, lifecycleSequence, event);
    RememberEvent(line);
}

public void OnPluginStart()
{
    probeEpoch = GetTime();
    RegServerCmd("agpb_wp_probe_state", State);
    RegServerCmd("agpb_wp_probe_bomb", Bomb);
    RegServerCmd("agpb_wp_probe_lifecycle", Lifecycle);
    HookEvent("bomb_planted", BombEvent);
    HookEvent("bomb_defused", BombEvent);
    HookEvent("bomb_exploded", BombEvent);
    HookEvent("player_death", LifecycleEvent);
    HookEvent("player_spawn", LifecycleEvent);
    HookEvent("player_team", LifecycleEvent);
    HookEvent("round_start", LifecycleEvent);
    HookEvent("round_end", LifecycleEvent);
}

public void OnMapStart()
{
    mapSerial++;
    plantedCount = 0; defusedCount = 0; explodedCount = 0;
    plantedUserId = 0; defusedUserId = 0; explodedUserId = 0;
    deathCount = 0; spawnCount = 0; teamCount = 0; roundStartCount = 0; roundEndCount = 0;
    deathUserId = 0; deathSlot = -1; deathSerial = -1; deathSequence = 0;
    spawnUserId = 0; spawnSlot = -1; spawnSerial = -1; spawnSequence = 0;
    teamUserId = 0; teamSlot = -1; teamSerial = -1; teamSequence = 0;
    for (int client = 1; client <= MaxClients; client++)
    {
        clientDeaths[client] = 0; clientSpawns[client] = 0; clientTeams[client] = 0;
    }
    MapEvent("map_start");
}

public void OnMapEnd()
{
    MapEvent("map_end");
}

public void OnClientPutInServer(int client)
{
    clientDeaths[client] = 0; clientSpawns[client] = 0; clientTeams[client] = 0;
}

public Action State(int args)
{
    char map[64]; GetCurrentMap(map, sizeof(map));
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsClientInGame(i)) continue;
        float position[3], velocity[3], eye[3];
        GetClientAbsOrigin(i, position);
        GetEntPropVector(i, Prop_Data, "m_vecVelocity", velocity);
        GetClientEyeAngles(i, eye);
        int zone = HasEntProp(i, Prop_Send, "m_bInBombZone") ? GetEntProp(i, Prop_Send, "m_bInBombZone") : -1;
        int defusing = HasEntProp(i, Prop_Send, "m_bIsDefusing") ? GetEntProp(i, Prop_Send, "m_bIsDefusing") : -1;
        int flags = GetEntityFlags(i);
        int view = HasEntProp(i, Prop_Data, "m_hViewEntity") ? GetEntPropEnt(i, Prop_Data, "m_hViewEntity") : -1;
        PrintToServer("[WP_PROBE_STATE] t=%.3f map=%s slot=%d userid=%d fake=%d team=%d alive=%d flags=%d pos=(%.6f %.6f %.6f) vel=(%.6f %.6f %.6f) zone=%d defusing=%d v=1 epoch=%d map_serial=%d tick=%d serial=%d frozen=%d view_ent=%d eye=(%.6f %.6f %.6f) buttons=%d player_state=%d life_state=%d observer=%d deaths=%d spawns=%d teams=%d round_starts=%d round_ends=%d lifecycle_seq=%d client_deaths=%d client_spawns=%d client_teams=%d",
            GetGameTime(), map, i, GetClientUserId(i), IsFakeClient(i), GetClientTeam(i), IsPlayerAlive(i), GetEntityFlags(i),
            position[0], position[1], position[2], velocity[0], velocity[1], velocity[2], zone, defusing,
            probeEpoch, mapSerial, GetGameTickCount(), GetClientSerial(i), (flags & FL_FROZEN) != 0, view,
            eye[0], eye[1], eye[2], GetClientButtons(i), ReadPlayerInt(i, "m_iPlayerState"),
            ReadPlayerInt(i, "m_lifeState"), ReadPlayerInt(i, "m_iObserverMode"), deathCount, spawnCount, teamCount,
            roundStartCount, roundEndCount, lifecycleSequence, clientDeaths[i], clientSpawns[i], clientTeams[i]);
    }
    return Plugin_Handled;
}

public Action Lifecycle(int args)
{
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_PROBE_LIFECYCLE_COUNTS] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s seq=%d deaths=%d spawns=%d teams=%d round_starts=%d round_ends=%d death_userid=%d death_slot=%d death_serial=%d death_seq=%d spawn_userid=%d spawn_slot=%d spawn_serial=%d spawn_seq=%d team_userid=%d team_slot=%d team_serial=%d team_seq=%d cached=%d",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, lifecycleSequence,
        deathCount, spawnCount, teamCount, roundStartCount, roundEndCount,
        deathUserId, deathSlot, deathSerial, deathSequence, spawnUserId, spawnSlot, spawnSerial, spawnSequence,
        teamUserId, teamSlot, teamSerial, teamSequence, cacheCount);
    // Re-querying preserves the original identity, map, tick and sequence. These
    // are cached observations, never newly fired game events. Deduplicate by
    // (epoch, seq); a replay cannot prove another death/spawn/team/round change.
    for (int offset = cacheCount; offset > 0; offset--)
    {
        int index = (cacheWrite - offset + EVENT_CACHE_SIZE) % EVENT_CACHE_SIZE;
        PrintToServer("%s replay=1", lifecycleCache[index]);
    }
    return Plugin_Handled;
}

public void LifecycleEvent(Event event, const char[] name, bool dontBroadcast)
{
    int userid = event.GetInt("userid", 0), client = GetClientOfUserId(userid);
    bool valid = client >= 1 && client <= MaxClients && IsClientInGame(client);
    int slot = valid ? client : -1, serial = valid ? GetClientSerial(client) : -1;
    lifecycleSequence++;
    if (StrEqual(name, "player_death"))
    {
        deathCount++; if (valid) clientDeaths[client]++;
        deathUserId = userid; deathSlot = slot; deathSerial = serial; deathSequence = lifecycleSequence;
    }
    else if (StrEqual(name, "player_spawn"))
    {
        spawnCount++; if (valid) clientSpawns[client]++;
        spawnUserId = userid; spawnSlot = slot; spawnSerial = serial; spawnSequence = lifecycleSequence;
    }
    else if (StrEqual(name, "player_team"))
    {
        teamCount++; if (valid) clientTeams[client]++;
        teamUserId = userid; teamSlot = slot; teamSerial = serial; teamSequence = lifecycleSequence;
    }
    else if (StrEqual(name, "round_start")) roundStartCount++;
    else if (StrEqual(name, "round_end")) roundEndCount++;
    char map[64], line[EVENT_LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[WP_PROBE_LIFECYCLE] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s seq=%d event=%s userid=%d slot=%d serial=%d fake=%d team=%d alive=%d oldteam=%d newteam=%d deaths=%d spawns=%d teams=%d round_starts=%d round_ends=%d winner=%d reason=%d",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, lifecycleSequence, name, userid,
        slot, serial, valid ? (IsFakeClient(client) ? 1 : 0) : -1,
        valid ? GetClientTeam(client) : -1, valid ? (IsPlayerAlive(client) ? 1 : 0) : -1,
        event.GetInt("oldteam", -1), event.GetInt("team", -1), deathCount, spawnCount, teamCount,
        roundStartCount, roundEndCount, event.GetInt("winner", -1), event.GetInt("reason", -1));
    RememberEvent(line);
}

public Action Bomb(int args)
{
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_PROBE_BOMB_EVENTS] t=%.3f map=%s planted=%d planted_userid=%d defused=%d defused_userid=%d exploded=%d exploded_userid=%d",
        GetGameTime(), map, plantedCount, plantedUserId, defusedCount, defusedUserId, explodedCount, explodedUserId);
    int entity = -1, count = 0;
    while ((entity = FindEntityByClassname(entity, "planted_c4")) != -1)
    {
        float position[3]; GetEntPropVector(entity, Prop_Send, "m_vecOrigin", position);
        int ticking = HasEntProp(entity, Prop_Send, "m_bBombTicking") ? GetEntProp(entity, Prop_Send, "m_bBombTicking") : -1;
        int defused = HasEntProp(entity, Prop_Send, "m_bBombDefused") ? GetEntProp(entity, Prop_Send, "m_bBombDefused") : -1;
        float blow = HasEntProp(entity, Prop_Send, "m_flC4Blow") ? GetEntPropFloat(entity, Prop_Send, "m_flC4Blow") : -1.0;
        PrintToServer("[WP_PROBE_BOMB] t=%.3f map=%s ent=%d ticking=%d defused=%d blow=%.3f pos=(%.2f %.2f %.2f)",
            GetGameTime(), map, entity, ticking, defused, blow, position[0], position[1], position[2]);
        count++;
    }
    if (count == 0) PrintToServer("[WP_PROBE_BOMB] t=%.3f map=%s none", GetGameTime(), map);
    return Plugin_Handled;
}

public void BombEvent(Event event, const char[] name, bool dontBroadcast)
{
    int userid = event.GetInt("userid", 0);
    if (StrEqual(name, "bomb_planted")) { plantedCount++; plantedUserId = userid; }
    if (StrEqual(name, "bomb_defused")) { defusedCount++; defusedUserId = userid; }
    if (StrEqual(name, "bomb_exploded")) { explodedCount++; explodedUserId = userid; }
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_PROBE_EVENT] t=%.3f map=%s event=%s userid=%d planted=%d defused=%d exploded=%d",
        GetGameTime(), map, name, userid, plantedCount, defusedCount, explodedCount);
}
