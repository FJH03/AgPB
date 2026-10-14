/**
 * AgPB - bot 生命周期、队伍切换与底层命令注入。
 */

#include <tier1/strtools.h>

#include <iserverunknown.h>
#include <ihandleentity.h>
#include <in_buttons.h>
#include <const.h>
#include <mathlib/mathlib.h>
#include <toolframework/itoolentity.h>

#include <ISmmPlugin.h>

#include "plugin.h"
#include "bot.h"
#include "waypoint.h"
#include "obstacle.h"

#include <vstdlib/random.h>

extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();

// 蹲守时长（秒，随机区间）；EBot/SyPB 是 35~60 秒，这里默认短一点方便测试
ConVar agpb_camp_min( "agpb_camp_min", "8", FCVAR_GAMEDLL,
                      "Min seconds a bot camps at a CAMP waypoint." );
ConVar agpb_camp_max( "agpb_camp_max", "20", FCVAR_GAMEDLL,
                      "Max seconds a bot camps at a CAMP waypoint." );

static ConVar agpb_obstacle_debug( "agpb_obstacle_debug", "0", FCVAR_GAMEDLL,
                                  "Log path obstacle changes using Source ZBot-style volume probes." );

// ---------------------------------------------------------------------------
// 最小导航的参数
// ---------------------------------------------------------------------------

// USP 跑速上限（实测：forwardmove=400 时 m_vecVelocity[1] 顶到 250，见 PROGRESS.md 问题 6）
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

// 随机漫游（agpb_bot_roam）：优先挑直线距离 ≥ 300 的点；挑不到可达点就 1 秒后再试
#define AgPB_ROAM_MIN_DIST   300.0f
#define AgPB_ROAM_RETRY      1.0f
#define AgPB_ROAM_TRIES      16
// 漫游选点避开别的 bot（站着的 / 正走向的），太挤就换一个点
#define AgPB_ROAM_SPREAD     400.0f

// 队友让行：正前方这个距离内站着别的 bot 就错身让路（EBot 窄道让路的简化版）
#define AgPB_FRIEND_BLOCK_DIST 72.0f

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
	m_flNextObstacleCheck = 0.0f;
	m_uLastObstacleHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_bLastObstacleFound = false;
	ClearBreakable();
	m_bRoam = false;
	m_flNextRoamTime = 0.0f;
	m_iLastRoamGoal = -1;
	m_flCampUntil = 0.0f;
	m_bCampCrouch = false;
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
	StopRoute();
	m_bRoam = false;

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
	// Navigation and obstacle firing fill the same public command chain.
	CBotCmd cmd;
	cmd.Reset();
	// Releasing movement/attack must not snap an idle bot back to world yaw 0.
	cmd.viewangles.x = GetNetVarFloat( "m_angEyeAngles[0]", 0.0f );
	cmd.viewangles.y = GetNetVarFloat( "m_angEyeAngles[1]", 0.0f );
	if ( m_pInfo != NULL && m_pInfo->IsDead() && HasRoute() )
	{
		StopRoute();
		m_bRoam = false;
	}

	// 有路线就走路线（最小导航）。
	if ( HasRoute() )
	{
		UpdateRoute( pGlobals, cmd );
	}

	// 蹲守：到 CAMP 点后原地待一段随机时间（保持朝向；点带 CROUCH 就蹲着）
	if ( m_flCampUntil > 0.0f && !HasRoute() )
	{
		if ( pGlobals->curtime >= m_flCampUntil )
		{
			m_flCampUntil = 0.0f;
			m_bCampCrouch = false;
		}
		else
		{
			// cmd.Reset() 会把视角清成 0；把当前朝向读回来，站着不乱转
			cmd.viewangles.y = GetNetVarFloat( "m_angEyeAngles[1]", 0.0f );

			if ( m_bCampCrouch )
				cmd.buttons |= IN_DUCK;
		}
	}

	// 随机漫游：路线走完（或没路线）就再随机挑一个点接着走（蹲守时不挑）
	if ( m_bRoam && !HasRoute() && m_flCampUntil <= 0.0f && pGlobals->curtime >= m_flNextRoamTime )
	{
		if ( !StartRandomRoute() )
			m_flNextRoamTime = pGlobals->curtime + AgPB_ROAM_RETRY;
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
	ClearBreakable();
	m_vecRoute.RemoveAll();
	m_iRouteIndex = 0;
	m_iGoalWaypoint = -1;
	m_flNextObstacleCheck = 0.0f;
	m_uLastObstacleHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_bLastObstacleFound = false;
}

void CAgPB::ClearBreakable()
{
	m_uBreakHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_uBreakWeapon = (uintp)INVALID_EHANDLE_INDEX;
	m_vBreakHit.Init();
	m_flBreakDeadline = m_flBreakNextAttack = 0.0f;
	m_iBreakBestHealth = m_iBreakClip = -1;
	m_iBreakShots = m_iBreakNoDamageShots = 0;
	m_bBreakCrouch = m_bBreakRelease = false;
	m_bBreakNear = false;
	m_bBreakKnifePending = false;
	m_flBreakWeaponAttack = 0.0f;
}

