/**
 * AgPB fake-client compatibility patch (GPL-3.0).
 * Source reference: Valve CCSPlayer::Radio, cs_player.cpp.
 * Independent optional SourceMod plugin; AgPB's C++ control layer is unchanged.
 * Uses this host's CNSR x64 SourcePawn Address=int64 SDK, not stock 32-bit SP.
 */
#pragma semicolon 1
#pragma newdecls required
#include <sourcemod>

#define MAX_SITE 16
#define MAX_STUB 128

public Plugin myinfo = {
    name = "AgPB FakeClient Guard",
    author = "AgPB",
    description = "Version-checked Radio null-bot/profile guard",
    version = "1.0.0",
    url = ""
};

GameData g_Data;
Address g_Site, g_Cave, g_Cleanup;
int g_SiteSize, g_StubSize;
int g_Original[MAX_SITE], g_Patch[MAX_SITE], g_Stub[MAX_STUB];
int g_CountBot, g_CountProfile, g_CountNative;
bool g_Active, g_Changing;
int g_FreezeEnds;
ConVar g_Enable;

int HexDigit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ReadHex(const char[] key, int[] bytes, int capacity)
{
    char text[1024];
    if (!g_Data.GetKeyValue(key, text, sizeof(text))) return -1;
    int length = strlen(text);
    if (length % 2 != 0 || length / 2 > capacity) return -1;
    for (int i = 0; i < length / 2; i++) {
        int high = HexDigit(text[2 * i]), low = HexDigit(text[2 * i + 1]);
        if (high < 0 || low < 0) return -1;
        bytes[i] = high * 16 + low;
    }
    return length / 2;
}

int RequiredOffset(const char[] key)
{
    int value = g_Data.GetOffset(key);
    if (value < 0) SetFailState("Missing gamedata offset: %s", key);
    return value;
}

void Rel32(int[] bytes, int capacity, int offset, Address next, Address target)
{
    Address delta = target - next;
    if (offset < 0 || offset + 4 > capacity || delta < -2147483648 || delta > 2147483647)
        SetFailState("Invalid relative jump; no patch applied");
    int value = view_as<int>(delta);
    for (int i = 0; i < 4; i++) bytes[offset + i] = (value >>> (i * 8)) & 255;
}

void FixStub(const char[] key, Address target)
{
    int offset = RequiredOffset(key);
    Rel32(g_Stub, g_StubSize, offset, g_Cave + offset + 4, target);
}

bool IsCounterByte(int i)
{
    return (i >= g_CountBot && i < g_CountBot + 4) ||
           (i >= g_CountProfile && i < g_CountProfile + 4) ||
           (i >= g_CountNative && i < g_CountNative + 4);
}

bool MatchSite(bool patched)
{
    for (int i = 0; i < g_SiteSize; i++) {
        int expected = patched ? g_Patch[i] : g_Original[i];
        if (LoadFromAddress(g_Site + i, NumberType_Int8) != expected) return false;
    }
    return true;
}

bool MatchCave(bool patched)
{
    for (int i = 0; i < g_StubSize; i++) {
        if (patched && IsCounterByte(i)) continue;
        int expected = patched ? g_Stub[i] : 0xCC;
        if (LoadFromAddress(g_Cave + i, NumberType_Int8) != expected) return false;
    }
    return true;
}

bool ApplyPatch()
{
    if (g_Active) return MatchSite(true) && MatchCave(true);
    if (!MatchSite(false) || !MatchCave(false)) {
        LogError("Original instructions/padding differ; refusing to write memory");
        return false;
    }
    // Finish the independent in-module stub before connecting the live branch.
    for (int i = 0; i < g_StubSize; i++)
        StoreToAddress(g_Cave + i, g_Stub[i], NumberType_Int8);
    for (int i = 0; i < g_SiteSize; i++)
        StoreToAddress(g_Site + i, g_Patch[i], NumberType_Int8);
    g_Active = true;
    PrintToServer("[AgPB Guard] Radio null-bot/profile patch enabled; server CRC 770BAE64");
    return true;
}

bool RestorePatch()
{
    if (!g_Active) return true;
    if (!MatchSite(true) || !MatchCave(true)) {
        LogError("Patch ownership changed; refusing to overwrite another patch");
        return false;
    }
    // Disconnect the branch first; only then restore its unused padding.
    for (int i = 0; i < g_SiteSize; i++)
        StoreToAddress(g_Site + i, g_Original[i], NumberType_Int8);
    for (int i = 0; i < g_StubSize; i++)
        StoreToAddress(g_Cave + i, 0xCC, NumberType_Int8);
    g_Active = false;
    PrintToServer("[AgPB Guard] Original Radio instructions restored");
    return true;
}

int Count(int offset)
{
    return g_Active ? LoadFromAddress(g_Cave + offset, NumberType_Int32) : 0;
}

bool HasFakeClients()
{
    for (int i = 1; i <= MaxClients; i++)
        if (IsClientConnected(i) && IsFakeClient(i)) return true;
    return false;
}

