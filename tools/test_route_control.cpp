// Standalone regression of the real controller under Source-style ground
// friction/acceleration, including speed caps and altered surface friction.
#include "../src/route_control.h"
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
    // cs_office's authored 52->53 jump lands before the low opening, with
    // CROUCH on the next node (60), not the landing anchor (53). Replay the
    // takeoff, airborne and grounded phases through the production policies.
    struct Phase { bool grounded; float dz; float distance; bool jump; bool crouch; };
    const Phase phases[] = {
        {true, 30, 49, true, false},
        {false, -17, 42, false, false},
        {false, 0, 30, false, false},
        {true, 0, 28, false, true},
        {true, 0, 20, false, true}
    };
    bool seenAirborne = false;
    for (const Phase& phase : phases) {
        if (!phase.grounded) seenAirborne = true;
        const bool landed = LandedJump(true, seenAirborne, phase.grounded, phase.dz);
        const RoutePosture posture = ComputeRoutePosture(false, true, false,
            phase.distance, true, landed, phase.grounded, false);
        Require(posture.jump == phase.jump && posture.crouch == phase.crouch,
                "jump reaches authored crouch section without airborne duck or repeated jump");
    }
    const RoutePosture takeoff = ComputeRoutePosture(true, true, true, 60, false, false, true, false);
    Require(!takeoff.jump && !takeoff.crouch, "crouch approach releases duck before next takeoff");
    const RoutePosture landingTakeoff = ComputeRoutePosture(true, true, true, 28, true, true, true, false);
    Require(!landingTakeoff.jump && !landingTakeoff.crouch, "landed chain prepares the next jump without repeating this one");
    const RoutePosture distant = ComputeRoutePosture(false, true, false, 65, false, false, true, false);
    Require(!distant.crouch, "next crouch does not affect a distant ordinary anchor");
    const RoutePosture authored = ComputeRoutePosture(true, false, false, 28, true, true, true, false);
    Require(authored.crouch && !authored.jump, "authored crouch also applies after landing");
    const RoutePosture recovery = ComputeRoutePosture(false, true, false, 28, true, true, true, true);
    Require(recovery.crouch && !recovery.jump, "grounded recovery preserves low-opening posture");
}
int main() {
    JumpIntoCrouchSection();
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
