// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#include <cstrike>
#pragma semicolon 1
#pragma newdecls required

// Test-only scene preparation. Never include this fixture in the AgPB package.
// Every command requires sv_cheats. The original human commands still reject
// fake clients. Explicit lifecycle commands may prepare a same-team test bot via
// ordinary game suicide/respawn/team paths and a finite point_viewcontrol. They
// never inject bot commands or write player/bomb properties or synthetic events.
public Plugin myinfo = {
    name = "AgPB regression scene fixture",
    author = "AgPB",
    description = "Cheat-gated human and explicit lifecycle test preparation",
    version = "1.2"
};
int plantClient;
int plantUserId;
float plantDeadline;
int dropClient;
int dropUserId;
int dropReference = INVALID_ENT_REFERENCE;
float dropTarget[3];
float dropDeadline;
bool dropIssued;
int freezeClient;
int freezeUserId;
int freezeHuman;
int freezeHumanUserId;
int freezeCameraRef = INVALID_ENT_REFERENCE;
float freezeDeadline;
bool freezePending;
float freezeDuration;
int clearanceGeneration;
int clearanceBot, clearanceUserId, clearanceSerial;
int clearanceHuman, clearanceHumanUserId, clearanceHumanSerial;
int clearanceCoreIndex, clearanceMode, clearanceArmTick, clearanceTriggerTick;
float clearanceDeadline;
bool clearancePending;
char clearanceWeapon[64];

// These flags are documented by CTriggerCamera in the local engine source:
// PLAYER_POSITION (1), PLAYER_TARGET (2), PLAYER_TAKECONTROL (4). Enable calls
// CBasePlayer::EnableControl(false), which adds the real engine FL_FROZEN flag.
// No INFINITE_WAIT flag: the entity and the independent watchdog both expire.
#define LIFECYCLE_CAMERA_FLAGS "7"

bool CheatsEnabled()
{
    ConVar cheats = FindConVar("sv_cheats");
    return cheats != null && cheats.BoolValue;
}

bool RequireCheats()
{
    if (CheatsEnabled()) return true;
    PrintToServer("[AGPB_REG] rejected: sv_cheats must be enabled");
    return false;
}

bool HumanValid(int client, bool alive)
{
    return client >= 1 && client <= MaxClients && IsClientInGame(client) &&
        !IsFakeClient(client) && (!alive || IsPlayerAlive(client));
}

bool PlayingFakeExists(int opposingTo = 0)
{
    for (int client = 1; client <= MaxClients; client++)
    {
        if (!IsClientInGame(client) || !IsFakeClient(client)) continue;
        int team = GetClientTeam(client);
        if ((team == CS_TEAM_T || team == CS_TEAM_CT) && (opposingTo == 0 || team != opposingTo)) return true;
    }
    return false;
}

bool RequireHuman(int client, bool alive)
{
    if (!HumanValid(client, alive))
    {
        PrintToServer("[AGPB_REG] rejected: slot must identify an in-game human%s", alive ? " who is alive" : "");
        return false;
    }
    return true;
}

bool RequireSafeScene(int client)
{
    int team = GetClientTeam(client);
    if (team != CS_TEAM_T && team != CS_TEAM_CT)
    {
        PrintToServer("[AGPB_REG] rejected: human must be on T or CT");
        return false;
    }
    if (PlayingFakeExists(team))
    {
        PrintToServer("[AGPB_REG] rejected: all playing bots must share the human team");
        return false;
    }
    return true;
}

int ClientArg()
{
    char text[24]; GetCmdArg(1, text, sizeof(text));
    return StringToInt(text);
}

int SlotArg(int arg)
{
    char text[24]; GetCmdArg(arg, text, sizeof(text));
    int value;
    int used = StringToIntEx(text, value);
    return used == strlen(text) && used > 0 ? value : 0;
}

int ViewEntity(int client)
{
    return HasEntProp(client, Prop_Data, "m_hViewEntity") ?
        GetEntPropEnt(client, Prop_Data, "m_hViewEntity") : -1;
}

bool FreezeIdentityValid()
{
    return freezeClient >= 1 && freezeClient <= MaxClients && IsClientInGame(freezeClient) &&
        IsFakeClient(freezeClient) && GetClientUserId(freezeClient) == freezeUserId;
}

