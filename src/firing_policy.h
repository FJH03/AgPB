/*
 * AgPB SyPB primary-fire button and AI-delay policy. GPL-3.0-only.
 * Baseline: SyPB 4c364fb source/combat.cpp FireWeapon/DoFirePause and
 * source/globals.cpp g_weaponSelect/g_fireDelay. Compared with CS-EBOT
 * 2333562 source/combat.cpp FireWeapon and source/globals.cpp.
 * SyPB basecode.cpp Think/ThinkFrame evaluates BotAI at most 15 Hz and
 * retains its buttons between player commands. Reproduce that fire-decision
 * latch while native commands and immediate safety checks remain per tick.
 * The caller supplies the existing DoFirePause-derived recoil pause,
 * reaction, LOS and ammunition gates, then commits only the attack button
 * actually submitted after all movement/stance filters. Native weapon
 * timers remain authoritative over real shots, not over requested buttons.
 * Scope, burst-mode, silencer and grenades are not
 * implemented here. Knife is basic primary attack, not full KnifeAttack.
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
#ifndef AGPB_FIRING_POLICY_H
#define AGPB_FIRING_POLICY_H
#include <cmath>
#include "weapon_selection.h"

namespace AgPBFiring {
using Weapon = AgPBWeaponSelect::Weapon;
// Local combat_visibility.h Summary semantics, not SyPB's numeric bit IDs
// and not AgPB's raw per-spot visibility mask. Caller must convert explicitly.
constexpr unsigned int BodyVisible = 1u, HeadVisible = 2u;
constexpr float DecisionInterval = 1.0f / 15.0f;
enum class Reason {
    Invalid, Unsupported, Reacting, Occluded, Empty, Reloading, Paused,
    Deadline, Release, EngineCooldown, AutomaticHold, AutomaticCooldownHold,
    Semiautomatic, KnifePrimary
};
struct State {
    Weapon weapon = Weapon::Unknown;
    float nextShotAt = 0.0f, lastTime = -1.0f;
    float nextDecisionAt = 0.0f;
    bool cachedAttack = false, cachedHold = false;
    Reason cachedReason = Reason::Invalid;
    float cachedDelay = 0.0f;
    std::uint64_t decisionId = 0, committedId = 0;
};
struct Input {
    Weapon weapon = Weapon::Unknown;
    float now = 0.0f, distance = 0.0f;
    int skill = 80;
    bool previousAttack = false; // Last actually submitted CUserCmd button.
    // Native readiness is diagnostic: SyPB requests a button regardless of
    // native cooldown. Never use this value to cut a latched primary request.
    bool canAttack = true;
    bool reacted = true, lineClear = true, clipReady = true;
    bool reloading = false, paused = false;
    unsigned int visibility = BodyVisible | HeadVisible;
    float randomUnit = 0.5f;    // Injected uniform sample, clamped to [0,1].
};
struct Output {
    bool attack = false, hold = false;
    bool freshDecision = false;
    std::uint64_t decisionId = 0;
    Reason reason = Reason::Invalid;
    float delay = 0.0f, nextShotAt = 0.0f;
    // Proposal identity: prevents stale/double commits from consuming delay.
    Weapon weapon = Weapon::Unknown;
    float evaluatedAt = -1.0f, previousDeadline = 0.0f;
};
inline void Reset(State &state) {
    // Preserve the token sequence across lifecycle/weapon/time resets. A
    // previously captured same-weapon proposal cannot match a fresh decision.
    const std::uint64_t previousId = state.decisionId;
    state = State();
    state.decisionId = previousId;
}
inline bool Supported(Weapon weapon) { return AgPBWeaponSelect::Preference(weapon) >= 0; }
inline bool PrimaryFireHold(Weapon weapon) {
    // Exact SyPB table: XM1014 and the auto-snipers are classified as
    // non-hold here even though native weapon capabilities may differ.
    return weapon == Weapon::Knife || (weapon >= Weapon::MP5 && weapon <= Weapon::AUG) || weapon == Weapon::M249;
}
inline int DelayIndex(int skill) {
    skill = skill < 1 ? 1 : skill > 100 ? 100 : skill;
    return 5 - skill / 20; // SyPB abs((skill / 20) - 5), six explicit entries.
}
inline float PrimaryBaseDelay(Weapon weapon) {
    switch (weapon) {
    case Weapon::Knife: return 0.10f;
    case Weapon::USP: case Weapon::Glock: case Weapon::XM1014: case Weapon::UMP45: return 0.15f;
    case Weapon::Deagle: case Weapon::Elite: return 0.20f;
    case Weapon::P228: case Weapon::FiveSeven: return 0.14f;
    case Weapon::M3: return 0.86f;
    case Weapon::MP5: case Weapon::P90: case Weapon::Famas: case Weapon::M249: return 0.10f;
    case Weapon::TMP: return 0.05f;
    case Weapon::MAC10: return 0.06f;
    case Weapon::AK47: case Weapon::Galil: return 0.09f;
    case Weapon::SG552: case Weapon::AUG: return 0.11f;
    case Weapon::M4A1: return 0.08f;
    case Weapon::Scout: return 0.18f;
    case Weapon::AWP: return 0.22f;
    case Weapon::G3SG1: case Weapon::SG550: return 0.25f;
    default: return 0.0f;
    }
}
inline bool Valid(const Input &input) {
    return std::isfinite(input.now) && input.now >= 0.0f &&
        std::isfinite(input.distance) && input.distance >= 0.0f && std::isfinite(input.randomUnit);
}
inline bool ResetRequired(const State &state, const Input &input) {
    return state.weapon != input.weapon || !std::isfinite(state.lastTime) ||
        !std::isfinite(state.nextShotAt) || !std::isfinite(state.nextDecisionAt) ||
        !std::isfinite(state.cachedDelay) || input.now < state.lastTime;
}
inline bool DecisionDue(const State &state, const Input &input) {
    // Read an existing DoFirePause timer only at this boundary. A successful
    // sniper attempt also sets that timer; reading it on the following native
    // tick would cancel its own still-latched button before native cooldown ends.
    // Newly rejected attempts still pass paused=true immediately to Step.
    return Valid(input) && Supported(input.weapon) &&
        (ResetRequired(state, input) || state.decisionId == 0 || input.now >= state.nextDecisionAt);
}
inline float DelayFor(const Input &input) {
    // Close fire has no added AI delay, while native cooldown still applies.
    // FireWeapon delegates distant knife handling before its delay assignment.
    if (input.distance < 256.0f || input.weapon == Weapon::Knife) return 0.0f;
    float delay = 0.0f;
    if (!PrimaryFireHold(input.weapon)) {
        // All firearm rows share these min/max arrays in the local baseline.
        // The knife has a different row, deliberately unused for AI timing.
        const float minimum[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.6f};
        const float maximum[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.7f};
        const int index = DelayIndex(input.skill);
        const float unit = input.randomUnit < 0.0f ? 0.0f : input.randomUnit > 1.0f ? 1.0f : input.randomUnit;
        delay = PrimaryBaseDelay(input.weapon) + minimum[index] + (maximum[index] - minimum[index]) * unit;
    }
    if (input.distance >= 1200.0f) {
        // Preserve the actual SyPB branch order. Its first test means ANY
        // head/body bit, so the subsequent head-only/body-only branches are
        // unreachable; do not silently reinterpret the OR as an all-bit test.
        if (input.visibility & (HeadVisible | BodyVisible)) {
            if (delay != 0.0f) delay -= 0.02f;
        } else if (input.visibility & HeadVisible) {
            delay += input.distance >= 2400.0f ? (delay == 0.0f ? 0.15f : 0.10f)
                                              : (delay == 0.0f ? 0.10f : 0.05f);
        } else if (input.visibility & BodyVisible) {
            delay += input.distance >= 2400.0f ? (delay == 0.0f ? 0.12f : 0.08f)
                                              : (delay == 0.0f ? 0.08f : 0.0f);
        } else {
            delay += input.distance >= 2400.0f ? (delay == 0.0f ? 0.18f : 0.15f)
                                              : (delay == 0.0f ? 0.15f : 0.10f);
        }
    }
    return delay;
}
inline const char *ReasonName(Reason reason) {
    switch (reason) {
    case Reason::Unsupported: return "unsupported";
    case Reason::Reacting: return "reacting";
    case Reason::Occluded: return "fire-blocked";
    case Reason::Empty: return "empty";
    case Reason::Reloading: return "reloading";
    case Reason::Paused: return "recoil-pause";
    case Reason::Deadline: return "ai-delay";
    case Reason::Release: return "semi-release";
    case Reason::EngineCooldown: return "engine-cooldown";
    case Reason::AutomaticHold: return "automatic-hold";
    case Reason::AutomaticCooldownHold: return "automatic-cooldown-hold";
    case Reason::Semiautomatic: return "semiautomatic";
    case Reason::KnifePrimary: return "knife-primary";
    default: return "invalid";
    }
}
inline Output Step(State &state, const Input &input) {
    Output out;
    if (!Valid(input)) { Reset(state); return out; }
    if (!Supported(input.weapon)) { Reset(state); out.reason = Reason::Unsupported; return out; }
    const bool fresh = DecisionDue(state, input);
    if (ResetRequired(state, input)) Reset(state);
    state.weapon = input.weapon; state.lastTime = input.now;
    if (fresh) {
        ++state.decisionId;
        if (state.decisionId == 0) ++state.decisionId;
        // Match the source's now+interval timer, without catch-up decisions.
        state.nextDecisionAt = input.now + DecisionInterval;
        state.cachedAttack = false;
        state.cachedHold = PrimaryFireHold(input.weapon);
        state.cachedDelay = 0.0f;
        state.cachedReason = Reason::Invalid;
    }
    out.weapon = input.weapon; out.evaluatedAt = input.now;
    out.freshDecision = fresh; out.decisionId = state.decisionId;
    out.previousDeadline = out.nextShotAt = state.nextShotAt;
    out.hold = state.cachedHold;
    Reason blocked = Reason::Invalid;
    if (!input.reacted) blocked = Reason::Reacting;
    else if (!input.lineClear) blocked = Reason::Occluded;
    else if (input.reloading) blocked = Reason::Reloading;
    else if (!input.clipReady) blocked = Reason::Empty;
    else if (input.paused) blocked = Reason::Paused;
    if (blocked != Reason::Invalid) {
        // Invalidate this decision's cached button even if the blocking
        // condition clears on the next native tick. In particular, a new
        // recoil rejection may have no future pause timer in the source.
        state.cachedAttack = false;
        state.cachedReason = blocked;
        out.reason = blocked;
        return out;
    }
    if (fresh) {
        if (input.now < state.nextShotAt) state.cachedReason = Reason::Deadline;
        // Only the source's close branch checks oldbuttons. Its distant
        // branch requests attack directly and relies on m_shootTime/native
        // weapon behavior; do not invent an additional far toggle or gate.
        else if (!state.cachedHold && input.distance < 256.0f && input.previousAttack)
            state.cachedReason = Reason::Release;
        else {
            state.cachedAttack = true;
            state.cachedDelay = DelayFor(input);
            state.cachedReason = input.weapon == Weapon::Knife ? Reason::KnifePrimary : state.cachedHold
                ? Reason::AutomaticHold : Reason::Semiautomatic;
        }
    }
    out.attack = state.cachedAttack;
    out.reason = state.cachedReason;
    if (out.attack && out.hold && !input.canAttack) out.reason = Reason::AutomaticCooldownHold;
    out.delay = state.cachedDelay;
    // A consumed decision reports its real deadline; an unsubmitted latch
    // keeps the same sampled delay but can commit after braking/pose settles.
    if (out.attack && state.committedId != state.decisionId) out.nextShotAt = input.now + out.delay;
    return out;
}
inline bool Commit(State &state, const Input &input, const Output &proposal, bool submittedAttack) {
    if (!submittedAttack || !proposal.attack || !Valid(input) ||
        state.weapon != input.weapon || proposal.weapon != input.weapon ||
        proposal.evaluatedAt != input.now || state.lastTime != input.now ||
        proposal.decisionId == 0 || proposal.decisionId != state.decisionId ||
        state.committedId == proposal.decisionId || !state.cachedAttack ||
        proposal.delay != state.cachedDelay || !input.reacted || !input.lineClear ||
        !input.clipReady || input.reloading || input.paused ||
        !std::isfinite(input.now + state.cachedDelay)) return false;
    // This is a submitted button, not proof of a real shot. Native cooldown
    // does not change source trigger cadence, and latched subsequent commands
    // must not continually push the AI deadline into the future.
    state.nextShotAt = input.now + state.cachedDelay;
    state.committedId = proposal.decisionId;
    return true;
}
}
#endif
