/*
 * AgPB bomb escape budget. GPL-3.0-only.
 * SyPB source/basecode.cpp OutOfBombTimer / TASK_ESCAPEFROMBOMB supplies
 * the normal-mode baseline (travel budget, nearest safe route). CS-EBOT's
 * source/basecode.cpp and navigate.cpp were compared; zombie avoidance
 * behavior and GoldSrc's fixed bomb distance are not used here.
 * This engine-independent policy is also exercised by the standalone tests.
 */
#ifndef _INCLUDE_AGPB_BOMB_ESCAPE_POLICY_H_
#define _INCLUDE_AGPB_BOMB_ESCAPE_POLICY_H_

#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

struct AgPBBombRouteSeed
{
	int waypoint;
	float seconds;
	float length;
};

struct AgPBBombRouteLabel
{
	AgPBBombRouteLabel() : seconds( 1.0e30f ), length( 0.0f ), parent( -1 ), entry( -1 ) {}
	float seconds, length;
	int parent, entry;
};

// Multiple supported entries share one search. The callback exposes only
// existing authored edges; this neither creates links nor infers jumps.
template <typename EdgeReader>
void AgPB_BombRouteBudgets( int count, int slots, const std::vector<AgPBBombRouteSeed> &seeds,
                            EdgeReader readEdge, std::vector<AgPBBombRouteLabel> &labels )
{
	labels.assign( count, AgPBBombRouteLabel() );
	typedef std::pair<float, int> Item;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item> > pending;
	for ( const AgPBBombRouteSeed &seed : seeds )
	{
		if ( seed.waypoint < 0 || seed.waypoint >= count || seed.seconds < 0.0f ||
		     !std::isfinite( seed.seconds ) || seed.seconds >= labels[seed.waypoint].seconds ) continue;
		AgPBBombRouteLabel &label = labels[seed.waypoint];
		label.seconds = seed.seconds; label.length = seed.length; label.entry = seed.waypoint;
		pending.push( Item( seed.seconds, seed.waypoint ) );
	}
	while ( !pending.empty() )
	{
		const Item current = pending.top(); pending.pop();
		if ( current.first > labels[current.second].seconds ) continue;
		for ( int slot = 0; slot < slots; ++slot )
		{
			int next = -1; float length = 0.0f, seconds = 0.0f;
			if ( !readEdge( current.second, slot, next, length, seconds ) || next < 0 || next >= count ||
			     seconds < 0.0f || !std::isfinite( seconds ) ) continue;
			const float arrival = current.first + seconds;
			if ( arrival >= labels[next].seconds ) continue;
			AgPBBombRouteLabel &label = labels[next];
			label.seconds = arrival; label.length = labels[current.second].length + length;
			label.parent = current.second; label.entry = labels[current.second].entry;
			pending.push( Item( arrival, next ) );
		}
	}
}

inline float AgPB_BombTravelSeconds( float distance, float maximumSpeed,
                                    bool crouch, bool ladder, bool jump )
{
	float speed = maximumSpeed > 0.0f && std::isfinite( maximumSpeed ) ? maximumSpeed : 200.0f;
	if ( speed > 250.0f ) speed = 250.0f;
	// Allow for acceleration and turns instead of assuming permanent top speed.
	speed *= 0.85f;
	if ( crouch ) speed *= 0.34f;
	if ( ladder && speed > 100.0f ) speed = 100.0f;
	if ( speed < 1.0f ) speed = 1.0f;
	return ( distance > 0.0f ? distance / speed + 0.15f : 0.0f ) + ( jump ? 0.85f : 0.0f );
}

inline bool AgPB_BombPositionSafe( float distance, bool radiusKnown, float radius )
{
	// Reserve room for the hull, the approach shell and residual stopping travel.
	return radiusKnown && std::isfinite( radius ) && radius >= 0.0f && distance >= radius + 128.0f;
}

inline bool AgPB_BombCandidateBetter( bool safe, float seconds, float distance,
                                      bool bestSafe, float bestSeconds, float bestDistance )
{
	if ( safe != bestSafe ) return safe;
	if ( safe ) return seconds < bestSeconds;
	// An unreachable safety boundary is explicitly a best-effort retreat.
	return distance > bestDistance || ( distance == bestDistance && seconds < bestSeconds );
}

inline bool AgPB_BombDefuseRemaining( bool ownDefuse, bool countdownKnown,
                                     float countdown, float now, float &remaining )
{
	if ( !ownDefuse || !countdownKnown || !std::isfinite( countdown ) || countdown <= 0.0f ||
	     !std::isfinite( now ) ) return false;
	remaining = countdown > now ? countdown - now : 0.0f;
	return std::isfinite( remaining );
}

inline bool AgPB_BombShouldEscape( bool terrorist, bool timerKnown, float secondsLeft,
                                   bool radiusKnown, bool committed, bool canDefuse,
                                   float defuseSeconds, bool safeHere, bool safeRoute,
                                   float escapeSeconds, bool ownDefuseCountdownKnown = false )
{
	if ( committed || !timerKnown || !std::isfinite( secondsLeft ) || !radiusKnown ) return true;
	if ( !terrorist )
	{
		if ( !canDefuse || !std::isfinite( defuseSeconds ) || defuseSeconds < 0.0f ) return true;
		// SyPB OutOfBombTimer preserves an active progress bar. CS-EBOT's
		// corresponding basecode/navigation has no normal-mode defuse task.
		// Keep our confirmed own defuse only if it finishes strictly before
		// C4; the approach/start reserve must not cancel a close-call defuse.
		return ownDefuseCountdownKnown ? !( defuseSeconds < secondsLeft ) : secondsLeft <= defuseSeconds + 1.0f;
	}
	if ( !safeHere && !safeRoute ) return true;
	float reserve = safeHere ? 0.0f : escapeSeconds + 2.0f;
	if ( reserve < 8.0f ) reserve = 8.0f;
	return secondsLeft <= reserve;
}

inline bool AgPB_BombObjectiveEligible( bool escape, bool declined, float now, float retryAfter )
{
	return escape || !declined || now >= retryAfter;
}

inline bool AgPB_BombThreatEnded( bool sameRound, bool sameBomb, bool resolved )
{
	// Missing entities, UNKNOWN observations and elapsed timers prove nothing.
	return !sameRound || ( sameBomb && resolved );
}

template <typename BombObservation>
BombObservation AgPB_BombMergeUnknownObservation( const BombObservation &previous,
                                                   const BombObservation &observed,
                                                   bool previousHandleValid, bool observedHandleValid )
{
	BombObservation remembered = previous;
	if ( observed.plantedObserved && ( observed.handle == previous.handle || !previousHandleValid ) )
	{
		if ( observedHandleValid ) remembered.handle = observed.handle;
		if ( observed.positionKnown ) { remembered.position = observed.position; remembered.positionKnown = true; }
	}
	return remembered;
}

#endif
