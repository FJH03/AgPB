// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#pragma semicolon 1
#pragma newdecls required

// Optional observations only. This plugin never creates clients, changes teams,
// writes properties, submits input, applies damage or fires synthetic events.
// An opposing target requires separate user authorization and core Radio guard
// verification. Binding or a clear trace is not combat acceptance evidence.
public Plugin myinfo = {
    name = "AgPB read-only combat probe",
    author = "AgPB",
    description = "Player identities, real combat events and observed damage",
    version = "1.0"
};

#define CACHE_LIMIT 128
#define LINE_SIZE 1024
#define DAMAGE_DEPTH 4

enum struct CombatIdentity
{
    int slot;
    int userid;
    int serial;
    int fake;
    int team;
    int alive;
}

enum struct DamageContext
{
    int id;
    CombatIdentity attacker;
    CombatIdentity victim;
    int weaponRef;
    char weaponClass[64];
    int health;
    bool healthKnown;
}

char epoch[40];
int mapSerial, sequence, watchGeneration, sampleSequence, damageSequence, damageOverflows;
bool watching;
CombatIdentity subject, human, watchedTarget;
bool fireEventHooked, hurtEventHooked, deathEventHooked;
bool bulletHooked[MAXPLAYERS + 1], damageHooked[MAXPLAYERS + 1];
int depth[MAXPLAYERS + 1];
DamageContext damageContexts[(MAXPLAYERS + 1) * DAMAGE_DEPTH];
int fires[MAXPLAYERS + 1], bulletCalls[MAXPLAYERS + 1];
int hurtsDealt[MAXPLAYERS + 1], hurtsTaken[MAXPLAYERS + 1];
int damageCalls[MAXPLAYERS + 1], deaths[MAXPLAYERS + 1];
char cache[CACHE_LIMIT][LINE_SIZE];
int cacheWrite, cacheCount;

bool ClientValid(int client)
{
    return client >= 1 && client <= MaxClients && IsClientInGame(client);
}

void ReadIdentity(CombatIdentity identity, int client, int rawUserid = 0)
{
    identity.slot = -1; identity.userid = rawUserid; identity.serial = -1;
    identity.fake = -1; identity.team = -1; identity.alive = -1;
    if (!ClientValid(client)) return;
    identity.slot = client; identity.userid = GetClientUserId(client);
    identity.serial = GetClientSerial(client); identity.fake = IsFakeClient(client) ? 1 : 0;
    identity.team = GetClientTeam(client); identity.alive = IsPlayerAlive(client) ? 1 : 0;
}

bool Matches(CombatIdentity identity, int client)
{
    return ClientValid(client) && identity.slot == client && identity.userid == GetClientUserId(client)
        && identity.serial == GetClientSerial(client);
}

bool Relevant(int client)
{
    return watching && (Matches(subject, client) || Matches(human, client) || Matches(watchedTarget, client));
}

bool ReadInt(int entity, const char[] property, int &value)
{
    value = -1;
    if (entity <= 0 || !IsValidEntity(entity)) return false;
    if (HasEntProp(entity, Prop_Send, property)) { value = GetEntProp(entity, Prop_Send, property); return true; }
    if (HasEntProp(entity, Prop_Data, property)) { value = GetEntProp(entity, Prop_Data, property); return true; }
    return false;
}

bool ReadFloat(int entity, const char[] property, float &value)
{
    value = -1.0;
    if (entity <= 0 || !IsValidEntity(entity)) return false;
    if (HasEntProp(entity, Prop_Send, property)) { value = GetEntPropFloat(entity, Prop_Send, property); return true; }
    if (HasEntProp(entity, Prop_Data, property)) { value = GetEntPropFloat(entity, Prop_Data, property); return true; }
    return false;
}

int ActiveWeapon(int client)
{
    if (!ClientValid(client) || !HasEntProp(client, Prop_Send, "m_hActiveWeapon")) return -1;
    return GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon");
}

