// SPDX-License-Identifier: GPL-3.0-only
// Opt-in equipment/observation fixture for an explicitly authorized showmatch.
// No AI, movement, aim or damage changes. Guard integrity is verified externally.
#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
    name = "AgPB showmatch fixture", author = "AgPB",
    description = "Opt-in round equipment and read-only showmatch snapshots", version = "1.0"
};

bool g_Enabled;
int generation, roundNumber, eventSequence;
int identities[MAXPLAYERS + 1], fires[MAXPLAYERS + 1], hits[MAXPLAYERS + 1], kills[MAXPLAYERS + 1], deaths[MAXPLAYERS + 1];
int focusedHuman, focusedSerial, savedMode, savedTargetRef = INVALID_ENT_REFERENCE;
ConVar cheats, guard;

bool Valid(int client) { return client > 0 && client <= MaxClients && IsClientInGame(client); }
bool Fighter(int client) { return Valid(client) && IsFakeClient(client) && GetClientTeam(client) >= 2 && GetClientTeam(client) <= 3; }
bool Prerequisites() { return cheats != null && cheats.BoolValue && guard != null && guard.BoolValue; }
int Read(int entity, const char[] prop) {
    if (entity < 0 || !IsValidEntity(entity)) return -1;
    if (HasEntProp(entity, Prop_Send, prop)) return GetEntProp(entity, Prop_Send, prop);
    if (HasEntProp(entity, Prop_Data, prop)) return GetEntProp(entity, Prop_Data, prop);
    return -1;
}
void RefreshIdentity(int client) {
    if (!Valid(client) || identities[client] == GetClientSerial(client)) return;
    identities[client] = GetClientSerial(client);
    fires[client] = hits[client] = kills[client] = deaths[client] = 0;
}
void RestoreFocus() {
    int client = focusedHuman;
    if (Valid(client) && GetClientSerial(client) == focusedSerial && GetClientTeam(client) == 1 && !IsPlayerAlive(client)) {
        SetEntProp(client, Prop_Send, "m_iObserverMode", savedMode);
        int target = EntRefToEntIndex(savedTargetRef);
        SetEntPropEnt(client, Prop_Send, "m_hObserverTarget", target == INVALID_ENT_REFERENCE ? -1 : target);
    }
    focusedHuman = focusedSerial = 0;
    savedTargetRef = INVALID_ENT_REFERENCE;
}
void Disable() {
    g_Enabled = false;
    generation++;
    RestoreFocus();
}
public void OnPluginStart() {
    cheats = FindConVar("sv_cheats");
    guard = FindConVar("sm_agpb_fakeclient_guard");
    if (cheats != null) cheats.AddChangeHook(PrerequisiteChanged);
    if (guard != null) guard.AddChangeHook(PrerequisiteChanged);
    RegServerCmd("agpb_showmatch_enable", EnableCommand);
    RegServerCmd("agpb_showmatch_state", StateCommand);
    RegServerCmd("agpb_showmatch_focus", FocusCommand);
    HookEvent("player_spawn", Spawn, EventHookMode_Post);
    HookEvent("round_start", RoundStart, EventHookMode_PostNoCopy);
    HookEvent("round_freeze_end", FreezeEnd, EventHookMode_PostNoCopy);
    HookEvent("weapon_fire", MatchEvent, EventHookMode_Post);
    HookEvent("player_hurt", MatchEvent, EventHookMode_Post);
    HookEvent("player_death", MatchEvent, EventHookMode_Post);
    HookEvent("round_end", RoundEnd, EventHookMode_Post);
    PrintToServer("[AGPB_SHOWMATCH] loaded disabled; enable requires sv_cheats=1 and independently verified Radio guard active=1 intact=1");
}
public void OnMapEnd() { Disable(); }
public void OnPluginEnd() { Disable(); }
public void PrerequisiteChanged(ConVar convar, const char[] oldValue, const char[] newValue) {
    if (g_Enabled && !Prerequisites()) {
        Disable();
        PrintToServer("[AGPB_SHOWMATCH] disabled: equipment prerequisite changed; remove all bots before disabling Radio guard");
    }
}
public Action EnableCommand(int args) {
    char text[8];
    if (args != 1) { PrintToServer("[AGPB_SHOWMATCH] usage: agpb_showmatch_enable 0|1"); return Plugin_Handled; }
    GetCmdArg(1, text, sizeof(text));
    if (StrEqual(text, "0")) {
        Disable();
        PrintToServer("[AGPB_SHOWMATCH] disabled; equipped items remain; no game settings changed");
        return Plugin_Handled;
    }
    if (!StrEqual(text, "1") || !Prerequisites()) {
        PrintToServer("[AGPB_SHOWMATCH] rejected: require 1, sv_cheats=1, loaded enabled Radio guard, and external active/intact check");
        return Plugin_Handled;
    }
    bool frozen = GameRules_GetProp("m_bFreezePeriod") != 0;
    for (int i = 1; i <= MaxClients; i++) {
        if (Fighter(i) && IsPlayerAlive(i) && !frozen) {
            PrintToServer("[AGPB_SHOWMATCH] rejected: enable before creating bots or during freeze period");
            return Plugin_Handled;
        }
    }
    g_Enabled = true; generation++;
    for (int i = 1; i <= MaxClients; i++) { identities[i] = 0; RefreshIdentity(i); }
    eventSequence = roundNumber = 0;
    EquipAll();
    ServerCommand("sm_agpb_guard_status");
    PrintToServer("[AGPB_SHOWMATCH] enabled equipment_only=1 guard_intact_external_verification_required=1");
    return Plugin_Handled;
}
void Equip(int client) {
    if (!g_Enabled || !Prerequisites() || !Fighter(client) || !IsPlayerAlive(client)) return;
    // Diagnostic round preparation through public SDKTools and SendTable props.
    // Only the primary slot is replaced; C4 and normal map objectives are kept.
    int weapon = GetPlayerWeaponSlot(client, 0);
    char classname[64];
    if (weapon > MaxClients && IsValidEntity(weapon)) GetEntityClassname(weapon, classname, sizeof(classname));
    if (!StrEqual(classname, "weapon_m4a1")) {
        if (weapon > MaxClients && IsValidEntity(weapon)) {
            if (!RemovePlayerItem(client, weapon)) {
                PrintToServer("[AGPB_SHOWMATCH] equip_failed slot=%d reason=remove_primary", client);
                return;
            }
            AcceptEntityInput(weapon, "Kill");
        }
        weapon = GivePlayerItem(client, "weapon_m4a1");
    }
    if (weapon <= MaxClients || !IsValidEntity(weapon)) {
        PrintToServer("[AGPB_SHOWMATCH] equip_failed slot=%d reason=give_m4a1", client);
        return;
    }
    EquipPlayerWeapon(client, weapon);
    // Weapon_Equip establishes ownership but need not switch away from a pistol.
    // Use the game's ordinary selection command once during equipment setup.
    FakeClientCommand(client, "use weapon_m4a1");
    SetEntProp(weapon, Prop_Send, "m_iClip1", 30);
    int ammoType = Read(weapon, "m_iPrimaryAmmoType"), reserve = -1;
    if (ammoType >= 0 && HasEntProp(client, Prop_Send, "m_iAmmo") && ammoType < GetEntPropArraySize(client, Prop_Send, "m_iAmmo")) {
        SetEntProp(client, Prop_Send, "m_iAmmo", 90, _, ammoType);
        reserve = GetEntProp(client, Prop_Send, "m_iAmmo", _, ammoType);
    }
    else PrintToServer("[AGPB_SHOWMATCH] equip_failed slot=%d reason=reserve_ammo_prop", client);
    SetEntProp(client, Prop_Send, "m_ArmorValue", 100);
    SetEntProp(client, Prop_Send, "m_bHasHelmet", 1);
    PrintToServer("[AGPB_SHOWMATCH] equipped round=%d slot=%d userid=%d serial=%d team=%d primary=%d active_ent=%d clip=%d reserve=%d armor=%d helmet=%d",
        roundNumber, client, GetClientUserId(client), GetClientSerial(client), GetClientTeam(client), weapon,
        GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon"),
        Read(weapon, "m_iClip1"), reserve,
        Read(client, "m_ArmorValue"), Read(client, "m_bHasHelmet"));
}
void EquipAll() { for (int i = 1; i <= MaxClients; i++) Equip(i); }
public void Spawn(Event event, const char[] name, bool dontBroadcast) {
    if (!g_Enabled) return;
    int client = GetClientOfUserId(event.GetInt("userid"));
    if (!Fighter(client)) return;
    DataPack data;
    CreateDataTimer(0.2, EquipSpawn, data, TIMER_FLAG_NO_MAPCHANGE);
    data.WriteCell(GetClientSerial(client)); data.WriteCell(generation);
}
public Action EquipSpawn(Handle timer, DataPack data) {
    data.Reset();
    int client = GetClientFromSerial(data.ReadCell());
    if (data.ReadCell() == generation) Equip(client);
    return Plugin_Stop;
}
public void RoundStart(Event event, const char[] name, bool dontBroadcast) {
    if (!g_Enabled) return;
    roundNumber++;
    CreateTimer(0.3, EquipRound, generation, TIMER_FLAG_NO_MAPCHANGE);
}
public Action EquipRound(Handle timer, any expectedGeneration) {
    if (g_Enabled && expectedGeneration == generation && GameRules_GetProp("m_bFreezePeriod") != 0) EquipAll();
    return Plugin_Stop;
}
public void FreezeEnd(Event event, const char[] name, bool dontBroadcast) {
    if (!g_Enabled) return;
    // This event is dispatched before the game's first active movement tick.
    EquipAll();
    PrintToServer("[AGPB_SHOWMATCH] freeze_end round=%d tick=%d", roundNumber, GetGameTickCount());
    ServerCommand("sm_agpb_guard_status");
}
public void MatchEvent(Event event, const char[] name, bool dontBroadcast) {
    if (!g_Enabled) return;
    bool fire = StrEqual(name, "weapon_fire"), hurt = StrEqual(name, "player_hurt");
    int victim = fire ? 0 : GetClientOfUserId(event.GetInt("userid"));
    int actor = GetClientOfUserId(event.GetInt(fire ? "userid" : "attacker"));
    RefreshIdentity(actor); RefreshIdentity(victim);
    if (Valid(actor)) {
        if (fire) fires[actor]++;
        else if (hurt) hits[actor]++;
        else if (actor != victim) kills[actor]++;
    }
    if (!fire && !hurt && Valid(victim)) deaths[victim]++;
    char weapon[64]; event.GetString("weapon", weapon, sizeof(weapon), "unknown");
    PrintToServer("[AGPB_SHOWMATCH_EVENT] seq=%d round=%d tick=%d event=%s actor_slot=%d actor_userid=%d actor_serial=%d actor_team=%d victim_slot=%d victim_userid=%d victim_serial=%d victim_team=%d weapon=%s dmg_health=%d dmg_armor=%d headshot=%d",
        ++eventSequence, roundNumber, GetGameTickCount(), name, actor, Valid(actor) ? GetClientUserId(actor) : -1,
        Valid(actor) ? GetClientSerial(actor) : -1, Valid(actor) ? GetClientTeam(actor) : -1,
        victim, Valid(victim) ? GetClientUserId(victim) : -1, Valid(victim) ? GetClientSerial(victim) : -1,
        Valid(victim) ? GetClientTeam(victim) : -1, weapon,
        hurt ? event.GetInt("dmg_health", -1) : -1, hurt ? event.GetInt("dmg_armor", -1) : -1,
        !fire && !hurt ? (event.GetBool("headshot") ? 1 : 0) : -1);
}
public void RoundEnd(Event event, const char[] name, bool dontBroadcast) {
    if (!g_Enabled) return;
    PrintToServer("[AGPB_SHOWMATCH] round_end round=%d tick=%d winner=%d reason=%d", roundNumber, GetGameTickCount(), event.GetInt("winner"), event.GetInt("reason"));
    StateCommand(0);
}
public Action StateCommand(int args) {
    if (args != 0) { PrintToServer("[AGPB_SHOWMATCH] usage: agpb_showmatch_state"); return Plugin_Handled; }
    int ct, t, humans;
    for (int i = 1; i <= MaxClients; i++) {
        if (!Valid(i)) continue;
        RefreshIdentity(i);
        if (!IsFakeClient(i)) humans++;
        else if (GetClientTeam(i) == 2) t++;
        else if (GetClientTeam(i) == 3) ct++;
        int active = GetEntPropEnt(i, Prop_Send, "m_hActiveWeapon");
        int primary = GetPlayerWeaponSlot(i, 0), ammoType = Read(primary, "m_iPrimaryAmmoType"), reserve = -1;
        if (ammoType >= 0 && HasEntProp(i, Prop_Send, "m_iAmmo") && ammoType < GetEntPropArraySize(i, Prop_Send, "m_iAmmo"))
            reserve = GetEntProp(i, Prop_Send, "m_iAmmo", _, ammoType);
        char weapon[64] = "none", primaryName[64] = "none", playerName[MAX_NAME_LENGTH];
        if (active > MaxClients && IsValidEntity(active)) GetEntityClassname(active, weapon, sizeof(weapon));
        if (primary > MaxClients && IsValidEntity(primary)) GetEntityClassname(primary, primaryName, sizeof(primaryName));
        GetClientName(i, playerName, sizeof(playerName));
        PrintToServer("[AGPB_SHOWMATCH_PLAYER] slot=%d userid=%d serial=%d name=%s fake=%d team=%d alive=%d hp=%d armor=%d helmet=%d active=%s primary=%s clip=%d reserve=%d observer=%d observer_target=%d fires=%d hits=%d kills=%d deaths=%d",
            i, GetClientUserId(i), GetClientSerial(i), playerName, IsFakeClient(i) ? 1 : 0, GetClientTeam(i), IsPlayerAlive(i) ? 1 : 0,
            Read(i, "m_iHealth"), Read(i, "m_ArmorValue"), Read(i, "m_bHasHelmet"), weapon, primaryName, Read(primary, "m_iClip1"), reserve,
            Read(i, "m_iObserverMode"), GetEntPropEnt(i, Prop_Send, "m_hObserverTarget"), fires[i], hits[i], kills[i], deaths[i]);
    }
    PrintToServer("[AGPB_SHOWMATCH_STATE] enabled=%d round=%d tick=%d t_fakes=%d ct_fakes=%d real_humans=%d score_t=%d score_ct=%d sv_cheats=%d guard_enabled=%d guard_intact_external_verification_required=1",
        g_Enabled ? 1 : 0, roundNumber, GetGameTickCount(), t, ct, humans, GetTeamScore(2), GetTeamScore(3),
        cheats != null ? cheats.IntValue : -1, guard != null ? guard.IntValue : -1);
    return Plugin_Handled;
}
public Action FocusCommand(int args) {
    char text[16]; int human, target;
    if (args != 2) { PrintToServer("[AGPB_SHOWMATCH] usage: agpb_showmatch_focus human_slot target_slot|0"); return Plugin_Handled; }
    GetCmdArg(1, text, sizeof(text)); int used = StringToIntEx(text, human);
    if (used < 1 || used != strlen(text)) return Plugin_Handled;
    GetCmdArg(2, text, sizeof(text)); used = StringToIntEx(text, target);
    if (used < 1 || used != strlen(text)) return Plugin_Handled;
    if (!g_Enabled || !Prerequisites() || !Valid(human) || IsFakeClient(human) || GetClientTeam(human) != 1 || IsPlayerAlive(human)) {
        PrintToServer("[AGPB_SHOWMATCH] focus rejected: enabled fixture and real spectator required; join spectator before creating bots");
        return Plugin_Handled;
    }
    if (target == 0) { RestoreFocus(); return Plugin_Handled; }
    if (!Fighter(target) || !IsPlayerAlive(target)) { PrintToServer("[AGPB_SHOWMATCH] focus rejected: living fake fighter required"); return Plugin_Handled; }
    if (focusedHuman != human || focusedSerial != GetClientSerial(human)) {
        RestoreFocus();
        focusedHuman = human; focusedSerial = GetClientSerial(human);
        savedMode = Read(human, "m_iObserverMode");
        int previous = GetEntPropEnt(human, Prop_Send, "m_hObserverTarget");
        savedTargetRef = previous >= 0 && IsValidEntity(previous) ? EntIndexToEntRef(previous) : INVALID_ENT_REFERENCE;
    }
    SetEntProp(human, Prop_Send, "m_iObserverMode", 4); // Public OBS_MODE_IN_EYE enum value.
    SetEntPropEnt(human, Prop_Send, "m_hObserverTarget", target);
    PrintToServer("[AGPB_SHOWMATCH] focus human_slot=%d target_slot=%d target_userid=%d target_serial=%d observer=%d observer_target=%d",
        human, target, GetClientUserId(target), GetClientSerial(target), Read(human, "m_iObserverMode"), GetEntPropEnt(human, Prop_Send, "m_hObserverTarget"));
    return Plugin_Handled;
}
