/**
 * AgPB - bot 生命周期、队伍切换与底层命令注入。
 */

#include <tier1/strtools.h>

#include <iserverunknown.h>
#include <ihandleentity.h>
#include <in_buttons.h>
#include <const.h>
#include <mathlib/mathlib.h>

#include <ISmmPlugin.h>

#include "plugin.h"
#include "bot.h"
#include "waypoint.h"

extern CGlobalVars *gpGlobals;

// ---------------------------------------------------------------------------
// 最小导航的参数
// ---------------------------------------------------------------------------

// USP 跑速上限（ARCHIVE §10 实测：forwardmove=400 时 m_vecVelocity[1] 顶到 250）
#define AgPB_WALK_SPEED      250.0f

// 到达判定的两根时间轴（EBot navigate.cpp:643-690 的简化版）：
// 高速掠过时用"沿当前速度走一小段后的最近距离"来判断，而不是只看当前距离
#define AgPB_ARRIVE_MIN_T     0.10f
#define AgPB_ARRIVE_LOOKAHEAD 0.20f

// 卡住判定：每 AgPB_STUCK_WINDOW 秒看一次，位移小于 AgPB_STUCK_MOVE 就算卡住
#define AgPB_STUCK_WINDOW    2.0f
#define AgPB_STUCK_MOVE      24.0f

// 下一段是跳跃边时，离起跳点还有这么远就松开蹲（退蹲要 0.2 秒，留足余量）
#define AgPB_UNDUCK_AHEAD    120.0f

// 找起点时允许的最大吸附距离
#define AgPB_ROUTE_PICK      512.0f

/**
 * 在"服务端本地"执行一条客户端命令。
 *
 * 绝对不要用 IVEngineServer::ClientCommand()：它的实现是 stuffcmd
 * （engine/vengineserver_impl.cpp:1006），通过 NET_StringCmd + SendNetMsg
 * 把命令发给客户端控制台，假客户端没有 netchannel，命令会石沉大海。
 *
 * IServerPluginHelpers::ClientCommand 走的是
 *   CServerPlugin::ClientCommand -> CBaseClient::ExecuteStringCommand
 * 完全在服务端本地执行，假客户端也有效。
 */
static void RunClientCommand( IServerPluginHelpers *pHelpers, edict_t *pEdict, const char *pszCmd )
{
	if ( pHelpers == NULL || pEdict == NULL || pszCmd == NULL )
		return;

	pHelpers->ClientCommand( pEdict, pszCmd );
}

CAgPB::CAgPB()
{
	m_Name[0] = '\0';
	m_pEdict = NULL;
	m_pController = NULL;
	m_pInfo = NULL;
	m_iIndex = 0;
	m_iTeam = 0;
	m_iCommandNumber = 0;
	m_bClassRequested = false;
	m_flNextJoinAttempt = 0.0f;
	m_flJoinDeadline = -1.0f;
	m_vVelOverride = Vector( 0.0f, 0.0f, 0.0f );
	m_bHasVelOverride = false;
	m_iRouteIndex = 0;
	m_iGoalWaypoint = -1;
	m_vStuckAnchor = Vector( 0.0f, 0.0f, 0.0f );
	m_flStuckCheckTime = 0.0f;
}

bool CAgPB::Create( const BotEngineContext &ctx,
						const char *name,
						int team,
						char *error,
						size_t maxlen )
{
	if ( !ctx.IsReady() )
	{
		if ( error && maxlen )
			Q_snprintf( error, (int)maxlen, "IBotManager / IVEngineServer / IPlayerInfoManager unavailable" );
		return false;
	}

	// server.dll 的实现（CPluginBotManager::CreateBot）会：
	//   1. engine->CreateFakeClient(name)
	//   2. 打上 FL_CLIENT | FL_FAKECLIENT
	//   3. ChangeTeam(TEAM_UNASSIGNED) + RemoveAllItems(true) + Spawn()
	// 全程不创建 CCSBot，因此引擎自带的 bot AI 不会介入。
	edict_t *pEdict = ctx.pBotManager->CreateBot( name );
	if ( pEdict == NULL )
	{
		if ( error && maxlen )
			Q_snprintf( error, (int)maxlen, "CreateBot() failed: no free client slot" );
		return false;
	}

	m_Ctx = ctx;
	m_pEdict = pEdict;
	m_iIndex = ctx.pEngine->IndexOfEdict( pEdict );

	Q_strncpy( m_Name, name, sizeof( m_Name ) - 1 );
	m_Name[sizeof( m_Name ) - 1] = '\0';

	m_pController = ctx.pBotManager->GetBotController( pEdict );
	if ( m_pController == NULL )
	{
		if ( error && maxlen )
			Q_snprintf( error, (int)maxlen, "GetBotController() returned NULL (fake client not recognised as a bot?)" );
		return false;
	}

	m_pInfo = ctx.pPlayerInfoManager->GetPlayerInfo( pEdict );

	// 非法队伍值等于"不主动入队"，交给 SetTeam 校验。
	m_iTeam = 0;
	SetTeam( team );

	return true;
}

