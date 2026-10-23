#include "../src/bomb_escape_policy.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main()
{
	// The closest safe route wins even if another node is much farther from C4.
	assert( AgPB_BombCandidateBetter( true, 5, 1900, true, 15, 3500 ) );
	assert( !AgPB_BombCandidateBetter( true, 15, 3500, true, 5, 1900 ) );
	assert( AgPB_BombCandidateBetter( true, 10, 1900, false, 2, 1700 ) );
	assert( AgPB_BombCandidateBetter( false, 3, 1600, false, 2, 1200 ) );
	assert( !AgPB_BombPositionSafe( 1800, true, 1750 ) );
	assert( AgPB_BombPositionSafe( 1878, true, 1750 ) );
	assert( !AgPB_BombPositionSafe( 10000, false, 0 ) );
	// Route ETA includes slower held weapons, crouch, ladder and jump costs.
	const float walk = AgPB_BombTravelSeconds( 500, 250, false, false, false );
	assert( AgPB_BombTravelSeconds( 500, 150, false, false, false ) > walk );
	assert( AgPB_BombTravelSeconds( 500, 250, true, false, false ) > walk * 2.5f );
	assert( AgPB_BombTravelSeconds( 500, 250, false, true, false ) > walk );
	assert( AgPB_BombTravelSeconds( 500, 250, false, false, true ) > walk + 0.8f );
	assert( AgPB_BombTravelSeconds( 100, 250, false, false, false ) + walk > walk );
	// A 15-second escape starts before the old fixed 8-second threshold.
	assert( AgPB_BombShouldEscape( true, true, 17, true, false, false, 0, false, true, 15 ) );
	assert( !AgPB_BombShouldEscape( true, true, 18, true, false, false, 0, false, true, 15 ) );
	// CT budget includes getting to C4, not just the five-second defuse.
	assert( AgPB_BombShouldEscape( false, true, 8, true, false, true, 4 + 5, false, true, 6 ) );
	assert( !AgPB_BombShouldEscape( false, true, 12, true, false, true, 4 + 5, false, true, 6 ) );
	// A nearly finished own defuse may continue using the actual countdown.
	assert( !AgPB_BombShouldEscape( false, true, 3, true, false, true, 1, false, true, 8 ) );
	// An active own defuse may use the last second reserved for starting a
	// new defuse, but must still finish strictly earlier than the explosion.
	assert( !AgPB_BombShouldEscape( false, true, 3, true, false, true, 2.5f, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, 2.5f, false, true, 8 ) );
	assert( !AgPB_BombShouldEscape( false, true, 3, true, false, true, 2.999f, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, 3, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, 3.001f, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, 2, false, true, 8 ) );
	assert( !AgPB_BombShouldEscape( false, true, 3.001f, true, false, true, 2, false, true, 8 ) );
	assert( AgPB_BombShouldEscape( false, false, 3, true, false, true, 1, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, false, false, true, 1, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, true, true, 1, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 0, true, false, true, 0, false, true, 8, true ) );
	assert( !AgPB_BombShouldEscape( false, true, 0.01f, true, false, true, 0, false, true, 8, true ) );
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	assert( AgPB_BombShouldEscape( false, true, nan, true, false, true, 1, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, nan, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, true, -1, false, true, 8, true ) );
	assert( AgPB_BombShouldEscape( false, true, 3, true, false, false, 1, false, true, 8, true ) );
	float remaining = -1;
	assert( AgPB_BombDefuseRemaining( true, true, 102.5f, 100, remaining ) && remaining == 2.5f );
	assert( AgPB_BombDefuseRemaining( true, true, 100, 100, remaining ) && remaining == 0 );
	assert( AgPB_BombDefuseRemaining( true, true, 99.99f, 100, remaining ) && remaining == 0 );
	assert( !AgPB_BombDefuseRemaining( false, true, 102.5f, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, false, 102.5f, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, true, 0, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, true, -1, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, true, nan, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, true, inf, 100, remaining ) );
	assert( !AgPB_BombDefuseRemaining( true, true, 102.5f, nan, remaining ) );
	assert( AgPB_BombShouldEscape( false, false, 999, true, false, true, 1, false, true, 8 ) );
	assert( AgPB_BombShouldEscape( true, true, 40, false, false, false, 0, false, false, 0 ) );
	assert( AgPB_BombShouldEscape( true, true, 40, true, false, false, 0, false, false, 0 ) );
	assert( AgPB_BombShouldEscape( true, true, 40, true, true, false, 0, false, true, 5 ) );
	// Ordinary objective cooldown cannot suppress emergency escape.
	assert( !AgPB_BombObjectiveEligible( false, true, 10, 15 ) );
	assert( AgPB_BombObjectiveEligible( true, true, 10, 15 ) );
	assert( AgPB_BombObjectiveEligible( false, true, 15, 15 ) );
	assert( !AgPB_BombThreatEnded( true, true, false ) );
	assert( !AgPB_BombThreatEnded( true, false, true ) );
	assert( AgPB_BombThreatEnded( true, true, true ) );
	assert( AgPB_BombThreatEnded( false, false, false ) );
	// A farther supported entry can bypass a long authored detour. The
	// winning parent chain must start there, not at the nearest node.
	float edges[4][4] = { { -1, 8, 10, -1 }, { -1, -1, 0.6f, -1 },
		{ -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
	auto edgeReader = [&]( int from, int slot, int &to, float &length, float &seconds ) {
		to = slot; seconds = edges[from][slot]; length = seconds * 200.0f; return seconds >= 0.0f;
	};
	std::vector<AgPBBombRouteSeed> seeds = { { 0, 0.1f, 10 }, { 1, 0.5f, 100 } };
	std::vector<AgPBBombRouteLabel> routes;
	AgPB_BombRouteBudgets( 4, 4, seeds, edgeReader, routes );
	assert( routes[2].entry == 1 && routes[2].parent == 1 );
	assert( std::fabs( routes[2].seconds - 1.1f ) < 0.001f );
	assert( routes[3].entry == -1 );
	// A longer standing route beats a shorter crouching route on actual ETA.
	edges[0][1] = AgPB_BombTravelSeconds( 100, 250, true, false, false );
	edges[1][2] = -1;
	edges[1][3] = AgPB_BombTravelSeconds( 100, 250, true, false, false );
	edges[0][2] = AgPB_BombTravelSeconds( 180, 250, false, false, false );
	edges[2][3] = AgPB_BombTravelSeconds( 180, 250, false, false, false );
	seeds.resize( 1 );
	AgPB_BombRouteBudgets( 4, 4, seeds, edgeReader, routes );
	assert( routes[3].parent == 2 && routes[3].entry == 0 );
	assert( routes[3].seconds < seeds[0].seconds + edges[0][1] + edges[1][3] );

	struct Observation { int handle; bool plantedObserved, positionKnown; float position; };
	Observation missing = { 1, true, false, 0 }, recovered = { 1, true, true, 900 };
	Observation retained = AgPB_BombMergeUnknownObservation( missing, recovered, true, true );
	assert( retained.positionKnown && retained.position == 900 );
	retained = AgPB_BombMergeUnknownObservation( retained, missing, true, true );
	assert( retained.positionKnown && retained.position == 900 );
	missing.handle = 0;
	retained = AgPB_BombMergeUnknownObservation( missing, recovered, false, true );
	assert( retained.handle == 1 && retained.positionKnown );
	recovered.handle = 2;
	retained = AgPB_BombMergeUnknownObservation( retained, recovered, true, true );
	assert( retained.handle == 1 );
	std::puts( "bomb escape policy: 63 checks passed" );
	return 0;
}
