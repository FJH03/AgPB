// AgPB observed enemy memory and bounded normal-mode investigation.
// GPL-3.0-only. SyPB source/basecode.cpp last-enemy/ACTIONFORENEMY and CAMP,
// source/navigate.cpp SetLastEnemy/goal selection, source/combat.cpp visibility
// are the behavior baseline; the matching CS-EBOT files were compared.
// Copyright (c) 2003-2023 HsK-Dev Blog.
// Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
// The complete original permission notice is retained in bot_combat.cpp.
// AgPB deliberately snapshots sensed positions: legacy hidden-entity tracking,
// wall shooting and zombie-specific pursuit are not included.
#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <in_buttons.h>
#include <gametrace.h>
#include <iservernetworkable.h>
#include <vstdlib/random.h>
#include <cfloat>
#include "bot.h"
#include "plugin.h"
#include "game_mode.h"
#include "waypoint.h"
#include "search_policy.h"
#include "observation_look.h"

extern ConVar agpb_combat;
extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();

namespace {
AgPBSearch::Point SavePoint(const Vector &point) { return {point.x, point.y, point.z}; }
Vector LoadPoint(const AgPBSearch::Point &point) { return Vector(point.x, point.y, point.z); }

class SearchTraceFilter : public CTraceFilter {
public:
    explicit SearchTraceFilter(IHandleEntity *self) : m_Self(self) {}
    bool ShouldHitEntity(IHandleEntity *entity, int) { return entity != m_Self; }
private:
    IHandleEntity *m_Self;
};

bool VisibleLookPoint(const BotEngineContext &ctx, edict_t *self,
                      const Vector &eye, const Vector &point) {
    if (ctx.pTrace == NULL || self == NULL || self->IsFree() || self->GetUnknown() == NULL) return false;
    IServerNetworkable *network = self->GetNetworkable();
    if (network == NULL || network->GetEntityHandle() == NULL) return false;
    SearchTraceFilter filter(network->GetEntityHandle());
    Ray_t ray; ray.Init(eye, point);
    trace_t hit; ctx.pTrace->TraceRay(ray, MASK_SOLID, &filter, &hit);
    return !hit.startsolid && !hit.allsolid && hit.fraction >= 0.99f;
}

void TurnToSnapshot(const Vector &eye, const Vector &point, float interval, CBotCmd &cmd) {
    const AgPBObservationLook::Angles current = AgPBObservationLook::Sanitize({cmd.viewangles.x, cmd.viewangles.y});
    cmd.viewangles.x = current.pitch; cmd.viewangles.y = current.yaw; cmd.viewangles.z = 0.0f;
    const Vector direction = point - eye;
    if (!point.IsValid() || direction.LengthSqr() < 1.0f) return;
    QAngle wanted; VectorAngles(direction, wanted);
    // This is an idle observation turn, separate from the five SyPB combat
    // difficulty/aim branches. It never claims attack or changes movement.
    const AgPBObservationLook::Angles view = AgPBObservationLook::Step(
        {cmd.viewangles.x, cmd.viewangles.y}, {wanted.x, wanted.y}, 180.0f, interval);
    cmd.viewangles.x = view.pitch;
    cmd.viewangles.y = view.yaw;
    cmd.viewangles.z = 0.0f;
}
}

void CAgPB::ResetSearch() {
    m_Search = AgPBSearch::State();
}

void CAgPB::RememberVisibleEnemy(edict_t *target, const Vector &point, float now) {
    // Caller has just proved current LOS. No hidden-target update path calls
    // this function, and hearing has a separate observation record.
    if (!IsCombatEnemy(target) || !point.IsValid()) return;
    IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo(target);
    if (info == NULL) return;
    AgPBSearch::Observe(m_Search.visual, AgPB_RefEHandle(target), info->GetUserID(),
        SavePoint(info->GetAbsOrigin()), SavePoint(point), now, AgPBSearch::VisualTTL);
}