void CAgPB::Destroy()
{
	if ( m_Ctx.pEngine != NULL && m_pEdict != NULL )
	{
		// Kick the fake client so the engine recycles the slot.
		// IVEngineServer::ServerCommand() 不是变参函数，必须先格式化。
		char szCmd[64];
		Q_snprintf( szCmd, (int)sizeof( szCmd ), "kickid %d\n", m_Ctx.pEngine->GetPlayerUserId( m_pEdict ) );
		m_Ctx.pEngine->ServerCommand( szCmd );
	}

	m_pEdict = NULL;
	m_pController = NULL;
	m_pInfo = NULL;
}

bool CAgPB::SetTeam( int team )
{
	switch ( team )
	{
		case AgPB_TEAM_SPECTATOR:
		case AgPB_TEAM_T:
		case AgPB_TEAM_CT:
			break;

		default:
			return false;
	}

	m_iTeam = team;

	// 重新开始一轮切换流程。
	m_bClassRequested = false;
	m_flJoinDeadline = -1.0f;
	m_flNextJoinAttempt = 0.0f;

	return true;
}

void CAgPB::TryJoinTeam( float flCurTime )
{
	if ( m_pInfo == NULL || m_iTeam <= 0 )
		return;

	const int iCurTeam = m_pInfo->GetTeamIndex();

	if ( iCurTeam == m_iTeam )
	{
		// 已经在目标队伍。
		// T / CT 还需要补一次"选模型"：CCSPlayer::ChangeTeam() 的
		// active-player 分支只把玩家停在 STATE_PICKINGCLASS，而
		// CCSGameRules::FPlayerCanRespawn() 要求
		// GetClass() != CS_CLASS_NONE，不发这一步不会出生。
		// 观察者不需要。
		if ( ( m_iTeam == AgPB_TEAM_T || m_iTeam == AgPB_TEAM_CT ) && !m_bClassRequested )
		{
			m_bClassRequested = true;
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, "joinclass 0" );
		}

		return;
	}

	if ( m_flJoinDeadline < 0.0f )
		m_flJoinDeadline = flCurTime + 15.0f;

	if ( flCurTime >= m_flJoinDeadline || flCurTime < m_flNextJoinAttempt )
		return;

	// 首选：直通 CCSPlayer::ChangeTeam()，不经命令通道，没有限流。
	m_pInfo->ChangeTeam( m_iTeam );

	// 兜底：若该队伍编号没有对应的 CTeam 对象，
	// CCSPlayer::ChangeTeam() 会打印 "invalid team index" 并直接返回。
	// 这种情况改走客户端命令通道（HandleCommand_JoinTeam 的检查更宽松）。
	if ( m_pInfo->GetTeamIndex() != m_iTeam )
	{
		if ( m_iTeam == AgPB_TEAM_SPECTATOR )
		{
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, "spectate" );
		}
		else
		{
			char szCmd[32];
			Q_snprintf( szCmd, (int)sizeof( szCmd ), "jointeam %d", m_iTeam );
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, szCmd );
		}
	}

	m_flNextJoinAttempt = flCurTime + 0.5f;
}

