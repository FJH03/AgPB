// Task posture shared by navigation, combat and owned-weapon execution.
// SyPB's TASK_CAMP retains position; WAYPOINT_CROUCH selects its duck posture.
// AgPB: GPL-3.0-only. Engine-independent lifecycle/ownership boundaries.
#ifndef AGPB_HOLDING_POLICY_H
#define AGPB_HOLDING_POLICY_H
#include "tasks.h"
#include <cmath>

namespace AgPBHolding {
inline bool NavigationCrouch(bool crouch, bool camp) {
    // SyPB basecode.cpp BotAI: CROUCH without CAMP is a low passage.
    // CAMP+CROUCH authors the arrived holding posture, not its approach.
    return crouch && !camp;
}
inline bool Active(const AgPBTask &task, AgPBTaskState state, float now,
                   bool alive, bool normalMode, bool frozen) {
    return alive && normalMode && !frozen && state == AgPBTaskState::Active &&
        task.token != 0 && std::isfinite(now) && std::isfinite(task.deadline) &&
        (task.deadline <= 0.0f || now < task.deadline);
}
inline bool AgentValid(const AgPBTask &task, std::uint64_t token, int waypoint,
                       float deadline, float now) {
    return task.id != AgPBTaskId::AgentIntention ||
        (token != 0 && task.token == token && task.waypoint == waypoint &&
         std::isfinite(deadline) && now < deadline);
}
inline bool HoldsPosition(const AgPBTask &task, bool agentCamp, bool agentArrived) {
    return task.id == AgPBTaskId::Camp || task.id == AgPBTaskId::Pause ||
        (task.id == AgPBTaskId::AgentIntention && agentCamp && agentArrived);
}
inline bool RequiresCrouch(const AgPBTask &task, bool holding, bool holdingNodeCrouch,
                          std::uint64_t routeOwner, bool currentCrouch,
                          bool previousCrouch, float previousDistance) {
    if (holding && holdingNodeCrouch) return true;
    // currentCrouch/previousCrouch are navigation-crouch classifications
    // from NavigationCrouch, not raw authored CROUCH flags. In particular,
    // a previous CAMP+CROUCH node cannot trigger the 64-unit passage carry.
    // A retained route may also belong to the current execution child's owner.
    const bool ownsRoute = routeOwner != 0 && (routeOwner == task.token ||
        (task.id == AgPBTaskId::DestroyBreakable && task.parentToken == routeOwner));
    return ownsRoute && (currentCrouch || (previousCrouch &&
        std::isfinite(previousDistance) && previousDistance >= 0.0f && previousDistance < 64.0f));
}
}
#endif
