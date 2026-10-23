// AgPB optional agent waypoint intentions. GPL-3.0-only.
// SyPB-derived local task arbitration remains in charge of navigation/actions;
// CS-EBOT's absolute process deadlines were compared. An external agent only
// leases one waypoint goal. Local objectives, escape, combat and clearing keep
// their existing priorities and CUserCmd execution paths.
#include <tier1/strtools.h>
#include <cmath>
#include "plugin.h"
#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "waypoint.h"

extern CGlobalVars *gpGlobals;
extern unsigned int AgPB_MapSerial();

bool CAgPB::SetAgentIntent( bool camp, int waypoint, float ttl, char *error, size_t maxlen )
{
    auto reject = [&]( const char *why ) {
        if ( error != NULL && maxlen != 0 ) Q_strncpy( error, why, (int)maxlen );
        return false;
    };
    if ( gpGlobals == NULL || !std::isfinite( gpGlobals->curtime ) )
        return reject( "game time is unavailable" );
    UpdateAgentIntent( gpGlobals );
    if ( !m_bAutonomous ) return reject( "normal autonomy is disabled; enable it explicitly first" );
    if ( AgPB_GameMode() != AgPB_MODE_NORMAL ) return reject( "agent intentions require normal mode" );
    if ( !IsAlive() || m_pInfo == NULL ) return reject( "bot is not alive" );
    if ( !std::isfinite( ttl ) || ttl < 0.5f || ttl > 30.0f )
        return reject( "TTL must be finite and within 0.5..30 seconds" );
    const AgPBPath *node = BotWaypoints().Get( waypoint );
    if ( node == NULL || !node->origin.IsValid() || !BotWaypoints().IsPassable( waypoint, m_pInfo->GetTeamIndex() ) )
        return reject( "waypoint is invalid or forbidden for this team" );
    const float deadline = gpGlobals->curtime + ttl;
    if ( !std::isfinite( deadline ) ) return reject( "lease deadline is invalid" );
    if ( m_AgentIntent.token != 0 && m_AgentIntent.camp == camp && m_AgentIntent.waypoint == waypoint &&
         m_Tasks.RenewDeadline( m_AgentIntent.token, deadline ) )
    {
        // A heartbeat must not replace the task token, clear its hop, reset
        // arrival or disturb a currently active higher-priority objective.
        m_AgentIntent.deadline = deadline;
        Q_strncpy( m_AgentIntent.reason, "renewed", sizeof( m_AgentIntent.reason ) );
        return true;
    }
    ClearAgentIntent( "replaced" );
    AgPBTask task;
    task.id = AgPBTaskId::AgentIntention;
    task.desire = AgPBTaskPriority::AgentIntention;
    task.waypoint = waypoint;
    task.deadline = deadline;
    task.canResume = true;
    m_AgentIntent.token = m_Tasks.Push( task );
    if ( m_AgentIntent.token == 0 ) return reject( "agent task was not retained" );
    m_AgentIntent.mapSerial = AgPB_MapSerial();
    m_AgentIntent.roundEpoch = BotObjectives().Snapshot().roundEpoch;
    m_AgentIntent.waypoint = waypoint;
    m_AgentIntent.team = m_pInfo->GetTeamIndex();
    m_AgentIntent.deadline = deadline;
    m_AgentIntent.camp = camp;
    m_AgentIntent.arrived = false;
    Q_strncpy( m_AgentIntent.reason, "accepted", sizeof( m_AgentIntent.reason ) );
    RouteReport( "agent %s accepted: waypoint #%d token=%llu deadline=%.3f; local autonomy stays enabled",
        camp ? "camp" : "move", waypoint, (unsigned long long)m_AgentIntent.token, deadline );
    return true;
}