void CAgPB::Think( CGlobalVars *pGlobals )
{
	if ( !IsValid() || pGlobals == NULL )
		return;

	TryJoinTeam( pGlobals->curtime );

	// 即使 bot 处于死亡/未出生状态，也必须每 tick 调用一次
	// RunPlayerMove()：武器逻辑、动画与 PostThink 都挂在
	// CPlayerMove::RunCommand 的调用链上。
	//
	// 目前不下发任何输入 —— 移动 / 瞄准 / 战斗将由移植过来的
	// control / navigate / combat 模块填充这里。
	CBotCmd cmd;
	cmd.Reset();

	// 有路线就走路线（最小导航）。
	if ( HasRoute() )
	{
		UpdateRoute( pGlobals, cmd );
	}

	// 【弹道跳/调试】把本 tick 的速度覆盖值写进引擎（真成员地址），
	// 时序：这里 -> RunPlayerMove -> 引擎的 GroundMove/AirMove 在这个速度上继续算
	// （摩擦、重力，以及 CheckJumpButton 里那个 `+=` 冲量都会叠加在它之上）。
	if ( m_bHasVelOverride )
	{
		SetNetVarVector( "m_vecVelocity", m_vVelOverride );
		m_bHasVelOverride = false;
	}

	cmd.command_number = ++m_iCommandNumber;
	cmd.tick_count = pGlobals->tickcount;
	// 线性同余凑一个每帧不同的种子（武器散布随机流用），省掉 <stdlib.h> 的 rand()
	cmd.random_seed = (int)( (unsigned int)pGlobals->tickcount * 1103515245u + 12345u );

	m_pController->RunPlayerMove( &cmd );
}

// ---------------------------------------------------------------------------
// 最小导航（验证用）
// ---------------------------------------------------------------------------

int CAgPB::RouteNode( int i ) const
{
	if ( i < 0 || i >= m_vecRoute.Count() )
		return -1;

	return m_vecRoute[i];
}

void CAgPB::RouteReport( const char *pszFormat, ... ) const
{
	char szText[512];
	va_list args;

	va_start( args, pszFormat );
	V_vsnprintf( szText, sizeof( szText ), pszFormat, args );
	va_end( args );

	META_CONPRINTF( "[AgPB] %s: %s\n", m_Name, szText );
}

void CAgPB::StopRoute()
{
	m_vecRoute.RemoveAll();
	m_iRouteIndex = 0;
	m_iGoalWaypoint = -1;
}

bool CAgPB::StartRoute( int iGoalWaypoint, char *error, size_t maxlen )
{
	CAgPBWaypoints &wp = BotWaypoints();

	StopRoute();

	if ( !IsValid() )
	{
		Q_strncpy( error, "bot is not valid (spawned yet?)", (int)maxlen );
		return false;
	}

	if ( m_pInfo == NULL )
	{
		Q_strncpy( error, "no player info", (int)maxlen );
		return false;
	}

	if ( !wp.IsValid( iGoalWaypoint ) )
	{
		Q_snprintf( error, (int)maxlen, "invalid waypoint index %d (%d total)", iGoalWaypoint, wp.Count() );
		return false;
	}

	const Vector vPos = m_pInfo->GetAbsOrigin();
	const int iStart = wp.FindNearest( vPos, AgPB_ROUTE_PICK );

	if ( !wp.IsValid( iStart ) )
	{
		Q_snprintf( error, (int)maxlen, "no waypoint within %.0f units of the bot", AgPB_ROUTE_PICK );
		return false;
	}

	CUtlVector<int> vecPath;

	if ( !wp.FindPath( iStart, iGoalWaypoint, vecPath, m_iTeam ) )
	{
		Q_snprintf( error, (int)maxlen, "no path %d -> %d (graph disconnected?)", iStart, iGoalWaypoint );
		return false;
	}

	if ( vecPath.Count() < 2 )
	{
		Q_snprintf( error, (int)maxlen, "already standing on waypoint #%d", iGoalWaypoint );
		return false;
	}

	m_vecRoute = vecPath;
	m_iRouteIndex = 1;              // [0] 是脚下这个点，从第 1 段开始走
	m_iGoalWaypoint = iGoalWaypoint;
	m_vStuckAnchor = vPos;
	m_flStuckCheckTime = ( gpGlobals != NULL ) ? ( gpGlobals->curtime + AgPB_STUCK_WINDOW ) : 0.0f;

	return true;
}

