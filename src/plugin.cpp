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
#include <engine/ivdebugoverlay.h>

#include "plugin.h"
#include "bot.h"
#include "waypoint.h"
#include "menu.h"
#include "wpdraw.h"
#include "wpedit.h"

AgPBPlugin g_AgPBPlugin;

SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK1_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, edict_t *);
SH_DECL_HOOK2_void(IServerGameClients, ClientCommand, SH_NOATTRIB, 0, edict_t *, const CCommand &);

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
IVDebugOverlay *debugoverlay = NULL;

static CAgPBManager g_Bots;

CAgPBManager &AgPB_Bots()
{
	return g_Bots;
}

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
 * 命令目标的两种写法（agpb_netlist / agpb_netwrite 共用）：
 *   <bot-idx>              —— bot 列表下标（agpb_list）
 *   ent <edict-idx>        —— 任意实体的 edict 下标（agpb_ents）
 */
struct CmdTargetInfo
{
	edict_t *pEdict;
	char     szName[96];
	int      iArgBase;    // 字段名所在的参数下标
};

/** 解析失败时自己打印原因并返回 false。 */
static bool ResolveCmdTarget( const CCommand &args, CmdTargetInfo &out )
{
	out.pEdict = NULL;
	out.szName[0] = '\0';
	out.iArgBase = 1;

	const bool bEntTarget = ( args.ArgC() >= 2 ) && ( V_stricmp( args.Arg( 1 ), "ent" ) == 0 );

	if ( bEntTarget )
	{
		if ( args.ArgC() < 3 )
		{
			META_CONPRINTF( "[AgPB] usage: ... ent <edict-idx> ...   (edict list: agpb_ents)\n" );
			return false;
		}

		const int iEdict = V_atoi( args.Arg( 2 ) );
		edict_t *pEdict = ( engine != NULL ) ? engine->PEntityOfEntIndex( iEdict ) : NULL;

		if ( pEdict == NULL || pEdict->IsFree() )
		{
			META_CONPRINTF( "[AgPB] no entity at edict index %d (see agpb_ents)\n", iEdict );
			return false;
		}

		const char *pszClass = AgPB_EntityClassName( pEdict );

		out.pEdict = pEdict;
		out.iArgBase = 3;
		Q_snprintf( out.szName, sizeof( out.szName ), "edict[%d] %s",
		            iEdict, ( pszClass != NULL ) ? pszClass : "?" );
		return true;
	}

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );

	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index: %s\n", args.Arg( 1 ) );
		return false;
	}

	out.pEdict = pBot->Edict();
	out.iArgBase = 2;
	Q_snprintf( out.szName, sizeof( out.szName ), "%s", pBot->Name() );
	return true;
}

/**
 * agpb_netlist <bot-idx> [name-filter]
 * agpb_netlist ent <edict-idx> [name-filter]
 *
 * 把某个实体的 ServerClass 展开成 (名字, 偏移, 类型) 列表并打印当前值。
 * 这是 M2 反射层的验证工具，也是移植 EBot Entity 层时查字段名的字典。
 *
 * **能不能写某个字段，唯一可靠的判断方式就是看这张表** —— 它列的是 SendTable
 * （= 网络字段）。非网络字段（例如非玩家实体上的 `m_vecVelocity`）这里根本没有，
 * `agpb_netwrite` 也会回 `field not found`（见 ARCHIVE §6）。
 */
