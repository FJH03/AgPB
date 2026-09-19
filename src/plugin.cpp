/**
 * AgPB - Metamod:Source plugin entry point.
 *
 * 目标引擎：Counter-Strike: Source（Source 1 / x86_64）
 * 不使用 SourceMod，也不使用引擎自带的 CCSBot。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <eiface.h>
#include <edict.h>
#include <icvar.h>
#include <tier1/convar.h>
#include <engine/IEngineTrace.h>

#include "plugin.h"
#include "bot.h"

AgPBPlugin g_AgPBPlugin;

SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);

IServerGameDLL *server = NULL;
IServerGameClients *gameclients = NULL;
IVEngineServer *engine = NULL;
ICvar *icvar = NULL;
IEngineTrace *enginetrace = NULL;
IPlayerInfoManager *playerinfomanager = NULL;
IServerPluginHelpers *helpers = NULL;
IServerGameEnts *gameents = NULL;
CGlobalVars *gpGlobals = NULL;

static CAgPBManager g_Bots;

// ---------------------------------------------------------------------------
// ConVars / ConCommands
// ---------------------------------------------------------------------------

static ConVar agpb_enable( "agpb_enable", "1", FCVAR_GAMEDLL,
                          "Enable or disable per-tick AgPB bot thinking." );

static ConVar agpb_team( "agpb_team", "2", FCVAR_GAMEDLL,
                         "Default team for agpb_add (1=spectator, 2=T, 3=CT)." );

/**
 * MMS 要求插件通过 IConCommandBaseAccessor 注册自己的 ConVar / ConCommand。
 */
class BaseAccessor : public IConCommandBaseAccessor
{
public:
	bool RegisterConCommandBase( ConCommandBase *pCommandBase )
	{
		return META_REGBASECMD( pCommandBase );
	}
} s_BaseAccessor;

static void Cmd_Add( const CCommand &args )
{
	int team = agpb_team.GetInt();
	if ( args.ArgC() >= 2 )
		team = atoi( args.Arg( 1 ) );

	if ( team != 1 && team != 2 && team != 3 )
	{
		META_CONPRINTF( "[AgPB] invalid team %d (use 1=spec, 2=T, 3=CT)\n", team );
		return;
	}

	char error[256];
	error[0] = '\0';

	CAgPB *pBot = g_Bots.Add( team, error, sizeof( error ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] agpb_add failed: %s\n", error );
		return;
	}

	META_CONPRINTF( "[AgPB] created bot '%s' (slot=%d, team=%d)\n",
	                pBot->Name(), pBot->Index(), team );
}

static void Cmd_Kick( const CCommand &args )
{
	if ( args.ArgC() >= 2 && !stricmp( args.Arg( 1 ), "all" ) )
	{
		const int n = g_Bots.Count();
		g_Bots.RemoveAll();
		META_CONPRINTF( "[AgPB] removed %d bot(s).\n", n );
		return;
	}

	const int index = ( args.ArgC() >= 2 ) ? atoi( args.Arg( 1 ) ) : -1;
	if ( !g_Bots.Remove( index ) )
		META_CONPRINTF( "[AgPB] invalid list index: %d (use agpb_list)\n", index );
}

static void Cmd_List( const CCommand &args )
{
	META_CONPRINTF( "[AgPB] %d bot(s), IBotManager=%s, helpers=%s\n",
	                g_Bots.Count(),
	                g_Bots.IsReady() ? "ok" : "missing",
	                helpers ? "ok" : "missing" );

	for ( int i = 0; i < g_Bots.Count(); ++i )
	{
		CAgPB *pBot = g_Bots.Get( i );
		IPlayerInfo *pInfo = pBot->PlayerInfo();

		Vector origin( 0.0f, 0.0f, 0.0f );
		int team = -1;
		int health = -1;
		const char *weapon = "?";

		if ( pInfo != NULL )
		{
			origin = pInfo->GetAbsOrigin();
			team = pInfo->GetTeamIndex();
			health = pInfo->GetHealth();
			weapon = pInfo->GetWeaponName();
		}

		META_CONPRINTF( "  [%d] %s slot=%d team=%d want=%d hp=%d wpn=%s pos=%.1f,%.1f,%.1f\n",
		                i, pBot->Name(), pBot->Index(), team, pBot->DesiredTeam(), health, weapon,
		                origin.x, origin.y, origin.z );
	}
}