int WeaponIdentity(int weapon, char[] classname, int size)
{
    strcopy(classname, size, "unknown");
    if (weapon <= MaxClients || !IsValidEntity(weapon)) return INVALID_ENT_REFERENCE;
    GetEntityClassname(weapon, classname, size);
    return EntIndexToEntRef(weapon);
}

void Remember(const char[] line)
{
    strcopy(cache[cacheWrite], LINE_SIZE, line);
    cacheWrite = (cacheWrite + 1) % CACHE_LIMIT;
    if (cacheCount < CACHE_LIMIT) cacheCount++;
    PrintToServer("%s replay=0", line);
}

// Each fresh observation has a unique (epoch, seq), including snapshot lines.
// Cached replays preserve that identity, tick, map and binding generation.
void EventLine(const char[] name, CombatIdentity actor, CombatIdentity victim, const char[] weapon,
    int weaponRef = INVALID_ENT_REFERENCE, int damageId = 0, int amount = -1,
    int armorDamage = -1, int health = -1, int armor = -1, int hitgroup = -1,
    int shots = -1, int headshot = -1, int inflictor = -1)
{
    char map[64], line[LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[WP_COMBAT_EVENT] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d generation=%d event=%s actor_slot=%d actor_userid=%d actor_serial=%d actor_fake=%d actor_team=%d actor_alive=%d victim_slot=%d victim_userid=%d victim_serial=%d victim_fake=%d victim_team=%d victim_alive=%d weapon=%s active_ref=%d damage_id=%d dmg_health=%d dmg_armor=%d health=%d armor=%d hitgroup=%d shots=%d headshot=%d inflictor=%d",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, watchGeneration, name,
        actor.slot, actor.userid, actor.serial, actor.fake, actor.team, actor.alive,
        victim.slot, victim.userid, victim.serial, victim.fake, victim.team, victim.alive,
        weapon, weaponRef, damageId, amount, armorDamage, health, armor, hitgroup, shots, headshot, inflictor);
    Remember(line);
}

void Marker(const char[] name)
{
    CombatIdentity none; ReadIdentity(none, -1);
    EventLine(name, none, none, "unknown");
}

bool ActivePlayer(int client)
{
    if (!ClientValid(client) || !IsPlayerAlive(client) || GetClientTeam(client) < 2) return false;
    int state;
    return ReadInt(client, "m_iPlayerState", state) && state == 0;
}

void SceneSnapshot(int sample)
{
    bool subjectPresent = Matches(subject, subject.slot), humanPresent = Matches(human, human.slot);
    bool targetPresent = Matches(watchedTarget, watchedTarget.slot);
    bool same = watching && subjectPresent && humanPresent && IsFakeClient(subject.slot)
        && !IsFakeClient(human.slot) && ActivePlayer(subject.slot) && ActivePlayer(human.slot)
        && GetClientTeam(subject.slot) == GetClientTeam(human.slot);
    int opposing, unboundOpposing;
    if (humanPresent)
        for (int i = 1; i <= MaxClients; i++)
            if (ClientValid(i) && IsFakeClient(i) && GetClientTeam(i) >= 2 && GetClientTeam(i) != GetClientTeam(human.slot))
            { opposing++; if (!Matches(watchedTarget, i)) unboundOpposing++; }
    bool targetOpposing = targetPresent && humanPresent && GetClientTeam(watchedTarget.slot) >= 2
        && GetClientTeam(watchedTarget.slot) != GetClientTeam(human.slot);
    bool bindingOk = same && unboundOpposing == 0 && (watchedTarget.userid == 0 || targetPresent)
        && (!targetOpposing || IsFakeClient(watchedTarget.slot));
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_COMBAT_SCENE] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d sample=%d generation=%d watching=%d subject_slot=%d subject_userid=%d subject_serial=%d subject_present=%d human_slot=%d human_userid=%d human_serial=%d human_present=%d target_slot=%d target_userid=%d target_serial=%d target_present=%d subject_human_ok=%d human_slot_lower=%d opposing_fake_count=%d unbound_opposing_count=%d target_opposing=%d scene_binding_ok=%d guard_verified=-1 external_authorization_verified=-1 replay=0",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, sample, watchGeneration, watching,
        subject.slot, subject.userid, subject.serial, subjectPresent, human.slot, human.userid, human.serial, humanPresent,
        watchedTarget.slot, watchedTarget.userid, watchedTarget.serial, targetPresent, same, human.slot < subject.slot, opposing, unboundOpposing, targetOpposing, bindingOk);
}