void CAgPB::OnPerceptionEvent(edict_t *source, const Vector &eventOrigin,
                             float now, float radius, float ttl) {
    if (!m_bCombatEnabled || !agpb_combat.GetBool() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
        !IsAlive() || m_pInfo == NULL || !IsCombatEnemy(source) || !eventOrigin.IsValid() ||
        AgPB_IsRoundFrozen() || (GetNetVarInt("m_fFlags") & FL_FROZEN) != 0) return;
    if (!(m_bAutonomous || m_bRoam || HasRoute() || m_Tasks.Current().id != AgPBTaskId::Normal)) return;
    // The caller captures this origin once in the public game-event callback.
    // Range is measured at that instant; later planning never polls its origin.
    if (!AgPBSearch::Audible((eventOrigin - m_pInfo->GetAbsOrigin()).LengthSqr(), radius, ttl)) return;
    IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo(source);
    if (info == NULL) return;
    const Vector look = eventOrigin + Vector(0, 0, AgPB_HullStandHeight() - 4.0f);
    AgPBSearch::Observe(m_Search.sound, AgPB_RefEHandle(source), info->GetUserID(),
        SavePoint(eventOrigin), SavePoint(look), now, ttl);
}

bool CAgPB::UpdateEnemyMemoryLook(CGlobalVars *globals, CBotCmd &cmd) {
    if (globals == NULL || m_pInfo == NULL || AgPB_GameMode() != AgPB_MODE_NORMAL ||
        !m_bCombatEnabled || !agpb_combat.GetBool() || !IsAlive() || AgPB_IsRoundFrozen() ||
        fabsf(cmd.forwardmove) > 0.01f || fabsf(cmd.sidemove) > 0.01f || fabsf(cmd.upmove) > 0.01f ||
        (GetNetVarInt("m_fFlags") & (FL_ONGROUND | FL_FROZEN)) != FL_ONGROUND ||
        GetNetVarInt("movetype", MOVETYPE_NONE) == MOVETYPE_LADDER) return false;
    const float now = globals->curtime;
    const AgPBSearch::Observation *memory = NULL;
    if (AgPBSearch::Alive(m_Search.visual, now) && now - m_Search.visual.observedAt <= AgPBSearch::VisualLookTTL)
        memory = &m_Search.visual;
    else if (AgPBSearch::Alive(m_Search.sound, now)) memory = &m_Search.sound;
    if (memory == NULL || !IsCombatEnemy(HandleToEdict((uintp)memory->handle), memory->userId)) return false;
    Vector offset = GetNetVarVector("m_vecViewOffset");
    if (offset.z <= 0.0f) offset.z = AgPB_HullStandHeight() - 4.0f;
    TurnToSnapshot(m_pInfo->GetAbsOrigin() + offset, LoadPoint(memory->look), globals->interval_per_tick, cmd);
    cmd.buttons &= ~(IN_ATTACK | IN_ATTACK2);
    return true;
}