bool RequireLifecycleScene(int bot, int human, bool observerBot = false)
{
    if (!RequireHuman(human, true) || !RequireSafeScene(human)) return false;
    if (bot < 1 || bot > MaxClients || !IsClientInGame(bot) || !IsFakeClient(bot))
    {
        PrintToServer("[AGPB_REG] rejected: lifecycle target must identify an in-game fake client");
        return false;
    }
    int team = GetClientTeam(human);
    for (int client = 1; client <= MaxClients; client++)
    {
        if (!IsClientInGame(client)) continue;
        int otherTeam = GetClientTeam(client);
        if ((otherTeam == CS_TEAM_T || otherTeam == CS_TEAM_CT) && otherTeam != team)
        {
            PrintToServer("[AGPB_REG] rejected: all playing clients must share the anchor human team");
            return false;
        }
    }
    if (observerBot ? GetClientTeam(bot) != CS_TEAM_SPECTATOR : GetClientTeam(bot) != team)
    {
        PrintToServer("[AGPB_REG] rejected: lifecycle bot must be %s", observerBot ? "a spectator before SAME" : "on the anchor human team");
        return false;
    }
    if (GetEntityFlags(human) & FL_FROZEN)
    {
        PrintToServer("[AGPB_REG] rejected: anchor human must not be frozen");
        return false;
    }
    return true;
}

bool RequireNoLifecycleFreeze()
{
    if (freezeUserId == 0) return true;
    PrintToServer("[AGPB_REG] rejected: clear or wait for the active lifecycle viewcontrol before changing life/team");
    return false;
}

bool SingleAgPBFake(int bot)
{
    if (bot < 1 || bot > MaxClients || !IsClientInGame(bot) || !IsFakeClient(bot)) return false;
    int count;
    for (int client = 1; client <= MaxClients; client++)
        if (IsClientInGame(client) && IsFakeClient(client)) count++;
    char name[64]; GetClientName(bot, name, sizeof(name));
    return count == 1 && StrContains(name, "AgPB_", false) == 0;
}

bool ClearanceArmValid()
{
    return clearanceUserId != 0 && CheatsEnabled() && freezeUserId == 0 &&
        SingleAgPBFake(clearanceBot) && IsPlayerAlive(clearanceBot) &&
        GetClientUserId(clearanceBot) == clearanceUserId && GetClientSerial(clearanceBot) == clearanceSerial &&
        HumanValid(clearanceHuman, true) && GetClientUserId(clearanceHuman) == clearanceHumanUserId &&
        GetClientSerial(clearanceHuman) == clearanceHumanSerial &&
        GetClientTeam(clearanceHuman) >= CS_TEAM_T && GetClientTeam(clearanceHuman) <= CS_TEAM_CT &&
        GetClientTeam(clearanceBot) == GetClientTeam(clearanceHuman) && !PlayingFakeExists(GetClientTeam(clearanceHuman)) &&
        (GetEntityFlags(clearanceBot) & FL_FROZEN) == 0 && (GetEntityFlags(clearanceHuman) & FL_FROZEN) == 0 &&
        GetGameTime() < clearanceDeadline;
}

void ResetClearanceArm()
{
    clearanceGeneration++;
    clearanceBot = 0; clearanceUserId = 0; clearanceSerial = 0;
    clearanceHuman = 0; clearanceHumanUserId = 0; clearanceHumanSerial = 0;
    clearanceCoreIndex = 0; clearanceMode = 0; clearanceDeadline = 0.0; clearancePending = false;
    clearanceArmTick = 0; clearanceTriggerTick = 0; clearanceWeapon[0] = '\0';
}

void CancelClearanceArm(const char[] reason)
{
    if (clearanceUserId == 0) return;
    PrintToServer("[AGPB_REG_CLEARANCE] v=1 action=cancelled arm=%d mode=%s t=%.3f tick=%d arm_tick=%d trigger_tick=%d slot=%d userid=%d serial=%d human_slot=%d human_userid=%d human_serial=%d core_idx=%d reason=\"%s\"",
        clearanceGeneration, clearanceMode == 1 ? "stop" : "death", GetGameTime(), GetGameTickCount(),
        clearanceArmTick, clearanceTriggerTick, clearanceBot, clearanceUserId, clearanceSerial,
        clearanceHuman, clearanceHumanUserId, clearanceHumanSerial, clearanceCoreIndex, reason);
    ResetClearanceArm();
}

