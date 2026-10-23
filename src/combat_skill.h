// AgPB SyPB normal-mode difficulty and aim policy. GPL-3.0-only.
// SyPB 4c364fb: control.cpp difficulty mapping; globals.cpp g_skillTab;
// combat.cpp GetAimPosition/LookupEnemy/FocusEnemy/DoFirePause;
// navigate.cpp FacePosition. Matching CS-EBOT 2333562 files were compared;
// its different skill ranges and angular-velocity aim model are not used.
// Copyright (c) 2003-2023 HsK-Dev Blog.
// Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
// The complete original permission notice is retained in bot_combat.cpp.
#ifndef AGPB_COMBAT_SKILL_H
#define AGPB_COMBAT_SKILL_H
#include <cmath>

namespace AgPBCombatSkill {
struct Range { int minimum; int maximum; };
struct Row { float minReaction; float maxReaction; int recoilAmount; };
struct View { float pitch; float yaw; };

inline int ClampSkill(int skill) { return skill < 1 ? 1 : skill > 100 ? 100 : skill; }
inline Range DifficultyRange(int difficulty) {
    const Range presets[] = {{1,29}, {30,49}, {50,79}, {80,99}, {100,100}};
    return presets[difficulty < 0 ? 0 : difficulty > 4 ? 4 : difficulty];
}
inline int Bucket(int skill) { return ClampSkill(skill) / 20; }
inline Row SkillRow(int skill) {
    const Row rows[] = {{0.8f,1.0f,50}, {0.6f,0.8f,40}, {0.4f,0.6f,35},
                        {0.2f,0.3f,30}, {0.1f,0.2f,23}, {0.0f,0.1f,20}};
    return rows[Bucket(skill)];
}
inline float Normalize(float value) {
    if (!std::isfinite(value)) return 0.0f;
    value = std::fmod(value, 360.0f);
    if (value > 180.0f) value -= 360.0f;
    if (value < -180.0f) value += 360.0f;
    return value;
}
inline float Pitch(float value) {
    value = Normalize(value);
    return value < -89 ? -89 : value > 89 ? 89 : value;
}
inline float ReactionDelay(float actualReaction, int skill, int random0To100) {
    return actualReaction / (random0To100 < ClampSkill(skill) ? 3.0f : 1.0f);
}
inline bool WantsFire(bool reacted, float shootingConeDot) {
    return reacted && std::isfinite(shootingConeDot) && shootingConeDot >= 0.80f;
}
inline bool PerfectAim(int skill, bool wantsFire, bool sniper) {
    return wantsFire && (ClampSkill(skill) >= 70 || sniper);
}
inline bool AimAtHead(int skill, int random30To80, bool awp, int enemyHealth) {
    return ClampSkill(skill) >= random30To80 && (!awp || enemyHealth > 150);
}
inline bool HasAimNoise(int skill) { return ClampSkill(skill) <= 50; }
inline bool ImmediateRearRecognition(int skill) { return ClampSkill(skill) == 100; }
inline float RelativeNoise(float minimum, float maximum, float unitRandom) {
    // The GoldSrc player origin/bounds are centered; Source player origins
    // are at the feet. Apply the same bbox-sized error around the aim point
    // using centered relative extents, rather than adding a second body height.
    return minimum + (maximum - minimum) * unitRandom - (minimum + maximum) * 0.5f;
}
inline float Spring(int skill, bool insideViewCone, float previousDeviation, bool scoped) {
    float spring = insideViewCone ? 5.0f : 10.0f;
    // Preserve SyPB's integer division: these two bonuses apply at skill100.
    const float sourceSkillRatio = float(ClampSkill(skill) / 100);
    if (previousDeviation < 5.0f)
        spring += (5.0f - previousDeviation) * 0.25f * sourceSkillRatio * spring;
    if (scoped && previousDeviation >= 5.0f) spring *= 2.0f;
    return spring * (0.2f + ClampSkill(skill) / 125.0f);
}
inline View StepAim(View current, View target, float predictedPitch, float predictedYaw,
                    float interval, int skill, bool wantsFire, bool sniper,
                    bool insideViewCone, float previousDeviation, bool scoped) {
    View result = {Pitch(current.pitch), Normalize(current.yaw)};
    if (!std::isfinite(target.pitch) || !std::isfinite(target.yaw) ||
        !std::isfinite(interval) || interval <= 0.0f) return result;
    if (PerfectAim(skill, wantsFire, sniper)) return View{Pitch(target.pitch), Normalize(target.yaw)};
    const float spring = Spring(skill, insideViewCone, previousDeviation, scoped);
    // This is SyPB's proportional spring, without an invented rate cap.
    // Frame-independent weapon simulation still owns all physical movement.
    result.pitch = Pitch(result.pitch + interval * spring *
        Normalize(target.pitch + predictedPitch - result.pitch));
    result.yaw = Normalize(result.yaw + interval * spring *
        Normalize(target.yaw + predictedYaw - result.yaw));
    return result;
}
inline bool RecoilExceeded(int skill, float punchPitch, float punchYaw, float distance) {
    constexpr float PiOver360 = 0.00872664625997164788f;
    const float angle = (std::fabs(punchPitch) + std::fabs(punchYaw)) * PiOver360;
    return std::tan(angle) * (distance + distance / 4.0f) > SkillRow(skill).recoilAmount;
}
inline float RecoilPauseMaximum(int skill) { return 0.4f + 1.2f * ((100 - ClampSkill(skill)) / 100.0f); }
}
#endif
