/*
 * AgPB owned-inventory weapon selection. GPL-3.0-only.
 * Behavior baseline: SyPB 4c364fb source/combat.cpp FireWeapon,
 * SelectBestWeapon and IsWeaponBadInDistance, using the fixed g_weaponSelect order.
 * Personality orders and grenade defaults come from source/globals.cpp
 * g_weaponPrefs and source/basecode.cpp GetBestWeaponCarried/PerformWeaponPurchase.
 * Compared with CS-EBOT 2333562 source/combat.cpp and source/globals.cpp.
 * Per-bot pending acknowledgement and cooldown guard the
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
#include "local_personality.h"

class IFileSystem;

namespace AgPBWeaponSelect {
// The first 26 values are the SyPB g_weaponSelect indices used by general.cfg.
enum class Weapon {
    Knife, USP, Glock, Deagle, P228, Elite, FiveSeven, M3, XM1014,
    MP5, TMP, P90, MAC10, UMP45, AK47, SG552, M4A1, Galil, Famas,
    AUG, Scout, AWP, G3SG1, SG550, M249, Shield, Unknown, C4, Grenade
};

constexpr int WeaponCount = static_cast<int>(Weapon::Shield) + 1;
constexpr int GrenadeCount = 3;
using WeaponOrder = Weapon[WeaponCount];

namespace Detail {
struct Preferences {
    // SyPB lists weapons from lowest to highest preference. Its built-in arrays
    // repeat Galil in Normal and AUG in Rusher. Shield is a config index.
    WeaponOrder orders[3] = {
        { Weapon::Knife, Weapon::Glock, Weapon::USP, Weapon::P228, Weapon::Elite, Weapon::FiveSeven, Weapon::Deagle,
          Weapon::MAC10, Weapon::TMP, Weapon::M249, Weapon::Shield, Weapon::UMP45, Weapon::P90, Weapon::XM1014,
          Weapon::M3, Weapon::G3SG1, Weapon::SG550, Weapon::Famas, Weapon::AWP, Weapon::Galil, Weapon::AUG,
          Weapon::SG552, Weapon::Galil, Weapon::MP5, Weapon::AK47, Weapon::M4A1 },
        { Weapon::Knife, Weapon::Glock, Weapon::USP, Weapon::P228, Weapon::Elite, Weapon::FiveSeven, Weapon::Deagle,
          Weapon::M249, Weapon::AUG, Weapon::G3SG1, Weapon::SG550, Weapon::Scout, Weapon::AWP, Weapon::TMP,
          Weapon::MAC10, Weapon::UMP45, Weapon::M3, Weapon::XM1014, Weapon::P90, Weapon::MP5, Weapon::Famas,
          Weapon::Galil, Weapon::AUG, Weapon::SG552, Weapon::M4A1, Weapon::AK47 },
        { Weapon::Knife, Weapon::Glock, Weapon::USP, Weapon::P228, Weapon::Elite, Weapon::FiveSeven, Weapon::Deagle,
          Weapon::M3, Weapon::XM1014, Weapon::MAC10, Weapon::TMP, Weapon::UMP45, Weapon::P90, Weapon::MP5,
          Weapon::M249, Weapon::Famas, Weapon::AK47, Weapon::Galil, Weapon::M4A1, Weapon::SG552, Weapon::AUG,
          Weapon::Scout, Weapon::AWP, Weapon::G3SG1, Weapon::SG550, Weapon::Shield }
    };
    int grenadeChance[GrenadeCount] = { 95, 85, 40 };
    int grenadeMoney[GrenadeCount] = { 300, 200, 300 };
};

inline Preferences &PreferenceStorage() {
    static Preferences preferences;
    return preferences;
}
}

bool LoadPreferences(IFileSystem *fileSystem, int *invalidEntries = nullptr);

inline const WeaponOrder &PersonalityOrder(AgPBLocalPersonality personality) {
    const Detail::Preferences &preferences = Detail::PreferenceStorage();
    if (personality == AgPBLocalPersonality::Rusher) return preferences.orders[1];
    if (personality == AgPBLocalPersonality::Careful) return preferences.orders[2];
    return preferences.orders[0];
}

// Config order: HE, flashbang, smoke.
inline int GrenadeChance(int index) { return Detail::PreferenceStorage().grenadeChance[index]; }
inline int GrenadeMoney(int index) { return Detail::PreferenceStorage().grenadeMoney[index]; }

constexpr float SwitchCooldown = 0.5f;
constexpr float PendingSeconds = 1.5f;
constexpr float FailedRetrySeconds = 1.0f;

struct Candidate {
    Weapon weapon = Weapon::Unknown;
    int clip = -1, reserve = -1;
    // Stable full identity, e.g. the complete EHANDLE, never a transient slot
    // or an index in this array. Zero is invalid; the caller verifies owner.
    std::uint64_t key = 0;
    int maxClip = -1;
};
struct Input {
    float now = 0.0f;
    int current = -1;
    bool enemyVisible = false;
    float distance = 0.0f;
    bool underwater = false;
    // An explicit local task can request its owned knife or reload target.
    std::uint64_t forcedKey = 0;
};
struct State {
    std::uint64_t pendingKey = 0, failedKey = 0, observedCurrentKey = 0;
    float pendingUntil = 0.0f, failedUntil = 0.0f, nextSwitchAt = 0.0f, lastTime = -1.0f;
    bool pendingForced = false;
};
enum class Reason { Unavailable, Invalid, Hold, Switch, Pending, Cooldown, RetryWait };
struct Selection {
    int index = -1;
    bool switchNow = false;
    bool waiting = false;
    // True only for the confirmed active firearm with spare rounds and space
    // in its magazine. No reload is requested while a switch waits.
    bool reload = false;
    Reason reason = Reason::Unavailable;
};

inline void Reset(State &state) { state = State(); }
inline bool Pistol(Weapon weapon) {
    return weapon >= Weapon::USP && weapon <= Weapon::FiveSeven;
}
inline bool Shotgun(Weapon weapon) { return weapon == Weapon::M3 || weapon == Weapon::XM1014; }
inline bool Sniper(Weapon weapon) { return weapon >= Weapon::Scout && weapon <= Weapon::SG550; }
inline bool Firearm(Weapon weapon) { return weapon >= Weapon::USP && weapon <= Weapon::M249; }
inline bool Selectable(Weapon weapon) { return weapon == Weapon::Knife || Firearm(weapon); }
inline int Preference(Weapon weapon) { return Selectable(weapon) ? static_cast<int>(weapon) : -1; }
inline bool CanFire(const Candidate &item) { return Firearm(item.weapon) && item.clip > 0; }
inline bool CanReload(const Candidate &item) {
    return Firearm(item.weapon) && item.reserve > 0 &&
        (item.clip == 0 || (item.clip > 0 && item.clip < item.maxClip));
}
inline bool Allowed(const Candidate &item, const Input &input) {
    return item.key != 0 && Selectable(item.weapon) && !(input.underwater && Shotgun(item.weapon));
}
inline bool BadDistance(Weapon weapon, float distance) {
    return (Shotgun(weapon) && distance > 750.0f) || (Sniper(weapon) && distance < 300.0f);
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
inline bool Blocked(const Candidate &item, std::uint64_t currentKey, const State &state, float now) {
    return item.key == state.failedKey && item.key != currentKey && now < state.failedUntil;
}
inline void ClearPending(State &state) {
    state.pendingKey = 0;
    state.pendingUntil = 0.0f;
    state.pendingForced = false;
}

inline void Prefer(const Candidate *items, int index, int current, int &best) {
    if (best < 0 || Preference(items[index].weapon) > Preference(items[best].weapon) ||
        (index == current && items[index].weapon == items[best].weapon)) best = index;
}

struct DesiredWeapon {
    int index;
    bool reload;
    bool forced;
};

inline DesiredWeapon ChooseWeapon(const State &state, const Candidate *items, int count, const Input &input,
                                  int current, std::uint64_t currentKey) {
    int highestLoaded = -1, inRange = -1, idle = -1, knife = -1;
    int primaryReserve = -1, pistolReserve = -1;
    bool loadedPistol = false;
    for (int index = 0; index < count; ++index) {
        const Candidate &item = items[index];
        if (!Allowed(item, input) || Blocked(item, currentKey, state, input.now)) continue;
        const bool fires = CanFire(item);
        const bool reloads = CanReload(item);
        if (item.key == input.forcedKey && (fires || reloads || item.weapon == Weapon::Knife))
            return {index, reloads, true};
        if (item.weapon == Weapon::Knife) {
            Prefer(items, index, current, knife);
            continue;
        }
        if (fires) {
            Prefer(items, index, current, highestLoaded);
            if (input.enemyVisible && !BadDistance(item.weapon, input.distance)) Prefer(items, index, current, inRange);
            if (Pistol(item.weapon)) loadedPistol = true;
        }
        if (reloads) Prefer(items, index, current, Pistol(item.weapon) ? pistolReserve : primaryReserve);
        if (!input.enemyVisible && (item.reserve > 0 || (index == current && fires)))
            Prefer(items, index, current, idle);
    }
    if (!input.enemyVisible) {
        // SelectBestWeapon checks reserve and the active clip. A proven loaded
        // holstered gun is usable if those checks yield no firearm.
        if (idle >= 0) return {idle, items[idle].clip == 0 && CanReload(items[idle]), false};
        return {highestLoaded >= 0 ? highestLoaded : knife, false, false};
    }

    // SyPB bypasses distance filtering when a secondary has a loaded clip.
    // Read the owned secondary directly to avoid using a personality rank as a weapon index.
    if (loadedPistol) return {highestLoaded, false, false};
    if (inRange >= 0) return {inRange, false, false};
    if (primaryReserve >= 0) return {primaryReserve, true, false};
    if (pistolReserve >= 0) return {pistolReserve, true, false};
    // Inventories without a knife can still use a proven loaded gun.
    return {knife >= 0 ? knife : highestLoaded, false, false};
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
    DesiredWeapon desired = ChooseWeapon(state, items, count, input, current, currentKey);
    if (state.pendingKey != 0) {
        const bool stillDesired = desired.index >= 0 && items[desired.index].key == state.pendingKey &&
            (!state.pendingForced || desired.forced);
        if (currentKey == state.pendingKey || !stillDesired) {
            ClearPending(state);
        } else if (input.now >= state.pendingUntil) {
            state.failedKey = state.pendingKey; state.failedUntil = input.now + FailedRetrySeconds;
            ClearPending(state);
            desired = ChooseWeapon(state, items, count, input, current, currentKey);
        } else {
            Selection out;
            out.index = desired.index;
            out.waiting = true;
            out.reason = Reason::Pending;
            return out;
        }
    }
    Selection out;
    out.index = desired.index;
    const bool retryWait = input.now < state.failedUntil;
    if (desired.index < 0) {
        out.reason = retryWait ? Reason::RetryWait : Reason::Unavailable;
        return out;
    }
    if (desired.index == current) {
        out.reason = retryWait ? Reason::RetryWait : Reason::Hold;
        out.reload = desired.reload;
        return out;
    }
    // An empty gun or explicit local task can switch inside the ordinary cooldown.
    const bool currentFires = current >= 0 && Allowed(items[current], input) && CanFire(items[current]);
    if (!desired.forced && currentFires && input.now < state.nextSwitchAt) {
        out.index = current;
        out.reason = Reason::Cooldown;
        return out;
    }
    state.pendingKey = items[desired.index].key;
    state.pendingUntil = input.now + PendingSeconds;
    state.pendingForced = desired.forced;
    state.nextSwitchAt = input.now + SwitchCooldown;
    out.switchNow = true;
    out.waiting = true;
    out.reason = Reason::Switch;
    return out;
}
}
#endif
