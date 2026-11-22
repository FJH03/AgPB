// Source-independent SyPB teammate avoidance and recovery regression.
#include "../src/player_avoidance.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace AgPBPlayerAvoid;
static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static bool Same(Vec2 a, Vec2 b) { return Near(a.x, b.x) && Near(a.y, b.y); }
static Input Moving() {
    Input in; in.now = 10; in.interval = 0.015f; in.maxSpeed = 250;
    in.wish = Vec2(250, 0); in.bottom = 0; in.top = 72;
    in.team = 3; in.slot = 2; in.owner = 11; in.recentEnemy = true;
    return in;
}
static Neighbor Player() {
    Neighbor n; n.handle = 0x10001; n.userid = 2; n.slot = 1;
    n.team = 3; n.position = Vec2(45, 12); n.bottom = 0; n.top = 72;
    return n;
}
static Probes Open() {
    Probes p; p.right = p.left = p.back = p.rightBack = p.leftBack = p.rightOnly = p.leftOnly = true; return p;
}
static void Contacts() {
    Input in = Moving(); Neighbor n = Player();
    Contact c = FindContact(in, &n, 1);
    Require(c.valid && c.handle == n.handle && c.userid == n.userid && c.slot == 1 &&
        c.preferredSide == 1 && c.retreat, "front teammate selects the away side and backward component");
    n = Player(); n.team = 2;
    Require(!FindContact(in, &n, 1).valid, "enemy is not a teammate avoidance target");
    n = Player(); n.bottom = 72; n.top = 144;
    Require(!FindContact(in, &n, 1).valid, "touching vertical boundaries on another floor do not overlap");
    n.bottom = -72; n.top = 0;
    Require(!FindContact(in, &n, 1).valid, "a player directly below is not a lateral obstruction");
    n = Player(); n.position = Vec2(-45, 0);
    c = FindContact(in, &n, 1);
    Require(c.valid && !c.retreat, "contact behind the movement direction does not request reverse");
    Neighbor pair[] = {Player(), Player()};
    pair[0].handle = 300; pair[0].slot = 3; pair[0].userid = 30; pair[0].position = Vec2(50, 0);
    pair[1].handle = 200; pair[1].slot = 2; pair[1].userid = 20; pair[1].position = Vec2(35, 0);
    Require(FindContact(in, pair, 2).handle == 200, "closest same-level teammate wins");
    const Neighbor swap = pair[0]; pair[0] = pair[1]; pair[1] = swap;
    Require(FindContact(in, pair, 2).handle == 200, "nearest selection is independent of enumeration order");
    pair[0].position = pair[1].position;
    Require(FindContact(in, pair, 2).slot == 2, "equal-distance selection favours the lower slot");
    const Neighbor swap2 = pair[0]; pair[0] = pair[1]; pair[1] = swap2;
    Require(FindContact(in, pair, 2).slot == 2, "equal-distance slot tie break is stable");
    pair[0].position = Vec2(40, 0);
    pair[0].z = pair[0].bottom = 40; pair[0].top = 112;
    Require(FindContact(in, pair, 2).slot == 2, "nearest selection uses 3D origin distance for overlapping hull heights");
    pair[0].position = Vec2(-52, 0); pair[0].velocity = Vec2(-300, 0);
    pair[0].z = pair[0].bottom = 0; pair[0].top = 72;
    pair[1].position = Vec2(54, 0);
    Require(FindContact(in, &pair[1], 1).valid && !FindContact(in, pair, 2).valid,
        "the nearest teammate is selected before applying the contact thresholds");

    Neighbor crowd[] = {Player(), Player(), Player(), Player()};
    for (int i = 0; i < 4; ++i) {
        crowd[i].slot = i + 1; crowd[i].handle = i + 1;
        crowd[i].position = Vec2(float(100 - i * 20), 0);
    }
    Require(FindContact(in, crowd, 4).slot == 4, "four successive closest records do not suppress avoidance");
    Require(!FindContact(in, NULL, 3).valid && !FindContact(in, pair, 0).valid, "empty inventory of neighbors is safe");
}
static void Prediction() {
    Input in = Moving(); Neighbor n = Player(); n.position = Vec2(130, 0);
    in.velocity = Vec2(250, 0); n.velocity = Vec2(-250, 0);
    Require(!FindContact(in, &n, 1).valid, "one native movement frame leaves distant closing traffic alone");
    in.wish = in.velocity = Vec2(200, 0); n.position = Vec2(54, 0); n.velocity = Vec2(100, 0);
    Require(FindContact(in, &n, 1).valid, "wish and velocity together predict contact at exactly 48 units");
    n.position.x = 54.1f;
    Require(!FindContact(in, &n, 1).valid, "separating teammate above the predicted 48-unit threshold is clear");
    in = Moving(); n = Player(); n.position = Vec2(56, 0); n.velocity = Vec2(-100, 0);
    Contact c = FindContact(in, &n, 1);
    Require(c.valid && !c.retreat, "approaching teammate at exactly 56 units selects side without reversing");
    n.position.x = 55.9f;
    Require(FindContact(in, &n, 1).retreat, "approaching teammate ahead inside 56 units adds reverse movement");
    n.velocity.x = 100;
    Require(!FindContact(in, &n, 1).valid, "separating teammate outside the one-frame 48-unit radius is clear");
    in.maxSpeed = 48; n.position = Vec2(48, 0); n.velocity = Vec2();
    Require(!FindContact(in, &n, 1).valid, "closest teammate must be strictly within max speed range");
    n.position.x = 47.9f;
    Require(FindContact(in, &n, 1).valid, "teammate inside max speed range participates in close checks");
    in = Moving(); n = Player(); in.wish = Vec2(0, 250); n.position = Vec2(0, 45);
    Require(FindContact(in, &n, 1).retreat, "the actual world wish determines forward contact");
    in.wish = Vec2();
    Require(!FindContact(in, &n, 1).valid, "holding position cannot invent a travel direction");
}
static void Steering() {
    Input in = Moving(); Neighbor n = Player(); State state; Probes probes = Open();
    Contact c = FindContact(in, &n, 1);
    Output out = Step(state, in, c, probes);
    Require(out.active && out.yielding && out.wish.y < 0 && out.wish.x < 0 &&
        Same(out.wish, CandidateWish(in, 1, true)), "front blocker selects the exact checked away-and-back vector");
    n.position.y = -12; in.now += 0.2f; c = FindContact(in, &n, 1);
    Require(c.preferredSide == -1 && Step(state, in, c, probes).wish.y > 0,
        "a crossing teammate changes the away side on the next frame");
    n.position = Vec2(0, 45); c = FindContact(in, &n, 1);
    out = Step(state, in, c, probes);
    Require(Same(out.wish, CandidateWish(in, 1)) && out.wish.x > 0,
        "contact to the side keeps the forward component");
    probes.right = false;
    out = Step(state, in, c, probes);
    Require(out.wish.y > 0 && Same(out.wish, CandidateWish(in, -1)), "one-sided wall selects the other fully checked side");
    probes.right = true;
    Require(Step(state, in, c, probes).wish.y < 0, "opening the preferred side restores the away path");
    probes = Probes(); probes.rightBack = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, 1, true)), "blocked sides may use checked diagonal retreat");
    n.position = Vec2(45, 12); c = FindContact(in, &n, 1);
    probes.rightBack = false; probes.leftBack = true;
    probes.right = probes.left = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, -1, true)),
        "front blocker uses the opposite checked retreat if its away diagonal is blocked");
    probes.leftBack = false; probes.back = true;
    Require(Same(Step(state, in, c, probes).wish, Vec2(-250, 0)), "checked straight retreat follows both blocked diagonals");
    probes.back = false; out = Step(state, in, c, probes);
    Require(out.active && out.yielding && Same(out.wish, Vec2()), "front blocker causes a wait when all retreats are blocked");
    for (int speed = 1; speed <= 300; ++speed) {
        in.wish = Vec2(float(speed), float(speed) * 0.5f); in.maxSpeed = 150;
        for (int side = -1; side <= 1; ++side) {
            for (int back = 0; back <= 1; ++back) {
                const Vec2 candidate = CandidateWish(in, side, back != 0);
                Require(Length(candidate) <= 150.001f && Length(candidate) <= Length(in.wish) + 0.001f,
                    "all candidate directions preserve the lesser of original wish speed and player maximum");
            }
        }
    }
}
static void ReplanningAndIdentity() {
    Input in = Moving(); in.now = 0; Neighbor n = Player(); State state; Probes blocked;
    Contact c = FindContact(in, &n, 1);
    Require(!Step(state, in, c, blocked).requestReplan && Near(state.blockedSince, 0), "first all-blocked contact begins a real time budget");
    in.now = 4.99f; Require(!Step(state, in, c, blocked).requestReplan, "waiting does not replan before five seconds");
    in.now = 5; Require(Step(state, in, c, blocked).requestReplan, "all-blocked wait requests a replan at five seconds");
    in.now = 5.01f; Require(!Step(state, in, c, blocked).requestReplan, "native ticks cannot repeatedly request the same replan");
    in.now = 9.99f; Require(!Step(state, in, c, blocked).requestReplan, "second replan has a separate five-second budget");
    in.now = 10; Require(Step(state, in, c, blocked).requestReplan, "continued crowd blocking periodically requests recovery instead of permanent immunity");
    in.now = 10.1f; ++n.handle; ++n.userid; c = FindContact(in, &n, 1);
    Step(state, in, c, blocked);
    Require(state.handle == n.handle && state.userid == n.userid && Near(state.blockedSince, 0),
        "new player identity rebinds while retaining continuous crowd-blocking time");
    in.now += 0.1f; ++in.owner; Step(state, in, c, blocked);
    Require(state.owner == in.owner && Near(state.blockedSince, in.now), "replacement task owner gets a fresh avoidance lifecycle");
    in.now = 1; Step(state, in, c, blocked);
    Require(Near(state.lastTime, 1) && Near(state.blockedSince, 1) && Near(state.nextReplanAt, 6),
        "time rollback cannot retain future avoidance leases or recovery deadlines");
    Step(state, in, Contact(), blocked);
    Require(state.owner == 0 && state.handle == 0 && state.blockedSince == -1 && state.side == 0,
        "contact loss resets side and identity state");
    in.owner = 0; Require(!Step(state, in, c, blocked).active && state.owner == 0, "unowned movement cannot retain a player lease");

    in = Moving(); in.now = 0; n = Player(); Reset(state);
    Step(state, in, FindContact(in, &n, 1), blocked);
    for (int second = 1; second <= 5; ++second) {
        in.now = float(second); ++n.handle; ++n.userid;
        const Output crowd = Step(state, in, FindContact(in, &n, 1), blocked);
        Require(crowd.active && crowd.requestReplan == (second == 5) && Near(state.blockedSince, 0),
            "rotating crowd identities cannot renew an all-blocked owner's recovery budget indefinitely");
    }
}
static void PhysicalContacts() {
    Input in = Moving(); Neighbor n = Player();
    in.recentEnemy = false;
    in.hullMins = n.hullMins = Vec2(-16, -16);
    in.hullMaxs = n.hullMaxs = Vec2(16, 16);
    n.position = Vec2(45, 0);
    Require(!FindContact(in, &n, 1).valid, "calm traffic outside the short hull sweep uses its original movement");
    in.recentEnemy = true;
    Contact c = FindContact(in, &n, 1);
    Require(c.valid && !c.physical && c.retreat, "recent enemy sight enables the original 48/56-unit behavior");
    in.recentEnemy = false;
    n.position = Vec2(40, 31);
    c = FindContact(in, &n, 1);
    Require(c.valid && c.physical && c.yielding && c.preferredSide == 1,
        "a short wish across an axis-aligned hull corner triggers calm recovery");
    n.position = Vec2(31, 31);
    Require(FindContact(in, &n, 1).physical, "overlapping hull corners trigger beyond the center-circle radius");
    in.wish = Vec2(-250, 0);
    Require(!FindContact(in, &n, 1).valid, "movement separating from an existing hull overlap remains available");
    in.wish = Vec2(250, 0);
    n.position = Vec2(32, 32);
    Require(!FindContact(in, &n, 1).valid, "movement tangent to a hull does not become a physical jam");
    n.position.y = 0; n.bottom = 72; n.top = 144;
    Require(!FindContact(in, &n, 1).valid, "a hull directly above cannot cause a ground jam");
    n.bottom = 0; n.top = 72;
    Neighbor crowd[] = {n, n};
    crowd[0].slot = 4; crowd[0].position = Vec2(-31, 0);
    crowd[1].slot = 5; crowd[1].position = Vec2(35, 0);
    Require(FindContact(in, crowd, 2).slot == 5, "a closer teammate behind does not mask the blocking hull ahead");
}
static void PhysicalYieldAndRecovery() {
    Input in = Moving(); Neighbor n = Player(); State state;
    in.recentEnemy = false;
    in.hullMins = n.hullMins = Vec2(-16, -16);
    in.hullMaxs = n.hullMaxs = Vec2(16, 16);
    n.position = Vec2(32, 0);
    n.wish = Vec2(-250, 0); n.canYield = true;
    Contact c = FindContact(in, &n, 1, &state);
    Require(c.physical && c.yielding, "higher-slot bot yields in an opposing ground jam");
    Probes probes = Open();
    Output out = Step(state, in, c, probes);
    Require(out.active && out.yielding && Same(out.wish, SideWish(in, -1)),
        "physical recovery prefers a checked lateral move over either form of retreat");
    n.position = Vec2(48, 1); in.now += in.interval;
    c = FindContact(in, &n, 1, &state);
    Require(!FindContact(in, &n, 1).valid && c.physical && c.preferredSide == 1 &&
        Same(Step(state, in, c, probes).wish, SideWish(in, -1)),
        "ongoing recovery keeps its checked side across small retreats and side preference changes");
    ++n.userid;
    Require(!FindContact(in, &n, 1, &state).valid, "reusing a slot cannot inherit the wider sweep of the old player");
    n.position = Vec2(32, 1);
    c = FindContact(in, &n, 1, &state);
    probes = Probes(); probes.leftOnly = probes.back = true;
    Require(Same(Step(state, in, c, probes).wish, SideWish(in, -1)),
        "a blocked preferred side can use the other checked lateral move");
    probes = Probes(); probes.rightBack = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, 1, true)),
        "blocked lateral steps fall back to checked diagonal retreat");
    probes = Probes(); probes.back = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, 0, true)),
        "blocked diagonal steps fall back to checked straight retreat");
    probes = Probes();
    out = Step(state, in, c, probes);
    Require(out.active && out.yielding && Same(out.wish, Vec2()) && !out.requestReplan,
        "a blocked physical jam waits for a legal move");
    in.now = 15;
    c = FindContact(in, &n, 1, &state);
    Require(Step(state, in, c, probes).requestReplan, "a physical jam requests recovery after five seconds");

    in.slot = 1; n.slot = 2; Reset(state);
    c = FindContact(in, &n, 1, &state);
    out = Step(state, in, c, Open());
    Require(c.physical && out.active && !out.yielding && Same(out.wish, Vec2()),
        "the lower-slot bot waits without pushing into the yielding teammate");
    n.wish = Vec2(); n.canYield = false;
    c = FindContact(in, &n, 1, &state);
    Require(c.physical && Step(state, in, c, Open()).yielding, "a human or holding bot receives right of way");
    n.position.y = 34;
    c = FindContact(in, &n, 1, &state);
    out = Step(state, in, c, Open());
    Require(!out.active && Same(out.wish, in.wish), "the original route wish resumes after lateral clearance");
}
static void GroundLookahead() {
    Input in = Moving();
    Neighbor n = Player();
    in.recentEnemy = false;
    in.hullMins = n.hullMins = Vec2(-16, -16);
    in.hullMaxs = n.hullMaxs = Vec2(16, 16);
    n.position = Vec2(64, 0);
    Require(!FindContact(in, &n, 1).valid, "an ordinary route is clear of a distant stationary teammate");
    in.blockedLookahead = 45.0f;
    Require(FindContact(in, &n, 1).physical, "a stopped ground wish recovers a teammate inside its original probe");
    in.blockedLookahead = 0.0f;
    n.position = Vec2(80, 0);
    n.velocity = Vec2(-150, 0);
    const Contact c = FindContact(in, &n, 1);
    Require(c.valid && c.anticipated && !c.physical, "moving head-on traffic can step aside before contact");
    State state;
    Require(Step(state, in, c, Open()).wish.x > 0, "an open diagonal keeps forward progress");
    Require(!Step(state, in, c, Probes()).active, "prediction alone cannot stall a narrow passage");
    n.velocity.x = 150.0f;
    Require(!FindContact(in, &n, 1).valid, "a departing teammate does not cause an early sidestep");
}
static void PhysicalSharedAnchor() {
    Input in = Moving(); Neighbor n = Player(); State state;
    in.recentEnemy = false;
    in.hullMins = n.hullMins = Vec2(-16, -16);
    in.hullMaxs = n.hullMaxs = Vec2(16, 16);
    n.position = Vec2(32, 0);
    n.wish = Vec2(-250, 0); n.canYield = true;
    Contact c = FindContact(in, &n, 1, &state);
    Step(state, in, c, Open());
    // The shared anchor is twelve units ahead of the yielding bot's starting
    // position. A sidestep makes the route wish turn back toward that anchor.
    in.now += in.interval; in.position = Vec2(0, 10); in.wish = Vec2(120, -100);
    c = FindContact(in, &n, 1, &state);
    Require(c.physical && Same(c.approach, Vec2(1, 0)), "a changing anchor wish keeps the original escape direction");
    const Input steering = SteeringInput(in, c);
    Output out = Step(state, in, c, Open());
    Require(Same(out.wish, SideWish(steering, -1)) && Near(out.wish.x, 0) &&
        Near(out.wish.y, Length(in.wish)), "recovery moves laterally past the ordered bot at the current authorized speed");
    Probes backOnly;
    backOnly.back = true;
    out = Step(state, in, c, backOnly);
    Require(Same(out.wish, CandidateWish(steering, 0, true)) && Near(out.wish.y, 0) && out.wish.x < 0,
        "a narrow lane uses a checked retreat on the same fixed approach axis");
}
static void InvalidData() {
    Input in = Moving(); Neighbor n = Player(); State state; Probes probes = Open();
    Contact c = FindContact(in, &n, 1); Step(state, in, c, probes);
    in.now = std::numeric_limits<float>::quiet_NaN();
    Require(!FindContact(in, &n, 1).valid && !Step(state, in, c, probes).active && state.owner == 0,
        "invalid time releases state rather than emitting stale steering");
    in = Moving(); in.wish.x = std::numeric_limits<float>::infinity();
    Require(Same(Step(state, in, c, probes).wish, Vec2()), "invalid wish never reaches a movement command");
    in = Moving(); n.velocity.x = std::numeric_limits<float>::quiet_NaN();
    Require(!FindContact(in, &n, 1).valid, "unknown neighbor motion is not treated as a reliable obstruction prediction");
    n = Player(); n.handle = 0; Require(!FindContact(in, &n, 1).valid, "missing full handle cannot bind a neighbor");
    n = Player(); n.userid = 0; Require(!FindContact(in, &n, 1).valid, "missing userid cannot bind a neighbor");
    n = Player(); n.slot = 0; Require(!FindContact(in, &n, 1).valid, "non-player slot cannot bind a neighbor");
    in.interval = 0; Require(!FindContact(in, &n, 1).valid, "paused simulation cannot start prediction");
}
int main() {
    Contacts(); Prediction(); Steering(); ReplanningAndIdentity();
    PhysicalContacts(); PhysicalYieldAndRecovery(); GroundLookahead(); PhysicalSharedAnchor(); InvalidData();
    std::puts("player avoidance policy regression passed");
    return 0;
}