void CAgPB::UpdateRoute( CGlobalVars *pGlobals, CBotCmd &cmd )
{
	if ( !HasRoute() || m_pInfo == NULL )
		return;

	CAgPBWaypoints &wp = BotWaypoints();

	const int iNode = m_vecRoute[m_iRouteIndex];
	const AgPBPath *pNode = wp.Get( iNode );

	if ( pNode == NULL )
	{
		RouteReport( "route aborted: waypoint #%d no longer exists", iNode );
		StopRoute();
		return;
	}

	// 这一段要不要跳 —— 看"上一点 -> 这一点"那条边的标志
	unsigned int uLegFlags = 0;

	if ( m_iRouteIndex > 0 )
		wp.IsConnected( m_vecRoute[m_iRouteIndex - 1], m_vecRoute[m_iRouteIndex], &uLegFlags );

	const Vector vPos = m_pInfo->GetAbsOrigin();
	const Vector vDelta = pNode->origin - vPos;
	const float flDist2D = vDelta.Length2D();

	// 到达判定照 EBot（navigate.cpp:643-690）：
	//   radius >= 50 且不是跳跃边 → 进半径就算到（给"大范围的点"用）
	//   否则（小半径 / 蹲点 / 跳点）→ 要走到 max(radius, 4) 以内才算到，
	//   而且移动中要用速度外推一次，免得高速从点旁边擦过去被判成"没到"然后绕圈
	const bool bJumpLeg = ( uLegFlags & AgPB_PATH_JUMP ) != 0;
	const float flRadius = (float)pNode->radius;

	bool bArrived = false;

	if ( flRadius >= 50.0f && !bJumpLeg )
	{
		bArrived = ( flDist2D < flRadius );
	}
	else
	{
		const float flCheck = ( flRadius > 4.0f ) ? flRadius : 4.0f;

		if ( flDist2D < flCheck )
		{
			bArrived = true;
		}
		else
		{
			const Vector vVel = GetNetVarVector( "m_vecVelocity" );
			const float flSpeed2 = vVel.x * vVel.x + vVel.y * vVel.y;

			if ( flSpeed2 > 1.0f )
			{
				float flT = ( vDelta.x * vVel.x + vDelta.y * vVel.y ) / flSpeed2;

				if ( flT < AgPB_ARRIVE_MIN_T )
					flT = AgPB_ARRIVE_MIN_T;
				else
				{
					float flLookahead = AgPB_ARRIVE_LOOKAHEAD;

					if ( flRadius < 5.0f || bJumpLeg )
						flLookahead *= 1.5f;

					if ( flT > flLookahead )
						flT = flLookahead;
				}

				const Vector vClosest = vPos + Vector( vVel.x * flT, vVel.y * flT, 0.0f );

				if ( ( vClosest - pNode->origin ).Length2D() < flCheck )
					bArrived = true;
			}
		}
	}

	if ( bArrived )
	{
		++m_iRouteIndex;

		if ( m_iRouteIndex >= m_vecRoute.Count() )
		{
			RouteReport( "arrived at waypoint #%d (%d hop(s) walked)", m_iGoalWaypoint, m_vecRoute.Count() - 1 );
			StopRoute();
		}

		return;
	}

	// 朝下一个点转（视角会跟随 CUserCmd，ARCHIVE §10 已实测）
	QAngle angTo;
	VectorAngles( vDelta, angTo );
	cmd.viewangles.y = angTo.y;

	// 前进（USP 跑速上限 250）
	cmd.forwardmove = AgPB_WALK_SPEED;

	// 蹲：EBot 的 WAYPOINT_CROUCH 意思是"必须蹲着才能到达这个点"
	//   - 正要走向的点带 CROUCH → 一路按着蹲
	//   - 刚离开的点带 CROUCH 而且离得还近（人还在矮区里）→ 继续蹲，别站起来顶天花板
	bool bCrouch = ( ( pNode->flags & AgPB_WP_CROUCH ) != 0 );

	if ( !bCrouch && m_iRouteIndex > 0 )
	{
		const AgPBPath *pPrev = wp.Get( m_vecRoute[m_iRouteIndex - 1] );

		if ( pPrev != NULL && ( pPrev->flags & AgPB_WP_CROUCH ) != 0 )
		{
			if ( ( pPrev->origin - vPos ).Length2D() < 64.0f )
				bCrouch = true;
		}
	}

	// 下一段是跳跃边 → 提前松蹲：退出蹲态要 TIME_TO_UNDUCK = 0.2 秒
	// （shareddefs.h:112），不松的话到了起跳点还是"蹲着跳"，只能跳 42
	// （矮区里松蹲是安全的：引擎的 CanUnduck() 会因为头顶没空间而拒绝站起，cs_gamemovement.cpp:857）
	if ( bCrouch && !bJumpLeg && m_iRouteIndex + 1 < m_vecRoute.Count() )
	{
		unsigned int uNextLeg = 0;

		if ( wp.IsConnected( m_vecRoute[m_iRouteIndex], m_vecRoute[m_iRouteIndex + 1], &uNextLeg ) &&
		     ( uNextLeg & AgPB_PATH_JUMP ) != 0 &&
		     flDist2D < AgPB_UNDUCK_AHEAD )
		{
			bCrouch = false;
		}
	}

	// ---- 按钮：跳 / 蹲 —— 严格照 CS:S 引擎的真实机制（cs_gamemovement.cpp）
	//
	// 1) 跳跃边：站地上就按跳，**不按蹲** —— 站着跳 57、蹲着跳只有 42（:723-728）
	// 2) 起跳那一 tick 只要没按蹲，引擎会自动给 bot 接上 crouch-jump：
	//    m_duckUntilOnGround = true + FinishDuck()（:767-772），空中它还替 bot 按着蹲（:173-178），
	//    落地后自己决定何时站起（:1017+）；空中蹲会把脚抬起 8.5 单位，更容易上高台
	// 3) 我们**绝不能**在跳跃边/空中按 IN_DUCK —— 那会把引擎这套自动序列取消掉（:166-171）
	if ( bJumpLeg )
	{
		if ( ( GetNetVarInt( "m_fFlags", 0 ) & FL_ONGROUND ) != 0 )
			cmd.buttons |= IN_JUMP;
	}
	else if ( bCrouch )
	{
		cmd.buttons |= IN_DUCK;
	}

	// 卡住检测：每 AgPB_STUCK_WINDOW 秒看一次位移，没动就放弃并报坐标
	if ( pGlobals != NULL && pGlobals->curtime >= m_flStuckCheckTime )
	{
		if ( ( vPos - m_vStuckAnchor ).Length() < AgPB_STUCK_MOVE )
		{
			RouteReport( "stuck at %.0f %.0f %.0f: no progress towards waypoint #%d (dist %.0f, hop %d/%d) - stopped",
			             vPos.x, vPos.y, vPos.z, iNode, flDist2D,
			             m_iRouteIndex, m_vecRoute.Count() - 1 );
			StopRoute();
			return;
		}

		m_vStuckAnchor = vPos;
		m_flStuckCheckTime = pGlobals->curtime + AgPB_STUCK_WINDOW;
	}
}

