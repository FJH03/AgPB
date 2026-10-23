/*
 * AgPB local normal-mode goals and idle observation. GPL-3.0-only.
 * Adapted from SyPB source/navigate.cpp FindGoal, source/control.cpp
 * personality initialization and source/basecode.cpp TASK_NORMAL objective
 * guards / FindDefendWaypoint and TASK_CAMP observation.
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 * The original permission notice is retained in local_planner.h.
 * Compared with the same CS-EBOT source files; ZM/ZE branches are excluded.
 */
#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <in_buttons.h>
#include <vstdlib/random.h>
#include <algorithm>
#include <vector>
#include "bot.h"
#include "local_planner.h"
#include "observation_look.h"
#include "objectives.h"
#include "waypoint.h"

extern CGlobalVars *gpGlobals;

namespace
{
struct FriendlyOccupancy
{
    Vector position;
    float weight;
};

float RegionDistance( const Vector &point, const AgPBObjectiveRegion &region )
{
    if ( region.boundsKnown )
    {
        Vector nearest;
        for ( int i = 0; i < 3; ++i ) nearest[i] = clamp( point[i], region.worldMins[i], region.worldMaxs[i] );
        return ( point - nearest ).Length();
    }
    return region.positionKnown ? ( point - region.center ).Length() : AgPB_WP_DIST_INF;
}

float ObjectiveDistance( const Vector &position, const AgPBObjectiveSnapshot &world )
{
    float distance = AgPB_WP_DIST_INF;
    if ( ( world.mapFlags & AGPB_MAP_BOMB ) != 0 )
        for ( int i = 0; i < world.bombSites.Count(); ++i )
            distance = MIN( distance, RegionDistance( position, world.bombSites[i] ) );
    if ( ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 )
        for ( int i = 0; i < world.hostages.Count(); ++i )
        {
            const AgPBObjectiveHostage &hostage = world.hostages[i];
            if ( !hostage.positionKnown || hostage.rescued || ( hostage.aliveKnown && !hostage.alive ) ) continue;
            distance = MIN( distance, ( position - hostage.position ).Length() );
        }
    return distance;
}

bool IsSniperWeapon( edict_t *weapon )
{
    if ( weapon == NULL || weapon->IsFree() ) return false;
    const char *name = weapon->GetClassName();
    return name != NULL && ( Q_stricmp( name, "weapon_awp" ) == 0 ||
        Q_stricmp( name, "weapon_scout" ) == 0 || Q_stricmp( name, "weapon_g3sg1" ) == 0 ||
        Q_stricmp( name, "weapon_sg550" ) == 0 );
}

const char *PersonalityName( AgPBLocalPersonality personality )
{
    return personality == AgPBLocalPersonality::Rusher ? "rusher" :
        personality == AgPBLocalPersonality::Careful ? "careful" : "normal";
}

float Toward( float current, float target, float amount )
{
    return current < target ? MIN( current + amount, target ) : MAX( current - amount, target );
}
}

void CAgPB::ResetAutonomy( float now )
{
    const bool initialized = m_Autonomy.initialized;
    const AgPBLocalPersonality personality = m_Autonomy.personality;
    const float aggression = m_Autonomy.baseAggression, fear = m_Autonomy.baseFear;
    m_Autonomy = AgPBLocalPlannerState();
    if ( initialized )
    {
        m_Autonomy.initialized = true;
        m_Autonomy.personality = personality;
        m_Autonomy.aggression = m_Autonomy.baseAggression = aggression;
        m_Autonomy.fear = m_Autonomy.baseFear = fear;
    }
    else
        AgPB_LocalInitializePersonality( m_Autonomy,
            static_cast<AgPBLocalPersonality>( RandomInt( 0, 2 ) ), static_cast<std::uint32_t>( RandomInt( 1, 0x7fffffff ) ) );
    m_Autonomy.nextEmotionUpdate = now + 0.5f;
    m_Autonomy.nextScan = now;
}

void CAgPB::RememberAutonomyGoal( int waypoint, float now )
{
    AgPB_LocalRememberGoal( m_Autonomy, waypoint, now );
}

