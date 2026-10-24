// SPDX-License-Identifier: GPL-3.0-only
// SyPB close-player prediction, with CS-EBOT's general priority/yield recovery.
// See player_avoidance.h for source versions and retained copyright notice.
#include <in_buttons.h>
#include <const.h>
#include <iservernetworkable.h>
#include <ihandleentity.h>
#include <vector>
#include "plugin.h"
#include "bot.h"
#include "waypoint.h"
#include "movement_control.h"
#include "game_mode.h"

extern bool AgPB_IsRoundFrozen();

void CAgPB::ResetPlayerAvoidance()
{
	AgPBPlayerAvoid::Reset( m_PlayerAvoidance );
	m_flPlayerAvoidanceAt = -1.0f;
	m_bPlayerAvoidanceActive = m_bPlayerAvoidanceYielding = false;
	m_pszPlayerAvoidance = "idle";
}

bool CAgPB::PlayerAvoidanceActive( float now ) const
{
	if ( !m_bPlayerAvoidanceActive || !std::isfinite( now ) || now < m_flPlayerAvoidanceAt ||
		now - m_flPlayerAvoidanceAt > 0.1f || !HasRoute() ||
		m_PlayerAvoidance.owner != m_uRouteTaskToken || m_uRouteTaskToken != m_Tasks.Current().token ) return false;
	edict_t *other = HandleToEdict( (uintp)m_PlayerAvoidance.handle );
	IPlayerInfo *info = other != NULL ? m_Ctx.pPlayerInfoManager->GetPlayerInfo( other ) : NULL;
	return info != NULL && info->IsPlayer() && !info->IsObserver() && !info->IsDead() &&
		info->GetUserID() == m_PlayerAvoidance.userid && m_pInfo != NULL &&
		info->GetTeamIndex() == m_pInfo->GetTeamIndex() &&
		AgPB_ReadNetVarInt( other, "m_lifeState", -1 ) == LIFE_ALIVE;
}

