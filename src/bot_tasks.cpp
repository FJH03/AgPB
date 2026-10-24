/*
 * AgPB normal-mode task dispatch and goal selection.
 * Adapted from SyPB source/basecode.cpp task dispatch and navigate.cpp FindGoal.
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 * SyPB's permission notice is retained in tasks.cpp. AgPB: GPL-3.0-only.
 * CS-EBOT's process handling and goal selection were compared; its zombie
 * branches are deliberately not part of normal bomb/hostage/no-objective play.
 */
#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <gametrace.h>
#include <in_buttons.h>
#include <iservernetworkable.h>
#include <vstdlib/random.h>
#include "plugin.h"
#include "bot.h"
#include "objectives.h"
#include "game_mode.h"
#include "waypoint.h"
#include "bomb_escape_policy.h"
#include "holding_policy.h"

extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();
extern ConVar agpb_camp_min;
extern ConVar agpb_camp_max;

static Vector TaskPosition( const AgPBTask &task )
{
	return Vector( task.position[0], task.position[1], task.position[2] );
}

static void SetTaskPosition( AgPBTask &task, const Vector &position )
{
	task.hasPosition = true;
	for ( int i = 0; i < 3; ++i ) task.position[i] = position[i];
}

bool CAgPB::IsHoldingTaskPosition() const
{
	const AgPBTask &task = m_Tasks.Current();
	if ( gpGlobals == NULL || !AgPBHolding::Active( task, m_Tasks.State( task.token ), gpGlobals->curtime,
		IsAlive(), AgPB_GameMode() == AgPB_MODE_NORMAL,
		AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 ) ||
		!AgPBHolding::AgentValid( task, m_AgentIntent.token, m_AgentIntent.waypoint,
			m_AgentIntent.deadline, gpGlobals->curtime ) ) return false;
	return AgPBHolding::HoldsPosition( task, m_AgentIntent.camp, m_AgentIntent.arrived );
}

bool CAgPB::NavigationRequiresCrouch( unsigned int flags )
{
	// SyPB basecode.cpp: navigation excludes CAMP from authored crouch.
	// CAMP+CROUCH selects the posture only after entering a holding task.
	return AgPBHolding::NavigationCrouch( ( flags & AgPB_WP_CROUCH ) != 0,
		( flags & AgPB_WP_CAMP ) != 0 );
}

bool CAgPB::TaskRequiresCrouch() const
{
	const AgPBTask &task = m_Tasks.Current();
	if ( gpGlobals == NULL || !AgPBHolding::Active( task, m_Tasks.State( task.token ), gpGlobals->curtime,
		IsAlive(), AgPB_GameMode() == AgPB_MODE_NORMAL,
		AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 ) ||
		!AgPBHolding::AgentValid( task, m_AgentIntent.token, m_AgentIntent.waypoint,
			m_AgentIntent.deadline, gpGlobals->curtime ) ) return false;
	const AgPBPath *holding = BotWaypoints().Get( task.waypoint );
	const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
	const AgPBPath *previous = BotWaypoints().Get( RouteNode( m_iRouteIndex - 1 ) );
	const bool previousCrouch = previous != NULL && NavigationRequiresCrouch( previous->flags );
	const float previousDistance = previousCrouch && m_pInfo != NULL
		? ( previous->origin - m_pInfo->GetAbsOrigin() ).Length2D() : 64.0f;
	return AgPBHolding::RequiresCrouch( task, IsHoldingTaskPosition(),
		holding != NULL && ( holding->flags & AgPB_WP_CROUCH ) != 0,
		m_uRouteTaskToken, node != NULL && NavigationRequiresCrouch( node->flags ),
		previousCrouch, previousDistance );
}

void CAgPB::ApplyTaskPosture( CBotCmd &cmd ) const
{
	// Never release crouch requested by combat, geometry or explicit input.
	// Think starts a fresh command each tick, so invalidated tasks add nothing.
	if ( TaskRequiresCrouch() ) cmd.buttons |= IN_DUCK;
}

void CAgPB::ResetTaskUse()
{
	m_flNextTaskUse = 0.0f;
	m_uLastTaskUseHandle = (uintp)INVALID_EHANDLE_INDEX;
}

void CAgPB::ResetBombEscape()
{
	m_uEscapeBombHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_uEscapeRoundEpoch = 0;
	m_bEscapeCommitted = false;
	m_vecEscapeFailedGoals.RemoveAll();
	m_flEscapeRetryAfter = m_flNextEscapeReport = 0.0f;
}

void CAgPB::ResetTaskSystem( const char *reason )
{
	ResetPlayerAvoidance();
	++m_uTaskResetCount;
	m_pszTaskResetReason = reason;
	ClearAgentIntent( reason );
	ClearCombat( reason );
	ResetWeaponSelection( reason );
	ResetSearch();
	ResetAutonomy( gpGlobals != NULL ? gpGlobals->curtime : 0.0f );
	ClearRoute();
	m_Tasks.Reset();
	m_uObservedTaskToken = m_uRouteTaskToken = 0;
	m_flNextTaskPlan = m_flTaskActionStarted = 0.0f;
	m_uDeclinedObjectiveHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_flObjectiveRetryAfter = 0.0f;
	ResetTaskUse();
	ResetBombEscape();
	m_bHasVelOverride = false;
	m_bPendingJumpLaunch = false;
	m_vVelOverride.Init();
}