static void Cmd_NetList( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_netlist <bot-idx> [name-filter]\n" );
		META_CONPRINTF( "[AgPB]        agpb_netlist ent <edict-idx> [name-filter]\n" );
		return;
	}

	CmdTargetInfo target;

	if ( !ResolveCmdTarget( args, target ) )
		return;

	const CNetVarTable *pTable = AgPB_EntityNetVarTable( target.pEdict );

	if ( pTable == NULL )
	{
		META_CONPRINTF( "[AgPB] no SendTable for %s\n", target.szName );
		return;
	}

	const char *pFilter = ( args.ArgC() > target.iArgBase ) ? args.Arg( target.iArgBase ) : NULL;
	void *pBase = AgPB_EntityBase( target.pEdict );

	META_CONPRINTF( "[AgPB] %s class=%s props=%d base=%p filter=%s\n",
	                target.szName, pTable->ClassName(), pTable->Count(), pBase,
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

// ---------------------------------------------------------------------------
// agpb_netwrite <bot-idx> <field> <value> [element]
// agpb_netwrite ent <edict-idx> <field> <value> [element]
//
// 开发用：按 SourceMod 的口径写一个网络字段 —— 宽度取 SendProp 的位宽
// （SPROP_VARINT 按 4 字节），写完调 CBaseEdict::StateChanged 通知引擎。
//
// 目标可以是 bot（列表下标），也可以是**任意实体**（edict 下标，用 agpb_ents 查）——
// EBot 的 ssm 写的就是别的实体：手雷的 velocity（`ssm/throw*.cpp`）、
// 箱子的 health（`ssm/destroybreakable.cpp:18`）。
//
// 为什么要打印"邻字段对照"：写错宽度会踩坏邻居字段（ARCHIVE §6），
// 而这种错在游戏里的表现是"某个不相干的数值自己变了"，极难查。
// 这里把目标字段 ±8 字节内的字段在写前 / 写后各读一遍，谁被踩了一眼可见。
// ---------------------------------------------------------------------------

// 注意：Q_snprintf（= V_snprintf）的长度参数是 int，所以这里一律用 int，
// 免得 size_t 转 int 的 C4267 警告把编译输出刷花。
static void DescribeNetVar( void *pBase, const BotNetVar &nv, char *pszOut, int nMaxLen )
{
	switch ( nv.type )
	{
		case DPT_Float:
			Q_snprintf( pszOut, nMaxLen, "%s +%d float=%.4f",
			            nv.name, nv.offset, NetVar_GetFloat( pBase, nv ) );
			break;

		case DPT_Vector:
		{
			const Vector v = NetVar_GetVector( pBase, nv );
			Q_snprintf( pszOut, nMaxLen, "%s +%d vec=(%.2f %.2f %.2f)",
			            nv.name, nv.offset, v.x, v.y, v.z );
			break;
		}

		case DPT_VectorXY:
		{
			// 只读两个 float：VectorXY 后面紧跟别的字段，按 Vector 读会多读 4 字节
			const float *p = (const float *)( (const char *)pBase + nv.offset );
			Q_snprintf( pszOut, nMaxLen, "%s +%d vec2=(%.2f %.2f)",
			            nv.name, nv.offset, p[0], p[1] );
			break;
		}

		case DPT_String:
			// 不按指针解引用（char* 布局会崩服务端），只报位置
			Q_snprintf( pszOut, nMaxLen, "%s +%d string", nv.name, nv.offset );
			break;

		case DPT_Array:
			Q_snprintf( pszOut, nMaxLen, "%s +%d array[%d x %d]",
			            nv.name, nv.offset, nv.elements, nv.stride );
			break;

		default:
			Q_snprintf( pszOut, nMaxLen, "%s +%d int=%d",
			            nv.name, nv.offset, NetVar_GetInt( pBase, nv ) );
			break;
	}
}

/** 目标字段 ±8 字节内的其它字段（写前 / 写后各来一份对照）。 */
static void BuildNeighbourDump( const CNetVarTable *pTable, void *pBase,
                                const BotNetVar &nv, char *pszOut, int nMaxLen )
{
	int used = 0;
	pszOut[0] = '\0';

	for ( int i = 0; i < pTable->Count(); ++i )
	{
		const BotNetVar &other = pTable->Prop( i );

		if ( V_strcmp( other.name, nv.name ) == 0 )
			continue;

		const int delta = other.offset - nv.offset;

		if ( delta < -8 || delta > 8 )
			continue;

		if ( used + 1 >= nMaxLen )
			break;

		char buf[160];
		DescribeNetVar( pBase, other, buf, (int)sizeof( buf ) );

		if ( used != 0 && used + 3 < nMaxLen )
			used += Q_snprintf( pszOut + used, nMaxLen - used, " | " );

		used += Q_snprintf( pszOut + used, nMaxLen - used, "%s", buf );
	}
}

/**
 * agpb_ents [class-filter]
 *
 * 列出世界里的实体（edict 下标 / 类名 / 血量 / 坐标），用来给
 * `agpb_netwrite ent <idx> ...` 找目标 —— 想写手雷的速度、箱子的血量，
 * 先得知道那个实体的 edict 下标。
 *
 * 输出上限 60 行，过滤用类名子串（例如 `agpb_ents grenade` / `agpb_ents weapon`）。
 */
static void Cmd_Ents( const CCommand &args )
{
	if ( engine == NULL || gpGlobals == NULL )
	{
		META_CONPRINTF( "[AgPB] engine globals unavailable\n" );
		return;
	}

	const char *pFilter = ( args.ArgC() >= 2 ) ? args.Arg( 1 ) : NULL;
	const int maxEnts = ( gpGlobals->maxEntities < MAX_EDICTS ) ? gpGlobals->maxEntities : MAX_EDICTS;
	int printed = 0;

	META_CONPRINTF( "[AgPB] entities matching '%s' (max %d edicts):\n",
	                ( pFilter != NULL ) ? pFilter : "*", maxEnts );

	for ( int i = 0; i < maxEnts; ++i )
	{
		edict_t *pEdict = engine->PEntityOfEntIndex( i );

		if ( pEdict == NULL || pEdict->IsFree() )
			continue;

		const char *pszClass = AgPB_EntityClassName( pEdict );

		if ( pszClass == NULL )
			continue;

		if ( pFilter != NULL && !ContainsNoCase( pszClass, pFilter ) )
			continue;

		const Vector vOrigin = AgPB_ReadNetVarVector( pEdict, "m_vecOrigin" );
		const int iHealth = AgPB_ReadNetVarInt( pEdict, "m_iHealth", -1 );

		META_CONPRINTF( "  [%4d] %-30s hp=%-4d pos=%.0f,%.0f,%.0f\n",
		                i, pszClass, iHealth, vOrigin.x, vOrigin.y, vOrigin.z );

		if ( ++printed >= 60 )
		{
			META_CONPRINTF( "  ... truncated, narrow it down with a class filter\n" );
			break;
		}
	}

	META_CONPRINTF( "[AgPB] %d entit(ies) listed\n", printed );
}

static void Cmd_NetWrite( const CCommand &args )
{
	if ( args.ArgC() < 4 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_netwrite <bot-idx> <field> <value> [element]\n" );
		META_CONPRINTF( "[AgPB]        agpb_netwrite ent <edict-idx> <field> <value> [element]   (edict: agpb_ents)\n" );
		META_CONPRINTF( "[AgPB]        vec3 field: ... <field> <x> <y> <z>\n" );
		return;
	}

	CmdTargetInfo target;

	if ( !ResolveCmdTarget( args, target ) )
		return;

	const int iFieldArg   = target.iArgBase;
	const int iValueArg   = iFieldArg + 1;
	const int iElementArg = iFieldArg + 2;

	if ( args.ArgC() <= iValueArg )
	{
		META_CONPRINTF( "[AgPB] %s needs a value: <field> <value> [element]\n", target.szName );
		return;
	}

	const CNetVarTable *pTable = AgPB_EntityNetVarTable( target.pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( target.pEdict, args.Arg( iFieldArg ) );
	void *pBase = AgPB_EntityBase( target.pEdict );

	if ( pVar == NULL || pTable == NULL )
	{
		META_CONPRINTF( "[AgPB] field '%s' not found (class=%s, props=%d)\n",
		                args.Arg( iFieldArg ),
		                ( pTable != NULL ) ? pTable->ClassName() : "?",
		                ( pTable != NULL ) ? pTable->Count() : 0 );
		META_CONPRINTF( "[AgPB]   note: only SendTable (networked) fields can be written; see agpb_netlist for the list\n" );
		return;
	}

	if ( pBase == NULL )
	{
		META_CONPRINTF( "[AgPB] entity base pointer unavailable\n" );
		return;
	}

	const int iElement = ( args.ArgC() > iElementArg ) ? V_atoi( args.Arg( iElementArg ) ) : 0;
	const bool bVector = ( pVar->type == DPT_Vector );
	const bool bFloat = NetVar_CanWriteFloat( *pVar );

	if ( bVector )
	{
		// 写之前先把"这一下会动几个字节"摆出来：宽度是引擎给的，不是我猜的
		META_CONPRINTF( "[AgPB] %s %s offset=+%d type=vec3 -> write 12 bytes (3 floats)\n",
		                target.szName, pVar->name, pVar->offset );
	}
	else
	{
		META_CONPRINTF( "[AgPB] %s %s offset=+%d type=%s elems=%d stride=%d bits=%d varint=%s -> width %d\n",
		                target.szName, pVar->name, pVar->offset, NetVarTypeName( pVar->type ),
		                pVar->elements, pVar->stride,
		                ( pVar->pProp != NULL ) ? pVar->pProp->m_nBits : -1,
		                ( pVar->pProp != NULL && ( pVar->pProp->GetFlags() & SPROP_VARINT ) != 0 ) ? "yes" : "no",
		                bFloat ? 4 : NetVar_WriteWidth( *pVar ) );
	}

	if ( !bVector && pVar->type != DPT_Array && args.ArgC() > iElementArg )
	{
		META_CONPRINTF( "[AgPB] note: %s is not an array, element %d will be rejected\n",
		                pVar->name, iElement );
	}

	char szBefore[512];
	char szAfter[512];

	BuildNeighbourDump( pTable, pBase, *pVar, szBefore, (int)sizeof( szBefore ) );

	NetVarWriteResult result;

	if ( bVector )
	{
		if ( args.ArgC() <= iValueArg + 2 )
		{
			META_CONPRINTF( "[AgPB] %s is a vec3 field: give three numbers <x> <y> <z>\n", pVar->name );
			return;
		}

		const Vector v( V_atof( args.Arg( iValueArg ) ),
		                V_atof( args.Arg( iValueArg + 1 ) ),
		                V_atof( args.Arg( iValueArg + 2 ) ) );

		result = AgPB_WriteNetVarVector( target.pEdict, pVar->name, v );
	}
	else if ( bFloat )
	{
		result = AgPB_WriteNetVarFloat( target.pEdict, pVar->name, V_atof( args.Arg( iValueArg ) ), iElement );
	}
	else
	{
		result = AgPB_WriteNetVarInt( target.pEdict, pVar->name, V_atoi( args.Arg( iValueArg ) ), iElement );
	}

	if ( result != NETVAR_WRITE_OK )
	{
		META_CONPRINTF( "[AgPB] write refused: %s\n", NetVarWriteResultName( result ) );
		return;
	}

	char szValue[128];

	if ( pVar->type == DPT_Array && pVar->elementType == DPT_Float )
	{
		Q_snprintf( szValue, sizeof( szValue ), "[%d] = %.4f",
		            iElement, NetVar_GetArrayFloat( pBase, *pVar, iElement ) );
	}
	else if ( pVar->type == DPT_Array )
	{
		Q_snprintf( szValue, sizeof( szValue ), "[%d] = %d",
		            iElement, NetVar_GetArrayInt( pBase, *pVar, iElement ) );
	}
	else if ( pVar->type == DPT_Float )
	{
		Q_snprintf( szValue, sizeof( szValue ), "= %.4f", NetVar_GetFloat( pBase, *pVar ) );
	}
	else if ( pVar->type == DPT_Vector )
	{
		const Vector v = NetVar_GetVector( pBase, *pVar );
		Q_snprintf( szValue, sizeof( szValue ), "= %.2f %.2f %.2f", v.x, v.y, v.z );
	}
	else
	{
		Q_snprintf( szValue, sizeof( szValue ), "= %d (via engine proxy)",
		            NetVar_GetInt( pBase, *pVar ) );
	}

	BuildNeighbourDump( pTable, pBase, *pVar, szAfter, (int)sizeof( szAfter ) );

	META_CONPRINTF( "[AgPB] changed=%s (engine notified at offset %d)\n", pVar->name, pVar->offset );
	META_CONPRINTF( "[AgPB] readback %s %s\n", pVar->name, szValue );
	META_CONPRINTF( "[AgPB] neighbours before: %s\n",
	                ( szBefore[0] != '\0' ) ? szBefore : "(none within +-8 bytes)" );
	META_CONPRINTF( "[AgPB] neighbours after : %s\n",
	                ( szAfter[0] != '\0' ) ? szAfter : "(none within +-8 bytes)" );
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

	// 半径：默认用 EBot 的 wayzone 算法（agpb_wp_autowayzone 0 时跳过）
	AgPB_EditAutoRadius( iIndex, NULL );

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

/**
 * agpb_bot_goto <idx> [waypoint]
 *
 * 最小导航验证：让 bot 沿路点图走到目标点。不给 waypoint 就用"编辑器当前目标点"
 * （准星指向的点，没有就用缓存点）—— 打两个点、把 bot 派过去，就是这么用。
 */
static void Cmd_BotGoto( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_bot_goto <idx> [waypoint]  (default: the waypoint you aim at)\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );

	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index '%s' (see agpb_list)\n", args.Arg( 1 ) );
		return;
	}

	int iGoal = -1;

	if ( args.ArgC() >= 3 )
	{
		iGoal = V_atoi( args.Arg( 2 ) );
	}
	else
	{
		AgPB_RefreshHost();
		iGoal = AgPB_EditTargetWaypoint();

		if ( !BotWaypoints().IsValid( iGoal ) )
		{
			META_CONPRINTF( "[AgPB] no target waypoint: aim at one, or agpb_wp_cache it first\n" );
			return;
		}
	}

	char szError[256];
	szError[0] = '\0';

	if ( !pBot->StartRoute( iGoal, szError, sizeof( szError ) ) )
	{
		META_CONPRINTF( "[AgPB] %s: %s\n", pBot->Name(), szError );
		return;
	}

	META_CONPRINTF( "[AgPB] %s: walking to #%d, %d hop(s):", pBot->Name(), iGoal, pBot->RouteCount() );

	for ( int i = 0; i < pBot->RouteCount(); ++i )
		META_CONPRINTF( " %d", pBot->RouteNode( i ) );

	META_CONPRINTF( "\n" );
}

/** agpb_bot_stop <idx|all> —— 停止行走（清掉路线与输入）。 */
/**
 * agpb_bot_vel <idx> <x> <y> <z>
 *
 * 【弹道跳开发用】下一 tick 直接把 bot 的 `m_vecVelocity` 灌成这个值（写的是引擎里的
 * 真成员）。写的是"下一 tick、紧挨 RunPlayerMove 之前"，所以引擎的摩擦/重力/
 * 起跳冲量都会作用在它之上 —— 这正是弹道跳需要的时序。
 */
static void Cmd_BotVel( const CCommand &args )
{
	if ( args.ArgC() < 5 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_bot_vel <idx> <x> <y> <z>  (set m_vecVelocity for one tick)\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );

	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index '%s'\n", args.Arg( 1 ) );
		return;
	}

	const Vector vVel( (float)V_atof( args.Arg( 2 ) ),
	                   (float)V_atof( args.Arg( 3 ) ),
	                   (float)V_atof( args.Arg( 4 ) ) );

	pBot->SetVelocityOverride( vVel );

	META_CONPRINTF( "[AgPB] %s: next tick m_vecVelocity = (%.0f %.0f %.0f)\n",
	                pBot->Name(), vVel.x, vVel.y, vVel.z );
}

