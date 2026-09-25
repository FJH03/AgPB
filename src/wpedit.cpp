/**
 * AgPB - 路点编辑器实现，见 wpedit.h。
 *
 * 这一层对应 EBot 的 Waypoint 编辑器（ToggleFlags / SetRadius /
 * CreateWaypointPath / Delete / CacheWaypoint / TeleportWaypoint /
 * NodesValid / GetWaypointInfo）和它那套 g_menus[] 路点菜单。
 * 输出字符串一律英文（SRCDS 控制台是 GBK，中文会乱码）。
 */

#include <tier1/strtools.h>
#include <trace.h>
#include <engine/IEngineTrace.h>
#include <ihandleentity.h>
#include <iserverunknown.h>

#include "plugin.h"
#include "bot.h"
#include "menu.h"
#include "waypoint.h"
#include "wpdraw.h"
#include "wpedit.h"

extern IVEngineServer *engine;
extern ICvar *icvar;
extern IEngineTrace *enginetrace;
extern IPlayerInfoManager *playerinfomanager;
extern IServerPluginHelpers *helpers;
extern IServerGameEnts *gameents;
extern CGlobalVars *gpGlobals;

// 绘制开关（EBot 的 ebot_show_waypoints 对应物）
ConVar agpb_wp_show( "agpb_wp_show", "0", FCVAR_GAMEDLL,
                     "Draw the waypoint graph with the debug overlay (0/1)." );
ConVar agpb_wp_labels( "agpb_wp_labels", "0", FCVAR_GAMEDLL,
                       "Draw the index number above every waypoint (0/1)." );
ConVar agpb_wp_alllinks( "agpb_wp_alllinks", "1", FCVAR_GAMEDLL,
                         "Draw every link (1) or only the nearest waypoint's links (0)." );
ConVar agpb_wp_thick( "agpb_wp_thick", "3", FCVAR_GAMEDLL,
                      "Overlay line thickness: how many offset passes to draw (1..5)." );
ConVar agpb_wp_xray( "agpb_wp_xray", "1", FCVAR_GAMEDLL,
                     "Draw the waypoint graph through walls (1/0)." );
ConVar agpb_wp_autowayzone( "agpb_wp_autowayzone", "1", FCVAR_GAMEDLL,
                            "On add, compute the arrival radius by scanning the terrain "
                            "(EBot's CalculateWayzone). 0 = use the type defaults (64/32/0)." );

// 起点的最大吸附距离（EBot 一律用 75）
#define AgPB_PICK_RANGE 75.0f

// 准星指向判定的水平角容差（EBot GetFacingIndex 的 range = 5.32）
#define AgPB_FACING_RANGE 6.0f

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

AgPBWaypointEditor &BotWaypointEditor()
{
	static AgPBWaypointEditor s_Editor;
	return s_Editor;
}

AgPBWaypointEditor::AgPBWaypointEditor()
{
	bShow = false;
	bLabels = false;
	bAllLinks = true;

	iCache = -1;
	iNearest = -1;
	iFacing = -1;

	vHostOrigin = Vector( 0.0f, 0.0f, 0.0f );
	vHostEye = Vector( 0.0f, 0.0f, 0.0f );
	angHostView = QAngle( 0.0f, 0.0f, 0.0f );

	bHostValid = false;
}

/** 输出：优先打到客户端控制台（listen server 上就是房主自己）。 */
static void PrintTo( edict_t *pClient, const char *pszFormat, ... )
{
	char szText[512];
	va_list args;

	va_start( args, pszFormat );
	V_vsnprintf( szText, sizeof( szText ), pszFormat, args );
	va_end( args );

	if ( pClient != NULL && engine != NULL )
		engine->ClientPrintf( pClient, szText );
	else
		META_CONPRINTF( "[AgPB] %s", szText );
}

edict_t *AgPB_FindHost()
{
	if ( engine == NULL || playerinfomanager == NULL || gpGlobals == NULL )
		return NULL;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		edict_t *pEdict = engine->PEntityOfEntIndex( i );

		if ( pEdict == NULL || pEdict->IsFree() )
			continue;

		IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo( pEdict );

		if ( pInfo == NULL || pInfo->IsFakeClient() )
			continue;

		return pEdict;
	}

	return NULL;
}

/** CBaseEntity*（公开头文件里只有前置声明，当不透明指针用）。 */
static void *EdictNetVarBase( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return NULL;

	IServerUnknown *pUnk = pEdict->GetUnknown();

	if ( pUnk == NULL )
		return NULL;

	return (void *)pUnk->GetBaseEntity();
}

/** 读一个"元素型"字段（m_angEyeAngles[1] 这种），走引擎自己的 proxy。 */
static float ReadHostFloat( edict_t *pEdict, const void *pBase, const char *pszName, float flDefault )
{
	const CNetVarTable *pTable = BotNetVarRegistry().GetForEdict( pEdict );

	if ( pTable == NULL )
		return flDefault;

	const BotNetVar *pVar = pTable->Find( pszName );

	if ( pVar == NULL )
		return flDefault;

	DVariant out;
	out.m_Float = flDefault;

	NetVar_CallProxy( *pVar, pBase, 0, 0, &out );

	return out.m_Float;
}

// ---------------------------------------------------------------------------
// 刷新房主状态
// ---------------------------------------------------------------------------