// ---------------------------------------------------------------------------
// CAgPBManager
// ---------------------------------------------------------------------------

CAgPBManager::CAgPBManager()
{
	m_iNextSerial = 1;
}

void CAgPBManager::Init( const BotEngineContext &ctx, CreateInterfaceFn pServerFactory )
{
	m_Ctx = ctx;

	// 关键：IBotManager 是 server.dll 通过 CreateInterface 暴露的
	// "BotManager001"（实现类为 CPluginBotManager），而不是引擎接口。
	if ( m_Ctx.pBotManager == NULL && pServerFactory != NULL )
	{
		m_Ctx.pBotManager = (IBotManager *)pServerFactory( INTERFACEVERSION_PLAYERBOTMANAGER, NULL );
	}
}

void CAgPBManager::Shutdown()
{
	RemoveAll();

	m_Ctx = BotEngineContext();
}

CAgPB *CAgPBManager::Add( int team, char *error, size_t maxlen )
{
	if ( !IsReady() )
	{
		if ( error && maxlen )
			Q_snprintf( error, (int)maxlen, "IBotManager ('%s') unavailable", INTERFACEVERSION_PLAYERBOTMANAGER );
		return NULL;
	}

	char szName[64];
Q_snprintf( szName, (int)sizeof( szName ), "AgPB_%02d", m_iNextSerial );

	CAgPB *pBot = new CAgPB();
	if ( !pBot->Create( m_Ctx, szName, team, error, maxlen ) )
	{
		delete pBot;
		return NULL;
	}

	++m_iNextSerial;
	m_Bots.AddToTail( pBot );

	return pBot;
}