static void Cmd_BotStop( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		META_CONPRINTF( "[AgPB] usage: agpb_bot_stop <idx|all>\n" );
		return;
	}

	if ( V_stricmp( args.Arg( 1 ), "all" ) == 0 )
	{
		for ( int i = 0; i < g_Bots.Count(); ++i )
		{
			if ( CAgPB *pBot = g_Bots.Get( i ) )
				pBot->StopRoute();
		}

		META_CONPRINTF( "[AgPB] all routes stopped\n" );
		return;
	}

	CAgPB *pBot = g_Bots.Get( V_atoi( args.Arg( 1 ) ) );

	if ( pBot == NULL )
	{
		META_CONPRINTF( "[AgPB] invalid list index '%s'\n", args.Arg( 1 ) );
		return;
	}

	pBot->StopRoute();
	META_CONPRINTF( "[AgPB] %s: route stopped\n", pBot->Name() );
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
                                 "Dump a netvar table: agpb_netlist <bot-idx|ent <edict>> [name-filter]", FCVAR_GAMEDLL );
static ConCommand agpb_nethandle_cmd( "agpb_nethandle", Cmd_NetHandle,
                                 "Resolve an EHANDLE field: agpb_nethandle <idx> <field-name>", FCVAR_GAMEDLL );
static ConCommand agpb_netwrite_cmd( "agpb_netwrite", Cmd_NetWrite,
                                 "DEV: write a netvar field (width from the SendProp bit count): agpb_netwrite <bot-idx|ent <edict>> <field> <value> [element]", FCVAR_GAMEDLL );
