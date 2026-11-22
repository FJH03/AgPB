// AgPB ZM behavior. GPL-3.0-only.
// Behavioral reference: CS-EBOT 2333562 source/ssm/default.cpp,
// source/navigate.cpp (FindGoalHuman/FindGoalZombie) and source/combat.cpp
// (both Copyright (c) 2003-2009 Yet Another POD-Bot Development Team)
// for FindFriendsAndEnemiens/CheckReachable. Source commands, ZR identity,
// sight memory, route ownership and waypoint scoring are implemented for AgPB.
#include <algorithm>
#include <cmath>
#include <vector>
#include <const.h>
#include <in_buttons.h>
#include <mathlib/mathlib.h>
#include <vstdlib/random.h>

#include "bot.h"
#include "movement_control.h"
#include "waypoint.h"

extern bool AgPB_IsRoundFrozen();

namespace
{
constexpr float SightInterval = 0.20f;
constexpr float SightMemory = 5.0f;
constexpr float SharedSightMemory = 2.5f;
constexpr float ThreatRange = 224.0f;

struct ZMWaypointChoice
{
	int waypoint;
	float score;
	bool camp;
};

bool ZMPreferredWaypoint( const ZMWaypointChoice &left, const ZMWaypointChoice &right )
{
	if ( left.score != right.score ) return left.score < right.score;
	return left.waypoint < right.waypoint;
}

float ThreatDistance( const Vector &position, const Vector &threat )
{
	const Vector delta = position - threat;
	return delta.Length2D() + 0.75f * std::abs( delta.z );
}

float PlayerHullSeparation( const Vector &delta, float distance )
{
	const float largestAxis = MAX( std::abs( delta.x ), std::abs( delta.y ) );
	return largestAxis > 0.0f ? distance * ( 2.0f * AgPB_HULL_RADIUS + 2.0f ) / largestAxis : 0.0f;
}

bool SafeTerminal( const AgPBPath *node )
{
	return node != NULL && node->origin.IsValid() && ( node->flags & ( AgPB_WP_LADDER | AgPB_WP_LIFT ) ) == 0;
}
}

IPlayerInfo *CAgPB::ZMPlayer( edict_t *entity, AgPBZRRole role, int expectedUserId ) const
{
	if ( role == AgPBZRRole::Unknown || entity == NULL || entity->IsFree() || m_Ctx.pPlayerInfoManager == NULL ) return NULL;
	IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( entity );
	if ( info == NULL || !info->IsConnected() || !info->IsPlayer() || info->IsHLTV() || info->IsReplay() ||
	     info->IsObserver() || info->IsDead() || info->GetHealth() <= 0 ||
	     AgPB_ReadNetVarInt( entity, "m_lifeState", LIFE_DEAD ) != LIFE_ALIVE ) return NULL;
	const int userId = info->GetUserID();
	if ( userId <= 0 || ( expectedUserId > 0 && userId != expectedUserId ) ||
	     AgPB_ZRRoleForUserId( userId ) != role ) return NULL;
	return info;
}

void CAgPB::ResetZombieMode()
{
	if ( m_ZM.task != 0 )
	{
		if ( m_uRouteTaskToken == m_ZM.task ) ClearRoute();
		m_Tasks.RemoveToken( m_ZM.task );
	}
	m_ZM = ZMState();
}

bool CAgPB::IsZMHoldingPosition() const
{
	return m_ZM.role == AgPBZRRole::Human && m_ZM.holding && m_ZM.task != 0 &&
		m_Tasks.State( m_ZM.task ) == AgPBTaskState::Active;
}

int CAgPB::FindZMRouteEntry() const
{
	CAgPBWaypoints &waypoints = BotWaypoints();
	const Vector origin = m_pInfo->GetAbsOrigin();
	const int team = m_pInfo->GetTeamIndex();
	const bool alreadyDuck = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
	int best = -1;
	float bestDistance = 512.0f * 512.0f;
	for ( int i = 0; i < waypoints.Count(); ++i )
	{
		const AgPBPath *node = waypoints.Get( i );
		if ( !waypoints.IsPassable( i, team ) || !node->origin.IsValid() ) continue;
		const float distance = ( node->origin - origin ).LengthSqr();
		if ( distance >= bestDistance ||
		     !CanWalkToTaskPosition( node->origin, alreadyDuck || NavigationRequiresCrouch( node->flags ) ) ) continue;
		best = i;
		bestDistance = distance;
	}
	return best;
}