void Attach(int client)
{
    if (!watching || !ClientValid(client)) return;
    if (!bulletHooked[client]) bulletHooked[client] = SDKHookEx(client, SDKHook_FireBulletsPost, BulletsPost);
    if (damageHooked[client]) return;
    bool pre = SDKHookEx(client, SDKHook_OnTakeDamage, DamagePre);
    bool post = SDKHookEx(client, SDKHook_OnTakeDamagePost, DamagePost);
    damageHooked[client] = pre && post;
    if (!damageHooked[client])
    {
        if (pre) SDKUnhook(client, SDKHook_OnTakeDamage, DamagePre);
        if (post) SDKUnhook(client, SDKHook_OnTakeDamagePost, DamagePost);
    }
}

void Unwatch()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (ClientValid(i))
        {
            if (bulletHooked[i]) SDKUnhook(i, SDKHook_FireBulletsPost, BulletsPost);
            if (damageHooked[i])
            {
                SDKUnhook(i, SDKHook_OnTakeDamage, DamagePre);
                SDKUnhook(i, SDKHook_OnTakeDamagePost, DamagePost);
            }
        }
        bulletHooked[i] = false; damageHooked[i] = false; depth[i] = 0;
    }
    watching = false;
}

public void OnPluginStart()
{
    Format(epoch, sizeof(epoch), "%d-%08x-%08x", GetTime(), GetURandomInt(), GetURandomInt());
    ReadIdentity(subject, -1); ReadIdentity(human, -1); ReadIdentity(watchedTarget, -1);
    RegServerCmd("agpb_wp_probe_combat_watch", WatchCommand);
    RegServerCmd("agpb_wp_probe_combat", SnapshotCommand);
    RegServerCmd("agpb_wp_probe_combat_clear", ClearCommand);
    fireEventHooked = HookEventEx("weapon_fire", CombatEvent);
    hurtEventHooked = HookEventEx("player_hurt", CombatEvent);
    deathEventHooked = HookEventEx("player_death", CombatEvent);
    HookEventEx("player_spawn", CombatEvent);
    HookEventEx("player_team", CombatEvent);
    HookEventEx("round_start", RoundEvent);
    HookEventEx("round_end", RoundEvent);
    Marker("plugin_start");
}

public void OnMapStart()
{
    mapSerial++;
    // Never follow client slots or a scene binding across maps.
    Unwatch();
    Marker("map_start");
}

public void OnMapEnd() { Marker("map_end"); Unwatch(); }
public void OnPluginEnd() { Unwatch(); }

public void OnClientPutInServer(int client)
{
    fires[client] = 0; bulletCalls[client] = 0; hurtsDealt[client] = 0;
    hurtsTaken[client] = 0; damageCalls[client] = 0; deaths[client] = 0;
    bulletHooked[client] = false; damageHooked[client] = false; depth[client] = 0;
    Attach(client);
}

public void OnClientDisconnect(int client)
{
    if (Relevant(client))
    {
        CombatIdentity actor, none; ReadIdentity(actor, client); ReadIdentity(none, -1);
        EventLine("client_disconnect", actor, none, "unknown");
    }
    bulletHooked[client] = false; damageHooked[client] = false; depth[client] = 0;
}

bool ParseClient(int argument, int &client)
{
    char text[24]; GetCmdArg(argument, text, sizeof(text));
    int used = StringToIntEx(text, client);
    return used > 0 && used == strlen(text) && ClientValid(client);
}

