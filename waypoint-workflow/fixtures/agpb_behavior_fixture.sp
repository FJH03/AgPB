// SPDX-License-Identifier: GPL-3.0-only
#include <sourcemod>
#include <sdktools>
#include <cstrike>
#pragma semicolon 1
#pragma newdecls required

// Test-only scene preparation. Never include this fixture in the AgPB package.
// Every command requires sv_cheats; client mutations always revalidate a HUMAN
// userid. No bot input, bot team/spawn, inventory grant, entity creation, bomb
// property writes or synthetic bomb events are used.
public Plugin myinfo = {
    name = "AgPB human regression scene fixture",
    author = "AgPB",
    description = "Cheat-gated human-only preparation for manual behavior regression",
    version = "1.0"
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
    RegServerCmd("agpb_reg_clear", Clear);
    HookEvent("bomb_planted", Planted);
    CreateTimer(0.1, Watch, _, TIMER_REPEAT);
}

public void OnMapStart()
{
    ClearPlant("map changed"); ClearDrop("map changed");
}

public void OnPluginEnd()
{
    // Injected buttons were per-CUserCmd only: there is no persistent +attack.
    ClearPlant("fixture unloaded"); ClearDrop("fixture unloaded");
}

public void OnClientDisconnect(int client)
{
    if (client == plantClient) ClearPlant("human disconnected");
    if (client == dropClient) ClearDrop("human disconnected");
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
    ClearPlant("explicit clear"); ClearDrop("explicit clear");
    PrintToServer("[AGPB_REG] fixture requests cleared; no bot was modified");
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
