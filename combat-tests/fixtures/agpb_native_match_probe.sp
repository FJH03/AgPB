// SPDX-License-Identifier: GPL-3.0-only
// Optional read-only observer. No preparation, native AI calls, settings,
// movement, input, damage or synthetic events. Caller verifies authorization
// and Radio guard separately, and ends the scene immediately on human death.
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = { name = "AgPB native match observer", author = "AgPB", description = "Read-only exploratory match events", version = "1.0" };
#define CACHE_LIMIT 128
#define LINE_SIZE 1024
char epoch[40], cache[CACHE_LIMIT][LINE_SIZE];
int sequence, mapSerial, generation, cacheWrite, cacheCount;
bool watching;
int subject, subjectUserid, subjectSerial, human, humanUserid, humanSerial;
int serials[MAXPLAYERS + 1], fires[MAXPLAYERS + 1], dealt[MAXPLAYERS + 1], taken[MAXPLAYERS + 1], kills[MAXPLAYERS + 1], deaths[MAXPLAYERS + 1];

bool Valid(int client) { return client > 0 && client <= MaxClients && IsClientInGame(client); }
int Prop(int entity, const char[] name) {
    if (entity <= 0 || !IsValidEntity(entity)) return -1;
    if (HasEntProp(entity, Prop_Send, name)) return GetEntProp(entity, Prop_Send, name);
    if (HasEntProp(entity, Prop_Data, name)) return GetEntProp(entity, Prop_Data, name);
    return -1;
}
bool Active(int client) { return Valid(client) && IsPlayerAlive(client) && GetClientTeam(client) >= 2 && Prop(client, "m_iPlayerState") == 0; }
bool Matches(int client, int userid, int serial) { return Valid(client) && GetClientUserId(client) == userid && GetClientSerial(client) == serial; }
void Counters(int client) {
    if (!Valid(client) || serials[client] == GetClientSerial(client)) return;
    serials[client] = GetClientSerial(client);
    fires[client] = dealt[client] = taken[client] = kills[client] = deaths[client] = 0;
}
void MatchIdentity(int userid, char[] output, int length) {
    int client = GetClientOfUserId(userid);
    Format(output, length, "slot=%d userid=%d serial=%d team=%d fake=%d", Valid(client) ? client : -1, userid,
        Valid(client) ? GetClientSerial(client) : -1, Valid(client) ? GetClientTeam(client) : -1, Valid(client) ? (IsFakeClient(client) ? 1 : 0) : -1);
}
void Remember(const char[] text) {
    strcopy(cache[cacheWrite], LINE_SIZE, text);
    cacheWrite = (cacheWrite + 1) % CACHE_LIMIT;
    if (cacheCount < CACHE_LIMIT) cacheCount++;
    PrintToServer("%s replay=0", text);
}
void Detach() {
    if (!watching) return;
    UnhookEvent("weapon_fire", RealEvent, EventHookMode_Post);
    UnhookEvent("player_hurt", RealEvent, EventHookMode_Post);
    UnhookEvent("player_death", RealEvent, EventHookMode_Post);
    watching = false;
}
public void OnPluginStart() {
    Format(epoch, sizeof(epoch), "%d-%08x-%08x", GetTime(), GetURandomInt(), GetURandomInt());
    mapSerial = 1;
    RegServerCmd("agpb_match_watch", WatchCommand);
    RegServerCmd("agpb_match_state", StateCommand);
    RegServerCmd("agpb_match_clear", ClearCommand);
}
public void OnMapStart() { Detach(); mapSerial++; cacheWrite = cacheCount = 0; }
public void OnMapEnd() { Detach(); }
public void OnPluginEnd() { Detach(); }