public Action WatchCommand(int args)
{
    if (args != 2 && args != 3)
    { PrintToServer("[WP_COMBAT] usage: agpb_wp_probe_combat_watch subject_bot_slot human_slot [target_slot]"); return Plugin_Handled; }
    int bot, observer, opponent = -1;
    if (!ParseClient(1, bot) || !ParseClient(2, observer) || (args == 3 && !ParseClient(3, opponent)))
    { PrintToServer("[WP_COMBAT] rejected: select existing in-game player slots"); return Plugin_Handled; }
    if (observer >= bot || opponent == bot || opponent == observer || !IsFakeClient(bot) || IsFakeClient(observer)
        || !ActivePlayer(bot) || !ActivePlayer(observer) || GetClientTeam(bot) != GetClientTeam(observer))
    { PrintToServer("[WP_COMBAT] rejected: subject must be an alive ACTIVE fake with an alive ACTIVE same-team lower-slot real human; target must be distinct"); return Plugin_Handled; }
    if (opponent != -1 && (!ActivePlayer(opponent) || !IsFakeClient(opponent)))
    { PrintToServer("[WP_COMBAT] rejected: optional target must be an existing alive ACTIVE fake client"); return Plugin_Handled; }
    for (int i = 1; i <= MaxClients; i++)
        if (ClientValid(i) && IsFakeClient(i) && GetClientTeam(i) >= 2 && GetClientTeam(i) != GetClientTeam(observer) && i != opponent)
        { PrintToServer("[WP_COMBAT] rejected: an opposing fake is not the explicitly bound target"); return Plugin_Handled; }
    Unwatch();
    ReadIdentity(subject, bot); ReadIdentity(human, observer); ReadIdentity(watchedTarget, opponent);
    watchGeneration++; watching = true;
    for (int i = 1; i <= MaxClients; i++) Attach(i);
    Marker("watch_bound");
    PrintToServer("[WP_COMBAT] observation only; any opposing scene requires separate authorization and verified core Radio guard");
    SnapshotCommand(0);
    return Plugin_Handled;
}

public Action ClearCommand(int args)
{
    if (args != 0) { PrintToServer("[WP_COMBAT] usage: agpb_wp_probe_combat_clear"); return Plugin_Handled; }
    Marker("watch_clear"); Unwatch();
    // Keep event cache, counters, sequence and original identities for attribution.
    PrintToServer("[WP_COMBAT] observations detached; no player or game setting was modified");
    return Plugin_Handled;
}