bool CAgPB::StartZMRoute( int goal, float now, bool camp, bool retreat )
{
	if ( goal == m_ZM.goal && HasRoute() && m_uRouteTaskToken == m_ZM.task )
	{
		m_ZM.retreating = retreat;
		return true;
	}
	char error[160];
	if ( !BuildRoute( goal, error, sizeof( error ) ) ) return false;
	m_Tasks.RemoveToken( m_ZM.task );
	AgPBTask task;
	task.id = AgPBTaskId::MoveToPosition;
	task.desire = AgPBTaskPriority::MoveToPosition;
	task.waypoint = goal;
	task.deadline = now + 60.0f;
	task.canResume = true;
	m_ZM.task = m_Tasks.Push( task );
	if ( m_ZM.task == 0 ) { ClearRoute(); return false; }
	m_uRouteTaskToken = m_ZM.task;
	m_ZM.goal = goal;
	m_ZM.campGoal = camp;
	m_ZM.retreating = retreat;
	m_ZM.holding = false;
	RouteReport( "zm %s: waypoint #%d", retreat ? "retreat" : camp ? "camp" : "move", goal );
	return true;
}

bool CAgPB::PlanZMHuman( CGlobalVars *globals, const Vector *threat, bool retreat )
{
	const float now = globals->curtime;
	CAgPBWaypoints &waypoints = BotWaypoints();
	const int entry = FindZMRouteEntry();
	if ( entry < 0 ) return false;
	const int team = m_pInfo->GetTeamIndex();
	const Vector origin = m_pInfo->GetAbsOrigin();
	CUtlVector<float> distances;
	CUtlVector<short> parents;
	waypoints.ComputeDistances( entry, distances, parents, team );
	std::vector<Vector> allies;
	std::vector<int> reservations;
	for ( int slot = 1; slot <= globals->maxClients; ++slot )
	{
		edict_t *entity = m_Ctx.pEngine->PEntityOfEntIndex( slot );
		IPlayerInfo *friendInfo = entity != m_pEdict ? ZMPlayer( entity, AgPBZRRole::Human ) : NULL;
		if ( friendInfo != NULL && friendInfo->GetAbsOrigin().IsValid() ) allies.push_back( friendInfo->GetAbsOrigin() );
	}
	CAgPBManager &bots = AgPB_Bots();
	for ( int i = 0; i < bots.Count(); ++i )
	{
		const CAgPB *bot = bots.Get( i );
		if ( bot == this || bot->m_ZM.role != AgPBZRRole::Human || !bot->m_ZM.campGoal ||
		     bot->m_ZM.task == 0 || bot->m_Tasks.State( bot->m_ZM.task ) == AgPBTaskState::Missing ||
		     ZMPlayer( bot->Edict(), AgPBZRRole::Human ) == NULL ) continue;
		const AgPBPath *node = waypoints.Get( bot->m_ZM.goal );
		if ( SafeTerminal( node ) ) reservations.push_back( bot->m_ZM.goal );
	}
	const float currentDanger = threat != NULL ? ThreatDistance( origin, *threat ) : 0.0f;
	std::vector<ZMWaypointChoice> choices;
	for ( int i = 0; i < waypoints.Count(); ++i )
	{
		const AgPBPath *node = waypoints.Get( i );
		if ( !SafeTerminal( node ) || !waypoints.IsPassable( i, team ) || distances[i] >= AgPB_WP_DIST_INF ||
		     ( i == m_ZM.blockedGoal && now < m_ZM.blockedUntil ) ) continue;
		const float dangerDistance = threat != NULL ? ThreatDistance( node->origin, *threat ) : 0.0f;
		if ( retreat && ( i == m_ZM.goal || dangerDistance < currentDanger + 80.0f ||
		     distances[i] > 1800.0f ) ) continue;
		const bool camp = ( node->flags & AgPB_WP_CAMP ) != 0;
		// Reserve each authored stand position for one bot. Nearby positions
		// can support one another while still accounting for crowding.
		if ( camp && std::find( reservations.begin(), reservations.end(), i ) != reservations.end() ) continue;
		if ( !camp && !retreat && ( i == entry || ( node->origin - origin ).LengthSqr() < 64.0f * 64.0f ) ) continue;
		float support = 0.0f, crowd = 0.0f;
		for ( const Vector &ally : allies )
		{
			const Vector offset = ally - node->origin;
			if ( std::abs( offset.z ) > 72.0f ) continue;
			const float distance = offset.Length2D();
			support += MAX( 0.0f, 1.0f - distance / 400.0f );
			if ( distance < 56.0f ) crowd += 1.0f;
		}
		for ( int reserved : reservations )
			if ( ( waypoints.Get( reserved )->origin - node->origin ).LengthSqr() < 64.0f * 64.0f ) crowd += 1.0f;
		if ( !camp && !retreat && support < 0.35f ) continue;
		float score = distances[i] - MIN( support, 2.0f ) * 125.0f + crowd * 350.0f;
		if ( !camp ) score += retreat ? 80.0f : 2000.0f;
		if ( threat != NULL )
		{
			score += 4.0f * MAX( 0.0f, 500.0f - dangerDistance );
			if ( retreat ) score -= MIN( dangerDistance, 800.0f ) * 0.50f;
			// A nearby danger on the first leg makes an otherwise safe-looking
			// endpoint unattractive while the survivor is withdrawing.
			int first = i;
			while ( parents[first] >= 0 && parents[first] != entry )
				first = parents[first];
			const AgPBPath *initial = waypoints.Get( first );
			if ( initial != NULL && ThreatDistance( initial->origin, *threat ) < currentDanger )
			{
				if ( retreat && ( initial->origin - origin ).Length2DSqr() > 24.0f * 24.0f ) continue;
				score += 600.0f;
			}
		}
		choices.push_back( { i, score, camp } );
	}
	std::sort( choices.begin(), choices.end(), ZMPreferredWaypoint );
	const std::size_t tries = MIN( choices.size(), static_cast<std::size_t>( 6 ) );
	for ( std::size_t i = 0; i < tries; ++i )
		if ( StartZMRoute( choices[i].waypoint, now, choices[i].camp, retreat ) ) return true;
	return false;
}