bool CAgPB::BeginBreakable( const AgPBObstacle &obstacle, float now, bool crouch )
{
	if ( !obstacle.found || !obstacle.breakableCandidate || !obstacle.visible ||
	     HandleToEdict( obstacle.handle ) == NULL ) return false;
	if ( !obstacle.nearProbe )
	{
		CBotCmd selection;
		selection.Reset();
		// ZBot keeps approaching far props if only a knife is available.
		if ( SelectBreakableWeapon( selection, false ) == NULL && selection.weaponselect == 0 ) return false;
	}
	ClearBreakable();
	m_uBreakHandle = obstacle.handle;
	m_vBreakHit = obstacle.hitPosition;
	m_flBreakDeadline = now + 10.0f;
	m_iBreakBestHealth = obstacle.healthKnown ? obstacle.health : -1;
	m_bBreakCrouch = crouch;
	m_bBreakNear = obstacle.nearProbe;
	ReportObstacle( obstacle );
	RouteReport( "breaking obstacle ent=%d; preserving hop %d and goal #%d",
	             NetVar_HandleEntry( m_uBreakHandle ), m_iRouteIndex, m_iGoalWaypoint );
	return true;
}

void CAgPB::FailBreakable( const char *reason )
{
	RouteReport( "obstacle break failed: %s (ent=%d, shots=%d, goal=#%d) - stopped",
	             reason, NetVar_HandleEntry( m_uBreakHandle ), m_iBreakShots, m_iGoalWaypoint );
	StopRoute();
	m_bRoam = false;
}

// SyPB pauses navigation while destroying a breakable; CS-EBOT is the
// implementation cross-check. No zombie branches or health writes are used.
static int ReadWeaponClip( edict_t *weapon )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( weapon, "m_iClip1" );
	if ( field == NULL || field->type != DPT_Int ) return -1;
	// DT_LocalWeaponData uses SendPropIntWithMinusOneFlag: its public send
	// proxy returns the stored int plus one. Decode it for gameplay decisions;
	// otherwise an empty magazine (0) looks like one remaining round.
	return AgPB_ReadNetVarInt( weapon, "m_iClip1" ) - 1;
}

static bool IsObstacleKnife( edict_t *weapon )
{
	IServerNetworkable *net = weapon != NULL ? weapon->GetNetworkable() : NULL;
	const char *name = net != NULL ? net->GetClassName() : NULL;
	return name != NULL && Q_stricmp( name, "weapon_knife" ) == 0;
}

bool CAgPB::TestWeapon( const char *name, bool only )
{
	if ( !IsValid() || m_pInfo == NULL || m_pInfo->IsDead() || HasRoute() ) return false;
	// Explicit console fixture only: public bot interface creates/equips items.
	// Autonomous selection, reload, firing and melee remain CUserCmd-only.
	if ( only ) m_pController->RemoveAllItems( false );
	m_pController->SetActiveWeapon( name );
	return true;
}

edict_t *CAgPB::SelectBreakableWeapon( CBotCmd &cmd, bool allowKnife ) const
{
	uintp handle;
	edict_t *active = GetNetVarHandleValue( "m_hActiveWeapon", &handle ) ? HandleToEdict( handle ) : NULL;
	const BotNetVar *weapons = FindNetVar( "m_hMyWeapons" );
	void *base = NetVarBase();
	if ( allowKnife && IsObstacleKnife( active ) ) return active;
	// A reflected clip identifies a firearm; knives/grenades have no positive clip.
	if ( active != NULL && AgPB_FindEntityNetVar( active, "m_iClip1" ) != NULL )
	{
		const int clip = ReadWeaponClip( active );
		const int ammo = AgPB_ReadNetVarInt( active, "m_iPrimaryAmmoType", -1 );
		if ( clip > 0 || ( clip == 0 && ammo >= 0 && GetNetVarArrayInt( "m_iAmmo", ammo ) > 0 ) )
			return active;
	}
	if ( base != NULL && weapons != NULL && weapons->type == DPT_Array &&
	     weapons->elementType == DPT_Int && weapons->stride == sizeof( uintp ) )
	{
		for ( int i = 0; i < weapons->elements; ++i )
		{
			edict_t *weapon = HandleToEdict( NetVar_GetArrayHandle( base, *weapons, i ) );
			if ( weapon == NULL ) continue;
			const int clip = ReadWeaponClip( weapon );
			const int ammo = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
			if ( clip > 0 || ( clip == 0 && ammo >= 0 && GetNetVarArrayInt( "m_iAmmo", ammo ) > 0 ) )
			{
				cmd.weaponselect = NetVar_HandleEntry( AgPB_RefEHandle( weapon ) );
				return NULL; // Wait for engine switch before attacking.
			}
		}
		if ( allowKnife )
		{
			for ( int i = 0; i < weapons->elements; ++i )
			{
				edict_t *weapon = HandleToEdict( NetVar_GetArrayHandle( base, *weapons, i ) );
				if ( IsObstacleKnife( weapon ) )
				{
					cmd.weaponselect = NetVar_HandleEntry( AgPB_RefEHandle( weapon ) );
					return NULL;
				}
			}
		}
	}
	return NULL;
}

