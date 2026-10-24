// Source-independent regression of SyPB's 15 Hz fire-decision/button latch.
#include "../src/firing_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float actual, float expected) { return std::fabs(actual - expected) < 0.0001f; }

int main() {
    using namespace AgPBFiring;
    State state; Input input;
    input.weapon = Weapon::M4A1; input.now = 10; input.distance = 500;
    Output out = Step(state, input);
    const std::uint64_t first = out.decisionId;
    Require(out.attack && out.hold && out.freshDecision && first != 0 && Near(out.delay, 0),
        "first automatic decision requests hold without a fixed native or 0.15-second AI interval");
    Require(Commit(state, input, out, true) && Near(state.nextShotAt, 10),
        "first actually submitted decision consumes its zero AI delay once");
    for (int tick = 1; tick <= 6; ++tick) {
        input.now = 10 + tick * 0.01f; input.canAttack = false; input.previousAttack = true;
        out = Step(state, input);
        Require(out.attack && out.hold && !out.freshDecision && out.decisionId == first &&
            out.reason == Reason::AutomaticCooldownHold,
            "native commands retain the same automatic decision through native cooldown");
        Require(!Commit(state, input, out, true) && Near(state.nextShotAt, 10),
            "latched native commands cannot refresh a committed decision's AI timer");
    }
    input.now = state.nextDecisionAt + 0.001f;
    out = Step(state, input);
    Require(out.attack && out.freshDecision && out.decisionId != first && !input.canAttack,
        "next decision may request an automatic button while the engine is still on cooldown");
    Require(Commit(state, input, out, true) && Near(state.nextShotAt, input.now),
        "native readiness is not an extra gate on first submitted decision commitment");
    Require(PrimaryFireHold(Weapon::MP5) && PrimaryFireHold(Weapon::P90) && PrimaryFireHold(Weapon::Famas) &&
        PrimaryFireHold(Weapon::M249) && !PrimaryFireHold(Weapon::XM1014) && !PrimaryFireHold(Weapon::SG550),
        "hold classification reproduces the baseline table rather than inferred native capabilities");

    Reset(state); input = Input(); input.weapon = Weapon::M4A1; input.now = 20;
    input.distance = 1500; input.visibility = 0;
    out = Step(state, input);
    Require(Near(out.delay, 0.15f) && Commit(state, input, out, true) && Near(state.nextShotAt, 20.15f),
        "source no-head/body distant automatic delay is committed once");
    input.now = 20.02f; input.previousAttack = true; input.canAttack = false;
    out = Step(state, input);
    Require(out.attack && !out.freshDecision && Near(out.nextShotAt, 20.15f),
        "source buttons remain latched until next decision even after setting m_shootTime");
    Require(!Commit(state, input, out, true), "held timer-producing decision cannot postpone its own deadline");
    input.now = state.nextDecisionAt + 0.001f;
    out = Step(state, input);
    Require(out.freshDecision && !out.attack && out.reason == Reason::Deadline,
        "next AI decision releases attack while the committed shoot time remains in the future");
    input.now = state.nextDecisionAt + 0.001f;
    Require(!Step(state, input).attack, "second decision still respects the same original future deadline");
    input.now = 20.16f;
    out = Step(state, input);
    Require(!out.freshDecision && !out.attack && out.reason == Reason::Deadline,
        "deadline expiry between AI decisions does not cause a native-tick early re-press");
    input.now = state.nextDecisionAt + 0.001f;
    Require(Step(state, input).attack, "next due decision can resume the automatic trigger");

    Reset(state); input = Input(); input.weapon = Weapon::USP; input.now = 30;
    input.distance = 100; input.skill = 1; input.canAttack = false;
    out = Step(state, input);
    Require(out.attack && !out.hold && out.freshDecision && Near(out.delay, 0),
        "close pistol requests a button without native readiness or extra skill delay");
    Require(Commit(state, input, out, true), "first submitted near pistol decision commits even during native cooldown");
    input.now = 30.01f; input.previousAttack = true;
    out = Step(state, input);
    Require(out.attack && !out.freshDecision, "pistol press lasts its decision interval instead of one native tick");
    input.now = state.nextDecisionAt + 0.001f;
    out = Step(state, input);
    Require(out.freshDecision && !out.attack && out.reason == Reason::Release,
        "only a new close-range decision releases the previously submitted pistol button");
    input.now += 0.01f; input.previousAttack = false;
    out = Step(state, input);
    Require(!out.attack && !out.freshDecision && out.reason == Reason::Release,
        "pistol release also lasts a full decision interval");
    input.now = state.nextDecisionAt + 0.001f;
    Require(Step(state, input).attack, "next close pistol decision may re-press after an actual release");

    Reset(state); input = Input(); input.weapon = Weapon::USP; input.now = 40;
    input.distance = 256; input.skill = 100; input.randomUnit = 0.5f;
    input.previousAttack = true; input.canAttack = false;
    out = Step(state, input);
    Require(out.attack && out.freshDecision && !out.hold && Near(out.delay, 0.20f),
        "far pistol follows source direct press and skill delay without adding old-button/native gates");
    Require(!Commit(state, input, out, false) && Near(state.nextShotAt, 0),
        "movement/pose filtering of a decision consumes no unsubmitted AI delay");
    input.now = 40.03f; input.randomUnit = 1.0f;
    out = Step(state, input);
    Require(out.attack && !out.freshDecision && Near(out.delay, 0.20f),
        "filtered decision retains its sampled delay and braking request across native ticks");
    Require(Commit(state, input, out, true) && Near(state.nextShotAt, 40.23f),
        "delayed first actual submission starts AI delay from submission time, not evaluation time");
    input.now = 40.04f; out = Step(state, input);
    Require(!Commit(state, input, out, true) && Near(state.nextShotAt, 40.23f),
        "later held command cannot move a semi-auto deadline forward");

    // A stationary sniper attempt succeeds in DoFirePause but sets its .1s
    // timer even if native cooldown has not expired. Existing pause timers
    // belong to the next AI decision, not to the intervening native commands.
    Reset(state); input = Input(); input.weapon = Weapon::AWP; input.now = 45;
    input.distance = 800; input.skill = 100; input.canAttack = false;
    float pauseUntil = 0.0f;
    Require(DecisionDue(state, input), "first sniper command has a due fire decision");
    input.paused = DecisionDue(state, input) && input.now < pauseUntil;
    out = Step(state, input);
    Require(out.freshDecision && out.attack && !out.hold, "stationary sniper proposes attack during native cooldown");
    const std::uint64_t sniperDecision = out.decisionId;
    pauseUntil = input.now + 0.1f; // Successful source DoFirePause side effect.
    Require(Commit(state, input, out, true), "successful sniper button commits once during native cooldown");
    input.now += 0.03f; input.canAttack = true; input.previousAttack = true;
    Require(!DecisionDue(state, input), "native cooldown expiry is not a new fire decision");
    input.paused = DecisionDue(state, input) && input.now < pauseUntil;
    out = Step(state, input);
    Require(out.attack && !out.freshDecision && out.decisionId == sniperDecision && !input.paused,
        "successful sniper keeps its button until native cooldown expires without self-cancelling on its pause timer");
    Require(!Commit(state, input, out, true), "held sniper command cannot reconsume its AI delay");
    input.now = state.nextDecisionAt + 0.001f;
    Require(DecisionDue(state, input) && input.now < pauseUntil, "next sniper decision sees the existing pause window");
    input.paused = DecisionDue(state, input) && input.now < pauseUntil;
    out = Step(state, input);
    Require(out.freshDecision && !out.attack && out.reason == Reason::Paused && out.decisionId != sniperDecision,
        "existing sniper pause blocks the next decision and releases its button");

    // An instantaneous pause can intentionally have no future timer, as in
    // DoFirePause's already-recent recoil rejection. It still cancels the latch.
    for (int gate = 0; gate < 5; ++gate) {
        Reset(state); input = Input(); input.weapon = Weapon::M4A1;
        input.now = 50 + gate * 2.0f; input.distance = 600;
        const Output beforeBlock = Step(state, input);
        Require(beforeBlock.attack && beforeBlock.freshDecision, "safety case begins with a real new attack decision");
        Reason expected;
        if (gate == 0) { input.reacted = false; expected = Reason::Reacting; }
        else if (gate == 1) { input.lineClear = false; expected = Reason::Occluded; }
        else if (gate == 2) { input.clipReady = false; expected = Reason::Empty; }
        else if (gate == 3) { input.reloading = true; expected = Reason::Reloading; }
        else { input.paused = true; expected = Reason::Paused; }
        out = Step(state, input);
        Require(!out.attack && !out.freshDecision && out.decisionId == beforeBlock.decisionId && out.reason == expected,
            "same-tick safety rejection cancels the current decision without creating a second decision");
        Require(!Commit(state, input, beforeBlock, true), "pre-rejection proposal cannot be committed after latch invalidation");
        input.reacted = input.lineClear = input.clipReady = true; input.reloading = input.paused = false;
        input.now += 0.01f; out = Step(state, input);
        Require(!out.attack && !out.freshDecision && Near(state.nextShotAt, 0),
            "clearing safety condition cannot leak the rejected attack on the next native tick");
        input.now = state.nextDecisionAt + 0.001f;
        out = Step(state, input);
        Require(out.attack && out.freshDecision && out.decisionId != beforeBlock.decisionId,
            "a later due safe decision may regenerate the primary button");
    }

    input = Input(); input.weapon = Weapon::USP; input.skill = 100; input.distance = 500; input.randomUnit = 0.5f;
    Require(DelayIndex(1) == 5 && DelayIndex(20) == 4 && DelayIndex(80) == 1 && DelayIndex(100) == 0 &&
        Near(DelayFor(input), 0.20f), "six source skill buckets keep the Godlike half-auto delay");
    input.skill = 1;
    Require(Near(DelayFor(input), 0.80f), "novice half-auto uses the sixth min/max row");
    input.weapon = Weapon::M3;
    Require(Near(DelayFor(input), 1.51f), "pump shotgun keeps its distinct source base delay");
    input.weapon = Weapon::AWP; input.skill = 100;
    Require(Near(DelayFor(input), 0.27f), "AWP retains its per-weapon half-auto base delay");
    input.weapon = Weapon::M4A1;
    for (int skill = 1; skill <= 100; ++skill) {
        input.skill = skill;
        Require(Near(DelayFor(input), 0), "automatic visible fire has no invented skill-dependent fixed tap interval");
    }
    input.weapon = Weapon::USP; input.skill = 100; input.distance = 1500;
    input.visibility = HeadVisible;
    Require(Near(DelayFor(input), 0.18f), "head-only takes source's first any-head/body far branch");
    input.visibility = BodyVisible;
    Require(Near(DelayFor(input), 0.18f), "body-only does not reach the later unreachable body branch");
    input.visibility = HeadVisible | BodyVisible;
    Require(Near(DelayFor(input), 0.18f), "both visible bits keep the same source adjustment");
    input.visibility = 0;
    Require(Near(DelayFor(input), 0.30f), "neither head nor body retains the explicit far half-auto addition");
    input.distance = 2400;
    Require(Near(DelayFor(input), 0.35f), "longest-distance half-auto addition remains faithful");
    input.weapon = Weapon::M4A1;
    Require(Near(DelayFor(input), 0.18f), "automatic far no-head/body branch retains its actual source pause");
    input.visibility = HeadVisible;
    Require(Near(DelayFor(input), 0), "head-visible automatic does not get a guessed fixed long-range burst");

    Reset(state); input = Input(); input.weapon = Weapon::M4A1; input.distance = 500;
    int decisions = 0;
    for (int tick = 0; tick <= 64; ++tick) {
        input.now = tick / 64.0f;
        const bool due = DecisionDue(state, input);
        out = Step(state, input);
        Require(out.freshDecision == due, "read-only decision boundary matches Step on every native tick");
        if (out.freshDecision) ++decisions;
        Require(out.attack, "automatic remains held between quantized 15 Hz decisions");
        Commit(state, input, out, true);
        input.previousAttack = true;
    }
    Require(decisions == 13, "source now-plus-interval cadence is quantized to due native ticks without catch-up decisions");

    Reset(state); input = Input(); input.weapon = Weapon::USP; input.now = 80; input.distance = 500;
    const Output oldProposal = Step(state, input);
    Reset(state);
    const Output newer = Step(state, input);
    Require(newer.freshDecision && newer.decisionId != oldProposal.decisionId &&
        !Commit(state, input, oldProposal, true) && Commit(state, input, newer, true),
        "same-weapon same-time reset cannot resurrect a stale proposal token");
    input.now += 0.01f; out = Step(state, input);
    Require(!Commit(state, input, newer, true), "proposal evaluated at an older native time cannot be replayed");
    const std::uint64_t oldId = out.decisionId;
    input.weapon = Weapon::M4A1;
    Require(DecisionDue(state, input), "weapon reset is visible to the read-only decision boundary");
    out = Step(state, input);
    Require(out.freshDecision && out.decisionId != oldId && out.attack && state.nextShotAt == 0,
        "weapon change resets the old deadline and decision latch");
    input.now = 1;
    Require(DecisionDue(state, input), "time reversal is visible to the read-only decision boundary");
    out = Step(state, input);
    Require(out.freshDecision && out.attack && out.decisionId != oldId, "time reversal invalidates stale timing and keeps token identity distinct");
    state.cachedDelay = std::numeric_limits<float>::infinity();
    Require(DecisionDue(state, input) && Step(state, input).freshDecision,
        "nonfinite cached timing resets at the same boundary reported by DecisionDue");
    input.now = std::numeric_limits<float>::quiet_NaN();
    Require(!DecisionDue(state, input), "invalid input has no new decision boundary");
    Require(Step(state, input).reason == Reason::Invalid && state.weapon == Weapon::Unknown,
        "nonfinite time resets active firing state safely");
    input.now = 2; input.distance = -1;
    Require(!Step(state, input).attack, "negative distance cannot propose fire");
    input.distance = 500; input.randomUnit = std::numeric_limits<float>::infinity();
    Require(Step(state, input).reason == Reason::Invalid, "nonfinite random sample cannot create timing state");
    input.randomUnit = 0.5f; input.weapon = Weapon::Grenade;
    Require(!DecisionDue(state, input), "unsupported weapons have no fire decision boundary");
    Require(Step(state, input).reason == Reason::Unsupported, "grenades never enter primary gunfire policy");
    input.weapon = Weapon::C4;
    Require(!Step(state, input).attack, "C4 remains an objective action");
    Reset(state);
    Require(state.nextShotAt == 0 && state.nextDecisionAt == 0 && state.lastTime == -1 &&
        !state.cachedAttack && state.weapon == Weapon::Unknown && state.committedId == 0,
        "lifecycle reset releases deadlines, latch and commit history");
    std::puts("firing policy tests passed");
}
