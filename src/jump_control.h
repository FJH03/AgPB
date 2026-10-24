/*
 * AgPB authored jump launch policy. GPL-3.0-only.
 * Mechanism: SyPB 4c364fb source/waypoint.cpp jump learning and
 * source/navigate.cpp JumpAction, compared with CS-EBOT 2333562
 * source/waypoint.cpp and source/navigate.cpp jumpReady/waitForLanding.
 * Only explicitly authored directed connections may request a launch.
 *
 * The vector has SyPB's pre-jump meaning: Source adds its native vertical
 * jump impulse afterwards. Zero means ordinary engine-owned jumping and
 * must not zero the player's current velocity. Source references are only
 * physics/interface rules: player_command.cpp CheckMovingGround and
 * cstrike/cs_gamemovement.cpp PreventBunnyJumping/CheckJumpButton.
 *
 * Copyright (c) 2003-2023, by HsK-Dev Blog.
 * Copyright (c) 2003-2009, by Yet Another POD-Bot Development Team.
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef AGPB_JUMP_CONTROL_H
#define AGPB_JUMP_CONTROL_H
#include <cmath>
#include <limits>

namespace AgPBJump {
constexpr float MaximumComponent = 2000.0f;
constexpr float BunnySpeedFactor = 1.1f;
constexpr float BunnySpeedMargin = 1.0f;

struct Vector3 {
    float x, y, z;
    Vector3(float vx = 0.0f, float vy = 0.0f, float vz = 0.0f) : x(vx), y(vy), z(vz) {}
};
inline bool Finite(const Vector3 &value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
inline bool ValidVelocity(const Vector3 &value) {
    return Finite(value) && std::fabs(value.x) <= MaximumComponent &&
        std::fabs(value.y) <= MaximumComponent && std::fabs(value.z) <= MaximumComponent;
}
inline bool NonZero(const Vector3 &value) {
    return Finite(value) && (value.x != 0.0f || value.y != 0.0f || value.z != 0.0f);
}
inline bool ValidInterval(float interval) { return std::isfinite(interval) && interval > 0.0f; }

// CheckMovingGround consumes basevelocity through ApplyAbsVelocityImpulse
// before SetupMove reads absolute velocity. Unlike raw local-velocity writes,
// that engine path updates the absolute cache. The caller must first verify
// that no conveyor/external basevelocity owns this command; it must validate
// the derived impulse against any applicable engine impulse limit as well.
inline bool BaseVelocityDelta(const Vector3 &authored, const Vector3 &current,
                              float interval, Vector3 &delta) {
    delta = Vector3();
    if (!ValidVelocity(authored) || !NonZero(authored) || !Finite(current) || !ValidInterval(interval)) return false;
    const double factor = 1.0 + double(interval) * 0.5;
    const double values[] = {
        (double(authored.x) - double(current.x)) / factor,
        (double(authored.y) - double(current.y)) / factor,
        (double(authored.z) - double(current.z)) / factor
    };
    const double limit = double(std::numeric_limits<float>::max());
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(values[axis]) || std::fabs(values[axis]) > limit) return false;
    delta = Vector3(float(values[0]), float(values[1]), float(values[2]));
    return Finite(delta);
}

// PreventBunnyJumping considers the full velocity before the native jump
// impulse. StartGravity may already have subtracted half a command's gravity;
// the triangle bound covers either sign of authored Z without assuming a
// particular stance, jump height, material jump factor or stamina state.
// Returns 0 on invalid input. A zero authored vector returns the original
// limit, since ordinary jumps must not temporarily raise any player field.
// Restoring the original maxspeed immediately after RunPlayerMove is owned
// by the caller; this function never mutates a player or global cvar.
inline float BunnyMaxSpeed(const Vector3 &authored, float original,
                           float effectiveGravity, float interval) {
    if (!ValidVelocity(authored) || !std::isfinite(original) || original <= 0.0f ||
        !std::isfinite(effectiveGravity) || !ValidInterval(interval)) return 0.0f;
    if (!NonZero(authored)) return original;
    const double x = authored.x, y = authored.y, z = authored.z;
    const double length = std::sqrt(x * x + y * y + z * z);
    const double allowance = std::fabs(double(effectiveGravity)) * double(interval) * 0.5;
    const double required = (length + allowance) / double(BunnySpeedFactor) + double(BunnySpeedMargin);
    const double chosen = required > double(original) ? required : double(original);
    if (!std::isfinite(chosen) || chosen > double(std::numeric_limits<float>::max())) return 0.0f;
    return float(chosen);
}

struct State {
    int from = -1, to = -1;
    bool issued = false;
};
inline void Reset(State &state) { state = State(); }
inline void SetLeg(State &state, int from, int to) {
    if (state.from == from && state.to == to) return;
    state.from = from; state.to = to; state.issued = false;
}
inline void MarkIssued(State &state) { state.issued = true; }

struct Conditions {
    bool grounded = false;
    bool previousJump = false;
    bool paused = false;
    bool frozen = false;
    bool routeOwned = false;
    // Includes a nonzero base vector, FL_BASEVELOCITY or a moving/conveyor
    // support whose public movement path would replace the pending impulse.
    bool externalBaseVelocity = false;
};
inline bool Ready(const Conditions &conditions) {
    return conditions.grounded && conditions.routeOwned && !conditions.previousJump &&
        !conditions.paused && !conditions.frozen && !conditions.externalBaseVelocity;
}
inline bool ShouldInject(const State &state, const Vector3 &authored, const Conditions &conditions) {
    return state.from >= 0 && state.to >= 0 && state.from != state.to && !state.issued &&
        ValidVelocity(authored) && NonZero(authored) && Ready(conditions);
}
// MarkIssued only after the launch has actually been queued successfully.
// An issued leg stays spent after a failed takeoff or landing. Freeze/task
// suspension drops a pending write but must retain this bit for the same leg;
// reset on route cancellation/owner replacement or select a different leg.
// Zero-vector ordinary jumps intentionally bypass this injection-only gate;
// their view snap and existing CUserCmd posture remain the caller's policy.
}
#endif
