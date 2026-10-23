// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#pragma semicolon 1
#pragma newdecls required

// Optional read-only clearance evidence. No entity creation, input, damage,
// property writes, synthetic events or changes to the M3a probe are used.
public Plugin myinfo = {
    name = "AgPB read-only clearance probe",
    author = "AgPB",
    description = "Original prop identities, real damage and weapon observations",
    version = "1.0"
};

#define WATCH_LIMIT 16
#define DAMAGE_DEPTH 4
#define CACHE_LIMIT 64
#define LINE_SIZE 1024

enum struct Watch
{
    bool used;
    bool destroyed;
    bool hooked;
    int entity;
    int reference;
    int generation;
    int initialHealth;
    bool initialHealthKnown;
    int damageSeq;
    int breakSeq;
    int destroySeq;
    int depth;
    char classname[64];
    char targetname[128];
    char model[160];
    float position[3];
}

enum struct DamageContext
{
    bool valid;
    int id;
    int slot;
    int userid;
    int serial;
    int fake;
    int weaponRef;
    int activeRef;
    int beforeHealth;
    bool beforeKnown;
    char weaponClass[64];
    char activeClass[64];
    bool smackKnown;
    float smackTime;
}

Watch watches[WATCH_LIMIT];
DamageContext contexts[WATCH_LIMIT * DAMAGE_DEPTH];
int probeEpoch, mapSerial, sequence, generation, damageId;
int damageCount, breakCount, destroyCount, fireCount;
int clientFires[MAXPLAYERS + 1], clientDamages[MAXPLAYERS + 1];
bool propEventHooked, brushEventHooked, fireEventHooked;
char cache[CACHE_LIMIT][LINE_SIZE];
int cacheWrite, cacheCount;

bool ReadInt(int entity, const char[] prop, int &value)
{
    value = -1;
    if (entity <= 0 || !IsValidEntity(entity)) return false;
    if (HasEntProp(entity, Prop_Send, prop)) { value = GetEntProp(entity, Prop_Send, prop); return true; }
    if (HasEntProp(entity, Prop_Data, prop)) { value = GetEntProp(entity, Prop_Data, prop); return true; }
    return false;
}

bool ReadFloat(int entity, const char[] prop, float &value)
{
    value = -1.0;
    if (entity <= 0 || !IsValidEntity(entity)) return false;
    if (HasEntProp(entity, Prop_Send, prop)) { value = GetEntPropFloat(entity, Prop_Send, prop); return true; }
    if (HasEntProp(entity, Prop_Data, prop)) { value = GetEntPropFloat(entity, Prop_Data, prop); return true; }
    return false;
}

int ActiveWeapon(int client)
{
    if (client < 1 || client > MaxClients || !IsClientInGame(client) || !HasEntProp(client, Prop_Send, "m_hActiveWeapon")) return -1;
    return GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
}

int WeaponIdentity(int weapon, char[] classname, int size)
{
    strcopy(classname, size, "unknown");
    if (weapon <= MaxClients || !IsValidEntity(weapon)) return INVALID_ENT_REFERENCE;
    GetEntityClassname(weapon, classname, size);
    return EntIndexToEntRef(weapon);
}

void Actor(DamageContext context, int attacker, int weapon)
{
    context.valid = true; context.id = 0;
    context.slot = -1; context.userid = 0; context.serial = -1; context.fake = -1;
    context.beforeKnown = false; context.beforeHealth = -1;
    context.weaponRef = WeaponIdentity(weapon, context.weaponClass, sizeof(context.weaponClass));
    int active = ActiveWeapon(attacker);
    context.activeRef = WeaponIdentity(active, context.activeClass, sizeof(context.activeClass));
    context.smackKnown = ReadFloat(active, "m_flSmackTime", context.smackTime);
    if (attacker < 1 || attacker > MaxClients || !IsClientInGame(attacker)) return;
    context.slot = attacker; context.userid = GetClientUserId(attacker);
    context.serial = GetClientSerial(attacker); context.fake = IsFakeClient(attacker) ? 1 : 0;
}

