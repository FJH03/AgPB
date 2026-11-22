// AgPB normal-mode cover. GPL-3.0-only.
// SyPB 4c364fb source/navigate.cpp FindCoverWaypoint and source/combat.cpp
// Remove-From-Combat behavior: prefer covered points the bot can reach first.
// Copyright (c) 2003-2023 HsK-Dev Blog.
// Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
// The original permission notice is retained in bot_combat.cpp.
#include <algorithm>
#include <cmath>
#include <vector>
#include <bspflags.h>
#include <const.h>
#include <gametrace.h>
#include <in_buttons.h>
#include "bot.h"
#include "objectives.h"
#include "waypoint.h"

extern bool AgPB_IsRoundFrozen();
extern ConVar agpb_combat;

namespace
{
constexpr float CoverRetry = 1.0f;
constexpr float CoverCooldown = 4.0f;
constexpr float CoverTaskSeconds = 8.0f;
constexpr float CoverDwell = 0.75f;
constexpr float CoverReloadDwell = 3.25f;
constexpr float CoverGrenadeSettle = 0.2f;
constexpr float EnemyAnchorRange = 192.0f;
constexpr std::size_t CoverAttempts = 24;

struct CoverCandidate
{
    int waypoint;
    float distance;
};

bool IsGroundCoverNode( const AgPBPath *node )
{
    return node != NULL && node->origin.IsValid() &&
        ( node->flags & ( AgPB_WP_LADDER | AgPB_WP_LIFT ) ) == 0;
}

bool GroundCoverRoute( const CAgPBWaypoints &waypoints, const CUtlVector<int> &route )
{
    if ( route.Count() == 0 ) return false;
    for ( int i = 0; i < route.Count(); ++i )
    {
        if ( !IsGroundCoverNode( waypoints.Get( route[i] ) ) ) return false;
        unsigned int flags = 0;
        if ( i > 0 && ( !waypoints.IsConnected( route[i - 1], route[i], &flags ) ||
             ( flags & ( AgPB_PATH_JUMP | AgPB_PATH_DOUBLE | AgPB_PATH_VISIBLE ) ) != 0 ) ) return false;
    }
    return true;
}

bool HasStaticCover( IEngineTrace &trace, const Vector &source, const Vector &destination )
{
    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    ray.Init( source, destination );
    trace_t hit;
    trace.TraceRay( ray, CONTENTS_SOLID, &filter, &hit );
    return !hit.startsolid && !hit.allsolid && hit.fraction < 0.99f;
}
}