void CAgPB::SuspendTaskActions( float now )
{
	ResetPlayerAvoidance();
	ClearCombat( "frozen" );
	ResetWeaponSelection( "frozen" );
	ClearBreakable();
	ResetTaskUse();
	ResetRouteProgress( now );
	// A terminal arrival needs one uninterrupted unfrozen settling interval.
	m_flRouteSettleSince = -1.0f;
	m_bHasVelOverride = false;
	m_bPendingJumpLaunch = false;
	m_vVelOverride.Init();
}

void CAgPB::StopRoute()
{
	m_bAutonomous = m_bRoam = false;
	ResetTaskSystem( "stop" );
}

void CAgPB::SetAutonomous( bool enable )
{
	m_bAutonomous = enable;
	m_bRoam = false;
	ResetTaskSystem( "autonomy" );
}

bool CAgPB::StartRoute( int goal, char *error, size_t maxlen )
{
	StopRoute();
	if ( !BuildRoute( goal, error, maxlen ) ) return false;
	AgPBTask task;
	task.id = AgPBTaskId::MoveToPosition;
	task.desire = AgPBTaskPriority::MoveToPosition;
	task.waypoint = goal;
	task.canResume = true;
	m_uRouteTaskToken = m_Tasks.Push( task );
	return true;
}

void CAgPB::CompleteCurrentTask( const char *reason, bool failed )
{
	const AgPBTask task = m_Tasks.Current();
	if ( task.id == AgPBTaskId::Investigate ) m_Search.investigation = AgPBSearch::Investigation();
	RouteReport( "task %s %s: %s", AgPBTaskName( task.id ), failed ? "failed" : "complete", reason );
	if ( failed && task.id == AgPBTaskId::EscapeBomb )
	{
		if ( task.waypoint >= 0 && m_vecEscapeFailedGoals.Find( task.waypoint ) == m_vecEscapeFailedGoals.InvalidIndex() )
			m_vecEscapeFailedGoals.AddToTail( task.waypoint );
		const float now = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
		m_flEscapeRetryAfter = m_flNextTaskPlan = now + 1.0f;
		ClearRoute();
		// Keep emergency priority while waiting. Do not resume a suspended
		// patrol/defuse, and never apply the ordinary five-second cooldown.
		AgPBTask hold = task;
		hold.waypoint = -1; hold.deadline = 0.0f;
		if ( m_pInfo != NULL ) SetTaskPosition( hold, m_pInfo->GetAbsOrigin() );
		m_Tasks.Push( hold );
		ResetTaskUse();
		return;
	}
	if ( failed && task.hasEntity )
	{
		m_uDeclinedObjectiveHandle = (uintp)task.entityHandle;
		m_flObjectiveRetryAfter = ( gpGlobals != NULL ? gpGlobals->curtime : 0.0f ) + 5.0f;
	}
	ClearRoute();
	m_Tasks.Complete( task.token );
	m_flNextTaskPlan = ( gpGlobals != NULL ? gpGlobals->curtime : 0.0f ) + ( failed ? 1.0f : 0.1f );
	ResetTaskUse();
}

edict_t *CAgPB::FindOwnedWeapon( const char *classname ) const
{
	const BotNetVar *weapons = FindNetVar( "m_hMyWeapons" );
	void *base = NetVarBase();
	if ( base == NULL || weapons == NULL || weapons->type != DPT_Array ||
	     weapons->elementType != DPT_Int || weapons->stride != sizeof( uintp ) ) return NULL;
	for ( int i = 0; i < weapons->elements; ++i )
	{
		edict_t *weapon = HandleToEdict( NetVar_GetArrayHandle( base, *weapons, i ) );
		const char *name = weapon != NULL && !weapon->IsFree() ? weapon->GetClassName() : NULL;
		if ( name != NULL && Q_stricmp( name, classname ) == 0 ) return weapon;
	}
	return NULL;
}

class CTaskUseTraceFilter : public CTraceFilter
{
public:
	CTaskUseTraceFilter( IHandleEntity *self ) : m_Self( self ) {}
	bool ShouldHitEntity( IHandleEntity *entity, int ) { return entity != m_Self; }
private:
	IHandleEntity *m_Self;
};

bool CAgPB::TaskAimAt( edict_t *target, const Vector &point, CBotCmd &cmd ) const
{
	if ( target == NULL || m_Ctx.pTrace == NULL || m_Ctx.pGameEnts == NULL || m_pInfo == NULL ) return false;
	Vector offset = GetNetVarVector( "m_vecViewOffset" );
	if ( offset.z <= 0.0f ) offset.z = AgPB_HullStandHeight() - 4.0f;
	const Vector eye = m_pInfo->GetAbsOrigin() + offset;
	Vector direction = point - eye;
	if ( VectorNormalize( direction ) < 0.01f ) return false;
	QAngle angles;
	VectorAngles( direction, angles );
	angles.x = clamp( AngleNormalize( angles.x ), -89.0f, 89.0f );
	angles.y = AngleNormalize( angles.y ); angles.z = 0;
	cmd.viewangles = angles;
	IServerNetworkable *network = m_pEdict != NULL && !m_pEdict->IsFree() && m_pEdict->GetUnknown() != NULL
		? m_pEdict->GetNetworkable() : NULL;
	CTaskUseTraceFilter filter( network != NULL ? network->GetEntityHandle() : NULL );
	Ray_t ray; ray.Init( eye, point + direction * 8.0f );
	trace_t hit;
	m_Ctx.pTrace->TraceRay( ray, MASK_SOLID, &filter, &hit );
	return !hit.startsolid && !hit.allsolid && hit.m_pEnt != NULL &&
		m_Ctx.pGameEnts->BaseEntityToEdict( hit.m_pEnt ) == target;
}

