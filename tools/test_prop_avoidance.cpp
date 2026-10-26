// SPDX-License-Identifier: GPL-3.0-only
// Standalone, SDK-free regression for the ground-prop recovery policy.
// The geometry oracle is an independent swept point against Minkowski-expanded
// rectangles. Motion uses Source's public ground Friction/Accelerate equations
// (game/shared/gamemovement.cpp), followed by collision sliding. It does not
// replace the live engine hull/step/floor acceptance checks.
#include "../src/prop_avoidance.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
namespace Policy = AgPBPropAvoid;
constexpr float kDt = 1.0f / 64.0f;
constexpr float kRadius = 16.0f;
constexpr float kSpeed = 170.0f;
constexpr std::uint64_t kOwner = 0x100000055ULL;
constexpr int kGoal = 37;
struct P {
    float x = 0, y = 0;
    P(float px = 0, float py = 0) : x(px), y(py) {}
    P operator+(P b) const { return P(x + b.x, y + b.y); }
    P operator-(P b) const { return P(x - b.x, y - b.y); }
    P operator*(float n) const { return P(x * n, y * n); }
};
float Dot(P a, P b) { return a.x * b.x + a.y * b.y; }
float Size(P a) { return std::sqrt(Dot(a, a)); }
P Unit(P a) { const float n = Size(a); return n > 0.00001f ? a * (1 / n) : P(); }
P Native(Policy::Vec2 a) { return P(a.x, a.y); }
Policy::Vec2 PolicyVector(P a) { return Policy::Vec2(a.x, a.y); }
void Require(bool okay, const char *reason) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
struct Rect {
    float minX, minY, maxX, maxY;
    bool prop = false, pushable = false;
    std::uint64_t id = 0;
};
struct Hit {
    bool blocked = false;
    float fraction = 1;
    P normal;
    int index = -1;
};
bool Inside(P at, const Rect &box, float radius) {
    return at.x > box.minX - radius + 0.0001f && at.x < box.maxX + radius - 0.0001f &&
        at.y > box.minY - radius + 0.0001f && at.y < box.maxY + radius - 0.0001f;
}
Hit SweepRect(P from, P to, const Rect &box, float radius) {
    Hit hit;
    const P delta = to - from;
    if (Inside(from, box, radius)) {
        hit.blocked = true; hit.fraction = 0; hit.normal = Unit(delta) * -1;
        return hit;
    }
    float enter = -std::numeric_limits<float>::infinity();
    float leave = std::numeric_limits<float>::infinity();
    P normal;
    for (int axis = 0; axis != 2; ++axis) {
        const float start = axis == 0 ? from.x : from.y;
        const float move = axis == 0 ? delta.x : delta.y;
        const float lo = (axis == 0 ? box.minX : box.minY) - radius;
        const float hi = (axis == 0 ? box.maxX : box.maxY) + radius;
        if (std::fabs(move) < 0.000001f) {
            // Sliding exactly along a face does not enter its interior.
            if (start <= lo + 0.00001f || start >= hi - 0.00001f) return hit;
            continue;
        }
        float first = (lo - start) / move, last = (hi - start) / move;
        P face = axis == 0 ? P(-1, 0) : P(0, -1);
        if (first > last) { std::swap(first, last); face = face * -1; }
        if (first > enter) { enter = first; normal = face; }
        leave = std::min(leave, last);
        if (enter > leave) return hit;
    }
    if (leave <= 0.000001f || enter < -0.000001f || enter > 1 || enter > leave) return hit;
    hit.blocked = true; hit.fraction = std::max(0.0f, enter); hit.normal = normal;
    return hit;
}
struct World {
    std::vector<Rect> boxes;
    Hit Sweep(P from, P to) const {
        Hit best;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            Hit hit = SweepRect(from, to, boxes[i], kRadius);
            if (hit.blocked && (!best.blocked || hit.fraction < best.fraction)) {
                hit.index = i; best = hit;
            }
        }
        return best;
    }
    bool Clear(P from, P to) const { return !Sweep(from, to).blocked; }
    bool Unpenetrated(P at) const {
        for (const Rect &box : boxes) if (Inside(at, box, kRadius)) return false;
        return true;
    }
    void Advance(P &position, P &velocity, P wish) {
        // Source ground friction: surfaceFriction=1, sv_friction=4,
        // sv_stopspeed=80. Acceleration uses sv_accelerate=10.
        const float speed = Size(velocity);
        if (speed >= 0.1f) {
            const float next = std::max(0.0f, speed - std::max(speed, 80.0f) * 4 * kDt);
            velocity = velocity * (next / speed);
        }
        const float requested = std::min(kSpeed, Size(wish));
        const P direction = Unit(wish);
        const float add = requested - Dot(velocity, direction);
        if (add > 0) velocity = velocity + direction * std::min(add, 10 * kDt * requested);
        float time = kDt;
        for (int bump = 0; bump < 4 && time > 0.000001f; ++bump) {
            const P delta = velocity * time;
            const Hit hit = Sweep(position, position + delta);
            if (!hit.blocked) { position = position + delta; break; }
            const float skinFraction = Size(delta) > 0 ? 0.002f / Size(delta) : 0;
            position = position + delta * std::max(0.0f, hit.fraction - skinFraction);
            Rect &box = boxes[hit.index];
            if (box.pushable && hit.normal.x < -0.5f && delta.x > 0) {
                // A compliant movable prop shares the remaining forward motion.
                // This is an independent collision response, not policy progress.
                const float shift = delta.x * (1 - hit.fraction) * 0.4f;
                box.minX += shift; box.maxX += shift;
                position.x = box.minX - kRadius - 0.002f;
                velocity.x *= 0.4f;
                break;
            }
            const float inward = Dot(velocity, hit.normal);
            if (inward < 0) velocity = velocity - hit.normal * inward;
            time *= 1 - hit.fraction;
        }
        Require(Unpenetrated(position), "independent movement never penetrates an expanded solid");
    }
};
P Forecast(P wish, P velocity) {
    P delta = wish * 0.2f;
    const float distance = Size(delta);
    if (distance > 0.0001f && distance < kRadius) delta = delta * (kRadius / distance);
    return delta + velocity * kDt;
}
struct Frame {
    Policy::Input input;
    Policy::Probes probes;
    Policy::Output output;
};
Frame Evaluate(Policy::State &state, const World &world, P at, P velocity,
               P goal, P wish, float time, int side = 1) {
    Frame frame;
    Policy::Input &input = frame.input;
    input.now = time; input.maxSpeed = kSpeed; input.goalDistance = Size(goal - at);
    input.owner = kOwner; input.goal = kGoal; input.wish = PolicyVector(wish);
    input.permitted = true; input.preferredSide = side;
    const P forwardEnd = at + Forecast(Native(Policy::OriginalWish(input)), velocity);
    const Hit contact = world.Sweep(at, forwardEnd);
    input.forwardClear = !contact.blocked;
    input.blockedByProp = contact.blocked && world.boxes[contact.index].prop;
    input.blocker = input.blockedByProp ? world.boxes[contact.index].id : 0;
    if (Policy::NeedsProbes(state, input)) {
        const auto safe = [&](int candidateSide, bool back) {
            const P candidate = Native(Policy::CandidateWish(input, candidateSide, back));
            return world.Clear(at, at + Forecast(candidate, velocity));
        };
        frame.probes.right = safe(1, false); frame.probes.left = safe(-1, false);
        frame.probes.rightBack = safe(1, true); frame.probes.leftBack = safe(-1, true);
        frame.probes.back = safe(0, true);
    }
    frame.output = Policy::Step(state, input, frame.probes);
    Require(Policy::Finite(frame.output.wish), "the policy emits finite command components");
    Require(Policy::Length(frame.output.wish) <= kSpeed + 0.001f, "the local command stays within the speed cap");
    if (state.active) Require(state.owner == kOwner && state.goal == kGoal, "active recovery preserves route authority and goal");
    if (frame.output.active && Policy::Length(frame.output.wish) > 0.01f)
        Require(world.Clear(at, at + Forecast(Native(frame.output.wish), velocity)),
            "an emitted detour was certified by the independent collision oracle");
    return frame;
}
World CrateWorld(bool narrow = false) {
    World world;
    world.boxes.push_back(Rect{20, -18, 52, 18, true, false, 0x200000101ULL});
    if (narrow) {
        world.boxes.push_back(Rect{-100, 18, 100, 30});
        world.boxes.push_back(Rect{-100, -30, 100, -18});
        world.boxes.push_back(Rect{-30, -100, -20, 100});
    }
    return world;
}
void TestOracle() {
    World world;
    world.boxes.push_back(Rect{40, -8, 56, 8, true, false, 11});
    Require(world.Clear(P(0, 30), P(100, 30)), "a visually near prop without hull contact is clear");
    Require(!world.Clear(P(0, 23), P(100, 23)), "the full-width hull catches an off-center prop");
    Require(!world.Clear(P(0, -50), P(100, 50)), "a swept hull catches collision despite clear endpoints");
    const Hit hit = world.Sweep(P(0, 0), P(100, 0));
    Require(hit.blocked && std::fabs(hit.fraction - 0.24f) < 0.0001f && hit.normal.x == -1,
        "independent Minkowski expansion reports the first actual hull contact");
}
void TestBypass(int side, bool oneSideClosed, bool wallAbove = false) {
    World world;
    world.boxes.push_back(Rect{65, -30, 115, 30, true, false, 0x300000121ULL});
    if (oneSideClosed) world.boxes.push_back(wallAbove ? Rect{40, 62, 140, 150} : Rect{40, -150, 140, -62});
    P position, velocity;
    const P goal(260, 0);
    Policy::State state;
    bool sawRecovery = false, arrived = false;
    float excursion = 0, arrivalTime = -1;
    int replans = 0;
    for (int tick = 0; tick < 16 * 64; ++tick) {
        const float distance = Size(goal - position);
        const P wish = Unit(goal - position) * std::min(kSpeed, distance * 4);
        const Frame frame = Evaluate(state, world, position, velocity, goal, wish, tick * kDt, side);
        if (frame.output.failed) {
            std::fprintf(stderr, "bypass detail: side=%d one-side-closed=%d wall-above=%d position=(%.3f,%.3f) distance=%.3f since=%.3f\n",
                side, oneSideClosed ? 1 : 0, wallAbove ? 1 : 0, position.x, position.y, distance, state.started);
        }
        Require(!frame.output.failed, "closed-loop Source ground motion can actually round a nonbreakable crate");
        sawRecovery = sawRecovery || frame.output.active;
        replans += frame.output.requestReplan ? 1 : 0;
        world.Advance(position, velocity, Native(frame.output.wish));
        excursion = std::max(excursion, std::fabs(position.y));
        if (Size(goal - position) < 1.5f && Size(velocity) < 2) {
            arrived = true; arrivalTime = tick * kDt; break;
        }
    }
    Require(sawRecovery && excursion > 46, "arrival includes a real full-hull detour outside the crate footprint");
    Require(arrived, "the detour rejoins the unchanged goal and settles there");
    Require(replans <= 1, "a successful local episode does not repeatedly recompute its route");
    std::printf("PASS motion bypass side=%d closed-side=%d wall-above=%d arrival=%.3fs excursion=%.3f recomputes=%d\n",
        side, oneSideClosed ? 1 : 0, wallAbove ? 1 : 0, arrivalTime, excursion, replans);
}
void TestProgressingPush() {
    World world = CrateWorld();
    world.boxes[0].pushable = true;
    P position, velocity;
    Policy::State state;
    float previousSampleX = 0;
    int lastSample = 0, actualProgressSamples = 0;
    bool sawProp = false;
    for (int tick = 0; tick < 3 * 64; ++tick) {
        const Frame frame = Evaluate(state, world, position, velocity, P(1000, 0), P(kSpeed, 0), tick * kDt);
        sawProp = sawProp || frame.input.blockedByProp;
        Require(!frame.output.active && !frame.output.requestReplan && !frame.output.failed,
            "a physically moving pushable prop does not activate a lateral recovery");
        Require(std::fabs(frame.output.wish.y) < 0.00001f, "normal pushing preserves the forward navigation wish");
        world.Advance(position, velocity, Native(frame.output.wish));
        if (tick - lastSample >= 20) {
            if (tick >= 40) {
                Require(position.x - previousSampleX >= 2, "actual pushed motion progresses at least two units per observation window");
                ++actualProgressSamples;
            }
            previousSampleX = position.x; lastSample = tick;
        }
    }
    Require(sawProp && actualProgressSamples >= 5 && position.x > 20 && std::fabs(position.y) < 0.001f,
        "progress comes from the independent coupled motion rather than a synthetic distance counter");
    std::printf("PASS progressing push actual-distance=%.3f samples=%d\n", position.x, actualProgressSamples);
}
void TestNarrowPassage(bool flicker) {
    World world = CrateWorld(true);
    Policy::State state;
    P position, velocity;
    const P goal(260, 0);
    float started = -1, replanAt = -1, failedAt = -1;
    int replans = 0, clearFrames = 0, blockerChanges = 0;
    std::uint64_t lastBlocker = 0;
    for (int tick = 0; tick < 10 * 64; ++tick) {
        if (flicker) {
            // A moving/disappearing contact can briefly make the original wish
            // clear. Alternate entity identities without renewing the episode.
            const bool clear = state.active && tick % 8 == 0;
            world.boxes[0].minX = clear ? 2000.0f : 20.0f;
            world.boxes[0].maxX = clear ? 2032.0f : 52.0f;
            world.boxes[0].id = 0x200000101ULL + static_cast<std::uint64_t>(tick / 23);
        }
        const Frame frame = Evaluate(state, world, position, velocity, goal, P(kSpeed, 0), tick * kDt);
        if (state.active && started < 0) started = state.started;
        if (started >= 0) {
            Require(state.active && state.started == started, "sub-threshold clearance and new blockers cannot renew the recovery budget");
            clearFrames += frame.input.forwardClear ? 1 : 0;
            if (frame.input.blocker && lastBlocker && frame.input.blocker != lastBlocker) ++blockerChanges;
            if (frame.input.blocker) lastBlocker = frame.input.blocker;
            if (!frame.input.forwardClear) {
                Require(!frame.probes.right && !frame.probes.left && !frame.probes.rightBack &&
                    !frame.probes.leftBack && !frame.probes.back, "all five candidates independently collide in a too-narrow passage");
                Require(Policy::Length(frame.output.wish) == 0, "no certified detour means a finite wait, not an unsafe movement");
            }
        }
        if (frame.output.requestReplan) { ++replans; replanAt = tick * kDt; }
        if (frame.output.failed) { failedAt = tick * kDt; break; }
        // Keep the flicker experiment's player physically fixed: it isolates
        // contact stability; the ordinary narrow passage runs the motion loop.
        if (!flicker) world.Advance(position, velocity, Native(frame.output.wish));
    }
    Require(started >= 0 && replans == 1, "a blocked episode requests exactly one route recomputation");
    Require(replanAt - started >= 3 && replanAt - started <= 3 + 2 * kDt, "recomputation is bounded at three seconds");
    Require(failedAt - started >= 6 && failedAt - started <= 6 + 8 * kDt, "unresolved recovery fails by its six-second budget");
    Require(state.owner == kOwner && state.goal == kGoal, "bounded failure retains the policy's route owner and original goal");
    if (flicker) Require(clearFrames > 10 && blockerChanges > 5, "budget stability includes real clearance flicker and multiple blockers");
    std::printf("PASS narrow passage flicker=%d observe=%.3f replan=%.3f fail=%.3f\n",
        flicker ? 1 : 0, started, replanAt - started, failedAt - started);
}
void TestReplanCandidates() {
    const int owned[] = {3, 7, kGoal, 50};
    const int unchanged[] = {3, 7, kGoal, 50};
    const int remaining[] = {kGoal, 50};
    const int prefix[] = {2, 3, 7, kGoal, 50};
    const int sameBlockedEdge[] = {9, 7, kGoal, 50};
    const int laterBranch[] = {7, kGoal, 41, 50};
    const int alternative[] = {7, 11, kGoal, 50};
    const int otherEntry[] = {11, 50};
    Require(!Policy::HasAlternativeRoute(owned, 4, 2, unchanged, 4) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, remaining, 2) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, prefix, 5) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, sameBlockedEdge, 4) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, laterBranch, 4),
        "rejoining old anchors or changing only after the blocked edge is not a bypass");
    Require(Policy::HasAlternativeRoute(owned, 4, 2, alternative, 4) &&
        Policy::HasAlternativeRoute(owned, 4, 2, otherEntry, 2),
        "a different authored branch or reachable graph entry can bypass the blocked leg");
    Require(!Policy::HasAlternativeRoute(nullptr, 4, 2, alternative, 4) &&
        !Policy::HasAlternativeRoute(owned, 4, 4, alternative, 4) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, alternative, 0) &&
        !Policy::HasAlternativeRoute(owned, 4, 2, alternative, 3),
        "invalid candidates and a changed final goal cannot replace the owned route");
}
void TestIntegratedReplanBudget(bool prepend) {
    World world = CrateWorld(true);
    Policy::State state;
    P position, velocity;
    std::vector<int> route = {7, kGoal};
    int index = 1, queries = 0, adoptions = 0, anchorArrivals = 0;
    float started = -1, replanAt = -1, failedAt = -1;
    for (int tick = 0; tick < 10 * 64; ++tick) {
        // An adopted BuildRoute candidate starts at its grounded anchor again.
        // This runs real motion: accepting the old [anchor, goal] path would
        // move back out of contact, reach the anchor and renew the episode.
        const P target = route[index] == kGoal ? P(260, 0) : P(-40, 0);
        const P wish = Unit(target - position) * kSpeed;
        const Frame frame = Evaluate(state, world, position, velocity, target, wish, tick * kDt);
        if (state.active && started < 0) started = state.started;
        if (frame.output.requestReplan) {
            ++queries; replanAt = tick * kDt;
            const std::vector<int> candidate = prepend ? std::vector<int>{9, 7, kGoal} :
                std::vector<int>{7, kGoal};
            if (Policy::HasAlternativeRoute(route.data(), static_cast<int>(route.size()), index,
                candidate.data(), static_cast<int>(candidate.size()))) {
                route = candidate; index = 0; ++adoptions;
            }
        }
        if (frame.output.failed) { failedAt = tick * kDt; break; }
        world.Advance(position, velocity, Native(frame.output.wish));
        if (route[index] != kGoal && Size(target - position) < 15) {
            ++index; ++anchorArrivals; Policy::Reset(state);
        }
    }
    Require(queries == 1 && adoptions == 0 && anchorArrivals == 0 && index == 1 &&
        route.size() == 2 && route[0] == 7 && route[1] == kGoal,
        "a query returning the old path preserves the live route index and cannot induce backtracking");
    Require(started >= 0 && replanAt - started >= 3 && replanAt - started <= 3 + 2 * kDt &&
        failedAt - started >= 6 && failedAt - started <= 6 + 2 * kDt,
        "integrated rejected replanning queries once at three seconds and fails at six");
    Require(state.owner == kOwner && state.goal == kGoal && state.started == started,
        "a rejected graph candidate preserves the same owner, final goal and collision deadline");
    std::printf("PASS integrated replan prepend=%d queries=%d adoptions=%d replan=%.3f fail=%.3f\n",
        prepend ? 1 : 0, queries, adoptions, replanAt - started, failedAt - started);
}
Policy::State ActiveState() {
    Policy::State state;
    const World world = CrateWorld(true);
    Evaluate(state, world, P(), P(), P(260, 0), P(kSpeed, 0), 0);
    const Frame frame = Evaluate(state, world, P(), P(), P(260, 0), P(kSpeed, 0), 0.3125f);
    Require(frame.output.active && state.active, "guard fixture begins with actual blocked recovery");
    return state;
}
void TestGuards() {
    const Policy::State active = ActiveState();
    const World world = CrateWorld(true);
    Policy::State seed = active;
    const Policy::Input good = Evaluate(seed, world, P(), P(), P(260, 0), P(kSpeed, 0), 0.328125f).input;
    for (int mode = 0; mode < 10; ++mode) {
        Policy::State state = active;
        Policy::Input input = good;
        switch (mode) {
        case 0: input.wish = Policy::Vec2(); break;
        case 1: input.permitted = false; break;
        case 2: input.owner = kOwner + 1; break;
        case 3: input.goal = kGoal + 1; break;
        case 4: input.wish.x = std::numeric_limits<float>::quiet_NaN(); break;
        case 5: input.now = std::numeric_limits<float>::quiet_NaN(); break;
        case 6: input.maxSpeed = std::numeric_limits<float>::quiet_NaN(); break;
        case 7: input.goalDistance = std::numeric_limits<float>::quiet_NaN(); break;
        case 8: input.owner = 0; break;
        case 9: input.now = 0.125f; break;
        }
        const Policy::Output output = Policy::Step(state, input, Policy::Probes());
        Require(!output.active && !output.requestReplan && !output.failed && !state.active,
            "stop, authority changes, invalid inputs and clock rollback cannot inherit an active detour");
        Require(Policy::Finite(output.wish), "invalid inputs cannot leak NaN movement");
        if (mode == 0 || mode == 4 || mode == 6)
            Require(Policy::Length(output.wish) == 0, "zero or invalid movement stays stopped");
        if (mode == 1)
            Require(output.wish.x == good.wish.x && output.wish.y == good.wish.y,
                "disallowed recovery preserves the caller's current authored wish");
        if (mode == 2 || mode == 3 || mode == 9)
            Require(state.started < 0 && output.wish.x > 0 && output.wish.y == 0,
                "new authority or game-clock epoch begins observation without an inherited sidestep");
    }
    Policy::State state = active;
    Policy::Input input = good;
    input.now = 1; input.forwardClear = true; input.blockedByProp = false; input.blocker = 0;
    Policy::Step(state, input, Policy::Probes());
    Require(state.active, "one clear frame does not erase collision memory");
    input.now = 1.3125f;
    const Policy::Output clear = Policy::Step(state, input, Policy::Probes());
    Require(!state.active && !clear.active, "sustained original-path clearance completes the recovery episode");
    Require(!Policy::KnownBreakHealth(true, 0) && !Policy::KnownBreakHealth(true, -1) &&
        !Policy::KnownBreakHealth(false, 0) && !Policy::KnownBreakHealth(false, 100) &&
        Policy::KnownBreakHealth(true, 100), "zero or unknown physics health never proves breakability");
    std::puts("PASS stop, permission, owner/goal changes, NaN, clock reset, stable rejoin and physics health");
}
}
int main() {
    TestOracle();
    TestBypass(1, false);
    TestBypass(-1, false);
    TestBypass(1, true);
    TestBypass(-1, true);
    // Mirror the adjacent wall as well as the preferred strafe. The earlier
    // failure oscillated before rounding the crate when both sideways wishes
    // became obstructed in turn despite a safe same-side retreat.
    TestBypass(1, true, true);
    TestBypass(-1, true, true);
    TestProgressingPush();
    TestNarrowPassage(false);
    TestNarrowPassage(true);
    TestReplanCandidates();
    TestIntegratedReplanBudget(false);
    TestIntegratedReplanBudget(true);
    TestGuards();
    std::puts("PASS independent rectangle-hull geometry and Source-style ground-prop recovery regressions");
}
