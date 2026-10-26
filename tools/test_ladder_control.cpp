// Standalone regression for authored ladders and the Source movement contract.
#include "../src/ladder_control.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static float VerticalVelocity(float pitch, float faceToInwardCosine) {
    // Independent vertical-ladder projection from Source LadderMove: the
    // vertical look component and inward horizontal component both contribute.
    const float angle = pitch * 0.01745329251994329577f;
    return 200.0f * (-std::sin(angle) + std::cos(angle) * faceToInwardCosine);
}
int main() {
    using namespace AgPBLadder;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Wish up = TraverseWish(true, 0), down = TraverseWish(false, 0);
    Require(up.forward > 0 && down.forward > 0 && up.side == 0 && down.side == 0,
        "both traversal directions submit a forward wish toward the ladder face");
    Require(VerticalVelocity(up.pitch, 1) > 0 && VerticalVelocity(down.pitch, 1) < 0,
        "actual Source ladder projection climbs up and down with the selected pitches");
    Require(VerticalVelocity(0, 1) > 0,
        "the previous zero-pitch inward movement cannot descend a vertical ladder");
    const float mountPitch = MountPitch(true) * 0.01745329251994329577f;
    const float mountX = std::cos(mountPitch), mountZ = -std::sin(mountPitch);
    Require(-0.2f * mountX + 0.98f * mountZ > 0 && -0.2f < 0,
        "upward mount sweeps away from a rising floor that blocks a horizontal hull sweep");
    Require(mountX > 0.5f && mountZ > 0 && MountPitch(false) == 0,
        "upward attachment still reaches the face while downward attachment stays horizontal");
    const float exitDirections[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (float cosine : exitDirections)
        Require(VerticalVelocity(DismountPitch(true, true), cosine) > 0,
            "top dismount keeps upward movement for forward, sideways and outward exits");
    Require(DismountPitch(false, true) == 0 && DismountPitch(true, false) == 0,
        "bottom detach and airborne exit use horizontal ground/air wishes");
    Require(TraverseWish(true, 3).side > 0 && TraverseWish(false, -3).side < 0 &&
        TraverseWish(true, 2).side == 0 && TraverseWish(true, nan).side == 0,
        "lateral alignment has a finite dead band and correct Source right-axis sign");
    Require(EntryReached(0, 0, 0) && EntryReached(9, 0, 12) && !EntryReached(0, 0, 16) &&
        !EntryReached(0, 0, 32) && !EntryReached(nan, 0, 0),
        "an aligned XY position cannot consume a ladder point on another level");
    Require(EntryReached(13, 0, 0) && !CanAdvanceUnattachedEntry(false, 13, 0, 0) &&
        !CanAdvanceUnattachedEntry(false, 0, 0, 0),
        "descending approach retains its top target until a real ladder attachment");
    Require(CanAdvanceUnattachedEntry(true, 13, 0, 0) &&
        !CanAdvanceUnattachedEntry(true, 16, 0, 0),
        "ascending ground entry keeps its existing approach tolerance");
    Require(!HeightReached(true, -190, -176) && HeightReached(true, -177, -176) &&
        !HeightReached(false, -260, -278) && HeightReached(false, -277, -278) &&
        !HeightReached(true, nan, -176),
        "up/down endpoint transitions require the authored feet height");

    State state;
    Begin(state, 2, 3, 4, 17, -278, -176, -90, 10);
    Require(state.Active() && state.stage == Stage::Approach && state.ascending &&
        state.owner == 17 && state.first == 2 && state.exit == 4 && state.faceYaw == -90,
        "route initialization retains directed endpoints and command ownership");
    Progress(state, 100, 10);
    Progress(state, 99.5f, 12);
    Require(!TimedOut(state, 13.9f) && TimedOut(state, 14.1f),
        "sub-unit jitter cannot indefinitely postpone a failed mount");
    Progress(state, 98, 13);
    Require(!TimedOut(state, 16.9f), "real progress refreshes the bounded stall deadline");
    SetStage(state, Stage::Traverse, 17);
    Require(state.bestDistance < 0 && state.progressAt == 17,
        "attachment changes progress targets without inheriting the approach budget");
    state.attached = true;
    Progress(state, 20, 17);
    SetStage(state, Stage::Dismount, 18);
    Require(state.attached && state.stage == Stage::Dismount && state.owner == 17,
        "top and bottom departure retain the same route owner");
    for (int i = 0; i < 20; ++i) Progress(state, 100.0f - i * 2, 18.0f + i * 0.1f);
    Require(TimedOut(state, 25), "continuous motion cannot exceed the total traversal bound");
    Require(TimedOut(state, 9), "a game-clock reset cannot keep an old traversal alive");
    Reset(state);
    Require(!state.Active() && !state.attached && state.owner == 0 && state.first == -1,
        "stop, owner cancellation and death reset remove every ladder command state");
    Begin(state, 6, 7, 8, 31, -176, -278, 0, 30);
    Require(!state.ascending && state.owner == 31 && state.stage == Stage::Approach,
        "the reverse manually connected traversal initializes a fresh descent");
    Begin(state, 6, 7, 8, 31, -10000, 0, 0, 30);
    Require(state.duration == 45, "authored ladder length cannot remove the absolute deadline");
    std::puts("PASS authored ladder directions, endpoint heights, departure and lifecycle bounds");
}