int CAgPB::PickTaskWaypoint( const Vector &position, unsigned int requiredFlags ) const
{
	CAgPBWaypoints &wp = BotWaypoints();
	const int team = m_pInfo != NULL ? m_pInfo->GetTeamIndex() : m_iTeam;
	const int start = m_pInfo != NULL ? wp.FindNearest( m_pInfo->GetAbsOrigin(), 512.0f ) : -1;
	if ( !wp.IsValid( start ) ) return -1;
	CUtlVector<float> distances;
	CUtlVector<short> parents;
	wp.ComputeDistances( start, distances, parents, team );
	int best = -1;
	float score = 512.0f * 512.0f;
	for ( int i = 0; i < wp.Count(); ++i )
	{
		const AgPBPath *node = wp.Get( i );
		if ( !wp.IsPassable( i, team ) || distances[i] >= AgPB_WP_DIST_INF ||
		     ( requiredFlags != 0 && ( node->flags & requiredFlags ) == 0 ) ) continue;
		const float distance = ( node->origin - position ).LengthSqr();
		if ( distance < score ) { score = distance; best = i; }
	}
	return best;
}

bool CAgPB::NavigateTaskTo( const Vector &position, int preferredWaypoint, float interactionRange,
                          CGlobalVars *globals, CBotCmd &cmd )
{
	if ( !position.IsValid() || m_pInfo == NULL )
	{
		CompleteCurrentTask( "objective has no finite position", true ); return false;
	}
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector delta = position - here;
	if ( delta.Length2D() <= interactionRange && fabsf( delta.z ) <= AgPB_HullStandHeight() )
	{
		if ( HasRoute() ) ClearRoute();
		return true;
	}
	if ( HasRoute() )
	{
		const std::uint64_t token = m_Tasks.Current().token;
		UpdateRoute( globals, cmd );
		if ( !HasRoute() && !m_bRouteArrived && m_Tasks.Current().token == token )
			CompleteCurrentTask( "manual route failed", true );
		return false;
	}
	if ( !m_bRouteArrived && delta.Length2D() <= 96.0f && fabsf( delta.z ) < 32.0f )
	{
		// Interacting with an adjacent item is a short supported approach,
		// not a route across the map. It also works while a map is unwaypointed.
		m_bRouteArrived = true;
		m_uRouteTaskToken = m_Tasks.Current().token;
	}
	if ( !m_bRouteArrived )
	{
		int goal = preferredWaypoint;
		if ( !BotWaypoints().IsValid( goal ) ) goal = PickTaskWaypoint( position );
		char error[192];
		if ( goal < 0 || !BuildRoute( goal, error, sizeof( error ) ) )
		{
			CompleteCurrentTask( goal < 0 ? "no reachable manual waypoint near objective" : error, true );
			return false;
		}
		m_uRouteTaskToken = m_Tasks.Current().token;
		UpdateRoute( globals, cmd );
		return false;
	}
	// Only the final interaction approach leaves the authored waypoint center.
	// A grounded, supported hull sweep must allow it; this never creates edges.
	Vector walkTarget = position;
	if ( fabsf( walkTarget.z - here.z ) < 32.0f ) walkTarget.z = here.z;
	const float flatDistance = delta.Length2D();
	if ( flatDistance > interactionRange && interactionRange > 8.0f )
	{
		// Validate the approach only as far as the interaction shell. Sweeping
		// all the way into a solid hostage would reject an otherwise legal use.
		const float fraction = clamp( ( flatDistance - interactionRange + 1.0f ) / flatDistance, 0.0f, 1.0f );
		walkTarget = here + ( walkTarget - here ) * fraction;
	}
	const AgPBPath *node = BotWaypoints().Get( preferredWaypoint );
	const bool crouch = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0 ||
		( node != NULL && NavigationRequiresCrouch( node->flags ) );
	if ( delta.Length2D() > 512.0f || !CanWalkToTaskPosition( walkTarget, crouch ) )
	{
		CompleteCurrentTask( "objective final approach is not walkable", true ); return false;
	}
	QAngle view; VectorAngles( Vector( delta.x, delta.y, 0 ), view );
	cmd.viewangles.x = 0; cmd.viewangles.y = view.y;
	cmd.forwardmove = crouch ? 250.0f : clamp( delta.Length2D() * 4.0f, 100.0f, 250.0f );
	if ( crouch ) cmd.buttons |= IN_DUCK;
	return false;
}

static bool InObjectiveRegion( const Vector &feet, const AgPBObjectiveRegion &region )
{
	if ( !region.boundsKnown ) return region.positionKnown && ( feet - region.center ).Length2D() < 96.0f;
	return feet.x >= region.worldMins.x && feet.x <= region.worldMaxs.x &&
		feet.y >= region.worldMins.y && feet.y <= region.worldMaxs.y &&
		feet.z <= region.worldMaxs.z && feet.z + AgPB_HullStandHeight() >= region.worldMins.z;
}