bool CAgPB::TryStartCover( CGlobalVars *globals, bool reloading, bool withdrawing )
{
    const AgPBTask &current = m_Tasks.Current();
    if ( globals == NULL || !m_bAutonomous || m_pInfo == NULL || m_Ctx.pTrace == NULL ||
         !IsAlive() || !m_bCombatEnabled || !agpb_combat.GetBool() ||
         AgPB_GameMode() != AgPB_MODE_NORMAL || AgPB_IsRoundFrozen() ||
         ( GetNetVarInt( "m_fFlags" ) & ( FL_ONGROUND | FL_FROZEN ) ) != FL_ONGROUND ||
         GetNetVarInt( "movetype", MOVETYPE_NONE ) != MOVETYPE_WALK ||
         GetNetVarInt( "m_nWaterLevel" ) >= 2 || m_Ladder.Active() || m_bPendingJumpLaunch ||
         IsHoldingTaskPosition() || !current.canResume || current.desire >= AgPBTaskPriority::SeekCover ||
         m_AgentIntent.token != 0 || m_Tasks.Contains( AgPBTaskId::AgentIntention ) ) return false;

    const float now = globals->curtime;
    const AgPBSearch::Observation &sight = m_Search.visual;
    const AgPBObjectiveBomb &bomb = BotObjectives().Snapshot().bomb;
    const bool planted = bomb.state == AGPB_BOMB_PLANTED ||
        ( bomb.state == AGPB_BOMB_UNKNOWN && bomb.plantedObserved );
    const bool urgent = reloading || ( m_pInfo->GetHealth() <= 30 && !planted );
    if ( now < m_flNextCoverAt || ( !urgent && ( !withdrawing || planted ) ) ||
         !AgPBSearch::Alive( sight, now ) || now - sight.observedAt > AgPBSearch::VisualLookTTL ||
         !IsCombatEnemy( HandleToEdict( (uintp)sight.handle ), sight.userId ) ) return false;

    CAgPBWaypoints &waypoints = BotWaypoints();
    unsigned int incoming = 0, outgoing = 0;
    if ( m_iRouteIndex > 0 )
        waypoints.IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &incoming );
    waypoints.IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &outgoing );
    const AgPBPath *activeNode = waypoints.Get( RouteNode( m_iRouteIndex ) );
    if ( ( ( incoming | outgoing ) & ( AgPB_PATH_JUMP | AgPB_PATH_DOUBLE ) ) != 0 ||
         ( activeNode != NULL && !IsGroundCoverNode( activeNode ) ) ) return false;

    m_flNextCoverAt = now + CoverRetry;
    const Vector here = m_pInfo->GetAbsOrigin();
    const Vector enemyOrigin( sight.position.x, sight.position.y, sight.position.z );
    const Vector threatEye = enemyOrigin + Vector( 0.0f, 0.0f, AgPB_HullStandHeight() - 8.0f );
    const Vector observedPoint( sight.look.x, sight.look.y, sight.look.z );
    if ( !here.IsValid() ) return false;
    // SyPB bounds close escapes at 512 and permits a wider 1024 cover search
    // in its other enemy-action path. The observed distance limits either.
    const float budget = clamp( ( enemyOrigin - here ).Length(), 300.0f, urgent ? 512.0f : 1024.0f );
    const int team = m_pInfo->GetTeamIndex();
    const int enemyTeam = team == AgPB_TEAM_T ? AgPB_TEAM_CT : AgPB_TEAM_T;
    const bool ducking = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
    int entry = -1, enemyEntry = -1;
    float entryDistance = budget, enemyEntryDistance = EnemyAnchorRange;
    for ( int i = 0; i < waypoints.Count(); ++i )
    {
        const AgPBPath *node = waypoints.Get( i );
        if ( !IsGroundCoverNode( node ) ) continue;
        const float fromEnemy = ( node->origin - enemyOrigin ).Length();
        if ( fromEnemy < enemyEntryDistance && waypoints.IsPassable( i, enemyTeam ) &&
             !HasStaticCover( *m_Ctx.pTrace, threatEye, node->origin + Vector( 0.0f, 0.0f, 32.0f ) ) )
        {
            enemyEntry = i;
            enemyEntryDistance = fromEnemy;
        }
        const float fromBot = ( node->origin - here ).Length();
        if ( fromBot < entryDistance && waypoints.IsPassable( i, team ) &&
             CanWalkToTaskPosition( node->origin, ducking || NavigationRequiresCrouch( node->flags ) ) )
        {
            entry = i;
            entryDistance = fromBot;
        }
    }
    if ( entry < 0 || enemyEntry < 0 ) return false;

    CUtlVector<float> ourDistances, enemyDistances;
    CUtlVector<short> parents;
    waypoints.ComputeDistances( entry, ourDistances, parents, team );
    waypoints.ComputeDistances( enemyEntry, enemyDistances, parents, enemyTeam );

    const AgPBPath *enemyNode = waypoints.Get( enemyEntry );
    CUtlVector<Vector> nearbyThreats;
    for ( int slot = 0; slot < AgPB_WP_MAX_PATH_INDEX; ++slot )
    {
        const int neighbor = enemyNode->index[slot];
        const AgPBPath *node = waypoints.Get( neighbor );
        if ( IsGroundCoverNode( node ) && waypoints.IsPassable( neighbor, enemyTeam ) &&
             ( enemyNode->connectionFlags[slot] & ( AgPB_PATH_JUMP | AgPB_PATH_DOUBLE ) ) == 0 &&
             ( node->origin - enemyOrigin ).LengthSqr() <= EnemyAnchorRange * EnemyAnchorRange )
            nearbyThreats.AddToTail( node->origin + Vector( 0.0f, 0.0f, AgPB_HullStandHeight() - 8.0f ) );
    }

    std::vector<CoverCandidate> candidates;
    for ( int i = 0; i < waypoints.Count(); ++i )
    {
        const AgPBPath *node = waypoints.Get( i );
        const float distance = ourDistances[i] + entryDistance;
        if ( i != entry && IsGroundCoverNode( node ) && distance <= budget &&
             distance < enemyDistances[i] )
            candidates.push_back( { i, distance } );
    }
    std::sort( candidates.begin(), candidates.end(), []( const CoverCandidate &a, const CoverCandidate &b ) {
        return a.distance == b.distance ? a.waypoint < b.waypoint : a.distance < b.distance;
    } );
    for ( std::size_t i = 0; i < candidates.size() && i < CoverAttempts; ++i )
    {
        const CoverCandidate &candidate = candidates[i];
        const AgPBPath *node = waypoints.Get( candidate.waypoint );
        const Vector head = node->origin + Vector( 0.0f, 0.0f, AgPB_HullStandHeight() - 8.0f );
        const Vector torso = node->origin + Vector( 0.0f, 0.0f, AgPB_HullStandHeight() * 0.5f );
        if ( !HasStaticCover( *m_Ctx.pTrace, threatEye, head ) ||
             !HasStaticCover( *m_Ctx.pTrace, threatEye, torso ) ||
             !HasStaticCover( *m_Ctx.pTrace, observedPoint, head ) ) continue;
        bool exposed = false;
        for ( int neighbor = 0; neighbor < nearbyThreats.Count() && !exposed; ++neighbor )
            exposed = !HasStaticCover( *m_Ctx.pTrace, nearbyThreats[neighbor], head );
        if ( exposed ) continue;

        CUtlVector<int> route;
        char error[160];
        if ( !FindRouteCandidate( candidate.waypoint, route, error, sizeof( error ) ) ||
             !GroundCoverRoute( waypoints, route ) || route[0] != entry ) continue;

        AgPBTask task;
        task.id = AgPBTaskId::SeekCover;
        task.desire = AgPBTaskPriority::SeekCover;
        task.waypoint = candidate.waypoint;
        task.deadline = now + CoverTaskSeconds;
        task.canResume = false;
        const std::uint64_t token = m_Tasks.Push( task );
        if ( token == 0 || m_Tasks.Current().token != token ) return false;

        // Keep the verified ground route; rebuilding could choose another entry.
        ClearRoute();
        m_vecRoute = route;
        m_iRouteIndex = 0;
        m_iGoalWaypoint = task.waypoint;
        m_uRouteTaskToken = token;
        ResetRouteProgress( now );
        m_iCoverEnemyUserId = sight.userId;
        m_flCoverReachedAt = 0.0f;
        m_flNextCoverAt = now + CoverCooldown;
        m_uCoverReloadWeapon = (uintp)INVALID_EHANDLE_INDEX;
        m_iCoverReloadClip = -1;
        if ( reloading )
        {
            uintp held;
            bool knife;
            int clip, reserve;
            if ( CurrentCombatWeapon( held, knife, clip, reserve ) != NULL && !knife && clip >= 0 && reserve > 0 )
            {
                m_uCoverReloadWeapon = held;
                m_iCoverReloadClip = clip;
            }
        }
        m_bCombatReloadRequested = false;
        RouteReport( "seek cover: waypoint #%d enemy=%d travel=%.0f reason=%s", task.waypoint, sight.userId,
            candidate.distance, reloading ? "reload" : urgent ? "low-health" : "withdraw" );
        return true;
    }
    return false;
}

