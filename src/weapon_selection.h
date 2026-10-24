/*
 * AgPB owned-inventory weapon selection. GPL-3.0-only.
 * Behavior baseline: SyPB 4c364fb source/combat.cpp FireWeapon,
 * SelectBestWeapon, commented SelectPistol and IsWeaponBadInDistance;
 * exact preference order from source/globals.cpp g_weaponSelect.
 * Compared with CS-EBOT 2333562 source/combat.cpp and source/globals.cpp.
 * Zombie overrides, buying, inventory writes and close-range knife tricks
 * are excluded. Per-bot pending acknowledgement and cooldown guard the
 * Source CUserCmd transport; the caller owns handles and entity ownership.
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
#ifndef AGPB_WEAPON_SELECTION_H
#define AGPB_WEAPON_SELECTION_H
#include <cmath>
#include <cstdint>

namespace AgPBWeaponSelect {
// This order is the original table order, not prices, damage or game IDs.
enum class Weapon {
    Knife, USP, Glock, Deagle, P228, Elite, FiveSeven, M3, XM1014,
    MP5, TMP, P90, MAC10, UMP45, AK47, SG552, M4A1, Galil, Famas,
    AUG, Scout, AWP, G3SG1, SG550, M249, Unknown, C4, Grenade
};
constexpr float SwitchCooldown = 0.5f;
constexpr float PendingSeconds = 1.5f;
constexpr float FailedRetrySeconds = 1.0f;

struct Candidate {
    Weapon weapon = Weapon::Unknown;
    int clip = -1, reserve = -1;
    // Stable full identity, e.g. the complete EHANDLE, never a transient slot
    // or an index in this array. Zero is invalid; the caller verifies owner.
    std::uint64_t key = 0;
};
struct Input {
    float now = 0.0f;
    int current = -1;
    bool enemyVisible = false;
    float distance = 0.0f;
    bool underwater = false;
};
struct State {
    std::uint64_t pendingKey = 0, failedKey = 0, observedCurrentKey = 0;
    float pendingUntil = 0.0f, failedUntil = 0.0f, nextSwitchAt = 0.0f, lastTime = -1.0f;
};
enum class Reason { Unavailable, Invalid, Hold, Switch, Pending, Cooldown, RetryWait };
struct Selection {
    int index = -1;
    bool switchNow = false;
    bool waiting = false;
    // True only for an actually current, known-empty firearm with known
    // reserve ammunition. Never reload the old weapon while a switch waits.
    bool reload = false;
    Reason reason = Reason::Unavailable;
};

inline void Reset(State &state) { state = State(); }
inline int Preference(Weapon weapon) {
    const int rank = static_cast<int>(weapon);
    return rank >= static_cast<int>(Weapon::Knife) && rank <= static_cast<int>(Weapon::M249) ? rank : -1;
}
inline bool Pistol(Weapon weapon) {
    return weapon >= Weapon::USP && weapon <= Weapon::FiveSeven;
}
inline bool Shotgun(Weapon weapon) { return weapon == Weapon::M3 || weapon == Weapon::XM1014; }
inline bool Sniper(Weapon weapon) { return weapon >= Weapon::Scout && weapon <= Weapon::SG550; }
inline bool Firearm(Weapon weapon) { return Preference(weapon) > 0; }
inline bool CanFire(const Candidate &item) { return Firearm(item.weapon) && item.clip > 0; }
inline bool CanReload(const Candidate &item) { return Firearm(item.weapon) && item.clip == 0 && item.reserve > 0; }
inline bool Allowed(const Candidate &item, const Input &input) {
    return item.key != 0 && Preference(item.weapon) >= 0 && !(input.underwater && Shotgun(item.weapon));
}
inline bool BadDistance(const Candidate &item, bool current, const Input &input) {
    if (!input.enemyVisible) return false;
    // SyPB's 750-unit shotgun range is retained for acquisition. CS-EBOT's
    // current-weapon 768-unit boundary supplies a small retention interval;
    // its separate 512-unit acquisition cutoff is not substituted for SyPB.
    if (Shotgun(item.weapon)) return input.distance > (current ? 768.0f : 750.0f);
    return Sniper(item.weapon) && input.distance < 300.0f;
}
inline const char *ReasonName(Reason reason) {
    switch (reason) {
    case Reason::Invalid: return "invalid";
    case Reason::Hold: return "hold";
    case Reason::Switch: return "switch";
    case Reason::Pending: return "pending";
    case Reason::Cooldown: return "cooldown";
    case Reason::RetryWait: return "retry-wait";
    default: return "unavailable";
    }
}
inline int FindKey(const Candidate *items, int count, std::uint64_t key) {
    if (key == 0) return -1;
    for (int index = 0; index < count; ++index) if (items[index].key == key) return index;
    return -1;
}
inline bool Blocked(const Candidate &item, std::uint64_t currentKey, const State &state, float now) {
    return item.key == state.failedKey && item.key != currentKey && now < state.failedUntil;
}
inline Selection Held(const Candidate *items, int index, const Input &input, Reason reason) {
    Selection out; out.index = index; out.reason = reason;
    out.reload = index >= 0 && index == input.current && CanReload(items[index]);
    return out;
}

inline Selection Step(State &state, const Candidate *items, int count, const Input &input) {
    Selection invalid;
    if (count < 0 || (count > 0 && items == nullptr) || !std::isfinite(input.now) || input.now < 0.0f ||
        (input.enemyVisible && (!std::isfinite(input.distance) || input.distance < 0.0f))) {
        Reset(state); invalid.reason = Reason::Invalid; return invalid;
    }
    if (!std::isfinite(state.lastTime) || !std::isfinite(state.pendingUntil) ||
        !std::isfinite(state.failedUntil) || !std::isfinite(state.nextSwitchAt) || input.now < state.lastTime) Reset(state);
    state.lastTime = input.now;
    const int current = input.current >= 0 && input.current < count ? input.current : -1;
    const std::uint64_t currentKey = current >= 0 ? items[current].key : 0;
    if (currentKey != state.observedCurrentKey) {
        if (currentKey != 0 && state.observedCurrentKey != 0 && state.nextSwitchAt < input.now + SwitchCooldown)
            state.nextSwitchAt = input.now + SwitchCooldown;
        state.observedCurrentKey = currentKey;
    }
    if (currentKey != 0 && currentKey == state.failedKey) { state.failedKey = 0; state.failedUntil = 0.0f; }
    if (state.pendingKey != 0) {
        const int pending = FindKey(items, count, state.pendingKey);
        bool readyAlternative = false;
        if (input.enemyVisible && pending >= 0 && !CanFire(items[pending]))
            for (int index = 0; index < count; ++index)
                if (index != pending && Allowed(items[index], input) && CanFire(items[index]) &&
                    !Blocked(items[index], currentKey, state, input.now)) readyAlternative = true;
        if (currentKey == state.pendingKey) {
            state.pendingKey = 0; state.pendingUntil = 0.0f;
        } else if (pending < 0 || !Allowed(items[pending], input) ||
                   !(CanFire(items[pending]) || CanReload(items[pending]) || items[pending].weapon == Weapon::Knife) ||
                   readyAlternative) {
            // A newly visible enemy must not leave a loaded current pistol
            // waiting for an out-of-combat empty-primary/knife request.
            state.pendingKey = 0; state.pendingUntil = 0.0f;
        } else if (input.now >= state.pendingUntil) {
            state.failedKey = state.pendingKey; state.failedUntil = input.now + FailedRetrySeconds;
            state.pendingKey = 0; state.pendingUntil = 0.0f;
        } else {
            Selection out; out.index = pending; out.waiting = true; out.reason = Reason::Pending; return out;
        }
    }
    int ready = -1, reloadable = -1, available = -1, knife = -1;
    bool readyGood = false, reloadGood = false;
    for (int index = 0; index < count; ++index) {
        const Candidate &item = items[index];
        if (!Allowed(item, input) || Blocked(item, currentKey, state, input.now)) continue;
        if (item.weapon == Weapon::Knife) {
            if (knife < 0 || index == current) knife = index;
            continue;
        }
        const int rank = Preference(item.weapon);
        const bool good = !BadDistance(item, index == current, input);
        // Distance is a preference between usable firearms, never permission
        // to choose a knife while the only loaded firearm is imperfect.
        if (CanFire(item) && (ready < 0 || (good && !readyGood) ||
            (good == readyGood && rank > Preference(items[ready].weapon)))) { ready = index; readyGood = good; }
        if (CanReload(item) && (reloadable < 0 || (good && !reloadGood) ||
            (good == reloadGood && rank > Preference(items[reloadable].weapon)))) { reloadable = index; reloadGood = good; }
        if ((CanFire(item) || CanReload(item)) && (available < 0 || rank > Preference(items[available].weapon))) available = index;
    }
    int desired;
    if (!input.enemyVisible) desired = available;
    else if (ready >= 0) desired = ready;
    else if (current >= 0 && Allowed(items[current], input) && CanReload(items[current])) desired = current;
    else desired = reloadable;
    if (desired < 0) desired = knife;
    if (desired < 0) {
        Selection out; out.reason = input.now < state.failedUntil ? Reason::RetryWait : Reason::Unavailable; return out;
    }
    if (desired == current) return Held(items, desired, input,
        input.now < state.failedUntil ? Reason::RetryWait : Reason::Hold);
    // An empty gun may immediately yield to a loaded backup even inside the
    // ordinary preference cooldown. Reload-capable empty guns cannot reclaim
    // that backup during combat until their actual clip observation refills.
    const bool currentFires = current >= 0 && Allowed(items[current], input) && CanFire(items[current]);
    if (currentFires && input.now < state.nextSwitchAt) return Held(items, current, input, Reason::Cooldown);
    state.pendingKey = items[desired].key;
    state.pendingUntil = input.now + PendingSeconds;
    state.nextSwitchAt = input.now + SwitchCooldown;
    Selection out; out.index = desired; out.switchNow = true; out.waiting = true; out.reason = Reason::Switch;
    return out;
}
}
#endif