void AgPB_RefreshHost()
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	CAgPBWaypoints &wp = BotWaypoints();

	ed.iNearest = -1;
	ed.iFacing = -1;
	ed.bHostValid = false;

	edict_t *pHost = AgPB_FindHost();

	if ( pHost == NULL || playerinfomanager == NULL )
		return;

	IPlayerInfo *pInfo = playerinfomanager->GetPlayerInfo( pHost );

	if ( pInfo == NULL )
		return;

	ed.vHostOrigin = pInfo->GetAbsOrigin();

	// 视角优先读 m_angEyeAngles[0..1]（ARCHIVE 实测 +6984/+6988 跟随 CUserCmd），
	// 读不到才退回 GetAbsAngles()（那是身体朝向，不是视线）。
	QAngle angView = pInfo->GetAbsAngles();
	Vector vViewOfs( 0.0f, 0.0f, 64.0f );

	void *pBase = EdictNetVarBase( pHost );

	if ( pBase != NULL )
	{
		const float flPitch = ReadHostFloat( pHost, pBase, "m_angEyeAngles[0]", angView.x );
		const float flYaw   = ReadHostFloat( pHost, pBase, "m_angEyeAngles[1]", angView.y );

		angView = QAngle( flPitch, flYaw, 0.0f );

		vViewOfs.x = ReadHostFloat( pHost, pBase, "m_vecViewOffset[0]", 0.0f );
		vViewOfs.y = ReadHostFloat( pHost, pBase, "m_vecViewOffset[1]", 0.0f );
		vViewOfs.z = ReadHostFloat( pHost, pBase, "m_vecViewOffset[2]", 64.0f );
	}

	ed.angHostView = angView;
	ed.vHostEye = ed.vHostOrigin + vViewOfs;
	ed.bHostValid = true;

	if ( wp.Count() <= 0 )
		return;

	ed.iNearest = wp.FindNearest( ed.vHostOrigin, AgPB_PICK_RANGE );

	// 准星指向的点：水平角差最小 + 通视（EBot GetFacingIndex 的简化版）
	int iBest = -1;
	float flBestDiff = AgPB_FACING_RANGE;

	for ( int i = 0; i < wp.Count(); ++i )
	{
		const AgPBPath *pPath = wp.Get( i );

		if ( pPath == NULL || i == ed.iNearest )
			continue;

		const Vector vTo = pPath->origin - ed.vHostEye;

		if ( vTo.LengthSqr() > ( 500.0f * 500.0f ) )
			continue;

		QAngle angTo;
		VectorAngles( vTo, angTo );

		float flDiff = AngleDiff( angTo.y, angView.y );

		if ( flDiff < 0.0f )
			flDiff = -flDiff;

		if ( flDiff > flBestDiff )
			continue;

		if ( !AgPB_TraceClear( ed.vHostEye, pPath->origin, pHost ) )
			continue;

		iBest = i;
		flBestDiff = flDiff;
	}

	ed.iFacing = iBest;
}

void AgPB_EditorFrame()
{
	AgPBWaypointEditor &ed = BotWaypointEditor();

	ed.bShow = agpb_wp_show.GetBool();
	ed.bLabels = agpb_wp_labels.GetBool();
	ed.bAllLinks = agpb_wp_alllinks.GetBool();

	// 关掉绘制时不做任何刷新：facing 要打 trace，不便宜
	if ( !ed.bShow )
		return;

	AgPB_RefreshHost();

	edict_t *pHost = AgPB_FindHost();

	if ( pHost != NULL )
		AgPB_DrawWaypoints( pHost );
}

int AgPB_EditTargetWaypoint()
{
	AgPBWaypointEditor &ed = BotWaypointEditor();

	if ( BotWaypoints().IsValid( ed.iFacing ) )
		return ed.iFacing;

	return ed.iCache;
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

void AgPB_EditToggleShow( edict_t *pClient )
{
	const bool bNew = !agpb_wp_show.GetBool();

	agpb_wp_show.SetValue( bNew ? 1 : 0 );
	BotWaypointEditor().bShow = bNew;

	PrintTo( pClient, "waypoint drawing %s\n", bNew ? "ON" : "OFF" );
}

void AgPB_EditToggleLabels( edict_t *pClient )
{
	const bool bNew = !agpb_wp_labels.GetBool();

	agpb_wp_labels.SetValue( bNew ? 1 : 0 );
	BotWaypointEditor().bLabels = bNew;

	PrintTo( pClient, "waypoint index labels %s\n", bNew ? "ON" : "OFF" );
}

void AgPB_EditToggleAllLinks( edict_t *pClient )
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	const bool bNew = !ed.bAllLinks;

	agpb_wp_alllinks.SetValue( bNew ? 1 : 0 );
	ed.bAllLinks = bNew;

	PrintTo( pClient, "drawing %s links\n", bNew ? "ALL" : "nearest-only" );
}

/**
 * 按 agpb_wp_autowayzone 决定要不要自动算半径（EBot 的 CalculateWayzone）。
 * 关掉的话调用方会用类型默认值（64/32/0，EBot 加点菜单那套）。
 */
void AgPB_EditAutoRadius( int iIndex, edict_t *pClient )
{
	if ( !agpb_wp_autowayzone.GetBool() )
		return;

	CAgPBWaypoints &wp = BotWaypoints();

	wp.CalculateWayzone( iIndex, ( pClient != NULL ) ? pClient : AgPB_FindHost() );

	const AgPBPath *pPath = wp.Get( iIndex );

	if ( pPath != NULL )
		PrintTo( pClient, "waypoint #%d wayzone radius = %d\n", iIndex, (int)pPath->radius );
}

void AgPB_EditCache( edict_t *pClient )
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	CAgPBWaypoints &wp = BotWaypoints();

	AgPB_RefreshHost();

	if ( !wp.IsValid( ed.iNearest ) )
	{
		ed.iCache = -1;
		PrintTo( pClient, "cache cleared (no waypoint within %.0f units)\n", AgPB_PICK_RANGE );
		return;
	}

	ed.iCache = ed.iNearest;

	const AgPBPath *pPath = wp.Get( ed.iCache );

	PrintTo( pClient, "cached waypoint #%d (%.0f %.0f %.0f)\n",
	         ed.iCache, pPath->origin.x, pPath->origin.y, pPath->origin.z );
}

void AgPB_EditAddType( edict_t *pClient, unsigned int uFlags, const char *pszTypeName )
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	CAgPBWaypoints &wp = BotWaypoints();

	AgPB_RefreshHost();

	if ( !ed.bHostValid )
	{
		PrintTo( pClient, "no human player to take a position from\n" );
		return;
	}

	const int iIndex = wp.Add( ed.vHostOrigin, uFlags );

	if ( iIndex < 0 )
	{
		PrintTo( pClient, "add failed: %s\n", wp.Status() );
		return;
	}

	// 到达半径：
	//   agpb_wp_autowayzone 1（默认）→ EBot 的 CalculateWayzone 按地形算
	//   0 → EBot 加点菜单那套固定值（Normal/T/CT/Rescue = 64，Camp = 32，Avoid = 0）
	if ( agpb_wp_autowayzone.GetBool() )
	{
		AgPB_EditAutoRadius( iIndex, pClient );
	}
	else if ( AgPBPath *pAdded = wp.GetMutable( iIndex ) )
	{
		if ( uFlags & AgPB_WP_AVOID )
			pAdded->radius = 0;
		else if ( uFlags & AgPB_WP_CAMP )
			pAdded->radius = 32;
		else
			pAdded->radius = 64;
	}

	char szFlags[192];
	AgPB_WaypointFlagsString( wp.Get( iIndex )->flags, szFlags, sizeof( szFlags ) );

	PrintTo( pClient, "waypoint #%d added (%s, flags=%s, r=%d) at %.0f %.0f %.0f\n",
	         iIndex,
	         ( pszTypeName != NULL ) ? pszTypeName : "normal",
	         szFlags,
	         (int)wp.Get( iIndex )->radius,
	         ed.vHostOrigin.x, ed.vHostOrigin.y, ed.vHostOrigin.z );

	ed.iNearest = iIndex;
}

