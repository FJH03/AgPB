// Production physics arithmetic regression, independent of the game SDK.
#include "../src/bullet_penetration.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float actual, float expected) { return std::fabs(actual - expected) < 0.001f; }
static AgPBBullet::Profile Rifle() {
    AgPBBullet::Profile profile;
    const auto ammo = AgPBBullet::AmmoProfile(AgPBBullet::Ammo::MM762);
    profile.power = ammo.power; profile.penetrationDistance = ammo.distance;
    profile.damage = 40; profile.range = 8000; profile.rangeModifier = 0.98f; profile.count = 2;
    return profile;
}
int main() {
    using namespace AgPBBullet;
    Require(AmmoProfile(Ammo::AE50).power == 30 && AmmoProfile(Ammo::AE50).distance == 1000 &&
            AmmoProfile(Ammo::MM762).power == 39 && AmmoProfile(Ammo::MM762).distance == 5000 &&
            AmmoProfile(Ammo::MM556).power == 35 && AmmoProfile(Ammo::MM556).distance == 4000 &&
            AmmoProfile(Ammo::Mag338).power == 45 && AmmoProfile(Ammo::Mag338).distance == 8000 &&
            AmmoProfile(Ammo::MM9).power == 21 && AmmoProfile(Ammo::MM9).distance == 800 &&
            AmmoProfile(Ammo::ACP45).power == 15 && AmmoProfile(Ammo::ACP45).distance == 500 &&
            AmmoProfile(Ammo::Sig357).power == 25 && AmmoProfile(Ammo::Sig357).distance == 800 &&
            AmmoProfile(Ammo::MM57).power == 30 && AmmoProfile(Ammo::MM57).distance == 2000 &&
            AmmoProfile(Ammo::Buckshot).power == 0 && AmmoProfile(Ammo::Unknown).power == 0,
            "source ammo names map to exact penetration power and distance");
    Require(Near(MaterialModifiers('M', false, 'M').penetration, 1) &&
            Near(MaterialModifiers('W', false, 'W').penetration, 2) &&
            Near(MaterialModifiers('C', true, 'C').penetration, 1) &&
            Near(MaterialModifiers('C', true, 'C').damage, 0.99f), "same wood/metal bonus and contents-grate override");
    State wood(Rifle());
    Require(ApplyImpact(wood, 0.05f) && Near(wood.distance, 400), "impact adds current segment travel");
    const float impactDamage = 40 * std::pow(0.98f, 0.8f);
    Require(Near(wood.damage, impactDamage) && Penetrate(wood, 12, 'W', false, 'W') &&
            Near(wood.power, 33) && Near(wood.distance, 412) && Near(wood.range, 3794) &&
            Near(wood.damage, impactDamage * 0.6f) && wood.count == 1,
            "thin wood penetrates with exact power damage range and counter updates");
    const float beforeSecond = wood.damage;
    Require(ApplyImpact(wood, 100.0f / wood.range) && Near(wood.distance, 512) &&
            Near(wood.damage, beforeSecond * std::pow(0.98f, 512.0f / 500.0f)),
            "each impact attenuates by cumulative rather than segment distance");
    Require(Penetrate(wood, 20, 'W', false, 'W') && Near(wood.power, 23) && wood.count == 0 &&
            !Penetrate(wood, 1, 'W', false, 'W'), "multiple walls consume penetration count and remaining power");
    State concrete(Rifle());
    Require(!Penetrate(concrete, 16, 'C', false, 'C') && Near(concrete.power, 39) && concrete.count == 2,
            "concrete thicker than power times material modifier blocks without partial state updates");
    Require(Penetrate(concrete, 15, 'C', false, 'C') && Near(concrete.power, 1.5f) &&
            !Penetrate(concrete, 1, 'C', false, 'C'), "remaining power constrains the next surface");
    State distant(Rifle());
    Require(ApplyImpact(distant, 0.75f) && distant.count == 0 &&
            !Penetrate(distant, 1, 'W', false, 'W'), "ammo penetration distance cuts off further ordinary walls");
    Require(Penetrate(distant, 1, 'C', true, 'C') && distant.count == -1 &&
            !Penetrate(distant, 1, 'C', true, 'C'), "contents grate has one count-zero exception then stops");
    Profile zeroCount = Rifle(); zeroCount.count = 0;
    State materialGrate(zeroCount);
    Require(!Penetrate(materialGrate, 1, 'G', false, 'G'), "grate material alone does not grant contents-grate counter exception");
    State exhausted(Rifle()); exhausted.range = 50; exhausted.distance = 50;
    Require(!Penetrate(exhausted, 1, 'W', false, 'W'), "nonpositive remaining forward range fails closed");
    State invalid(Rifle());
    Require(!ApplyImpact(invalid, -0.1f) && !ApplyImpact(invalid, 1.1f) &&
            !ApplyImpact(invalid, std::numeric_limits<float>::quiet_NaN()) &&
            !Penetrate(invalid, std::numeric_limits<float>::infinity(), 'W', false, 'W'),
            "invalid trace and thickness inputs fail closed");
    std::puts("PASS Source ammo, material, range, power, damage and grate penetration arithmetic");
    return 0;
}