int WatchFor(int entity, bool post = false)
{
    // A victim can be removed inside TakeDamage before its Post callback.
    if (post)
        for (int i = 0; i < WATCH_LIMIT; i++)
            if (watches[i].used && watches[i].entity == entity && watches[i].depth > 0) return i;
    for (int i = 0; i < WATCH_LIMIT; i++)
        if (watches[i].used && !watches[i].destroyed && watches[i].entity == entity &&
            EntRefToEntIndex(watches[i].reference) == entity) return i;
    return -1;
}

void Remember(const char[] line)
{
    strcopy(cache[cacheWrite], LINE_SIZE, line);
    cacheWrite = (cacheWrite + 1) % CACHE_LIMIT;
    if (cacheCount < CACHE_LIMIT) cacheCount++;
    PrintToServer("%s replay=0", line);
}

void Observe(const char[] event, int watch, DamageContext context, const char[] attribution,
    bool damageKnown = false, float damage = -1.0, int damageType = -1,
    bool afterKnown = false, int afterHealth = -1, const char[] eventWeapon = "unknown")
{
    sequence++;
    if (watch >= 0)
    {
        if (StrEqual(event, "damage_post")) watches[watch].damageSeq = sequence;
        else if (StrEqual(event, "destroy")) watches[watch].destroySeq = sequence;
        else watches[watch].breakSeq = sequence;
    }
    char map[64], line[LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[WP_CLEARANCE_EVENT] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s seq=%d event=%s watch=%d generation=%d ent=%d ref=%d class=%s damage_id=%d context_valid=%d attribution=%s attacker_slot=%d attacker_userid=%d attacker_serial=%d attacker_fake=%d weapon_ref=%d weapon_class=%s active_ref=%d active_class=%s smack_known=%d smack_time=%.6f damage_known=%d damage=%.6f damagetype=%d health_before_known=%d health_before=%d health_after_known=%d health_after=%d event_weapon=%s",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, sequence, event, watch,
        watch >= 0 ? watches[watch].generation : -1, watch >= 0 ? watches[watch].entity : -1,
        watch >= 0 ? watches[watch].reference : INVALID_ENT_REFERENCE,
        watch >= 0 ? watches[watch].classname : "unknown", context.id, context.valid, attribution,
        context.slot, context.userid, context.serial, context.fake, context.weaponRef, context.weaponClass,
        context.activeRef, context.activeClass, context.smackKnown, context.smackTime, damageKnown,
        damage, damageType, context.beforeKnown, context.beforeHealth, afterKnown, afterHealth, eventWeapon);
    Remember(line);
}

public void OnPluginStart()
{
    probeEpoch = GetTime();
    RegServerCmd("agpb_wp_probe_clearance_watch", WatchCommand);
    RegServerCmd("agpb_wp_probe_clearance_clear", ClearCommand);
    RegServerCmd("agpb_wp_probe_clearance", SnapshotCommand);
    propEventHooked = HookEventEx("break_prop", BreakEvent);
    brushEventHooked = HookEventEx("break_breakable", BreakEvent);
    fireEventHooked = HookEventEx("weapon_fire", WeaponFireObserved);
}

public void OnMapStart()
{
    mapSerial++;
    for (int i = 0; i < WATCH_LIMIT; i++) watches[i].used = false;
    for (int i = 1; i <= MaxClients; i++) { clientFires[i] = 0; clientDamages[i] = 0; }
}

public void OnClientPutInServer(int client)
{
    clientFires[client] = 0; clientDamages[client] = 0;
}

void ClearWatches()
{
    for (int i = 0; i < WATCH_LIMIT; i++)
    {
        if (!watches[i].used) continue;
        int entity = EntRefToEntIndex(watches[i].reference);
        if (entity > MaxClients && IsValidEntity(entity) && watches[i].hooked)
        {
            SDKUnhook(entity, SDKHook_OnTakeDamage, DamagePre);
            SDKUnhook(entity, SDKHook_OnTakeDamagePost, DamagePost);
        }
        watches[i].used = false;
    }
}

public void OnPluginEnd() { ClearWatches(); }

public Action ClearCommand(int args)
{
    ClearWatches();
    PrintToServer("[WP_CLEARANCE] observations cleared; no game entity was modified");
    return Plugin_Handled;
}

