/*
 * AgPB normal-mode combat movement policy. GPL-3.0-only.
 * Adapted from SyPB 4c364fb source/combat.cpp Bot::CombatFight and weapon
 * predicates; compared with CS-EBOT 2333562 source/combat.cpp. CS-EBOT shares
 * the SMG lists but omits XM1014 from UsesBadPrimary, and has no equivalent
 * normal-mode CombatFight core. Normal shotguns follow the SyPB predicate.
 * Zombie/NPC branches, random combat jumps and engine-bot AI are excluded.
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
#ifndef AGPB_COMBAT_MOVEMENT_H
#define AGPB_COMBAT_MOVEMENT_H
#include <cmath>

namespace AgPBCombatMove {
enum class Weapon { Knife, Pistol, Shotgun, SMG, Rifle, Sniper, MachineGun, Unknown };
enum class FightStyle { Stay, Strafe };
enum class Motion { Invalid, Hold, Approach, Retreat, ReloadRetreat, Strafe, Crouch };
enum class StrafePlan { Weapon, Distance, View, Reload, Crouch, Retreat, Moving, Precision };

struct Input {
    Weapon weapon = Weapon::Unknown;
    float now = 0.0f, distance = 0.0f, health = 100.0f, aggression = 0.5f;
    float maxSpeed = 250.0f, walkSpeed = 120.0f;
    int skill = 80;
    // IN_ATTACK visible at this CombatFight call. StepFrame supplies zero to
    // the pre-fire task call and the actual proposal to the late override.
    // Desire to fire or a remembered firing window is not this button state.
    bool attacking = false;
    bool reloading = false, enemyInViewCone = false, bombPlanted = false;
    // SyPB UsesBadPrimary includes UMP45/MAC10/TMP/P90, but excludes MP5.
    // Set this for those SMGs; shotguns are handled by their category below.
    bool mobilePrimary = false;
    // Required authored crouch is retained even while moving. canDuck means
    // the caller verified that crouching still leaves a usable target view.
    bool crouchRequired = false, canDuck = false;
    // Dot(self-enemy direction, enemy right). Only its sign is used when a
    // strafe lease starts; movement output is in the bot-to-enemy basis.
    float enemySide = 0.0f;
};

struct Random {
    // Inject fresh [0,1] samples. Choices consume them only at their original
    // SyPB timer/chance boundaries, so a direction does not change every tick.
    float style = 0.5f, strafeFlip = 0.5f, duration = 0.5f, duck = 0.5f;
};

struct State {
    bool initialized = false;
    Weapon weapon = Weapon::Unknown;
    FightStyle style = FightStyle::Stay;
    float lastTime = 0.0f, nextStyleAt = 0.0f, strafeUntil = 0.0f, duckUntil = 0.0f;
    int strafeSign = 1;
};

struct Output {
    // Forward is towards the currently visible enemy; positive side is right
    // in that same horizontal basis, independent of the aim view's yaw.
    float forward = 0.0f, side = 0.0f;
    bool duck = false;
    // Caller must withhold the actual shot until reflected horizontal speed
    // has settled; this policy never writes velocity or synthesizes braking.
    bool stopForAccuracy = false;
    Motion motion = Motion::Hold;
    StrafePlan strafe = StrafePlan::Weapon;
};

struct Axes {
    float forward = 0.0f, side = 0.0f;
};

template<class CanStep>
Axes AlternateStep(const Output &movement, CanStep canStep) {
    if (movement.side == 0.0f) return {};
    const float forward = movement.forward;
    const float side = movement.side;
    if (canStep(forward, -side)) return {forward, -side};
    if (forward == 0.0f) return {};
    if (canStep(0.0f, side)) return {0.0f, side};
    if (canStep(0.0f, -side)) return {0.0f, -side};
    if (canStep(forward, 0.0f)) return {forward, 0.0f};
    return {};
}

inline void Reset(State &state) { state = State(); }
inline float Clamp(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}
inline float CombatApproach(Weapon weapon, float health, float aggression, bool reloading) {
    if (reloading) return 19.0f;
    const float approach = health * Clamp(aggression, 0.0f, 1.0f);
    if (weapon == Weapon::SMG) return approach + 20.0f;
    if (weapon == Weapon::Pistol || weapon == Weapon::Sniper) return Clamp(approach, 0.0f, 49.0f);
    return approach;
}
inline bool WantsToWithdraw(float approach, bool bombPlanted, bool enemyInViewCone) {
    return approach < 20.0f && !bombPlanted && enemyInViewCone;
}
inline float Sample(float value) { return std::isfinite(value) ? Clamp(value, 0.0f, 1.0f) : 0.5f; }
inline bool KnownWeapon(Weapon weapon) {
    return weapon == Weapon::Knife || weapon == Weapon::Pistol || weapon == Weapon::Shotgun ||
        weapon == Weapon::SMG || weapon == Weapon::Rifle || weapon == Weapon::Sniper || weapon == Weapon::MachineGun;
}
inline bool Valid(const Input &in) {
    return KnownWeapon(in.weapon) && std::isfinite(in.now) && in.now >= 0.0f &&
        std::isfinite(in.distance) && in.distance >= 0.0f &&
        std::isfinite(in.health) && in.health > 0.0f &&
        std::isfinite(in.aggression) && std::isfinite(in.enemySide) &&
        std::isfinite(in.maxSpeed) && in.maxSpeed > 0.0f &&
        std::isfinite(in.walkSpeed) && in.walkSpeed >= 0.0f;
}
inline bool Finite(const State &state) {
    return std::isfinite(state.lastTime) && std::isfinite(state.nextStyleAt) &&
        std::isfinite(state.strafeUntil) && std::isfinite(state.duckUntil) &&
        (state.strafeSign == -1 || state.strafeSign == 1);
}
inline const char *MotionName(Motion motion) {
    switch (motion) {
    case Motion::Invalid: return "invalid";
    case Motion::Approach: return "approach";
    case Motion::Retreat: return "retreat";
    case Motion::ReloadRetreat: return "reload-retreat";
    case Motion::Strafe: return "strafe";
    case Motion::Crouch: return "crouch";
    default: return "hold";
    }
}

inline StrafePlan PlanProactiveStrafe(const Input &in, bool duck, float forward) {
    if (in.weapon != Weapon::Rifle && in.weapon != Weapon::Pistol) return StrafePlan::Weapon;
    if (in.distance >= (in.weapon == Weapon::Rifle ? 768.0f : 544.0f)) return StrafePlan::Distance;
    if (!in.enemyInViewCone) return StrafePlan::View;
    if (in.reloading) return StrafePlan::Reload;
    if (duck) return StrafePlan::Crouch;
    if (forward < 0.0f) return StrafePlan::Retreat;
    return StrafePlan::Moving;
}

inline Output Step(State &state, const Input &in, const Random &random = Random()) {
    Output out;
    if (!Valid(in)) { Reset(state); out.motion = Motion::Invalid; return out; }
    const bool reset = !state.initialized || state.weapon != in.weapon || !Finite(state) || in.now < state.lastTime;
    if (reset) {
        Reset(state);
        state.initialized = true;
        state.weapon = in.weapon;
        state.nextStyleAt = in.now;
    }
    state.lastTime = in.now;
    out.duck = in.crouchRequired;
    const float walk = Clamp(in.walkSpeed, 0.0f, in.maxSpeed);
    const bool mobile = in.weapon == Weapon::Shotgun || (in.weapon == Weapon::SMG && in.mobilePrimary);
    const int skill = in.skill < 1 ? 1 : in.skill > 100 ? 100 : in.skill;
    if (in.weapon == Weapon::Knife) {
        // SyPB's non-zombie knife branch always closes distance. The caller
        // retains melee acquisition/range and all collision constraints.
        out.forward = in.maxSpeed;
        out.motion = Motion::Approach;
        return out;
    }

    // SyPB CombatFight normal branch: health/personality control approach,
    // SMGs favor closing, while pistols/snipers do not chase for aggression.
    const float approach = CombatApproach(in.weapon, in.health, in.aggression, in.reloading);
    bool setStrafe = false;
    if (WantsToWithdraw(approach, in.bombPlanted, in.enemyInViewCone)) {
        out.forward = -in.maxSpeed;
        setStrafe = true;
    } else if (approach >= 50.0f && (!in.attacking || mobile)) {
        out.forward = in.maxSpeed;
    }
    if (in.distance < 96.0f && in.weapon != Weapon::Sniper) {
        out.forward = -in.maxSpeed;
        setStrafe = true;
        if (in.weapon != Weapon::SMG) out.duck = true;
    }
    const bool closeRangeDuck = in.distance < 96.0f &&
        in.weapon != Weapon::SMG && in.weapon != Weapon::Sniper;
    // SyPB tests this command's attack bit, so an inter-shot command can still
    // approach. A Strafe style alone never authorizes healthy SMG side motion.
    if (in.attacking && !mobile) setStrafe = false;

    // YaPB ba08596 AttackMovement permits proactive rifle and pistol side
    // motion during fire. Keep AgPB's retreat, crouch and direction handling.
    out.strafe = PlanProactiveStrafe(in, out.duck, out.forward);
    const bool proactiveStrafe = out.strafe == StrafePlan::Moving;
    if (proactiveStrafe) {
        setStrafe = true;
        out.forward = 0.0f;
    }

    // GetWalkSpeed also sees a duck requested in this command, even before
    // posture settles. The caller supplies its other current/old-state gates.
    const float strafeSpeed = out.duck ? in.maxSpeed : walk;

    if (in.weapon == Weapon::Sniper) {
        state.style = FightStyle::Stay;
        state.nextStyleAt = in.now + 3.0f;
    } else if (reset || in.now > state.nextStyleAt) {
        const float roll = Sample(random.style);
        if (in.weapon == Weapon::Rifle || in.weapon == Weapon::SMG || in.weapon == Weapon::MachineGun) {
            if (in.distance < 450.0f) state.style = FightStyle::Strafe;
            else if (in.distance < 1024.0f) state.style = roll < (in.weapon == Weapon::SMG ? 0.50f : 0.30f)
                ? FightStyle::Strafe : FightStyle::Stay;
            else state.style = roll < (in.weapon == Weapon::SMG ? 0.80f : 0.93f)
                ? FightStyle::Stay : FightStyle::Strafe;
        } else state.style = roll < 0.65f ? FightStyle::Strafe : FightStyle::Stay;
        state.nextStyleAt = in.now + 3.0f;
    }
    if (proactiveStrafe || in.reloading || (skill >= 70 && state.style == FightStyle::Strafe && in.distance < 800.0f)) {
        if (!setStrafe) state.strafeUntil = in.now + 1.0f + 1.5f * Sample(random.duration);
        else {
            if (in.now > state.strafeUntil) {
                state.strafeSign = in.enemySide < 0.0f ? 1 : -1;
                if (Sample(random.strafeFlip) < 0.30f) state.strafeSign = -state.strafeSign;
                state.strafeUntil = in.now + 0.5f + 2.0f * Sample(random.duration);
            }
            // Outside close range, release movement briefly before the next
            // sidestep. The caller uses actual speed to gate the settled shot.
            const bool precisionWindow = proactiveStrafe && in.distance >= 256.0f &&
                state.strafeUntil - in.now <= 0.30f;
            if (precisionWindow) out.strafe = StrafePlan::Precision;
            else out.side = strafeSpeed * float(state.strafeSign);
        }
    } else if (state.style == FightStyle::Stay && out.forward == 0.0f &&
               in.canDuck && Sample(random.duck) < 0.10f) {
        state.duckUntil = in.now + 0.5f;
    }
    if (state.duckUntil > in.now) {
        out.forward = out.side = 0.0f;
        out.duck = true;
        if (proactiveStrafe) out.strafe = StrafePlan::Crouch;
    }
    // SyPB CombatFight's final walk conversion follows its timed duck hold:
    // SMGs retain full-speed advance; every firearm retreats at walk speed.
    // GetWalkSpeed returns max speed when this command requests ducking.
    const float finalWalk = out.duck ? in.maxSpeed : walk;
    if (out.forward > 0.0f && in.weapon != Weapon::SMG) out.forward = finalWalk;
    else if (out.forward < 0.0f) out.forward = -finalWalk;
    if (in.reloading) {
        // SyPB's final reload override retreats at full speed and releases
        // optional timed crouch. It clears m_duckTime, not this command's
        // IN_DUCK from the close-range branch (SyPB combat.cpp:1449,1588).
        // Required authored crouch remains owned by the route/environment.
        out.forward = -in.maxSpeed;
        state.duckUntil = 0.0f;
        out.duck = in.crouchRequired || closeRangeDuck;
        out.motion = Motion::ReloadRetreat;
    } else if (out.forward < 0.0f) out.motion = Motion::Retreat;
    else if (out.forward > 0.0f) out.motion = Motion::Approach;
    else if (out.side != 0.0f) out.motion = Motion::Strafe;
    else if (out.duck) out.motion = Motion::Crouch;
    out.stopForAccuracy = in.attacking && !mobile && !in.reloading && out.forward == 0.0f && out.side == 0.0f;
    return out;
}

inline Output StepFrame(State &state, const Input &in,
                        const Random &early = Random(), const Random &late = Random()) {
    // SyPB basecode.cpp:3305 clears buttons at ThinkFrame start. The selected
    // TASK_FIGHTENEMY calls CombatFight at3772 BEFORE FireWeapon at5425.
    // Only the additional ordinary-player override at5445/5461 (skill>=60)
    // can see that frame's later IN_ATTACK. Low skills retain the early move
    // and can therefore approach while the subsequent FireWeapon attacks.
    // The caller owns early Fight eligibility/task arbitration and must skip
    // this wrapper for holding tasks; visibility alone is not a Fight task.
    Input beforeFire = in;
    beforeFire.attacking = false;
    Output first = Step(state, beforeFire, early);
    // The early-only path still gates the actual attack during a precision stop.
    if (first.strafe == StrafePlan::Precision) first.stopForAccuracy = in.attacking;
    if (first.motion == Motion::Invalid || in.skill < 60) return first;
    return Step(state, in, late);
}
}
#endif