public Action ClearanceArm(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 4) { PrintToServer("[AGPB_REG] usage: agpb_reg_clearance_arm stop|death bot_slot human_slot core_idx"); return Plugin_Handled; }
    char mode[16]; GetCmdArg(1, mode, sizeof(mode));
    int action;
    if (StrEqual(mode, "stop", false)) action = 1;
    else if (StrEqual(mode, "death", false)) action = 2;
    else { PrintToServer("[AGPB_REG] rejected: clearance action must be stop or death"); return Plugin_Handled; }
    int bot = SlotArg(2), human = SlotArg(3);
    char text[24]; GetCmdArg(4, text, sizeof(text)); int coreIndex;
    int parsed = StringToIntEx(text, coreIndex);
    // With exactly one fake client named AgPB_*, the core's only list index is
    // zero. Never accept an arbitrary index that could stop a different object.
    if (parsed <= 0 || parsed != strlen(text) || coreIndex != 0 || !SingleAgPBFake(bot))
    { PrintToServer("[AGPB_REG] rejected: arm requires exactly one AgPB fake client and core_idx 0"); return Plugin_Handled; }
    if (!RequireNoLifecycleFreeze() || !RequireLifecycleScene(bot, human) || !IsPlayerAlive(bot) || (GetEntityFlags(bot) & FL_FROZEN) != 0)
        return Plugin_Handled;
    CancelClearanceArm("replaced request"); ResetClearanceArm();
    clearanceBot = bot; clearanceUserId = GetClientUserId(bot); clearanceSerial = GetClientSerial(bot);
    clearanceHuman = human; clearanceHumanUserId = GetClientUserId(human); clearanceHumanSerial = GetClientSerial(human);
    clearanceCoreIndex = coreIndex; clearanceMode = action; clearanceArmTick = GetGameTickCount();
    clearanceDeadline = GetGameTime() + 10.0;
    PrintToServer("[AGPB_REG_CLEARANCE] v=1 action=armed arm=%d mode=%s t=%.3f tick=%d slot=%d userid=%d serial=%d human_slot=%d human_userid=%d human_serial=%d core_idx=%d deadline=%.3f timeout=10.000 trigger=weapon_fire dispatch=RequestFrame",
        clearanceGeneration, mode, GetGameTime(), clearanceArmTick, bot, clearanceUserId, clearanceSerial,
        human, clearanceHumanUserId, clearanceHumanSerial, coreIndex, clearanceDeadline);
    return Plugin_Handled;
}

public void ClearanceWeaponFire(Event event, const char[] name, bool dontBroadcast)
{
    if (clearanceUserId == 0 || clearancePending || event.GetInt("userid", 0) != clearanceUserId) return;
    if (!ClearanceArmValid()) { CancelClearanceArm("weapon event identity/safety/timeout"); return; }
    clearancePending = true; clearanceTriggerTick = GetGameTickCount();
    event.GetString("weapon", clearanceWeapon, sizeof(clearanceWeapon), "unknown");
    PrintToServer("[AGPB_REG_CLEARANCE] v=1 action=triggered arm=%d mode=%s t=%.3f tick=%d arm_tick=%d slot=%d userid=%d serial=%d human_slot=%d human_userid=%d human_serial=%d core_idx=%d event=weapon_fire weapon=%s",
        clearanceGeneration, clearanceMode == 1 ? "stop" : "death", GetGameTime(), clearanceTriggerTick,
        clearanceArmTick, clearanceBot, clearanceUserId, clearanceSerial, clearanceHuman,
        clearanceHumanUserId, clearanceHumanSerial, clearanceCoreIndex, clearanceWeapon);
    // The public event is emitted before PrimaryAttack. Never stop/kill inside
    // that event or a damage callback; observe and act from the next frame.
    RequestFrame(ClearanceNextFrame, clearanceGeneration);
}

public void ClearanceNextFrame(any generation)
{
    if (generation != clearanceGeneration || clearanceUserId == 0 || !clearancePending) return;
    if (!ClearanceArmValid()) { CancelClearanceArm("next-frame identity/safety/timeout"); return; }
    // Another MMS GameFrame hook can fire the event before SourceMod drains its
    // queue in the same engine tick. Keep the requested next-tick separation.
    if (GetGameTickCount() <= clearanceTriggerTick)
    {
        RequestFrame(ClearanceNextFrame, generation);
        return;
    }
    int snapshotTick = GetGameTickCount();
    PrintToServer("[AGPB_REG_CLEARANCE] v=1 action=before arm=%d mode=%s t=%.3f tick=%d arm_tick=%d trigger_tick=%d slot=%d userid=%d serial=%d human_slot=%d human_userid=%d human_serial=%d core_idx=%d weapon=%s",
        clearanceGeneration, clearanceMode == 1 ? "stop" : "death", GetGameTime(), snapshotTick,
        clearanceArmTick, clearanceTriggerTick, clearanceBot, clearanceUserId, clearanceSerial,
        clearanceHuman, clearanceHumanUserId, clearanceHumanSerial, clearanceCoreIndex, clearanceWeapon);
    ServerCommand("agpb_bot_info %d; agpb_bot_clearance %d; agpb_wp_probe_state; agpb_wp_probe_clearance %d",
        clearanceCoreIndex, clearanceCoreIndex, clearanceBot);
    ServerExecute();
    // Queue execution can process other native commands. Revalidate before the
    // one requested mutation; diagnostics never manufacture an active break.
    if (!ClearanceArmValid() || generation != clearanceGeneration || GetGameTickCount() != snapshotTick)
    { CancelClearanceArm("post-snapshot identity/safety/tick changed"); return; }
    if (clearanceMode == 1)
    {
        ServerCommand("agpb_bot_stop %d", clearanceCoreIndex);
        ServerExecute();
    }
    else ForcePlayerSuicide(clearanceBot);
    PrintToServer("[AGPB_REG_CLEARANCE] v=1 action=done arm=%d mode=%s t=%.3f tick=%d arm_tick=%d trigger_tick=%d snapshot_tick=%d slot=%d userid=%d serial=%d human_slot=%d human_userid=%d human_serial=%d core_idx=%d alive=%d weapon=%s",
        clearanceGeneration, clearanceMode == 1 ? "stop" : "death", GetGameTime(), GetGameTickCount(),
        clearanceArmTick, clearanceTriggerTick, snapshotTick, clearanceBot, clearanceUserId, clearanceSerial,
        clearanceHuman, clearanceHumanUserId, clearanceHumanSerial, clearanceCoreIndex,
        IsPlayerAlive(clearanceBot), clearanceWeapon);
    ResetClearanceArm();
}