public Action WatchCommand(int args)
{
    if (args != 2) { PrintToServer("[WP_CLEARANCE] usage: agpb_wp_probe_clearance_watch name|ent value"); return Plugin_Handled; }
    char kind[16], value[128]; GetCmdArg(1, kind, sizeof(kind)); GetCmdArg(2, value, sizeof(value));
    int entity = -1;
    if (StrEqual(kind, "ent"))
    {
        int used = StringToIntEx(value, entity);
        if (used != strlen(value) || used <= 0) entity = -1;
    }
    else if (StrEqual(kind, "name"))
    {
        for (int i = MaxClients + 1; i < GetMaxEntities(); i++)
        {
            if (!IsValidEntity(i) || !HasEntProp(i, Prop_Data, "m_iName")) continue;
            char name[128]; GetEntPropString(i, Prop_Data, "m_iName", name, sizeof(name));
            if (!StrEqual(name, value)) continue;
            if (entity != -1) { PrintToServer("[WP_CLEARANCE] rejected: targetname is ambiguous"); return Plugin_Handled; }
            entity = i;
        }
    }
    if (entity <= MaxClients || !IsValidEntity(entity)) { PrintToServer("[WP_CLEARANCE] rejected: select one existing non-player prop"); return Plugin_Handled; }
    char classname[64]; GetEntityClassname(entity, classname, sizeof(classname));
    if (StrContains(classname, "prop_physics") != 0 && StrContains(classname, "func_breakable") != 0)
    { PrintToServer("[WP_CLEARANCE] rejected: only existing physics/breakable props can be watched"); return Plugin_Handled; }
    int existing = WatchFor(entity);
    if (existing >= 0) { TargetSnapshot(existing); return Plugin_Handled; }
    int watch = -1;
    for (int i = 0; i < WATCH_LIMIT; i++) if (!watches[i].used) { watch = i; break; }
    if (watch == -1) { PrintToServer("[WP_CLEARANCE] rejected: 16 observations already retained; clear explicitly"); return Plugin_Handled; }
    watches[watch].used = true; watches[watch].destroyed = false;
    watches[watch].entity = entity; watches[watch].reference = EntIndexToEntRef(entity);
    watches[watch].generation = ++generation; watches[watch].depth = 0;
    watches[watch].damageSeq = 0; watches[watch].breakSeq = 0; watches[watch].destroySeq = 0;
    strcopy(watches[watch].classname, sizeof(watches[].classname), classname);
    watches[watch].targetname[0] = '\0'; watches[watch].model[0] = '\0';
    if (HasEntProp(entity, Prop_Data, "m_iName")) GetEntPropString(entity, Prop_Data, "m_iName", watches[watch].targetname, sizeof(watches[].targetname));
    if (HasEntProp(entity, Prop_Data, "m_ModelName")) GetEntPropString(entity, Prop_Data, "m_ModelName", watches[watch].model, sizeof(watches[].model));
    watches[watch].initialHealthKnown = ReadInt(entity, "m_iHealth", watches[watch].initialHealth);
    bool pre = SDKHookEx(entity, SDKHook_OnTakeDamage, DamagePre);
    bool post = SDKHookEx(entity, SDKHook_OnTakeDamagePost, DamagePost);
    watches[watch].hooked = pre && post;
    if (!watches[watch].hooked)
    {
        if (pre) SDKUnhook(entity, SDKHook_OnTakeDamage, DamagePre);
        if (post) SDKUnhook(entity, SDKHook_OnTakeDamagePost, DamagePost);
    }
    TargetSnapshot(watch);
    return Plugin_Handled;
}