static ConCommand agpb_ents_cmd( "agpb_ents", Cmd_Ents,
                                 "List world entities (edict index / class / hp / pos): agpb_ents [class-filter]", FCVAR_GAMEDLL );
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
// 路点编辑器 / 菜单（实现在 wpedit.cpp，这里只注册）
// ---------------------------------------------------------------------------

static ConCommand agpb_menu_cmd( "agpb_menu", AgPB_Cmd_Menu,
                                 "Open the AgPB menu, or pick an item: agpb_menu [n]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_show_cmd( "agpb_wp_show", AgPB_Cmd_Show,
                                 "Draw/hide the waypoint graph: agpb_wp_show [0|1]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_labels_cmd( "agpb_wp_labels", AgPB_Cmd_Labels,
                                 "Draw the index above every waypoint: agpb_wp_labels [0|1]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_alllinks_cmd( "agpb_wp_alllinks", AgPB_Cmd_AllLinks,
                                 "Draw all links or only the nearest one's: agpb_wp_alllinks [0|1]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_cache_cmd( "agpb_wp_cache", AgPB_Cmd_Cache,
                                 "Cache the nearest waypoint (link target).", FCVAR_GAMEDLL );
static ConCommand agpb_wp_type_cmd( "agpb_wp_type", AgPB_Cmd_Type,
                                 "Add a waypoint of a type at your position: agpb_wp_type <type>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_flag_cmd( "agpb_wp_flag", AgPB_Cmd_Flag,
                                 "Toggle a flag on the nearest waypoint: agpb_wp_flag <flag|clear>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_radius_cmd( "agpb_wp_radius", AgPB_Cmd_Radius,
                                 "Set the nearest waypoint's radius: agpb_wp_radius <0..255>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_connect_cmd( "agpb_wp_connect", AgPB_Cmd_Connect,
                                 "Link nearest -> target: agpb_wp_connect <out|in|both|jump|boost|visible>", FCVAR_GAMEDLL );