void ClearFreeze(const char[] reason)
{
    if (freezeUserId == 0) return;
    int camera = EntRefToEntIndex(freezeCameraRef);
    bool sameIdentity = FreezeIdentityValid();
    // Disable uses the engine's restoration path (view, controls, damage and
    // viewmodel). Do not repair flags/netvars if the engine cannot restore them.
    bool disabled;
    if (camera > MaxClients && IsValidEntity(camera))
    {
        // A second external viewcontrol may already have disabled this camera.
        // Never let stale cleanup overwrite that other controller's view.
        if (!sameIdentity || ViewEntity(freezeClient) == camera)
            disabled = AcceptEntityInput(camera, "Disable");
        AcceptEntityInput(camera, "Kill");
    }
    int flags = sameIdentity ? GetEntityFlags(freezeClient) : -1;
    int frozen = sameIdentity ? ((flags & FL_FROZEN) != 0 ? 1 : 0) : -1;
    PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=freeze_end t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d camera_ref=%d disable=%d flags=%d frozen=%d view_ent=%d pending=%d reason=\"%s\"",
        GetGameTime(), GetGameTickCount(), freezeClient, freezeUserId, freezeHuman, freezeHumanUserId,
        freezeCameraRef, disabled, flags, frozen, sameIdentity ? ViewEntity(freezeClient) : -1, freezePending, reason);
    freezeClient = 0; freezeUserId = 0; freezeHuman = 0; freezeHumanUserId = 0;
    freezeCameraRef = INVALID_ENT_REFERENCE; freezeDeadline = 0.0;
    freezePending = false; freezeDuration = 0.0;
}

bool FloatArg(int arg, float &value)
{
    char text[64]; GetCmdArg(arg, text, sizeof(text));
    int used = StringToFloatEx(text, value);
    // Reject NaN/infinity as well as partially parsed arguments.
    return used == strlen(text) && used > 0 && value >= -1000000.0 && value <= 1000000.0;
}

bool ExistingHumanC4(int client, int &entity)
{
    entity = GetPlayerWeaponSlot(client, CS_SLOT_C4);
    if (entity <= MaxClients || !IsValidEntity(entity)) return false;
    char classname[64]; GetEntityClassname(entity, classname, sizeof(classname));
    return StrEqual(classname, "weapon_c4") && HasEntProp(entity, Prop_Send, "m_hOwnerEntity") &&
        GetEntPropEnt(entity, Prop_Send, "m_hOwnerEntity") == client;
}

void ClearPlant(const char[] reason)
{
    if (plantUserId != 0)
    {
        PrintToServer("[AGPB_REG] human plant input stopped: userid=%d reason=%s", plantUserId, reason);
    }
    plantClient = 0; plantUserId = 0; plantDeadline = 0.0;
}

void ClearDrop(const char[] reason)
{
    if (dropUserId != 0)
        PrintToServer("[AGPB_REG] dropped-C4 preparation ended: userid=%d reason=%s", dropUserId, reason);
    dropClient = 0; dropUserId = 0; dropReference = INVALID_ENT_REFERENCE;
    dropDeadline = 0.0; dropIssued = false;
}

public void OnPluginStart()
{
    RegServerCmd("agpb_reg_place_human", PlaceHuman);
    RegServerCmd("agpb_reg_human_plant", HumanPlant);
    RegServerCmd("agpb_reg_drop_c4", DropC4);
    RegServerCmd("agpb_reg_human_team", HumanTeam);
    RegServerCmd("agpb_reg_lifecycle_freeze", LifecycleFreeze);
    RegServerCmd("agpb_reg_lifecycle_kill", LifecycleKill);
    RegServerCmd("agpb_reg_lifecycle_respawn", LifecycleRespawn);
    RegServerCmd("agpb_reg_lifecycle_team", LifecycleTeam);
    RegServerCmd("agpb_reg_clearance_arm", ClearanceArm);
    RegServerCmd("agpb_reg_clear", Clear);
    HookEvent("bomb_planted", Planted);
    HookEvent("weapon_fire", ClearanceWeaponFire);
    CreateTimer(0.1, Watch, _, TIMER_REPEAT);
}