bool CAgPB::PlanPlantedBombTask( CGlobalVars *globals, bool urgentOnly )
{
	// Game facts remain observable, but independent local goals cannot revoke
	// an effective high-level task owner, including its execution child.
	if ( m_AgentIntent.token != 0 ) return false;
	const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
	const AgPBObjectiveBomb &bomb = world.bomb;
	const float now = globals->curtime;
	if ( m_bEscapeCommitted && m_uEscapeRoundEpoch != world.roundEpoch )
	{
		m_Tasks.Remove( AgPBTaskId::EscapeBomb );
		ResetBombEscape();
	}
	if ( ( world.mapFlags & AGPB_MAP_BOMB ) == 0 ||
	     ( bomb.state != AGPB_BOMB_PLANTED && !( bomb.state == AGPB_BOMB_UNKNOWN && bomb.plantedObserved ) ) )
		return false;
	if ( m_uEscapeBombHandle != bomb.handle )
	{
		m_Tasks.Remove( AgPBTaskId::EscapeBomb );
		ResetBombEscape();
		m_uEscapeBombHandle = bomb.handle;
		m_uEscapeRoundEpoch = world.roundEpoch;
	}
	const AgPBTask *running = m_Tasks.Find( AgPBTaskId::EscapeBomb );
	if ( running != NULL && ( running->waypoint >= 0 || now < m_flEscapeRetryAfter ) ) return true;
	// In-flight route execution already has a supported entry. Defer expensive
	// replanning until grounded rather than misclassifying that entry as absent.
	if ( HasRoute() && ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ) return running != NULL;
	const int team = m_pInfo->GetTeamIndex();
	const Vector here = m_pInfo->GetAbsOrigin();
	const float hereDistance = bomb.positionKnown ? ( here - bomb.position ).Length() : 0.0f;
	const bool safeHere = bomb.positionKnown && AgPB_BombPositionSafe( hereDistance, world.bombBlastRadiusKnown, world.bombBlastRadius );
	const bool timeKnown = bomb.blowTimeKnown && IsFinite( bomb.blowTime ) && bomb.blowTime > 0.0f;
	const float left = timeKnown ? bomb.blowTime - now : -1.0f;
	const bool defusing = GetNetVarInt( "m_bIsDefusing" ) != 0;
	const float defuseDuration = GetNetVarInt( "m_bHasDefuser" ) != 0 ? 5.0f : 10.0f;
	float defuseSeconds = AgPB_WP_DIST_INF;
	float defuseGoalDistance = AgPB_WP_DIST_INF;
	int defuseGoal = -1, escapeGoal = -1, escapeStart = -1;
	float escapeSeconds = AgPB_WP_DIST_INF, escapeLength = 0.0f, escapeDistance = hereDistance;
	bool safeRoute = false;
	bool ownDefuseCountdownKnown = false;
	if ( defusing )
	{
		// The world snapshot is throttled and may predate our first use tick.
		// Read this serial-checked C4's live countdown before applying SyPB's
		// active-progress exemption; missing/zero/invalid data grants no exemption.
		edict_t *charge = HandleToEdict( bomb.handle );
		const BotNetVar *countdown = charge != NULL ? AgPB_FindEntityNetVar( charge, "m_flDefuseCountDown" ) : NULL;
		const bool countdownKnown = countdown != NULL && countdown->type == DPT_Float && AgPB_EntityBase( charge ) != NULL;
		ownDefuseCountdownKnown = AgPB_BombDefuseRemaining( defusing, countdownKnown,
			countdownKnown ? AgPB_ReadNetVarFloat( charge, "m_flDefuseCountDown" ) : 0.0f, now, defuseSeconds );
	}
	else if ( bomb.positionKnown && ( here - bomb.position ).Length2D() <= 44.0f &&
	          fabsf( here.z - bomb.position.z ) <= AgPB_HullStandHeight() ) defuseSeconds = defuseDuration;

	CAgPBWaypoints &wp = BotWaypoints();
	const float speed = GetNetVarFloat( "m_flMaxspeed", 200.0f );
	std::vector<AgPBBombRouteSeed> entries;
	if ( bomb.positionKnown )
		for ( int i = 0; i < wp.Count(); ++i )
		{
			const AgPBPath *node = wp.Get( i );
			if ( !wp.IsPassable( i, team ) || !node->origin.IsValid() ) continue;
			const float distance = ( node->origin - here ).Length();
			const bool crouch = NavigationRequiresCrouch( node->flags );
			if ( distance > 512.0f || !CanWalkToTaskPosition( node->origin, crouch ) ) continue;
			AgPBBombRouteSeed entry = { i, 0.5f + AgPB_BombTravelSeconds( distance, speed,
				crouch, ( node->flags & AgPB_WP_LADDER ) != 0, false ), distance };
			entries.push_back( entry );
		}
	std::vector<AgPBBombRouteLabel> routes;
	AgPB_BombRouteBudgets( wp.Count(), AgPB_WP_MAX_PATH_INDEX, entries,
		[&]( int current, int slot, int &next, float &length, float &seconds ) {
			const AgPBPath *from = wp.Get( current );
			next = from->index[slot];
			const AgPBPath *to = wp.Get( next );
			const unsigned int flags = from->connectionFlags[slot];
			if ( to == NULL || !to->origin.IsValid() || !wp.IsPassable( next, team ) ||
			     ( flags & AgPB_PATH_DOUBLE ) != 0 ) return false;
			length = ( to->origin - from->origin ).Length();
			seconds = AgPB_BombTravelSeconds( length, speed,
				NavigationRequiresCrouch( from->flags ) || NavigationRequiresCrouch( to->flags ),
				( ( from->flags | to->flags ) & AgPB_WP_LADDER ) != 0, ( flags & AgPB_PATH_JUMP ) != 0 );
			return true;
		}, routes );
	for ( int i = 0; i < wp.Count(); ++i )
	{
		const AgPBBombRouteLabel &route = routes[i];
		if ( route.seconds >= AgPB_WP_DIST_INF ) continue;
		const AgPBPath *node = wp.Get( i );
		const float d = ( node->origin - bomb.position ).Length();
		if ( !defusing && team == AgPB_TEAM_CT && d <= 512.0f )
		{
			const float approach = AgPB_BombTravelSeconds( MAX( 0.0f, d - 44.0f ), speed,
				NavigationRequiresCrouch( node->flags ), false, false );
			const float budget = route.seconds + approach + defuseDuration;
			// Preserve the normal final-approach target: the reachable node
			// closest to C4, rather than an optimistic cross-wall shortcut.
			if ( d < defuseGoalDistance && defuseSeconds > defuseDuration )
			{
				defuseGoalDistance = d; defuseSeconds = budget; defuseGoal = i;
			}
		}
		if ( m_vecEscapeFailedGoals.Find( i ) != m_vecEscapeFailedGoals.InvalidIndex() ) continue;
		const bool safe = AgPB_BombPositionSafe( d - 32.0f, world.bombBlastRadiusKnown, world.bombBlastRadius );
		if ( !safe && d <= hereDistance + 64.0f ) continue;
		if ( AgPB_BombCandidateBetter( safe, route.seconds, d, safeRoute, escapeSeconds, escapeDistance ) )
		{
			escapeGoal = i; escapeStart = route.entry; escapeSeconds = route.seconds;
			escapeLength = route.length; escapeDistance = d; safeRoute = safe;
		}
	}
	auto installRoute = [&]( int goal, std::uint64_t token ) {
		if ( m_Tasks.Current().token != token ) return;
		ClearRoute();
		if ( goal < 0 ) return;
		// Execute the selected ETA path exactly; rebuilding by Euclidean
		// distance or the nearest entry could choose a different, slower path.
		for ( int node = goal, count = 0; node >= 0 && count < wp.Count(); ++count )
		{
			m_vecRoute.AddToTail( node );
			node = routes[node].parent;
		}
		for ( int i = 0; i < m_vecRoute.Count() / 2; ++i )
		{
			const int other = m_vecRoute.Count() - i - 1, saved = m_vecRoute[i];
			m_vecRoute[i] = m_vecRoute[other]; m_vecRoute[other] = saved;
		}
		m_iRouteIndex = 0; m_iGoalWaypoint = goal; m_uRouteTaskToken = token;
		ResetRouteProgress( now );
	};
	const bool canDefuse = defuseSeconds < AgPB_WP_DIST_INF;
	const bool escape = AgPB_BombShouldEscape( team == AgPB_TEAM_T, timeKnown, left,
		world.bombBlastRadiusKnown, m_bEscapeCommitted, canDefuse, defuseSeconds, safeHere, safeRoute, escapeSeconds,
		ownDefuseCountdownKnown );
	if ( !AgPB_BombObjectiveEligible( escape, bomb.handle == m_uDeclinedObjectiveHandle, now, m_flObjectiveRetryAfter ) ) return false;
	if ( !escape )
	{
		if ( team != AgPB_TEAM_CT || m_Tasks.Contains( AgPBTaskId::DefuseBomb ) ||
		     ( urgentOnly && m_Tasks.Current().desire >= AgPBTaskPriority::Objective ) ) return false;
		AgPBTask task;
		task.id = AgPBTaskId::DefuseBomb; task.desire = AgPBTaskPriority::Objective;
		task.hasEntity = true; task.entityHandle = bomb.handle; task.waypoint = defuseGoal;
		task.deadline = now + 90.0f; task.canResume = true; SetTaskPosition( task, bomb.position );
		installRoute( task.waypoint, m_Tasks.Push( task ) );
		return true;
	}
	m_bEscapeCommitted = true;
	AgPBTask task;
	task.id = AgPBTaskId::EscapeBomb; task.desire = AgPBTaskPriority::EscapeBomb;
	task.hasEntity = true; task.entityHandle = bomb.handle; task.canResume = true;
	task.waypoint = safeHere ? -1 : escapeGoal;
	SetTaskPosition( task, task.waypoint >= 0 ? wp.Get( task.waypoint )->origin : here );
	if ( running == NULL || running->waypoint != task.waypoint ) installRoute( task.waypoint, m_Tasks.Push( task ) );
	m_flEscapeRetryAfter = now + 1.0f;
	if ( now >= m_flNextEscapeReport )
	{
		RouteReport( "escape plan: left=%.2f timer=%s goal=#%d entry=#%d path=%.1f eta=%.2f bomb-dist=%.1f radius=%.1f source=%s safe=%s defuse-eta=%.2f failed-goals=%d",
			left, timeKnown ? "known" : "unknown", task.waypoint, escapeStart, escapeLength,
			safeHere ? 0.0f : ( escapeGoal >= 0 ? escapeSeconds : -1.0f ), safeHere ? hereDistance : escapeDistance,
			world.bombBlastRadiusKnown ? world.bombBlastRadius : -1.0f, AgPB_BombRadiusSourceName( world.bombRadiusSource ),
			safeHere || safeRoute ? "yes" : "NO-best-effort", canDefuse ? defuseSeconds : -1.0f, m_vecEscapeFailedGoals.Count() );
		m_flNextEscapeReport = now + 2.0f;
	}
	return true;
}

