/*
 * AgPB normal-mode goal policy, adapted from SyPB source/navigate.cpp
 * FindGoal, source/control.cpp personality initialization and
 * source/basecode.cpp TASK_NORMAL objective-area guard decisions.
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * CS-EBOT source/navigate.cpp was compared. Its FindGoalHuman/Zombie camp
 * selection is specific to ZM/ZE and is not used. AgPB: GPL-3.0-only.
 * This production policy has no engine dependency, for standalone regression.
 */
#ifndef _INCLUDE_AGPB_LOCAL_PLANNER_H_
#define _INCLUDE_AGPB_LOCAL_PLANNER_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

enum class AgPBLocalPersonality { Normal, Rusher, Careful };
enum class AgPBLocalTactic { Defense, Camp, Offense, Objective };

struct AgPBLocalVisit
{
    int waypoint = -1;
    float time = -1000.0f;
};

struct AgPBLocalPlannerState
{
    bool initialized = false;
    AgPBLocalPersonality personality = AgPBLocalPersonality::Normal;
    float baseAggression = 0.55f, baseFear = 0.55f;
    float aggression = 0.55f, fear = 0.55f;
    int lastHealth = -1;
    float nextEmotionUpdate = 0.0f;
    AgPBLocalVisit visits[8];
    int nextVisit = 0;
    int selectedWaypoint = -1;
    AgPBLocalTactic selectedTactic = AgPBLocalTactic::Offense;
    bool selectedCamp = false;
    float selectedAt = 0.0f;
    float lastCampAt = -1000.0f;
    std::uint64_t scanOwner = 0;
    int lookWaypoint = -1;
    float nextScan = 0.0f;
    float lookPoint[3] = { 0.0f, 0.0f, 0.0f };
};

struct AgPBLocalGoalContext
{
    int currentWaypoint = -1;
    float now = 0.0f;
    bool bombMap = false;
    bool hostageMap = false;
    bool defending = false;
    bool sniper = false;
};

struct AgPBLocalGoalCandidate
{
    int waypoint = -1;
    float pathDistance = 0.0f;
    float occupancy = 0.0f; // Same-team bodies and reserved task/route goals only.
    bool reachable = true;
    bool passable = true;
    bool objective = false;
    bool defendArea = false;
    bool camp = false;
    bool sniper = false;
    bool avoid = false;
};