static ConCommand agpb_wp_cut_cmd( "agpb_wp_cut", AgPB_Cmd_Cut,
                                 "Remove the link between nearest and target.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_teleport_cmd( "agpb_wp_teleport", AgPB_Cmd_Teleport,
                                 "Teleport you to a waypoint: agpb_wp_teleport [idx]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_noclip_cmd( "agpb_wp_noclip", AgPB_Cmd_Noclip,
                                 "Toggle noclip on you (needs sv_cheats 1).", FCVAR_GAMEDLL );
static ConCommand agpb_wp_check_cmd( "agpb_wp_check", AgPB_Cmd_Check,
                                 "Check the waypoint graph for structural errors.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_stats_cmd( "agpb_wp_stats", AgPB_Cmd_Stats,
                                 "Print waypoint/link statistics for this map.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_legend_cmd( "agpb_wp_legend", AgPB_Cmd_Legend,
                                 "Print the waypoint color legend to your console.", FCVAR_GAMEDLL );
static ConCommand agpb_wp_wayzone_cmd( "agpb_wp_wayzone", AgPB_Cmd_Wayzone,
                                 "Recompute the arrival radius (wayzone): agpb_wp_wayzone [idx|all]", FCVAR_GAMEDLL );
static ConCommand agpb_wp_reach_cmd( "agpb_wp_reach", AgPB_Cmd_Reach,
                                 "Geometry check from where you stand: agpb_wp_reach [idx]", FCVAR_GAMEDLL );

