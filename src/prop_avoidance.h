// SPDX-License-Identifier: GPL-3.0-only
/*
 * Ground-physics-prop recovery, independently adapted from SyPB 4c364fb
 * source/navigate.cpp CheckTerrain/ResetCollideState and compared with
 * CS-EBOT 2333562 source/navigate.cpp CheckTerrain. Retains bounded side
 * probes and collision memory; excludes their automatic jump/duck/NPC/ZM
 * branches. The caller certifies actual hull/step/floor geometry and owns
 * route authority. Only an already nonzero navigation wish can be changed.
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
#ifndef AGPB_PROP_AVOIDANCE_H
#define AGPB_PROP_AVOIDANCE_H
#include <cmath>
#include <cstdint>

namespace AgPBPropAvoid {
struct Vec2 {
    float x, y;
    Vec2(float xValue = 0, float yValue = 0) : x(xValue), y(yValue) {}
};
inline bool Finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline float Length(Vec2 v) { return std::hypot(v.x, v.y); }
inline Vec2 Scale(Vec2 v, float s) { return Vec2(v.x * s, v.y * s); }
inline Vec2 Unit(Vec2 v) {
    const float length = Length(v);
    return Finite(v) && std::isfinite(length) && length > 0.0001f ? Scale(v, 1.0f / length) : Vec2();
}
inline bool KnownBreakHealth(bool healthKnown, int health) { return healthKnown && health > 0; }
inline bool HasAlternativeRoute(const int *owned, int ownedCount, int current,
                                const int *candidate, int candidateCount) {
    if (owned == nullptr || candidate == nullptr || current < 0 || current >= ownedCount ||
        candidateCount <= 0 || candidate[candidateCount - 1] != owned[ownedCount - 1]) return false;
    int common = 0;
    while (common < ownedCount && common < candidateCount &&
        owned[ownedCount - 1 - common] == candidate[candidateCount - 1 - common]) ++common;
    // Rejoining an old anchor or prepending anchors to the same route cannot
    // bypass the obstruction; neither can walking directly to its old target.
    if (common == ownedCount || common == candidateCount || candidate[0] == owned[current]) return false;
    if (current > 0)
        for (int i = 1; i < candidateCount; ++i)
            if (candidate[i - 1] == owned[current - 1] && candidate[i] == owned[current]) return false;
    return true;
}
struct Input {
    float now = 0, maxSpeed = 0, goalDistance = 0;
    std::uint64_t owner = 0, blocker = 0;
    int goal = -1, preferredSide = 1;
    Vec2 wish;
    bool permitted = false;
    bool blockedByProp = false; // Real nearby collision, not an OBB proximity guess.
    bool forwardClear = false; // Full hull + legal steps + supported floor.
};
struct State {
    std::uint64_t owner = 0, blocker = 0;
    int goal = -1, side = 0;
    float lastTime = -1, sampleAt = -1, sampleDistance = 0;
    float started = -1, clearSince = -1, sideUntil = 0;
    bool active = false, replanned = false;
};
struct Probes {
    bool right = false, left = false, rightBack = false, leftBack = false, back = false;
};
struct Output {
    Vec2 wish;
    bool active = false, requestReplan = false, failed = false;
    const char *reason = "clear";
};
inline void Reset(State &state) { state = State(); }
inline bool Valid(const Input &in) {
    return std::isfinite(in.now) && in.now >= 0 && std::isfinite(in.maxSpeed) && in.maxSpeed > 0 &&
        std::isfinite(in.goalDistance) && in.goalDistance >= 0 && Finite(in.wish) &&
        std::isfinite(Length(in.wish)) && in.owner != 0 && in.goal >= 0;
}
inline Vec2 OriginalWish(const Input &in) {
    const float speed = Length(in.wish);
    if (!Finite(in.wish) || !std::isfinite(speed) || !std::isfinite(in.maxSpeed) || in.maxSpeed <= 0) return Vec2();
    return speed > in.maxSpeed ? Scale(in.wish, in.maxSpeed / speed) : in.wish;
}
inline Vec2 CandidateWish(const Input &in, int side, bool retreat = false) {
    if (!Valid(in)) return Vec2();
    const Vec2 original = OriginalWish(in);
    const float speed = Length(original);
    if (speed <= 0.0001f) return Vec2();
    if (side == 0) return retreat ? Scale(original, -1) : original;
    const Vec2 forward = Unit(original), right(forward.y, -forward.x);
    const float sign = side > 0 ? 1.0f : -1.0f;
    const float along = retreat ? -0.7f : 0.2f, across = retreat ? 0.7f : 1.0f;
    return Scale(Unit(Vec2(forward.x * along + right.x * sign * across,
        forward.y * along + right.y * sign * across)), speed);
}
inline bool NeedsProbes(const State &state, const Input &in) {
    return Valid(in) && in.permitted && !in.forwardClear && Length(OriginalWish(in)) > 0.01f &&
        (state.active || (in.blockedByProp && state.owner == in.owner && state.goal == in.goal &&
        state.sampleAt >= 0 && in.now - state.sampleAt >= 0.3f && state.sampleDistance - in.goalDistance < 2.0f));
}
inline Output Step(State &state, const Input &in, const Probes &probes) {
    Output out;
    out.wish = OriginalWish(in);
    if (!Valid(in) || !in.permitted || Length(out.wish) <= 0.01f) {
        Reset(state); out.reason = "ineligible"; return out;
    }
    if (state.owner != in.owner || state.goal != in.goal || !std::isfinite(state.lastTime) ||
        in.now < state.lastTime || !std::isfinite(state.sampleAt) || !std::isfinite(state.started) ||
        !std::isfinite(state.clearSince) || !std::isfinite(state.sideUntil)) {
        Reset(state); state.owner = in.owner; state.goal = in.goal;
    }
    state.lastTime = in.now;
    if (!state.active) {
        if (!in.blockedByProp || in.blocker == 0 || in.forwardClear) {
            Reset(state); out.reason = "clear"; return out;
        }
        state.blocker = in.blocker;
        if (state.sampleAt < 0) { state.sampleAt = in.now; state.sampleDistance = in.goalDistance; }
        if (in.now - state.sampleAt < 0.3f) { out.reason = "prop-observe"; return out; }
        if (state.sampleDistance - in.goalDistance >= 2.0f) {
            // A prop which can be pushed while progressing is not a stuck event.
            state.sampleAt = in.now; state.sampleDistance = in.goalDistance;
            out.reason = "prop-progress"; return out;
        }
        state.active = true; state.started = in.now;
        state.side = in.preferredSide < 0 ? -1 : 1;
        state.sideUntil = in.now + 0.5f;
    }
    if (in.forwardClear) {
        // Preserve the episode through one-frame clearance/contact flicker.
        if (state.clearSince < 0) state.clearSince = in.now;
        out.reason = "prop-rejoin";
        if (in.now - state.clearSince >= 0.3f) Reset(state);
        return out;
    }
    state.clearSince = -1;
    if (in.blockedByProp && in.blocker != 0) state.blocker = in.blocker;
    out.active = true;
    if (in.now - state.started >= 6.0f) {
        out.wish = Vec2(); out.failed = true; out.reason = "prop-timeout"; return out;
    }
    if (!state.replanned && in.now - state.started >= 3.0f) {
        state.replanned = true; out.requestReplan = true;
    }
    int side = state.side != 0 ? state.side : (in.preferredSide < 0 ? -1 : 1);
    bool available = side > 0 ? probes.right : probes.left, retreat = false;
    // Keep collision memory on the same side while rounding a corner. A
    // forward-side probe may become blocked before the hull has cleared the
    // prop; reversing the strafe immediately creates left/right oscillation.
    // SyPB and CS-EBOT retain a chosen collision action across commands.
    if (!available) {
        retreat = true; available = side > 0 ? probes.rightBack : probes.leftBack;
    }
    if (!available) {
        side = -side; retreat = false;
        available = side > 0 ? probes.right : probes.left;
        if (!available) { retreat = true; available = side > 0 ? probes.rightBack : probes.leftBack; }
        if (!available) side = -side;
    }
    if (side != state.side || in.now >= state.sideUntil) state.sideUntil = in.now + 0.5f;
    state.side = side;
    if (available) {
        out.wish = CandidateWish(in, side, retreat);
        out.reason = retreat ? (side > 0 ? "prop-right-back" : "prop-left-back")
                             : (side > 0 ? "prop-right" : "prop-left");
    } else if (probes.back) { out.wish = CandidateWish(in, 0, true); out.reason = "prop-back"; }
    else { out.wish = Vec2(); out.reason = "prop-wait"; }
    return out;
}
}
#endif