static void Cmd_SetTeam( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_team <idx> <1=spec|2=T|3=CT>\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const int team = atoi( args.Arg( 2 ) );
	if ( !pBot->SetTeam( team ) )
	{
		META_CONPRINTF( "[AgPB] invalid team %d (use 1=spec, 2=T, 3=CT)\n", team );
		return;
	}

	const int curTeam = ( pBot->PlayerInfo() != NULL ) ? pBot->PlayerInfo()->GetTeamIndex() : -1;
	META_CONPRINTF( "[AgPB] bot %s: requested team %d (current=%d)\n",
	                pBot->Name(), team, curTeam );
}

// 大小写无关的子串匹配，用于 netlist 的过滤器。
static const char *NetVarTypeName( SendPropType type )
{
	switch ( type )
	{
		case DPT_Int:       return "int";
		case DPT_Float:     return "float";
		case DPT_Vector:    return "vec3";
		case DPT_VectorXY:  return "vec2";
		case DPT_String:    return "string";
		case DPT_Array:     return "array";
		case DPT_DataTable: return "datatable";
		default:            return "?";
	}
}

static bool ContainsNoCase( const char *haystack, const char *needle )
{
	if ( needle == NULL || needle[0] == '\0' )
		return true;

	if ( haystack == NULL )
		return false;

	for ( const char *h = haystack; *h; ++h )
	{
		const char *a = h;
		const char *b = needle;

		while ( *a && *b && tolower( (unsigned char)*a ) == tolower( (unsigned char)*b ) )
		{
			++a;
			++b;
		}

		if ( *b == '\0' )
			return true;
	}

	return false;
}

/**
 * agpb_netlist <idx> [name-filter]
 *
 * 把某个 bot 的 ServerClass 展开成 (名字, 偏移, 类型) 列表并打印当前值。
 * 这是 M2 反射层的验证工具，也是移植 EBot Entity 层时查字段名的字典。
 */
static void Cmd_NetList( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_netlist <idx> [name-filter]\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const CNetVarTable *pTable = pBot->NetVarTable();
	if ( pTable == NULL )
	{
		META_CONPRINTF( "[AgPB] no SendTable for bot %s\n", pBot->Name() );
		return;
	}

	const char *pFilter = ( args.ArgC() >= 3 ) ? args.Arg( 2 ) : NULL;
	void *pBase = pBot->NetVarBase();

	META_CONPRINTF( "[AgPB] %s class=%s props=%d base=%p filter=%s\n",
	                pBot->Name(), pTable->ClassName(), pTable->Count(), pBase,
	                ( pFilter != NULL ) ? pFilter : "(none)" );

	if ( pBase == NULL )
	{
		META_CONPRINTF( "[AgPB] entity base pointer unavailable, values suppressed\n" );
		return;
	}

	int printed = 0;

	for ( int i = 0; i < pTable->Count(); ++i )
	{
		const BotNetVar &nv = pTable->Prop( i );

		if ( !ContainsNoCase( nv.name, pFilter ) )
			continue;

		++printed;

		switch ( nv.type )
		{
			case DPT_Float:
				META_CONPRINTF( "  %-32s +%-5d s=%-3d float  = %.4f\n",
				                nv.name, nv.offset, nv.stride, NetVar_GetFloat( pBase, nv ) );
				break;

			case DPT_Vector:
			case DPT_VectorXY:
			{
				const Vector v = NetVar_GetVector( pBase, nv );
				META_CONPRINTF( "  %-32s +%-5d s=%-3d vec3   = %.2f %.2f %.2f\n",
				                nv.name, nv.offset, nv.stride, v.x, v.y, v.z );
				break;
			}

			case DPT_String:
			{
				// 布局有两种：内联 char[N]（DT_String 的常见形式）和 char*。
				// 运行时无法可靠区分，所以绝不按指针解引用（垃圾指针会崩服务端），
				// 只把该偏移处的字节当定长缓冲按文本打印。
				char buf[48];
				memcpy( buf, (const char *)pBase + nv.offset, sizeof( buf ) - 1 );
				buf[sizeof( buf ) - 1] = '\0';

				META_CONPRINTF( "  %-32s +%-5d s=%-3d string = \"%s\"\n",
				                nv.name, nv.offset, nv.stride, buf );
				break;
			}

			case DPT_Array:
			{
				META_CONPRINTF( "  %-32s +%-5d array  [%d x %d] elem=%s\n",
				                nv.name, nv.offset, nv.elements, nv.stride,
				                NetVarTypeName( nv.elementType ) );

				// 只展开小数组，避免 m_iAmmo 之类把控制台刷爆。
				if ( nv.stride > 0 && nv.elements > 0 && nv.elements <= 32 )
				{
					for ( int e = 0; e < nv.elements; ++e )
					{
						if ( nv.elementType == DPT_Float )
							META_CONPRINTF( "      [%2d] = %.3f\n", e, NetVar_GetArrayFloat( pBase, nv, e ) );
						else if ( nv.elementType == DPT_Vector )
						{
							const Vector v = *(const Vector *)( (const char *)pBase + nv.offset + nv.stride * e );
							META_CONPRINTF( "      [%2d] = %.2f %.2f %.2f\n", e, v.x, v.y, v.z );
						}
						else
							META_CONPRINTF( "      [%2d] = %d\n", e, NetVar_GetArrayInt( pBase, nv, e ) );
					}
				}
				break;
			}

			default:
				META_CONPRINTF( "  %-32s +%-5d s=%-3d int    = %d\n",
				                nv.name, nv.offset, nv.stride, NetVar_GetInt( pBase, nv ) );
				break;
		}
	}

	if ( pFilter != NULL && printed == 0 )
	{
		META_CONPRINTF( "[AgPB] no field matched '%s'\n", pFilter );
	}
}

