// Owned-inventory selection regression, independent of Source SDK/runtime.
#include "../src/weapon_selection.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static AgPBWeaponSelect::Candidate Item(AgPBWeaponSelect::Weapon weapon, int clip, int reserve, std::uint64_t key) {
    AgPBWeaponSelect::Candidate out; out.weapon = weapon; out.clip = clip; out.reserve = reserve; out.key = key; return out;
}
int main() {
    using namespace AgPBWeaponSelect;
    Input input; State state;
    input.now = 10.0f; input.current = 0;
    Candidate table[] = {Item(Weapon::Knife, -1, -1, 1), Item(Weapon::AK47, 30, 90, 2),
        Item(Weapon::M4A1, 30, 90, 3), Item(Weapon::Galil, 30, 90, 4), Item(Weapon::Famas, 25, 90, 5)};
    Selection selected = Step(state, table, 5, input);
    Require(selected.index == 4 && selected.switchNow,
        "highest available SyPB table entry wins, including its non-price-based Galil/Famas order");
    Require(Preference(Weapon::USP) < Preference(Weapon::Glock) && Preference(Weapon::Deagle) < Preference(Weapon::P228) &&
        Preference(Weapon::M249) > Preference(Weapon::SG550), "specific weapon preferences reproduce the local baseline table");

    Candidate shotgun[] = {Item(Weapon::Knife, -1, -1, 10), Item(Weapon::USP, 12, 24, 11), Item(Weapon::M3, 8, 32, 12)};
    Reset(state); input = Input(); input.now = 20; input.current = 0; input.enemyVisible = true; input.distance = 150;
    selected = Step(state, shotgun, 3, input);
    Require(selected.index == 2 && selected.switchNow, "near-range shotgun is preferred to a loaded pistol");
    Reset(state); input.distance = 900;
    Require(Step(state, shotgun, 3, input).index == 1, "distant target selects the loaded pistol instead of the shotgun");
    Reset(state); input.current = 2; input.distance = 760;
    selected = Step(state, shotgun, 3, input);
    Require(selected.index == 2 && !selected.switchNow, "current shotgun retains its EBot-derived 768-unit hold interval");
    Reset(state); input.current = 1;
    Require(Step(state, shotgun, 3, input).index == 1, "holstered shotgun does not acquire beyond SyPB's 750-unit boundary");
    input.distance = 750;
    Reset(state); Require(Step(state, shotgun, 3, input).index == 2, "750-unit acquisition boundary remains inclusive");
    input.distance = 768.1f; input.current = 2;
    Reset(state); Require(Step(state, shotgun, 3, input).index == 1, "shotgun beyond the retention boundary yields to a ready pistol");
    shotgun[1].clip = 0; shotgun[1].reserve = 0;
    Reset(state); input.distance = 1200;
    Require(Step(state, shotgun, 3, input).index == 2, "only loaded shotgun remains preferable to knife at long distance");
    shotgun[1].clip = 12;
    Reset(state); input.underwater = true;
    Require(Step(state, shotgun, 3, input).index == 1, "underwater shotgun exclusion follows both reference implementations");

    Candidate sniper[] = {Item(Weapon::Knife, -1, -1, 20), Item(Weapon::USP, 12, 24, 21), Item(Weapon::AWP, 10, 30, 22)};
    Reset(state); input = Input(); input.now = 30; input.current = 2; input.enemyVisible = true; input.distance = 200;
    Require(Step(state, sniper, 3, input).index == 1, "loaded secondary is preferred to a normal-mode sniper below 300 units");
    Reset(state); input.distance = 300;
    Require(Step(state, sniper, 3, input).index == 2, "sniper is preferred at the inclusive 300-unit baseline boundary");
    Reset(state); input.distance = 40; sniper[1].clip = 0; sniper[1].reserve = 0;
    Require(Step(state, sniper, 3, input).index == 2, "close target does not force knife while a loaded firearm is still available");

    Candidate backup[] = {Item(Weapon::Knife, -1, -1, 30), Item(Weapon::M4A1, 0, 90, 31), Item(Weapon::USP, 12, 24, 32)};
    Reset(state); input = Input(); input.now = 40; input.current = 1; input.enemyVisible = true; input.distance = 500;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && selected.switchNow && !selected.reload,
        "empty primary immediately selects a fireable secondary instead of starting a combat reload");
    input.now += 0.05f;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && selected.waiting && !selected.switchNow && !selected.reload,
        "unconfirmed selection does not repeat a weaponselect command or reload the old gun");
    input.current = 2; input.now += 0.05f;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && !selected.switchNow && !selected.waiting, "actual active identity acknowledges the backup switch");
    input.now += 5;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && !selected.switchNow, "empty primary does not reclaim the pistol after the cooldown expires");
    backup[2].clip = 0; backup[2].reserve = 0; input.current = 1;
    Reset(state); selected = Step(state, backup, 3, input);
    Require(selected.index == 1 && selected.reload && !selected.switchNow,
        "without a ready backup the known-empty current primary may reload its known reserve");
    backup[2].reserve = 24; input.current = 2;
    Reset(state); selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && selected.reload, "no loaded gun preserves the current reloadable pistol instead of thrashing empty guns");
    input.enemyVisible = false;
    Reset(state); selected = Step(state, backup, 3, input);
    Require(selected.index == 1 && selected.switchNow && !selected.reload,
        "outside combat SelectBestWeapon can restore a higher-preference primary with known reserve");
    input.current = 1; input.now += 0.1f;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 1 && selected.reload, "out-of-combat primary reload is requested only after current-weapon acknowledgement");

    Reset(state); input.current = 2; input.enemyVisible = false; backup[2].clip = 12;
    Require(Step(state, backup, 3, input).index == 1, "an idle loaded pistol may request restoration of its reloadable primary");
    input.now += 0.01f; input.enemyVisible = true;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && !selected.switchNow && !selected.waiting && state.pendingKey == 0,
        "new enemy cancels unacknowledged empty-primary preference instead of withholding a ready current pistol");

    backup[1].clip = 1; backup[1].reserve = 0; backup[2].clip = 12; input.enemyVisible = true; input.current = 1;
    Reset(state); selected = Step(state, backup, 3, input);
    Require(selected.index == 1 && !selected.reload, "one known bullet is usable but is not treated as an infinite clip");
    backup[1].clip = 0; input.now += 0.01f;
    selected = Step(state, backup, 3, input);
    Require(selected.index == 2 && selected.switchNow, "consuming the last bullet changes the decision to a loaded backup");
    backup[1].clip = -1; backup[1].reserve = 90;
    Reset(state); Require(Step(state, backup, 3, input).index == 2, "unknown primary clip is not treated as loaded or automatically reloadable");
    backup[1].clip = 0; backup[1].reserve = -1; backup[2].clip = 0; backup[2].reserve = -1;
    Reset(state); Require(Step(state, backup, 3, input).index == 0, "unknown reserve cannot make empty firearms usable");

    Candidate ignored[] = {Item(Weapon::Knife, -1, -1, 40), Item(Weapon::C4, 1, 100, 41),
        Item(Weapon::Grenade, 1, 100, 42), Item(Weapon::Unknown, 200, 200, 43)};
    Reset(state); input = Input(); input.current = 0; input.now = 60;
    selected = Step(state, ignored, 4, input);
    Require(selected.index == 0 && !selected.switchNow, "C4, grenades and unknown classes never enter normal inventory preference");
    ignored[0].key = 0;
    Reset(state); Require(Step(state, ignored, 4, input).index == -1, "invalid knife identity cannot become a fabricated fallback");

    Candidate pending[] = {Item(Weapon::Knife, -1, -1, 50), Item(Weapon::M4A1, 30, 90, 51)};
    Reset(state); input = Input(); input.now = 70; input.current = 0;
    Require(Step(state, pending, 2, input).switchNow, "a new preferred inventory candidate creates one pending request");
    Candidate reordered[] = {pending[1], pending[0]}; input.current = 1; input.now = 70.1f;
    selected = Step(state, reordered, 2, input);
    Require(selected.index == 0 && selected.waiting && !selected.switchNow, "pending full identity survives inventory array reordering");
    input.current = 0; input.now = 70.2f;
    selected = Step(state, reordered, 2, input);
    Require(selected.index == 0 && !selected.waiting, "reordered actual active identity acknowledges the correct item");

    Reset(state); input.current = 0; input.now = 80;
    Require(Step(state, pending, 2, input).switchNow, "pending timeout case begins with an actual switch request");
    input.now = 81.6f;
    selected = Step(state, pending, 2, input);
    Require(selected.index == 0 && !selected.switchNow && state.failedKey == 51,
        "unacknowledged switch times out without reissuing every tick");
    input.now = 82;
    Require(!Step(state, pending, 2, input).switchNow, "failed target stays throttled during its retry interval");
    input.now = 82.7f;
    Require(Step(state, pending, 2, input).switchNow, "failed target gets a bounded later retry");
    input.now = 82.8f;
    Require(Step(state, pending, 1, input).index == 0 && state.pendingKey == 0,
        "removed inventory entity cannot retain a stale pending destination");

    Candidate cooldown[] = {Item(Weapon::USP, 12, 24, 60), Item(Weapon::M4A1, 30, 90, 61),
        Item(Weapon::SG550, 30, 90, 62)};
    Reset(state); input = Input(); input.now = 90; input.current = 0; input.enemyVisible = true; input.distance = 100;
    Require(Step(state, cooldown, 3, input).index == 1, "near target first selects the ready rifle rather than sniper");
    input.current = 1; input.now = 90.1f; input.distance = 1000;
    selected = Step(state, cooldown, 3, input);
    Require(selected.index == 1 && !selected.switchNow && selected.reason == Reason::Cooldown,
        "target range change cannot replace a just-acknowledged usable weapon every tick");
    input.now = 90.2f; cooldown[1].clip = 0; cooldown[2].clip = 0; cooldown[2].reserve = 0;
    selected = Step(state, cooldown, 3, input);
    Require(selected.index == 0 && selected.switchNow, "loaded emergency backup bypasses preference cooldown for an empty current gun");

    input.now = std::numeric_limits<float>::quiet_NaN();
    selected = Step(state, cooldown, 3, input);
    Require(selected.index == -1 && selected.reason == Reason::Invalid && state.pendingKey == 0,
        "nonfinite game time releases pending state without selecting a weapon");
    input.now = 100; input.distance = std::numeric_limits<float>::infinity();
    Require(Step(state, cooldown, 3, input).reason == Reason::Invalid, "visible enemy requires a finite nonnegative distance");
    input.enemyVisible = false; input.now = 100; input.current = 0; cooldown[1].clip = 30;
    Step(state, cooldown, 3, input);
    input.now = 5;
    selected = Step(state, cooldown, 3, input);
    Require(selected.switchNow && state.pendingUntil == 5 + PendingSeconds, "time reversal resets stale pending deadlines");
    Reset(state);
    Require(state.pendingKey == 0 && state.failedKey == 0 && state.observedCurrentKey == 0,
        "explicit lifecycle reset releases selection and retry state");
    Require(Step(state, nullptr, 1, input).reason == Reason::Invalid, "invalid inventory pointer cannot be dereferenced");
    std::puts("weapon selection policy tests passed");
}
