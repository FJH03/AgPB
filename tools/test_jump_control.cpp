// Pure regression for authored directed jump launch preparation and arithmetic.
#include "../src/jump_control.h"
#include "../src/route_control.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float actual, float expected, float epsilon = 0.001f) {
    return std::fabs(actual - expected) <= epsilon;
}
static void CrouchJumpLaunchLifecycle() {
    // Compose the real route posture and injection policies. The authored
    // office 52->53 landing is CROUCH: that request must coexist with the
    // first jump, rather than leave the bot waiting indefinitely for unduck.
    using namespace AgPBJump;
    State state;
    SetLeg(state, 52, 53);
    Conditions conditions;
    conditions.grounded = conditions.routeOwned = true;
    const Vector3 authored(-2.345997f, 119.977066f, -85.0f);
    const AgPBTerminal::RoutePosture takeoff = AgPBTerminal::ComputeRoutePosture(
        true, true, false, 49.0f, true, false, true, false);
    Require(takeoff.jump && takeoff.crouch && ShouldInject(state, authored, conditions),
        "a CROUCH landing may request duck and the first owned jump together");
    Vector3 impulse;
    const Vector3 current(0.0f, 0.0f, 0.0f);
    const float dt = 0.015f;
    Require(BaseVelocityDelta(authored, current, dt, impulse) &&
        Near((1.0f + dt * 0.5f) * impulse.z, -85.0f),
        "crouch-jump still prepares authored pre-jump XYZ without inventing a native final Z");
    MarkIssued(state); // Caller commits only the successfully queued impulse.
    int injections = 1;
    for (int tick = 0; tick < 20; ++tick) {
        conditions.grounded = false;
        conditions.previousJump = tick == 0;
        const AgPBTerminal::RoutePosture flight = AgPBTerminal::ComputeRoutePosture(
            true, true, false, 35.0f, true, false, false, false);
        Require(!flight.jump && flight.crouch && !ShouldInject(state, authored, conditions),
            "airborne CROUCH posture neither requests a fresh native jump nor reinjects velocity");
        if (ShouldInject(state, authored, conditions)) { MarkIssued(state); ++injections; }
    }
    conditions.grounded = true; conditions.previousJump = false;
    const bool landed = AgPBTerminal::LandedJump(true, true, true, 0.0f);
    const AgPBTerminal::RoutePosture landing = AgPBTerminal::ComputeRoutePosture(
        true, true, false, 20.0f, true, landed, true, false);
    Require(landed && !landing.jump && landing.crouch && !ShouldInject(state, authored, conditions),
        "the CROUCH landing retains posture without a second launch on touchdown");
    Require(!ShouldInject(state, authored, conditions) && injections == 1,
        "a failed takeoff or an off-height landing also cannot consume a second same-leg impulse");
    SetLeg(state, 53, 60);
    Require(ShouldInject(state, authored, conditions),
        "a newly selected directed jump has its own launch even when the route retains crouch");

    Reset(state); SetLeg(state, 52, 53);
    conditions.grounded = false;
    Require(!ShouldInject(state, authored, conditions) && !state.issued,
        "an unissued connection first observed airborne must wait rather than injecting in flight");
    conditions.grounded = true;
    Require(ShouldInject(state, authored, conditions),
        "airborne rejection does not spend a never-issued grounded launch");
}
int main() {
    using namespace AgPBJump;
    CrouchJumpLaunchLifecycle();
    const float dt = 1.0f / 64.0f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    Require(ValidVelocity(Vector3()) && !NonZero(Vector3()), "zero vector remains an ordinary jump sentinel");
    Require(ValidVelocity(Vector3(-2000, 2000, -2000)), "signed authored component boundaries accepted");
    Require(!ValidVelocity(Vector3(2000.01f, 0, 0)) && !ValidVelocity(Vector3(0, -2000.01f, 0)),
        "out-of-range authored components refused rather than silently clamped");
    Require(!ValidVelocity(Vector3(0, 0, nan)) && !ValidVelocity(Vector3(infinity, 0, 0)),
        "nonfinite launch data rejected before arithmetic");
    Require(!NonZero(Vector3(0, nan, 0)), "nonfinite data cannot masquerade as a nonzero launch");

    Vector3 delta(1, 2, 3);
    Require(!BaseVelocityDelta(Vector3(), Vector3(250, 0, 0), dt, delta) && !NonZero(delta),
        "zero authored velocity does not turn current motion into a stopping impulse");
    const Vector3 authored(-600, 450, 180), current(100, -90, -12);
    Require(BaseVelocityDelta(authored, current, dt, delta), "full signed XYZ launch impulse prepared");
    const float factor = 1.0f + dt * 0.5f;
    Require(Near(current.x + factor * delta.x, authored.x) &&
        Near(current.y + factor * delta.y, authored.y) &&
        Near(current.z + factor * delta.z, authored.z),
        "CheckMovingGround consumption produces all three authored pre-jump components");
    Require(!Near(current.x + delta.x, authored.x, 0.1f),
        "raw unscaled delta would not produce the requested engine impulse");
    Require(BaseVelocityDelta(Vector3(2000, -2000, 2000), Vector3(-2000, 2000, -2000), dt, delta) &&
        Near(-2000 + factor * delta.x, 2000) && Near(2000 + factor * delta.y, -2000),
        "reversing motion uses full delta without clipping the target vector");
    Require(!BaseVelocityDelta(authored, Vector3(nan, 0, 0), dt, delta) && !NonZero(delta),
        "unavailable current velocity cannot leave a partially prepared impulse");
    Require(!BaseVelocityDelta(authored, current, 0, delta) &&
        !BaseVelocityDelta(authored, current, -dt, delta) &&
        !BaseVelocityDelta(authored, current, infinity, delta), "invalid command intervals rejected");

    Require(BunnyMaxSpeed(Vector3(), 250, 800, dt) == 250,
        "ordinary zero-vector jump preserves the original player speed limit");
    Require(BunnyMaxSpeed(Vector3(100, 0, 0), 1000, 800, dt) == 1000,
        "an already sufficient speed limit is never lowered");
    const Vector3 cases[] = { Vector3(600, 0, 0), Vector3(-600, 450, 180),
        Vector3(0, 0, -2000), Vector3(2000, 2000, 2000), Vector3(-2000, -2000, -2000) };
    for (const Vector3 &velocity : cases) {
        const float maximum = BunnyMaxSpeed(velocity, 250, 800, dt);
        const double zBeforeJump = double(velocity.z) - 800.0 * double(dt) * 0.5;
        const double beforeNativeImpulse = std::sqrt(double(velocity.x) * velocity.x +
            double(velocity.y) * velocity.y + zBeforeJump * zBeforeJump);
        Require(maximum >= 250 && double(BunnySpeedFactor) * maximum > beforeNativeImpulse,
            "temporary limit covers the actual three-dimensional bunny-cap speed after half gravity");
    }
    Require(BunnyMaxSpeed(Vector3(500, 0, 0), 250, -800, dt) ==
        BunnyMaxSpeed(Vector3(500, 0, 0), 250, 800, dt), "gravity allowance is conservative for either sign");
    Require(BunnyMaxSpeed(authored, nan, 800, dt) == 0 &&
        BunnyMaxSpeed(authored, 0, 800, dt) == 0 &&
        BunnyMaxSpeed(authored, 250, infinity, dt) == 0 &&
        BunnyMaxSpeed(authored, 250, 800, 0) == 0 &&
        BunnyMaxSpeed(Vector3(0, nan, 0), 250, 800, dt) == 0,
        "invalid temporary speed calculation returns an explicit failure sentinel");

    State state; Conditions ready;
    ready.grounded = ready.routeOwned = true;
    SetLeg(state, 11, 27);
    Require(ShouldInject(state, authored, ready), "grounded owned authored directed leg can launch");
    Require(!ShouldInject(state, Vector3(), ready), "zero velocity still permits caller normal jump without injection");
    Require(!ShouldInject(state, Vector3(nan, 0, 0), ready), "invalid velocity cannot arm a leg");
    Conditions blocked = ready; blocked.grounded = false;
    Require(!ShouldInject(state, authored, blocked), "airborne observation cannot reinject speed");
    blocked = ready; blocked.previousJump = true;
    Require(!ShouldInject(state, authored, blocked), "jump must be released before a fresh native takeoff");
    blocked = ready; blocked.paused = true;
    Require(!ShouldInject(state, authored, blocked), "paused command cannot consume a launch impulse");
    blocked = ready; blocked.frozen = true;
    Require(!ShouldInject(state, authored, blocked), "real engine or round freeze suspends preparation");
    blocked = ready; blocked.routeOwned = false;
    Require(!ShouldInject(state, authored, blocked), "lost route ownership cannot launch a stale connection");
    blocked = ready; blocked.externalBaseVelocity = true;
    Require(!ShouldInject(state, authored, blocked), "external basevelocity is never overwritten");
    Require(ShouldInject(state, authored, ready), "waiting conditions never mark an unqueued leg spent");
    MarkIssued(state);
    Require(!ShouldInject(state, authored, ready), "failed native jump cannot repeatedly inject on the same grounded leg");
    SetLeg(state, 11, 27);
    Require(state.issued && !ShouldInject(state, authored, ready), "same-leg selection does not rearm a landed or interrupted launch");
    blocked = ready; blocked.frozen = true;
    Require(!ShouldInject(state, authored, blocked) && !ShouldInject(state, authored, ready),
        "freeze and thaw preserve the already-issued connection");
    SetLeg(state, 27, 11);
    Require(ShouldInject(state, authored, ready), "reverse directed connection has its own one-shot launch");
    SetLeg(state, 27, 27);
    Require(!ShouldInject(state, authored, ready), "self connection cannot prepare a launch");
    Reset(state);
    Require(!state.issued && state.from == -1 && state.to == -1 && !ShouldInject(state, authored, ready),
        "route cancellation reset removes the old leg identity");
    SetLeg(state, 11, 27);
    Require(ShouldInject(state, authored, ready), "new route after explicit reset can traverse the same connection again");
    std::puts("jump launch policy tests passed");
}