public void OnMapStart()
{
    CancelClearanceArm("map changed");
    // Old-map entity references cannot identify an entity on the new map.
    freezeClient = 0; freezeUserId = 0; freezeHuman = 0; freezeHumanUserId = 0;
    freezeCameraRef = INVALID_ENT_REFERENCE; freezeDeadline = 0.0;
    freezePending = false; freezeDuration = 0.0;
    ClearPlant("map changed"); ClearDrop("map changed");
}

public void OnMapEnd()
{
    CancelClearanceArm("map ending");
    ClearFreeze("map ending");
}

public void OnPluginEnd()
{
    // Injected buttons were per-CUserCmd only: there is no persistent +attack.
    ClearPlant("fixture unloaded"); ClearDrop("fixture unloaded");
    ClearFreeze("fixture unloaded");
    CancelClearanceArm("fixture unloaded");
}

public void OnClientDisconnect(int client)
{
    if (client == plantClient) ClearPlant("human disconnected");
    if (client == dropClient) ClearDrop("human disconnected");
    if (client == freezeClient || client == freezeHuman) ClearFreeze("test participant disconnected");
    if (client == clearanceBot || client == clearanceHuman) CancelClearanceArm("test participant disconnected");
}

public Action LifecycleFreeze(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 3 && args != 4) { PrintToServer("[AGPB_REG] usage: agpb_reg_lifecycle_freeze bot_slot human_slot seconds [after_seconds] (duration=0.5..15; after=0..5)"); return Plugin_Handled; }
    int bot = SlotArg(1), human = SlotArg(2);
    if (!RequireNoLifecycleFreeze() || !RequireLifecycleScene(bot, human)) return Plugin_Handled;
    float duration;
    if (!FloatArg(3, duration) || duration < 0.5 || duration > 15.0 || !IsPlayerAlive(bot))
    {
        PrintToServer("[AGPB_REG] rejected: lifecycle freeze needs a live bot and duration 0.5..15 seconds");
        return Plugin_Handled;
    }
    float after;
    if (args == 4 && (!FloatArg(4, after) || after < 0.0 || after > 5.0))
    {
        PrintToServer("[AGPB_REG] rejected: lifecycle freeze delay must be 0..5 seconds");
        return Plugin_Handled;
    }
    if (after > 0.0)
    {
        freezeClient = bot; freezeUserId = GetClientUserId(bot);
        freezeHuman = human; freezeHumanUserId = GetClientUserId(human);
        freezePending = true; freezeDuration = duration; freezeDeadline = GetGameTime() + after;
        PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=freeze_armed t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d duration=%.3f after=%.3f earliest=%.3f watchdog=0.100",
            GetGameTime(), GetGameTickCount(), bot, freezeUserId, human, freezeHumanUserId, duration, after, freezeDeadline);
    }
    else BeginLifecycleFreeze(bot, human, duration);
    return Plugin_Handled;
}

bool BeginLifecycleFreeze(int bot, int human, float duration)
{
    if (!CheatsEnabled() || !RequireLifecycleScene(bot, human) || !IsPlayerAlive(bot)) return false;
    int originalFlags = GetEntityFlags(bot), originalView = ViewEntity(bot);
    // A readable NULL EHANDLE is returned as -1 and means the ordinary player
    // view. Absence of the property is a separate failure, not an active camera.
    if ((originalFlags & FL_FROZEN) != 0 || !HasEntProp(bot, Prop_Data, "m_hViewEntity") ||
        (originalView > 0 && originalView != bot))
    {
        PrintToServer("[AGPB_REG] rejected: bot is already frozen, has an external view, or lacks readable m_hViewEntity");
        return false;
    }
    int camera = CreateEntityByName("point_viewcontrol");
    if (camera <= MaxClients || !IsValidEntity(camera))
    {
        PrintToServer("[AGPB_REG] rejected: point_viewcontrol creation failed");
        return false;
    }
    char wait[24]; FloatToString(duration, wait, sizeof(wait));
    DispatchKeyValue(camera, "spawnflags", LIFECYCLE_CAMERA_FLAGS);
    DispatchKeyValue(camera, "wait", wait);
    if (!DispatchSpawn(camera))
    {
        AcceptEntityInput(camera, "Kill");
        PrintToServer("[AGPB_REG] rejected: point_viewcontrol spawn failed");
        return false;
    }
    freezeClient = bot; freezeUserId = GetClientUserId(bot);
    freezeHuman = human; freezeHumanUserId = GetClientUserId(human);
    freezeCameraRef = EntIndexToEntRef(camera); freezeDeadline = GetGameTime() + duration;
    freezePending = false; freezeDuration = duration;
    bool enabled = AcceptEntityInput(camera, "Enable", bot, bot);
    int flags = GetEntityFlags(bot), view = ViewEntity(bot);
    int owner = HasEntProp(camera, Prop_Data, "m_hPlayer") ? GetEntPropEnt(camera, Prop_Data, "m_hPlayer") : -1;
    PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=freeze_begin t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d camera=%d camera_ref=%d camera_owner=%d spawnflags=7 duration=%.3f deadline=%.3f enable=%d before_flags=%d before_view=%d flags=%d frozen=%d view_ent=%d",
        GetGameTime(), GetGameTickCount(), bot, freezeUserId, human, freezeHumanUserId, camera, freezeCameraRef,
        owner, duration, freezeDeadline, enabled, originalFlags, originalView, flags, (flags & FL_FROZEN) != 0, view);
    if (!enabled || (flags & FL_FROZEN) == 0 || view != camera || owner != bot)
    {
        ClearFreeze("engine freeze readback failed");
        return false;
    }
    return true;
}