void AgPB_EditDeleteNearest( edict_t *pClient )
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	CAgPBWaypoints &wp = BotWaypoints();

	AgPB_RefreshHost();

	if ( !wp.IsValid( ed.iNearest ) )
	{
		PrintTo( pClient, "no waypoint within %.0f units to delete\n", AgPB_PICK_RANGE );
		return;
	}

	const int iIndex = ed.iNearest;
	const Vector vOrigin = wp.Get( iIndex )->origin;

	wp.Delete( iIndex );

	ed.iNearest = -1;

	if ( ed.iCache == iIndex )
		ed.iCache = -1;
	else if ( ed.iCache > iIndex )
		--ed.iCache;

	PrintTo( pClient, "waypoint #%d deleted (%.0f %.0f %.0f); %d left, %d link(s)\n",
	         iIndex, vOrigin.x, vOrigin.y, vOrigin.z, wp.Count(), wp.LinkCount() );
}

void AgPB_EditConnect( edict_t *pClient, int iMode )
{
	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	AgPB_RefreshHost();

	const int iFrom = ed.iNearest;

	if ( !wp.IsValid( iFrom ) )
	{
		PrintTo( pClient, "no waypoint within %.0f units to start from\n", AgPB_PICK_RANGE );
		return;
	}

	const int iTo = AgPB_EditTargetWaypoint();

	if ( !wp.IsValid( iTo ) )
	{
		PrintTo( pClient, "no target waypoint (aim at one, or agpb_wp_cache first)\n" );
		return;
	}

	if ( iTo == iFrom )
	{
		PrintTo( pClient, "cannot connect waypoint #%d to itself\n", iFrom );
		return;
	}

	const char *pszMode = "bothways";
	unsigned int uFlags = 0;
	bool bAdded = false;

	switch ( iMode )
	{
		case 0:
			pszMode = "outgoing";
			bAdded = wp.AddLinkDirected( iFrom, iTo, 0 );
			break;

		case 1:
			pszMode = "incoming";
			bAdded = wp.AddLinkDirected( iTo, iFrom, 0 );
			break;

		case 3:
			pszMode = "jump";
			uFlags = AgPB_PATH_JUMP;
			bAdded = wp.AddLinkDirected( iFrom, iTo, uFlags );

			// 照 EBot 的 AddPath(type=1)：起跳那侧的点也打上 JUMP，
			// 并把它的半径压到 4 —— 机器人得先走到起跳点上再起跳。
			if ( AgPBPath *pFromPath = wp.GetMutable( iFrom ) )
			{
				pFromPath->flags |= AgPB_WP_JUMP;
				pFromPath->radius = 4;
			}
			break;

		case 4:
			pszMode = "boost";
			uFlags = AgPB_PATH_DOUBLE;
			bAdded = wp.AddLinkDirected( iFrom, iTo, uFlags );
			break;

		case 5:
			pszMode = "visible";
			uFlags = AgPB_PATH_VISIBLE;
			bAdded = wp.AddLinkDirected( iFrom, iTo, uFlags );
			break;

		default:
			pszMode = "bothways";
			bAdded = wp.AddLink( iFrom, iTo, 0 );
			break;
	}

	PrintTo( pClient, "%s link %d <-> %d (%s, flags=0x%X); %d link(s) total\n",
	         pszMode, iFrom, iTo, bAdded ? "added" : "updated", uFlags, wp.LinkCount() );
}

void AgPB_EditCut( edict_t *pClient )
{
	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	AgPB_RefreshHost();

	const int iFrom = ed.iNearest;
	const int iTo = AgPB_EditTargetWaypoint();

	if ( !wp.IsValid( iFrom ) || !wp.IsValid( iTo ) )
	{
		PrintTo( pClient, "need a waypoint under you and a target (aim or cache)\n" );
		return;
	}

	const bool bRemoved = wp.RemoveLink( iFrom, iTo );

	PrintTo( pClient, "unlink %d <-> %d: %s; %d link(s) left\n",
	         iFrom, iTo, bRemoved ? "ok" : "not connected", wp.LinkCount() );
}

void AgPB_EditSetRadius( edict_t *pClient, int iRadius )
{
	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	AgPB_RefreshHost();

	AgPBPath *pPath = wp.GetMutable( ed.iNearest );

	if ( pPath == NULL )
	{
		PrintTo( pClient, "no waypoint within %.0f units\n", AgPB_PICK_RANGE );
		return;
	}

	if ( iRadius < 0 )
		iRadius = 0;

	if ( iRadius > 255 )
		iRadius = 255;

	pPath->radius = (unsigned char)iRadius;

	PrintTo( pClient, "waypoint #%d radius = %d\n", ed.iNearest, iRadius );
}

void AgPB_EditToggleFlag( edict_t *pClient, unsigned int uFlag, const char *pszFlagName )
{
	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	AgPB_RefreshHost();

	AgPBPath *pPath = wp.GetMutable( ed.iNearest );

	if ( pPath == NULL )
	{
		PrintTo( pClient, "no waypoint within %.0f units\n", AgPB_PICK_RANGE );
		return;
	}

	if ( ( pPath->flags & uFlag ) != 0 )
		pPath->flags &= ~uFlag;
	else
		pPath->flags |= uFlag;

	char szFlags[192];
	AgPB_WaypointFlagsString( pPath->flags, szFlags, sizeof( szFlags ) );

	PrintTo( pClient, "waypoint #%d flag %s %s; flags now: %s\n",
	         ed.iNearest,
	         ( pszFlagName != NULL ) ? pszFlagName : "?",
	         ( ( pPath->flags & uFlag ) != 0 ) ? "ON" : "OFF",
	         szFlags );
}

