// SPDX-License-Identifier: GPL-3.0-only
#pragma semicolon 1
#pragma newdecls required
#include <sourcemod>

#define AGPB_ZR_CONVAR "agpb_zr_available"
#define AGPB_ZR_ROSTER_CONVAR "agpb_zr_roles"
#define ZR_LIBRARY "zombiereloaded"
#define ZR_ZOMBIE_NATIVE "ZR_IsClientZombie"

native bool ZR_IsClientZombie(int client);

bool g_MapRunning;
bool g_RoundEnded;
bool g_RefreshQueued;
bool g_ZRLibraryRemoved;
int g_RefreshSerial;
int g_RosterHeartbeat;

public Plugin myinfo = {
    name = "AgPB Zombie:Reloaded Bridge",
    author = "AgPB",
    description = "Reports the Zombie:Reloaded API and live player roles to AgPB",
    version = "1.1.0",
    url = ""
};

public APLRes AskPluginLoad2(Handle myself, bool late, char[] error, int errMax)
{
    MarkNativeAsOptional(ZR_ZOMBIE_NATIVE);
    return APLRes_Success;
}

public void OnPluginStart()
{
    HookEvent("player_spawn", OnPlayerLifecycle);
    HookEvent("player_death", OnPlayerLifecycle);
    HookEvent("player_team", OnPlayerLifecycle);
    HookEvent("round_start", OnRoundLifecycle, EventHookMode_PostNoCopy);
    HookEvent("round_end", OnRoundLifecycle, EventHookMode_PostNoCopy);
    // Some ZR builds provide the native without registering a plugin library.
    CreateTimer(0.5, PollBridge, _, TIMER_REPEAT);
}

public void OnAllPluginsLoaded()
{
    RefreshBridge();
}

public void OnConfigsExecuted()
{
    RefreshBridge();
}

public void OnMapStart()
{
    g_MapRunning = true;
    g_RoundEnded = false;
    g_RefreshQueued = false;
    ClearRoster();
    QueueRosterRefresh();
}

public void OnMapEnd()
{
    g_MapRunning = false;
    g_RefreshQueued = false;
    StopReporting();
}

public void OnPluginPauseChange(bool pause)
{
    g_RefreshQueued = false;
    if (pause) {
        StopReporting();
    } else {
        // Map and round forwards may have been skipped while paused.
        char map[PLATFORM_MAX_PATH];
        g_MapRunning = GetCurrentMap(map, sizeof(map)) > 0;
        g_RoundEnded = false;
        QueueRosterRefresh();
    }
}

public void OnPluginEnd()
{
    StopReporting();
}

public void OnLibraryAdded(const char[] name)
{
    if (StrEqual(name, ZR_LIBRARY)) {
        g_ZRLibraryRemoved = false;
        QueueRosterRefresh();
    }
}

public void OnLibraryRemoved(const char[] name)
{
    if (StrEqual(name, ZR_LIBRARY)) {
        g_ZRLibraryRemoved = true;
        StopReporting();
    }
}

public void OnClientDisconnect(int client)
{
    InvalidateClient(client);
}

public void OnClientPutInServer(int client)
{
    InvalidateClient(client);
}

public void OnPlayerLifecycle(Event event, const char[] name, bool dontBroadcast)
{
    InvalidateUserId(event.GetInt("userid"));
}

public void OnRoundLifecycle(Event event, const char[] name, bool dontBroadcast)
{
    g_RoundEnded = StrEqual(name, "round_end");
    ClearRoster();
    QueueRosterRefresh();
}

public void ZR_OnClientInfected(int client, int attacker, bool motherInfect, bool respawnOverride, bool respawn)
{
    InvalidateClient(client);
}

public void ZR_OnClientHumanPost(int client, bool respawn, bool protect)
{
    InvalidateClient(client);
}

public Action PollBridge(Handle timer)
{
    RefreshBridge();
    return Plugin_Continue;
}

void InvalidateClient(int client)
{
    if (client > 0 && client <= MaxClients && IsClientConnected(client)) {
        InvalidateUserId(GetClientUserId(client));
    }
}

void InvalidateUserId(int userId)
{
    if (!g_MapRunning || g_RoundEnded || userId <= 0) {
        return;
    }

    ConVar signal = FindConVar(AGPB_ZR_ROSTER_CONVAR);
    if (signal != null) {
        char packet[16];
        FormatEx(packet, sizeof(packet), "drop %d", userId);
        signal.SetString(packet);
    }
    QueueRosterRefresh();
}

void QueueRosterRefresh()
{
    if (!g_MapRunning || g_RoundEnded || g_RefreshQueued) {
        return;
    }

    g_RefreshQueued = true;
    // Events can run before ZR's own spawn/team handlers.
    RequestFrame(RefreshQueuedRoster, ++g_RefreshSerial);
}

public void RefreshQueuedRoster(int serial)
{
    if (!g_RefreshQueued || serial != g_RefreshSerial) {
        return;
    }
    g_RefreshQueued = false;
    RefreshBridge();
}

void RefreshBridge()
{
    if (!g_MapRunning || g_RefreshQueued) {
        return;
    }
    if (!CanTestFeatures() || GetFeatureStatus(FeatureType_Native, ZR_ZOMBIE_NATIVE) != FeatureStatus_Available) {
        g_ZRLibraryRemoved = false;
        StopReporting();
        return;
    }
    if (g_ZRLibraryRemoved && !LibraryExists(ZR_LIBRARY)) {
        return;
    }
    g_ZRLibraryRemoved = false;

    ConVar signal = FindConVar(AGPB_ZR_ROSTER_CONVAR);
    if (signal == null || g_RoundEnded) {
        SetAvailability(true);
        return;
    }

    // An optional fake native may remain bound while its provider is paused.
    // Catch that error so no partial snapshot is published.
    Call_StartFunction(null, PublishRoster);
    Call_PushCell(signal);
    if (Call_Finish() != SP_ERROR_NONE) {
        StopReporting();
    }
}

public void PublishRoster(ConVar signal)
{
    if (g_RosterHeartbeat == 1000000) {
        g_RosterHeartbeat = 0;
    }
    char packet[16 + MAXPLAYERS * 16];
    int length = FormatEx(packet, sizeof(packet), "%d", ++g_RosterHeartbeat);
    for (int client = 1; client <= MaxClients; client++) {
        // ZR's native is only valid for connected, living players.
        if (!IsClientInGame(client) || !IsPlayerAlive(client) || IsClientObserver(client)) {
            continue;
        }

        int userId = GetClientUserId(client);
        if (userId > 0) {
            int role = ZR_IsClientZombie(client) ? 2 : 1;
            length += FormatEx(packet[length], sizeof(packet) - length, " %d %d", userId, role);
        }
    }
    SetAvailability(true);
    signal.SetString(packet);
}

void SetAvailability(bool available)
{
    ConVar signal = FindConVar(AGPB_ZR_CONVAR);
    if (signal != null && signal.BoolValue != available) {
        signal.BoolValue = available;
    }
}

void ClearRoster()
{
    ConVar signal = FindConVar(AGPB_ZR_ROSTER_CONVAR);
    if (signal != null) {
        signal.SetString("");
    }
}

void StopReporting()
{
    SetAvailability(false);
    ClearRoster();
}