/**
 * agpb_nethandle <idx> <field-name>
 *
 * 把一个 EHANDLE 字段解包成 entry / serial，再解析回实体。
 * M3 的武器系统（m_hActiveWeapon 等）靠的就是这条链路。
 */
static void Cmd_NetHandle( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_nethandle <idx> <field-name>\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const BotNetVar *pVar = pBot->FindNetVar( args.Arg( 2 ) );
	if ( pVar == NULL )
	{
		META_CONPRINTF( "[AgPB] field '%s' not found (table has %d props)\n",
		                args.Arg( 2 ),
		                ( pBot->NetVarTable() != NULL ) ? pBot->NetVarTable()->Count() : 0 );
		return;
	}

	void *pBase = pBot->NetVarBase();

	// 先把字段本身的信息全部打出来，失败时才能看出原因。
	META_CONPRINTF( "[AgPB] %s offset=+%d type=%d elems=%d stride=%d base=%p\n",
	                pVar->name, pVar->offset, (int)pVar->type, pVar->elements, pVar->stride, pBase );

	if ( pBase == NULL )
		return;

	// 读寄存器宽度由 stride 决定；不拿 stride 做门禁。
	const uintp h = NetVar_ReadHandleRaw( pBase, *pVar );

	META_CONPRINTF( "[AgPB] %s raw=0x%016llX\n", pVar->name, (unsigned long long)h );

	if ( !NetVar_HandleIsValid( h ) )
	{
		META_CONPRINTF( "[AgPB] %s handle is INVALID (empty, INVALID_EHANDLE_INDEX)\n", pVar->name );
		return;
	}

	const int entry = NetVar_HandleEntry( h );
	const int serial = NetVar_HandleSerial( h );

	edict_t *pTarget = pBot->HandleToEdict( h );
	const char *pszClass = AgPB_EntityClassName( pTarget );

	META_CONPRINTF( "[AgPB] %s entry=%d serial=%d resolved=%s class=%s\n",
	                pVar->name, entry, serial,
	                ( pTarget != NULL ) ? "yes" : "NO",
	                ( pszClass != NULL ) ? pszClass : "-" );

	if ( pTarget != NULL || engine == NULL )
		return;

	// 解析失败：扫一遍 edict 表反查这个句柄到底属于谁。
	// 同时验证打包规则 entry = h & ENT_ENTRY_MASK / serial = h >> NUM_ENT_ENTRY_BITS。
	edict_t *pAtEntry = engine->PEntityOfEntIndex( entry );

	if ( pAtEntry != NULL )
	{
		// 类名 + refEHandle 是关键证据：如果 refEHandle == raw，
		// 就说明 entry/偏移全对，只是 edict 的 serial 口径不同。
		META_CONPRINTF( "  edict[%d]: free=%d EdictIndex=%d netSerial=%d refEHandle=0x%016llX class=%s\n",
		                entry, pAtEntry->IsFree() ? 1 : 0,
		                (int)pAtEntry->m_EdictIndex, (int)pAtEntry->m_NetworkSerialNumber,
		                (unsigned long long)AgPB_RefEHandle( pAtEntry ),
		                AgPB_EntityClassName( pAtEntry ) );
	}
	else
	{
		META_CONPRINTF( "  edict[%d]: PEntityOfEntIndex returned NULL\n", entry );
	}

	const int maxEnts = ( gpGlobals != NULL && gpGlobals->maxEntities < MAX_EDICTS )
	                  ? gpGlobals->maxEntities : MAX_EDICTS;

	int matches = 0;

	for ( int i = 0; i < maxEnts; ++i )
	{
		edict_t *pE = engine->PEntityOfEntIndex( i );
		if ( pE == NULL || pE->IsFree() )
			continue;

		// 比的是引擎自己维护的 ref ehandle 整值，不猜 serial 口径。
		if ( AgPB_RefEHandle( pE ) != h )
			continue;

		++matches;
		META_CONPRINTF( "  MATCH edict[%d] EdictIndex=%d serial=%d class=%s\n",
		                i, (int)pE->m_EdictIndex, (int)pE->m_NetworkSerialNumber,
		                AgPB_EntityClassName( pE ) );
	}

	META_CONPRINTF( "  scan of %d edicts done, %d match(es)\n", maxEnts, matches );
}

