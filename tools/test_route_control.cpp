// Standalone regression of the real controller under Source-style ground
// friction/acceleration, including speed caps and altered surface friction.
#include "../src/route_control.h"
#include "../src/holding_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace AgPBTerminal;
static void Require(bool good, const char* why) { if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
static void Simulate(float x, float y, float vx, float vy, float radius,
                     float dt, float surface, float cap, const char* name) {
    float stable = 0;
    for (int tick = 0; tick < int(12 / dt); ++tick) {
        const bool inside = ControlPositionInside(-x, -y, 0, radius, true);
        const Wish input = ComputeWish(-x, -y, vx, vy, dt, 5, 4, 100, 250, inside);
        float wishSpeed = std::sqrt(input.x * input.x + input.y * input.y);
        Require(std::isfinite(wishSpeed) && wishSpeed <= 250.01f, "finite bounded CUserCmd");
        float speed = std::sqrt(vx * vx + vy * vy);
        const float keep = speed > 0.01f ? Max(0.0f, speed - Max(speed, 100) * 4 * surface * dt) / speed : 0;
        vx *= keep; vy *= keep;
        if (wishSpeed > 0.001f) {
            const float nx = input.x / wishSpeed, ny = input.y / wishSpeed;
            wishSpeed = Min(cap, wishSpeed);
            const float add = wishSpeed - (vx * nx + vy * ny);
            if (add > 0) {
                const float amount = Min(add, 5 * dt * wishSpeed * surface);
                vx += nx * amount; vy += ny * amount;
            }
        }
        x += vx * dt; y += vy * dt;
        if (ControlPositionInside(-x, -y, 0, radius, true) && CanSettle(-x, -y, 0, radius, true, vx, vy, 0)) stable += dt;
        else stable = 0;
        if (stable >= StableSeconds) { std::printf("PASS %s distance=%.3f speed=%.3f seconds=%.3f\n", name, std::sqrt(x*x+y*y), std::sqrt(vx*vx+vy*vy), tick*dt); return; }
    }
    std::fprintf(stderr,"FAILED simulation %s x=%.3f y=%.3f v=%.3f %.3f\n",name,x,y,vx,vy); std::exit(1);
}
static void JumpIntoCrouchSection() {
    // The current office landing anchor #53 itself is CROUCH. Traversing
    // 52->53 must not require standing up before or during this jump.
    struct Phase { bool grounded; float dz; float distance; bool jump; bool crouch; };
    const Phase phases[] = {
        {true, 30, 49, true, true},
        {false, -17, 42, false, true},
        {false, 0, 30, false, true},
        {true, 0, 28, false, true},
        {true, 0, 20, false, true}
    };
    bool seenAirborne = false;
    for (const Phase& phase : phases) {
        if (!phase.grounded) seenAirborne = true;
        const bool landed = LandedJump(true, seenAirborne, phase.grounded, phase.dz);
        const RoutePosture posture = ComputeRoutePosture(true, true, false,
            phase.distance, true, landed, phase.grounded, false);
        Require(posture.jump == phase.jump && posture.crouch == phase.crouch,
                "CROUCH landing remains ducked across takeoff, flight and landing without a repeated jump");
    }
    const RoutePosture takeoff = ComputeRoutePosture(true, true, true, 60, false, false, true, false);
    Require(!takeoff.jump && takeoff.crouch, "current CROUCH passage does not stand up merely because the next connection jumps");
    const RoutePosture landingTakeoff = ComputeRoutePosture(true, true, true, 28, true, true, true, false);
    Require(!landingTakeoff.jump && landingTakeoff.crouch, "landed jump chain retains low-passage posture without repeating the completed jump");
    const RoutePosture distant = ComputeRoutePosture(false, true, false, 65, false, false, true, false);
    Require(!distant.crouch, "next crouch does not affect a distant ordinary anchor");
    const RoutePosture authored = ComputeRoutePosture(true, false, false, 28, true, true, true, false);
    Require(authored.crouch && !authored.jump, "authored crouch also applies after landing");
    const RoutePosture recovery = ComputeRoutePosture(false, true, false, 28, true, true, true, true);
    Require(recovery.crouch && !recovery.jump, "grounded recovery preserves low-opening posture");
    const RoutePosture crouchedRecovery = ComputeRoutePosture(true, false, false, 49, true, false, true, true);
    Require(crouchedRecovery.crouch && !crouchedRecovery.jump,
        "failed-jump recovery suspends jumping without discarding the current authored CROUCH");
}
static void OrdinaryJumpAndCampBoundaries() {
    // A future low passage is different from a current CROUCH point. This
    // ordinary jump still relies on engine-owned airborne crouch-jump behavior.
    const RoutePosture approach = ComputeRoutePosture(false, true, true, 4, false, false, true, false);
    Require(!approach.jump && !approach.crouch,
        "a non-CROUCH takeoff anchor does not pre-duck just because its next connection jumps into crouch");
    const RoutePosture standingTakeoff = ComputeRoutePosture(false, true, false, 49, true, false, true, false);
    const RoutePosture flight = ComputeRoutePosture(false, true, false, 30, true, false, false, false);
    Require(standingTakeoff.jump && !standingTakeoff.crouch && !flight.jump && !flight.crouch,
        "a non-CROUCH ordinary jump adds no manual takeoff or airborne duck for a later passage");
    const RoutePosture landing = ComputeRoutePosture(false, true, false, 28, true, true, true, false);
    Require(!landing.jump && landing.crouch, "the next low passage may begin after an ordinary jump has actually landed");
    const RoutePosture exactBoundary = ComputeRoutePosture(false, true, false, 64, false, false, true, false);
    const RoutePosture nearby = ComputeRoutePosture(false, true, false, 63.9f, false, false, true, false);
    Require(!exactBoundary.crouch && nearby.crouch,
        "next-node pre-crouch remains a nearby grounded passage behavior with the existing 64-unit boundary");
    const RoutePosture ungroundedApproach = ComputeRoutePosture(false, true, false, 20, false, false, false, false);
    Require(!ungroundedApproach.crouch, "ordinary airborne travel cannot start a future node's manual pre-crouch");
    const bool campNavigation = AgPBHolding::NavigationCrouch(true, true);
    const RoutePosture campApproach = ComputeRoutePosture(campNavigation, false, false, 4, false, false, true, false);
    const RoutePosture passageApproach = ComputeRoutePosture(AgPBHolding::NavigationCrouch(true, false),
        false, false, 4, false, false, true, false);
    Require(!campApproach.crouch && passageApproach.crouch,
        "CAMP+CROUCH remains arrived holding posture while ordinary CROUCH still governs navigation");
    Require(!AgPBHolding::NavigationCrouch(false, true), "standing CAMP does not introduce passage crouch");
}
int main() {
    JumpIntoCrouchSection();
    OrdinaryJumpAndCampBoundaries();
    Require(!LandedJump(true,false,true,0),"takeoff is not a completed jump");
    Require(!LandedJump(true,true,false,0),"airborne jump still runs");
    Require(!LandedJump(true,true,true,32),"wrong landing height cannot complete jump");
    Require(LandedJump(true,true,true,0.00025f),"correct landing aligns without repeated jump");
    Require(!LandedJump(false,true,true,0),"ordinary leg is not a jump landing");
    Require(!ControlPositionInside(3.9f,0,0,0,true),"control leaves an inner measurement margin");
    Require(CanSettle(3,0,3,0,true,0,0,0),"raw radius zero has four-unit XY precision, separate height");
    Require(!CanSettle(3,0,4.1f,0,true,0,0,0),"wrong height");
    Require(!CanSettle(0,0,0,8,false,0,0,0),"airborne is not settled");
    Require(!CanSettle(0,0,0,8,true,6,0,0),"moving is not settled");
    Require(!CanSettle(56.9f,0,0,8,true,0,0,0),"historical overshoot stays a failure");
    Require(!CanSettle(0,0,0,8,true,std::numeric_limits<float>::quiet_NaN(),0,0),"unknown velocity is not success");
    Simulate(-500,0,250,0,0,0.015f,1,250,"zero_radius_running");
    Simulate(-300,-200,0,250,8,0.015f,1,250,"lateral_momentum");
    Simulate(-2,0,250,0,0,0.015f,1,250,"brake_inside_radius");
    Simulate(-200,50,0,0,4,0.015f,1,85,"crouch_cap");
    Simulate(-500,0,250,0,8,0.015f,0.5f,250,"low_surface_friction");
    Simulate(-500,0,250,0,8,0.015f,1.5f,250,"high_surface_friction");
    Simulate(-500,0,250,0,24,0.03125f,1,250,"32_tick");
    Simulate(-500,0,250,0,0,0.0078125f,1,250,"128_tick");
    return 0;
}