bool CAgPB::PlanZMZombie( float now, const Vector *target )
{
	CAgPBWaypoints &waypoints = BotWaypoints();
	const int entry = FindZMRouteEntry();
	if ( entry < 0 ) return false;
	const int team = m_pInfo->GetTeamIndex();
	CUtlVector<float> distances;
	CUtlVector<short> parents;
	waypoints.ComputeDistances( entry, distances, parents, team );
	// CS-EBOT FindGoalZombie: prefer T-marked patrol points two times
	// out of three. AgPB filters both pools through the authored graph.
	const bool preferTerrorPoints = target == NULL && RandomInt( 1, 3 ) != 1;
	std::vector<ZMWaypointChoice> choices;
	for ( int i = 0; i < waypoints.Count(); ++i )
	{
		const AgPBPath *node = waypoints.Get( i );
		if ( !SafeTerminal( node ) || !waypoints.IsPassable( i, team ) || distances[i] >= AgPB_WP_DIST_INF ||
		     ( i == m_ZM.blockedGoal && now < m_ZM.blockedUntil ) ) continue;
		float score;
		if ( target != NULL )
		{
			const float miss = ( *target - node->origin ).Length();
			if ( miss > 512.0f ) continue;
			score = miss * 3.0f + distances[i] * 0.08f;
			if ( i == entry && miss > 64.0f ) score += 300.0f;
			if ( i == m_ZM.previousGoal && !HasRoute() ) continue;
		}
		else
		{
			if ( i == entry ) continue;
			const bool terrorPoint = ( node->flags & ( AgPB_WP_TERRORIST | AgPB_WP_CAMP ) ) == AgPB_WP_TERRORIST;
			score = RandomFloat( 0.0f, 1.0f );
			if ( preferTerrorPoints && !terrorPoint ) score += 2.0f;
		}
		choices.push_back( { i, score, false } );
	}
	std::sort( choices.begin(), choices.end(), ZMPreferredWaypoint );
	const std::size_t tries = MIN( choices.size(), static_cast<std::size_t>( 5 ) );
	for ( std::size_t i = 0; i < tries; ++i )
		if ( StartZMRoute( choices[i].waypoint, now, false, false ) )
		{
			m_ZM.pursuing = target != NULL;
			if ( target != NULL ) m_ZM.pursuitPoint = *target;
			else m_ZM.pursuitUserId = 0;
			return true;
		}
	return false;
}

