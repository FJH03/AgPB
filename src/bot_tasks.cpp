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

void CAgPB::ResetTaskUse()
{
	m_flNextTaskUse = 0.0f;
	m_uLastTaskUseHandle = (uintp)INVALID_EHANDLE_INDEX;
}

void CAgPB::ResetTaskSystem()
{
	ClearRoute();
	m_Tasks.Reset();
	m_uObservedTaskToken = m_uRouteTaskToken = 0;
	m_flNextTaskPlan = m_flTaskActionStarted = 0.0f;
	m_uDeclinedObjectiveHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_flObjectiveRetryAfter = 0.0f;
	ResetTaskUse();
}

void CAgPB::StopRoute()
{
	m_bAutonomous = m_bRoam = false;
	ResetTaskSystem();
}

void CAgPB::SetAutonomous( bool enable )
{
	m_bAutonomous = enable;
	m_bRoam = false;
	ResetTaskSystem();
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
	RouteReport( "task %s %s: %s", AgPBTaskName( task.id ), failed ? "failed" : "complete", reason );
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
		IServerNetworkable *network = weapon != NULL ? weapon->GetNetworkable() : NULL;
		const char *name = network != NULL ? network->GetClassName() : NULL;
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
	IServerNetworkable *network = m_pEdict->GetNetworkable();
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
		( node != NULL && ( node->flags & AgPB_WP_CROUCH ) != 0 );
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

void CAgPB::PlanNormalTask( CGlobalVars *globals, bool urgentOnly )
{
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
		if ( bomb.state == AGPB_BOMB_PLANTED && bomb.positionKnown && eligible( bomb.handle ) )
		{
			const float left = bomb.blowTimeKnown ? bomb.blowTime - globals->curtime : 999.0f;
			const bool defusing = GetNetVarInt( "m_bIsDefusing" ) != 0;
			const float defuse = defusing ? ( bomb.defuseCountdownKnown ? MAX( 0.0f, bomb.defuseCountdown - globals->curtime ) : 0.0f ) :
				( GetNetVarInt( "m_bHasDefuser" ) != 0 ? 5.0f : 10.0f );
			if ( left <= ( team == AgPB_TEAM_CT ? defuse + ( defusing ? 0.2f : 1.0f ) : 8.0f ) )
			{
				task.id = AgPBTaskId::EscapeBomb; task.desire = AgPBTaskPriority::EscapeBomb;
				assignEntity( bomb.handle, bomb.position ); task.waypoint = -1;
				float far = -1.0f;
				const int start = BotWaypoints().FindNearest( here, 512.0f );
				CUtlVector<float> distances; CUtlVector<short> parents;
				BotWaypoints().ComputeDistances( start, distances, parents, team );
				for ( int i = 0; i < BotWaypoints().Count(); ++i )
				{
					const AgPBPath *node = BotWaypoints().Get( i );
					if ( !BotWaypoints().IsPassable( i, team ) ) continue;
					const float d = ( node->origin - bomb.position ).Length2D();
					if ( d <= far ) continue;
					if ( distances[i] >= AgPB_WP_DIST_INF ) continue;
					far = d; task.waypoint = i;
				}
				if ( task.waypoint >= 0 ) { m_Tasks.Push( task ); return; }
			}
			else if ( team == AgPB_TEAM_CT )
			{
				task.id = AgPBTaskId::DefuseBomb; assignEntity( bomb.handle, bomb.position );
				m_Tasks.Push( task ); return;
			}
		}
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
		// SyPB's defenders/escorts choose an objective area even without the C4.
		const int site = regionWaypoint( world.bombSites, AgPB_WP_GOAL );
		if ( site >= 0 )
		{
			task.id = AgPBTaskId::MoveToPosition; task.desire = AgPBTaskPriority::MoveToPosition;
			task.waypoint = site; SetTaskPosition( task, BotWaypoints().Get( site )->origin );
			m_Tasks.Push( task ); return;
		}
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
		else if ( freeHostage != NULL && !urgentOnly )
		{
			task.id = AgPBTaskId::MoveToPosition; task.desire = AgPBTaskPriority::MoveToPosition;
			task.waypoint = PickTaskWaypoint( freeHostage->position, AgPB_WP_CAMP );
			if ( task.waypoint < 0 ) task.waypoint = PickTaskWaypoint( freeHostage->position );
			if ( task.waypoint >= 0 ) { m_Tasks.Push( task ); return; }
		}
	}
	if ( urgentOnly ) return;
	// No objective, exhausted objectives, or no authored route to the objective:
	// preserve the normal-mode random patrol / CAMP behavior as a task.
	if ( StartRandomRoute() )
	{
		task.id = AgPBTaskId::MoveToPosition; task.desire = AgPBTaskPriority::MoveToPosition;
		task.waypoint = m_iGoalWaypoint; task.deadline = 0.0f; task.hasEntity = task.hasPosition = false;
		m_uRouteTaskToken = m_Tasks.Push( task );
	}
	else m_flNextTaskPlan = globals->curtime + 1.0f;
}