bool CAgPB::PlanEnemyInvestigation(CGlobalVars *globals) {
    const AgPBTask &current = m_Tasks.Current();
    const bool localTask = current.id == AgPBTaskId::Normal || current.id == AgPBTaskId::MoveToPosition ||
        current.id == AgPBTaskId::Camp || current.id == AgPBTaskId::Pause;
    if (globals == NULL || !m_bAutonomous || !localTask || m_pInfo == NULL || !IsAlive() ||
        !m_bCombatEnabled || !agpb_combat.GetBool() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
        AgPB_IsRoundFrozen() || (GetNetVarInt("m_fFlags") & (FL_ONGROUND | FL_FROZEN)) != FL_ONGROUND ||
        GetNetVarInt("movetype", MOVETYPE_NONE) == MOVETYPE_LADDER || current.desire >= AgPBTaskPriority::Objective ||
        m_AgentIntent.token != 0 || m_Tasks.Contains(AgPBTaskId::AgentIntention) ||
        NetVar_HandleIsValid(m_uCombatEnemy)) return false;
    const float now = globals->curtime;
    AgPBSearch::Observation *evidence = NULL;
    if (AgPBSearch::Alive(m_Search.visual, now) && !m_Search.visual.consumed) evidence = &m_Search.visual;
    if (AgPBSearch::Alive(m_Search.sound, now) && !m_Search.sound.consumed &&
        (evidence == NULL || m_Search.sound.observedAt > evidence->observedAt)) evidence = &m_Search.sound;
    if (evidence == NULL) return false;
    // One finite investigation per observation. A failed route cannot renew
    // a stale lease; a subsequent actual sight/sound can create fresh evidence.
    evidence->consumed = true;
    if (m_Search.visual.observedAt <= evidence->observedAt) m_Search.visual.consumed = true;
    if (m_Search.sound.observedAt <= evidence->observedAt) m_Search.sound.consumed = true;
    if (!IsCombatEnemy(HandleToEdict((uintp)evidence->handle), evidence->userId)) {
        evidence->valid = false; return false;
    }
    CAgPBWaypoints &wp = BotWaypoints();
    const int team = m_pInfo->GetTeamIndex();
    int start = -1; float entryDistance = 512.0f;
    const Vector here = m_pInfo->GetAbsOrigin();
    for (int i = 0; i < wp.Count(); ++i) {
        const AgPBPath *node = wp.Get(i);
        if (node == NULL || !node->origin.IsValid() || !wp.IsPassable(i, team)) continue;
        const float distance = (node->origin - here).Length();
        if (distance >= entryDistance || !CanWalkToTaskPosition(node->origin, (node->flags & AgPB_WP_CROUCH) != 0)) continue;
        start = i; entryDistance = distance;
    }
    if (!wp.IsValid(start)) return false;
    CUtlVector<float> distances; CUtlVector<short> parents;
    wp.ComputeDistances(start, distances, parents, team);
    const Vector snapshot = LoadPoint(evidence->position);
    int anchor = -1; float best = FLT_MAX;
    for (int i = 0; i < wp.Count(); ++i) {
        const AgPBPath *node = wp.Get(i);
        if (node == NULL || (node->flags & (AgPB_WP_LADDER | AgPB_WP_LIFT)) != 0 ||
            !AgPBSearch::SearchCandidate(distances[i], (node->origin - snapshot).LengthSqr(),
                AgPBSearch::MaximumTravel, false, wp.IsPassable(i, team))) continue;
        const float score = (node->origin - snapshot).LengthSqr() + distances[i] * 24.0f;
        if (score < best) { best = score; anchor = i; }
    }
    if (anchor < 0) return false;
    AgPBTask task; task.id = AgPBTaskId::Investigate; task.desire = AgPBTaskPriority::Investigate;
    task.waypoint = anchor; task.canResume = false; task.deadline = AgPBSearch::Deadline(now, *evidence);
    task.hasPosition = true;
    for (int i = 0; i < 3; ++i) task.position[i] = snapshot[i];
    const std::uint64_t token = m_Tasks.Push(task);
    if (token == 0) return false;
    AgPBSearch::Investigation search;
    search.token = token; search.evidence = *evidence; search.anchor = anchor;
    search.deadline = task.deadline; search.scanLook = evidence->look;
    m_Search.investigation = search;
    RouteReport("investigate: %s ent=%d userid=%d age=%.2f snapshot=(%.1f %.1f %.1f) waypoint=#%d deadline=%.2f token=%llu",
        evidence == &m_Search.visual ? "seen" : "heard", NetVar_HandleEntry((uintp)evidence->handle), evidence->userId,
        now - evidence->observedAt, snapshot.x, snapshot.y, snapshot.z, anchor, task.deadline, (unsigned long long)token);
    return true;
}