public Action LifecycleKill(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 2) { PrintToServer("[AGPB_REG] usage: agpb_reg_lifecycle_kill bot_slot human_slot"); return Plugin_Handled; }
    int bot = SlotArg(1), human = SlotArg(2);
    if (!RequireNoLifecycleFreeze() || !RequireLifecycleScene(bot, human)) return Plugin_Handled;
    if (!IsPlayerAlive(bot)) { PrintToServer("[AGPB_REG] rejected: lifecycle kill requires a live bot"); return Plugin_Handled; }
    ForcePlayerSuicide(bot);
    PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=kill_requested t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d team=%d alive=%d",
        GetGameTime(), GetGameTickCount(), bot, GetClientUserId(bot), human, GetClientUserId(human), GetClientTeam(bot), IsPlayerAlive(bot));
    return Plugin_Handled;
}

public Action LifecycleRespawn(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 2) { PrintToServer("[AGPB_REG] usage: agpb_reg_lifecycle_respawn bot_slot human_slot"); return Plugin_Handled; }
    int bot = SlotArg(1), human = SlotArg(2);
    if (!RequireNoLifecycleFreeze() || !RequireLifecycleScene(bot, human)) return Plugin_Handled;
    if (IsPlayerAlive(bot)) { PrintToServer("[AGPB_REG] rejected: lifecycle respawn requires a dead bot"); return Plugin_Handled; }
    CS_RespawnPlayer(bot);
    PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=respawn_requested t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d team=%d alive=%d",
        GetGameTime(), GetGameTickCount(), bot, GetClientUserId(bot), human, GetClientUserId(human), GetClientTeam(bot), IsPlayerAlive(bot));
    return Plugin_Handled;
}

public Action LifecycleTeam(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 3) { PrintToServer("[AGPB_REG] usage: agpb_reg_lifecycle_team bot_slot human_slot SPEC|SAME"); return Plugin_Handled; }
    int bot = SlotArg(1), human = SlotArg(2);
    char target[12]; GetCmdArg(3, target, sizeof(target));
    bool observer = StrEqual(target, "SPEC", false), same = StrEqual(target, "SAME", false);
    if (!observer && !same) { PrintToServer("[AGPB_REG] rejected: lifecycle team must be SPEC or SAME"); return Plugin_Handled; }
    if (!RequireNoLifecycleFreeze() || !RequireLifecycleScene(bot, human, same)) return Plugin_Handled;
    int oldTeam = GetClientTeam(bot), team = observer ? CS_TEAM_SPECTATOR : GetClientTeam(human);
    // Never cross directly between playing teams. SPEC permits human_team to
    // change only after every playing bot is observed to be out of T/CT.
    if (observer) ChangeClientTeam(bot, team);
    else { CS_SwitchTeam(bot, team); CS_RespawnPlayer(bot); }
    PrintToServer("[AGPB_REG_LIFECYCLE] v=1 action=team_requested t=%.3f tick=%d slot=%d userid=%d human_slot=%d human_userid=%d oldteam=%d requested=%d team=%d alive=%d",
        GetGameTime(), GetGameTickCount(), bot, GetClientUserId(bot), human, GetClientUserId(human), oldTeam, team, GetClientTeam(bot), IsPlayerAlive(bot));
    return Plugin_Handled;
}