void CAgPB::PlanNormalTask( CGlobalVars *globals, bool urgentOnly )
{
	if ( m_AgentIntent.token != 0 ) return;
	if ( PlanPlantedBombTask( globals, urgentOnly ) ) return;
	const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
	const int team = m_pInfo->GetTeamIndex();
	const uintp self = AgPB_RefEHandle( m_pEdict );
	const Vector here = m_pInfo->GetAbsOrigin();
	AgPBTask task;
	task.desire = AgPBTaskPriority::Objective;
	task.deadline = globals->curtime + 90.0f;
	task.canResume = true;
	auto eligible = [&]( uintp handle ) {
		return handle != m_uDeclinedObjectiveHandle || globals->curtime >= m_flObjectiveRetryAfter;
	};
	auto assignEntity = [&]( uintp handle, const Vector &position ) {
		task.entityHandle = handle; task.hasEntity = true; SetTaskPosition( task, position );
		task.waypoint = PickTaskWaypoint( position );
	};
	auto regionWaypoint = [&]( const CUtlVector<AgPBObjectiveRegion> &regions, unsigned int flag ) {
		int best = -1; float bestScore = AgPB_WP_DIST_INF;
		CAgPBWaypoints &wp = BotWaypoints();
		const int start = wp.FindNearest( here, 512.0f );
		if ( !wp.IsValid( start ) ) return -1;
		CUtlVector<float> distances; CUtlVector<short> parents;
		wp.ComputeDistances( start, distances, parents, team );
		for ( int pass = 0; pass < 2 && best < 0; ++pass )
			for ( int i = 0; i < wp.Count(); ++i )
			{
				const AgPBPath *node = wp.Get( i );
				if ( !wp.IsPassable( i, team ) || distances[i] >= AgPB_WP_DIST_INF ||
				     ( pass == 0 && ( node->flags & flag ) == 0 ) ) continue;
				for ( int r = 0; r < regions.Count(); ++r )
				{
					if ( !InObjectiveRegion( node->origin, regions[r] ) ) continue;
					float score = distances[i];
					if ( flag == AgPB_WP_RESCUE && regions[r].boundsKnown )
					{
						// Followers trail behind the CT. Prefer an authored point
						// inside the volume so the hostage can actually touch it.
						const float margin = MIN( MIN( node->origin.x - regions[r].worldMins.x,
							regions[r].worldMaxs.x - node->origin.x ), MIN( node->origin.y - regions[r].worldMins.y,
							regions[r].worldMaxs.y - node->origin.y ) );
						score += MAX( 0.0f, 128.0f - margin ) * 8.0f;
					}
					if ( score < bestScore ) { best = i; bestScore = score; }
				}
			}
		return best;
	};
	if ( ( world.mapFlags & AGPB_MAP_BOMB ) != 0 )
	{
		const AgPBObjectiveBomb &bomb = world.bomb;
		if ( team == AgPB_TEAM_T && FindOwnedWeapon( "weapon_c4" ) != NULL )
		{
			task.id = AgPBTaskId::PlantBomb;
			task.hasEntity = true; task.entityHandle = AgPB_RefEHandle( FindOwnedWeapon( "weapon_c4" ) );
			if ( eligible( (uintp)task.entityHandle ) )
			{
				task.waypoint = regionWaypoint( world.bombSites, AgPB_WP_GOAL );
				if ( GetNetVarInt( "m_bInBombZone" ) != 0 ) SetTaskPosition( task, here );
				else if ( task.waypoint >= 0 ) SetTaskPosition( task, BotWaypoints().Get( task.waypoint )->origin );
				if ( task.hasPosition ) { m_Tasks.Push( task ); return; }
			}
		}
		if ( team == AgPB_TEAM_T && bomb.state == AGPB_BOMB_DROPPED && bomb.positionKnown && eligible( bomb.handle ) )
		{
			task.id = AgPBTaskId::PickupBomb; assignEntity( bomb.handle, bomb.position );
			m_Tasks.Push( task ); return;
		}
		if ( urgentOnly ) return;
		// Non-carriers select their own normal-mode tactic below. They do not
		// all reserve whichever bombsite has the shortest distance from spawn.
	}
	if ( urgentOnly && ( world.mapFlags & AGPB_MAP_HOSTAGE ) == 0 ) return;
	if ( ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 )
	{
		const AgPBObjectiveHostage *following = NULL, *freeHostage = NULL;
		float best = AgPB_WP_DIST_INF;
		for ( int i = 0; i < world.hostages.Count(); ++i )
		{
			const AgPBObjectiveHostage &hostage = world.hostages[i];
			if ( !hostage.positionKnown || ( hostage.aliveKnown && !hostage.alive ) || hostage.rescued ) continue;
			if ( hostage.leaderKnown && hostage.leader == self ) { following = &hostage; continue; }
			if ( ( hostage.leaderKnown && NetVar_HandleIsValid( hostage.leader ) ) || !eligible( hostage.handle ) ) continue;
			const float d = ( hostage.position - here ).LengthSqr();
			if ( d < best && PickTaskWaypoint( hostage.position ) >= 0 ) { best = d; freeHostage = &hostage; }
		}
		if ( team == AgPB_TEAM_CT )
		{
			if ( following != NULL && ( freeHostage == NULL || best > 256.0f * 256.0f ) )
			{
				task.id = AgPBTaskId::EscortHostages; task.hasEntity = true; task.entityHandle = following->handle;
				task.waypoint = regionWaypoint( world.rescueZones, AgPB_WP_RESCUE );
				if ( task.waypoint >= 0 )
				{
					SetTaskPosition( task, BotWaypoints().Get( task.waypoint )->origin );
					m_Tasks.Push( task ); return;
				}
			}
			if ( freeHostage != NULL )
			{
				task.id = AgPBTaskId::RescueHostage; assignEntity( freeHostage->handle, freeHostage->position );
				m_Tasks.Push( task ); return;
			}
		}
		// Hostage defenders choose varied authored patrol/guard goals below.
	}
	if ( urgentOnly ) return;
	// Urgent map tasks have had first refusal. Search only real recent
	// observations; otherwise choose a SyPB-style local patrol/guard tactic.
	if ( PlanEnemyInvestigation( globals ) ) return;
	if ( !PlanAutonomousPatrol( globals ) ) m_flNextTaskPlan = globals->curtime + 1.0f;
}