void TargetSnapshot(int watch)
{
    int entity = EntRefToEntIndex(watches[watch].reference);
    bool exists = !watches[watch].destroyed && entity > MaxClients && IsValidEntity(entity);
    int health = -1, broken = -1;
    bool healthKnown = exists && ReadInt(entity, "m_iHealth", health);
    bool brokenKnown = exists && ReadInt(entity, "m_bBroken", broken);
    bool positionKnown = exists && HasEntProp(entity, Prop_Data, "m_vecAbsOrigin");
    if (positionKnown) GetEntPropVector(entity, Prop_Data, "m_vecAbsOrigin", watches[watch].position);
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_CLEARANCE_TARGET] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s watch=%d generation=%d ent=%d ref=%d exists=%d class=%s name=\"%s\" model=\"%s\" damage_hooked=%d initial_health_known=%d initial_health=%d health_known=%d health=%d broken_known=%d broken=%d pos_known=%d pos=(%.6f %.6f %.6f) damage_seq=%d break_seq=%d destroy_seq=%d",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, watch, watches[watch].generation,
        watches[watch].entity, watches[watch].reference, exists, watches[watch].classname,
        watches[watch].targetname, watches[watch].model, watches[watch].hooked,
        watches[watch].initialHealthKnown, watches[watch].initialHealth, healthKnown, health, brokenKnown, broken,
        positionKnown, watches[watch].position[0], watches[watch].position[1], watches[watch].position[2],
        watches[watch].damageSeq, watches[watch].breakSeq, watches[watch].destroySeq);
}

void WeaponSnapshot(int client)
{
    int active = ActiveWeapon(client); char classname[64];
    int reference = WeaponIdentity(active, classname, sizeof(classname));
    int clip, ammoType, reserve = -1, reload;
    bool clipKnown = ReadInt(active, "m_iClip1", clip), ammoKnown = ReadInt(active, "m_iPrimaryAmmoType", ammoType);
    bool reloadKnown = ReadInt(active, "m_bInReload", reload), reserveKnown;
    if (ammoKnown && ammoType >= 0 && HasEntProp(client, Prop_Send, "m_iAmmo") && ammoType < GetEntPropArraySize(client, Prop_Send, "m_iAmmo"))
    { reserve = GetEntProp(client, Prop_Send, "m_iAmmo", _, ammoType); reserveKnown = true; }
    float primary, secondary, playerAttack, smack;
    bool primaryKnown = ReadFloat(active, "m_flNextPrimaryAttack", primary), secondaryKnown = ReadFloat(active, "m_flNextSecondaryAttack", secondary);
    bool playerKnown = ReadFloat(client, "m_flNextAttack", playerAttack), smackKnown = ReadFloat(active, "m_flSmackTime", smack);
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_CLEARANCE_WEAPON] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s slot=%d userid=%d serial=%d fake=%d alive=%d active_ent=%d active_ref=%d class=%s clip_known=%d clip=%d ammotype_known=%d ammotype=%d reserve_known=%d reserve=%d reload_known=%d reload=%d primary_known=%d primary=%.6f secondary_known=%d secondary=%.6f player_attack_known=%d player_attack=%.6f smack_known=%d smack_time=%.6f client_fires=%d client_damages=%d",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, client, GetClientUserId(client), GetClientSerial(client),
        IsFakeClient(client), IsPlayerAlive(client), active, reference, classname, clipKnown, clip, ammoKnown, ammoType,
        reserveKnown, reserve, reloadKnown, reload, primaryKnown, primary, secondaryKnown, secondary,
        playerKnown, playerAttack, smackKnown, smack, clientFires[client], clientDamages[client]);
}

public Action SnapshotCommand(int args)
{
    if (args != 1) { PrintToServer("[WP_CLEARANCE] usage: agpb_wp_probe_clearance bot_slot"); return Plugin_Handled; }
    char text[24]; GetCmdArg(1, text, sizeof(text)); int client;
    int used = StringToIntEx(text, client);
    if (used != strlen(text) || used <= 0 || client < 1 || client > MaxClients || !IsClientInGame(client))
    { PrintToServer("[WP_CLEARANCE] rejected: select an in-game client slot"); return Plugin_Handled; }
    int watched;
    for (int i = 0; i < WATCH_LIMIT; i++) if (watches[i].used) { watched++; TargetSnapshot(i); }
    WeaponSnapshot(client);
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_CLEARANCE_COUNTS] v=1 epoch=%d map_serial=%d tick=%d t=%.3f map=%s seq=%d damage_calls=%d breaks=%d destroys=%d fires=%d watched=%d cached=%d break_prop_hooked=%d break_breakable_hooked=%d weapon_fire_hooked=%d",
        probeEpoch, mapSerial, GetGameTickCount(), GetGameTime(), map, sequence, damageCount, breakCount, destroyCount,
        fireCount, watched, cacheCount, propEventHooked, brushEventHooked, fireEventHooked);
    for (int offset = cacheCount; offset > 0; offset--)
        PrintToServer("%s replay=1", cache[(cacheWrite - offset + CACHE_LIMIT) % CACHE_LIMIT]);
    return Plugin_Handled;
}

