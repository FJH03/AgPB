/**
 * AgPB - Metamod:Source plugin entry point.
 *
 * 目标引擎：Counter-Strike: Source（Source 1 / x86_64）
 * 不使用 SourceMod，也不使用引擎自带的 CCSBot。
 */

#include <eiface.h>
#include <edict.h>
#include <icvar.h>
#include <tier1/convar.h>
#include <tier1/strtools.h>
#include <filesystem.h>
#include <engine/IEngineTrace.h>

#include "plugin.h"
#include "bot.h"
#include "waypoint.h"

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
IFileSystem *filesystem = NULL;

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
		team = V_atoi( args.Arg( 1 ) );

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
	if ( args.ArgC() >= 2 && !V_stricmp( args.Arg( 1 ), "all" ) )
	{
		const int n = g_Bots.Count();
		g_Bots.RemoveAll();
		META_CONPRINTF( "[AgPB] removed %d bot(s).\n", n );
		return;
	}

	const int index = ( args.ArgC() >= 2 ) ? V_atoi( args.Arg( 1 ) ) : -1;
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

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const int team = V_atoi( args.Arg( 2 ) );
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

	// V_stristr 就是大小写无关的子串查找，不用自己逐字符 tolower
	return ( V_stristr( haystack, needle ) != NULL );
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

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );
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
				Q_memcpy( buf, (const char *)pBase + nv.offset, sizeof( buf ) - 1 );
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

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );
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

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );
	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return;
	}

	const float forward = (float)V_atof( args.Arg( 2 ) );
	const float yaw = ( args.ArgC() >= 4 ) ? (float)V_atof( args.Arg( 3 ) ) : 0.0f;

	pBot->SetTestInput( forward, yaw );

	META_CONPRINTF( "[AgPB] %s test input set: forward=%.1f yaw=%.1f\n",
	                pBot->Name(), forward, yaw );
}

// ---------------------------------------------------------------------------
// 路点编辑器（agpb_wp_*）
//
// 为什么自己打点：.nav 只描述"哪块地面能站"，"怎么从 A 到 B"要靠几何去推，
// 推错了就是那条"nav 说连通、物理上过不去"的连接。
// 路点图的每条连边是人验证过的动作，不存在推导。
// ---------------------------------------------------------------------------

/**
 * 取"你"的位置 —— 打点命令不带坐标时用这个。
 *
 * MMS 的 ConCommand 回调不带 client 信息，但在 listen server 上
 * "第一个非假客户端玩家"就是你自己，够用了。
 */
static bool FindHumanOrigin( Vector &vOut )
{
	if ( engine == NULL || playerinfomanager == NULL || gpGlobals == NULL )
		return false;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		edict_t *pEdict = engine->PEntityOfEntIndex( i );

		if ( pEdict == NULL || pEdict->IsFree() )
			continue;

		IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo( pEdict );

		if ( pInfo == NULL || pInfo->IsFakeClient() )
			continue;

		vOut = pInfo->GetAbsOrigin();
		return true;
	}

	return false;
}

/** agpb_wp_add [x y z] —— 加一个路点；不给坐标就用你当前位置。 */
static void Cmd_WpAdd( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();

	Vector vOrigin( 0.0f, 0.0f, 0.0f );

	if ( args.ArgC() >= 4 )
	{
		vOrigin.x = (float)V_atof( args.Arg( 1 ) );
		vOrigin.y = (float)V_atof( args.Arg( 2 ) );
		vOrigin.z = (float)V_atof( args.Arg( 3 ) );
	}
	else if ( !FindHumanOrigin( vOrigin ) )
	{
		META_CONPRINTF( "[AgPB] no human player to take a position from; pass x y z\n" );
		return;
	}

	const int iIndex = wp.Add( vOrigin );

	if ( iIndex < 0 )
	{
		META_CONPRINTF( "[AgPB] wp_add failed: %s\n", wp.Status() );
		return;
	}

	META_CONPRINTF( "[AgPB] waypoint %d added at %.0f %.0f %.0f (%d total, %d links)\n",
	                iIndex, vOrigin.x, vOrigin.y, vOrigin.z, wp.Count(), wp.LinkCount() );
}

