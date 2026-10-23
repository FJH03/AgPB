// AgPB Source CS bullet penetration arithmetic. GPL-3.0-only.
// Physics reference: Valve source-engine-czero game/shared/cstrike/
// cs_player_shared.cpp GetBulletTypeParameters, GetMaterialParameters and
// CCSPlayer::FireBullet. Weapon script defaults: cs_weapon_parse.cpp.
// These are local read-only estimates of physical rules, not engine bot AI.
#ifndef AGPB_BULLET_PENETRATION_H
#define AGPB_BULLET_PENETRATION_H
#include <cmath>

namespace AgPBBullet {
enum class Ammo { AE50, MM762, MM556, Mag338, MM9, Buckshot, ACP45, Sig357, MM57, Unknown };
struct AmmoParameters { float power, distance; };
inline AmmoParameters AmmoProfile(Ammo ammo) {
    switch (ammo) {
    case Ammo::AE50: return {30.0f, 1000.0f};
    case Ammo::MM762: return {39.0f, 5000.0f};
    case Ammo::MM556: return {35.0f, 4000.0f};
    case Ammo::Mag338: return {45.0f, 8000.0f};
    case Ammo::MM9: return {21.0f, 800.0f};
    case Ammo::ACP45: return {15.0f, 500.0f};
    case Ammo::Sig357: return {25.0f, 800.0f};
    case Ammo::MM57: return {30.0f, 2000.0f};
    case Ammo::Buckshot:
    case Ammo::Unknown: return {0.0f, 0.0f};
    }
    return {0.0f, 0.0f};
}
struct Profile {
    float power = 0.0f, penetrationDistance = 0.0f;
    float range = 8192.0f, damage = 42.0f, rangeModifier = 0.98f;
    int count = 1;
};
struct State : Profile {
    float distance;
    State() : distance(0.0f) {}
    explicit State(const Profile &profile) : Profile(profile), distance(0.0f) {}
};
struct Modifiers { float penetration, damage; };
inline Modifiers MaterialModifiers(int enterMaterial, bool entryGrate, int exitMaterial) {
    Modifiers result = {1.0f, 0.5f};
    switch (enterMaterial) {
    case 'M': case 'D': result = {0.5f, 0.3f}; break;
    case 'C': result = {0.4f, 0.25f}; break;
    case 'G': result = {1.0f, 0.99f}; break;
    case 'V': result = {0.5f, 0.45f}; break;
    case 'T': result = {0.65f, 0.3f}; break;
    case 'P': result = {0.4f, 0.45f}; break;
    case 'W': result = {1.0f, 0.6f}; break;
    default: break;
    }
    // CONTENTS_GRATE overrides material, even for invisible concrete grates.
    if (entryGrate) result = {1.0f, 0.99f};
    if (enterMaterial == exitMaterial && (exitMaterial == 'W' || exitMaterial == 'M'))
        result.penetration *= 2.0f;
    return result;
}
inline bool Valid(const State &state) {
    return std::isfinite(state.power) && state.power >= 0.0f &&
        std::isfinite(state.penetrationDistance) && state.penetrationDistance >= 0.0f &&
        std::isfinite(state.range) && state.range > 0.0f &&
        std::isfinite(state.damage) && state.damage > 0.0f &&
        std::isfinite(state.rangeModifier) && state.rangeModifier > 0.0f &&
        std::isfinite(state.distance) && state.distance >= 0.0f;
}
inline bool ApplyImpact(State &state, float traceFraction) {
    if (!Valid(state) || !std::isfinite(traceFraction) || traceFraction < 0.0f || traceFraction > 1.0f)
        return false;
    State next = state;
    next.distance += traceFraction * next.range;
    // Preserve Source's cumulative-distance attenuation at every impact.
    next.damage *= std::pow(next.rangeModifier, next.distance / 500.0f);
    if (next.distance > next.penetrationDistance && next.count > 0) next.count = 0;
    if (!Valid(next)) return false;
    state = next;
    return true;
}
inline bool Penetrate(State &state, float thickness, int enterMaterial, bool entryGrate, int exitMaterial) {
    if (!Valid(state) || !std::isfinite(thickness) || thickness < 0.0f ||
        state.count < 0 || (state.count == 0 && !entryGrate)) return false;
    const Modifiers modifier = MaterialModifiers(enterMaterial, entryGrate, exitMaterial);
    if (thickness > state.power * modifier.penetration) return false;
    State next = state;
    next.power -= thickness / modifier.penetration;
    next.distance += thickness;
    // Source subtracts cumulative distance from its current range and halves
    // what remains after each exit. Do not replace this with segment distance.
    next.range = (next.range - next.distance) * 0.5f;
    next.damage *= modifier.damage;
    --next.count;
    if (!Valid(next)) return false;
    state = next;
    return true;
}
}
#endif