bool CAgPB::UpdateBreakable( CGlobalVars *globals, CBotCmd &cmd )
{
	if ( !NetVar_HandleIsValid( m_uBreakHandle ) ) return false;
	cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
	cmd.buttons &= ~( IN_ATTACK | IN_JUMP );
	if ( m_bBreakCrouch ) cmd.buttons |= IN_DUCK;
	const float now = globals->curtime;
	edict_t *target = HandleToEdict( m_uBreakHandle );
	AgPBObstacle current;
	AgPB_ClassifyObstacle( m_Ctx, target, current );
	// Account for the last shot before testing for target removal: a single
	// pistol bullet can destroy the radio during the preceding RunPlayerMove.
	uintp observedHandle;
	edict_t *observed = GetNetVarHandleValue( "m_hActiveWeapon", &observedHandle ) ? HandleToEdict( observedHandle ) : NULL;
	if ( observed != NULL && observedHandle == m_uBreakWeapon )
	{
		if ( IsObstacleKnife( observed ) && m_bBreakKnifePending &&
		     AgPB_ReadNetVarFloat( observed, "m_flNextPrimaryAttack" ) > m_flBreakWeaponAttack + 0.01f )
		{
			++m_iBreakShots;
			++m_iBreakNoDamageShots;
			m_bBreakKnifePending = false;
			RouteReport( "obstacle swing confirmed: ent=%d health=%d",
			             NetVar_HandleEntry( m_uBreakHandle ), current.healthKnown ? current.health : -1 );
		}
		const int observedClip = ReadWeaponClip( observed );
		if ( observedClip >= 0 && m_iBreakClip >= 0 && observedClip < m_iBreakClip )
		{
			m_iBreakShots += m_iBreakClip - observedClip;
			m_iBreakNoDamageShots += m_iBreakClip - observedClip;
			RouteReport( "obstacle shot confirmed: ent=%d ammo=%d->%d health=%d",
			             NetVar_HandleEntry( m_uBreakHandle ), m_iBreakClip, observedClip,
			             current.healthKnown ? current.health : -1 );
		}
		m_iBreakClip = observedClip;
	}
	AgPBObstacle path;
	// Check this target's geometry, not the nearest entity: a player crossing
	// in front must pause firing rather than be mistaken for prop displacement.
	InspectObstacle( path, -1, true, m_uBreakHandle );
	if ( target == NULL || current.broken || ( current.healthKnown && current.health <= 0 ) ||
	     !path.found || path.handle != m_uBreakHandle )
	{
		const bool destroyed = target == NULL || current.broken || ( current.healthKnown && current.health <= 0 );
		RouteReport( "obstacle %s ent=%d after %d confirmed attacks; resuming goal #%d at hop %d",
		             destroyed ? "destroyed/removed" : "moved out of path",
	             NetVar_HandleEntry( m_uBreakHandle ), m_iBreakShots, m_iGoalWaypoint, m_iRouteIndex );
		ClearBreakable();
		m_vStuckAnchor = m_pInfo->GetAbsOrigin();
		m_flStuckCheckTime = now + AgPB_STUCK_WINDOW;
		return true; // One tick with attack released before resuming locomotion.
	}
	if ( !current.breakableCandidate )
	{
		FailBreakable( "target no longer a shootable candidate" );
		return true;
	}
	if ( now >= m_flBreakDeadline )
	{
		FailBreakable( "10 second timeout" );
		return true;
	}
	// Acquisition determines near/far weapon policy. The expanded active-target
	// geometry recheck must not relabel a far prop as near and cancel switching.
	edict_t *weapon = SelectBreakableWeapon( cmd, m_bBreakNear );
	if ( weapon == NULL )
	{
		if ( cmd.weaponselect == 0 ) FailBreakable( "no usable firearm or nearby knife" );
		return true;
	}
	const uintp weaponHandle = AgPB_RefEHandle( weapon );
	bool melee = IsObstacleKnife( weapon );
	if ( m_uBreakWeapon != weaponHandle )
	{
		m_bBreakKnifePending = false;
		RouteReport( "obstacle weapon: %s (%s)", AgPB_EntityClassName( weapon ), melee ? "melee" : "firearm" );
	}
	const int clip = ReadWeaponClip( weapon );
	m_uBreakWeapon = weaponHandle;
	m_iBreakClip = clip;
	if ( current.healthKnown && ( m_iBreakBestHealth < 0 || current.health < m_iBreakBestHealth ) )
	{
		m_iBreakBestHealth = current.health;
		m_iBreakNoDamageShots = 0;
	}
	if ( m_iBreakNoDamageShots >= 6 && now >= m_flBreakNextAttack )
	{
		FailBreakable( "six attacks without confirmed damage/destruction" );
		return true;
	}
	Vector offset = GetNetVarVector( "m_vecViewOffset" );
	if ( offset.z <= 0.0f && FindNetVar( "m_vecViewOffset[2]" ) != NULL )
		offset.Init( GetNetVarFloat( "m_vecViewOffset[0]" ), GetNetVarFloat( "m_vecViewOffset[1]" ),
		             GetNetVarFloat( "m_vecViewOffset[2]" ) );
	if ( offset.z <= 0.0f ) offset.z = ( m_bBreakCrouch ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() ) - 4.0f;
	const Vector eye = m_pInfo->GetAbsOrigin() + offset;
	Vector aim = current.center;
	if ( melee && !AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, aim ) &&
	     !AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, m_vBreakHit ) )
	{
		// A near path hit may still be outside knife reach vertically. Prefer a
		// firearm if owned; never slash through a player to reach the prop.
		weapon = SelectBreakableWeapon( cmd, false );
		if ( weapon == NULL ) return true;
		melee = false;
	}
	if ( melee && !AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, aim ) ) aim = m_vBreakHit;
	if ( !melee && !AgPB_ObstacleAimClear( m_Ctx, m_pEdict, target, eye, aim ) )
	{
		aim = m_vBreakHit;
		if ( !AgPB_ObstacleAimClear( m_Ctx, m_pEdict, target, eye, aim ) ) return true;
	}
	QAngle angles;
	VectorAngles( aim - eye, angles );
	angles.x = AngleNormalize( angles.x );
	angles.x = clamp( angles.x, -89.0f, 89.0f );
	angles.y = AngleNormalize( angles.y );
	angles.z = 0.0f;
	// Source CS firearms shoot along EyeAngles + 2 * PunchAngle
	// (weapon_csbasegun.cpp). Compensate using the reflected current punch.
	const Vector punch = melee ? Vector( 0, 0, 0 ) : GetNetVarVector( "m_vecPunchAngle" );
	angles.x = clamp( AngleNormalize( angles.x - 2.0f * punch.x ), -89.0f, 89.0f );
	angles.y = AngleNormalize( angles.y - 2.0f * punch.y );
	cmd.viewangles = angles;
	if ( m_bBreakRelease )
	{
		m_bBreakRelease = false;
		return true;
	}
	if ( !melee && clip == 0 )
	{
		cmd.buttons |= IN_RELOAD;
		return true;
	}
	// ZBot waits until facing its look-at spot and checks for friends in fire.
	// Our strict MASK_SHOT trace includes all players, not only friends.
	if ( now < m_flBreakNextAttack ||
	     now < GetNetVarFloat( "m_flNextAttack" ) ||
	     now < AgPB_ReadNetVarFloat( weapon, "m_flNextPrimaryAttack" ) ||
	     fabsf( AngleDiff( GetNetVarFloat( "m_angEyeAngles[0]" ), angles.x ) ) > 1.0f ||
	     fabsf( AngleDiff( GetNetVarFloat( "m_angEyeAngles[1]" ), angles.y ) ) > 1.0f ) return true;
	cmd.buttons |= IN_ATTACK;
	if ( melee )
	{
		m_flBreakWeaponAttack = AgPB_ReadNetVarFloat( weapon, "m_flNextPrimaryAttack" );
		m_bBreakKnifePending = true;
	}
	m_bBreakRelease = true;
	m_flBreakNextAttack = now + 0.2f;
	return true;
}