bool CAgPBManager::Remove( int listIndex )
{
	if ( listIndex < 0 || listIndex >= m_Bots.Count() )
		return false;

	CAgPB *pBot = m_Bots[listIndex];
	pBot->Destroy();
	delete pBot;

	m_Bots.Remove( listIndex );
	return true;
}

void CAgPBManager::RemoveAll()
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		m_Bots[i]->Destroy();
		delete m_Bots[i];
	}

	m_Bots.RemoveAll();
}

void CAgPBManager::RemoveByEdict( edict_t *pEdict )
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		if ( m_Bots[i]->Edict() == pEdict )
		{
			// 玩家已经断开，不需要再 kick。
			delete m_Bots[i];
			m_Bots.Remove( i );
			return;
		}
	}
}

void CAgPBManager::ThinkAll( CGlobalVars *pGlobals )
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		m_Bots[i]->Think( pGlobals );
	}
}

CAgPB *CAgPBManager::Get( int listIndex )
{
	if ( listIndex < 0 || listIndex >= m_Bots.Count() )
		return NULL;

	return m_Bots[listIndex];
}

// ---------------------------------------------------------------------------
// netvar 反射（M2）
// ---------------------------------------------------------------------------

CNetVarRegistry &BotNetVarRegistry()
{
	// 字段表来自引擎启动时静态构建的 SendTable，生命周期覆盖整个进程，
	// 所以缓存一次就够，不需要任何失效逻辑。
	static CNetVarRegistry s_Registry;
	return s_Registry;
}

void *CAgPB::NetVarBase() const
{
	if ( m_pEdict == NULL )
		return NULL;

	IServerUnknown *pUnk = m_pEdict->GetUnknown();
	if ( pUnk == NULL )
		return NULL;

	// CBaseEntity*，公开头文件里只有前置声明，这里当不透明指针用。
	return (void *)pUnk->GetBaseEntity();
}

const CNetVarTable *CAgPB::NetVarTable() const
{
	if ( m_pEdict == NULL )
		return NULL;

	return BotNetVarRegistry().GetForEdict( m_pEdict );
}

const BotNetVar *CAgPB::FindNetVar( const char *name ) const
{
	const CNetVarTable *pTable = NetVarTable();
	if ( pTable == NULL )
		return NULL;

	return pTable->Find( name );
}

bool CAgPB::GetNetVarBool( const char *name, bool defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetBool( pBase, *pVar );
}

int CAgPB::GetNetVarInt( const char *name, int defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetInt( pBase, *pVar );
}

float CAgPB::GetNetVarFloat( const char *name, float defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetFloat( pBase, *pVar );
}

Vector CAgPB::GetNetVarVector( const char *name ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return vec3_origin;

	return NetVar_GetVector( pBase, *pVar );
}

int CAgPB::GetNetVarArrayInt( const char *name, int index, int defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	if ( pVar->type != DPT_Array || pVar->elementType == DPT_Float )
		return defaultValue;

	if ( index < 0 || index >= pVar->elements || pVar->stride <= 0 )
		return defaultValue;

	return NetVar_GetArrayInt( pBase, *pVar, index );
}

float CAgPB::GetNetVarArrayFloat( const char *name, int index, float defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	if ( pVar->type != DPT_Array || pVar->elementType != DPT_Float )
		return defaultValue;

	if ( index < 0 || index >= pVar->elements || pVar->stride <= 0 )
		return defaultValue;

	return NetVar_GetArrayFloat( pBase, *pVar, index );
}

