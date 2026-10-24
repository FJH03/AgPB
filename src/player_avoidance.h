/*
 * AgPB normal-mode player avoidance policy. GPL-3.0-only.
 * Baseline: SyPB 4c364fb source/navigate.cpp CheckCloseAvoidance/CheckTerrain.
 * Compared with CS-EBOT 2333562 source/navigate.cpp GetPlayerPriority and
 * CheckTerrain: human-first ordering, bounded motion prediction and side probes.
 * Zombie/semiclip/NPC behavior and automatic waypoint creation are excluded.
 * AgPB adaptation applies to authorized travel without SyPB's recent-enemy
 * gate. Caller owns full player identities, world hull/floor probes,
 * task authority, and replanning along the existing manually authored graph.
 * This policy changes only a world movement wish, never buttons or posture.
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
    int userid = 0, slot = 0, team = 0, priority = 0;
    Vec2 position, velocity;
    float bottom = 0.0f, top = 0.0f; // World-space hull heights, not offsets.
    Vec2 intent;                   // World desired direction/wish; zero falls back to velocity.
};
struct Input {
    float now = 0.0f, interval = 0.0f, maxSpeed = 0.0f;
    Vec2 position, velocity, wish; // Original command wish, before avoidance.
    float bottom = 0.0f, top = 0.0f;
    int team = 0, priority = 0;     // EBot ordering: smaller wins; humans 1, AgPB slot+3.
    std::uint64_t owner = 0;       // Current task/route authority; zero cannot retain state.
    bool ducking = false;
};
struct Contact {
    bool valid = false;
    std::uint64_t handle = 0;
    int userid = 0, slot = 0, priority = 0;
    bool yielding = false;
    float distance = 0.0f;
    int preferredSide = 0;         // +1 right, -1 left in the original wish basis.
    bool headOn = false;
};
struct State {
    std::uint64_t owner = 0, handle = 0;
    int userid = 0, slot = 0;
    float lastTime = -1.0f, blockedSince = -1.0f;
    int side = 0;
    float sideUntil = 0.0f, nextReplanAt = 0.0f;
};
struct Probes {
    // Each flag certifies the entire corresponding CandidateWish, including
    // players, walls, legal step height and ground support. Unset means unsafe.
    bool right = false, left = false, back = false, rightBack = false, leftBack = false;
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
        std::isfinite(in.bottom) && std::isfinite(in.top) && in.top > in.bottom &&
        (in.team == 2 || in.team == 3) && in.priority > 0;
}
inline float PredictionSeconds(const Input &in) {
    // EBot scales the movement frame by 10 when running, 5 when slow/ducked.
    // Source adaptation caps the public prediction horizon at .2 seconds.
    const float multiplier = !in.ducking && Length(in.velocity) > 14.0f ? 10.0f : 5.0f;
    const float predicted = in.interval * multiplier;
    return predicted < 0.2f ? predicted : 0.2f;
}
inline Vec2 CandidateWish(const Input &in, int side, bool retreat = false) {
    // Shared with the caller so the tested hull vector is the submitted vector.
    // side=0,retreat=true is straight back; side=0,false is the original wish.
    if (!Valid(in)) return Vec2();
    const Vec2 original = Limit(in.wish, in.maxSpeed);
    const float speed = Length(original);
    if (speed <= 0.0001f || side == 0) return retreat ? Scale(original, -1.0f) : original;
    const Vec2 forward = Unit(original), right(forward.y, -forward.x);
    const float sign = side > 0 ? 1.0f : -1.0f;
    const Vec2 direction = Add(Scale(forward, retreat ? -0.7f : 0.35f),
        Scale(right, sign * (retreat ? 0.7f : 1.0f)));
    return Limit(Scale(Unit(direction), speed), speed);
}
inline Contact FindContact(const Input &in, const Neighbor *neighbors, std::size_t count) {
    Contact best;
    if (!Valid(in) || neighbors == NULL || Length(in.wish) <= 0.0001f) return best;
    const Vec2 wish = Limit(in.wish, in.maxSpeed), forward = Unit(wish), right(forward.y, -forward.x);
    const float dt = in.interval < 0.2f ? in.interval : 0.2f;
    const float horizon = PredictionSeconds(in);
    const Vec2 selfMotion = Add(wish, in.velocity);
    const Vec2 shortSelf = Add(in.position, Scale(selfMotion, dt));
    const Vec2 futureSelf = Add(in.position, Scale(selfMotion, horizon));
    for (std::size_t i = 0; i < count; ++i) {
        const Neighbor &n = neighbors[i];
        if (n.handle == 0 || n.userid <= 0 || n.slot <= 0 || n.priority <= 0 || n.team != in.team ||
            !Finite(n.position) || !Finite(n.velocity) || !Finite(n.intent) ||
            !std::isfinite(n.bottom) || !std::isfinite(n.top) || n.top <= n.bottom ||
            n.bottom >= in.top || n.top <= in.bottom) continue;
        const Vec2 offset = Sub(n.position, in.position);
        const float distance = Length(offset);
        if (!std::isfinite(distance)) continue;
        // A teammate already behind and separating from our actual travel
        // must not make us swerve back into them. A faster rear catch-up remains.
        const Vec2 relativeMotion = Sub(n.velocity, selfMotion);
        if (Dot(offset, forward) < 0.0f && Dot(offset, relativeMotion) >= 0.0f) continue;
        const Vec2 shortOther = Add(n.position, Scale(n.velocity, dt));
        const Vec2 futureOther = Add(n.position, Scale(n.velocity, horizon));
        const bool close = Length(Sub(n.position, shortSelf)) <= 48.0f ||
            (distance <= 56.0f && Length(Sub(shortOther, in.position)) <= distance);
        const bool predicted = Length(Sub(n.position, futureSelf)) < 64.0f ||
            Length(Sub(futureOther, futureSelf)) < 56.0f ||
            (distance < 72.0f && Length(Sub(futureOther, in.position)) < distance);
        if (!close && !predicted) continue;
        // Choose the nearest threat regardless of priority. Stable tie breaks
        // avoid dependence on entity enumeration order in a crowded doorway.
        if (best.valid && (distance > best.distance || (distance == best.distance &&
            (n.priority > best.priority || (n.priority == best.priority &&
            (n.slot > best.slot || (n.slot == best.slot &&
            (n.userid > best.userid || (n.userid == best.userid && n.handle >= best.handle))))))))) continue;
        best.valid = true; best.handle = n.handle; best.userid = n.userid; best.slot = n.slot;
        best.priority = n.priority; best.yielding = in.priority > n.priority; best.distance = distance;
        best.preferredSide = Dot(Scale(offset, -1.0f), right) > 0.0f ? 1 : -1;
        const Vec2 otherDirection = Unit(Length(n.intent) > 0.0001f ? n.intent : n.velocity);
        best.headOn = Dot(forward, otherDirection) < -0.5f;
    }
    return best;
}
inline Output Step(State &state, const Input &in, const Contact &contact, const Probes &probes) {
    Output out;
    if (!Valid(in)) { Reset(state); out.reason = "invalid"; return out; }
    out.wish = Limit(in.wish, in.maxSpeed);
    if (in.owner == 0 || Length(out.wish) <= 0.0001f || !contact.valid || contact.handle == 0 ||
        contact.userid <= 0 || contact.slot <= 0 || contact.priority <= 0 ||
        !std::isfinite(contact.distance) || contact.distance < 0.0f ||
        (contact.preferredSide != 1 && contact.preferredSide != -1)) {
        Reset(state);
        out.reason = in.owner == 0 ? "unowned" : Length(out.wish) <= 0.0001f ? "idle" : "clear";
        return out;
    }
    if (state.owner != in.owner || !std::isfinite(state.lastTime) || in.now < state.lastTime ||
        !std::isfinite(state.blockedSince) || !std::isfinite(state.sideUntil) ||
        !std::isfinite(state.nextReplanAt) || (state.side != 0 && state.side != 1 && state.side != -1)) {
        Reset(state);
        state.owner = in.owner;
        state.blockedSince = in.now; state.nextReplanAt = in.now + 5.0f;
    }
    if (state.handle != contact.handle || state.userid != contact.userid || state.slot != contact.slot) {
        // Side choice belongs to this complete player identity. Preserve the
        // owner's continuous blocked budget when a crowd changes its nearest
        // threat; otherwise rotating blockers could postpone recovery forever.
        state.handle = contact.handle; state.userid = contact.userid; state.slot = contact.slot;
        state.side = 0; state.sideUntil = 0.0f;
    }
    state.lastTime = in.now;
    out.active = true; out.yielding = in.priority > contact.priority;
    // Waiting with all probes blocked is still blocked time. Requests are
    // periodic, with no catch-up burst; the caller retains goal/owner authority.
    if (in.now >= state.nextReplanAt) { out.requestReplan = true; state.nextReplanAt = in.now + 5.0f; }
    if (!out.yielding) { state.side = 0; state.sideUntil = 0.0f; out.reason = "priority"; return out; }
    int side = state.side != 0 && in.now < state.sideUntil ? state.side : contact.preferredSide;
    bool retreat = false, available = side > 0 ? probes.right : probes.left;
    if (!available && (side > 0 ? probes.left : probes.right)) { side = -side; available = true; }
    if (!available) {
        retreat = true;
        available = side > 0 ? probes.rightBack : probes.leftBack;
        if (!available && (side > 0 ? probes.leftBack : probes.rightBack)) { side = -side; available = true; }
    }
    if (state.side != side || in.now >= state.sideUntil) state.sideUntil = in.now + 0.5f;
    state.side = side;
    if (available) {
        out.wish = CandidateWish(in, side, retreat);
        out.reason = retreat ? (side > 0 ? "yield-right-back" : "yield-left-back")
                             : (side > 0 ? "yield-right" : "yield-left");
    } else if (probes.back) { out.wish = CandidateWish(in, 0, true); out.reason = "yield-back"; }
    else { out.wish = Vec2(); out.reason = "yield-wait"; }
    return out;
}
}
#endif
