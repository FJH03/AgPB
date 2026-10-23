// Production-policy regressions for partial cover and nonpenetrating shots.
#include "../src/combat_visibility.h"
#include "../src/combat_skill.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    using namespace AgPBCombatVisibility;
    for (int skill = 1; skill <= 100; ++skill) {
        const bool preferHead = AgPBCombatSkill::AimAtHead(skill, 50, false, 100);
        Require(SelectSpot(Bit(Body), preferHead) == Body, "body-only never selects covered head at any skill");
        Require(SelectSpot(Bit(Head), preferHead) == Head, "head-only never falls back to covered body at low skill");
        Require(SelectSpot(Bit(Body) | Bit(Head), preferHead) == (preferHead ? Head : Body), "both-visible preserves SyPB skill preference");
        Require(SelectSpot(0, preferHead) == -1, "fully covered target provides no aim point");
    }
    Require(SelectPreferredSpot(Bit(Body), true, true) == Head, "physically proven thin-cover head route retains SyPB preference");
    Require(SelectPreferredSpot(Bit(Body), true, false) == Body, "thick or unknown head cover retains exposed body");
    Require(SelectPreferredSpot(0, true, true) == -1, "penetration does not acquire fully hidden enemies");
    Require(SelectSpot(Bit(RightShoulder), true) == RightShoulder, "exposed shoulder is retained when center/head are covered");
    Require(SelectSpot(Bit(LeftLeg), false) == LeftLeg, "exposed leg is a valid proven body sample");
    Require(Summary(Bit(LeftTorso)) == BodyVisible && Summary(Bit(Head)) == HeadVisible &&
        Summary(Bit(Head) | Bit(LeftTorso)) == (BodyVisible | HeadVisible), "partial samples retain separate body/head flags");
    Require(!PlayerClipIsCloser(0.95f, 1040, 0.97f, 1000), "extended player clip cannot bypass nearer wall by fraction mismatch");
    Require(PlayerClipIsCloser(0.90f, 1040, 0.97f, 1000), "physically nearer player hit wins extended clip comparison");
    Require(ShotAllowed(true, false, RayHit::Target, 450, 520), "actual first hit on target is permitted");
    Require(ShotAllowed(true, false, RayHit::Clear, 8192, 520), "clear low-skill miss remains permitted");
    Require(!ShotAllowed(true, false, RayHit::World, 250, 520), "wall before target blocks a noisy shot");
    Require(ShotAllowed(true, false, RayHit::World, 250, 520, true), "verified penetration can authorize cover before target");
    Require(!ShotAllowed(true, false, RayHit::World, 510, 520), "cover overlapping target bounds blocks the shot");
    Require(!ShotAllowed(true, false, RayHit::World, 521, 520), "ambiguous edge depth is conservatively blocked");
    Require(ShotAllowed(true, false, RayHit::World, 800, 520), "world behind target preserves legitimate misses");
    Require(!ShotAllowed(true, false, RayHit::OtherEntity, 250, 520) &&
        !ShotAllowed(true, false, RayHit::OtherEntity, 800, 520), "teammate/prop on actual miss ray blocks before or behind target");
    Require(!ShotAllowed(false, false, RayHit::Clear, 8192, 520) &&
        !ShotAllowed(false, false, RayHit::Target, 450, 520), "noisy/recoil ray cannot authorize a covered ideal aim point");
    Require(!ShotAllowed(true, true, RayHit::Target, 450, 520), "solid starts block fire");
    Require(!ShotAllowed(true, false, RayHit::World, std::numeric_limits<float>::infinity(), 520), "invalid impact distance fails closed");
    std::puts("combat visibility tests passed");
}