void CAgPB::UpdatePlayerAvoidance( CGlobalVars *globals, CBotCmd &cmd )
{
	using namespace AgPBPlayerAvoid;
	const AgPBTask &task = m_Tasks.Current();
	const int flags = GetNetVarInt( "m_fFlags" );
	const bool interacting = ( cmd.buttons & IN_USE ) != 0 ||
		( task.id == AgPBTaskId::PlantBomb && ( cmd.buttons & IN_ATTACK ) != 0 ) ||
		( task.id == AgPBTaskId::DefuseBomb && GetNetVarInt( "m_bIsDefusing" ) != 0 );
	unsigned int leg = 0, nextLeg = 0;
	if ( m_iRouteIndex > 0 ) BotWaypoints().IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
	BotWaypoints().IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &nextLeg );
	const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
	if ( globals == NULL || !IsAlive() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
		AgPB_IsRoundFrozen() || ( flags & ( FL_FROZEN | FL_ONGROUND ) ) != FL_ONGROUND ||
		GetNetVarInt( "movetype" ) != MOVETYPE_WALK || GetNetVarInt( "m_nWaterLevel" ) >= 2 ||
		( ( cmd.buttons | m_LastSubmittedCmd.buttons ) & IN_JUMP ) != 0 || m_bPendingJumpLaunch ||
		( ( leg | nextLeg ) & AgPB_PATH_JUMP ) != 0 ||
		( node != NULL && ( node->flags & AgPB_WP_LADDER ) != 0 ) ||
		IsHoldingTaskPosition() || interacting || task.id == AgPBTaskId::DestroyBreakable ||
		( task.deadline > 0.0f && globals->curtime >= task.deadline ) ||
		cmd.weaponselect != 0 || cmd.upmove != 0.0f ||
		( cmd.forwardmove == 0.0f && cmd.sidemove == 0.0f ) )
	{
		// Never create movement after a stop, accuracy brake, holding task,
		// weapon wait or terminal settle. Only an existing ground wish is owned.
		ResetPlayerAvoidance();
		return;
	}
	Vector forward, right;
	AngleVectors( QAngle( 0, cmd.viewangles.y, 0 ), &forward, &right, NULL );
	const Vector original = forward * cmd.forwardmove + right * cmd.sidemove;
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector velocity = GetNetVarVector( "m_vecVelocity" );
	const Vector mins = m_pInfo->GetPlayerMins(), maxs = m_pInfo->GetPlayerMaxs();
	Input input;
	input.now = globals->curtime; input.interval = globals->interval_per_tick;
	input.maxSpeed = GetNetVarFloat( "m_flMaxspeed", 250.0f );
	input.position = Vec2( here.x, here.y ); input.velocity = Vec2( velocity.x, velocity.y );
	input.wish = Vec2( original.x, original.y );
	input.bottom = here.z + mins.z; input.top = here.z + maxs.z;
	input.team = m_pInfo->GetTeamIndex(); input.priority = m_iIndex + 3;
	input.owner = task.token; input.ducking = ( flags & FL_DUCKING ) != 0 || ( cmd.buttons & IN_DUCK ) != 0;
	if ( !Valid( input ) ) { ResetPlayerAvoidance(); return; }
	std::vector<Neighbor> neighbors;
	neighbors.reserve( globals->maxClients );
	for ( int slot = 1; slot <= globals->maxClients; ++slot )
	{
		if ( slot == m_iIndex ) continue;
		edict_t *entity = m_Ctx.pEngine->PEntityOfEntIndex( slot );
		if ( entity == NULL || entity->IsFree() ) continue;
		IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( entity );
		if ( info == NULL || !info->IsPlayer() || info->IsObserver() || info->IsDead() ||
			info->GetTeamIndex() != input.team || AgPB_ReadNetVarInt( entity, "m_lifeState", -1 ) != LIFE_ALIVE ) continue;
		IServerNetworkable *network = entity->GetNetworkable();
		if ( network == NULL || network->GetEntityHandle() == NULL ) continue;
		Neighbor neighbor;
		neighbor.handle = (std::uint64_t)network->GetEntityHandle()->GetRefEHandle().ToInt();
		neighbor.userid = info->GetUserID(); neighbor.slot = slot; neighbor.team = info->GetTeamIndex();
		neighbor.priority = 1; // EBot: humans and externally controlled players precede local bots.
		const Vector position = info->GetAbsOrigin(), speed = AgPB_ReadNetVarVector( entity, "m_vecVelocity" );
		const Vector otherMins = info->GetPlayerMins(), otherMaxs = info->GetPlayerMaxs();
		neighbor.position = Vec2( position.x, position.y ); neighbor.velocity = Vec2( speed.x, speed.y );
		neighbor.bottom = position.z + otherMins.z; neighbor.top = position.z + otherMaxs.z;
		for ( int index = 0; index < AgPB_Bots().Count(); ++index )
		{
			CAgPB *peer = AgPB_Bots().Get( index );
			if ( peer == NULL || peer->Edict() != entity ) continue;
			const AgPBTask &peerTask = peer->m_Tasks.Current();
			const bool protectedPosition = peer->IsHoldingTaskPosition() ||
				peerTask.id == AgPBTaskId::DestroyBreakable ||
				( peer->m_LastSubmittedCmd.buttons & IN_USE ) != 0 ||
				( !peer->HasRoute() && !peer->m_bAutonomous && peerTask.id == AgPBTaskId::Normal ) ||
				( peerTask.id == AgPBTaskId::PlantBomb && ( peer->m_LastSubmittedCmd.buttons & IN_ATTACK ) != 0 ) ||
				( peerTask.id == AgPBTaskId::DefuseBomb && peer->GetNetVarInt( "m_bIsDefusing" ) != 0 );
			neighbor.priority = protectedPosition ? 1 : slot + 3;
			Vector peerForward, peerRight;
			AngleVectors( QAngle( 0, peer->m_LastSubmittedCmd.viewangles.y, 0 ), &peerForward, &peerRight, NULL );
			const Vector wish = peerForward * peer->m_LastSubmittedCmd.forwardmove + peerRight * peer->m_LastSubmittedCmd.sidemove;
			neighbor.intent = Vec2( wish.x, wish.y );
			break;
		}
		neighbors.push_back( neighbor );
	}
	const Contact contact = FindContact( input, neighbors.data(), neighbors.size() );
	edict_t *blocker = contact.valid ? HandleToEdict( (uintp)contact.handle ) : NULL;
	Probes probes;
	const bool noSide = node != NULL && ( node->flags & AgPB_WP_FALLRISK ) != 0;
	const auto safe = [&]( int side, bool retreat ) {
		const Vec2 wish = CandidateWish( input, side, retreat );
		return CanPlayerAvoidStep( Vector( wish.x, wish.y, 0 ), input.ducking, input.interval, blocker );
	};
	if ( contact.valid && contact.yielding )
	{
		if ( !noSide )
		{
			probes.right = safe( 1, false ); probes.left = safe( -1, false );
			probes.rightBack = safe( 1, true ); probes.leftBack = safe( -1, true );
		}
		probes.back = safe( 0, true );
	}
	const Output output = Step( m_PlayerAvoidance, input, contact, probes );
	m_pszPlayerAvoidance = output.reason;
	m_bPlayerAvoidanceActive = output.active; m_bPlayerAvoidanceYielding = output.yielding;
	m_flPlayerAvoidanceAt = input.now;
	if ( !output.active ) return;
	Vector wish( output.wish.x, output.wish.y, 0 );
	// Even a high-priority actor cannot push an unsafe command through players.
	if ( !CanPlayerAvoidStep( wish, input.ducking, input.interval, blocker ) ) wish.Init();
	const AgPBMovement::LocalWish local = AgPBMovement::WorldToLocal( wish.x, wish.y, cmd.viewangles.y );
	cmd.forwardmove = local.forward; cmd.sidemove = local.side;
	if ( m_uRouteTaskToken == task.token && HasRoute() )
	{
		m_flStuckCheckTime = input.now + 2.0f;
		m_flStuckRecoveryUntil = 0.0f;
		if ( output.requestReplan )
		{
			const int goal = m_iGoalWaypoint;
			const std::uint64_t owner = m_uRouteTaskToken;
			const State continued = m_PlayerAvoidance;
			char error[192];
			++m_uPlayerAvoidanceReplans;
			const bool rebuilt = BuildRoute( goal, error, sizeof( error ) );
			// Failed candidates leave the old route intact. Successful candidates
			// retain the exact task/goal and use only existing manual graph edges.
			m_uRouteTaskToken = owner;
			m_PlayerAvoidance = continued;
			m_pszPlayerAvoidance = rebuilt ? "player-repath" : "player-repath-wait";
			m_bPlayerAvoidanceActive = true; m_bPlayerAvoidanceYielding = output.yielding;
			m_flPlayerAvoidanceAt = input.now;
			RouteReport( "player recovery goal=%d owner=%llu replan=%u result=%s",
				goal, (unsigned long long)owner, m_uPlayerAvoidanceReplans, rebuilt ? "ready" : error );
		}
	}
}

void CAgPB::ReportPlayerAvoidance() const
{
	RouteReport( "player avoidance: policy=sypb_ebot_players_v1 state=%s active=%d yielding=%d blocker=0x%llX userid=%d slot=%d owner=%llu side=%d since=%.3f replans=%u",
		m_pszPlayerAvoidance, m_bPlayerAvoidanceActive ? 1 : 0, m_bPlayerAvoidanceYielding ? 1 : 0,
		(unsigned long long)m_PlayerAvoidance.handle, m_PlayerAvoidance.userid, m_PlayerAvoidance.slot,
		(unsigned long long)m_PlayerAvoidance.owner, m_PlayerAvoidance.side, m_PlayerAvoidance.blockedSince, m_uPlayerAvoidanceReplans );
}