/** agpb_wp_del <idx> */
static void Cmd_WpDel( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_wp_del <idx>\n" );
		return;
	}

	CAgPBWaypoints &wp = BotWaypoints();
	const int iIndex = V_atoi( args.Arg( 1 ) );

	if ( !wp.IsValid( iIndex ) )
	{
		META_CONPRINTF( "[AgPB] invalid waypoint index %d (%d total)\n", iIndex, wp.Count() );
		return;
	}

	wp.Delete( iIndex );

	META_CONPRINTF( "[AgPB] waypoint %d deleted (%d left, %d links)\n",
	                iIndex, wp.Count(), wp.LinkCount() );
}

/** agpb_wp_link <from> <to> [flags] —— flags 是 PATHFLAG_* 组合（1=跳 2=连跳 4=仅通视） */
static void Cmd_WpLink( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_wp_link <from> <to> [flags]\n" );
		return;
	}

	const int iFrom = V_atoi( args.Arg( 1 ) );
	const int iTo   = V_atoi( args.Arg( 2 ) );
	const unsigned int iFlags = ( args.ArgC() >= 4 ) ? (unsigned int)V_atoi( args.Arg( 3 ) ) : 0u;

	CAgPBWaypoints &wp = BotWaypoints();

	if ( !wp.IsValid( iFrom ) || !wp.IsValid( iTo ) )
	{
		META_CONPRINTF( "[AgPB] invalid index (%d/%d of %d)\n", iFrom, iTo, wp.Count() );
		return;
	}

	const bool bAdded = wp.AddLink( iFrom, iTo, iFlags );

	META_CONPRINTF( "[AgPB] link %d <-> %d flags=0x%X (%s); %d links total\n",
	                iFrom, iTo, iFlags, bAdded ? "added" : "updated", wp.LinkCount() );
}

/** agpb_wp_unlink <from> <to> */
static void Cmd_WpUnlink( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_wp_unlink <from> <to>\n" );
		return;
	}

	CAgPBWaypoints &wp = BotWaypoints();

	const bool bRemoved = wp.RemoveLink( V_atoi( args.Arg( 1 ) ), V_atoi( args.Arg( 2 ) ) );

	META_CONPRINTF( "[AgPB] unlink %s; %d links left\n",
	                bRemoved ? "ok" : "not connected", wp.LinkCount() );
}

/** agpb_wp_list [max] */
static void Cmd_WpList( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();

	META_CONPRINTF( "[AgPB] map=%s file=%s\n", wp.MapName(), wp.FileName() );
	META_CONPRINTF( "[AgPB] %d waypoint(s), %d link(s) | %s\n",
	                wp.Count(), wp.LinkCount(), wp.Status() );

	int nMax = ( args.ArgC() >= 2 ) ? V_atoi( args.Arg( 1 ) ) : 32;

	if ( nMax <= 0 || nMax > wp.Count() )
		nMax = wp.Count();

	for ( int i = 0; i < nMax; ++i )
	{
		const AgPBPath *p = wp.Get( i );

		if ( p == NULL )
			continue;

		META_CONPRINTF( "  [%3d] %.0f %.0f %.0f  flags=0x%X  r=%d  ",
		                i, p->origin.x, p->origin.y, p->origin.z,
		                p->flags, (int)p->radius );

		int nLink = 0;

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			if ( p->index[s] < 0 )
				continue;

			META_CONPRINTF( "%s%d/0x%X", ( nLink > 0 ) ? "," : "->",
			                (int)p->index[s], (unsigned int)p->connectionFlags[s] );
			++nLink;
		}

		META_CONPRINTF( ( nLink > 0 ) ? "\n" : "(no links)\n" );
	}
}