void CAgPB::ClearAgentIntent( const char *reason )
{
    const std::uint64_t token = m_AgentIntent.token;
    const bool active = token != 0 && m_Tasks.Current().token == token;
    const bool ownsRoute = token != 0 && m_uRouteTaskToken == token;
    AgentIntentState empty;
    Q_strncpy( empty.reason, reason != NULL ? reason : "cleared", sizeof( empty.reason ) );
    m_AgentIntent = empty;
    if ( token == 0 ) return;
    // ClearRoute also removes the breakable child that belongs to this route.
    // A suspended lease must never cancel an objective's different route.
    if ( ownsRoute ) ClearRoute();
    if ( ownsRoute || m_uCombatOwnerToken == token ) ClearCombat( "agent-released" );
    m_Tasks.RemoveToken( token );
    if ( active || ownsRoute )
        m_flNextTaskPlan = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
    RouteReport( "agent intention released: %s token=%llu; local task=%s",
        m_AgentIntent.reason, (unsigned long long)token, AgPBTaskName( m_Tasks.Current().id ) );
}

void CAgPB::UpdateAgentIntent( CGlobalVars *globals )
{
    if ( m_AgentIntent.token == 0 ) return;
    if ( globals == NULL || !std::isfinite( globals->curtime ) )
    { ClearAgentIntent( "time-unavailable" ); return; }
    if ( !m_bAutonomous || AgPB_GameMode() != AgPB_MODE_NORMAL )
    { ClearAgentIntent( "local-mode-changed" ); return; }
    if ( !IsAlive() || m_pInfo == NULL || m_pInfo->GetTeamIndex() != m_AgentIntent.team )
    { ClearAgentIntent( "bot-lifecycle" ); return; }
    if ( m_AgentIntent.mapSerial != AgPB_MapSerial() ||
         m_AgentIntent.roundEpoch != BotObjectives().Snapshot().roundEpoch )
    { ClearAgentIntent( "scope-changed" ); return; }
    if ( !std::isfinite( m_AgentIntent.deadline ) || globals->curtime >= m_AgentIntent.deadline )
    { ClearAgentIntent( "expired" ); return; }
    const AgPBPath *node = BotWaypoints().Get( m_AgentIntent.waypoint );
    if ( node == NULL || !node->origin.IsValid() || !BotWaypoints().IsPassable( m_AgentIntent.waypoint, m_AgentIntent.team ) )
    { ClearAgentIntent( "waypoint-unavailable" ); return; }
    const AgPBTaskState state = m_Tasks.State( m_AgentIntent.token );
    if ( state == AgPBTaskState::Missing )
    { ClearAgentIntent( "task-ended" ); return; }
    // A local objective may move a camping bot away. Resume by navigating
    // back through the same authored graph, rather than camping at that new spot.
    if ( state == AgPBTaskState::Suspended ) m_AgentIntent.arrived = false;
}

void CAgPB::ReportAgentIntent() const
{
    const float now = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
    const AgPBTaskState taskState = m_Tasks.State( m_AgentIntent.token );
    const char *state = m_AgentIntent.token == 0 ? "none" :
        now >= m_AgentIntent.deadline ? "expired" :
        taskState == AgPBTaskState::Suspended ? "suspended" :
        taskState == AgPBTaskState::Missing ? "ended" :
        m_AgentIntent.arrived ? "camping" : "active";
    RouteReport( "agent v=1 userid=%d mapserial=%u round=%u lease-map=%u lease-round=%u token=%llu mode=%s wp=%d deadline=%.3f remaining=%.3f state=%s reason=%s auto=%d route-owner=%llu",
        m_pInfo != NULL ? m_pInfo->GetUserID() : -1, AgPB_MapSerial(), BotObjectives().Snapshot().roundEpoch,
        m_AgentIntent.mapSerial, m_AgentIntent.roundEpoch, (unsigned long long)m_AgentIntent.token,
        m_AgentIntent.token == 0 ? "none" : m_AgentIntent.camp ? "camp" : "move", m_AgentIntent.waypoint,
        m_AgentIntent.deadline, m_AgentIntent.token != 0 ? MAX( 0.0f, m_AgentIntent.deadline - now ) : 0.0f,
        state, m_AgentIntent.reason, m_bAutonomous ? 1 : 0, (unsigned long long)m_uRouteTaskToken );
}