bool CAgPB::InspectObstacle( AgPBObstacle &result, int iWaypoint, bool bStuck, uintp onlyHandle ) const
{
	result = AgPBObstacle();
	if ( !IsValid() || m_pInfo == NULL || m_pInfo->IsDead() )
		return false;

	Vector points[32];
	int ids[32];
	int count = 0;
	CAgPBWaypoints &wp = BotWaypoints();
	if ( iWaypoint >= 0 )
	{
		const AgPBPath *node = wp.Get( iWaypoint );
		if ( node == NULL )
			return false;
		points[count] = node->origin;
		ids[count++] = iWaypoint;
	}
	else
	{
		for ( int i = m_iRouteIndex; i < m_vecRoute.Count() && count < 32; ++i )
		{
			const AgPBPath *node = wp.Get( m_vecRoute[i] );
			if ( node == NULL )
				break;
			points[count] = node->origin;
			ids[count++] = m_vecRoute[i];
			// As in ZBot, do not inspect the other end of a ladder from below it.
			if ( ( node->flags & AgPB_WP_LADDER ) != 0 )
				break;
		}
	}
	if ( count == 0 )
		return false;

	const Vector origin = m_pInfo->GetAbsOrigin();
	Vector viewOffset = GetNetVarVector( "m_vecViewOffset" );
	// SENDINFO_VECTORELEM exposes three floats, not a whole-vector property.
	if ( viewOffset.z <= 0.0f && FindNetVar( "m_vecViewOffset[2]" ) != NULL )
		viewOffset.Init( GetNetVarFloat( "m_vecViewOffset[0]" ), GetNetVarFloat( "m_vecViewOffset[1]" ),
		                 GetNetVarFloat( "m_vecViewOffset[2]" ) );
	const bool crouched = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
	if ( viewOffset.z <= 0.0f )
		viewOffset.z = ( crouched ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() ) - 4.0f;
	return AgPB_FindPathObstacle( m_Ctx, m_pEdict, origin, origin + viewOffset,
	                             points, ids, count, crouched, bStuck, result, onlyHandle );
}