bool CAgPB::PlanAutonomousPatrol( CGlobalVars *globals )
{
    if ( globals == NULL || !m_bAutonomous || !IsAlive() || m_pInfo == NULL ||
         m_Tasks.Current().id != AgPBTaskId::Normal ||
         ( GetNetVarInt( "m_fFlags" ) & ( FL_ONGROUND | FL_FROZEN ) ) != FL_ONGROUND ) return false;
    if ( !m_Autonomy.initialized ) ResetAutonomy( globals->curtime );
    CAgPBWaypoints &wp = BotWaypoints();
    const int team = m_pInfo->GetTeamIndex();
    const Vector here = m_pInfo->GetAbsOrigin();
    int start = -1;
    float entryDistance = 512.0f;
    // Use a supported, team-passable entry. The nearest Euclidean node can
    // be across a wall or carry an access restriction for the other team.
    for ( int i = 0; i < wp.Count(); ++i )
    {
        const AgPBPath *node = wp.Get( i );
        if ( node == NULL || !node->origin.IsValid() || !wp.IsPassable( i, team ) ) continue;
        const float distance = ( node->origin - here ).Length();
        if ( distance >= entryDistance || !CanWalkToTaskPosition( node->origin, ( node->flags & AgPB_WP_CROUCH ) != 0 ) ) continue;
        entryDistance = distance; start = i;
    }
    if ( start < 0 ) return false;
    CUtlVector<float> distances;
    CUtlVector<short> parents;
    wp.ComputeDistances( start, distances, parents, team );

    if ( globals->curtime >= m_Autonomy.nextEmotionUpdate )
    {
        const float recovery = 0.05f * clamp( 1.0f + ( globals->curtime - m_Autonomy.nextEmotionUpdate ) / 0.5f, 1.0f, 10.0f );
        m_Autonomy.aggression = Toward( m_Autonomy.aggression, m_Autonomy.baseAggression, recovery );
        m_Autonomy.fear = Toward( m_Autonomy.fear, m_Autonomy.baseFear, recovery );
        m_Autonomy.nextEmotionUpdate = globals->curtime + 0.5f;
    }
    const int health = m_pInfo->GetHealth();
    if ( m_Autonomy.lastHealth > 0 && health < m_Autonomy.lastHealth )
    {
        m_Autonomy.fear = MIN( 1.0f, m_Autonomy.fear + 0.2f );
        m_Autonomy.aggression = MAX( 0.0f, m_Autonomy.aggression - 0.1f );
    }
    m_Autonomy.lastHealth = health;

    std::vector<FriendlyOccupancy> friendly;
    if ( m_Ctx.pEngine != NULL && m_Ctx.pPlayerInfoManager != NULL )
        for ( int i = 1; i <= globals->maxClients; ++i )
        {
            edict_t *player = m_Ctx.pEngine->PEntityOfEntIndex( i );
            if ( player == NULL || player == m_pEdict || player->IsFree() ) continue;
            IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( player );
            if ( info == NULL || !info->IsConnected() || !info->IsPlayer() || info->IsHLTV() ||
                 info->IsDead() || info->IsObserver() || info->GetTeamIndex() != team ) continue;
            FriendlyOccupancy body = { info->GetAbsOrigin(), 1.0f };
            if ( body.position.IsValid() ) friendly.push_back( body );
        }
    CAgPBManager &bots = AgPB_Bots();
    for ( int i = 0; i < bots.Count(); ++i )
    {
        const CAgPB *other = bots.Get( i );
        if ( other == NULL || other == this || !other->IsAlive() || other->PlayerInfo() == NULL ||
             other->PlayerInfo()->GetTeamIndex() != team ) continue;
        const int routeGoal = other->HasRoute() ? other->RouteGoal() : -1;
        const AgPBPath *route = wp.Get( routeGoal );
        if ( route != NULL ) { FriendlyOccupancy reserved = { route->origin, 1.0f }; friendly.push_back( reserved ); }
        const AgPBTask &task = other->m_Tasks.Current();
        const AgPBPath *goal = wp.Get( task.waypoint );
        if ( goal != NULL && task.waypoint != routeGoal )
        { FriendlyOccupancy reserved = { goal->origin, 1.0f }; friendly.push_back( reserved ); }
        else if ( goal == NULL && task.hasPosition )
        {
            FriendlyOccupancy reserved = { Vector( task.position[0], task.position[1], task.position[2] ), 1.0f };
            if ( reserved.position.IsValid() ) friendly.push_back( reserved );
        }
    }

    const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
    AgPBLocalGoalContext context;
    context.currentWaypoint = start; context.now = globals->curtime;
    context.bombMap = ( world.mapFlags & AGPB_MAP_BOMB ) != 0;
    context.hostageMap = ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0;
    context.defending = ( context.bombMap && team == AgPB_TEAM_CT ) || ( context.hostageMap && team == AgPB_TEAM_T );
    uintp weaponHandle; bool knife; int clip, reserve;
    context.sniper = IsSniperWeapon( CurrentCombatWeapon( weaponHandle, knife, clip, reserve ) );
    std::vector<AgPBLocalGoalCandidate> candidates;
    candidates.reserve( wp.Count() );
    for ( int i = 0; i < wp.Count(); ++i )
    {
        const AgPBPath *node = wp.Get( i );
        if ( node == NULL || !node->origin.IsValid() || !wp.IsPassable( i, team ) || distances[i] >= AgPB_WP_DIST_INF ) continue;
        AgPBLocalGoalCandidate candidate;
        candidate.waypoint = i; candidate.pathDistance = distances[i];
        candidate.camp = ( node->flags & AgPB_WP_CAMP ) != 0;
        candidate.sniper = ( node->flags & AgPB_WP_SNIPER ) != 0;
        candidate.avoid = ( node->flags & AgPB_WP_AVOID ) != 0;
        const float objectiveDistance = ObjectiveDistance( node->origin, world );
        candidate.objective = ( node->flags & AgPB_WP_GOAL ) != 0 || objectiveDistance <= 96.0f;
        candidate.defendArea = context.defending && objectiveDistance <= 512.0f;
        // T/CT flags were consumed only by IsPassable above. Unlike SyPB's
        // strategic point lists, AgPB's flags mean hard access restrictions.
        for ( const FriendlyOccupancy &occupied : friendly )
        {
            const Vector delta = occupied.position - node->origin;
            if ( fabsf( delta.z ) >= AgPB_HullStandHeight() * 1.5f ) continue;
            candidate.occupancy += occupied.weight * clamp( 1.0f - delta.Length2D() / 256.0f, 0.0f, 1.0f );
        }
        candidates.push_back( candidate );
    }
    char error[192];
    for ( int attempt = 0; attempt < 4; ++attempt )
    {
        const AgPBLocalGoalChoice choice = AgPB_SelectLocalGoal( m_Autonomy, context, candidates,
            static_cast<std::uint32_t>( RandomInt( 1, 0x7fffffff ) ) );
        if ( choice.waypoint < 0 ) return false;
        if ( !BuildRoute( choice.waypoint, error, sizeof( error ) ) )
        {
            for ( AgPBLocalGoalCandidate &candidate : candidates )
                if ( candidate.waypoint == choice.waypoint ) candidate.reachable = false;
            continue;
        }
        AgPBTask task;
        task.id = AgPBTaskId::MoveToPosition; task.desire = AgPBTaskPriority::MoveToPosition;
        task.waypoint = choice.waypoint; task.deadline = globals->curtime + 90.0f; task.canResume = true;
        m_uRouteTaskToken = m_Tasks.Push( task );
        m_Autonomy.selectedWaypoint = choice.waypoint;
        m_Autonomy.selectedTactic = choice.tactic;
        m_Autonomy.selectedCamp = choice.camp;
        m_Autonomy.selectedAt = globals->curtime;
        RouteReport( "local %s: -> #%d (%d hops), personality=%s aggression=%.2f fear=%.2f camp=%s",
            AgPB_LocalTacticName( choice.tactic ), choice.waypoint, MAX( 0, RouteCount() - 1 ),
            PersonalityName( m_Autonomy.personality ), m_Autonomy.aggression, m_Autonomy.fear, choice.camp ? "yes" : "no" );
        return true;
    }
    return false;
}