bool CAgPB::UpdateEnemyInvestigation(CGlobalVars *globals, CBotCmd &cmd) {
    if (m_Tasks.Current().id != AgPBTaskId::Investigate) return false;
    AgPBSearch::Investigation &search = m_Search.investigation;
    const AgPBTask task = m_Tasks.Current();
    const float now = globals->curtime;
    if (task.token != search.token || !m_bAutonomous || m_AgentIntent.token != 0 ||
        !AgPBSearch::CanContinue(search, now) ||
        !IsCombatEnemy(HandleToEdict((uintp)search.evidence.handle), search.evidence.userId)) {
        CompleteCurrentTask("investigation ended"); search = AgPBSearch::Investigation(); return true;
    }
    CAgPBWaypoints &wp = BotWaypoints();
    if (!HasRoute() && !m_bRouteArrived && search.dwellUntil == 0.0f) {
        char error[192];
        if (!BuildRoute(task.waypoint, error, sizeof(error))) {
            CompleteCurrentTask(error, true); search = AgPBSearch::Investigation(); return true;
        }
        // BuildRoute may choose a different supported graph entry or the
        // graph may have changed. Charge the actual authored leg, including
        // its short entry approach, rather than trusting the planning estimate.
        const AgPBPath *previous = wp.Get(RouteNode(0));
        float travel = previous != NULL ? (previous->origin - m_pInfo->GetAbsOrigin()).Length() : FLT_MAX;
        for (int i = 1; i < RouteCount() && travel < FLT_MAX; ++i) {
            const AgPBPath *node = wp.Get(RouteNode(i));
            if (node == NULL || previous == NULL) { travel = FLT_MAX; break; }
            travel += (node->origin - previous->origin).Length(); previous = node;
        }
        if (!AgPBSearch::ReserveTravel(search, travel)) {
            CompleteCurrentTask("local search travel budget exhausted"); search = AgPBSearch::Investigation(); return true;
        }
        m_uRouteTaskToken = task.token;
    }
    if (HasRoute()) {
        UpdateRoute(globals, cmd);
        if (m_Tasks.Current().token != task.token) return true; // A breakable child owns execution.
        if (HasRoute()) return true;
        if (!m_bRouteArrived) {
            CompleteCurrentTask("investigation route failed", true); search = AgPBSearch::Investigation(); return true;
        }
    }
    if (search.dwellUntil == 0.0f) {
        if (!AgPBSearch::Visited(search, task.waypoint) && search.visitedCount < AgPBSearch::MaximumVisits)
            search.visited[search.visitedCount++] = task.waypoint;
        search.dwellUntil = now + RandomFloat(0.9f, 1.5f);
        search.nextLookAt = now + 0.4f; search.scanLook = search.evidence.look;
    }
    cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
    cmd.buttons &= ~(IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP);
    const AgPBPath *standingNode = wp.Get(task.waypoint);
    if (standingNode != NULL && (standingNode->flags & AgPB_WP_CROUCH) != 0) cmd.buttons |= IN_DUCK;
    Vector offset = GetNetVarVector("m_vecViewOffset");
    if (offset.z <= 0.0f) offset.z = AgPB_HullStandHeight() - 4.0f;
    const Vector eye = m_pInfo->GetAbsOrigin() + offset;
    if (now >= search.nextLookAt) {
        // Reservoir sampling bounds expensive visibility traces even when
        // the manually authored graph contains many nearby nodes.
        int candidates[12], count = 0;
        for (int i = 0; i < wp.Count(); ++i) {
            const AgPBPath *node = wp.Get(i);
            if (node == NULL || !node->origin.IsValid() || i == task.waypoint ||
                !wp.IsPassable(i, m_pInfo->GetTeamIndex())) continue;
            const float height = ((node->flags & AgPB_WP_CROUCH) != 0 ? AgPB_HullDuckHeight() : AgPB_HullStandHeight()) - 4.0f;
            const Vector direction = node->origin + Vector(0, 0, height) - eye;
            if (!AgPBObservationLook::ScanCandidate(direction.x, direction.y, direction.z, 768.0f)) continue;
            const int slot = count < 12 ? count : RandomInt(0, count);
            if (slot < 12) candidates[slot] = i;
            ++count;
        }
        int chosen = -1, visibleCount = 0;
        for (int i = 0; i < MIN(count, 12); ++i) {
            const AgPBPath *node = wp.Get(candidates[i]);
            const float height = ((node->flags & AgPB_WP_CROUCH) != 0 ? AgPB_HullDuckHeight() : AgPB_HullStandHeight()) - 4.0f;
            const Vector point = node->origin + Vector(0, 0, height);
            if (VisibleLookPoint(m_Ctx, m_pEdict, eye, point) && RandomInt(1, ++visibleCount) == 1) chosen = candidates[i];
        }
        if (chosen >= 0) {
            const AgPBPath *node = wp.Get(chosen);
            const float height = ((node->flags & AgPB_WP_CROUCH) != 0 ? AgPB_HullDuckHeight() : AgPB_HullStandHeight()) - 4.0f;
            search.scanLook = SavePoint(node->origin + Vector(0, 0, height));
        }
        search.nextLookAt = now + RandomFloat(0.5f, 0.9f);
    }
    TurnToSnapshot(eye, LoadPoint(search.scanLook), globals->interval_per_tick, cmd);
    if (now < search.dwellUntil) return true;
    if (search.visitedCount >= AgPBSearch::MaximumVisits || now >= search.deadline) {
        CompleteCurrentTask("local search exhausted"); search = AgPBSearch::Investigation(); return true;
    }
    const int team = m_pInfo->GetTeamIndex();
    int start = -1; float entryDistance = 512.0f;
    const Vector here = m_pInfo->GetAbsOrigin();
    for (int i = 0; i < wp.Count(); ++i) {
        const AgPBPath *node = wp.Get(i);
        if (node == NULL || !node->origin.IsValid() || !wp.IsPassable(i, team)) continue;
        const float distance = (node->origin - here).Length();
        if (distance >= entryDistance || !CanWalkToTaskPosition(node->origin, (node->flags & AgPB_WP_CROUCH) != 0)) continue;
        start = i; entryDistance = distance;
    }
    if (!wp.IsValid(start)) { CompleteCurrentTask("search left authored graph", true); search = AgPBSearch::Investigation(); return true; }
    CUtlVector<float> distances, anchorDistances; CUtlVector<short> parents, anchorParents;
    wp.ComputeDistances(start, distances, parents, team);
    wp.ComputeDistances(search.anchor, anchorDistances, anchorParents, team);
    int next = -1; float score = FLT_MAX;
    const Vector snapshot = LoadPoint(search.evidence.position);
    for (int i = 0; i < wp.Count(); ++i) {
        const AgPBPath *node = wp.Get(i);
        if (node == NULL || (node->flags & (AgPB_WP_LADDER | AgPB_WP_LIFT)) != 0 ||
            anchorDistances[i] > AgPBSearch::LocalLegTravel ||
            !AgPBSearch::SearchCandidate(distances[i], (node->origin - snapshot).LengthSqr(),
                MIN(search.remainingTravel, AgPBSearch::LocalLegTravel), AgPBSearch::Visited(search, i), wp.IsPassable(i, team))) continue;
        // Prefer a different nearby branch over a near-duplicate camp point.
        if ((node->origin - m_pInfo->GetAbsOrigin()).Length2DSqr() < 80.0f * 80.0f) continue;
        const float candidateScore = distances[i] + (node->origin - snapshot).Length() * 0.25f;
        if (candidateScore < score) { score = candidateScore; next = i; }
    }
    if (next < 0) { CompleteCurrentTask("local search exhausted"); search = AgPBSearch::Investigation(); return true; }
    ClearRoute();
    // A local search owns one stable task token across all of its authored legs.
    m_Tasks.Current().waypoint = next;
    search.dwellUntil = 0.0f;
    return true;
}