void CAgPB::ReportObstacle( const AgPBObstacle &result ) const
{
	if ( !result.found )
	{
		RouteReport( "obstacle: none in the path probe (near 20 / far 300)" );
		return;
	}
	const IServerNetworkable *net = result.entity != NULL ? result.entity->GetNetworkable() : NULL;
	const char *classname = net != NULL ? net->GetClassName() : "world-or-static";
	char health[32], damage[32];
	Q_strncpy( health, "unknown", sizeof( health ) );
	Q_strncpy( damage, "unknown", sizeof( damage ) );
	if ( result.healthKnown )
		Q_snprintf( health, sizeof( health ), "%d", result.health );
	if ( result.damageKnown )
		Q_snprintf( damage, sizeof( damage ), "%d", result.damage );
	RouteReport( "obstacle: kind=%s ent=%d class=%s serverclass=%s group=%d wp=%d path-dist=%.1f probe=%s%s hit=(%.1f %.1f %.1f) center=(%.1f %.1f %.1f) visible=%s breakable-candidate=%s health=%s damage=%s",
	             result.kind, result.entity != NULL ? NetVar_HandleEntry( result.handle ) : -1,
	             classname != NULL ? classname : "unknown",
	             result.entity != NULL ? AgPB_EntityClassName( result.entity ) : "none",
	             result.collisionGroup, result.waypoint, result.pathDistance,
	             result.nearProbe ? "near" : "far", result.expanded ? "/expanded" : "",
	             result.hitPosition.x, result.hitPosition.y, result.hitPosition.z,
	             result.center.x, result.center.y, result.center.z,
	             result.visible ? "yes" : "no", result.breakableCandidate ? "yes" : "no",
	             health, damage );
}

bool CAgPB::StartRoute( int iGoalWaypoint, char *error, size_t maxlen )
{
	CAgPBWaypoints &wp = BotWaypoints();

	StopRoute();

	// 开新路线 = 放弃蹲守（比如玩家手动 agpb_bot_goto）
	m_flCampUntil = 0.0f;
	m_bCampCrouch = false;

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

	// Nearest does not mean reached: after interruption the goal may be the
	// nearest node while still outside its arrival radius. Walk the final
	// approach and let UpdateRoute apply the normal arrival test.
	m_vecRoute = vecPath;
	m_iRouteIndex = vecPath.Count() > 1 ? 1 : 0;
	m_iGoalWaypoint = iGoalWaypoint;
	m_vStuckAnchor = vPos;
	m_flStuckCheckTime = ( gpGlobals != NULL ) ? ( gpGlobals->curtime + AgPB_STUCK_WINDOW ) : 0.0f;

	return true;
}

// ---------------------------------------------------------------------------
// 随机漫游（agpb_bot_roam）：EBot / SyPB 的"没目标就随便走"最小版
// ---------------------------------------------------------------------------

void CAgPB::SetRoam( bool bEnable )
{
	m_bRoam = bEnable;
	m_flNextRoamTime = 0.0f;

	// 关掉漫游时顺手把当前路线停掉，不然还剩半条路要走
	if ( !bEnable )
		StopRoute();
}

/**
 * 随机挑一个可达路点走过去。
 *
 * 只挑 passable 的点（AVOID / 阵营不符等会被 IsWaypointPassable 挡掉），
 * 优先挑直线距离 >= AgPB_ROAM_MIN_DIST 的；A* 走不通就换一个。
 * 两轮都挑不到返回 false，Think 里会 1 秒后再试。
 */
bool CAgPB::StartRandomRoute()
{
	CAgPBWaypoints &wp = BotWaypoints();
	const int nCount = wp.Count();

	if ( nCount < 2 || !IsValid() || m_pInfo == NULL )
		return false;

	const Vector vPos = m_pInfo->GetAbsOrigin();
	const int iStart = wp.FindNearest( vPos, AgPB_ROUTE_PICK );

	if ( !wp.IsValid( iStart ) )
		return false;

	char szError[192];

	for ( int iPass = 0; iPass < 2; ++iPass )
	{
		const bool bPreferFar = ( iPass == 0 );

		for ( int iTry = 0; iTry < AgPB_ROAM_TRIES; ++iTry )
		{
			const int iGoal = RandomInt( 0, nCount - 1 );

			if ( iGoal == iStart || iGoal == m_iLastRoamGoal )
				continue;

			if ( !wp.IsPassable( iGoal, m_iTeam ) )
				continue;

			const AgPBPath *pGoal = wp.Get( iGoal );

			if ( pGoal == NULL )
				continue;

			if ( bPreferFar )
			{
				if ( ( pGoal->origin - vPos ).Length2D() < AgPB_ROAM_MIN_DIST )
					continue;

				if ( IsRoamGoalCrowded( pGoal->origin, AgPB_ROAM_SPREAD ) )
					continue;
			}

			if ( StartRoute( iGoal, szError, sizeof( szError ) ) )
			{
				m_iLastRoamGoal = iGoal;
				RouteReport( "roam: -> #%d (%d hop(s))", iGoal, RouteCount() - 1 );
				return true;
			}
		}
	}

	return false;
}

bool CAgPB::IsRoamGoalCrowded( const Vector &vGoal, float flRadius ) const
{
	CAgPBManager &bots = AgPB_Bots();

	for ( int i = 0; i < bots.Count(); ++i )
	{
		CAgPB *pOther = bots.Get( i );

		if ( pOther == NULL || pOther == this )
			continue;

		IPlayerInfo *pOtherInfo = pOther->PlayerInfo();

		if ( pOtherInfo == NULL || pOtherInfo->IsDead() )
			continue;

		// 已经有 bot 站在目标点附近
		if ( ( pOtherInfo->GetAbsOrigin() - vGoal ).Length2D() < flRadius )
			return true;

		// 或者已经有 bot 正朝那个点走
		if ( pOther->HasRoute() && pOther->RouteGoal() >= 0 )
		{
			const AgPBPath *pOtherGoal = BotWaypoints().Get( pOther->RouteGoal() );

			if ( pOtherGoal != NULL && ( pOtherGoal->origin - vGoal ).Length2D() < flRadius )
				return true;
		}
	}

	return false;
}

