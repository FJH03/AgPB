// Source-independent player threat prediction, priority and recovery regression.
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
    in.team = 3; in.priority = 5; in.owner = 11;
    return in;
}
static Neighbor Player() {
    Neighbor n; n.handle = 0x10001; n.userid = 2; n.slot = 1;
    n.team = 3; n.priority = 1; n.position = Vec2(45, 12); n.bottom = 0; n.top = 72;
    return n;
}
static Probes Open() {
    Probes p; p.right = p.left = p.back = p.rightBack = p.leftBack = true; return p;
}
static void Contacts() {
    Input in = Moving(); Neighbor n = Player();
    Contact c = FindContact(in, &n, 1);
    Require(c.valid && c.yielding && c.handle == n.handle && c.userid == n.userid && c.slot == 1 &&
        c.priority == 1 && c.preferredSide == 1, "human priority and relative away side replace slot-parity steering");
    n.priority = 7;
    c = FindContact(in, &n, 1);
    Require(c.valid && !c.yielding, "higher-priority AgPB still detects a lower-priority obstruction");
    in.priority = 8;
    Require(FindContact(in, &n, 1).yielding, "smaller numerical AgPB priority wins");
    n.priority = in.priority;
    Require(!FindContact(in, &n, 1).yielding, "equal priorities do not invent a mutual yield rule");
    n = Player(); n.team = 2;
    Require(!FindContact(in, &n, 1).valid, "enemy is not a teammate avoidance target");
    n = Player(); n.bottom = 72; n.top = 144;
    Require(!FindContact(in, &n, 1).valid, "touching vertical boundaries on another floor do not overlap");
    n.bottom = -72; n.top = 0;
    Require(!FindContact(in, &n, 1).valid, "a player directly below is not a lateral obstruction");
    n = Player(); n.position = Vec2(-30, 0); n.velocity = Vec2(-100, 0);
    Require(!FindContact(in, &n, 1).valid, "rear player moving away cannot turn the bot back into them");
    n.position = Vec2(-60, 0); n.velocity = Vec2(500, 0);
    Require(FindContact(in, &n, 1).valid, "rear faster catch-up is retained when predicted motion converges");
    n = Player(); n.intent = Vec2(-250, 0);
    Require(FindContact(in, &n, 1).headOn, "opposing movement intents identify head-on traffic");
    n.intent = Vec2(); n.velocity = Vec2(-100, 0);
    Require(FindContact(in, &n, 1).headOn, "velocity supplies head-on direction when intent is unavailable");
    Neighbor pair[] = {Player(), Player()};
    pair[0].handle = 300; pair[0].slot = 3; pair[0].userid = 30; pair[0].position = Vec2(50, 0);
    pair[1].handle = 200; pair[1].slot = 2; pair[1].userid = 20; pair[1].position = Vec2(35, 0);
    pair[1].priority = 9;
    c = FindContact(in, pair, 2);
    Require(c.handle == 200 && !c.yielding, "nearest threat wins even if a farther human has higher priority");
    const Neighbor swap = pair[0]; pair[0] = pair[1]; pair[1] = swap;
    Require(FindContact(in, pair, 2).handle == 200, "nearest selection is independent of enumeration order");
    pair[0].position = pair[1].position; pair[0].priority = pair[1].priority;
    const std::uint64_t tied = FindContact(in, pair, 2).handle;
    const Neighbor swap2 = pair[0]; pair[0] = pair[1]; pair[1] = swap2;
    Require(FindContact(in, pair, 2).handle == tied, "equal-distance selection has stable identity tie breaks");
    Require(!FindContact(in, NULL, 3).valid && !FindContact(in, pair, 0).valid, "empty inventory of neighbors is safe");
}
static void Prediction() {
    Input in = Moving(); Neighbor n = Player(); n.position = Vec2(130, 0);
    Require(!FindContact(in, &n, 1).valid, "distant stationary neighbor is not an immediate near threat");
    in.velocity = Vec2(250, 0); n.velocity = Vec2(-250, 0);
    Require(FindContact(in, &n, 1).valid, "both velocities predict a closing threat outside the static near radius");
    n.velocity = Vec2();
    Require(FindContact(in, &n, 1).valid && Near(PredictionSeconds(in), 0.15f),
        "running horizon scales ten native intervals and includes existing self motion");
    in.ducking = true;
    Require(!FindContact(in, &n, 1).valid && Near(PredictionSeconds(in), 0.075f),
        "ducking uses the shorter five-interval prediction");
    in.ducking = false; in.interval = 1;
    Require(Near(PredictionSeconds(in), 0.2f), "long native frames cannot predict beyond two tenths");
    n.position = Vec2(250, 0);
    Require(!FindContact(in, &n, 1).valid, "prediction cap prevents distant speculative avoidance");
    in = Moving(); n = Player(); in.wish = Vec2(0, 250); n.position = Vec2(0, 45);
    Require(FindContact(in, &n, 1).valid, "actual world wish rather than a view yaw determines forward contact");
    in.wish = Vec2();
    Require(!FindContact(in, &n, 1).valid, "holding position cannot invent a travel direction");
}
static void Steering() {
    Input in = Moving(); Neighbor n = Player(); State state; Probes probes = Open();
    Contact c = FindContact(in, &n, 1);
    Output out = Step(state, in, c, probes);
    Require(out.active && out.yielding && out.wish.y < 0 && out.wish.x > 0 &&
        Same(out.wish, CandidateWish(in, 1)), "yielding follows the exact pre-probed right vector");
    n.position.y = -12; in.now += 0.2f; c = FindContact(in, &n, 1);
    Require(c.preferredSide == -1 && Step(state, in, c, probes).wish.y < 0,
        "relative-side noise cannot flip an unexpired half-second side lease");
    in.now = 10.51f;
    Require(Step(state, in, c, probes).wish.y > 0, "expired side lease may follow the new away side");
    Reset(state); in.now = 11; n.position.y = 12; c = FindContact(in, &n, 1); probes.right = false;
    out = Step(state, in, c, probes);
    Require(out.wish.y > 0 && Same(out.wish, CandidateWish(in, -1)), "one-sided wall selects the other fully checked side");
    probes.right = true; in.now += 0.1f;
    Require(Step(state, in, c, probes).wish.y > 0, "opening the preferred side does not immediately undo the fallback lease");
    Reset(state); probes = Probes(); probes.rightBack = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, 1, true)), "blocked sides may use checked diagonal retreat");
    probes.rightBack = false; probes.leftBack = true;
    Require(Same(Step(state, in, c, probes).wish, CandidateWish(in, -1, true)), "blocked diagonal may fall back to the opposite retreat");
    probes.leftBack = false; probes.back = true;
    Require(Same(Step(state, in, c, probes).wish, Vec2(-250, 0)), "checked straight retreat follows both blocked sides");
    probes.back = false; out = Step(state, in, c, probes);
    Require(out.active && out.yielding && Same(out.wish, Vec2()), "all blocked candidates wait without forcing an unsafe side");
    n.priority = 9; c = FindContact(in, &n, 1);
    out = Step(state, in, c, probes);
    Require(out.active && !out.yielding && Same(out.wish, in.wish), "right-of-way bot keeps its original wish for caller safety filtering");
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
    in.now = 15; n.priority = 9; c = FindContact(in, &n, 1);
    const Output priority = Step(state, in, c, blocked);
    Require(!priority.yielding && priority.requestReplan, "higher-priority obstruction still participates in the bounded recovery budget");
    in.now = 15.1f; ++n.handle; ++n.userid; c = FindContact(in, &n, 1);
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

    in = Moving(); n = Player(); Reset(state); Probes open = Open();
    Step(state, in, FindContact(in, &n, 1), open);
    in.now += 0.1f; ++n.handle; n.position.y = -12;
    Require(Step(state, in, FindContact(in, &n, 1), open).wish.y > 0,
        "reused slot with a changed full handle cannot inherit the previous player's side lease");
    in.now += 0.1f; ++n.userid; n.position.y = 12;
    Require(Step(state, in, FindContact(in, &n, 1), open).wish.y < 0,
        "userid change independently invalidates the previous player's side lease");
    in.now += 0.1f; ++n.slot; n.position.y = -12;
    Require(Step(state, in, FindContact(in, &n, 1), open).wish.y > 0,
        "slot change independently invalidates the previous player's side lease");
    in = Moving(); in.now = 0; n = Player(); Reset(state);
    Step(state, in, FindContact(in, &n, 1), blocked);
    for (int second = 1; second <= 5; ++second) {
        in.now = float(second); ++n.handle; ++n.userid;
        const Output crowd = Step(state, in, FindContact(in, &n, 1), blocked);
        Require(crowd.active && crowd.requestReplan == (second == 5) && Near(state.blockedSince, 0),
            "rotating crowd identities cannot renew an all-blocked owner's recovery budget indefinitely");
    }
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
    Contacts(); Prediction(); Steering(); ReplanningAndIdentity(); InvalidData();
    std::puts("player avoidance policy regression passed");
    return 0;
}