bool CAgPB::ZMGroundMove( const Vector &step, CBotCmd &cmd ) const
{
	if ( GetNetVarInt( "movetype", MOVETYPE_NONE ) != MOVETYPE_WALK ||
	     GetNetVarInt( "m_nWaterLevel" ) >= 2 ) return false;
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector delta = step - here;
	const float length = delta.Length2D();
	if ( !delta.IsValid() || length < 6.0f || length > 240.0f || std::abs( delta.z ) > 32.0f ) return false;
	const bool crouch = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
	if ( !CanWalkToTaskPosition( step, crouch ) ) return false;
	QAngle yaw;
	VectorAngles( Vector( delta.x, delta.y, 0.0f ), yaw );
	cmd.viewangles.Init( 0.0f, yaw.y, 0.0f );
	cmd.forwardmove = clamp( length * 6.0f, 105.0f, 250.0f );
	if ( crouch ) cmd.buttons |= IN_DUCK;
	return true;
}

bool CAgPB::ZMThreatCanReach( edict_t *threat, const Vector &position ) const
{
	IPlayerInfo *source = ZMPlayer( threat, AgPBZRRole::Zombie );
	if ( source == NULL ) return false;
	const Vector origin = m_pInfo->GetAbsOrigin();
	const Vector delta = position - origin;
	const float length = delta.Length2D();
	const bool nearFloor = std::abs( delta.z ) <= 24.0f;
	if ( length > ThreatRange || std::abs( delta.z ) > AgPB_HullStandHeight() ) return false;
	const float separation = MAX( 40.0f, PlayerHullSeparation( delta, length ) );
	if ( nearFloor && length < separation + 6.0f ) return true;
	const bool crouch = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
	if ( nearFloor && length > separation &&
	     CanWalkToTaskPosition( origin + delta * ( 1.0f - separation / length ), crouch ) ) return true;
	CAgPBWaypoints &waypoints = BotWaypoints();
	const int start = waypoints.FindNearest( position, 96.0f );
	const int goal = waypoints.FindNearest( origin, 96.0f );
	const int team = source->GetTeamIndex();
	CUtlVector<int> path;
	if ( !waypoints.IsPassable( start, team ) || !waypoints.IsPassable( goal, team ) ||
	     !waypoints.FindPath( start, goal, path, team ) ) return false;
	float route = ( position - waypoints.Get( start )->origin ).Length() + ( origin - waypoints.Get( goal )->origin ).Length();
	for ( int i = 1; i < path.Count(); ++i )
		route += ( waypoints.Get( path[i] )->origin - waypoints.Get( path[i - 1] )->origin ).Length();
	return route <= ThreatRange + 80.0f;
}

void CAgPB::UpdateZMCombatOverlay( CGlobalVars *globals, CBotCmd &cmd, edict_t *target )
{
	Vector forward, right;
	AngleVectors( QAngle( 0.0f, cmd.viewangles.y, 0.0f ), &forward, &right, NULL );
	const Vector worldWish = forward * cmd.forwardmove + right * cmd.sidemove;
	UpdateZMCombat( globals, cmd, target, m_ZM.role );
	const AgPBMovement::LocalWish move = AgPBMovement::WorldToLocal( worldWish.x, worldWish.y, cmd.viewangles.y );
	cmd.forwardmove = move.forward;
	cmd.sidemove = move.side;
}