public void EnableChanged(ConVar convar, const char[] oldValue, const char[] newValue)
{
    if (g_Changing) return;
    bool enable = convar.BoolValue;
    bool success;
    if (!enable && HasFakeClients()) {
        PrintToServer("[AgPB Guard] Kick fake clients before disabling the compatibility patch");
        success = false;
    } else {
        success = enable ? ApplyPatch() : RestorePatch();
    }
    if (!success) {
        g_Changing = true;
        convar.BoolValue = g_Active;
        g_Changing = false;
    }
}

public Action Status(int client, int args)
{
    bool intact = g_Active && MatchSite(true) && MatchCave(true);
    ReplyToCommand(client, "[AgPB Guard] active=%d intact=%d null-bot=%d null-profile=%d native-path=%d freeze-ends=%d",
        g_Active, intact, Count(g_CountBot), Count(g_CountProfile), Count(g_CountNative), g_FreezeEnds);
    return Plugin_Handled;
}

public void FreezeEnd(Event event, const char[] name, bool dontBroadcast)
{
    g_FreezeEnds++;
    PrintToServer("[AgPB Guard] freeze end #%d; null-bot=%d null-profile=%d native-path=%d",
        g_FreezeEnds, Count(g_CountBot), Count(g_CountProfile), Count(g_CountNative));
}

public void OnPluginStart()
{
    g_Data = new GameData("agpb.fakeclient.guard");
    if (g_Data == null) SetFailState("Missing agpb.fakeclient.guard gamedata; no patch applied");
    // The CRC section gates every signature/key/offset, including architecture.
    g_Site = g_Data.GetAddress("RadioProfile");
    g_Cave = g_Data.GetAddress("RadioGuardCave");
    g_Cleanup = g_Data.GetAddress("RadioCleanup");
    if (g_Site == Address_Null || g_Cave == Address_Null || g_Cleanup == Address_Null)
        SetFailState("Unsupported server.dll/architecture or signature mismatch; no patch applied");
    g_SiteSize = RequiredOffset("SiteSize");
    g_StubSize = RequiredOffset("StubSize");
    if (g_SiteSize < 5 || g_SiteSize > MAX_SITE || g_StubSize > MAX_STUB)
        SetFailState("Invalid gamedata patch sizes; no patch applied");
    g_CountBot = RequiredOffset("NullBotCounter");
    g_CountProfile = RequiredOffset("NullProfileCounter");
    g_CountNative = RequiredOffset("NativeCounter");
    if (g_CountBot + 4 > g_StubSize || g_CountProfile + 4 > g_StubSize || g_CountNative + 4 > g_StubSize)
        SetFailState("Invalid counter offsets; no patch applied");
    if (ReadHex("SiteOriginal", g_Original, MAX_SITE) != g_SiteSize ||
        ReadHex("StubTemplate", g_Stub, MAX_STUB) != g_StubSize)
        SetFailState("Invalid gamedata byte templates; no patch applied");
    // Verify the normal cleanup target, not merely the patch location.
    int cleanupBytes[32];
    int cleanupLength = ReadHex("CleanupOriginal", cleanupBytes, sizeof(cleanupBytes));
    if (cleanupLength <= 0) SetFailState("Missing cleanup bytes; no patch applied");
    for (int i = 0; i < cleanupLength; i++)
        if (LoadFromAddress(g_Cleanup + i, NumberType_Int8) != cleanupBytes[i])
            SetFailState("Radio cleanup differs; no patch applied");
    for (int i = 0; i < g_SiteSize; i++) g_Patch[i] = 0x90;
    g_Patch[0] = 0xE9;
    Rel32(g_Patch, g_SiteSize, 1, g_Site + 5, g_Cave);
    FixStub("NormalCountRel32", g_Cave + g_CountNative);
    FixStub("ReturnRel32", g_Site + g_SiteSize);
    FixStub("NullBotCountRel32", g_Cave + g_CountBot);
    FixStub("NullBotExitRel32", g_Cleanup);
    FixStub("NullProfileCountRel32", g_Cave + g_CountProfile);
    FixStub("NullProfileExitRel32", g_Cleanup);
    g_Enable = CreateConVar("sm_agpb_fakeclient_guard", "1", "Enable the version-checked Radio compatibility guard", FCVAR_NOTIFY, true, 0.0, true, 1.0);
    if (g_Enable.BoolValue && !ApplyPatch()) SetFailState("Memory validation failed; no patch applied");
    g_Enable.AddChangeHook(EnableChanged);
    RegAdminCmd("sm_agpb_guard_status", Status, ADMFLAG_ROOT, "Report Radio guard integrity and branch counters");
    HookEvent("round_freeze_end", FreezeEnd, EventHookMode_PostNoCopy);
}

public void OnPluginEnd()
{
    // Administrators must kick fake clients before unloading this plugin.
    RestorePatch();
    delete g_Data;
}