void PlayerSnapshot(int client, int sample)
{
    int health, armor, state, life;
    bool healthKnown = ReadInt(client, "m_iHealth", health), armorKnown = ReadInt(client, "m_ArmorValue", armor);
    bool stateKnown = ReadInt(client, "m_iPlayerState", state), lifeKnown = ReadInt(client, "m_lifeState", life);
    float position[3], eye[3], angles[3];
    GetClientAbsOrigin(client, position); GetClientEyePosition(client, eye); GetClientEyeAngles(client, angles);
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_COMBAT_PLAYER] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d sample=%d generation=%d slot=%d userid=%d serial=%d fake=%d team=%d alive=%d subject=%d human=%d target=%d health_known=%d health=%d armor_known=%d armor=%d state_known=%d player_state=%d life_known=%d life_state=%d pos=(%.6f %.6f %.6f) eye=(%.6f %.6f %.6f) angles=(%.6f %.6f %.6f) fires=%d bullet_calls=%d hurts_dealt=%d hurts_taken=%d damage_calls=%d deaths=%d bullets_hooked=%d damage_hooked=%d replay=0",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, sample, watchGeneration,
        client, GetClientUserId(client), GetClientSerial(client), IsFakeClient(client), GetClientTeam(client), IsPlayerAlive(client),
        Matches(subject, client), Matches(human, client), Matches(watchedTarget, client), healthKnown, health, armorKnown, armor,
        stateKnown, state, lifeKnown, life, position[0], position[1], position[2], eye[0], eye[1], eye[2],
        angles[0], angles[1], angles[2], fires[client], bulletCalls[client], hurtsDealt[client], hurtsTaken[client],
        damageCalls[client], deaths[client], bulletHooked[client], damageHooked[client]);
    int active = ActiveWeapon(client); char classname[64];
    int reference = WeaponIdentity(active, classname, sizeof(classname));
    int clip, ammoType, reserve = -1, reload;
    bool clipKnown = ReadInt(active, "m_iClip1", clip), typeKnown = ReadInt(active, "m_iPrimaryAmmoType", ammoType);
    bool reloadKnown = ReadInt(active, "m_bInReload", reload), reserveKnown;
    if (typeKnown && ammoType >= 0 && HasEntProp(client, Prop_Send, "m_iAmmo")
        && ammoType < GetEntPropArraySize(client, Prop_Send, "m_iAmmo"))
    { reserve = GetEntProp(client, Prop_Send, "m_iAmmo", _, ammoType); reserveKnown = true; }
    float primary, secondary, playerAttack, smack;
    bool primaryKnown = ReadFloat(active, "m_flNextPrimaryAttack", primary);
    bool secondaryKnown = ReadFloat(active, "m_flNextSecondaryAttack", secondary);
    bool attackKnown = ReadFloat(client, "m_flNextAttack", playerAttack), smackKnown = ReadFloat(active, "m_flSmackTime", smack);
    PrintToServer("[WP_COMBAT_WEAPON] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d sample=%d generation=%d slot=%d userid=%d serial=%d active_ent=%d active_ref=%d class=%s clip_known=%d clip=%d ammotype_known=%d ammotype=%d reserve_known=%d reserve=%d reload_known=%d reload=%d primary_known=%d primary=%.6f secondary_known=%d secondary=%.6f player_attack_known=%d player_attack=%.6f smack_known=%d smack=%.6f replay=0",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, sample, watchGeneration,
        client, GetClientUserId(client), GetClientSerial(client), active, reference, classname,
        clipKnown, clip, typeKnown, ammoType, reserveKnown, reserve, reloadKnown, reload,
        primaryKnown, primary, secondaryKnown, secondary, attackKnown, playerAttack, smackKnown, smack);
}

public bool TraceFilter(int entity, int contentsMask, any shooter) { return entity != shooter; }

void TraceSnapshot(int shooter, int victim, int sample, bool cacheEvent = false)
{
    if (!ClientValid(shooter) || !ClientValid(victim) || !IsPlayerAlive(shooter) || !IsPlayerAlive(victim)) return;
    float from[3], to[3]; GetClientEyePosition(shooter, from); GetClientEyePosition(victim, to);
    Handle trace = TR_TraceRayFilterEx(from, to, MASK_SHOT, RayType_EndPoint, TraceFilter, shooter);
    int hit = TR_GetEntityIndex(trace);
    int reference = hit > 0 && IsValidEntity(hit) ? EntIndexToEntRef(hit) : INVALID_ENT_REFERENCE;
    bool didHit = TR_DidHit(trace), clear = !TR_StartSolid(trace) && !TR_AllSolid(trace) && (!didHit || hit == victim);
    char map[64], line[LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[WP_COMBAT_TRACE] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d sample=%d generation=%d shooter_slot=%d shooter_userid=%d shooter_serial=%d target_slot=%d target_userid=%d target_serial=%d mask=%d endpoint=eye from=(%.6f %.6f %.6f) to=(%.6f %.6f %.6f) did_hit=%d hit_ent=%d hit_ref=%d fraction=%.6f startsolid=%d allsolid=%d unobstructed=%d perception_verified=0",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, sample, watchGeneration,
        shooter, GetClientUserId(shooter), GetClientSerial(shooter), victim, GetClientUserId(victim), GetClientSerial(victim),
        MASK_SHOT, from[0], from[1], from[2], to[0], to[1], to[2], didHit, hit, reference, TR_GetFraction(trace),
        TR_StartSolid(trace), TR_AllSolid(trace), clear);
    delete trace;
    if (cacheEvent) Remember(line); else PrintToServer("%s replay=0", line);
}