void CAgPB::UpdateRoute( CGlobalVars *pGlobals, CBotCmd &cmd )
{
	if ( !HasRoute() || m_pInfo == NULL )
		return;
	// A round's freeze time is not failed movement. Keep the requested route
	// and release attack until movement is enabled again.
	if ( AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 )
	{
		ClearBreakable();
		m_vStuckAnchor = m_pInfo->GetAbsOrigin();
		m_flStuckCheckTime = pGlobals->curtime + AgPB_STUCK_WINDOW;
		return;
	}
	if ( UpdateBreakable( pGlobals, cmd ) ) return;

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
	const float flDist3D = vDelta.Length();

	// 大高差（比如点在自己头顶）：到达判定必须带上 Z，
	// 否则会被 2D 距离判成"已到"，直接跳过这一步（实测踩过）。
	const bool bVerticalLeg = fabsf( vDelta.z ) > 32.0f;

	// 到达判定照 EBot（navigate.cpp:643-690）：
	//   radius >= 50 且不是跳跃边 → 进半径就算到（给"大范围的点"用）
	//   否则（小半径 / 蹲点 / 跳点）→ 要走到 max(radius, 4) 以内才算到，
	//   而且移动中要用速度外推一次，免得高速从点旁边擦过去被判成"没到"然后绕圈
	const bool bJumpLeg = ( uLegFlags & AgPB_PATH_JUMP ) != 0;
	const float flRadius = (float)pNode->radius;

	bool bArrived = false;

	if ( bVerticalLeg )
	{
		// 梯子 / 垂直段：只看 3D 距离（梯子上速度低，不做速度外推）
		const float flCheck = ( flRadius > 4.0f ) ? flRadius : 4.0f;
		bArrived = ( flDist3D < flCheck );
	}
	else if ( flRadius >= 50.0f && !bJumpLeg )
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
			const int iGoal = m_iGoalWaypoint;
			const AgPBPath *pGoal = wp.Get( iGoal );

			RouteReport( "arrived at waypoint #%d (%d hop(s) walked)", iGoal, m_vecRoute.Count() - 1 );

			// CAMP 点：原地蹲守一段随机时间（EBot/SyPB 到 camp 点会 PushTask(TASK_CAMP)）
			if ( pGoal != NULL && ( pGoal->flags & AgPB_WP_CAMP ) != 0 )
			{
				float flMin = agpb_camp_min.GetFloat();
				float flMax = agpb_camp_max.GetFloat();

				if ( flMin < 1.0f )
					flMin = 1.0f;
				if ( flMax < flMin )
					flMax = flMin;

				m_flCampUntil = ( ( pGlobals != NULL ) ? pGlobals->curtime : 0.0f ) + RandomFloat( flMin, flMax );
				m_bCampCrouch = ( ( pGoal->flags & AgPB_WP_CROUCH ) != 0 );

				const float flCampFor = m_flCampUntil - ( ( pGlobals != NULL ) ? pGlobals->curtime : 0.0f );

				RouteReport( "camping at #%d for %.0f s%s", iGoal, flCampFor,
				             m_bCampCrouch ? " (crouched)" : "" );
			}

			StopRoute();
		}

		return;
	}

	// 朝下一个点转（视角会跟随 CUserCmd，已实测，见 PROGRESS.md 问题 6）。
	// 只用水平方向算 yaw：爬梯子时目标在正上方，水平分量≈0，
	// 拿三维向量算出来的 yaw 会乱跳（机器人转头撞墙）；这时保持当前朝向。
	Vector vFlat( vDelta.x, vDelta.y, 0.0f );

	if ( vFlat.LengthSqr() > 1.0f )
	{
		QAngle angTo;
		VectorAngles( vFlat, angTo );
		cmd.viewangles.x = 0.0f;
		cmd.viewangles.y = angTo.y;
	}
	else
	{
		cmd.viewangles.y = GetNetVarFloat( "m_angEyeAngles[1]", 0.0f );
	}

	// 前进（USP 跑速上限 250）
	cmd.forwardmove = AgPB_WALK_SPEED;

	// ---- 队友让行（最简版）----
	// 最小导航没有避让：正前方站着队友时，序号大的那个错身让路，
	// 避免一群 bot 互相堵在门口。EBot 的窄道/互锁让路
	// （navigate.cpp:2833-2950：优先级 + 碰撞探针 + 僵持暂停）是完整版，这里是缩小版。
	{
		CAgPBManager &bots = AgPB_Bots();
		CAgPB *pBlocking = NULL;

		for ( int i = 0; i < bots.Count(); ++i )
		{
			CAgPB *pOther = bots.Get( i );

			if ( pOther == NULL || pOther == this )
				continue;

			IPlayerInfo *pOtherInfo = pOther->PlayerInfo();

			if ( pOtherInfo == NULL || pOtherInfo->IsDead() )
				continue;

			const Vector vTo = pOtherInfo->GetAbsOrigin() - vPos;

			if ( vTo.Length2D() > AgPB_FRIEND_BLOCK_DIST || fabsf( vTo.z ) > AgPB_FRIEND_BLOCK_DIST )
				continue;

			QAngle angFace( 0.0f, cmd.viewangles.y, 0.0f );
			Vector vFwd;
			AngleVectors( angFace, &vFwd );
			vFwd.z = 0.0f;
			VectorNormalize( vFwd );

			Vector vDir( vTo.x, vTo.y, 0.0f );
			VectorNormalize( vDir );

			if ( DotProduct( vFwd, vDir ) < 0.5f )     // 只看正前方 ±60°
				continue;

			pBlocking = pOther;
			break;
		}

		// 对方 edict 序号更小 = 来得更早 → 让他：自己错身 + 减速
		if ( pBlocking != NULL && pBlocking->Index() < m_iIndex )
		{
			cmd.sidemove = AgPB_WALK_SPEED * 0.6f * ( ( m_iIndex & 1 ) ? 1.0f : -1.0f );
			cmd.forwardmove *= 0.25f;
		}
	}

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

	// Acquire a visible breakable on ordinary grounded legs. Do not interrupt
	// the engine's crouch-jump sequence; stuck recovery below may acquire on landing.
	if ( !bJumpLeg && ( pNode->flags & AgPB_WP_LADDER ) == 0 &&
	     ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) != 0 )
	{
		AgPBObstacle obstacle;
		InspectObstacle( obstacle );
		if ( BeginBreakable( obstacle, pGlobals->curtime, bCrouch ) )
		{
			UpdateBreakable( pGlobals, cmd );
			return;
		}
	}

	// Stuck recovery uses the wider near probe to catch small pushaway props.
	// Optional lookahead diagnostics are throttled and log only identity changes.
	if ( agpb_obstacle_debug.GetBool() && pGlobals != NULL && pGlobals->curtime >= m_flNextObstacleCheck )
	{
		AgPBObstacle obstacle;
		InspectObstacle( obstacle );
		if ( obstacle.found != m_bLastObstacleFound || obstacle.handle != m_uLastObstacleHandle )
			ReportObstacle( obstacle );
		m_bLastObstacleFound = obstacle.found;
		m_uLastObstacleHandle = obstacle.handle;
		m_flNextObstacleCheck = pGlobals->curtime + 0.25f;
	}

	if ( pGlobals != NULL && pGlobals->curtime >= m_flStuckCheckTime )
	{
		if ( ( vPos - m_vStuckAnchor ).Length() < AgPB_STUCK_MOVE )
		{
			AgPBObstacle obstacle;
			InspectObstacle( obstacle, -1, true );
			if ( ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) != 0 &&
			     ( pNode->flags & AgPB_WP_LADDER ) == 0 &&
			     BeginBreakable( obstacle, pGlobals->curtime, bCrouch ) )
			{
				UpdateBreakable( pGlobals, cmd );
				return;
			}
			ReportObstacle( obstacle );
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
	if ( pServerFactory != NULL )
	{
		m_Ctx.pServerTools = (IServerTools *)pServerFactory( VSERVERTOOLS_INTERFACE_VERSION, NULL );
		if ( m_Ctx.pServerTools == NULL )
			m_Ctx.pServerTools = (IServerTools *)pServerFactory( VSERVERTOOLS_INTERFACE_VERSION_2, NULL );
		if ( m_Ctx.pServerTools == NULL )
			m_Ctx.pServerTools = (IServerTools *)pServerFactory( VSERVERTOOLS_INTERFACE_VERSION_1, NULL );
		META_CONPRINTF( "[AgPB] read-only server tools: %s\n", m_Ctx.pServerTools != NULL ? "ok" : "MISSING" );
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
	return AgPB_EntityBase( m_pEdict );
}

const CNetVarTable *CAgPB::NetVarTable() const
{
	return AgPB_EntityNetVarTable( m_pEdict );
}

const BotNetVar *CAgPB::FindNetVar( const char *name ) const
{
	return AgPB_FindEntityNetVar( m_pEdict, name );
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

	if ( (uintp)pHandleEntity->GetRefEHandle().ToInt() != handle )
		return NULL;

	return pEdict;
}

bool CAgPB::SetNetVarFloat( const char *pszName, float flValue )
{
	// 走任意实体那套 API：bot 只是"实体"的一个特例（见 bot.h 的说明）
	const NetVarWriteResult result = AgPB_WriteNetVarFloat( m_pEdict, pszName, flValue );

	if ( result != NETVAR_WRITE_OK )
	{
		META_CONPRINTF( "[AgPB] %s: write %s refused (%s)\n",
		                m_Name, pszName, NetVarWriteResultName( result ) );
		return false;
	}

	return true;
}

bool CAgPB::SetNetVarInt( const char *pszName, int iValue )
{
	const NetVarWriteResult result = AgPB_WriteNetVarInt( m_pEdict, pszName, iValue );

	if ( result != NETVAR_WRITE_OK )
	{
		META_CONPRINTF( "[AgPB] %s: write %s refused (%s)\n",
		                m_Name, pszName, NetVarWriteResultName( result ) );
		return false;
	}

	return true;
}

bool CAgPB::SetNetVarVector( const char *pszName, const Vector &vValue )
{
	if ( pszName == NULL )
		return false;

	// 优先按 VECTORELEM 的三个元素写（m_vecVelocity 在表里就是 [0]/[1]/[2]）
	static const char *s_szIndex[3] = { "[0]", "[1]", "[2]" };

	char szName[96];

	const float *pValues = &vValue.x;
	bool bAllFound = true;

	for ( int i = 0; i < 3; ++i )
	{
		Q_snprintf( szName, sizeof( szName ), "%s%s", pszName, s_szIndex[i] );

		if ( AgPB_WriteNetVarFloat( m_pEdict, szName, pValues[i] ) != NETVAR_WRITE_OK )
		{
			bAllFound = false;
			break;
		}
	}

	if ( bAllFound )
		return true;

	// 退路：整条 Vector 字段（DPT_Vector）—— 内存里就是 3 个 float
	return ( AgPB_WriteNetVarVector( m_pEdict, pszName, vValue ) == NETVAR_WRITE_OK );
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

// ---------------------------------------------------------------------------
// netvar 访问：任意实体（见 bot.h 的说明）
// ---------------------------------------------------------------------------

void CAgPBManager::ReportNamedObstacle( const char *name ) const
{
	if ( m_Ctx.pServerTools == NULL || m_Ctx.pGameEnts == NULL ) return;
	CBaseEntity *base = m_Ctx.pServerTools->FindEntityByName( NULL, name );
	edict_t *entity = base != NULL ? m_Ctx.pGameEnts->BaseEntityToEdict( base ) : NULL;
	if ( entity == NULL || entity->IsFree() )
	{
		META_CONPRINTF( "[AgPB] named obstacle not found: %s\n", name );
		return;
	}
	AgPBObstacle result;
	AgPB_ClassifyObstacle( m_Ctx, entity, result );
	char origin[128];
	origin[0] = '\0';
	m_Ctx.pServerTools->GetKeyValue( base, "origin", origin, sizeof( origin ) );
	META_CONPRINTF( "[AgPB] named obstacle: %s ent=%d model-index=%d origin=%s center=(%.1f %.1f %.1f) group=%d candidate=%s health=%d\n",
	                name, NetVar_HandleEntry( result.handle ), AgPB_ReadNetVarInt( entity, "m_nModelIndex", -1 ), origin,
	                result.center.x, result.center.y, result.center.z, result.collisionGroup,
	                result.breakableCandidate ? "yes" : "no", result.healthKnown ? result.health : -1 );
}

void *AgPB_EntityBase( edict_t *pEdict )
{
	if ( pEdict == NULL || pEdict->IsFree() )
		return NULL;

	IServerUnknown *pUnk = pEdict->GetUnknown();
	if ( pUnk == NULL )
		return NULL;

	// CBaseEntity*，公开头文件里只有前置声明，这里当不透明指针用。
	return (void *)pUnk->GetBaseEntity();
}

const CNetVarTable *AgPB_EntityNetVarTable( edict_t *pEdict )
{
	return BotNetVarRegistry().GetForEdict( pEdict );
}

const BotNetVar *AgPB_FindEntityNetVar( edict_t *pEdict, const char *pszName )
{
	if ( pszName == NULL )
		return NULL;

	const CNetVarTable *pTable = AgPB_EntityNetVarTable( pEdict );
	if ( pTable == NULL )
		return NULL;

	return pTable->Find( pszName );
}

NetVarWriteResult AgPB_WriteNetVarInt( edict_t *pEdict, const char *pszName, int iValue, int iElement )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( pVar == NULL )
		return NETVAR_WRITE_NOT_FOUND;

	const NetVarWriteResult result = NetVar_WriteInt( pBase, *pVar, iValue, iElement );

	if ( result == NETVAR_WRITE_OK )
		NetVar_NotifyChanged( pEdict, pVar->offset );

	return result;
}

NetVarWriteResult AgPB_WriteNetVarFloat( edict_t *pEdict, const char *pszName, float flValue, int iElement )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( pVar == NULL )
		return NETVAR_WRITE_NOT_FOUND;

	const NetVarWriteResult result = NetVar_WriteFloat( pBase, *pVar, flValue, iElement );

	if ( result == NETVAR_WRITE_OK )
		NetVar_NotifyChanged( pEdict, pVar->offset );

	return result;
}