void CAgPB::RefreshTaskPlan( CGlobalVars *globals )
{
	if ( m_AgentIntent.token != 0 ) return;
	if ( !m_bAutonomous || AgPB_GameMode() != AgPB_MODE_NORMAL || AgPB_IsRoundFrozen() ||
	     ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 || globals->curtime < m_flNextTaskPlan ) return;
	const AgPBTask task = m_Tasks.Current();
	if ( task.deadline > 0.0f && globals->curtime >= task.deadline ) return;
	if ( task.id == AgPBTaskId::Normal )
	{
		m_flNextTaskPlan = globals->curtime + 0.5f;
		PlanNormalTask( globals );
		return;
	}
	// Keep the existing guarded SyPB-derived objective arbitration available
	// while a combat command overlay suspends locomotion/action dispatch.
	m_flNextTaskPlan = globals->curtime + 0.5f;
	const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
	const AgPBObjectiveBomb &bomb = world.bomb;
	const int team = m_pInfo->GetTeamIndex();
	const bool plantedHandled = PlanPlantedBombTask( globals, true );
	const bool eligible = bomb.handle != m_uDeclinedObjectiveHandle || globals->curtime >= m_flObjectiveRetryAfter;
	const bool bombUrgent = ( world.mapFlags & AGPB_MAP_BOMB ) != 0 && eligible &&
		( ( team == AgPB_TEAM_T && FindOwnedWeapon( "weapon_c4" ) != NULL &&
	       !m_Tasks.Contains( AgPBTaskId::PlantBomb ) ) ||
		( team == AgPB_TEAM_T && bomb.state == AGPB_BOMB_DROPPED &&
		  !m_Tasks.Contains( AgPBTaskId::PickupBomb ) ) );
	const bool hostageUrgent = ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 && team == AgPB_TEAM_CT &&
		!m_Tasks.Contains( AgPBTaskId::RescueHostage ) && !m_Tasks.Contains( AgPBTaskId::EscortHostages );
	if ( !plantedHandled && ( bombUrgent || hostageUrgent ) &&
	     m_Tasks.Current().desire < AgPBTaskPriority::Objective ) PlanNormalTask( globals, true );
	PlanEnemyInvestigation( globals );
}