// 最小导航（M3 验证：让 bot 真的沿路点图走起来）
static ConCommand agpb_bot_goto_cmd( "agpb_bot_goto", Cmd_BotGoto,
                                 "Walk a bot to a waypoint: agpb_bot_goto <idx> [waypoint]", FCVAR_GAMEDLL );
static ConCommand agpb_bot_stop_cmd( "agpb_bot_stop", Cmd_BotStop,
                                 "Stop a bot's route: agpb_bot_stop <idx|all>", FCVAR_GAMEDLL );
static ConCommand agpb_bot_vel_cmd( "agpb_bot_vel", Cmd_BotVel,
                                 "DEV: write the bot's m_vecVelocity for one tick: agpb_bot_vel <idx> <x> <y> <z>",
                                 FCVAR_GAMEDLL );

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
	GET_V_IFACE_CURRENT( GetEngineFactory, debugoverlay, IVDebugOverlay, VDEBUG_OVERLAY_INTERFACE_VERSION );

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
	SH_ADD_HOOK_MEMFUNC( IServerGameClients, ClientCommand, gameclients, this, &AgPBPlugin::Hook_ClientCommand, false );

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

	// 绘制要 IVDebugOverlay；拿不到就自动不画（菜单那条路不依赖它）。
	AgPB_DrawSetOverlay( debugoverlay );

	META_CONPRINTF( "[AgPB] loaded. gpGlobals=%p, IBotManager=%p, helpers=%p, overlay=%s, showmenu=%s\n",
	                gpGlobals, g_Bots.BotManager(), helpers,
	                ( debugoverlay != NULL ) ? "ok" : "MISSING",
	                ( g_SMAPI->FindUserMessage( "ShowMenu", NULL ) >= 0 ) ? "ok" : "MISSING" );

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
	SH_REMOVE_HOOK_MEMFUNC( IServerGameClients, ClientCommand, gameclients, this, &AgPBPlugin::Hook_ClientCommand, false );

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

	// 路点编辑器与 bot 驱动无关：agpb_enable=0 时也要能画图 / 打点
	AgPB_EditorFrame();

	// 菜单续命（客户端 5 秒没输入会自己收，靠重发顶着）
	AgPB_MenuTick();

	if ( !agpb_enable.GetBool() )
		return;

	g_Bots.ThinkAll( gpGlobals );
}