public Action SnapshotCommand(int args)
{
    if (args != 0) { PrintToServer("[WP_COMBAT] usage: agpb_wp_probe_combat"); return Plugin_Handled; }
    int sample = ++sampleSequence;
    SceneSnapshot(sample);
    for (int i = 1; i <= MaxClients; i++) if (ClientValid(i)) PlayerSnapshot(i, sample);
    if (watching && Matches(subject, subject.slot) && Matches(watchedTarget, watchedTarget.slot))
    { TraceSnapshot(subject.slot, watchedTarget.slot, sample); TraceSnapshot(watchedTarget.slot, subject.slot, sample); }
    char map[64]; GetCurrentMap(map, sizeof(map));
    PrintToServer("[WP_COMBAT_COUNTS] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d sample=%d generation=%d cached=%d cache_limit=%d weapon_fire_hooked=%d player_hurt_hooked=%d player_death_hooked=%d damage_seq=%d damage_overflows=%d replay=0",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, sample, watchGeneration,
        cacheCount, CACHE_LIMIT, fireEventHooked, hurtEventHooked, deathEventHooked, damageSequence, damageOverflows);
    for (int offset = cacheCount; offset > 0; offset--)
        PrintToServer("%s replay=1", cache[(cacheWrite - offset + CACHE_LIMIT) % CACHE_LIMIT]);
    return Plugin_Handled;
}

public void CombatEvent(Event event, const char[] name, bool dontBroadcast)
{
    if (!watching) return;
    int userid = event.GetInt("userid", 0), client = GetClientOfUserId(userid);
    int attackerUserid = event.GetInt("attacker", 0), attacker = GetClientOfUserId(attackerUserid);
    bool fire = StrEqual(name, "weapon_fire");
    if (!Relevant(client) && !Relevant(attacker)) return;
    CombatIdentity actor, victim;
    ReadIdentity(actor, fire ? client : attacker, fire ? userid : attackerUserid);
    ReadIdentity(victim, fire ? -1 : client, fire ? 0 : userid);
    int active = ActiveWeapon(actor.slot); char classname[64], weapon[64];
    int reference = WeaponIdentity(active, classname, sizeof(classname));
    event.GetString("weapon", weapon, sizeof(weapon), "unknown");
    if (fire && ClientValid(client)) fires[client]++;
    if (StrEqual(name, "player_hurt"))
    { if (ClientValid(client)) hurtsTaken[client]++; if (ClientValid(attacker)) hurtsDealt[attacker]++; }
    if (StrEqual(name, "player_death") && ClientValid(client)) deaths[client]++;
    int damageId;
    if (ClientValid(client) && depth[client] > 0 && depth[client] <= DAMAGE_DEPTH)
        damageId = damageContexts[client * DAMAGE_DEPTH + depth[client] - 1].id;
    EventLine(name, actor, victim, weapon, reference, damageId, event.GetInt("dmg_health", -1),
        event.GetInt("dmg_armor", -1), event.GetInt("health", -1), event.GetInt("armor", -1),
        event.GetInt("hitgroup", -1), -1, StrEqual(name, "player_death") ? (event.GetBool("headshot", false) ? 1 : 0) : -1);
    if (StrEqual(name, "player_team"))
    {
        char line[LINE_SIZE], map[64]; GetCurrentMap(map, sizeof(map));
        Format(line, sizeof(line), "[WP_COMBAT_TEAM] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d generation=%d userid=%d slot=%d serial=%d oldteam=%d newteam=%d",
            epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, watchGeneration,
            userid, client, ClientValid(client) ? GetClientSerial(client) : -1, event.GetInt("oldteam", -1), event.GetInt("team", -1));
        Remember(line);
    }
    if (fire && Matches(subject, client) && Matches(watchedTarget, watchedTarget.slot)) TraceSnapshot(client, watchedTarget.slot, 0, true);
}

public void RoundEvent(Event event, const char[] name, bool dontBroadcast) { Marker(name); }

public void BulletsPost(int client, int shots, const char[] weaponname)
{
    if (!Relevant(client)) return;
    CombatIdentity actor, none; ReadIdentity(actor, client); ReadIdentity(none, -1);
    int active = ActiveWeapon(client); char classname[64];
    int reference = WeaponIdentity(active, classname, sizeof(classname));
    bulletCalls[client]++;
    // The engine callback confirms a FireBullets call, not a hit or HP loss.
    EventLine("firebullets_post", actor, none, weaponname, reference, 0, -1, -1, -1, -1, -1, shots);
}