NetVarWriteResult AgPB_WriteNetVarVector( edict_t *pEdict, const char *pszName, const Vector &vValue )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( pVar == NULL )
		return NETVAR_WRITE_NOT_FOUND;

	const NetVarWriteResult result = NetVar_WriteVector( pBase, *pVar, vValue );

	if ( result == NETVAR_WRITE_OK )
		NetVar_NotifyChanged( pEdict, pVar->offset );

	return result;
}

int AgPB_ReadNetVarInt( edict_t *pEdict, const char *pszName, int iDefault )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL || pVar == NULL )
		return iDefault;

	return NetVar_GetInt( pBase, *pVar );
}

float AgPB_ReadNetVarFloat( edict_t *pEdict, const char *pszName, float flDefault )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL || pVar == NULL )
		return flDefault;

	return NetVar_GetFloat( pBase, *pVar );
}

Vector AgPB_ReadNetVarVector( edict_t *pEdict, const char *pszName )
{
	void *pBase = AgPB_EntityBase( pEdict );
	const BotNetVar *pVar = AgPB_FindEntityNetVar( pEdict, pszName );

	if ( pBase == NULL || pVar == NULL )
		return Vector( 0.0f, 0.0f, 0.0f );

	return NetVar_GetVector( pBase, *pVar );
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