bool CAgPB::GetNetVarHandleValue( const char *name, uintp *pHandle ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL || pHandle == NULL )
		return false;

	// EHandle 在网络上就是 DPT_Int；数组（DPT_Array）不走这条路。
	// 不用 stride 做门禁：标量的 m_ElementStride 永远是 SIZEOF_IGNORE(-1)。
	if ( pVar->type != DPT_Int )
		return false;

	*pHandle = NetVar_ReadHandleRaw( pBase, *pVar );
	return true;
}

bool CAgPB::GetNetVarHandle( const char *name, int *pEntry, int *pSerial ) const
{
	uintp h = 0;

	if ( !GetNetVarHandleValue( name, &h ) )
		return false;

	if ( !NetVar_HandleIsValid( h ) )
		return false;

	if ( pEntry != NULL )
		*pEntry = NetVar_HandleEntry( h );

	if ( pSerial != NULL )
		*pSerial = NetVar_HandleSerial( h );

	return true;
}

edict_t *CAgPB::HandleToEdict( uintp handle ) const
{
	if ( m_Ctx.pEngine == NULL || !NetVar_HandleIsValid( handle ) )
		return NULL;

	const int entry = NetVar_HandleEntry( handle );

	if ( entry < 0 || entry >= MAX_EDICTS )
		return NULL;

	edict_t *pEdict = m_Ctx.pEngine->PEntityOfEntIndex( entry );
	if ( pEdict == NULL || pEdict->IsFree() )
		return NULL;

	// 权威校验：拿引擎自己维护的 ref ehandle 整值比对。
	// 这同时验证了 entry 是否解对、以及内存里读到的值是否真是该实体的句柄。
	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL )
		return NULL;

	IHandleEntity *pHandleEntity = pNet->GetEntityHandle();
	if ( pHandleEntity == NULL )
		return NULL;

	if ( pHandleEntity->GetRefEHandle().ToInt() != (int)handle )
		return NULL;

	return pEdict;
}

bool CAgPB::SetNetVarFloat( const char *pszName, float flValue )
{
	void *pBase = NetVarBase();
	const BotNetVar *pVar = FindNetVar( pszName );

	if ( pBase == NULL || pVar == NULL )
		return false;

	NetVar_WriteFloat( pBase, *pVar, flValue );
	return true;
}

bool CAgPB::SetNetVarInt( const char *pszName, int iValue )
{
	void *pBase = NetVarBase();
	const BotNetVar *pVar = FindNetVar( pszName );

	if ( pBase == NULL || pVar == NULL )
		return false;

	NetVar_WriteInt( pBase, *pVar, iValue );
	return true;
}

bool CAgPB::SetNetVarVector( const char *pszName, const Vector &vValue )
{
	void *pBase = NetVarBase();

	if ( pBase == NULL || pszName == NULL )
		return false;

	// 优先按 VECTORELEM 的三个元素写（m_vecVelocity 在表里就是 [0]/[1]/[2]）
	static const char *s_szIndex[3] = { "[0]", "[1]", "[2]" };

	char szName[96];

	const float *pValues = &vValue.x;
	bool bAllFound = true;

	for ( int i = 0; i < 3; ++i )
	{
		Q_snprintf( szName, sizeof( szName ), "%s%s", pszName, s_szIndex[i] );

		const BotNetVar *pVar = FindNetVar( szName );

		if ( pVar == NULL )
		{
			bAllFound = false;
			break;
		}

		NetVar_WriteFloat( pBase, *pVar, pValues[i] );
	}

	if ( bAllFound )
		return true;

	// 退路：整条 Vector 字段（DPT_Vector / VectorXY）—— 内存里就是 3 个 float
	const BotNetVar *pVar = FindNetVar( pszName );

	if ( pVar == NULL )
		return false;

	*(Vector *)( (char *)pBase + pVar->offset ) = vValue;
	return true;
}

void CAgPB::SetVelocityOverride( const Vector &vVelocity )
{
	m_vVelOverride = vVelocity;
	m_bHasVelOverride = true;
}

const char *AgPB_EntityClassName( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return NULL;

	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL || pNet->GetServerClass() == NULL )
		return NULL;

	return pNet->GetServerClass()->GetName();
}

uintp AgPB_RefEHandle( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	IHandleEntity *pHandleEntity = pNet->GetEntityHandle();
	if ( pHandleEntity == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	return (uintp)pHandleEntity->GetRefEHandle().ToInt();
}