public Action WatchCommand(int args) {
    if (args != 2) { PrintToServer("[AGPB_MATCH] usage: agpb_match_watch subject_slot human_slot"); return Plugin_Handled; }
    char text[16]; int bot, observer;
    GetCmdArg(1, text, sizeof(text)); int used = StringToIntEx(text, bot);
    if (used < 1 || used != strlen(text)) { PrintToServer("[AGPB_MATCH] invalid subject slot"); return Plugin_Handled; }
    GetCmdArg(2, text, sizeof(text)); used = StringToIntEx(text, observer);
    if (used < 1 || used != strlen(text)) { PrintToServer("[AGPB_MATCH] invalid human slot"); return Plugin_Handled; }
    if (!Active(bot) || !Active(observer) || observer >= bot || !IsFakeClient(bot) || IsFakeClient(observer) || GetClientTeam(bot) != GetClientTeam(observer)) {
        PrintToServer("[AGPB_MATCH] rejected: alive ACTIVE fake subject and lower-slot real same-team human required"); return Plugin_Handled;
    }
    int opponents;
    for (int i = 1; i <= MaxClients; i++)
        if (Valid(i) && GetClientTeam(i) >= 2 && GetClientTeam(i) != GetClientTeam(observer)) {
            if (!IsFakeClient(i)) { PrintToServer("[AGPB_MATCH] rejected: opposing real player"); return Plugin_Handled; }
            opponents++;
        }
    if (opponents < 2 || opponents > 3) { PrintToServer("[AGPB_MATCH] rejected: exactly two or three opposing fakes required"); return Plugin_Handled; }
    Detach();
    subject = bot; subjectUserid = GetClientUserId(bot); subjectSerial = GetClientSerial(bot);
    human = observer; humanUserid = GetClientUserId(observer); humanSerial = GetClientSerial(observer);
    cacheWrite = cacheCount = 0; generation++;
    for (int i = 1; i <= MaxClients; i++) { serials[i] = 0; Counters(i); }
    HookEvent("weapon_fire", RealEvent, EventHookMode_Post);
    HookEvent("player_hurt", RealEvent, EventHookMode_Post);
    HookEvent("player_death", RealEvent, EventHookMode_Post);
    watching = true;
    PrintToServer("[AGPB_MATCH] watch_bound observation_only=1 guard_verified=-1 external_authorization_verified=-1");
    StateCommand(0);
    return Plugin_Handled;
}
public void RealEvent(Event event, const char[] name, bool dontBroadcast) {
    if (!watching) return;
    bool fire = StrEqual(name, "weapon_fire"), hurt = StrEqual(name, "player_hurt");
    int victimId = fire ? 0 : event.GetInt("userid"), actorId = fire ? event.GetInt("userid") : event.GetInt("attacker");
    int actor = GetClientOfUserId(actorId), victim = GetClientOfUserId(victimId);
    Counters(actor); Counters(victim);
    if (fire && Valid(actor)) fires[actor]++;
    if (hurt) { if (Valid(actor)) dealt[actor]++; if (Valid(victim)) taken[victim]++; }
    if (!fire && !hurt) { if (Valid(victim)) deaths[victim]++; if (Valid(actor) && actor != victim) kills[actor]++; }
    char actorText[128], victimText[128], weapon[64], map[64], line[LINE_SIZE];
    MatchIdentity(actorId, actorText, sizeof(actorText)); MatchIdentity(victimId, victimText, sizeof(victimText));
    event.GetString("weapon", weapon, sizeof(weapon), "unknown"); GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[AGPB_MATCH_EVENT] v=1 epoch=%s map_serial=%d map=%s tick=%d t=%.6f seq=%d generation=%d event=%s actor={%s} victim={%s} weapon=%s health=%d dmg_health=%d dmg_armor=%d hitgroup=%d headshot=%d human_death=%d",
        epoch, mapSerial, map, GetGameTickCount(), GetGameTime(), ++sequence, generation, name, actorText, victimText, weapon,
        hurt ? event.GetInt("health", -1) : -1, hurt ? event.GetInt("dmg_health", -1) : -1, hurt ? event.GetInt("dmg_armor", -1) : -1,
        hurt ? event.GetInt("hitgroup", -1) : -1, !fire && !hurt ? (event.GetBool("headshot") ? 1 : 0) : -1,
        !fire && !hurt && victimId == humanUserid && Matches(human, humanUserid, humanSerial) ? 1 : 0);
    Remember(line);
}
public Action StateCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_MATCH] usage: agpb_match_state"); return Plugin_Handled; }
    char map[64]; GetCurrentMap(map, sizeof(map));
    bool subjectPresent = Matches(subject, subjectUserid, subjectSerial), humanPresent = Matches(human, humanUserid, humanSerial);
    PrintToServer("[AGPB_MATCH_STATE] v=1 epoch=%s map_serial=%d map=%s tick=%d t=%.6f seq=%d generation=%d watching=%d subject_slot=%d subject_userid=%d subject_serial=%d subject_present=%d human_slot=%d human_userid=%d human_serial=%d human_present=%d human_alive=%d same_team=%d guard_verified=-1 external_authorization_verified=-1 scope=exploratory_only cached=%d cache_limit=%d replay=0",
        epoch, mapSerial, map, GetGameTickCount(), GetGameTime(), ++sequence, generation, watching,
        subject, subjectUserid, subjectSerial, subjectPresent, human, humanUserid, humanSerial, humanPresent,
        humanPresent ? (IsPlayerAlive(human) ? 1 : 0) : -1, subjectPresent && humanPresent ? (GetClientTeam(subject) == GetClientTeam(human) ? 1 : 0) : -1, cacheCount, CACHE_LIMIT);
    for (int i = 1; i <= MaxClients; i++) {
        if (!Valid(i)) continue;
        Counters(i);
        float pos[3]; GetClientAbsOrigin(i, pos);
        int active = HasEntProp(i, Prop_Send, "m_hActiveWeapon") ? GetEntPropEnt(i, Prop_Send, "m_hActiveWeapon") : -1;
        char weapon[64] = "unknown";
        if (active > 0 && IsValidEntity(active)) GetEntityClassname(active, weapon, sizeof(weapon));
        int ammoType = Prop(active, "m_iPrimaryAmmoType"), reserve = -1;
        if (ammoType >= 0 && HasEntProp(i, Prop_Send, "m_iAmmo") && ammoType < GetEntPropArraySize(i, Prop_Send, "m_iAmmo"))
            reserve = GetEntProp(i, Prop_Send, "m_iAmmo", _, ammoType);
        PrintToServer("[AGPB_MATCH_PLAYER] v=1 epoch=%s map_serial=%d tick=%d t=%.6f seq=%d generation=%d slot=%d userid=%d serial=%d team=%d fake=%d alive=%d hp=%d armor=%d player_state=%d pos=(%.3f,%.3f,%.3f) weapon=%s weapon_ent=%d clip=%d reserve=%d fires=%d hurts_dealt=%d hurts_taken=%d kills=%d deaths=%d replay=0",
            epoch, mapSerial, GetGameTickCount(), GetGameTime(), ++sequence, generation, i, GetClientUserId(i), GetClientSerial(i), GetClientTeam(i), IsFakeClient(i) ? 1 : 0, IsPlayerAlive(i) ? 1 : 0,
            Prop(i, "m_iHealth"), Prop(i, "m_ArmorValue"), Prop(i, "m_iPlayerState"), pos[0], pos[1], pos[2], weapon, active, Prop(active, "m_iClip1"), reserve, fires[i], dealt[i], taken[i], kills[i], deaths[i]);
    }
    for (int n = 0; n < cacheCount; n++) PrintToServer("%s replay=1", cache[(cacheWrite - cacheCount + n + CACHE_LIMIT) % CACHE_LIMIT]);
    return Plugin_Handled;
}
public Action ClearCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_MATCH] usage: agpb_match_clear"); return Plugin_Handled; }
    Detach();
    PrintToServer("[AGPB_MATCH] observations_detached=1 players_and_settings_unchanged=1");
    return Plugin_Handled;
}