void AgPB_EditClearFlags( edict_t *pClient )
{
	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	AgPB_RefreshHost();

	AgPBPath *pPath = wp.GetMutable( ed.iNearest );

	if ( pPath == NULL )
	{
		PrintTo( pClient, "no waypoint within %.0f units\n", AgPB_PICK_RANGE );
		return;
	}

	pPath->flags = 0;

	PrintTo( pClient, "waypoint #%d flags cleared\n", ed.iNearest );
}

void AgPB_EditTeleport( edict_t *pClient, int iIndex )
{
	CAgPBWaypoints &wp = BotWaypoints();

	edict_t *pHost = ( pClient != NULL ) ? pClient : AgPB_FindHost();

	if ( pHost == NULL )
	{
		PrintTo( pClient, "no human player to teleport\n" );
		return;
	}

	const AgPBPath *pPath = wp.Get( iIndex );

	if ( pPath == NULL )
	{
		PrintTo( pClient, "invalid waypoint index %d (%d total)\n", iIndex, wp.Count() );
		return;
	}

	// setpos 是**客户端**命令（CS:S 里受 sv_cheats 限制）：
	// 走 IVEngineServer::ClientCommand（stuffcmd）发给客户端执行。
	if ( engine != NULL )
	{
		engine->ClientCommand( pHost, "setpos %f %f %f",
		                       pPath->origin.x, pPath->origin.y, pPath->origin.z );
	}

	PrintTo( pClient, "teleport to #%d requested (needs sv_cheats 1)\n", iIndex );
}

void AgPB_EditToggleNoclip( edict_t *pClient )
{
	edict_t *pHost = ( pClient != NULL ) ? pClient : AgPB_FindHost();

	if ( pHost == NULL )
	{
		PrintTo( pClient, "no human player\n" );
		return;
	}

	// noclip 是服务端命令：用 IServerPluginHelpers 在服务端本地执行，
	// 不用 IVEngineServer::ClientCommand（那是 stuffcmd，见 ARCHIVE §3.2）。
	if ( helpers != NULL )
		helpers->ClientCommand( pHost, "noclip" );

	PrintTo( pClient, "noclip toggled (needs sv_cheats 1)\n" );
}

void AgPB_EditCheck( edict_t *pClient )
{
	CAgPBWaypoints &wp = BotWaypoints();

	const int nCount = wp.Count();
	int nErrors = 0;
	int nIsolated = 0;
	int nReported = 0;

	for ( int i = 0; i < nCount; ++i )
	{
		const AgPBPath *pPath = wp.Get( i );

		if ( pPath == NULL )
			continue;

		int nOut = 0;
		bool bSelf = false;
		bool bOutOfRange = false;

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iTo = pPath->index[s];

			if ( iTo < 0 )
				continue;

			if ( iTo == i )
			{
				bSelf = true;
				continue;
			}

			if ( iTo >= nCount )
			{
				bOutOfRange = true;
				continue;
			}

			++nOut;
		}

		bool bHasIn = false;

		if ( nOut == 0 )
		{
			for ( int j = 0; j < nCount && !bHasIn; ++j )
			{
				if ( j == i )
					continue;

				const AgPBPath *pOther = wp.Get( j );

				if ( pOther == NULL )
					continue;

				for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
				{
					if ( pOther->index[s] == i )
					{
						bHasIn = true;
						break;
					}
				}
			}
		}

		if ( bOutOfRange )
		{
			++nErrors;

			if ( nReported++ < 10 )
				PrintTo( pClient, "  error: #%d has an out-of-range link index\n", i );
		}

		if ( bSelf )
		{
			++nErrors;

			if ( nReported++ < 10 )
				PrintTo( pClient, "  error: #%d links to itself\n", i );
		}

		if ( nOut == 0 && !bHasIn )
		{
			++nIsolated;

			if ( nReported++ < 10 )
				PrintTo( pClient, "  warning: #%d is isolated (no links at all)\n", i );
		}
	}

	PrintTo( pClient, "check done: %d waypoint(s), %d link(s) | %d error(s), %d isolated\n",
	         nCount, wp.LinkCount(), nErrors, nIsolated );

	if ( nErrors == 0 && nIsolated == 0 )
		PrintTo( pClient, "graph looks fine (note: this is a structural check, not a geometry check)\n" );
}

void AgPB_EditStats( edict_t *pClient )
{
	CAgPBWaypoints &wp = BotWaypoints();

	const int nCount = wp.Count();

	int nT = 0, nCT = 0, nGoal = 0, nRescue = 0, nCamp = 0, nSniper = 0;
	int nAvoid = 0, nUseButton = 0, nLadder = 0, nCrouch = 0, nJump = 0;
	int nLift = 0, nFallCheck = 0, nFallRisk = 0;

	for ( int i = 0; i < nCount; ++i )
	{
		const AgPBPath *pPath = wp.Get( i );

		if ( pPath == NULL )
			continue;

		const unsigned int f = pPath->flags;

		if ( f & AgPB_WP_TERRORIST ) ++nT;
		if ( f & AgPB_WP_COUNTER )   ++nCT;
		if ( f & AgPB_WP_GOAL )      ++nGoal;
		if ( f & AgPB_WP_RESCUE )    ++nRescue;
		if ( f & AgPB_WP_CAMP )      ++nCamp;
		if ( f & AgPB_WP_SNIPER )    ++nSniper;
		if ( f & AgPB_WP_AVOID )     ++nAvoid;
		if ( f & AgPB_WP_USEBUTTON ) ++nUseButton;
		if ( f & AgPB_WP_LADDER )    ++nLadder;
		if ( f & AgPB_WP_CROUCH )    ++nCrouch;
		if ( f & AgPB_WP_JUMP )      ++nJump;
		if ( f & AgPB_WP_LIFT )      ++nLift;
		if ( f & AgPB_WP_FALLCHECK ) ++nFallCheck;
		if ( f & AgPB_WP_FALLRISK )  ++nFallRisk;
	}

	int nJumpLinks = 0, nBoostLinks = 0, nVisibleLinks = 0;
	int nDirected = 0, nPaired = 0;

	for ( int i = 0; i < nCount; ++i )
	{
		const AgPBPath *pPath = wp.Get( i );

		if ( pPath == NULL )
			continue;

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iTo = pPath->index[s];

			if ( iTo < 0 )
				continue;

			++nDirected;

			if ( wp.IsConnected( iTo, i ) )
				++nPaired;

			if ( pPath->connectionFlags[s] & AgPB_PATH_JUMP )    ++nJumpLinks;
			if ( pPath->connectionFlags[s] & AgPB_PATH_DOUBLE )  ++nBoostLinks;
			if ( pPath->connectionFlags[s] & AgPB_PATH_VISIBLE ) ++nVisibleLinks;
		}
	}

	PrintTo( pClient, "map=%s file=%s | %s\n", wp.MapName(), wp.FileName(), wp.Status() );
	PrintTo( pClient, "waypoints: %d  links: %d (one-way: %d)\n",
	         nCount, wp.LinkCount(), nDirected - nPaired );
	PrintTo( pClient, "T=%d CT=%d GOAL=%d RESCUE=%d CAMP=%d SNIPER=%d\n",
	         nT, nCT, nGoal, nRescue, nCamp, nSniper );
	PrintTo( pClient, "AVOID=%d USEBUTTON=%d LADDER=%d CROUCH=%d JUMP=%d LIFT=%d FALLCHECK=%d FALLRISK=%d\n",
	         nAvoid, nUseButton, nLadder, nCrouch, nJump, nLift, nFallCheck, nFallRisk );
	PrintTo( pClient, "link flags: jump=%d boost=%d visible=%d\n",
	         nJumpLinks, nBoostLinks, nVisibleLinks );
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------