void CAgPB::UpdateZombieMode( CGlobalVars *globals, CBotCmd &cmd )
{
	if ( globals == NULL || !IsAlive() || m_Ctx.pEngine == NULL ) return;
	const AgPBZRRole role = AgPB_ZRRoleForUserId( m_pInfo->GetUserID() );
	if ( role != m_ZM.role )
	{
		ResetZombieMode();
		ResetPlayerAvoidance();
		ClearCombat( "zm-role-change" );
		ResetWeaponSelection( "zm-role-change" );
		m_ZM.role = role;
	}
	if ( role == AgPBZRRole::Unknown ) return;
	const float now = globals->curtime;
	const int flags = GetNetVarInt( "m_fFlags" );
	if ( AgPB_IsRoundFrozen() || ( flags & FL_FROZEN ) != 0 )
	{
		SuspendTaskActions( now );
		return;
	}
	const auto releaseGoal = [&]() {
		if ( m_ZM.task != 0 )
		{
			if ( m_uRouteTaskToken == m_ZM.task ) ClearRoute();
			m_Tasks.RemoveToken( m_ZM.task );
		}
		m_ZM.task = 0;
		m_ZM.goal = -1;
		m_ZM.pursuitUserId = 0;
		m_ZM.campGoal = m_ZM.retreating = m_ZM.holding = m_ZM.pursuing = false;
	};
	const auto hold = [&]( int waypoint, bool camp ) {
		AgPBTask task;
		task.id = camp ? AgPBTaskId::Camp : AgPBTaskId::Pause;
		task.desire = camp ? AgPBTaskPriority::Camp : AgPBTaskPriority::Pause;
		task.waypoint = waypoint;
		m_ZM.task = m_Tasks.Push( task );
		m_ZM.goal = waypoint;
		m_ZM.campGoal = camp;
		m_ZM.holding = m_ZM.task != 0;
	};
	CAgPBWaypoints &waypoints = BotWaypoints();
	m_Tasks.Expire( now );
	if ( m_ZM.task != 0 )
	{
		const AgPBPath *goal = waypoints.Get( m_ZM.goal );
		if ( m_Tasks.State( m_ZM.task ) == AgPBTaskState::Missing ||
		     ( m_ZM.goal >= 0 && !waypoints.IsPassable( m_ZM.goal, m_pInfo->GetTeamIndex() ) ) ||
		     ( m_ZM.campGoal && ( goal == NULL || ( goal->flags & AgPB_WP_CAMP ) == 0 ) ) )
			releaseGoal();
	}

	// UpdateRoute can advance to a different authored leg during this command.
	const auto movementOwnsCommand = [&]() {
		if ( !HasRoute() ) return false;
		const AgPBPath *node = waypoints.Get( RouteNode( m_iRouteIndex ) );
		unsigned int leg = 0, nextLeg = 0;
		if ( m_iRouteIndex > 0 ) waypoints.IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
		waypoints.IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &nextLeg );
		return m_Tasks.Current().id == AgPBTaskId::DestroyBreakable || m_Ladder.Active() || m_bPendingJumpLaunch ||
			( flags & FL_ONGROUND ) == 0 || GetNetVarInt( "movetype" ) == MOVETYPE_LADDER ||
			( ( leg | nextLeg ) & AgPB_PATH_JUMP ) != 0 ||
			( node != NULL && ( node->flags & ( AgPB_WP_LADDER | AgPB_WP_LIFT ) ) != 0 );
	};
	const bool committedRoute = movementOwnsCommand();
	const AgPBPath *routeNode = waypoints.Get( RouteNode( m_iRouteIndex ) );
	const bool human = role == AgPBZRRole::Human;
	const AgPBZRRole opponent = human ? AgPBZRRole::Zombie : AgPBZRRole::Human;
	const Vector origin = m_pInfo->GetAbsOrigin();
	if ( !origin.IsValid() ) return;

	edict_t *visible = NULL;
	edict_t *last = HandleToEdict( m_ZM.sight.handle );
	IPlayerInfo *lastInfo = ZMPlayer( last, opponent, m_ZM.sight.userId );
	Vector aimPoint;
	const bool scan = now >= m_ZM.scanAt;
	if ( scan )
	{
		m_ZM.scanAt = now + SightInterval;
		float distance = 2400.0f * 2400.0f;
		for ( int slot = 1; slot <= globals->maxClients; ++slot )
		{
			edict_t *entity = m_Ctx.pEngine->PEntityOfEntIndex( slot );
			if ( entity == m_pEdict ) continue;
			IPlayerInfo *info = ZMPlayer( entity, opponent );
			if ( info == NULL || !info->GetAbsOrigin().IsValid() ||
			     ( AgPB_ReadNetVarInt( entity, "m_fFlags" ) & FL_NOTARGET ) != 0 ) continue;
			const float candidate = ( info->GetAbsOrigin() - origin ).LengthSqr();
			const uintp handle = AgPB_RefEHandle( entity );
			if ( candidate >= distance || !NetVar_HandleIsValid( handle ) || !ZombieModeAimPoint( entity, aimPoint ) ) continue;
			distance = candidate;
			visible = entity;
			m_ZM.sight.handle = handle;
			m_ZM.sight.userId = info->GetUserID();
			m_ZM.sight.origin = info->GetAbsOrigin();
			m_ZM.sight.seenAt = now;
		}
	}
	else if ( lastInfo != NULL && lastInfo->GetAbsOrigin().IsValid() &&
	          ( AgPB_ReadNetVarInt( last, "m_fFlags" ) & FL_NOTARGET ) == 0 && ZombieModeAimPoint( last, aimPoint ) )
	{
		visible = last;
		m_ZM.sight.origin = lastInfo->GetAbsOrigin();
		m_ZM.sight.seenAt = now;
	}
	const bool remember = m_ZM.sight.seenAt >= 0.0f && now - m_ZM.sight.seenAt < SightMemory &&
		( visible != NULL || lastInfo != NULL );
	bool routeDriven = false;
	if ( !committedRoute && ( flags & FL_ONGROUND ) != 0 )
	{
		if ( human )
		{
			const Vector *threat = remember ? &m_ZM.sight.origin : NULL;
			const float threatDistance = threat != NULL ? ThreatDistance( origin, *threat ) : 0.0f;
			if ( visible != NULL && ( now >= m_ZM.threatCheckAt || m_ZM.checkedThreat != m_ZM.sight.handle ||
			     std::abs( threatDistance - m_ZM.checkedThreatDistance ) >= 40.0f ) )
			{
				m_ZM.threatCheckAt = now + SightInterval;
				m_ZM.checkedThreat = m_ZM.sight.handle;
				m_ZM.checkedThreatDistance = threatDistance;
				m_ZM.reachableThreat = ZMThreatCanReach( visible, m_ZM.sight.origin );
			}
			const bool urgent = visible != NULL && m_ZM.reachableThreat;
			const AgPBPath *occupiedCamp = IsZMHoldingPosition() && m_ZM.campGoal ? BotWaypoints().Get( m_ZM.goal ) : NULL;
			if ( occupiedCamp != NULL && ( ( occupiedCamp->origin - origin ).Length2DSqr() > 96.0f * 96.0f ||
			     std::abs( occupiedCamp->origin.z - origin.z ) > 32.0f ) )
			{
				releaseGoal();
				m_ZM.planAt = now;
			}
			const bool unsafeLeg = urgent && routeNode != NULL &&
				ThreatDistance( routeNode->origin, m_ZM.sight.origin ) + 48.0f < threatDistance;
			const bool seekRetreat = urgent && ( !m_ZM.retreating || unsafeLeg );
			const bool seekCamp = !HasRoute() && ( !IsZMHoldingPosition() || !m_ZM.campGoal );
			const AgPBRadio retreatCall = m_pInfo->GetHealth() <= 40 ? AgPBRadio::NeedBackup : AgPBRadio::FallBack;
			bool retreatRoute = m_ZM.retreating && !unsafeLeg;
			if ( now >= m_ZM.planAt && ( seekCamp || seekRetreat ) )
			{
				const bool wasRetreating = m_ZM.retreating;
				m_ZM.planAt = now + ( urgent ? 0.45f : 1.8f );
				retreatRoute = PlanZMHuman( globals, threat, urgent ) && urgent;
				if ( retreatRoute && !wasRetreating ) SendRadio( retreatCall, now );
			}
			if ( urgent && !retreatRoute )
			{
				Vector away = origin - m_ZM.sight.origin;
				away.z = 0.0f;
				if ( VectorNormalize( away ) > 0.1f )
				{
					const Vector side( -away.y, away.x, 0.0f );
					if ( ZMGroundMove( origin + away * 160.0f, cmd ) ||
					     ZMGroundMove( origin + away * 95.0f + side * 95.0f, cmd ) ||
					     ZMGroundMove( origin + away * 95.0f - side * 95.0f, cmd ) )
					{
						releaseGoal();
						if ( occupiedCamp != NULL ) SendRadio( retreatCall, now );
						UpdateZMCombatOverlay( globals, cmd, visible );
						return;
					}
				}
				if ( unsafeLeg ) releaseGoal();
			}
		}
		else
		{
			const auto alreadySearched = [&]( const ZMSighting &sight ) {
				return now < m_ZM.searchedUntil &&
					( m_ZM.searchedPoint - sight.origin ).LengthSqr() <= 128.0f * 128.0f;
			};
			const ZMSighting *target = remember && ( visible != NULL || !alreadySearched( m_ZM.sight ) )
				? &m_ZM.sight : NULL;
			const bool canPlan = now >= m_ZM.planAt;
			if ( target == NULL && canPlan )
			{
				// Share a teammate's observed position and validate the
				// enemy identity without updating a hidden position.
				CAgPBManager &bots = AgPB_Bots();
				float closest = 0.0f;
				for ( int i = 0; i < bots.Count(); ++i )
				{
					const CAgPB *other = bots.Get( i );
					const ZMSighting &sight = other->m_ZM.sight;
					if ( other == this || other->m_ZM.role != role || sight.seenAt < 0.0f ||
					     now - sight.seenAt >= SharedSightMemory || ZMPlayer( other->Edict(), role ) == NULL ||
					     alreadySearched( sight ) ||
					     ZMPlayer( HandleToEdict( sight.handle ), opponent, sight.userId ) == NULL ) continue;
					const float distance = ( sight.origin - origin ).LengthSqr();
					if ( target != NULL && distance >= closest ) continue;
					closest = distance;
					target = &sight;
				}
			}
			if ( visible != NULL )
			{
				const Vector delta = m_ZM.sight.origin - origin;
				const float distance = delta.Length2D();
				const float separation = PlayerHullSeparation( delta, distance );
				const bool contact = distance <= separation + 6.0f && std::abs( delta.z ) < 32.0f;
				if ( contact || ( distance > separation && distance <= 260.0f &&
				     ZMGroundMove( origin + delta * ( 1.0f - separation / distance ), cmd ) ) )
				{
					releaseGoal();
					UpdateZMCombatOverlay( globals, cmd, visible );
					return;
				}
			}
			if ( canPlan )
			{
				const bool retarget = target != NULL && ( !m_ZM.pursuing ||
					target->userId != m_ZM.pursuitUserId ||
					( target->origin - m_ZM.pursuitPoint ).LengthSqr() >= 128.0f * 128.0f );
				m_ZM.planAt = now + ( !HasRoute() && target == NULL ? 1.25f : 0.65f );
				if ( !HasRoute() || retarget )
				{
					if ( target != NULL && PlanZMZombie( now, &target->origin ) )
						m_ZM.pursuitUserId = target->userId;
					else if ( !HasRoute() )
						PlanZMZombie( now, NULL );
				}
			}
		}
	}
	if ( HasRoute() )
	{
		const int arriving = m_ZM.goal;
		const bool camp = m_ZM.campGoal;
		UpdateRoute( globals, cmd );
		routeDriven = true;
		if ( !HasRoute() )
		{
			const bool arrived = m_bRouteArrived;
			if ( arrived && !human && m_ZM.pursuing && visible == NULL )
			{
				m_ZM.searchedPoint = m_ZM.pursuitPoint;
				m_ZM.searchedUntil = now + SightMemory;
			}
			releaseGoal();
			m_ZM.previousGoal = arriving;
			if ( arrived && human )
			{
				hold( arriving, camp );
				if ( camp && IsZMHoldingPosition() && visible == NULL ) SendRadio( AgPBRadio::InPosition, now );
			}
			else if ( !arrived )
			{
				m_ZM.blockedGoal = arriving;
				m_ZM.blockedUntil = now + 4.0f;
			}
		}
	}
	if ( human && !HasRoute() && !IsZMHoldingPosition() ) hold( -1, false );
	if ( IsZMHoldingPosition() )
	{
		const AgPBPath *node = m_ZM.campGoal ? BotWaypoints().Get( m_ZM.goal ) : NULL;
		if ( node != NULL && ( node->flags & AgPB_WP_CROUCH ) != 0 ) cmd.buttons |= IN_DUCK;
		if ( visible == NULL )
		{
			UpdateIdleLook( globals, cmd );
			if ( m_Autonomy.lookWaypoint < 0 && ( flags & FL_ONGROUND ) != 0 )
			{
				if ( now >= m_ZM.sweepAt )
				{
					m_ZM.sweepAt = now + RandomFloat( 2.0f, 4.0f );
					m_ZM.sweepYaw = cmd.viewangles.y + RandomFloat( -100.0f, 100.0f );
				}
				const AgPBMovement::View view = AgPBMovement::ApproachView(
					cmd.viewangles.x, cmd.viewangles.y, 0.0f, m_ZM.sweepYaw, globals->interval_per_tick );
				cmd.viewangles.Init( view.pitch, view.yaw, 0.0f );
			}
		}
	}
	if ( !committedRoute && !movementOwnsCommand() &&
	     ( !routeDriven || ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_JUMP | IN_USE ) ) == 0 ) )
		UpdateZMCombatOverlay( globals, cmd, visible );
	else if ( NetVar_HandleIsValid( m_uCombatEnemy ) ) ClearCombat( "zm-route-priority" );
}
