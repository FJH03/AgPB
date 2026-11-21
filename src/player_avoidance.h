/*
 * AgPB normal-mode player avoidance policy. GPL-3.0-only.
 * Baseline: SyPB 4c364fb source/navigate.cpp CheckCloseAvoidance/CheckTerrain.
 * Compared with CS-EBOT 2333562 source/navigate.cpp GetPlayerPriority and
 * CheckTerrain for human/slot priority and side probes during physical jams.
 * The caller gathers living teammates and checks the world hull and floor
 * before submitting a changed movement wish. Recovery requests retain task
 * authority and use the existing manually authored graph.
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
#ifndef AGPB_PLAYER_AVOIDANCE_H
#define AGPB_PLAYER_AVOIDANCE_H
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace AgPBPlayerAvoid {
struct Vec2 {
    float x, y;
    Vec2(float xValue = 0.0f, float yValue = 0.0f) : x(xValue), y(yValue) {}
};
inline Vec2 Add(Vec2 a, Vec2 b) { return Vec2(a.x + b.x, a.y + b.y); }
inline Vec2 Sub(Vec2 a, Vec2 b) { return Vec2(a.x - b.x, a.y - b.y); }
inline Vec2 Scale(Vec2 v, float scale) { return Vec2(v.x * scale, v.y * scale); }
inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float Length(Vec2 v) { return std::hypot(v.x, v.y); }
inline bool Finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline Vec2 Unit(Vec2 v) {
    const float length = Length(v);
    return std::isfinite(length) && length > 0.0001f ? Scale(v, 1.0f / length) : Vec2();
}
inline Vec2 Limit(Vec2 v, float maximum) {
    const float length = Length(v);
    if (!Finite(v) || !std::isfinite(length) || !std::isfinite(maximum) || maximum <= 0.0f) return Vec2();
    return length > maximum ? Scale(v, maximum / length) : v;
}
struct Neighbor {
    std::uint64_t handle = 0;
    int userid = 0, slot = 0, team = 0;
    Vec2 position, velocity, wish;  // Most recent authorized travel wish before avoidance.
    Vec2 hullMins, hullMaxs;        // Horizontal hull offsets from position.
    float z = 0.0f;
    float bottom = 0.0f, top = 0.0f; // World-space hull heights, not offsets.
    bool canYield = false;          // A managed bot currently eligible for ground avoidance.
};
struct Input {
    float now = 0.0f, interval = 0.0f, maxSpeed = 0.0f;
    Vec2 position, velocity, wish; // Original command wish, before avoidance.
    Vec2 hullMins, hullMaxs;
    float z = 0.0f;
    float bottom = 0.0f, top = 0.0f;
    int team = 0, slot = 0;
    std::uint64_t owner = 0;       // Current task/route authority; zero cannot retain state.
    bool ducking = false, recentEnemy = false;
};
struct Contact {
    bool valid = false;
    std::uint64_t handle = 0;
    int userid = 0, slot = 0;
    int preferredSide = 0;         // +1 right, -1 left in the original wish basis.
    Vec2 approach;                 // Initial world direction of a physical jam.
    bool retreat = false, physical = false, yielding = true;
};
struct State {
    std::uint64_t owner = 0, handle = 0;
    int userid = 0, slot = 0;
    float lastTime = -1.0f, blockedSince = -1.0f;
    int side = 0;
    float nextReplanAt = 0.0f;
    Vec2 approach;
    bool physical = false, yielding = false;
};
struct Probes {
    // Each flag certifies the entire corresponding wish, including
    // players, walls, legal step height and ground support. Unset means unsafe.
    bool right = false, left = false, back = false, rightBack = false, leftBack = false;
    bool rightOnly = false, leftOnly = false;
};
struct Output {
    Vec2 wish;
    bool active = false, yielding = false, requestReplan = false;
    const char *reason = "clear";
};
inline void Reset(State &state) { state = State(); }
inline bool Valid(const Input &in) {
    return std::isfinite(in.now) && in.now >= 0.0f && std::isfinite(in.interval) && in.interval > 0.0f &&
        std::isfinite(in.maxSpeed) && in.maxSpeed > 0.0f && Finite(in.position) && Finite(in.velocity) &&
        Finite(in.wish) && std::isfinite(Length(in.wish)) && std::isfinite(Length(in.velocity)) &&
        std::isfinite(in.z) && std::isfinite(in.bottom) && std::isfinite(in.top) && in.top > in.bottom &&
        (in.team == 2 || in.team == 3);
}
inline Vec2 CandidateWish(const Input &in, int side, bool retreat = false) {
    // Shared with the caller so the tested hull vector is the submitted vector.
    if (!Valid(in)) return Vec2();
    const Vec2 original = Limit(in.wish, in.maxSpeed);
    const float speed = Length(original);
    if (speed <= 0.0001f || side == 0) return retreat ? Scale(original, -1.0f) : original;
    const Vec2 forward = Unit(original), right(forward.y, -forward.x);
    const float sign = side > 0 ? 1.0f : -1.0f;
    // Keep SyPB's relative forward and strafe magnitudes while respecting the
    // Source command's existing speed budget (including walking and ducking).
    const Vec2 direction = Add(Scale(forward, retreat ? -1.0f : speed / in.maxSpeed), Scale(right, sign));
    return Limit(Scale(Unit(direction), speed), speed);
}
inline Vec2 SideWish(const Input &in, int side) {
    if (!Valid(in) || side == 0) return Vec2();
    const Vec2 wish = Limit(in.wish, in.maxSpeed);
    return side > 0 ? Vec2(wish.y, -wish.x) : Vec2(-wish.y, wish.x);
}
inline Input SteeringInput(const Input &in, const Contact &contact) {
    Input steering = in;
    // A route wish can turn sharply around an occupied waypoint. Keep the
    // escape direction fixed while taking the speed budget from the current wish.
    if (contact.physical && Length(contact.approach) > 0.0001f)
        steering.wish = Scale(contact.approach, Length(Limit(in.wish, in.maxSpeed)));
    return steering;
}
inline bool ValidHull(Vec2 mins, Vec2 maxs) {
    return Finite(mins) && Finite(maxs) && mins.x < maxs.x && mins.y < maxs.y;
}
inline bool ClipSweepAxis(float heading, float mins, float maxs, float &enter, float &leave) {
    if (std::fabs(heading) < 0.0001f) return mins < 0.0f && maxs > 0.0f;
    const float a = mins / heading, b = maxs / heading;
    enter = std::fmax(enter, std::fmin(a, b));
    leave = std::fmin(leave, std::fmax(a, b));
    return enter < leave;
}
inline float PlayerSweep(Vec2 offset, Vec2 heading, Vec2 mins, Vec2 maxs, float distance) {
    // Sweep the moving origin against the neighbor box expanded by our hull.
    // Exact tangency and any movement already separating an overlap can pass.
    const bool overlapping = mins.x < 0.0f && maxs.x > 0.0f && mins.y < 0.0f && maxs.y > 0.0f;
    if (Length(heading) < 0.0001f || (overlapping && Length(offset) > 0.0001f && Dot(offset, heading) <= 0.0f))
        return -1.0f;
    float enter = 0.0f, leave = distance;
    return ClipSweepAxis(heading.x, mins.x, maxs.x, enter, leave) &&
        ClipSweepAxis(heading.y, mins.y, maxs.y, enter, leave) ? enter : -1.0f;
}
inline bool SamePlayer(const State &state, const Neighbor &n) {
    return state.handle == n.handle && state.userid == n.userid && state.slot == n.slot;
}
inline Contact FindContact(const Input &in, const Neighbor *neighbors, std::size_t count, const State *state = nullptr) {
    Contact contact;
    if (!Valid(in) || neighbors == NULL || Length(in.wish) <= 0.0001f) return contact;
    constexpr float contactSweep = 12.0f;
    constexpr float recoverySweep = 40.0f;
    const Vec2 wish = Limit(in.wish, in.maxSpeed), forward = Unit(wish), right(forward.y, -forward.x);
    const bool scanPhysical = ValidHull(in.hullMins, in.hullMaxs);
    const Neighbor *nearest = nullptr;
    const Neighbor *blocker = nullptr;
    float nearestDistance = in.maxSpeed;
    float blockerDistance = 0.0f, blockerSweep = 0.0f;
    Vec2 approach;
    bool yields = true;
    for (std::size_t i = 0; i < count; ++i) {
        const Neighbor &n = neighbors[i];
        if (n.handle == 0 || n.userid <= 0 || n.slot <= 0 || n.team != in.team ||
            !Finite(n.position) || !Finite(n.velocity) || !std::isfinite(n.z) ||
            !std::isfinite(n.bottom) || !std::isfinite(n.top) || n.top <= n.bottom ||
            n.bottom >= in.top || n.top <= in.bottom) continue;
        const Vec2 offset = Sub(n.position, in.position);
        const float distance = std::hypot(Length(offset), n.z - in.z);
        const auto identity = std::tie(n.slot, n.userid, n.handle);
        if (in.recentEnemy && distance < in.maxSpeed && (distance < nearestDistance ||
            (nearest && distance == nearestDistance && identity < std::tie(nearest->slot, nearest->userid, nearest->handle)))) {
            nearest = &n;
            nearestDistance = distance;
        }
        if (!scanPhysical || !ValidHull(n.hullMins, n.hullMaxs)) continue;
        const Vec2 mins = Sub(Add(offset, n.hullMins), in.hullMaxs);
        const Vec2 maxs = Sub(Add(offset, n.hullMaxs), in.hullMins);
        const bool retained = state && state->physical && state->owner == in.owner && in.now >= state->lastTime &&
            SamePlayer(*state, n);
        // Retain the same obstruction beyond the initial sweep while a detour
        // still crosses its hull. A small retreat otherwise reissues the jammed wish.
        const float sweep = PlayerSweep(offset, forward, mins, maxs, retained ? recoverySweep : contactSweep);
        if (sweep < 0.0f || (blocker && std::tie(sweep, distance, n.slot, n.userid, n.handle) >=
            std::tie(blockerSweep, blockerDistance, blocker->slot, blocker->userid, blocker->handle))) continue;
        blocker = &n;
        blockerDistance = distance;
        blockerSweep = sweep;
        approach = retained ? state->approach : forward;
        // Both authorized wishes must lead into the other hull before the
        // lower slot waits for the other to move. Humans and holding bots keep priority.
        const bool opposing = n.canYield && (PlayerSweep(Scale(offset, -1.0f), Unit(n.wish),
            Scale(maxs, -1.0f), Scale(mins, -1.0f), contactSweep) >= 0.0f || (retained && !state->yielding));
        yields = !opposing || in.slot <= 0 || in.slot >= n.slot;
    }
    if (blocker) {
        contact.valid = contact.physical = true;
        contact.yielding = yields;
        contact.handle = blocker->handle;
        contact.userid = blocker->userid;
        contact.slot = blocker->slot;
        contact.approach = approach;
        const Vec2 escapeRight(approach.y, -approach.x);
        contact.preferredSide = Dot(Sub(blocker->position, in.position), escapeRight) < 0.0f ? 1 : -1;
        return contact;
    }
    if (nearest == nullptr) return contact;

    // SyPB checks the nearest teammate using one movement frame in XY.
    const Vec2 offset = Sub(nearest->position, in.position);
    const float distance = Length(offset);
    const float movedDistance = Length(Sub(offset, Scale(Add(wish, in.velocity), in.interval)));
    const float nextDistance = Length(Add(offset, Scale(nearest->velocity, in.interval)));
    if (!(movedDistance <= 48.0f || (distance <= 56.0f && nextDistance <= distance))) return contact;

    contact.valid = true;
    contact.handle = nearest->handle;
    contact.userid = nearest->userid;
    contact.slot = nearest->slot;
    contact.preferredSide = Dot(offset, right) < 0.0f ? 1 : -1;
    contact.retreat = distance < 56.0f && Dot(offset, forward) > 0.0f;
    return contact;
}
inline int AvailableSide(int preferred, bool right, bool left) {
    if (preferred > 0 ? right : left) return preferred;
    return (preferred > 0 ? left : right) ? -preferred : 0;
}
inline Output Step(State &state, const Input &in, const Contact &contact, const Probes &probes) {
    Output out;
    if (!Valid(in)) { Reset(state); out.reason = "invalid"; return out; }
    out.wish = Limit(in.wish, in.maxSpeed);
    if (in.owner == 0 || Length(out.wish) <= 0.0001f || !contact.valid || contact.handle == 0 ||
        contact.userid <= 0 || contact.slot <= 0 ||
        (contact.preferredSide != 1 && contact.preferredSide != -1)) {
        Reset(state);
        out.reason = in.owner == 0 ? "unowned" : Length(out.wish) <= 0.0001f ? "idle" : "clear";
        return out;
    }
    if (state.owner != in.owner || !std::isfinite(state.lastTime) || in.now < state.lastTime ||
        !std::isfinite(state.blockedSince) || !std::isfinite(state.nextReplanAt)) {
        Reset(state);
        state.owner = in.owner;
        state.blockedSince = in.now; state.nextReplanAt = in.now + 5.0f;
    }
    // A change of nearest teammate keeps the owner's continuous recovery budget.
    if (!state.physical || !contact.physical || state.handle != contact.handle ||
        state.userid != contact.userid || state.slot != contact.slot) state.side = 0;
    state.handle = contact.handle; state.userid = contact.userid; state.slot = contact.slot;
    state.lastTime = in.now;
    state.physical = contact.physical;
    state.approach = contact.approach;
    state.yielding = out.yielding = contact.yielding;
    out.active = true;
    // Waiting with all probes blocked is still blocked time. Requests are
    // periodic, with no catch-up burst; the caller retains goal/owner authority.
    if (in.now >= state.nextReplanAt) { out.requestReplan = true; state.nextReplanAt = in.now + 5.0f; }
    if (!out.yielding) {
        out.wish = Vec2();
        out.reason = "player-priority-wait";
        return out;
    }
    const int preferred = state.side != 0 ? state.side : contact.preferredSide;
    const Input steering = SteeringInput(in, contact);
    int side = 0;
    if (contact.physical) side = AvailableSide(preferred, probes.rightOnly, probes.leftOnly);
    else if (!contact.retreat) side = AvailableSide(preferred, probes.right, probes.left);
    const bool retreat = side == 0;
    if (retreat) side = AvailableSide(preferred, probes.rightBack, probes.leftBack);
    if (side != 0) {
        state.side = side;
        out.wish = contact.physical && !retreat ? SideWish(steering, side) : CandidateWish(steering, side, retreat);
        out.reason = retreat ? (side > 0 ? "yield-right-back" : "yield-left-back")
            : (side > 0 ? "yield-right" : "yield-left");
    } else if (probes.back) { out.wish = CandidateWish(steering, 0, true); out.reason = "yield-back"; }
    else { out.wish = Vec2(); out.reason = "yield-wait"; }
    return out;
}
}
#endif