/**
 * agpb_testmove <idx> <forward> [yaw]
 *
 * 临时验证手段：把 forwardmove / viewangles.y 写进下一条 CUserCmd，
 * 用来确认 IBotController::RunPlayerMove() 真的驱动了玩家。
 *
 * 验证方法：
 *     agpb_testmove 0 400 90
 *     agpb_netlist 0 m_vecVelocity     // 三个分量应变非零，bot 开始走
 *     agpb_netlist 0 m_angEyeAngles    // [1] 应等于 90
 *
 * 验证完即可删除；M3 移植的 control 模块会取代它。
 */
static void Cmd_TestMove( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_testmove <idx> <forward> [yaw]\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const float forward = (float)atof( args.Arg( 2 ) );
	const float yaw = ( args.ArgC() >= 4 ) ? (float)atof( args.Arg( 3 ) ) : 0.0f;

	pBot->SetTestInput( forward, yaw );

	META_CONPRINTF( "[AgPB] %s test input set: forward=%.1f yaw=%.1f\n",
	                pBot->Name(), forward, yaw );
}

static ConCommand agpb_add_cmd( "agpb_add", Cmd_Add,
                                "Create an AgPB bot. [team]", FCVAR_GAMEDLL );
static ConCommand agpb_kick_cmd( "agpb_kick", Cmd_Kick,
                                 "Remove bot(s): agpb_kick <idx|all>", FCVAR_GAMEDLL );
static ConCommand agpb_list_cmd( "agpb_list", Cmd_List,
                                 "List all AgPB bots.", FCVAR_GAMEDLL );
static ConCommand agpb_team_cmd( "agpb_team", Cmd_SetTeam,
                                 "Switch a bot's team: agpb_team <idx> <1=spec|2=T|3=CT>", FCVAR_GAMEDLL );
static ConCommand agpb_netlist_cmd( "agpb_netlist", Cmd_NetList,
                                 "Dump a bot's netvar table: agpb_netlist <idx> [name-filter]", FCVAR_GAMEDLL );
static ConCommand agpb_nethandle_cmd( "agpb_nethandle", Cmd_NetHandle,
                                 "Resolve an EHANDLE field: agpb_nethandle <idx> <field-name>", FCVAR_GAMEDLL );
static ConCommand agpb_testmove_cmd( "agpb_testmove", Cmd_TestMove,
                                 "TEMPORARY ucmd injection check: agpb_testmove <idx> <forward> [yaw]", FCVAR_GAMEDLL );

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

PLUGIN_EXPOSE( AgPBPlugin, g_AgPBPlugin );

