// AgPB visibility-constrained SyPB aim selection and shot-route decisions.
// GPL-3.0-only. SyPB combat.cpp GetAimPosition and basecode.cpp CheckVisibility
// are the behavior baseline; matching CS-EBOT combat.cpp/basecode.cpp paths
// were compared. Correct SyPB's any-visible-bit head branch: preferences may
// choose a currently proven visible spot or a separately proven penetration
// route to the same visually sensed enemy. Difficulty policy stays intact.
#ifndef AGPB_COMBAT_VISIBILITY_H
#define AGPB_COMBAT_VISIBILITY_H
#include <cmath>

namespace AgPBCombatVisibility {
enum Spot {
    Body = 0, Head, UpperBody, LeftShoulder, RightShoulder,
    LeftTorso, RightTorso, LowerBody, LeftLeg, RightLeg, SpotCount
};
constexpr unsigned int BodyVisible = 1u, HeadVisible = 2u;
inline unsigned int Bit(int spot) { return 1u << spot; }
inline int SelectSpot(unsigned int visibleSpots, bool preferHead) {
    if (preferHead && (visibleSpots & Bit(Head)) != 0) return Head;
    for (int i = 0; i < SpotCount; ++i)
        if (i != Head && (visibleSpots & Bit(i)) != 0) return i;
    return (visibleSpots & Bit(Head)) != 0 ? Head : -1;
}
inline unsigned int Summary(unsigned int visibleSpots) {
    const unsigned int bodySpots = ((1u << SpotCount) - 1u) & ~Bit(Head);
    return ((visibleSpots & bodySpots) != 0 ? BodyVisible : 0u) |
        ((visibleSpots & Bit(Head)) != 0 ? HeadVisible : 0u);
}
inline int SelectPreferredSpot(unsigned int visibleSpots, bool preferHead, bool coveredHeadReachable) {
    if (visibleSpots == 0) return -1; // Penetration never acquires an unseen enemy.
    if (preferHead && coveredHeadReachable) return Head;
    return SelectSpot(visibleSpots, preferHead);
}
inline bool PlayerClipIsCloser(float playerFraction, float extendedLength, float currentFraction, float primaryLength) {
    return std::isfinite(playerFraction) && playerFraction >= 0.0f && playerFraction <= 1.0f &&
        std::isfinite(extendedLength) && std::isfinite(primaryLength) && primaryLength > 0.0f &&
        playerFraction * extendedLength < currentFraction * primaryLength;
}
enum class RayHit { Clear, Target, World, OtherEntity };
inline bool ShotAllowed(bool idealSpotVisible, bool solidStart, RayHit firstHit,
                        float hitDistance, float targetFarDepth, bool penetrationReachesTarget = false) {
    if (!idealSpotVisible || solidStart || !std::isfinite(targetFarDepth) || targetFarDepth <= 0.0f) return false;
    if (firstHit == RayHit::Target || firstHit == RayHit::Clear) return true;
    // Aim noise and spring alignment can legitimately miss the target. Permit
    // a world impact beyond its entire projected bounds. Cover before/within
    // that depth requires a separate positive weapon/material/thickness probe.
    return firstHit == RayHit::World && std::isfinite(hitDistance) &&
        (hitDistance > targetFarDepth + 1.0f || penetrationReachesTarget);
}
}
#endif