/** agpb_wp_nearest —— 报告离你最近/最远的路点，方便手工连线时找下标。 */
static void Cmd_WpNearest( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();

	Vector vOrigin( 0.0f, 0.0f, 0.0f );

	if ( !FindHumanOrigin( vOrigin ) )
	{
		META_CONPRINTF( "[AgPB] no human player\n" );
		return;
	}

	const int iNear = wp.FindNearest( vOrigin );
	const int iFar  = wp.FindFarthest( vOrigin );

	META_CONPRINTF( "[AgPB] you at %.0f %.0f %.0f\n", vOrigin.x, vOrigin.y, vOrigin.z );

	if ( iNear >= 0 )
	{
		const AgPBPath *p = wp.Get( iNear );
		META_CONPRINTF( "[AgPB] nearest = %d at %.0f %.0f %.0f\n",
		                iNear, p->origin.x, p->origin.y, p->origin.z );
	}
	else
	{
		META_CONPRINTF( "[AgPB] nearest = none (no waypoints yet)\n" );
	}

	if ( iFar >= 0 && iFar != iNear )
	{
		const AgPBPath *p = wp.Get( iFar );
		META_CONPRINTF( "[AgPB] farthest = %d at %.0f %.0f %.0f\n",
		                iFar, p->origin.x, p->origin.y, p->origin.z );
	}
}

static void Cmd_WpSave( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();

	const bool bOk = wp.Save();

	META_CONPRINTF( "[AgPB] wp_save %s: %s\n", bOk ? "ok" : "FAILED", wp.Status() );
}

static void Cmd_WpLoad( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();

	wp.Load();

	META_CONPRINTF( "[AgPB] wp_load: %s\n", wp.Status() );
}

static void Cmd_WpClear( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();
	const int nWas = wp.Count();

	wp.Clear();

	META_CONPRINTF( "[AgPB] cleared %d waypoint(s) from memory (file untouched)\n", nWas );
}

/**
 * agpb_wp_path <to> | agpb_wp_path <from> <to>
 * 跑一遍 A* 把路径打出来。单参数时起点取"离你最近的路点"。
 * 这是 S3 的验收命令 —— 不用起 bot 就能验证路点图连通性和寻路。
 */
static void Cmd_WpPath( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_wp_path <to> | agpb_wp_path <from> <to>\n" );
		return;
	}

	CAgPBWaypoints &wp = BotWaypoints();

	int iFrom = -1;
	int iTo   = -1;

	if ( args.ArgC() >= 3 )
	{
		iFrom = V_atoi( args.Arg( 1 ) );
		iTo   = V_atoi( args.Arg( 2 ) );
	}
	else
	{
		iTo = V_atoi( args.Arg( 1 ) );

		Vector vOrigin( 0.0f, 0.0f, 0.0f );

		if ( !FindHumanOrigin( vOrigin ) )
		{
			META_CONPRINTF( "[AgPB] no human player; pass <from> <to>\n" );
			return;
		}

		iFrom = wp.FindNearest( vOrigin );
	}

	if ( !wp.IsValid( iFrom ) || !wp.IsValid( iTo ) )
	{
		META_CONPRINTF( "[AgPB] invalid index (%d -> %d of %d)\n", iFrom, iTo, wp.Count() );
		return;
	}

	CUtlVector< int > vecPath;

	if ( !wp.FindPath( iFrom, iTo, vecPath ) )
	{
		META_CONPRINTF( "[AgPB] no path %d -> %d (%d point(s), %d link(s) in graph)\n",
		                iFrom, iTo, wp.Count(), wp.LinkCount() );
		return;
	}

	META_CONPRINTF( "[AgPB] path %d -> %d: %d hop(s)\n", iFrom, iTo, vecPath.Count() );

	for ( int i = 0; i < vecPath.Count(); ++i )
	{
		const AgPBPath *p = wp.Get( vecPath[i] );
		const AgPBPath *pPrev = ( i > 0 ) ? wp.Get( vecPath[i - 1] ) : NULL;

		if ( p == NULL )
			continue;

		float flLeg = 0.0f;

		if ( pPrev != NULL )
		{
			const Vector vDelta = p->origin - pPrev->origin;
			flLeg = vDelta.Length();
		}

		unsigned int uLinkFlags = 0;

		if ( pPrev != NULL )
			wp.IsConnected( vecPath[i - 1], vecPath[i], &uLinkFlags );

		META_CONPRINTF( "  %2d. [%3d] %.0f %.0f %.0f  flags=0x%X  leg=%.0f link=0x%X\n",
		                i, vecPath[i], p->origin.x, p->origin.y, p->origin.z,
		                p->flags, flLeg, uLinkFlags );
	}

	META_CONPRINTF( "[AgPB] path distance = %.0f (straight line = %.0f)\n",
	                wp.PathDistance( iFrom, iTo ),
	                ( wp.Get( iTo )->origin - wp.Get( iFrom )->origin ).Length() );
}

