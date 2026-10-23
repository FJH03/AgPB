// AgPB terminal movement policy. GPL-3.0-only.
// Route traversal keeps SyPB/CS-EBOT waypoint semantics; only the final
// grounded approach uses velocity feedback. No entity state is written.
#ifndef AGPB_ROUTE_CONTROL_H
#define AGPB_ROUTE_CONTROL_H
#include <cmath>
namespace AgPBTerminal {
constexpr float MinRadius = 4.0f;
constexpr float HeightTolerance = 4.0f;
constexpr float MaxStopSpeed = 5.0f;
constexpr float StableSeconds = 0.8f;
inline float Min(float a, float b) { return a < b ? a : b; }
inline float Max(float a, float b) { return a > b ? a : b; }
inline float Clamp(float x, float a, float b) { return Min(Max(x, a), b); }
inline bool PositionInside(float dx, float dy, float dz, float radius, bool grounded) {
    return grounded && std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
        dx * dx + dy * dy <= Max(radius, MinRadius) * Max(radius, MinRadius) &&
        std::fabs(dz) <= HeightTolerance;
}
// Aim inside the acceptance boundary so float32 motion and diagnostic
// formatting cannot leave a settled bot balanced on the exact outer edge.
// This narrows the control target; it never enlarges an authored radius.
inline bool ControlPositionInside(float dx, float dy, float dz, float radius, bool grounded) {
    const float inner = Max(radius, MinRadius) - 0.5f;
    return PositionInside(dx, dy, dz, radius, grounded) && dx * dx + dy * dy <= inner * inner;
}
inline bool CanSettle(float dx, float dy, float dz, float radius, bool grounded,
                      float vx, float vy, float vz) {
    return PositionInside(dx, dy, dz, radius, grounded) &&
        std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz) &&
        vx * vx + vy * vy + vz * vz <= MaxStopSpeed * MaxStopSpeed;
}
// SyPB JumpAction marks one jump per connection; EBot separately tracks
// jumpReady/waitForLanding. Keep their action lifecycle while Source owns
// the actual jump physics through CUserCmd (no velocity writes).
inline bool LandedJump(bool jumpLeg, bool seenAirborne, bool grounded, float dz) {
    return jumpLeg && seenAirborne && grounded && std::isfinite(dz) &&
        std::fabs(dz) <= HeightTolerance;
}
struct RoutePosture { bool jump; bool crouch; };
inline RoutePosture ComputeRoutePosture(bool authoredCrouch, bool nextCrouch,
                                        bool nextJump, float targetDistance,
                                        bool jumpLeg, bool landedJump,
                                        bool grounded, bool recovering) {
    const bool canCrouch = !jumpLeg || landedJump;
    // A nearby authored crouch section can begin before the preceding anchor.
    // In particular, a completed jump may land just short of a low opening.
    // This changes command posture only; authored flags/radii are untouched.
    bool crouch = authoredCrouch || (grounded && canCrouch && nextCrouch &&
                                    !nextJump && targetDistance < 64.0f);
    // Source needs time to unduck before takeoff. Pending/in-flight jumps
    // retain their engine-owned crouch-jump sequence without added IN_DUCK.
    if (canCrouch && nextJump && targetDistance < 120.0f) crouch = false;
    RoutePosture result = {jumpLeg && !landedJump && grounded && !recovering,
                           crouch && canCrouch};
    return result;
}
struct Wish { float x; float y; };
inline Wish ComputeWish(float dx, float dy, float vx, float vy, float interval,
                        float acceleration, float friction, float stopSpeed,
                        float maxWish, bool inside) {
    Wish result = {0.0f, 0.0f};
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(vx) ||
        !std::isfinite(vy) || !std::isfinite(interval) || interval <= 0.0f)
        return result;
    maxWish = Clamp(maxWish, 1.0f, 250.0f);
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float speed = std::sqrt(vx * vx + vy * vy);
    // Source applies ground friction before Accelerate. This is a controller
    // estimate using public cvars, not a replacement simulation. The measured
    // velocity closes the loop on surfaces and weapon/crouch speed caps.
    const float remaining = Max(0.0f, speed - Max(speed, Max(0.0f, stopSpeed)) *
                                 Max(0.0f, friction) * interval);
    const float scale = speed > 0.01f ? remaining / speed : 0.0f;
    const float desiredSpeed = inside ? 0.0f : Min(maxWish, distance * 3.5f);
    const float factor = distance > 0.001f ? desiredSpeed / distance : 0.0f;
    const float changeX = dx * factor - vx * scale;
    const float changeY = dy * factor - vy * scale;
    const float change = std::sqrt(changeX * changeX + changeY * changeY);
    if (change < 0.01f) return result;
    // Low desired velocities still need enough wish input to overcome the
    // stop-speed friction floor; using distance directly as input stalls.
    const float fraction = Clamp(acceleration * interval, 0.01f, 1.0f);
    const float wishSpeed = Min(maxWish, change / fraction);
    result.x = changeX * wishSpeed / change;
    result.y = changeY * wishSpeed / change;
    return result;
}
}
#endif
