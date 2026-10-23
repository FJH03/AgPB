// AgPB bounded perception memory and local search policy. GPL-3.0-only.
// SyPB basecode.cpp last-enemy / ACTIONFORENEMY and CAMP behavior is the
// baseline; matching CS-EBOT basecode/combat/navigate paths were compared.
// Hidden entities are never a source of refreshed observation coordinates.
#ifndef AGPB_SEARCH_POLICY_H
#define AGPB_SEARCH_POLICY_H
#include <cmath>
#include <cstdint>

namespace AgPBSearch {
constexpr float VisualTTL = 8.0f;
constexpr float VisualLookTTL = 3.0f;
constexpr float SearchBudget = 12.0f;
constexpr float SearchRadius = 512.0f;
constexpr float MaximumTravel = 1800.0f;
constexpr float LocalLegTravel = 640.0f;
constexpr int MaximumVisits = 4;
struct Point { float x = 0.0f, y = 0.0f, z = 0.0f; };
inline bool Finite(const Point &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
struct Observation {
    std::uintptr_t handle = 0;
    int userId = 0;
    Point position, look;
    float observedAt = -1.0f, expiresAt = -1.0f;
    bool valid = false, consumed = false;
};
inline bool Alive(const Observation &o, float now) {
    return o.valid && o.userId > 0 && Finite(o.position) && Finite(o.look) &&
        std::isfinite(now) && now >= o.observedAt && now < o.expiresAt;
}
inline bool SameIdentity(const Observation &o, std::uintptr_t handle, int userId) {
    return o.valid && o.handle == handle && o.userId == userId;
}
inline bool Observe(Observation &o, std::uintptr_t handle, int userId,
                    const Point &position, const Point &look, float now, float ttl) {
    if (userId <= 0 || !Finite(position) || !Finite(look) || !std::isfinite(now) ||
        !std::isfinite(ttl) || ttl <= 0.0f) return false;
    o.handle = handle; o.userId = userId; o.position = position; o.look = look;
    o.observedAt = now; o.expiresAt = now + ttl; o.valid = true; o.consumed = false;
    return true;
}
inline bool Audible(float distanceSquared, float radius, float ttl) {
    return std::isfinite(distanceSquared) && distanceSquared >= 0.0f &&
        std::isfinite(radius) && radius > 0.0f && radius <= 1600.0f &&
        std::isfinite(ttl) && ttl > 0.0f && ttl <= 5.0f && distanceSquared <= radius * radius;
}
inline bool SearchCandidate(float pathDistance, float snapshotDistanceSquared,
                            float remainingTravel, bool visited, bool passable) {
    return passable && !visited && std::isfinite(pathDistance) && pathDistance >= 0.0f &&
        std::isfinite(snapshotDistanceSquared) && snapshotDistanceSquared >= 0.0f &&
        pathDistance <= remainingTravel && snapshotDistanceSquared <= SearchRadius * SearchRadius;
}
inline float Deadline(float now, const Observation &o) {
    const float fromStart = now + SearchBudget, fromObservation = o.observedAt + SearchBudget;
    return fromStart < fromObservation ? fromStart : fromObservation;
}
struct Investigation {
    std::uint64_t token = 0;
    Observation evidence;
    int anchor = -1, visited[MaximumVisits] = {-1, -1, -1, -1}, visitedCount = 0;
    float deadline = 0.0f, dwellUntil = 0.0f, remainingTravel = MaximumTravel;
    float nextLookAt = 0.0f;
    Point scanLook;
};
inline bool Visited(const Investigation &search, int waypoint) {
    for (int i = 0; i < search.visitedCount; ++i)
        if (search.visited[i] == waypoint) return true;
    return false;
}
inline bool ReserveTravel(Investigation &search, float distance) {
    if (!std::isfinite(distance) || distance < 0.0f || distance > search.remainingTravel) return false;
    search.remainingTravel -= distance;
    return true;
}
inline bool CanContinue(const Investigation &search, float now) {
    return search.token != 0 && std::isfinite(now) && now < search.deadline &&
        search.visitedCount < MaximumVisits;
}
struct State { Observation visual, sound; Investigation investigation; };
}
#endif