/** agpb_wp_dist <from> <to> —— 沿路点图的代价（LADDER/CROUCH 点位带 ×2 权重）。 */
static void Cmd_WpDist( const CCommand &args )
{
	if ( args.ArgC() < 3 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_wp_dist <from> <to>\n" );
		return;
	}

	CAgPBWaypoints &wp = BotWaypoints();

	const int iFrom = V_atoi( args.Arg( 1 ) );
	const int iTo   = V_atoi( args.Arg( 2 ) );

	if ( !wp.IsValid( iFrom ) || !wp.IsValid( iTo ) )
	{
		META_CONPRINTF( "[AgPB] invalid index (%d/%d of %d)\n", iFrom, iTo, wp.Count() );
		return;
	}

	const float flDist = wp.PathDistance( iFrom, iTo );

	if ( flDist >= AgPB_WP_DIST_INF )
	{
		META_CONPRINTF( "[AgPB] %d -> %d: unreachable\n", iFrom, iTo );
		return;
	}

	META_CONPRINTF( "[AgPB] %d -> %d: path %.0f, straight line %.0f\n",
	                iFrom, iTo, flDist,
	                ( wp.Get( iTo )->origin - wp.Get( iFrom )->origin ).Length() );
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

static ConCommand agpb_wp_add_cmd( "agpb_wp_add", Cmd_WpAdd,
                                 "Add a waypoint: agpb_wp_add [x y z]  (default: your position)", FCVAR_GAMEDLL );
static ConCommand agpb_wp_del_cmd( "agpb_wp_del", Cmd_WpDel,
                                 "Delete a waypoint: agpb_wp_del <idx>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_link_cmd( "agpb_wp_link", Cmd_WpLink,
                                 "Link two waypoints: agpb_wp_link <from> <to> [flags]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_unlink_cmd( "agpb_wp_unlink", Cmd_WpUnlink,
                                 "Unlink two waypoints: agpb_wp_unlink <from> <to>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_list_cmd( "agpb_wp_list", Cmd_WpList,
                                 "List waypoints: agpb_wp_list [max]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_nearest_cmd( "agpb_wp_nearest", Cmd_WpNearest,
                                 "Show nearest/farthest waypoint to you.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_save_cmd( "agpb_wp_save", Cmd_WpSave,
                                 "Save waypoints for this map.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_load_cmd( "agpb_wp_load", Cmd_WpLoad,
                                 "Reload waypoints for this map.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_clear_cmd( "agpb_wp_clear", Cmd_WpClear,
                                 "Drop all in-memory waypoints (file untouched).", FCVAR_GAMEDLL );
static ConCommand agpb_wp_path_cmd( "agpb_wp_path", Cmd_WpPath,
                                 "A* over the waypoint graph: agpb_wp_path <to> | <from> <to>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_dist_cmd( "agpb_wp_dist", Cmd_WpDist,
                                 "Path distance between two waypoints: agpb_wp_dist <from> <to>", FCVAR_GAMEDLL );

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
	GET_V_IFACE_CURRENT( GetEngineFactory, filesystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION );

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

	// 路点要读写 addons/AgPB/waypoints/*.agpw；插件链接不到 game DLL 里的
	// filesystem 全局，自己拿一份交给它。
	BotWaypoints().SetFileSystem( filesystem );

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

	// 让路点模块跟着当前地图走。SetMapName 内部只做一次字符串比较，
	// 只有换图（或第一次进图）才真的去读盘。
	//
	// 注意 gpGlobals->mapname 是 string_t，不能拿 0/NULL 去比
	// （string_t.h:34 里它是"指针式"的），STRING() 对空表返回 ""。
	{
		const char *pszMap = STRING( gpGlobals->mapname );

		if ( pszMap != NULL && pszMap[0] != '\0' )
			BotWaypoints().SetMapName( pszMap );
	}

	g_Bots.ThinkAll( gpGlobals );
}

void AgPBPlugin::Hook_ClientDisconnect( edict_t *pEntity )
{
	g_Bots.RemoveByEdict( pEntity );
}