/** 菜单抬头那行：把"现在会操作哪几个点"摊开写，打点时不用猜。 */
static void BuildContextMessage( char *pszOut, int iMaxLen )
{
	AgPBWaypointEditor &ed = BotWaypointEditor();
	CAgPBWaypoints &wp = BotWaypoints();

	char szFlags[192];
	Q_strncpy( szFlags, "无", sizeof( szFlags ) );

	char szNear[24];
	char szFacing[24];
	char szCache[24];

	Q_strncpy( szNear, "无", sizeof( szNear ) );
	Q_strncpy( szFacing, "无", sizeof( szFacing ) );
	Q_strncpy( szCache, "无", sizeof( szCache ) );

	int iRadius = 0;

	if ( wp.IsValid( ed.iNearest ) )
	{
		const AgPBPath *pPath = wp.Get( ed.iNearest );

		AgPB_WaypointFlagsString( pPath->flags, szFlags, sizeof( szFlags ) );

		// 公共的名字表是给控制台用的（英文），HUD 这边把它汉化一下
		if ( Q_stricmp( szFlags, "none" ) == 0 )
			Q_strncpy( szFlags, "无", sizeof( szFlags ) );

		iRadius = pPath->radius;

		Q_snprintf( szNear, sizeof( szNear ), "%d", ed.iNearest );
	}

	if ( wp.IsValid( ed.iFacing ) )
		Q_snprintf( szFacing, sizeof( szFacing ), "%d", ed.iFacing );

	if ( wp.IsValid( ed.iCache ) )
		Q_snprintf( szCache, sizeof( szCache ), "%d", ed.iCache );

	Q_snprintf( pszOut, iMaxLen,
	            "%d 个路点 | 最近 %s (半径 %d %s) | 指向 %s | 缓存 %s",
	            wp.Count(), szNear, iRadius, szFlags, szFacing, szCache );
}