void CAgPB::UpdateTasks( CGlobalVars *globals, CBotCmd &cmd )
{
	UpdateAgentIntent( globals );
	if ( AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 )
	{
		SuspendTaskActions( globals->curtime ); return;
	}
	if ( m_bAutonomous && AgPB_GameMode() != AgPB_MODE_NORMAL )
	{
		ResetTaskSystem( "mode" ); return;
	}
	const AgPBTask beforeExpiry = m_Tasks.Current();
	if ( beforeExpiry.deadline > 0.0f && globals->curtime >= beforeExpiry.deadline )
	{
		CompleteCurrentTask( "task deadline", beforeExpiry.id != AgPBTaskId::Camp && beforeExpiry.id != AgPBTaskId::Pause && beforeExpiry.id != AgPBTaskId::Investigate );
		return;
	}
	m_Tasks.Expire( globals->curtime );
	if ( m_uRouteTaskToken != 0 && m_Tasks.State( m_uRouteTaskToken ) == AgPBTaskState::Missing ) ClearRoute();
	if ( m_Tasks.Current().id != AgPBTaskId::Normal ) RefreshTaskPlan( globals );
	if ( m_Search.investigation.token != 0 && m_Tasks.State( m_Search.investigation.token ) == AgPBTaskState::Missing )
		m_Search.investigation = AgPBSearch::Investigation();
	AgPBTask task = m_Tasks.Current();
	if ( task.id != AgPBTaskId::DestroyBreakable && m_uBreakTaskToken != 0 ) ClearBreakable();
	if ( task.id != AgPBTaskId::DestroyBreakable && m_uRouteTaskToken != task.token &&
	     ( HasRoute() || m_bRouteArrived ) ) ClearRoute();
	if ( task.token != m_uObservedTaskToken )
	{
		m_uObservedTaskToken = task.token;
		m_flTaskActionStarted = task.id == AgPBTaskId::Normal ? 0.0f : globals->curtime;
		ResetTaskUse();
		RouteReport( "task -> %s (wp=%d, priority=%.0f)", AgPBTaskName( task.id ), task.waypoint, task.desire );
	}
	if ( task.id == AgPBTaskId::DestroyBreakable )
	{
		if ( !UpdateBreakable( globals, cmd ) ) CompleteCurrentTask( "breakable state released" );
		return;
	}
	if ( UpdateEnemyInvestigation( globals, cmd ) ) return;
	if ( IsHoldingTaskPosition() )
	{
		ApplyTaskPosture( cmd );
		if ( !UpdateEnemyMemoryLook( globals, cmd ) ) UpdateIdleLook( globals, cmd );
		return;
	}
	if ( task.id == AgPBTaskId::Normal )
	{
		if ( globals->curtime >= m_flNextTaskPlan )
		{
			m_flNextTaskPlan = globals->curtime + 0.5f;
			if ( m_bAutonomous ) PlanNormalTask( globals );
			else if ( m_bRoam && StartRandomRoute() )
			{
				AgPBTask move; move.id = AgPBTaskId::MoveToPosition;
				move.desire = AgPBTaskPriority::MoveToPosition; move.waypoint = m_iGoalWaypoint;
				m_uRouteTaskToken = m_Tasks.Push( move );
			}
			else m_flNextTaskPlan = globals->curtime + 1.0f;
		}
		return;
	}
	if ( ExecuteObjectiveTask( globals, cmd ) ) return;
	if ( task.id == AgPBTaskId::MoveToPosition || task.id == AgPBTaskId::AgentIntention )
	{
		const bool agent = task.id == AgPBTaskId::AgentIntention;
		if ( !HasRoute() && !m_bRouteArrived )
		{
			char error[192];
			if ( !BuildRoute( task.waypoint, error, sizeof( error ) ) )
			{
				if ( agent )
				{
					RouteReport( "agent route failed: %s", error );
					ClearAgentIntent( "route-failed" );
				}
				else CompleteCurrentTask( error, true );
				return;
			}
			m_uRouteTaskToken = task.token;
		}
		if ( HasRoute() ) UpdateRoute( globals, cmd );
		if ( !HasRoute() && m_Tasks.Current().token == task.token )
		{
			const bool arrived = m_bRouteArrived;
			if ( agent )
			{
				if ( arrived && m_AgentIntent.camp )
				{
					m_AgentIntent.arrived = true;
					RouteReport( "agent camp reached waypoint #%d; lease expires at %.3f", task.waypoint, m_AgentIntent.deadline );
				}
				else ClearAgentIntent( arrived ? "reached" : "route-failed" );
				return;
			}
			const bool camp = arrived && ( m_bAutonomous ?
				ShouldCampAtGoal( task.waypoint, globals->curtime ) :
				( BotWaypoints().Get( task.waypoint ) != NULL && ( BotWaypoints().Get( task.waypoint )->flags & AgPB_WP_CAMP ) != 0 ) );
			if ( arrived && m_bAutonomous ) RememberAutonomyGoal( task.waypoint, globals->curtime );
			CompleteCurrentTask( arrived ? "waypoint reached" : "manual route failed", !arrived );
			if ( camp )
			{
				AgPBTask camp; camp.id = AgPBTaskId::Camp; camp.desire = AgPBTaskPriority::Camp;
				camp.waypoint = task.waypoint;
				const float minimum = MAX( agpb_camp_min.GetFloat(), 1.0f );
				camp.deadline = globals->curtime + RandomFloat( minimum, MAX( agpb_camp_max.GetFloat(), minimum ) );
				if ( m_Tasks.Push( camp ) != 0 ) m_Autonomy.lastCampAt = globals->curtime;
			}
		}
	}
}