public Action DamagePre(int victim, int &attacker, int &inflictor, float &damage, int &damageType, int &weapon,
    float damageForce[3], float damagePosition[3])
{
    int watch = WatchFor(victim);
    if (watch < 0) return Plugin_Continue;
    int depth = watches[watch].depth++;
    if (depth >= DAMAGE_DEPTH) return Plugin_Continue;
    int index = watch * DAMAGE_DEPTH + depth;
    Actor(contexts[index], attacker, weapon); contexts[index].id = ++damageId;
    contexts[index].beforeKnown = ReadInt(victim, "m_iHealth", contexts[index].beforeHealth);
    // Observation only: never alter any reference parameter, including damage.
    return Plugin_Continue;
}

public void DamagePost(int victim, int attacker, int inflictor, float damage, int damageType, int weapon,
    const float damageForce[3], const float damagePosition[3])
{
    int watch = WatchFor(victim, true);
    if (watch < 0 || watches[watch].depth <= 0) return;
    int depth = --watches[watch].depth;
    if (depth >= DAMAGE_DEPTH) return; // Deep recursion is not certified.
    int index = watch * DAMAGE_DEPTH + depth;
    DamageContext context; context = contexts[index];
    bool valid = context.slot == attacker && context.weaponRef == (weapon > MaxClients && IsValidEntity(weapon) ? EntIndexToEntRef(weapon) : INVALID_ENT_REFERENCE);
    context.valid = context.valid && valid;
    int health; bool known = !watches[watch].destroyed && ReadInt(victim, "m_iHealth", health);
    if (!known) health = -1;
    damageCount++;
    if (context.slot >= 1 && context.slot <= MaxClients && IsClientInGame(context.slot) && GetClientUserId(context.slot) == context.userid)
        clientDamages[context.slot]++;
    Observe("damage_post", watch, context, "damage_callback", true, damage, damageType, known, health);
}

public void BreakEvent(Event event, const char[] name, bool dontBroadcast)
{
    int entity = event.GetInt("entindex", -1), watch = WatchFor(entity, true);
    if (watch < 0) return;
    DamageContext context; Actor(context, GetClientOfUserId(event.GetInt("userid", 0)), -1);
    if (watches[watch].depth > 0 && watches[watch].depth <= DAMAGE_DEPTH)
        context.id = contexts[watch * DAMAGE_DEPTH + watches[watch].depth - 1].id;
    int health; bool known = !watches[watch].destroyed && ReadInt(entity, "m_iHealth", health);
    if (!known) health = -1;
    breakCount++; Observe(name, watch, context, "game_event", false, -1.0, -1, known, health);
}

public void OnEntityDestroyed(int entity)
{
    for (int watch = 0; watch < WATCH_LIMIT; watch++)
    {
        if (!watches[watch].used || watches[watch].destroyed || watches[watch].entity != entity) continue;
        DamageContext context; Actor(context, -1, -1); context.valid = false;
        if (watches[watch].depth > 0 && watches[watch].depth <= DAMAGE_DEPTH)
        { context = contexts[watch * DAMAGE_DEPTH + watches[watch].depth - 1]; }
        watches[watch].destroyed = true;
        destroyCount++; Observe("destroy", watch, context, "destruction_damage_context");
    }
}

public void WeaponFireObserved(Event event, const char[] name, bool dontBroadcast)
{
    int client = GetClientOfUserId(event.GetInt("userid", 0));
    if (client < 1 || client > MaxClients || !IsClientInGame(client)) return;
    DamageContext context; Actor(context, client, -1);
    char weapon[64]; event.GetString("weapon", weapon, sizeof(weapon), "unknown");
    fireCount++; clientFires[client]++;
    Observe("weapon_fire", -1, context, "game_event", false, -1.0, -1, false, -1, weapon);
}