public Action DamagePre(int victim, int &attacker, int &inflictor, float &damage, int &damageType,
    int &weapon, float damageForce[3], float damagePosition[3])
{
    if (!ClientValid(victim)) return Plugin_Continue;
    int level = depth[victim]++;
    if (level >= DAMAGE_DEPTH)
    {
        if (Relevant(victim) || Relevant(attacker)) { damageOverflows++; Marker("damage_depth_overflow"); }
        return Plugin_Continue;
    }
    int index = victim * DAMAGE_DEPTH + level;
    damageContexts[index].id = 0;
    if (!Relevant(victim) && !Relevant(attacker)) return Plugin_Continue;
    damageContexts[index].id = ++damageSequence;
    ReadIdentity(damageContexts[index].attacker, attacker); ReadIdentity(damageContexts[index].victim, victim);
    damageContexts[index].weaponRef = WeaponIdentity(weapon, damageContexts[index].weaponClass, sizeof(damageContexts[].weaponClass));
    damageContexts[index].healthKnown = ReadInt(victim, "m_iHealth", damageContexts[index].health);
    // Never change callback reference parameters. A requested amount does not
    // prove damage; the post health and player_hurt event supply that evidence.
    DamageLine("damage_pre", damageContexts[index], attacker, weapon, damage, damageType, inflictor);
    return Plugin_Continue;
}

void DamageLine(const char[] name, DamageContext context, int attacker, int weapon, float damage, int damageType, int inflictor)
{
    int health; bool known = Matches(context.victim, context.victim.slot) && ReadInt(context.victim.slot, "m_iHealth", health);
    if (!known) health = -1;
    char classname[64]; int reference = WeaponIdentity(weapon, classname, sizeof(classname));
    bool valid = Matches(context.victim, context.victim.slot)
        && (context.attacker.slot == -1 ? !ClientValid(attacker) : Matches(context.attacker, attacker))
        && context.weaponRef == reference;
    char map[64], line[LINE_SIZE]; GetCurrentMap(map, sizeof(map));
    Format(line, sizeof(line), "[WP_COMBAT_DAMAGE] v=1 epoch=%s map_serial=%d tick=%d t=%.6f map=%s seq=%d generation=%d event=%s damage_id=%d context_valid=%d attacker_slot=%d attacker_userid=%d attacker_serial=%d attacker_fake=%d attacker_team=%d victim_slot=%d victim_userid=%d victim_serial=%d victim_fake=%d victim_team=%d weapon_ref=%d weapon_class=%s callback_weapon_ref=%d callback_weapon_class=%s inflictor=%d amount=%.6f damagetype=%d health_before_known=%d health_before=%d health_now_known=%d health_now=%d",
        epoch, mapSerial, GetGameTickCount(), GetGameTime(), map, ++sequence, watchGeneration,
        name, context.id, valid, context.attacker.slot, context.attacker.userid, context.attacker.serial, context.attacker.fake,
        context.attacker.team, context.victim.slot, context.victim.userid, context.victim.serial, context.victim.fake, context.victim.team,
        context.weaponRef, context.weaponClass, reference, classname, inflictor, damage, damageType,
        context.healthKnown, context.health, known, health);
    Remember(line);
}

public void DamagePost(int victim, int attacker, int inflictor, float damage, int damageType, int weapon,
    const float damageForce[3], const float damagePosition[3])
{
    if (victim < 1 || victim > MaxClients || depth[victim] <= 0) return;
    int level = --depth[victim];
    if (level >= DAMAGE_DEPTH) return;
    int index = victim * DAMAGE_DEPTH + level;
    if (!damageContexts[index].id) return;
    if (Matches(damageContexts[index].victim, victim)) damageCalls[victim]++;
    DamageLine("damage_post", damageContexts[index], attacker, weapon, damage, damageType, inflictor);
}
