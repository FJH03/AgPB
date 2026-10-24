// Holding/posture ownership regression; independent of the Source SDK/runtime.
#include "../src/holding_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main() {
    using namespace AgPBHolding;
    AgPBTask task;
    task.id = AgPBTaskId::Camp; task.token = 5; task.waypoint = 3; task.deadline = 20.0f;
    Require(Active(task, AgPBTaskState::Active, 10.0f, true, true, false), "live camp has posture authority");
    Require(!Active(task, AgPBTaskState::Active, 20.0f, true, true, false), "deadline is exclusive even before dispatcher expiry");
    Require(!Active(task, AgPBTaskState::Active, 10.0f, false, true, false), "death removes posture authority");
    Require(!Active(task, AgPBTaskState::Active, 10.0f, true, false, false), "other modes receive no normal posture overlay");
    Require(!Active(task, AgPBTaskState::Active, 10.0f, true, true, true), "freeze releases posture authority");
    Require(!Active(task, AgPBTaskState::Suspended, 10.0f, true, true, false), "suspended camp cannot impose posture on its replacement");
    Require(!Active(task, AgPBTaskState::Missing, 10.0f, true, true, false), "removed task cannot retain crouch");
    Require(!Active(task, AgPBTaskState::Active, std::numeric_limits<float>::quiet_NaN(), true, true, false), "unknown time cannot keep a lease alive");
    task.deadline = std::numeric_limits<float>::quiet_NaN();
    Require(!Active(task, AgPBTaskState::Active, 10.0f, true, true, false), "invalid deadline fails closed");
    task.deadline = 0.0f;
    Require(Active(task, AgPBTaskState::Active, 10.0f, true, true, false), "local no-expiry task remains supported");

    Require(HoldsPosition(task, false, false), "camp holds without implying crouch");
    Require(!RequiresCrouch(task, true, false, 0, false, false, 0), "CAMP alone may stand");
    Require(RequiresCrouch(task, true, true, 0, false, false, 0), "CAMP+CROUCH survives a completed and cleared route");
    task.id = AgPBTaskId::Pause;
    Require(HoldsPosition(task, false, false) && RequiresCrouch(task, true, true, 0, false, false, 0), "pause shares authored holding posture");
    task.id = AgPBTaskId::MoveToPosition;
    Require(!HoldsPosition(task, true, true), "ordinary route is not a holding task");
    Require(!RequiresCrouch(task, false, true, 0, false, false, 0), "an unreached destination cannot force its crouch early");
    Require(NavigationCrouch(true, false) && !NavigationCrouch(true, true) &&
        !NavigationCrouch(false, true) && !NavigationCrouch(false, false),
        "only CROUCH without CAMP classifies as navigation crouch");
    Require(RequiresCrouch(task, false, false, 5, NavigationCrouch(true, false), false, 100),
        "ordinary CROUCH route point requires low-passage approach posture");
    Require(!RequiresCrouch(task, false, true, 5, NavigationCrouch(true, true), false, 100),
        "owned route approaching CAMP+CROUCH does not crouch before holding");
    Require(RequiresCrouch(task, true, true, 5, NavigationCrouch(true, true), false, 100),
        "arrived CAMP+CROUCH retains holding crouch despite no navigation crouch");
    Require(!RequiresCrouch(task, false, false, 5, NavigationCrouch(false, true), false, 100) &&
        !RequiresCrouch(task, true, false, 0, false, false, 0),
        "CAMP alone requires neither navigation nor holding crouch");
    Require(!RequiresCrouch(task, false, false, 5, false, NavigationCrouch(true, true), 0) &&
        !RequiresCrouch(task, false, false, 5, false, NavigationCrouch(true, true), 63.99f),
        "previous CAMP+CROUCH cannot leave a 64-unit low-passage crouch residue");

    task.id = AgPBTaskId::AgentIntention; task.deadline = 20.0f;
    Require(AgentValid(task, 5, 3, 20.0f, 10.0f) && HoldsPosition(task, true, true), "arrived camp lease holds its exact goal");
    Require(!HoldsPosition(task, true, false) && !HoldsPosition(task, false, true), "agent approach and move-only intent do not hold");
    Require(!AgentValid(task, 6, 3, 20.0f, 10.0f), "replaced lease cannot reuse old arrival state");
    Require(!AgentValid(task, 5, 4, 20.0f, 10.0f), "arrival belongs to the exact waypoint");
    Require(!AgentValid(task, 5, 3, 10.0f, 10.0f), "agent TTL expires independently of paused task budget");

    task.id = AgPBTaskId::MoveToPosition;
    Require(RequiresCrouch(task, false, false, 5, true, false, 100), "current authored route point requires crouch");
    Require(RequiresCrouch(task, false, false, 5, false, true, 63.99f), "near previous crouch point retains low passage posture");
    Require(!RequiresCrouch(task, false, false, 5, false, true, 64.0f), "previous point releases at the existing 64-unit boundary");
    Require(!RequiresCrouch(task, false, false, 6, true, true, 0), "stale route owner cannot add crouch to a replacement task");
    Require(!RequiresCrouch(task, false, false, 0, true, true, 0), "stop-cleared route has no posture authority");
    Require(!RequiresCrouch(task, false, false, 5, false, true, std::numeric_limits<float>::quiet_NaN()), "invalid proximity cannot retain crouch");
    task.id = AgPBTaskId::DestroyBreakable; task.token = 6; task.parentToken = 5;
    Require(RequiresCrouch(task, false, false, 5, true, false, 100), "execution child retains its parent route passage posture");
    task.parentToken = 7;
    Require(!RequiresCrouch(task, false, false, 5, true, false, 0), "unrelated parent does not inherit route crouch");
    std::puts("holding policy regression passed");
    return 0;
}