void CAgPB::ReportTasks() const
{
	const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
	RouteReport( "autonomy=%s map=%s bomb-state=%d tasks=%d route-owner=%llu",
		m_bAutonomous ? "on" : "off", ( world.mapFlags & AGPB_MAP_BOMB ) != 0 ? "bomb" :
		( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 ? "hostage" : "no-objective",
		(int)world.bomb.state, m_Tasks.Count(), (unsigned long long)m_uRouteTaskToken );
	RouteReport( "bomb safety: radius=%.1f source=%s remembered=%s escape=%s failed-goals=%d retry=%.2f",
		world.bombBlastRadiusKnown ? world.bombBlastRadius : -1.0f, AgPB_BombRadiusSourceName( world.bombRadiusSource ),
		world.bomb.plantedObserved ? "yes" : "no", m_bEscapeCommitted ? "committed" : "no",
		m_vecEscapeFailedGoals.Count(), m_flEscapeRetryAfter );
	RouteReport( "local decision: personality=%d tactic=%s selected=#%d camp=%d look=#%d control=%s",
		static_cast<int>( m_Autonomy.personality ), AgPB_LocalTacticName( m_Autonomy.selectedTactic ),
		m_Autonomy.selectedWaypoint, m_Autonomy.selectedCamp ? 1 : 0, m_Autonomy.lookWaypoint,
		m_AgentIntent.token != 0 ? "agent" : "local" );
	ReportSearch();
	for ( int i = 0; i < m_Tasks.Count(); ++i )
	{
		const AgPBTask *task = m_Tasks.At( i );
		RouteReport( "  %s %s wp=%d priority=%.0f deadline=%.1f token=%llu parent=%llu entity=%d",
			m_Tasks.State( task->token ) == AgPBTaskState::Active ? "active" : "suspended",
			AgPBTaskName( task->id ), task->waypoint, task->desire, task->deadline,
			(unsigned long long)task->token, (unsigned long long)task->parentToken, task->hasEntity ? NetVar_HandleEntry( (uintp)task->entityHandle ) : -1 );
	}
}
