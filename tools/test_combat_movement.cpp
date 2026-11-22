// Regression of SyPB normal combat movement, independent of Source SDK/runtime.
#include "../src/combat_movement.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Stopped(const AgPBCombatMove::Output &out) { return out.forward == 0.0f && out.side == 0.0f; }

int main() {
    using namespace AgPBCombatMove;
    Input in; State state; Random random;
    in.weapon = Weapon::Rifle; in.now = 10.0f; in.distance = 600.0f;
    in.aggression = 0.9f; in.attacking = true;
    Output out = Step(state, in, random);
    Require(Stopped(out) && out.stopForAccuracy && !out.duck,
        "current rifle attack command stops despite aggressive approach desire");
    in.now += 0.1f;
    out = Step(state, in, random);
    Require(Stopped(out) && out.stopForAccuracy,
        "consecutive actual rifle attack commands retain the firing stop");
    in.attacking = false; in.now += 0.1f;
    out = Step(state, in, random);
    Require(out.forward == in.walkSpeed && !out.stopForAccuracy,
        "aggressive rifle advances at final walk speed on a command without IN_ATTACK");
    in.attacking = true; in.now += 0.1f;
    Require(Stopped(Step(state, in, random)), "next actual rifle attack command stops again");
    in.attacking = false;
    in.aggression = 0.3f;
    Require(Stopped(Step(state, in, random)), "cautious healthy rifle does not share aggressive approach");

    Reset(state); in = Input(); in.weapon = Weapon::Sniper;
    in.now = 20.0f; in.distance = 1500.0f; in.aggression = 1.0f; in.attacking = true;
    out = Step(state, in, random);
    Require(Stopped(out) && out.stopForAccuracy && state.style == FightStyle::Stay,
        "sniper stops to fire rather than adopting rifle or SMG approach");
    in.attacking = false; in.distance = 60.0f;
    Require(Stopped(Step(state, in, random)), "normal sniper is exempt from generic close-range retreat");

    Reset(state); in = Input(); in.weapon = Weapon::Rifle; in.now = 30.0f;
    in.distance = 80.0f; in.attacking = true;
    out = Step(state, in, random);
    Require(out.forward == -in.maxSpeed && out.side == 0.0f && out.duck && !out.stopForAccuracy,
        "nearby rifle retreats crouched but retains accurate-fire strafe suppression");
    in.distance = 96.0f;
    Require(Stopped(Step(state, in, random)), "96-unit boundary is outside close retreat");
    in.health = 15.0f; in.aggression = 1.0f; in.distance = 350.0f; in.attacking = false;
    in.enemyInViewCone = true;
    out = Step(state, in, random);
    Require(out.forward == -in.walkSpeed, "low-health exposed rifle backs away at final walk speed instead of charging");
    in.enemyInViewCone = false;
    Require(Stopped(Step(state, in, random)), "low health does not trigger this retreat when the enemy is outside the bot view cone");
    in.enemyInViewCone = true; in.bombPlanted = true;
    Require(Step(state, in, random).forward == 0.0f, "planted-bomb context suppresses generic fear retreat");

    Reset(state); in = Input(); in.weapon = Weapon::Rifle; in.now = 40.0f;
    in.distance = 400.0f; in.reloading = true; in.enemyInViewCone = true;
    out = Step(state, in, random);
    Require(out.forward == -in.maxSpeed && out.side != 0.0f && !out.duck && !out.stopForAccuracy &&
        out.motion == Motion::ReloadRetreat, "exposed reload retreats at full speed with lateral evasion");
    in.crouchRequired = true;
    Require(Step(state, in, random).duck, "reload retains required authored crouch");
    in.crouchRequired = false; in.bombPlanted = true;
    out = Step(state, in, random);
    Require(out.forward == -in.maxSpeed && out.side == 0.0f,
        "final reload retreat remains active even when planted-bomb context suppresses fear strafe");

    Reset(state); in = Input(); in.weapon = Weapon::Knife; in.now = 50.0f;
    in.distance = 32.0f; in.attacking = true;
    out = Step(state, in, random);
    Require(out.forward == in.maxSpeed && out.side == 0.0f && !out.stopForAccuracy,
        "visible acquired knife target gets closing movement rather than a firearm stop");

    Reset(state); in = Input(); in.weapon = Weapon::SMG; in.now = 60.0f;
    in.distance = 300.0f; in.aggression = 0.4f; in.attacking = true; in.mobilePrimary = true;
    out = Step(state, in, random);
    Require(out.forward == in.maxSpeed && !out.stopForAccuracy,
        "SyPB mobile SMG keeps its health/aggression advance while firing");
    in.mobilePrimary = false;
    out = Step(state, in, random);
    Require(Stopped(out) && out.stopForAccuracy, "MP5-style SMG does not inherit UsesBadPrimary mobile firing");
    in.mobilePrimary = true; in.distance = 70.0f;
    out = Step(state, in, random);
    Require(out.forward == -in.walkSpeed && !out.duck, "close mobile SMG retreats at walk speed without forced crouch");
    in.weapon = Weapon::Shotgun; in.distance = 250.0f; in.aggression = 0.9f; in.mobilePrimary = false;
    Require(Step(state, in, random).forward == in.walkSpeed, "SyPB shotgun closes while firing with final walk conversion");
    in.weapon = Weapon::Pistol;
    Require(Stopped(Step(state, in, random)), "pistol aggression is capped below approach threshold");
    in.weapon = Weapon::MachineGun;
    out = Step(state, in, random);
    Require(Stopped(out) && out.stopForAccuracy, "machine gun keeps accurate primary fire policy");

    Reset(state); in = Input(); in.weapon = Weapon::SMG; in.mobilePrimary = true;
    in.now = 70.0f; in.distance = 70.0f; in.enemySide = -1.0f;
    random.strafeFlip = 0.9f; random.duration = 0.5f;
    out = Step(state, in, random);
    Require(out.side == in.walkSpeed && state.strafeUntil == 71.5f,
        "enemy orientation chooses rightward strafe with a finite direction lease");
    in.now = 70.5f; in.enemySide = 1.0f; random.strafeFlip = 0.0f;
    out = Step(state, in, random);
    Require(out.side == in.walkSpeed && state.strafeUntil == 71.5f,
        "new orientation and random samples do not flip direction before its lease expires");
    in.now = 71.5f; random.strafeFlip = 0.9f;
    Require(Step(state, in, random).side == in.walkSpeed,
        "direction lease remains active exactly at the SyPB strict expiry boundary");
    in.now = 71.5001f;
    Require(Step(state, in, random).side == -in.walkSpeed,
        "expired direction lease may choose a new direction from actual enemy orientation");
    in.skill = 69;
    Require(Step(state, in, random).side == 0.0f, "skill below 70 does not get normal strafe style evasion");
    in.reloading = true; in.enemyInViewCone = true;
    Require(Step(state, in, random).side != 0.0f, "reload allows evasion independently of skill style threshold");

    Reset(state); in = Input(); random = Random();
    in.weapon = Weapon::Rifle; in.now = 75.0f; in.distance = 80.0f;
    out = Step(state, in, random);
    Require(out.duck && out.forward == -in.maxSpeed && std::fabs(out.side) == in.maxSpeed,
        "new close-range duck affects both final retreat and GetWalkSpeed strafe in the same command");
    in.attacking = true; in.now += 0.01f;
    out = Step(state, in, random);
    Require(out.duck && out.forward == -in.maxSpeed && out.side == 0.0f,
        "actual close rifle shot suppresses side evasion while retaining crouched retreat");
    in.distance = 300.0f; in.health = 15.0f; in.aggression = 1.0f;
    in.attacking = false; in.enemyInViewCone = true; in.crouchRequired = true;
    out = Step(state, in, random);
    Require(out.duck && out.forward == -in.maxSpeed,
        "authored crouch also retains SyPB GetWalkSpeed maximum-speed exception");

    Reset(state); in = Input(); random = Random();
    in.weapon = Weapon::SMG; in.mobilePrimary = true; in.now = 76.0f;
    in.distance = 300.0f; in.aggression = 0.4f; in.attacking = true;
    out = Step(state, in, random);
    Require(state.style == FightStyle::Strafe && out.forward == in.maxSpeed && out.side == 0.0f,
        "healthy mobile SMG strafe style alone cannot synthesize sideways combat movement");
    const float postponedStrafe = state.strafeUntil;
    in.now += 0.1f; random.duration = 1.0f; in.enemySide = -1.0f;
    out = Step(state, in, random);
    Require(out.side == 0.0f && state.strafeUntil > postponedStrafe,
        "without setStrafe the source postpones direction selection instead of beginning healthy SMG strafe");
    in.mobilePrimary = false; in.attacking = false;
    Require(Step(state, in, random).forward == in.maxSpeed,
        "MP5 retains SMG aggression bonus and full approach on an inter-shot command");
    in.attacking = true;
    Require(Stopped(Step(state, in, random)), "MP5 actual attack remains distinct from mobilePrimary fire");
    in.mobilePrimary = true; in.health = 15.0f; in.aggression = 0.1f; in.enemyInViewCone = true;
    out = Step(state, in, random);
    Require(Stopped(out), "SMG approach bonus does not invent a low-health lateral retreat below close range");

    Reset(state); in = Input(); random = Random();
    in.weapon = Weapon::Rifle; in.now = 77.0f; in.distance = 600.0f; in.aggression = 0.3f;
    random.style = 0.0f;
    Step(state, in, random);
    Require(state.style == FightStyle::Strafe && state.nextStyleAt == 80.0f,
        "fight style keeps the source three-second reevaluation lease");
    in.now = 80.0f; random.style = 0.99f;
    Step(state, in, random);
    Require(state.style == FightStyle::Strafe && state.nextStyleAt == 80.0f,
        "fight style does not reroll exactly at its strict source deadline");
    in.now = 80.0001f;
    Step(state, in, random);
    Require(state.style == FightStyle::Stay && state.nextStyleAt > 83.0f,
        "fight style may reroll only after the source deadline");

    Reset(state); in = Input(); random = Random();
    in.weapon = Weapon::Rifle; in.now = 85.0f; in.distance = 799.0f;
    in.health = 15.0f; in.aggression = 1.0f; in.enemyInViewCone = true; in.skill = 69;
    random.style = 0.0f;
    Require(Step(state, in, random).side == 0.0f, "low skill blocks normal fear strafe even inside 800 units");
    in.skill = 70; in.now += 0.01f;
    Require(std::fabs(Step(state, in, random).side) == in.walkSpeed,
        "skill 70 permits source fear strafe at 799 units");
    in.distance = 800.0f; in.now += 0.01f;
    Require(Step(state, in, random).side == 0.0f, "800-unit boundary excludes normal style strafe");
    in.skill = 1; in.reloading = true;
    out = Step(state, in, random);
    Require(out.forward == -in.maxSpeed && std::fabs(out.side) == in.walkSpeed && !out.duck,
        "reload independently restores full retreat and fear strafe beyond skill and distance gates");

    Reset(state); in = Input(); in.weapon = Weapon::Sniper; in.now = 80.0f;
    in.distance = 1200.0f; in.attacking = true; in.canDuck = true;
    random.duck = 0.0f;
    out = Step(state, in, random);
    Require(out.duck && Stopped(out) && state.duckUntil == 80.5f,
        "verified crouched target view permits SyPB timed stationary crouch");
    in.now = 80.2f; random.duck = 0.9f;
    Require(Step(state, in, random).duck, "timed crouch persists between chance checks");
    in.now = 80.5f;
    Require(!Step(state, in, random).duck, "optional crouch ends at the original deadline");
    in.canDuck = false; random.duck = 0.0f;
    Require(!Step(state, in, random).duck, "unverified crouch cannot be selected by chance");
    in.crouchRequired = true;
    Require(Step(state, in, random).duck, "authored crouch remains distinct from optional combat crouch");

    in.weapon = Weapon::Rifle; in.now = 81.0f; in.crouchRequired = false;
    out = Step(state, in, random);
    Require(state.weapon == Weapon::Rifle && state.duckUntil == 0.0f,
        "weapon change resets previous fight posture and timing state");
    state.duckUntil = 100.0f; in.now = 79.0f;
    Require(!Step(state, in, random).duck && state.duckUntil == 0.0f,
        "reversed game time cannot preserve stale encounter crouch");
    in.health = std::numeric_limits<float>::quiet_NaN();
    out = Step(state, in, random);
    Require(Stopped(out) && out.motion == Motion::Invalid && !state.initialized,
        "invalid data safely releases intent and resets policy state");
    Reset(state);
    Require(!state.initialized && state.strafeUntil == 0.0f && state.duckUntil == 0.0f,
        "explicit encounter reset releases both movement timers");

    // Frame dispatch differs from an isolated post-fire CombatFight call.
    // Both M4 and AK map to Rifle; keep each reflected speed parameter intact.
    const float rifleMaximum[] = {230.0f, 221.0f};
    const float rifleWalk[] = {101.0f, 96.5f};
    for (int rifle = 0; rifle < 2; ++rifle) {
        in = Input(); in.weapon = Weapon::Rifle; in.now = 100.0f; in.distance = 400.0f;
        in.health = 100.0f; in.aggression = 0.55f; in.attacking = true;
        in.maxSpeed = rifleMaximum[rifle]; in.walkSpeed = rifleWalk[rifle];
        in.skill = 59; Reset(state);
        out = StepFrame(state, in);
        Require(out.forward == in.walkSpeed && out.side == 0.0f && !out.stopForAccuracy,
            "skill59 M4/AK Fight retains pre-fire approach while the real command attacks");
        in.skill = 60; Reset(state);
        out = StepFrame(state, in);
        Require(Stopped(out) && out.stopForAccuracy,
            "skill60 M4/AK late override sees actual attack and replaces the early approach");
        in.attacking = false; in.now += 0.1f;
        Require(StepFrame(state, in).forward == in.walkSpeed,
            "skill60 rifle late override can still approach on an actual non-attack frame");
    }

    in = Input(); in.weapon = Weapon::Rifle; in.now = 110.0f; in.distance = 800.0f;
    in.attacking = true; in.skill = 20; in.aggression = 0.3f; Reset(state);
    Require(Stopped(StepFrame(state, in)), "low-skill dispatch does not force cautious rifles to pursue");
    in.health = 15.0f; in.aggression = 1.0f; in.enemyInViewCone = true;
    out = StepFrame(state, in);
    Require(out.forward == -in.walkSpeed && out.side == 0.0f,
        "low-skill early Fight retains genuine low-health retreat while the later gun fires");
    in.bombPlanted = true;
    Require(Stopped(StepFrame(state, in)), "low-skill frame still respects planted-bomb fear suppression");

    for (int level = 59; level <= 60; ++level) {
        in = Input(); in.weapon = Weapon::SMG; in.now = 120.0f; in.distance = 300.0f;
        in.skill = level; in.aggression = 0.4f; in.attacking = true; in.mobilePrimary = true; Reset(state);
        out = StepFrame(state, in);
        Require(out.forward == in.maxSpeed && out.side == 0.0f && !out.stopForAccuracy,
            "mobile SMG approach survives both early and late phases without invented healthy strafe");
        in.mobilePrimary = false; Reset(state);
        out = StepFrame(state, in);
        Require(level < 60 ? out.forward == in.maxSpeed : Stopped(out),
            "MP5 shares the low-skill pre-fire approach but its high-skill attack override differs from mobile SMGs");
        in.weapon = Weapon::Shotgun; in.aggression = 0.9f; Reset(state);
        out = StepFrame(state, in);
        Require(out.forward == in.walkSpeed && !out.stopForAccuracy,
            "M3 mobile-primary predicate retains final walk approach in both frame phases");
        in.weapon = Weapon::Pistol; Reset(state);
        Require(Stopped(StepFrame(state, in)), "pistol approach cap also applies to the low-skill early call");
        in.weapon = Weapon::Rifle; in.distance = 80.0f; Reset(state);
        out = StepFrame(state, in);
        Require(out.forward == -in.maxSpeed && out.duck,
            "both dispatch levels retain same-command near-range duck and full crouched retreat");
    }

    in = Input(); in.weapon = Weapon::Rifle; in.now = 130.0f; in.distance = 1200.0f;
    in.skill = 60; in.aggression = 0.3f; in.attacking = true; in.canDuck = true; Reset(state);
    Random early, late; early.style = 0.0f; early.duck = 1.0f; late.duck = 0.0f;
    out = StepFrame(state, in, early, late);
    Require(out.duck && Stopped(out) && state.duckUntil == 130.5f,
        "high-skill frame passes independent early/late chance samples in the source order");
    early.duck = late.duck = 1.0f; in.now = 130.2f;
    Require(StepFrame(state, in, early, late).duck, "two same-time calls preserve an existing timed duck lease");
    in.now = 130.5f;
    Require(!StepFrame(state, in, early, late).duck, "frame duck expires exactly at the original half-second boundary");
    state.duckUntil = 200.0f; in.reloading = true; in.enemyInViewCone = true;
    out = StepFrame(state, in, early, late);
    Require(out.forward == -in.maxSpeed && !out.duck && state.duckUntil == 0.0f,
        "final reload override releases optional timed duck in both phases");
    in.crouchRequired = true;
    Require(StepFrame(state, in, early, late).duck, "frame reload still retains required authored crouch");

    in = Input(); in.weapon = Weapon::SMG; in.now = 140.0f; in.distance = 70.0f;
    in.skill = 59; in.reloading = true; in.enemyInViewCone = true; in.enemySide = -1.0f;
    early.strafeFlip = 1.0f; early.duration = 0.5f; Reset(state);
    out = StepFrame(state, in, early, late);
    Require(out.side == in.walkSpeed && state.strafeUntil == 141.5f,
        "low-skill reload can establish the source strafe lease through its early call");
    in.now = 141.5f; in.enemySide = 1.0f;
    Require(StepFrame(state, in, early, late).side == in.walkSpeed,
        "frame wrapper preserves strict strafe expiry exactly at its deadline");
    in.now = 141.5001f;
    Require(StepFrame(state, in, early, late).side == -in.walkSpeed,
        "frame wrapper allows source direction reselection only after strict expiry");
    in.now = 139.0f;
    Require(StepFrame(state, in, early, late).side == -in.walkSpeed && state.lastTime == in.now,
        "reversed frame time resets stale leases before selecting a fresh plan");
    in.health = std::numeric_limits<float>::quiet_NaN(); in.skill = 100;
    out = StepFrame(state, in, early, late);
    Require(out.motion == Motion::Invalid && Stopped(out) && !state.initialized,
        "invalid early frame input resets state and never reaches a late valid-looking override");

    const Weapon reloadKinds[] = {Weapon::Rifle, Weapon::Shotgun, Weapon::Pistol, Weapon::SMG, Weapon::Sniper};
    for (int level = 59; level <= 60; ++level) {
        for (Weapon kind : reloadKinds) {
            in = Input(); in.weapon = kind; in.now = 150.0f; in.distance = 80.0f;
            in.skill = level; in.reloading = true; in.attacking = true;
            in.canDuck = true; in.enemyInViewCone = true; Reset(state);
            state.duckUntil = 200.0f;
            // Initialize once before installing a prior optional duck lease.
            StepFrame(state, in, early, late);
            state.duckUntil = 200.0f;
            out = StepFrame(state, in, early, late);
            const bool forcedNearDuck = kind != Weapon::SMG && kind != Weapon::Sniper;
            Require(out.forward == -in.maxSpeed && out.duck == forcedNearDuck && state.duckUntil == 0.0f,
                "skill59/60 reload clears optional timer but retains only this command's non-SMG/non-sniper near duck");
            in.distance = 96.0f;
            out = StepFrame(state, in, early, late);
            Require(!out.duck && out.forward == -in.maxSpeed,
                "reload at the exact96 boundary does not invent a near duck for any weapon");
            in.crouchRequired = true;
            Require(StepFrame(state, in, early, late).duck,
                "all skill59/60 reload weapon classes retain required crouch even outside near range");
        }
    }

    const struct {
        Weapon weapon;
        float range, walkSpeed;
    } proactiveWeapons[] = {
        {Weapon::Rifle, 768.0f, 96.5f},
        {Weapon::Pistol, 544.0f, 109.0f},
    };
    for (const auto &test : proactiveWeapons) {
        in = Input(); in.weapon = test.weapon; in.now = 160.0f;
        in.distance = std::nextafter(test.range, 0.0f); in.walkSpeed = test.walkSpeed;
        in.skill = 1; in.aggression = 0.9f; in.attacking = true;
        in.enemyInViewCone = true; in.enemySide = -1.0f;
        early = Random(); early.style = 1.0f; early.strafeFlip = 1.0f; early.duration = 0.0f;
        late = Random(); late.strafeFlip = 0.0f; late.duration = 1.0f;
        Reset(state);
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && out.side == in.walkSpeed && !out.duck &&
            !out.stopForAccuracy && out.motion == Motion::Strafe && out.strafe == StrafePlan::Moving &&
            state.style == FightStyle::Stay && state.nextStyleAt == 163.0f && state.strafeUntil == 160.5f,
            "minimum skill rifle and pistol attack with pure walk-speed strafe on the early-only path");

        in.now = 160.125f; in.enemySide = 1.0f; early.duration = 1.0f;
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && out.side == in.walkSpeed && state.strafeUntil == 160.5f,
            "held attack and rotated enemy preserve the original direction and deadline");
        in.now = 160.1875f; in.attacking = false;
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && out.side == in.walkSpeed && state.strafeUntil == 160.5f,
            "the frame without attack shares the direction lease and remains purely lateral");
        in.now = 160.375f;
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && !out.stopForAccuracy && out.strafe == StrafePlan::Precision &&
            state.strafeSign == 1 && state.strafeUntil == 160.5f,
            "the accuracy pause stops movement between shots and preserves the direction lease");
        in.now = 160.4375f; in.attacking = true;
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && out.stopForAccuracy && out.strafe == StrafePlan::Precision &&
            state.strafeSign == 1 && state.strafeUntil == 160.5f,
            "minimum skill early-only dispatch gates the actual attack during the accuracy pause");
        in.now = 160.5f;
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && out.stopForAccuracy && state.strafeSign == 1 && state.strafeUntil == 160.5f,
            "proactive accuracy pause holds through the exact direction deadline");
        in.now = std::nextafter(in.now, 161.0f);
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && out.side == -in.walkSpeed && !out.stopForAccuracy &&
            state.strafeUntil == in.now + 2.5f && state.nextStyleAt == 163.0f,
            "the next firing frame can reverse for the enemy orientation and select a full duration without rerolling style");

        in.skill = 60; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && out.side == -in.walkSpeed && !out.stopForAccuracy &&
            out.strafe == StrafePlan::Moving, "skill60 post-fire dispatch retains the proactive strafe during attack");
        in.enemyInViewCone = false; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && out.stopForAccuracy && out.strafe == StrafePlan::View,
            "enemy outside own view cone keeps rifle and pistol precision stop");
        in.enemyInViewCone = true; in.distance = test.range; early.style = 0.0f; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && out.stopForAccuracy && out.strafe == StrafePlan::Distance &&
            state.style == FightStyle::Strafe,
            "exact weapon range excludes proactive side motion even when the style selects strafe");

        in.distance = 400.0f; in.crouchRequired = true; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(out.duck && Stopped(out) && out.stopForAccuracy && out.strafe == StrafePlan::Crouch,
            "required crouch has priority over proactive strafe");
        in.crouchRequired = false; in.distance = 95.0f; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(out.duck && out.forward == -in.maxSpeed && out.side == 0.0f,
            "close-range duck and retreat keep priority during a rifle or pistol shot");
        in.distance = 96.0f; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(!out.duck && out.forward == 0.0f && std::fabs(out.side) == in.walkSpeed &&
            out.strafe == StrafePlan::Moving,
            "rifle and pistol are eligible at the exact close-retreat boundary");
        in.distance = 400.0f; in.health = 15.0f; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(!out.duck && out.forward == -in.walkSpeed && out.side == 0.0f && out.motion == Motion::Retreat &&
            out.strafe == StrafePlan::Retreat,
            "low-health retreat stays ahead of rifle and pistol proactive strafing");
        in.health = 100.0f; in.reloading = true; in.attacking = false; Reset(state);
        out = StepFrame(state, in, early, late);
        Require(out.forward == -in.maxSpeed && std::fabs(out.side) == in.walkSpeed &&
            !out.stopForAccuracy && out.motion == Motion::ReloadRetreat && out.strafe == StrafePlan::Reload,
            "reload keeps full retreat and its own lateral evasion inside the proactive range");

        in.reloading = false; in.attacking = true; early.duration = 0.0f;
        in.now = 165.0f; in.distance = std::nextafter(256.0f, 0.0f); Reset(state);
        StepFrame(state, in, early, late);
        in.now = state.strafeUntil;
        out = StepFrame(state, in, early, late);
        Require(out.forward == 0.0f && std::fabs(out.side) == in.walkSpeed && !out.stopForAccuracy,
            "in-range rifle and pistol remain mobile through the direction deadline just inside 256 units");
        in.now = 165.0f; in.distance = 256.0f; Reset(state);
        StepFrame(state, in, early, late);
        in.now = 165.375f;
        out = StepFrame(state, in, early, late);
        Require(Stopped(out) && out.stopForAccuracy && out.strafe == StrafePlan::Precision &&
            state.strafeUntil == 165.5f,
            "the exact 256-unit boundary allows a late-lease accuracy pause");
    }

    in = Input(); in.weapon = Weapon::Sniper; in.now = 170.0f; in.distance = 400.0f;
    in.skill = 100; in.attacking = true; in.enemyInViewCone = true; Reset(state);
    out = StepFrame(state, in);
    Require(Stopped(out) && out.stopForAccuracy, "in-cone sniper stays with precision stop at maximum skill");
    in.weapon = Weapon::SMG; in.mobilePrimary = true; in.aggression = 0.4f; Reset(state);
    out = StepFrame(state, in);
    Require(out.forward == in.maxSpeed && out.side == 0.0f && !out.stopForAccuracy,
        "in-cone mobile SMG keeps its existing firing approach");
    in.mobilePrimary = false; Reset(state);
    out = StepFrame(state, in);
    Require(Stopped(out) && out.stopForAccuracy, "in-cone MP5 retains its existing firing stop");

    Output diagonal;
    diagonal.forward = -120.0f;
    diagonal.side = 100.0f;
    Axes alternate = AlternateStep(diagonal, [](float forward, float side) {
        return (forward == 0.0f && side == -100.0f) || (forward == -120.0f && side == 0.0f);
    });
    Require(alternate.forward == 0.0f && alternate.side == -100.0f,
        "a blocked diagonal can keep a safe mirrored sidestep before retreating straight");
    alternate = AlternateStep(diagonal, [](float forward, float side) {
        return forward == -120.0f && side == 0.0f;
    });
    Require(alternate.forward == -120.0f && alternate.side == 0.0f,
        "narrow passages retain a safe straight retreat when neither side fits");
    alternate = AlternateStep(diagonal, [](float forward, float) { return forward > 0.0f; });
    Require(alternate.forward == 0.0f && alternate.side == 0.0f,
        "unsafe retreat candidates cannot turn into a charge");
    const float pressuredRifle = CombatApproach(Weapon::Rifle, 50.0f, 0.35f, false);
    Require(WantsToWithdraw(pressuredRifle, false, true),
        "low approach hands the visible duel to cover even above the 30 HP emergency threshold");
    Require(!WantsToWithdraw(CombatApproach(Weapon::Rifle, 50.0f, 0.40f, false), false, true),
        "the exact approach threshold keeps the original strict comparison");
    Require(!WantsToWithdraw(pressuredRifle, true, true) && !WantsToWithdraw(pressuredRifle, false, false),
        "low approach still requires the own view cone and unplanted bomb");
    Require(!WantsToWithdraw(CombatApproach(Weapon::SMG, 30.0f, 0.0f, false), false, true),
        "SMGs retain SyPB's closing bias when evaluating low approach");
    std::puts("combat movement policy tests passed");
}