edict_t *CAgPB::CoverReloadWeapon() const
{
    uintp held;
    bool knife;
    int clip, reserve;
    edict_t *weapon = CurrentCombatWeapon( held, knife, clip, reserve );
    if ( weapon == NULL || knife ) return NULL;
    if ( AgPBWeaponSelect::Shotgun( ClassifyWeapon( weapon ) ) && AgPB_ReadNetVarInt( weapon, "m_reloadState" ) != 0 )
        return weapon;
    const bool requested = held == m_uCoverReloadWeapon && clip >= 0 && clip <= m_iCoverReloadClip;
    return reserve > 0 && ( clip == 0 || requested ) ? weapon : NULL;
}

bool CAgPB::CanThrowFromCover( float now ) const
{
    const AgPBTask &task = m_Tasks.Current();
    return task.id == AgPBTaskId::SeekCover && m_uRouteTaskToken == task.token &&
        m_bRouteArrived && !HasRoute() && m_flCoverReachedAt > 0.0f &&
        now - m_flCoverReachedAt >= CoverGrenadeSettle && now < task.deadline &&
        GetNetVarInt( "m_bDucking" ) == 0 && CoverReloadWeapon() == NULL;
}

void CAgPB::UpdateCoverTask( CGlobalVars *globals, CBotCmd &cmd )
{
    const AgPBTask task = m_Tasks.Current();
    if ( task.id != AgPBTaskId::SeekCover || globals == NULL ) return;
    if ( !m_bCombatEnabled || !agpb_combat.GetBool() || AgPB_GameMode() != AgPB_MODE_NORMAL )
    {
        CompleteCurrentTask( "cover combat disabled" );
        return;
    }
    const AgPBSearch::Observation &sight = m_Search.visual;
    edict_t *enemy = sight.valid && sight.userId == m_iCoverEnemyUserId
        ? HandleToEdict( (uintp)sight.handle ) : NULL;
    if ( !IsCombatEnemy( enemy, m_iCoverEnemyUserId ) )
    {
        CompleteCurrentTask( "cover threat unavailable" );
        return;
    }
    if ( HasRoute() )
    {
        if ( m_uRouteTaskToken != task.token || !GroundCoverRoute( BotWaypoints(), m_vecRoute ) )
        {
            CompleteCurrentTask( "cover path lost ground route", true );
            return;
        }
        UpdateRoute( globals, cmd );
        if ( m_Tasks.Current().token != task.token ) return;
        if ( HasRoute() )
        {
            if ( cmd.weaponselect == 0 &&
                 ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP ) ) == 0 &&
                 ( cmd.forwardmove != 0.0f || cmd.sidemove != 0.0f ) )
                UpdateCoverFire( globals, cmd, enemy, m_iCoverEnemyUserId );
            return;
        }
    }

    const float now = globals->curtime;
    const AgPBPath *node = BotWaypoints().Get( task.waypoint );
    if ( !m_bRouteArrived || m_uRouteTaskToken != task.token || !IsGroundCoverNode( node ) )
    {
        m_flNextCoverAt = MAX( m_flNextCoverAt, now + CoverRetry );
        CompleteCurrentTask( "cover route unavailable", true );
        return;
    }
    if ( m_flCoverReachedAt == 0.0f )
    {
        m_flCoverReachedAt = now;
        // Recheck when settled instead of losing the short cover window to
        // a prior routine LOS check. An actual throw/cancellation keeps its cooldown.
        m_flNextGrenadeCheck = MAX( m_flGrenadeRetryUntil, MIN( m_flNextGrenadeCheck, now + CoverGrenadeSettle ) );
    }
    UpdateEnemyMemoryLook( globals, cmd );
    if ( ( node->flags & AgPB_WP_CROUCH ) != 0 ) cmd.buttons |= IN_DUCK;

    edict_t *weapon = CoverReloadWeapon();
    const bool needsReload = weapon != NULL;
    // A shell reload advances and finishes through the native weapon's idle path.
    if ( needsReload && AgPB_ReadNetVarInt( weapon, "m_reloadState" ) == 0 )
    {
        const float playerReady = GetNetVarFloat( "m_flNextAttack", -1.0f );
        const float weaponReady = AgPB_ReadNetVarFloat( weapon, "m_flNextPrimaryAttack", -1.0f );
        if ( playerReady >= 0.0f && weaponReady >= 0.0f && now >= playerReady && now >= weaponReady )
            cmd.buttons |= IN_RELOAD;
    }
    if ( now - m_flCoverReachedAt < ( needsReload ? CoverReloadDwell : CoverDwell ) ) return;
    m_flNextCoverAt = MAX( m_flNextCoverAt, now + CoverCooldown );
    CompleteCurrentTask( "cover settled" );
}
