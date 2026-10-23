// Policy regression: observation identity/expiry, sound bounds and finite search.
#include "../src/search_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void Require(bool okay, const char *why) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    using namespace AgPBSearch;
    Observation memory;
    const std::uintptr_t handle = std::uintptr_t(0x12345678u) | (std::uintptr_t(1) << 40);
    const Point position{10, 20, 30}, look{10, 20, 80};
    Require(Observe(memory, handle, 7, position, look, 10, VisualTTL), "visible snapshot accepted");
    Require(SameIdentity(memory, handle, 7), "full handle and userid retained");
    Require(!SameIdentity(memory, handle ^ (std::uintptr_t(1) << 40), 7), "high handle bits prevent recycled identity");
    Require(!SameIdentity(memory, handle, 8), "userid prevents slot reuse");
    Require(Alive(memory, 10) && Alive(memory, 17.9f) && !Alive(memory, 18), "memory has exact finite TTL");
    Require(!Alive(memory, 9), "time reversal invalidates old map memory");
    Require(memory.position.x == 10 && memory.look.z == 80, "expiry checks never refresh coordinates");
    memory.consumed = true;
    Require(Alive(memory, 11) && memory.consumed, "search consumption leaves gaze memory intact");
    Observation sound;
    Require(Audible(600.0f * 600.0f, 600, 2.5f) && !Audible(601.0f * 601.0f, 600, 2.5f), "sound uses event-local distance");
    Require(!Audible(0, 1700, 3) && !Audible(0, 600, 6), "sound range and TTL are bounded");
    Require(Observe(sound, handle, 7, Point{50, 60, 30}, Point{50, 60, 70}, 11, 2.5f), "sound snapshot independent");
    Require(memory.position.x == 10 && sound.position.x == 50, "hearing never overwrites last visible coordinates");
    Require(!Observe(memory, handle, 7, Point{std::numeric_limits<float>::infinity(),0,0}, look, 11, 8), "invalid samples rejected");
    Investigation search;
    search.token = 1; search.evidence = memory; search.deadline = Deadline(12, memory);
    Require(search.deadline == 22 && CanContinue(search, 21.9f) && !CanContinue(search, 22), "lease tied to original evidence, never refreshed by polling");
    Require(SearchCandidate(400, 200*200, 1800, false, true), "reachable authored point accepted");
    Require(!SearchCandidate(2000, 0, 1800, false, true) && !SearchCandidate(100, 513*513, 1800, false, true), "travel and evidence radius bounded");
    Require(!SearchCandidate(1, 1, 1800, true, true) && !SearchCandidate(1, 1, 1800, false, false), "visited or team-ineligible point rejected");
    Require(ReserveTravel(search, 1700) && search.remainingTravel == 100, "actual route length charged once");
    Require(!ReserveTravel(search, 101) && search.remainingTravel == 100, "changed or different-entry route cannot exceed remaining budget");
    const float fixedDeadline = search.deadline;
    Require(Observe(sound, handle, 7, Point{60, 60, 30}, look, 15, 2.5f) && search.deadline == fixedDeadline,
        "later sound never renews current investigation deadline");
    search.visited[0] = 5; search.visitedCount = 1;
    Require(Visited(search, 5) && !Visited(search, 6), "visited nodes never revisited");
    search.visitedCount = MaximumVisits;
    Require(!CanContinue(search, 15), "four-node search terminates");
    std::puts("enemy search policy tests passed");
}