public Action PlaceHuman(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 5) { PrintToServer("[AGPB_REG] usage: agpb_reg_place_human slot x y z yaw"); return Plugin_Handled; }
    int client = ClientArg();
    if (!RequireHuman(client, true) || !RequireSafeScene(client)) return Plugin_Handled;
    float position[3], angles[3], velocity[3];
    for (int axis = 0; axis < 3; axis++)
        if (!FloatArg(axis + 2, position[axis])) { PrintToServer("[AGPB_REG] rejected: invalid coordinate"); return Plugin_Handled; }
    if (!FloatArg(5, angles[1])) { PrintToServer("[AGPB_REG] rejected: invalid yaw"); return Plugin_Handled; }
    ClearPlant("human repositioned"); ClearDrop("human repositioned");
    TeleportEntity(client, position, angles, velocity);
    PrintToServer("[AGPB_REG] human placed: slot=%d userid=%d pos=(%.2f %.2f %.2f) yaw=%.1f", client, GetClientUserId(client), position[0], position[1], position[2], angles[1]);
    return Plugin_Handled;
}

public Action HumanPlant(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 1) { PrintToServer("[AGPB_REG] usage: agpb_reg_human_plant slot"); return Plugin_Handled; }
    int client = ClientArg(), c4;
    if (!RequireHuman(client, true) || !RequireSafeScene(client)) return Plugin_Handled;
    if (GetClientTeam(client) != CS_TEAM_T || !ExistingHumanC4(client, c4) ||
        !HasEntProp(client, Prop_Send, "m_bInBombZone") || !GetEntProp(client, Prop_Send, "m_bInBombZone"))
    {
        PrintToServer("[AGPB_REG] rejected: human T must already own weapon_c4 and stand in a real bomb zone");
        return Plugin_Handled;
    }
    ClearPlant("replaced request"); ClearDrop("plant requested");
    plantClient = client; plantUserId = GetClientUserId(client); plantDeadline = GetGameTime() + 8.0;
    PrintToServer("[AGPB_REG] human plant input armed: slot=%d userid=%d existing_c4=%d timeout=8.0", client, plantUserId, c4);
    return Plugin_Handled;
}

public Action DropC4(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 4) { PrintToServer("[AGPB_REG] usage: agpb_reg_drop_c4 slot x y z"); return Plugin_Handled; }
    int client = ClientArg(), c4;
    if (!RequireHuman(client, true) || !RequireSafeScene(client)) return Plugin_Handled;
    if (!ExistingHumanC4(client, c4))
    {
        PrintToServer("[AGPB_REG] rejected: human must already own a real weapon_c4");
        return Plugin_Handled;
    }
    float target[3];
    for (int axis = 0; axis < 3; axis++)
        if (!FloatArg(axis + 2, target[axis])) { PrintToServer("[AGPB_REG] rejected: invalid coordinate"); return Plugin_Handled; }
    ClearPlant("drop requested"); ClearDrop("replaced request");
    dropClient = client; dropUserId = GetClientUserId(client); dropReference = EntIndexToEntRef(c4);
    dropTarget = target; dropDeadline = GetGameTime() + 4.0; dropIssued = false;
    FakeClientCommand(client, "use weapon_c4");
    PrintToServer("[AGPB_REG] human C4 drop armed: slot=%d userid=%d existing_c4=%d timeout=4.0", client, dropUserId, c4);
    return Plugin_Handled;
}

public Action HumanTeam(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    if (args != 2) { PrintToServer("[AGPB_REG] usage: agpb_reg_human_team slot T|CT"); return Plugin_Handled; }
    int client = ClientArg();
    if (!RequireHuman(client, false)) return Plugin_Handled;
    if (!RequireNoLifecycleFreeze()) return Plugin_Handled;
    if (PlayingFakeExists())
    {
        PrintToServer("[AGPB_REG] rejected: kick all T/CT bots manually before changing the human team");
        return Plugin_Handled;
    }
    char text[8]; GetCmdArg(2, text, sizeof(text));
    int team;
    if (StrEqual(text, "T", false)) team = CS_TEAM_T;
    else if (StrEqual(text, "CT", false)) team = CS_TEAM_CT;
    else { PrintToServer("[AGPB_REG] rejected: team must be T or CT"); return Plugin_Handled; }
    ClearPlant("human team requested"); ClearDrop("human team requested");
    CS_SwitchTeam(client, team);
    CS_RespawnPlayer(client);
    PrintToServer("[AGPB_REG] human team/respawn requested: slot=%d userid=%d actual_team=%d alive=%d", client, GetClientUserId(client), GetClientTeam(client), IsPlayerAlive(client));
    return Plugin_Handled;
}

public Action Clear(int args)
{
    if (!RequireCheats()) return Plugin_Handled;
    ClearPlant("explicit clear"); ClearDrop("explicit clear"); ClearFreeze("explicit clear");
    CancelClearanceArm("explicit clear");
    PrintToServer("[AGPB_REG] fixture requests cleared; lifecycle viewcontrol restoration requires probe readback");
    return Plugin_Handled;
}

