#include <sourcemod>
#include <sdktools>
#pragma semicolon 1
#pragma newdecls required

// Optional read-only diagnostics. No placement, inventory, input or team writes.
public Plugin myinfo = {
    name = "AgPB read-only waypoint probe",
    author = "AgPB",
    description = "Player state and observed bomb events for manual route acceptance",
    version = "1.0"
};
int plantedCount;
int defusedCount;
int explodedCount;
int plantedUserId;
int defusedUserId;
int explodedUserId;

public void OnPluginStart()
{
    RegServerCmd("agpb_wp_probe_state", State);
    RegServerCmd("agpb_wp_probe_bomb", Bomb);
    HookEvent("bomb_planted", BombEvent);
    HookEvent("bomb_defused", BombEvent);
    HookEvent("bomb_exploded", BombEvent);
}

public void OnMapStart()
{
    plantedCount = 0; defusedCount = 0; explodedCount = 0;
    plantedUserId = 0; defusedUserId = 0; explodedUserId = 0;
}

public Action State(int args)
{
    char map[64]; GetCurrentMap(map, sizeof(map));
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsClientInGame(i)) continue;
        float position[3], velocity[3];
        GetClientAbsOrigin(i, position);
        GetEntPropVector(i, Prop_Data, "m_vecVelocity", velocity);
        int zone = HasEntProp(i, Prop_Send, "m_bInBombZone") ? GetEntProp(i, Prop_Send, "m_bInBombZone") : -1;
        int defusing = HasEntProp(i, Prop_Send, "m_bIsDefusing") ? GetEntProp(i, Prop_Send, "m_bIsDefusing") : -1;
        PrintToServer("[WP_PROBE_STATE] t=%.3f map=%s slot=%d userid=%d fake=%d team=%d alive=%d flags=%d pos=(%.6f %.6f %.6f) vel=(%.6f %.6f %.6f) zone=%d defusing=%d",
            GetGameTime(), map, i, GetClientUserId(i), IsFakeClient(i), GetClientTeam(i), IsPlayerAlive(i), GetEntityFlags(i),
            position[0], position[1], position[2], velocity[0], velocity[1], velocity[2], zone, defusing);
    }
    return Plugin_Handled;
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