void AgPB_OpenMenu( edict_t *pClient, int iMenuId )
{
	if ( pClient == NULL )
		return;

	AgPB_RefreshHost();

	CAgPBMenu menu;
	char szContext[256];

	BuildContextMessage( szContext, sizeof( szContext ) );

	switch ( iMenuId )
	{
		case AgPB_MENU_MAIN:
		{
			menu.Reset( "AgPB 主菜单" );
			menu.AddItem( "路点菜单", 1 );
			menu.AddItem( "显示/隐藏 路点", 2 );
			menu.AddItem( "显示/隐藏 序号", 3 );
			menu.AddItem( "加一个 T bot", 4 );
			menu.AddItem( "加一个 CT bot", 5 );
			menu.AddItem( "踢掉所有 bot", 6 );
			menu.AddItem( "保存路点", 7 );
			menu.AddItem( "路点统计", 8 );
			menu.AddItem( "配色说明(控制台)", 9 );
			break;
		}

		case AgPB_MENU_WP_MAIN:
		{
			menu.Reset( "路点操作 (1/2)", szContext );
			menu.AddItem( "显示/隐藏 路点", 1 );
			menu.AddItem( "记住当前点(缓存)", 2 );
			menu.AddItem( "创建连线...", 3 );
			menu.AddItem( "删除连线(到目标)", 4 );
			menu.AddItem( "添加路点...", 5 );
			menu.AddItem( "删除路点(最近那个)", 6 );
			menu.AddItem( "设置半径...", 7 );
			menu.AddItem( "设置标志...", 8 );
			menu.AddItem( "下一页...", 9 );
			break;
		}

		case AgPB_MENU_WP_PAGE2:
		{
			menu.Reset( "路点操作 (2/2)", szContext );
			menu.AddItem( "路点统计", 1 );
			menu.AddItem( "检查路点", 2 );
			menu.AddItem( "传送到目标点", 3 );
			menu.AddItem( "穿墙(noclip)开关", 4 );
			menu.AddItem( "保存路点", 5 );
			menu.AddItem( "重新载入路点", 6 );
			menu.AddItem( "配色说明(控制台)", 7 );
			menu.AddItem( "上一页...", 8 );
			menu.AddItem( "连线显示:全部/仅最近", 9 );
			break;
		}

		case AgPB_MENU_WP_ADD:
		{
			menu.Reset( "路点类型 (1/2)", szContext );
			menu.AddItem( "普通点", 1 );
			menu.AddItem( "T 重要点", 2 );
			menu.AddItem( "CT 重要点", 3 );
			menu.AddItem( "回避点", 4 );
			menu.AddItem( "人质救援点", 5 );
			menu.AddItem( "蹲守点", 6 );
			menu.AddItem( "地图目标点", 7 );
			menu.AddItem( "跳跃点", 8 );
			menu.AddItem( "更多类型...", 9 );
			break;
		}

		case AgPB_MENU_WP_ADD2:
		{
			menu.Reset( "路点类型 (2/2)", szContext );
			menu.AddItem( "蹲行点", 1 );
			menu.AddItem( "梯子点", 2 );
			menu.AddItem( "需要按按钮", 3 );
			menu.AddItem( "狙击点", 4 );
			menu.AddItem( "等电梯", 5 );
			menu.AddItem( "需要看脚下", 6 );
			menu.AddItem( "别侧移(防掉落)", 7 );
			menu.AddItem( "返回...", 8 );
			break;
		}

		case AgPB_MENU_WP_FLAGS1:
		{
			menu.Reset( "切换路点标志 (1/2)", szContext );
			menu.AddItem( "需要看脚下", 1 );
			menu.AddItem( "T 专用", 2 );
			menu.AddItem( "CT 专用", 3 );
			menu.AddItem( "等电梯", 4 );
			menu.AddItem( "需要按按钮", 5 );
			menu.AddItem( "狙击点", 6 );
			menu.AddItem( "别侧移(防掉落)", 7 );
			menu.AddItem( "清空所有标志", 8 );
			menu.AddItem( "下一页...", 9 );
			break;
		}

		case AgPB_MENU_WP_FLAGS2:
		{
			menu.Reset( "切换路点标志 (2/2)", szContext );
			menu.AddItem( "Crouch", 1 );
			menu.AddItem( "Jump", 2 );
			menu.AddItem( "Camping", 3 );
			menu.AddItem( "Map goal", 4 );
			menu.AddItem( "Rescue zone", 5 );
			menu.AddItem( "Avoid", 6 );
			menu.AddItem( "Ladder", 7 );
			menu.AddItem( "Previous page...", 8 );
			break;
		}

		case AgPB_MENU_WP_RADIUS:
		{
			menu.Reset( "路点半径", szContext );
			menu.AddItem( "半径 0", 1 );
			menu.AddItem( "半径 8", 2 );
			menu.AddItem( "半径 16", 3 );
			menu.AddItem( "半径 32", 4 );
			menu.AddItem( "半径 48", 5 );
			menu.AddItem( "半径 64", 6 );
			menu.AddItem( "半径 80", 7 );
			menu.AddItem( "半径 96", 8 );
			menu.AddItem( "半径 128", 9 );
			break;
		}

		case AgPB_MENU_WP_PATH:
		{
			menu.Reset( "创建连线 (最近点 -> 目标点)", szContext );
			menu.AddItem( "单向:最近->目标", 1 );
			menu.AddItem( "单向:目标->最近", 2 );
			menu.AddItem( "双向", 3 );
			menu.AddItem( "跳跃连线", 4 );
			menu.AddItem( "叠罗汉连线", 5 );
			menu.AddItem( "仅通视", 6 );
			menu.AddItem( "删除这条连线", 7 );
			menu.AddItem( "返回...", 8 );
			menu.AddItem( "添加路点...", 9 );
			break;
		}

		default:
			return;
	}

	AgPB_MenuShow( pClient, iMenuId, menu );
}

static void AddWaypointByFlags( edict_t *pClient, unsigned int uFlags, const char *pszName )
{
	AgPB_EditAddType( pClient, uFlags, pszName );

	// 加完接着看同一页，方便连着打点（EBot 也是加完就重开菜单）
	AgPB_OpenMenu( pClient, AgPB_MENU_WP_ADD );
}