public Action OnPlayerRunCmd(int client, int &buttons, int &impulse, float velocity[3], float angles[3], int &weapon,
    int &subtype, int &cmdnum, int &tickcount, int &seed, int mouse[2])
{
    if (!HumanValid(client, false)) return Plugin_Continue;
    int userid = GetClientUserId(client);
    if (client != plantClient || userid != plantUserId) return Plugin_Continue;
    int c4;
    if (!CheatsEnabled() || !HumanValid(client, true) || GetClientTeam(client) != CS_TEAM_T ||
        PlayingFakeExists(GetClientTeam(client)) || GetGameTime() >= plantDeadline || !ExistingHumanC4(client, c4))
    {
        ClearPlant("input validation/timeout");
        return Plugin_Continue;
    }
    // Select only while inactive; repeated selection must not restart deployment.
    if (GetEntPropEnt(client, Prop_Send, "m_hActiveWeapon") != c4) weapon = c4;
    buttons |= IN_ATTACK;
    return Plugin_Changed;
}

public Action Watch(Handle timer)
{
    if (clearanceUserId != 0 && !ClearanceArmValid()) CancelClearanceArm("watchdog identity/safety/timeout");
    if (freezeUserId != 0)
    {
        int camera = EntRefToEntIndex(freezeCameraRef);
        bool identity = FreezeIdentityValid();
        if (freezePending)
        {
            if (!CheatsEnabled() || !identity || !IsPlayerAlive(freezeClient) ||
                !HumanValid(freezeHuman, true) || GetClientUserId(freezeHuman) != freezeHumanUserId ||
                GetClientTeam(freezeClient) != GetClientTeam(freezeHuman) || PlayingFakeExists(GetClientTeam(freezeHuman)))
                ClearFreeze("delayed freeze participant validation failed");
            else if (GetGameTime() >= freezeDeadline && !BeginLifecycleFreeze(freezeClient, freezeHuman, freezeDuration))
                ClearFreeze("delayed freeze rejected");
        }
        else if (!CheatsEnabled() || !identity || !IsPlayerAlive(freezeClient) ||
            !HumanValid(freezeHuman, true) || GetClientUserId(freezeHuman) != freezeHumanUserId ||
            GetClientTeam(freezeClient) != GetClientTeam(freezeHuman) || PlayingFakeExists(GetClientTeam(freezeHuman)) ||
            GetGameTime() >= freezeDeadline || camera <= MaxClients || !IsValidEntity(camera) ||
            (GetEntityFlags(freezeClient) & FL_FROZEN) == 0 || ViewEntity(freezeClient) != camera)
            ClearFreeze("watchdog/timeout/engine release");
    }
    if (plantUserId != 0 && (!CheatsEnabled() || !HumanValid(plantClient, true) ||
        GetClientUserId(plantClient) != plantUserId || GetGameTime() >= plantDeadline))
        ClearPlant("watchdog/timeout");
    if (dropUserId == 0) return Plugin_Continue;
    if (!CheatsEnabled() || !HumanValid(dropClient, true) || GetClientUserId(dropClient) != dropUserId ||
        PlayingFakeExists(GetClientTeam(dropClient)) || GetGameTime() >= dropDeadline)
    {
        ClearDrop("validation/timeout"); return Plugin_Continue;
    }
    int c4 = EntRefToEntIndex(dropReference);
    if (c4 <= MaxClients || !IsValidEntity(c4) || !HasEntProp(c4, Prop_Send, "m_hOwnerEntity"))
    {
        ClearDrop("original C4 no longer exists"); return Plugin_Continue;
    }
    int owner = GetEntPropEnt(c4, Prop_Send, "m_hOwnerEntity");
    if (!dropIssued)
    {
        if (owner != dropClient) { ClearDrop("original owner changed"); return Plugin_Continue; }
        if (GetEntPropEnt(dropClient, Prop_Send, "m_hActiveWeapon") != c4) return Plugin_Continue;
        FakeClientCommand(dropClient, "drop"); // Game's ordinary drop path, never a fabricated owner.
        dropIssued = true;
        return Plugin_Continue;
    }
    if (owner == dropClient) return Plugin_Continue;
    if (owner > 0) { ClearDrop("C4 was picked up; not repositioned"); return Plugin_Continue; }
    float zero[3]; TeleportEntity(c4, dropTarget, NULL_VECTOR, zero);
    PrintToServer("[AGPB_REG] original dropped C4 repositioned: ent=%d pos=(%.2f %.2f %.2f); physics/state still require observation", c4, dropTarget[0], dropTarget[1], dropTarget[2]);
    ClearDrop("original dropped C4 repositioned");
    return Plugin_Continue;
}

public void Planted(Event event, const char[] name, bool dontBroadcast)
{
    int userid = event.GetInt("userid", 0);
    if (plantUserId != 0 && userid == plantUserId)
    {
        PrintToServer("[AGPB_REG] observed real bomb_planted event: userid=%d t=%.3f", userid, GetGameTime());
        ClearPlant("observed real planted event");
    }
}