void CAgPB::ReportSearch() const {
    const float now = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
    auto report = [&](const char *kind, const AgPBSearch::Observation &memory) {
        RouteReport("perception %s: valid=%d fresh=%d ent=%d handle=0x%llX userid=%d age=%.2f ttl=%.2f consumed=%d snapshot=(%.1f %.1f %.1f)",
            kind, memory.valid ? 1 : 0, AgPBSearch::Alive(memory, now) ? 1 : 0,
            memory.valid ? NetVar_HandleEntry((uintp)memory.handle) : -1, (unsigned long long)memory.handle, memory.userId,
            memory.valid ? now - memory.observedAt : -1.0f, memory.valid ? MAX(0.0f, memory.expiresAt - now) : 0.0f,
            memory.consumed ? 1 : 0, memory.position.x, memory.position.y, memory.position.z);
    };
    report("seen", m_Search.visual); report("heard", m_Search.sound);
    const AgPBSearch::Investigation &search = m_Search.investigation;
    const AgPBTaskState state = m_Tasks.State(search.token);
    RouteReport("investigation: token=%llu state=%s anchor=%d visits=%d/%d deadline=%.2f left=%.2f travel-left=%.1f route-owner=%llu goal=%d hop=%d",
        (unsigned long long)search.token, state == AgPBTaskState::Active ? "active" : state == AgPBTaskState::Suspended ? "suspended" : "missing",
        search.anchor, search.visitedCount, AgPBSearch::MaximumVisits, search.deadline,
        search.token != 0 ? MAX(0.0f, search.deadline - now) : 0.0f, search.remainingTravel,
        (unsigned long long)m_uRouteTaskToken, m_iGoalWaypoint, m_iRouteIndex);
}