void AgPB_MenuDispatch( edict_t *pClient, int iMenuId, int iChoice )
{
	switch ( iMenuId )
	{
		case AgPB_MENU_MAIN:
		{
			switch ( iChoice )
			{
				case 1: AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 2: AgPB_EditToggleShow( pClient ); break;
				case 3: AgPB_EditToggleLabels( pClient ); break;
				case 4:
				{
					char szError[256];
					if ( AgPB_Bots().Add( AgPB_TEAM_T, szError, sizeof( szError ) ) != NULL )
						META_CONPRINTF( "[AgPB] bot added (T) from menu\n" );
					else
						META_CONPRINTF( "[AgPB] add bot failed: %s\n", szError );
					break;
				}
				case 5:
				{
					char szError[256];
					if ( AgPB_Bots().Add( AgPB_TEAM_CT, szError, sizeof( szError ) ) != NULL )
						META_CONPRINTF( "[AgPB] bot added (CT) from menu\n" );
					else
						META_CONPRINTF( "[AgPB] add bot failed: %s\n", szError );
					break;
				}
				case 6: AgPB_Bots().RemoveAll(); META_CONPRINTF( "[AgPB] all bots kicked\n" ); break;
				case 7: BotWaypoints().Save(); PrintTo( pClient, "waypoints saved: %s\n", BotWaypoints().Status() ); break;
				case 8: AgPB_EditStats( pClient ); break;
				case 9: AgPB_DrawPrintLegend( pClient ); break;
			}
			break;
		}

		case AgPB_MENU_WP_MAIN:
		{
			switch ( iChoice )
			{
				case 1: AgPB_EditToggleShow( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 2: AgPB_EditCache( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 3: AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 4: AgPB_EditCut( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 5: AgPB_OpenMenu( pClient, AgPB_MENU_WP_ADD ); break;
				case 6: AgPB_EditDeleteNearest( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 7: AgPB_OpenMenu( pClient, AgPB_MENU_WP_RADIUS ); break;
				case 8: AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 9: AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
			}
			break;
		}

		case AgPB_MENU_WP_PAGE2:
		{
			switch ( iChoice )
			{
				case 1: AgPB_EditStats( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 2: AgPB_EditCheck( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 3:
				{
					int iTarget = AgPB_EditTargetWaypoint();

					if ( !BotWaypoints().IsValid( iTarget ) )
						iTarget = BotWaypointEditor().iNearest;

					AgPB_EditTeleport( pClient, iTarget );
					AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 );
					break;
				}
				case 4: AgPB_EditToggleNoclip( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 5: BotWaypoints().Save(); PrintTo( pClient, "waypoints saved: %s\n", BotWaypoints().Status() ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 6: BotWaypoints().Load(); PrintTo( pClient, "waypoints loaded: %s\n", BotWaypoints().Status() ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 7: AgPB_DrawPrintLegend( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
				case 8: AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 9: AgPB_EditToggleAllLinks( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PAGE2 ); break;
			}
			break;
		}

		case AgPB_MENU_WP_ADD:
		{
			switch ( iChoice )
			{
				case 1: AddWaypointByFlags( pClient, 0, "normal" ); break;
				case 2: AddWaypointByFlags( pClient, AgPB_WP_TERRORIST, "terrorist" ); break;
				case 3: AddWaypointByFlags( pClient, AgPB_WP_COUNTER, "ct" ); break;
				case 4: AddWaypointByFlags( pClient, AgPB_WP_AVOID, "avoid" ); break;
				case 5: AddWaypointByFlags( pClient, AgPB_WP_RESCUE, "rescue" ); break;
				case 6: AddWaypointByFlags( pClient, AgPB_WP_CAMP, "camp" ); break;
				case 7: AddWaypointByFlags( pClient, AgPB_WP_GOAL, "goal" ); break;
				case 8: AddWaypointByFlags( pClient, AgPB_WP_JUMP, "jump" ); break;
				case 9: AgPB_OpenMenu( pClient, AgPB_MENU_WP_ADD2 ); break;
			}
			break;
		}

		case AgPB_MENU_WP_ADD2:
		{
			switch ( iChoice )
			{
				case 1: AddWaypointByFlags( pClient, AgPB_WP_CROUCH, "crouch" ); break;
				case 2: AddWaypointByFlags( pClient, AgPB_WP_LADDER, "ladder" ); break;
				case 3: AddWaypointByFlags( pClient, AgPB_WP_USEBUTTON, "usebutton" ); break;
				case 4: AddWaypointByFlags( pClient, AgPB_WP_SNIPER, "sniper" ); break;
				case 5: AddWaypointByFlags( pClient, AgPB_WP_LIFT, "lift" ); break;
				case 6: AddWaypointByFlags( pClient, AgPB_WP_FALLCHECK, "fallcheck" ); break;
				case 7: AddWaypointByFlags( pClient, AgPB_WP_FALLRISK, "fallrisk" ); break;
				case 8: AgPB_OpenMenu( pClient, AgPB_MENU_WP_ADD ); break;
			}
			break;
		}

		case AgPB_MENU_WP_FLAGS1:
		{
			switch ( iChoice )
			{
				case 1: AgPB_EditToggleFlag( pClient, AgPB_WP_FALLCHECK, "FALLCHECK" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 2: AgPB_EditToggleFlag( pClient, AgPB_WP_TERRORIST, "T" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 3: AgPB_EditToggleFlag( pClient, AgPB_WP_COUNTER, "CT" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 4: AgPB_EditToggleFlag( pClient, AgPB_WP_LIFT, "LIFT" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 5: AgPB_EditToggleFlag( pClient, AgPB_WP_USEBUTTON, "USEBUTTON" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 6: AgPB_EditToggleFlag( pClient, AgPB_WP_SNIPER, "SNIPER" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 7: AgPB_EditToggleFlag( pClient, AgPB_WP_FALLRISK, "FALLRISK" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 8: AgPB_EditClearFlags( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
				case 9: AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
			}
			break;
		}

		case AgPB_MENU_WP_FLAGS2:
		{
			switch ( iChoice )
			{
				case 1: AgPB_EditToggleFlag( pClient, AgPB_WP_CROUCH, "CROUCH" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 2: AgPB_EditToggleFlag( pClient, AgPB_WP_JUMP, "JUMP" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 3: AgPB_EditToggleFlag( pClient, AgPB_WP_CAMP, "CAMP" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 4: AgPB_EditToggleFlag( pClient, AgPB_WP_GOAL, "GOAL" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 5: AgPB_EditToggleFlag( pClient, AgPB_WP_RESCUE, "RESCUE" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 6: AgPB_EditToggleFlag( pClient, AgPB_WP_AVOID, "AVOID" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 7: AgPB_EditToggleFlag( pClient, AgPB_WP_LADDER, "LADDER" ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS2 ); break;
				case 8: AgPB_OpenMenu( pClient, AgPB_MENU_WP_FLAGS1 ); break;
			}
			break;
		}

		case AgPB_MENU_WP_RADIUS:
		{
			static const int s_Radius[] = { 0, 8, 16, 32, 48, 64, 80, 96, 128 };

			if ( iChoice >= 1 && iChoice <= 9 )
			{
				AgPB_EditSetRadius( pClient, s_Radius[iChoice - 1] );
				AgPB_OpenMenu( pClient, AgPB_MENU_WP_RADIUS );
			}
			break;
		}

		case AgPB_MENU_WP_PATH:
		{
			switch ( iChoice )
			{
				case 1: AgPB_EditConnect( pClient, 0 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 2: AgPB_EditConnect( pClient, 1 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 3: AgPB_EditConnect( pClient, 2 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 4: AgPB_EditConnect( pClient, 3 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 5: AgPB_EditConnect( pClient, 4 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 6: AgPB_EditConnect( pClient, 5 ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 7: AgPB_EditCut( pClient ); AgPB_OpenMenu( pClient, AgPB_MENU_WP_PATH ); break;
				case 8: AgPB_OpenMenu( pClient, AgPB_MENU_WP_MAIN ); break;
				case 9: AgPB_OpenMenu( pClient, AgPB_MENU_WP_ADD ); break;
			}
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// 控制台命令
// ---------------------------------------------------------------------------

void AgPB_Cmd_Menu( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( pHost == NULL )
	{
		META_CONPRINTF( "[AgPB] no human player to show a menu to\n" );
		return;
	}

	if ( args.ArgC() >= 2 && AgPB_MenuSelect( pHost, V_atoi( args.Arg( 1 ) ) ) )
		return;

	AgPB_OpenMenu( pHost, AgPB_MENU_MAIN );
}

void AgPB_Cmd_Show( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( args.ArgC() >= 2 )
	{
		const bool bOn = ( V_atoi( args.Arg( 1 ) ) != 0 );

		agpb_wp_show.SetValue( bOn ? 1 : 0 );
		BotWaypointEditor().bShow = bOn;

		PrintTo( pHost, "waypoint drawing %s\n", bOn ? "ON" : "OFF" );
		return;
	}

	AgPB_EditToggleShow( pHost );
}

void AgPB_Cmd_Labels( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( args.ArgC() >= 2 )
	{
		const bool bOn = ( V_atoi( args.Arg( 1 ) ) != 0 );

		agpb_wp_labels.SetValue( bOn ? 1 : 0 );
		BotWaypointEditor().bLabels = bOn;

		PrintTo( pHost, "waypoint index labels %s\n", bOn ? "ON" : "OFF" );
		return;
	}

	AgPB_EditToggleLabels( pHost );
}

void AgPB_Cmd_AllLinks( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	const bool bOn = ( args.ArgC() >= 2 ) ? ( V_atoi( args.Arg( 1 ) ) != 0 ) : !ed.bAllLinks;

	agpb_wp_alllinks.SetValue( bOn ? 1 : 0 );
	ed.bAllLinks = bOn;

	PrintTo( pHost, "drawing %s links\n", bOn ? "ALL" : "nearest-only" );
}

void AgPB_Cmd_Cache( const CCommand &args )
{
	AgPB_EditCache( AgPB_FindHost() );
}

void AgPB_Cmd_Type( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( args.ArgC() < 2 )
	{
		PrintTo( pHost, "usage: agpb_wp_type <normal|t|ct|avoid|rescue|camp|goal|jump|"
		                "crouch|ladder|usebutton|sniper|lift|fallcheck|fallrisk>\n" );
		return;
	}

	const char *pszName = args.Arg( 1 );
	const unsigned int uFlags = AgPB_WaypointFlagByName( pszName );

	if ( uFlags == 0 && V_stricmp( pszName, "normal" ) != 0 )
	{
		PrintTo( pHost, "unknown waypoint type '%s'\n", pszName );
		return;
	}

	AgPB_EditAddType( pHost, uFlags, pszName );
}

void AgPB_Cmd_Flag( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( args.ArgC() < 2 )
	{
		PrintTo( pHost, "usage: agpb_wp_flag <flag|clear>  (camp goal rescue avoid usebutton "
		                "ladder crouch jump lift fallcheck fallrisk sniper t ct)\n" );
		return;
	}

	if ( V_stricmp( args.Arg( 1 ), "clear" ) == 0 )
	{
		AgPB_EditClearFlags( pHost );
		return;
	}

	const unsigned int uFlag = AgPB_WaypointFlagByName( args.Arg( 1 ) );

	if ( uFlag == 0 )
	{
		PrintTo( pHost, "unknown flag '%s'\n", args.Arg( 1 ) );
		return;
	}

	AgPB_EditToggleFlag( pHost, uFlag, args.Arg( 1 ) );
}

void AgPB_Cmd_Radius( const CCommand &args )
{
	if ( args.ArgC() < 2 )
	{
		PrintTo( AgPB_FindHost(), "usage: agpb_wp_radius <0..255>\n" );
		return;
	}

	AgPB_EditSetRadius( AgPB_FindHost(), V_atoi( args.Arg( 1 ) ) );
}

void AgPB_Cmd_Connect( const CCommand &args )
{
	int iMode = 2;

	if ( args.ArgC() >= 2 )
	{
		const char *pszMode = args.Arg( 1 );

		if ( V_stricmp( pszMode, "out" ) == 0 )
			iMode = 0;
		else if ( V_stricmp( pszMode, "in" ) == 0 )
			iMode = 1;
		else if ( V_stricmp( pszMode, "both" ) == 0 )
			iMode = 2;
		else if ( V_stricmp( pszMode, "jump" ) == 0 )
			iMode = 3;
		else if ( V_stricmp( pszMode, "boost" ) == 0 )
			iMode = 4;
		else if ( V_stricmp( pszMode, "visible" ) == 0 )
			iMode = 5;
		else
		{
			PrintTo( AgPB_FindHost(), "usage: agpb_wp_connect <out|in|both|jump|boost|visible>\n" );
			return;
		}
	}

	AgPB_EditConnect( AgPB_FindHost(), iMode );
}

void AgPB_Cmd_Cut( const CCommand &args )
{
	AgPB_EditCut( AgPB_FindHost() );
}

void AgPB_Cmd_Teleport( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	int iIndex = -1;

	if ( args.ArgC() >= 2 )
		iIndex = V_atoi( args.Arg( 1 ) );
	else
	{
		AgPB_RefreshHost();
		iIndex = AgPB_EditTargetWaypoint();
	}

	AgPB_EditTeleport( pHost, iIndex );
}

void AgPB_Cmd_Noclip( const CCommand &args )
{
	AgPB_EditToggleNoclip( AgPB_FindHost() );
}

void AgPB_Cmd_Check( const CCommand &args )
{
	AgPB_EditCheck( AgPB_FindHost() );
}

void AgPB_Cmd_Stats( const CCommand &args )
{
	AgPB_EditStats( AgPB_FindHost() );
}

void AgPB_Cmd_Legend( const CCommand &args )
{
	edict_t *pHost = AgPB_FindHost();

	if ( pHost != NULL )
		AgPB_DrawPrintLegend( pHost );
}

/** agpb_wp_wayzone [idx|all] —— 用 EBot 的算法重算到达半径。 */
void AgPB_Cmd_Wayzone( const CCommand &args )
{
	CAgPBWaypoints &wp = BotWaypoints();
	edict_t *pHost = AgPB_FindHost();

	if ( args.ArgC() >= 2 && V_stricmp( args.Arg( 1 ), "all" ) == 0 )
	{
		const int nCount = wp.Count();

		for ( int i = 0; i < nCount; ++i )
			wp.CalculateWayzone( i, pHost );

		PrintTo( pHost, "recomputed wayzone radius for %d waypoint(s)\n", nCount );
		return;
	}

	int iIndex = -1;

	if ( args.ArgC() >= 2 )
	{
		iIndex = V_atoi( args.Arg( 1 ) );
	}
	else
	{
		AgPB_RefreshHost();
		iIndex = AgPB_EditTargetWaypoint();

		if ( !wp.IsValid( iIndex ) )
			iIndex = BotWaypointEditor().iNearest;
	}

	if ( !wp.IsValid( iIndex ) )
	{
		PrintTo( pHost, "usage: agpb_wp_wayzone [idx|all] (no waypoint nearby either)\n" );
		return;
	}

	wp.CalculateWayzone( iIndex, pHost );

	const AgPBPath *pPath = wp.Get( iIndex );

	PrintTo( pHost, "waypoint #%d wayzone radius = %d\n", iIndex, ( pPath != NULL ) ? (int)pPath->radius : 0 );
}