bool CAgPB::ShouldCampAtGoal( int waypoint, float now ) const
{
    CAgPBWaypoints &wp = BotWaypoints();
    const AgPBPath *node = wp.Get( waypoint );
    if ( node == NULL || m_pInfo == NULL || !m_bAutonomous || !IsAlive() ||
         m_Autonomy.selectedWaypoint != waypoint || !m_Autonomy.selectedCamp ||
         now < m_Autonomy.selectedAt || now - m_Autonomy.selectedAt > 90.0f ||
         !AgPB_LocalCampReady( m_Autonomy, now ) ) return false;
    const int team = m_pInfo->GetTeamIndex();
    const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
    const bool defending = ( ( world.mapFlags & AGPB_MAP_BOMB ) != 0 && team == AgPB_TEAM_CT ) ||
        ( ( world.mapFlags & AGPB_MAP_HOSTAGE ) != 0 && team == AgPB_TEAM_T );
    const bool guard = m_Autonomy.selectedTactic == AgPBLocalTactic::Defense && defending &&
        ObjectiveDistance( node->origin, world ) <= 512.0f;
    bool authored = ( node->flags & AgPB_WP_CAMP ) != 0;
    if ( !authored && ( node->flags & AgPB_WP_SNIPER ) != 0 )
    {
        uintp weaponHandle; bool knife; int clip, reserve;
        authored = IsSniperWeapon( CurrentCombatWeapon( weaponHandle, knife, clip, reserve ) );
    }
    if ( !authored && !guard ) return false;
    // Recheck the live same-team occupancy at arrival, since another player
    // or reservation may have entered this area while the route was running.
    CGlobalVars *globals = gpGlobals;
    if ( globals == NULL || m_Ctx.pEngine == NULL || m_Ctx.pPlayerInfoManager == NULL ) return false;
    for ( int i = 1; i <= globals->maxClients; ++i )
    {
        edict_t *player = m_Ctx.pEngine->PEntityOfEntIndex( i );
        if ( player == NULL || player == m_pEdict || player->IsFree() ) continue;
        IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( player );
        if ( info == NULL || !info->IsConnected() || !info->IsPlayer() || info->IsHLTV() ||
             info->IsDead() || info->IsObserver() || info->GetTeamIndex() != team ) continue;
        const Vector delta = info->GetAbsOrigin() - node->origin;
        if ( fabsf( delta.z ) < AgPB_HullStandHeight() * 1.5f && delta.Length2D() < 128.0f ) return false;
    }
    CAgPBManager &bots = AgPB_Bots();
    for ( int i = 0; i < bots.Count(); ++i )
    {
        const CAgPB *other = bots.Get( i );
        if ( other == NULL || other == this || !other->IsAlive() || other->PlayerInfo() == NULL ||
             other->PlayerInfo()->GetTeamIndex() != team ) continue;
        const int goals[2] = { other->HasRoute() ? other->RouteGoal() : -1, other->m_Tasks.Current().waypoint };
        for ( int goal : goals )
        {
            const AgPBPath *reserved = wp.Get( goal );
            if ( reserved == NULL ) continue;
            const Vector delta = reserved->origin - node->origin;
            if ( fabsf( delta.z ) < AgPB_HullStandHeight() * 1.5f && delta.Length2D() < 192.0f ) return false;
        }
    }
    return true;
}

