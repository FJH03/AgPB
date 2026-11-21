// SPDX-License-Identifier: GPL-3.0-only
// SyPB close-player avoidance, executed with Source hull safety.
// See player_avoidance.h for source versions and retained copyright notice.
#include <in_buttons.h>
#include <const.h>
#include <iservernetworkable.h>
#include <ihandleentity.h>
#include <engine/ICollideable.h>
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
	m_iPlayerAvoidanceWishTick = -1;
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

void CAgPB::UpdatePlayerAvoidance( CGlobalVars *globals, CBotCmd &cmd, const Vector &blockedWish )
{
	using namespace AgPBPlayerAvoid;
	const AgPBTask &task = m_Tasks.Current();
	const int flags = GetNetVarInt( "m_fFlags" );
	const float lastSeen = m_Search.visual.observedAt;
	const bool interacting = ( cmd.buttons & IN_USE ) != 0 ||
		( task.id == AgPBTaskId::PlantBomb && ( cmd.buttons & IN_ATTACK ) != 0 ) ||
		( task.id == AgPBTaskId::DefuseBomb && GetNetVarInt( "m_bIsDefusing" ) != 0 );
	const bool hasGroundCommand = cmd.forwardmove != 0.0f || cmd.sidemove != 0.0f;
	unsigned int leg = 0, nextLeg = 0;
	if ( m_iRouteIndex > 0 ) BotWaypoints().IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
	BotWaypoints().IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &nextLeg );
	const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
	if ( globals == NULL || m_pInfo == NULL || m_Ctx.pTrace == NULL || !IsAlive() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
		AgPB_IsRoundFrozen() || ( flags & ( FL_FROZEN | FL_ONGROUND ) ) != FL_ONGROUND ||
		GetNetVarInt( "movetype" ) != MOVETYPE_WALK || GetNetVarInt( "m_nWaterLevel" ) >= 2 ||
		( ( cmd.buttons | m_LastSubmittedCmd.buttons ) & IN_JUMP ) != 0 || m_bPendingJumpLaunch ||
		( ( leg | nextLeg ) & AgPB_PATH_JUMP ) != 0 ||
		( node != NULL && ( node->flags & AgPB_WP_LADDER ) != 0 ) ||
		IsHoldingTaskPosition() || interacting || task.id == AgPBTaskId::DestroyBreakable ||
		( task.deadline > 0.0f && globals->curtime >= task.deadline ) ||
		cmd.weaponselect != 0 || cmd.upmove != 0.0f ||
		( !hasGroundCommand && ( !blockedWish.IsValid() || blockedWish.Length2DSqr() <= 0.01f ) ) )
	{
		// Never create movement after a stop, accuracy brake, holding task,
		// weapon wait or terminal settle. A blocked movement wish only seeds
		// a new, fully traced teammate-avoidance candidate.
		ResetPlayerAvoidance();
		return;
	}
	Vector forward, right;
	AngleVectors( QAngle( 0, cmd.viewangles.y, 0 ), &forward, &right, NULL );
	const Vector original = hasGroundCommand ? forward * cmd.forwardmove + right * cmd.sidemove : blockedWish;
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector velocity = GetNetVarVector( "m_vecVelocity" );
	const Vector mins = m_pInfo->GetPlayerMins(), maxs = m_pInfo->GetPlayerMaxs();
	Input input;
	input.now = globals->curtime; input.interval = globals->interval_per_tick;
	input.maxSpeed = GetNetVarFloat( "m_flMaxspeed", 250.0f );
	input.position = Vec2( here.x, here.y ); input.z = here.z; input.velocity = Vec2( velocity.x, velocity.y );
	input.wish = Vec2( original.x, original.y );
	input.hullMins = Vec2( mins.x, mins.y ); input.hullMaxs = Vec2( maxs.x, maxs.y );
	input.bottom = here.z + mins.z; input.top = here.z + maxs.z;
	input.team = m_pInfo->GetTeamIndex(); input.slot = m_iIndex;
	input.recentEnemy = hasGroundCommand && std::isfinite( lastSeen ) && lastSeen >= 0.0f &&
		globals->curtime >= lastSeen && globals->curtime - lastSeen <= 1.5f;
	input.owner = task.token; input.ducking = ( flags & FL_DUCKING ) != 0 || ( cmd.buttons & IN_DUCK ) != 0;
	if ( !Valid( input ) || input.owner == 0 || original.Length2DSqr() <= 0.01f ) { ResetPlayerAvoidance(); return; }
	m_vPlayerAvoidanceWish = original;
	m_iPlayerAvoidanceWishTick = globals->tickcount;
	std::vector<const CAgPB *> peers( globals->maxClients + 1, NULL );
	CAgPBManager &bots = AgPB_Bots();
	for ( int i = 0; i < bots.Count(); ++i )
	{
		const CAgPB *bot = bots.Get( i );
		if ( bot != NULL && bot->Index() > 0 && bot->Index() <= globals->maxClients ) peers[bot->Index()] = bot;
	}
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
		ICollideable *collide = m_Ctx.pTrace->GetCollideable( network->GetEntityHandle() );
		if ( collide == NULL || collide->GetSolid() == SOLID_NONE || ( collide->GetSolidFlags() & FSOLID_NOT_SOLID ) != 0 ) continue;
		Neighbor neighbor;
		neighbor.handle = (std::uint64_t)network->GetEntityHandle()->GetRefEHandle().ToInt();
		neighbor.userid = info->GetUserID(); neighbor.slot = slot; neighbor.team = info->GetTeamIndex();
		const Vector position = info->GetAbsOrigin(), speed = AgPB_ReadNetVarVector( entity, "m_vecVelocity" );
		const Vector otherMins = info->GetPlayerMins(), otherMaxs = info->GetPlayerMaxs();
		neighbor.position = Vec2( position.x, position.y ); neighbor.z = position.z;
		neighbor.velocity = Vec2( speed.x, speed.y );
		neighbor.hullMins = Vec2( otherMins.x, otherMins.y ); neighbor.hullMaxs = Vec2( otherMaxs.x, otherMaxs.y );
		neighbor.bottom = position.z + otherMins.z; neighbor.top = position.z + otherMaxs.z;
		const CAgPB *peer = peers[slot];
		if ( peer != NULL && peer->Edict() == entity && peer->m_iPlayerAvoidanceWishTick >= 0 &&
			peer->m_iPlayerAvoidanceWishTick >= globals->tickcount - 1 && peer->m_iPlayerAvoidanceWishTick <= globals->tickcount )
		{
			neighbor.canYield = true;
			neighbor.wish = Vec2( peer->m_vPlayerAvoidanceWish.x, peer->m_vPlayerAvoidanceWish.y );
		}
		neighbors.push_back( neighbor );
	}
	const Contact contact = FindContact( input, neighbors.data(), neighbors.size(), &m_PlayerAvoidance );
	const Input steering = SteeringInput( input, contact );
	edict_t *blocker = contact.valid ? HandleToEdict( (uintp)contact.handle ) : NULL;
	Probes probes;
	const bool noSide = node != NULL && ( node->flags & AgPB_WP_FALLRISK ) != 0;
	const auto safe = [&]( Vec2 wish ) {
		return CanPlayerAvoidStep( Vector( wish.x, wish.y, 0 ), input.ducking, input.interval, blocker );
	};
	if ( contact.valid && ( !contact.physical || contact.yielding ) )
	{
		if ( !noSide )
		{
			if ( contact.physical )
			{
				probes.rightOnly = safe( SideWish( steering, 1 ) );
				probes.leftOnly = safe( SideWish( steering, -1 ) );
			}
			else if ( !contact.retreat )
			{
				probes.right = safe( CandidateWish( steering, 1 ) );
				probes.left = safe( CandidateWish( steering, -1 ) );
			}
			probes.rightBack = safe( CandidateWish( steering, 1, true ) );
			probes.leftBack = safe( CandidateWish( steering, -1, true ) );
		}
		probes.back = safe( CandidateWish( steering, 0, true ) );
	}
	const Output output = Step( m_PlayerAvoidance, input, contact, probes );
	m_pszPlayerAvoidance = output.reason;
	m_bPlayerAvoidanceActive = output.active; m_bPlayerAvoidanceYielding = output.yielding;
	m_flPlayerAvoidanceAt = input.now;
	if ( !output.active ) return;
	Vector wish( output.wish.x, output.wish.y, 0 );
	// Probe the actual submitted movement with the Source player hull.
	if ( wish.Length2DSqr() > 0.01f && !safe( output.wish ) ) wish.Init();
	const AgPBMovement::LocalWish local = AgPBMovement::WorldToLocal( wish.x, wish.y, cmd.viewangles.y );
	cmd.forwardmove = local.forward; cmd.sidemove = local.side;
	if ( m_uRouteTaskToken == task.token && HasRoute() )
	{
		m_flStuckCheckTime = input.now + 2.0f;
		m_flStuckRecoveryUntil = 0.0f;
		if ( output.requestReplan && !NetVar_HandleIsValid( m_uCombatEnemy ) )
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
			m_vPlayerAvoidanceWish = original;
			m_iPlayerAvoidanceWishTick = globals->tickcount;
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
	RouteReport( "player avoidance: policy=sypb_players_v2 state=%s active=%d yielding=%d blocker=0x%llX userid=%d slot=%d owner=%llu side=%d since=%.3f replans=%u",
		m_pszPlayerAvoidance, m_bPlayerAvoidanceActive ? 1 : 0, m_bPlayerAvoidanceYielding ? 1 : 0,
		(unsigned long long)m_PlayerAvoidance.handle, m_PlayerAvoidance.userid, m_PlayerAvoidance.slot,
		(unsigned long long)m_PlayerAvoidance.owner, m_PlayerAvoidance.side, m_PlayerAvoidance.blockedSince, m_uPlayerAvoidanceReplans );
}