struct AgPBLocalGoalChoice
{
    int waypoint = -1;
    AgPBLocalTactic tactic = AgPBLocalTactic::Offense;
    bool camp = false;
    float desires[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

class AgPBLocalRandom
{
public:
    explicit AgPBLocalRandom( std::uint32_t seed ) : m_State( seed != 0 ? seed : 0x9e3779b9u ) {}
    float Unit()
    {
        m_State ^= m_State << 13; m_State ^= m_State >> 17; m_State ^= m_State << 5;
        return static_cast<float>( m_State & 0x00ffffffu ) / 16777216.0f;
    }
    float Range( float low, float high ) { return low + ( high - low ) * Unit(); }
private:
    std::uint32_t m_State;
};

inline float AgPB_LocalClamp( float x, float low, float high )
{
    return std::max( low, std::min( x, high ) );
}

inline void AgPB_LocalInitializePersonality( AgPBLocalPlannerState &state,
                                              AgPBLocalPersonality personality, std::uint32_t seed )
{
    AgPBLocalRandom random( seed );
    state.initialized = true;
    state.personality = personality;
    const bool rusher = personality == AgPBLocalPersonality::Rusher;
    const bool careful = personality == AgPBLocalPersonality::Careful;
    state.baseAggression = random.Range( rusher ? 0.7f : careful ? 0.0f : 0.4f,
                                         rusher ? 1.0f : careful ? 0.4f : 0.7f );
    state.baseFear = random.Range( careful ? 0.7f : rusher ? 0.0f : 0.4f,
                                  careful ? 1.0f : rusher ? 0.4f : 0.7f );
    state.aggression = state.baseAggression;
    state.fear = state.baseFear;
}

inline void AgPB_LocalRememberGoal( AgPBLocalPlannerState &state, int waypoint, float now )
{
    if ( waypoint < 0 ) return;
    for ( AgPBLocalVisit &visit : state.visits )
        if ( visit.waypoint == waypoint ) { visit.time = now; return; }
    AgPBLocalVisit &visit = state.visits[state.nextVisit];
    visit.waypoint = waypoint; visit.time = now;
    state.nextVisit = ( state.nextVisit + 1 ) % 8;
}

inline float AgPB_LocalVisitAge( const AgPBLocalPlannerState &state, int waypoint, float now )
{
    for ( const AgPBLocalVisit &visit : state.visits )
        if ( visit.waypoint == waypoint && now >= visit.time ) return now - visit.time;
    return 1000.0f;
}

inline bool AgPB_LocalCandidateMatches( const AgPBLocalGoalCandidate &candidate,
                                       const AgPBLocalGoalContext &context, AgPBLocalTactic tactic,
                                       float farthest )
{
    switch ( tactic )
    {
    case AgPBLocalTactic::Defense:
        return candidate.defendArea || candidate.camp ||
            ( !context.defending && candidate.pathDistance < farthest * 0.55f );
    case AgPBLocalTactic::Camp:
        return candidate.camp || ( context.sniper && candidate.sniper );
    case AgPBLocalTactic::Offense:
        return candidate.objective || candidate.pathDistance >= farthest * 0.4f;
    case AgPBLocalTactic::Objective:
        return candidate.objective;
    }
    return false;
}

inline bool AgPB_LocalCampReady( const AgPBLocalPlannerState &state, float now )
{
    return std::isfinite( now ) && std::isfinite( state.lastCampAt ) && now - state.lastCampAt >= 20.0f;
}

inline bool AgPB_LocalMayCamp( AgPBLocalTactic tactic, const AgPBLocalGoalCandidate &candidate,
                               const AgPBLocalGoalContext &context, const AgPBLocalPlannerState &state,
                               float roll )
{
    if ( !AgPB_LocalCampReady( state, context.now ) || candidate.occupancy >= 0.5f ) return false;
    if ( tactic == AgPBLocalTactic::Camp )
        return candidate.camp || ( candidate.sniper && context.sniper );
    // SyPB's normal TASK_NORMAL can defend a hostage/bomb objective through
    // FindDefendWaypoint without an explicit CAMP flag. This stays a role-
    // and position-dependent tactical choice, never every route's arrival.
    return tactic == AgPBLocalTactic::Defense && context.defending && candidate.defendArea &&
        roll < AgPB_LocalClamp( state.fear, 0.0f, 1.0f );
}

inline AgPBLocalGoalChoice AgPB_SelectLocalGoal( const AgPBLocalPlannerState &state,
                                                const AgPBLocalGoalContext &context,
                                                const std::vector<AgPBLocalGoalCandidate> &candidates,
                                                std::uint32_t seed )
{
    AgPBLocalRandom random( seed );
    AgPBLocalGoalChoice result;
    float offensive = AgPB_LocalClamp( state.aggression, 0.0f, 1.0f ) * 100.0f;
    float defensive = AgPB_LocalClamp( state.fear, 0.0f, 1.0f ) * 100.0f;
    // SyPB's map-role adjustment belongs to behavior, not waypoint access flags.
    if ( context.bombMap || context.hostageMap )
    {
        defensive += context.defending ? 30.0f : -30.0f;
        offensive += context.defending ? -30.0f : 30.0f;
    }
    result.desires[0] = defensive + random.Range( 0.0f, 50.0f );
    result.desires[1] = defensive + random.Range( 0.0f, 70.0f );
    result.desires[2] = offensive + random.Range( 0.0f, 50.0f );
    result.desires[3] = offensive + random.Range( 0.0f, 70.0f );
    if ( context.sniper || ( context.bombMap && context.defending ) )
        result.desires[1] *= random.Range( 1.5f, 2.5f );
    int tactic = 0;
    for ( int i = 1; i < 4; ++i )
        if ( result.desires[i] > result.desires[tactic] ) tactic = i;
    result.tactic = static_cast<AgPBLocalTactic>( tactic );

    float farthest = 0.0f;
    auto eligible = [&]( const AgPBLocalGoalCandidate &c ) {
        return c.waypoint >= 0 && c.waypoint != context.currentWaypoint &&
            c.waypoint != state.selectedWaypoint && c.reachable && c.passable &&
            std::isfinite( c.pathDistance ) && c.pathDistance > 0.0f;
    };
    for ( const AgPBLocalGoalCandidate &c : candidates )
        if ( eligible( c ) ) farthest = std::max( farthest, c.pathDistance );
    if ( farthest <= 0.0f ) return result;

    std::vector<const AgPBLocalGoalCandidate *> choices;
    for ( const AgPBLocalGoalCandidate &c : candidates )
        if ( eligible( c ) && AgPB_LocalCandidateMatches( c, context, result.tactic, farthest ) )
            choices.push_back( &c );
    // A defender with no authored camp points can still choose SyPB's
    // objective-area guard tactic. Only the later conditional Defense rule
    // may permit stopping there; no waypoint flags or links are invented.
    if ( choices.empty() && result.tactic == AgPBLocalTactic::Camp && context.defending )
    {
        result.tactic = AgPBLocalTactic::Defense;
        for ( const AgPBLocalGoalCandidate &c : candidates )
            if ( eligible( c ) && c.defendArea ) choices.push_back( &c );
    }
    // A missing tactical category otherwise falls back to reachable patrol.
    if ( choices.empty() )
    {
        result.tactic = AgPBLocalTactic::Offense;
        for ( const AgPBLocalGoalCandidate &c : candidates )
            if ( eligible( c ) ) choices.push_back( &c );
    }

    bool fresh = false;
    for ( const AgPBLocalGoalCandidate *c : choices )
        if ( AgPB_LocalVisitAge( state, c->waypoint, context.now ) >= 45.0f ) fresh = true;
    // Do not ping-pong inside an exhausted tactical category while the graph
    // still offers unvisited patrol goals elsewhere.
    if ( !fresh )
    {
        std::vector<const AgPBLocalGoalCandidate *> unexplored;
        for ( const AgPBLocalGoalCandidate &c : candidates )
            if ( eligible( c ) && AgPB_LocalVisitAge( state, c.waypoint, context.now ) >= 45.0f )
                unexplored.push_back( &c );
        if ( !unexplored.empty() )
        {
            choices.swap( unexplored );
            result.tactic = AgPBLocalTactic::Offense;
            fresh = true;
        }
    }
    const float preferredFraction = result.tactic == AgPBLocalTactic::Defense ? 0.3f :
        result.tactic == AgPBLocalTactic::Camp ? 0.5f : result.tactic == AgPBLocalTactic::Offense ? 0.8f : 0.65f;
    float bestScore = -1.0e30f;
    const AgPBLocalGoalCandidate *best = NULL;
    // SyPB randomizes goal candidates instead of always choosing the nearest
    // objective. AgPB has no learned danger table yet: current friendly
    // occupancy, recent visits, authored avoid flags and graph distance rank
    // the randomized candidates without pretending to have combat experience.
    for ( const AgPBLocalGoalCandidate *c : choices )
    {
        const float age = AgPB_LocalVisitAge( state, c->waypoint, context.now );
        if ( fresh && age < 45.0f ) continue;
        const float travel = c->pathDistance / farthest;
        float score = random.Range( 0.0f, 70.0f ) + 35.0f * ( 1.0f - std::fabs( travel - preferredFraction ) );
        score -= 100.0f * AgPB_LocalClamp( c->occupancy, 0.0f, 5.0f );
        score -= 100.0f * ( 1.0f - AgPB_LocalClamp( age / 45.0f, 0.0f, 1.0f ) );
        if ( c->avoid ) score -= 30.0f;
        if ( result.tactic == AgPBLocalTactic::Camp && context.sniper && c->sniper ) score += 80.0f;
        if ( score > bestScore ) { bestScore = score; best = c; }
    }
    if ( best != NULL )
    {
        result.waypoint = best->waypoint;
        result.camp = AgPB_LocalMayCamp( result.tactic, *best, context, state, random.Unit() );
    }
    return result;
}

inline const char *AgPB_LocalTacticName( AgPBLocalTactic tactic )
{
    switch ( tactic )
    {
    case AgPBLocalTactic::Defense: return "defense";
    case AgPBLocalTactic::Camp: return "camp";
    case AgPBLocalTactic::Offense: return "offense";
    case AgPBLocalTactic::Objective: return "objective";
    }
    return "patrol";
}

#endif // _INCLUDE_AGPB_LOCAL_PLANNER_H_