void CAgPB::UpdateIdleLook( CGlobalVars *globals, CBotCmd &cmd )
{
    if ( globals == NULL || m_pInfo == NULL || m_Ctx.pTrace == NULL || HasRoute() ||
         fabsf( cmd.forwardmove ) > 0.01f || fabsf( cmd.sidemove ) > 0.01f || fabsf( cmd.upmove ) > 0.01f ||
         ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_USE | IN_JUMP | IN_RELOAD ) ) != 0 ||
         ( GetNetVarInt( "m_fFlags" ) & ( FL_ONGROUND | FL_FROZEN ) ) != FL_ONGROUND ||
         GetNetVarInt( "movetype", MOVETYPE_NONE ) == MOVETYPE_LADDER ) return;
    const AgPBTask &task = m_Tasks.Current();
    if ( task.id != AgPBTaskId::Camp && task.id != AgPBTaskId::Pause &&
         !( task.id == AgPBTaskId::AgentIntention && m_AgentIntent.camp && m_AgentIntent.arrived ) ) return;
    const AgPBObservationLook::Angles current = AgPBObservationLook::Sanitize({ cmd.viewangles.x, cmd.viewangles.y });
    cmd.viewangles.x = current.pitch; cmd.viewangles.y = current.yaw; cmd.viewangles.z = 0.0f;
    if ( m_Autonomy.scanOwner != task.token )
    {
        m_Autonomy.scanOwner = task.token;
        m_Autonomy.nextScan = globals->curtime;
        m_Autonomy.lookWaypoint = -1;
    }
    Vector eyeOffset = GetNetVarVector( "m_vecViewOffset" );
    if ( eyeOffset.z <= 0.0f ) eyeOffset.z = AgPB_HullStandHeight() - 4.0f;
    const Vector eye = m_pInfo->GetAbsOrigin() + eyeOffset;
    if ( globals->curtime >= m_Autonomy.nextScan )
    {
        // SyPB's TASK_CAMP alternates authored observation directions every
        // 2-5 seconds. AGPW has no camp angle fields: choose other authored
        // nodes, retain their true height, and require a current clear trace.
        m_Autonomy.nextScan = globals->curtime + RandomFloat( 2.0f, 5.0f );
        struct LookCandidate { int waypoint; float score; Vector point; };
        std::vector<LookCandidate> targets;
        CAgPBWaypoints &wp = BotWaypoints();
        const int team = m_pInfo->GetTeamIndex();
        for ( int i = 0; i < wp.Count(); ++i )
        {
            const AgPBPath *node = wp.Get( i );
            if ( node == NULL || !wp.IsPassable( i, team ) || !node->origin.IsValid() || i == task.waypoint || i == m_Autonomy.lookWaypoint ) continue;
            Vector point = node->origin;
            point.z += ( ( node->flags & AgPB_WP_CROUCH ) != 0 ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() ) - 4.0f;
            const Vector direction = point - eye;
            if ( !AgPBObservationLook::ScanCandidate( direction.x, direction.y, direction.z, 1400.0f ) ) continue;
            QAngle angle; VectorAngles( direction, angle );
            LookCandidate candidate = { i, RandomFloat( 0.0f, 40.0f ) +
                fabsf( AngleDiff( angle.y, cmd.viewangles.y ) ) * 0.25f + MIN( direction.Length(), 768.0f ) * 0.02f, point };
            targets.push_back( candidate );
        }
        std::sort( targets.begin(), targets.end(), []( const LookCandidate &a, const LookCandidate &b ) { return a.score > b.score; } );
        const int limit = MIN( static_cast<int>( targets.size() ), 24 );
        bool found = false;
        for ( int i = 0; i < limit; ++i )
        {
            const LookCandidate &candidate = targets[i];
            if ( !AgPB_TraceClear( eye, candidate.point, m_pEdict ) ) continue;
            m_Autonomy.lookWaypoint = candidate.waypoint;
            for ( int axis = 0; axis < 3; ++axis ) m_Autonomy.lookPoint[axis] = candidate.point[axis];
            found = true; break;
        }
        if ( !found ) m_Autonomy.lookWaypoint = -1;
    }
    if ( m_Autonomy.lookWaypoint < 0 ) return;
    Vector direction = Vector( m_Autonomy.lookPoint[0], m_Autonomy.lookPoint[1], m_Autonomy.lookPoint[2] ) - eye;
    if ( direction.LengthSqr() < 1.0f ) return;
    QAngle desired; VectorAngles( direction, desired );
    const AgPBObservationLook::Angles view = AgPBObservationLook::Step(
        { cmd.viewangles.x, cmd.viewangles.y }, { desired.x, desired.y }, 120.0f, globals->frametime );
    cmd.viewangles.x = view.pitch;
    cmd.viewangles.y = view.yaw;
    cmd.viewangles.z = 0.0f;
}