void CAgPB::UpdateTasks( CGlobalVars *globals, CBotCmd &cmd )
{
	if ( AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 )
	{
		ClearBreakable(); ResetRouteProgress( globals->curtime ); return;
	}
	if ( m_bAutonomous && AgPB_GameMode() != AgPB_MODE_NORMAL )
	{
		ResetTaskSystem(); return;
	}
	const AgPBTask beforeExpiry = m_Tasks.Current();
	if ( beforeExpiry.deadline > 0.0f && globals->curtime >= beforeExpiry.deadline )
	{
		CompleteCurrentTask( "task deadline", beforeExpiry.id != AgPBTaskId::Camp && beforeExpiry.id != AgPBTaskId::Pause );
		return;
	}
	m_Tasks.Expire( globals->curtime );
	if ( m_uRouteTaskToken != 0 && m_Tasks.State( m_uRouteTaskToken ) == AgPBTaskState::Missing ) ClearRoute();
	// Objective changes interrupt patrol/CAMP immediately, as SyPB's desires
	// do. Do not replace an already running objective on every observation.
	if ( m_bAutonomous && m_Tasks.Current().id != AgPBTaskId::Normal && globals->curtime >= m_flNextTaskPlan )
	{
		m_flNextTaskPlan = globals->curtime + 0.5f;
		const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
		const AgPBObjectiveBomb &bomb = world.bomb;
		const int team = m_pInfo->GetTeamIndex();
		const float left = bomb.blowTimeKnown ? bomb.blowTime - globals->curtime : 999.0f;
		const bool defusing = GetNetVarInt( "m_bIsDefusing" ) != 0;
		const float defuse = defusing ? ( bomb.defuseCountdownKnown ? MAX( 0.0f, bomb.defuseCountdown - globals->curtime ) : 0.0f ) :
			( GetNetVarInt( "m_bHasDefuser" ) != 0 ? 5.0f : 10.0f );
		const bool escape = bomb.state == AGPB_BOMB_PLANTED && left <= ( team == AgPB_TEAM_CT ? defuse + ( defusing ? 0.2f : 1.0f ) : 8.0f );
		const bool eligible = bomb.handle != m_uDeclinedObjectiveHandle || globals->curtime >= m_flObjectiveRetryAfter;
		const bool bombUrgent = ( world.mapFlags & AGPB_MAP_BOMB ) != 0 && eligible &&
			( ( escape && !m_Tasks.Contains( AgPBTaskId::EscapeBomb ) ) ||
		     ( !escape && bomb.state == AGPB_BOMB_PLANTED && team == AgPB_TEAM_CT &&
		       !m_Tasks.Contains( AgPBTaskId::DefuseBomb ) ) ||
		     ( team == AgPB_TEAM_T && FindOwnedWeapon( "weapon_c4" ) != NULL &&
		       !m_Tasks.Contains( AgPBTaskId::PlantBomb ) ) ||
			( team == AgPB_TEAM_T && bomb.state == AGPB_BOMB_DROPPED &&
			  !m_Tasks.Contains( AgPBTaskId::PickupBomb ) ) );
		const bool hostageUrgent = ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 && team == AgPB_TEAM_CT &&
			!m_Tasks.Contains( AgPBTaskId::RescueHostage ) && !m_Tasks.Contains( AgPBTaskId::EscortHostages );
		if ( ( bombUrgent || hostageUrgent ) &&
		     ( escape || m_Tasks.Current().desire < AgPBTaskPriority::Objective ) ) PlanNormalTask( globals, true );
	}
	AgPBTask task = m_Tasks.Current();
	if ( task.id != AgPBTaskId::DestroyBreakable && m_uBreakTaskToken != 0 ) ClearBreakable();
	if ( task.id != AgPBTaskId::DestroyBreakable && m_uRouteTaskToken != task.token &&
	     ( HasRoute() || m_bRouteArrived ) ) ClearRoute();
	if ( task.token != m_uObservedTaskToken )
	{
		m_uObservedTaskToken = task.token;
		m_flTaskActionStarted = globals->curtime;
		ResetTaskUse();
		RouteReport( "task -> %s (wp=%d, priority=%.0f)", AgPBTaskName( task.id ), task.waypoint, task.desire );
	}
	if ( task.id == AgPBTaskId::DestroyBreakable )
	{
		if ( !UpdateBreakable( globals, cmd ) ) CompleteCurrentTask( "breakable state released" );
		return;
	}
	if ( task.id == AgPBTaskId::Camp || task.id == AgPBTaskId::Pause )
	{
		const AgPBPath *node = BotWaypoints().Get( task.waypoint );
		if ( node != NULL && ( node->flags & AgPB_WP_CROUCH ) != 0 ) cmd.buttons |= IN_DUCK;
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
	if ( task.id == AgPBTaskId::MoveToPosition )
	{
		if ( !HasRoute() && !m_bRouteArrived )
		{
			char error[192];
			if ( !BuildRoute( task.waypoint, error, sizeof( error ) ) ) { CompleteCurrentTask( error, true ); return; }
			m_uRouteTaskToken = task.token;
		}
		if ( HasRoute() ) UpdateRoute( globals, cmd );
		if ( !HasRoute() && m_Tasks.Current().token == task.token )
		{
			const bool arrived = m_bRouteArrived;
			CompleteCurrentTask( arrived ? "waypoint reached" : "manual route failed", !arrived );
			const AgPBPath *node = BotWaypoints().Get( task.waypoint );
			if ( arrived && node != NULL && ( ( node->flags & AgPB_WP_CAMP ) != 0 || m_bAutonomous ) )
			{
				AgPBTask camp; camp.id = AgPBTaskId::Camp; camp.desire = AgPBTaskPriority::Camp;
				camp.waypoint = task.waypoint;
				const float minimum = MAX( agpb_camp_min.GetFloat(), 1.0f );
				camp.deadline = globals->curtime + RandomFloat( minimum, MAX( agpb_camp_max.GetFloat(), minimum ) );
				m_Tasks.Push( camp );
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
	for ( int i = 0; i < m_Tasks.Count(); ++i )
	{
		const AgPBTask *task = m_Tasks.At( i );
		RouteReport( "  %s %s wp=%d priority=%.0f deadline=%.1f token=%llu entity=%d",
			m_Tasks.State( task->token ) == AgPBTaskState::Active ? "active" : "suspended",
			AgPBTaskName( task->id ), task->waypoint, task->desire, task->deadline,
			(unsigned long long)task->token, task->hasEntity ? NetVar_HandleEntry( (uintp)task->entityHandle ) : -1 );
	}
}
