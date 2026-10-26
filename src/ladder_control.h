// AgPB authored ladder traversal policy. GPL-3.0-only.
// Baseline: SyPB 4c364fb source/navigate.cpp DoWaypointNav,
// SetWaypointOrigin and source/basecode.cpp ChooseAimDirection; compared
// with CS-EBOT 2333562 source/navigate.cpp and source/basecode.cpp.
// Source gamemovement.cpp is consulted only for CUserCmd/ladder physics.
// No engine bot AI, graph generation or entity-state writes are used.
// Copyright (c) 2003-2023, by HsK-Dev Blog.
// Copyright (c) 2003-2009, by Yet Another POD-Bot Development Team.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OF THE SOFTWARE.
#ifndef AGPB_LADDER_CONTROL_H
#define AGPB_LADDER_CONTROL_H
#include <cmath>
#include <cstdint>

namespace AgPBLadder {
enum class Stage { Idle, Approach, Traverse, Dismount };
struct State {
    Stage stage = Stage::Idle;
    int first = -1, last = -1, exit = -1;
    std::uint64_t owner = 0;
    bool ascending = true, attached = false;
    float faceYaw = 0.0f;
    float started = 0.0f, progressAt = 0.0f, bestDistance = -1.0f;
    float duration = 0.0f;
    bool Active() const { return stage != Stage::Idle; }
};
inline void Reset(State &state) { state = State(); }
inline const char *Name(Stage stage) {
    switch (stage) {
    case Stage::Approach: return "approach";
    case Stage::Traverse: return "traverse";
    case Stage::Dismount: return "dismount";
    default: return "idle";
    }
}
inline void Begin(State &state, int first, int last, int exit,
                  std::uint64_t owner, float fromZ, float toZ, float yaw, float now) {
    Reset(state);
    state.stage = Stage::Approach;
    state.first = first; state.last = last; state.exit = exit; state.owner = owner;
    state.ascending = toZ > fromZ; state.faceYaw = yaw;
    state.started = state.progressAt = now;
    // A bounded approach plus the authored climb length. Progress must still
    // occur within four seconds, so a tall ladder cannot hide a failed mount.
    state.duration = 10.0f + std::fabs(toZ - fromZ) / 80.0f;
    if (state.duration > 45.0f) state.duration = 45.0f;
}
inline void SetStage(State &state, Stage stage, float now) {
    if (state.stage == stage) return;
    state.stage = stage; state.bestDistance = -1.0f; state.progressAt = now;
}
inline void Progress(State &state, float distance, float now) {
    if (!std::isfinite(distance) || !std::isfinite(now)) return;
    if (state.bestDistance < 0.0f || state.bestDistance - distance >= 1.0f) {
        state.bestDistance = distance; state.progressAt = now;
    }
}
inline bool TimedOut(const State &state, float now) {
    return !std::isfinite(now) || now < state.started ||
        now - state.started > state.duration || now - state.progressAt > 4.0f;
}
inline bool HeightReached(bool ascending, float currentZ, float targetZ, float tolerance = 2.0f) {
    if (!std::isfinite(currentZ) || !std::isfinite(targetZ)) return false;
    return ascending ? currentZ >= targetZ - tolerance : currentZ <= targetZ + tolerance;
}
inline bool EntryReached(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        x * x + y * y + z * z <= 15.0f * 15.0f;
}
inline bool CanAdvanceUnattachedEntry(bool ascending, float x, float y, float z) {
    // A descending top point is an attachment target, not a ground-arrival
    // sphere. Consuming it while still supported turns the bot back into the
    // platform before its hull crosses the lip. Keep approaching until Source
    // reports a real ladder attachment; ascending ground entry is unchanged.
    return ascending && EntryReached(x, y, z);
}
struct Wish { float pitch, forward, side; };
inline float MountPitch(bool ascending) {
    // SyPB looks toward its 3D ladder destination. An upward approach also
    // keeps Source's two-unit attachment hull sweep above a rising floor;
    // a horizontal sweep can hit that floor before reaching the ladder face.
    // Descending retains a horizontal mount so the sweep sees the side face
    // instead of the non-climbable top face.
    return ascending ? -30.0f : 0.0f;
}
inline Wish TraverseWish(bool ascending, float lateralError) {
    // A zero-pitch inward wish always climbs in Source. Descending requires
    // looking down far enough that its down component exceeds the inward
    // component converted into upward motion. Buttons must accompany these
    // movement amounts: LadderMove reads directional buttons for climb speed.
    Wish wish = {ascending ? -60.0f : 80.0f, 200.0f, 0.0f};
    if (std::isfinite(lateralError) && std::fabs(lateralError) > 2.0f)
        wish.side = lateralError > 0.0f ? 200.0f : -200.0f;
    return wish;
}
inline float DismountPitch(bool ascending, bool attached) {
    // Even when the exit lies away from the face, looking up this far keeps
    // the converted ladder velocity positive until the hull clears the lip.
    // A grounded bottom exit instead moves away from the ladder naturally.
    return ascending && attached ? -60.0f : 0.0f;
}
}
#endif