bool AgPBPlugin::Load( PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late )
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT( GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER );
	GET_V_IFACE_CURRENT( GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION );
	GET_V_IFACE_CURRENT( GetEngineFactory, enginetrace, IEngineTrace, INTERFACEVERSION_ENGINETRACE_SERVER );
	GET_V_IFACE_CURRENT( GetEngineFactory, helpers, IServerPluginHelpers, INTERFACEVERSION_ISERVERPLUGINHELPERS );

	GET_V_IFACE_ANY( GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL );
	GET_V_IFACE_ANY( GetServerFactory, gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS );
	GET_V_IFACE_ANY( GetServerFactory, playerinfomanager, IPlayerInfoManager, INTERFACEVERSION_PLAYERINFOMANAGER );
	GET_V_IFACE_ANY( GetServerFactory, gameents, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS );

	gpGlobals = ismm->GetCGlobals();

	g_SMAPI->AddListener( this, this );

	// IServerPluginHelpers::ClientCommand 是唯一能让"服务端本地"执行客户端
	// 命令的途径（最终落到 CBaseClient::ExecuteStringCommand），假客户端必须
	// 走这条路。前提是 MMS 已经把 VSP 监听打开。
	if ( ismm->GetVSPInfo( NULL ) == NULL )
	{
		ismm->EnableVSPListener();
	}

	SH_ADD_HOOK_MEMFUNC( IServerGameDLL, GameFrame, server, this, &AgPBPlugin::Hook_GameFrame, true );
	SH_ADD_HOOK_MEMFUNC( IServerGameClients, ClientDisconnect, gameclients, this, &AgPBPlugin::Hook_ClientDisconnect, true );

	g_pCVar = icvar;
	ConVar_Register( 0, &s_BaseAccessor );

	// 第二个参数传 false：取真正的 server.dll 工厂，而不是合成包装。
	BotEngineContext ctx;
	ctx.pEngine = engine;
	ctx.pPlayerInfoManager = playerinfomanager;
	ctx.pHelpers = helpers;
	ctx.pTrace = enginetrace;
	ctx.pGameEnts = gameents;

	g_Bots.Init( ctx, ismm->GetServerFactory( false ) );

	META_CONPRINTF( "[AgPB] loaded. gpGlobals=%p, IBotManager=%p, helpers=%p\n",
	                gpGlobals, g_Bots.BotManager(), helpers );

	if ( !g_Bots.IsReady() )
	{
		META_CONPRINTF( "[AgPB] warning: interface '%s' not found, agpb_add will not work.\n",
		                INTERFACEVERSION_PLAYERBOTMANAGER );
	}

	return true;
}

bool AgPBPlugin::Unload( char *error, size_t maxlen )
{
	SH_REMOVE_HOOK_MEMFUNC( IServerGameDLL, GameFrame, server, this, &AgPBPlugin::Hook_GameFrame, true );
	SH_REMOVE_HOOK_MEMFUNC( IServerGameClients, ClientDisconnect, gameclients, this, &AgPBPlugin::Hook_ClientDisconnect, true );

	g_Bots.RemoveAll();
	g_Bots.Shutdown();

	return true;
}

bool AgPBPlugin::Pause( char *error, size_t maxlen )
{
	return true;
}

bool AgPBPlugin::Unpause( char *error, size_t maxlen )
{
	return true;
}

void AgPBPlugin::AllPluginsLoaded()
{
	META_CONPRINTF( "[AgPB] all plugins loaded, IBotManager=%s, helpers=%s\n",
	                g_Bots.IsReady() ? "ready" : "MISSING",
	                helpers ? "ready" : "MISSING" );
}

void AgPBPlugin::OnLevelShutdown()
{
	// 换图会销毁所有 edict，先清掉本地记录，避免悬挂指针。
	g_Bots.RemoveAll();
}

void AgPBPlugin::Hook_GameFrame( bool simulating )
{
	if ( !simulating || gpGlobals == NULL )
		return;

	if ( !agpb_enable.GetBool() )
		return;

	g_Bots.ThinkAll( gpGlobals );
}

void AgPBPlugin::Hook_ClientDisconnect( edict_t *pEntity )
{
	g_Bots.RemoveByEdict( pEntity );
}