void AgPBPlugin::Hook_ClientDisconnect( edict_t *pEntity )
{
	AgPB_MenuForget( pEntity );
	g_Bots.RemoveByEdict( pEntity );
}

/**
 * 客户端命令钩子。
 *
 * 菜单点选回到服务器有两条路：
 *   1. HUD 菜单（CreateMessage）里每个选项的 command = "agpb_menu <n>"
 *   2. EBot 风格的 menuselect <n>（数字键）
 * 两者都在这里分发；menuselect 只有在本插件的菜单开着时才吞掉，
 * 免得把 CS 自己的买枪 / 选队菜单弄坏。
 */
void AgPBPlugin::Hook_ClientCommand( edict_t *pEntity, const CCommand &args )
{
	if ( pEntity == NULL || pEntity->IsFree() || args.ArgC() <= 0 )
		RETURN_META( MRES_IGNORED );

	const char *pszCmd = args.Arg( 0 );

	if ( V_stricmp( pszCmd, "agpb_menu" ) == 0 )
	{
		if ( args.ArgC() >= 2 )
		{
			if ( AgPB_MenuSelect( pEntity, V_atoi( args.Arg( 1 ) ) ) )
				RETURN_META( MRES_SUPERCEDE );
		}
		else
		{
			AgPB_OpenMenu( pEntity, AgPB_MENU_MAIN );
			RETURN_META( MRES_SUPERCEDE );
		}

		RETURN_META( MRES_IGNORED );
	}

	if ( V_stricmp( pszCmd, "menuselect" ) == 0 &&
	     args.ArgC() >= 2 &&
	     AgPB_MenuCurrent( pEntity ) != AgPB_MENU_NONE )
	{
		if ( AgPB_MenuSelect( pEntity, V_atoi( args.Arg( 1 ) ) ) )
			RETURN_META( MRES_SUPERCEDE );
	}

	RETURN_META( MRES_IGNORED );
}
