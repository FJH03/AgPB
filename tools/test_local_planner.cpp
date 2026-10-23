// Production-policy regression; no SDK or live server is needed.
#include "../src/local_planner.h"
#include <cstdio>
#include <cstdlib>
#include <set>

static void Require( bool condition, const char *message )
{
    if ( !condition ) { std::fprintf( stderr, "FAIL: %s\n", message ); std::exit( 1 ); }
}

static bool Near( float a, float b ) { return std::fabs( a - b ) < 0.001f; }

static AgPBLocalGoalCandidate Candidate( int waypoint, float distance, bool goal = false, bool camp = false )
{
    AgPBLocalGoalCandidate candidate;
    candidate.waypoint = waypoint; candidate.pathDistance = distance;
    candidate.objective = goal; candidate.camp = camp;
    return candidate;
}

int main()
{
    for ( unsigned int seed = 1; seed <= 200; ++seed )
    {
        AgPBLocalPlannerState state;
        AgPB_LocalInitializePersonality( state, AgPBLocalPersonality::Normal, seed );
        Require( state.baseAggression >= 0.4f && state.baseAggression <= 0.7f &&
                 state.baseFear >= 0.4f && state.baseFear <= 0.7f, "SyPB normal personality ranges" );
        AgPB_LocalInitializePersonality( state, AgPBLocalPersonality::Rusher, seed );
        Require( state.baseAggression >= 0.7f && state.baseAggression <= 1.0f &&
                 state.baseFear >= 0.0f && state.baseFear <= 0.4f, "SyPB rusher personality ranges" );
        AgPB_LocalInitializePersonality( state, AgPBLocalPersonality::Careful, seed );
        Require( state.baseAggression >= 0.0f && state.baseAggression <= 0.4f &&
                 state.baseFear >= 0.7f && state.baseFear <= 1.0f, "SyPB careful personality ranges" );
    }

    AgPBLocalGoalContext context;
    context.currentWaypoint = 0; context.now = 100.0f;
    std::vector<AgPBLocalGoalCandidate> candidates;
    candidates.push_back( Candidate( 0, 0.0f ) );
    candidates.push_back( Candidate( 1, 300.0f, false, true ) );
    candidates.push_back( Candidate( 2, 500.0f, true ) );
    candidates.push_back( Candidate( 3, 900.0f ) );
    candidates.push_back( Candidate( 4, 700.0f, true, true ) );
    candidates[1].defendArea = true; candidates[4].sniper = true;
    AgPBLocalPlannerState normal, rusher, careful;
    AgPB_LocalInitializePersonality( normal, AgPBLocalPersonality::Normal, 11 );
    AgPB_LocalInitializePersonality( rusher, AgPBLocalPersonality::Rusher, 11 );
    AgPB_LocalInitializePersonality( careful, AgPBLocalPersonality::Careful, 11 );
    int rushOffense = 0, carefulOffense = 0;
    std::set<int> normalTactics;
    for ( unsigned int seed = 1; seed <= 1000; ++seed )
    {
        const AgPBLocalGoalChoice rush = AgPB_SelectLocalGoal( rusher, context, candidates, seed * 7919 );
        const AgPBLocalGoalChoice caution = AgPB_SelectLocalGoal( careful, context, candidates, seed * 7919 );
        if ( rush.tactic == AgPBLocalTactic::Offense || rush.tactic == AgPBLocalTactic::Objective ) ++rushOffense;
        if ( caution.tactic == AgPBLocalTactic::Offense || caution.tactic == AgPBLocalTactic::Objective ) ++carefulOffense;
        normalTactics.insert( static_cast<int>( AgPB_SelectLocalGoal( normal, context, candidates, seed * 7919 ).tactic ) );
    }
    Require( rushOffense > carefulOffense + 500, "personality changes tactical selection, not only turning speed" );
    Require( normalTactics.size() == 4, "all four source desires can win in normal play" );

    AgPBLocalGoalContext defender = context, attacker = context;
    defender.hostageMap = attacker.hostageMap = true;
    defender.defending = true;
    const AgPBLocalGoalChoice defend = AgPB_SelectLocalGoal( normal, defender, candidates, 91 );
    const AgPBLocalGoalChoice attack = AgPB_SelectLocalGoal( normal, attacker, candidates, 91 );
    Require( Near( defend.desires[0] - attack.desires[0], 60.0f ) &&
             Near( attack.desires[2] - defend.desires[2], 60.0f ), "map-role fear/offense +/-30 source adjustment" );
    defender.bombMap = true; defender.hostageMap = false;
    const AgPBLocalGoalChoice bombDefense = AgPB_SelectLocalGoal( normal, defender, candidates, 91 );
    Require( bombDefense.desires[1] >= defend.desires[1] * 1.5f &&
             bombDefense.desires[1] <= defend.desires[1] * 2.5f, "bomb defenders receive source camp desire multiplier" );

    // Objective patrol must vary reachable authored goals, not repeatedly pick
    // the nearest site. Every seed is a fresh policy invocation, not a mock.
    normal.aggression = 1.0f; normal.fear = 0.0f;
    candidates.clear();
    candidates.push_back( Candidate( 1, 50.0f, true ) );
    candidates.push_back( Candidate( 2, 500.0f, true ) );
    candidates.push_back( Candidate( 3, 900.0f, true ) );
    std::set<int> selected;
    for ( unsigned int seed = 1; seed <= 256; ++seed )
        selected.insert( AgPB_SelectLocalGoal( normal, context, candidates, seed * 7919 ).waypoint );
    Require( selected.size() == 3, "objective choices do not collapse to nearest goal" );

    normal.selectedWaypoint = 2;
    AgPB_LocalRememberGoal( normal, 3, 95.0f );
    candidates.push_back( Candidate( 0, 500.0f, true ) );
    candidates[0].reachable = false;
    candidates.push_back( Candidate( 4, 600.0f, true ) ); candidates.back().passable = false;
    candidates.push_back( Candidate( 5, 700.0f, false, true ) );
    for ( unsigned int seed = 1; seed <= 256; ++seed )
        Require( AgPB_SelectLocalGoal( normal, context, candidates, seed * 7919 ).waypoint == 5,
                 "exclude self/current goal, recent goal, unreachable and team-inaccessible nodes" );
    // A depleted objective category must give way to a fresh ordinary point.
    Require( AgPB_SelectLocalGoal( normal, context, candidates, 91 ).tactic == AgPBLocalTactic::Offense,
             "exhausted tactical category yields to unvisited patrol goals" );
    AgPB_LocalRememberGoal( normal, 5, 96.0f );
    Require( AgPB_SelectLocalGoal( normal, context, candidates, 91 ).waypoint >= 0,
             "small exhausted graphs still permit a reachable revisit" );
    Require( Near( AgPB_LocalVisitAge( normal, 5, 100.0f ), 4.0f ), "arrival history keeps absolute game time" );
    AgPB_LocalRememberGoal( normal, 5, 99.0f );
    Require( Near( AgPB_LocalVisitAge( normal, 5, 100.0f ), 1.0f ), "revisiting refreshes existing ring entry" );

    normal.selectedWaypoint = -1;
    normal.visits[0] = AgPBLocalVisit(); normal.visits[1] = AgPBLocalVisit();
    candidates.clear();
    candidates.push_back( Candidate( 1, 500.0f, true ) );
    candidates.push_back( Candidate( 2, 500.0f, true ) );
    candidates[0].occupancy = 1.0f;
    for ( unsigned int seed = 1; seed <= 256; ++seed )
        Require( AgPB_SelectLocalGoal( normal, context, candidates, seed * 7919 ).waypoint == 2,
                 "friendly body or route/task reservation penalizes an otherwise equal goal" );

    AgPBLocalPlannerState guardState;
    guardState.fear = 0.8f;
    AgPBLocalGoalContext guardContext = context;
    AgPBLocalGoalCandidate ordinary = Candidate( 1, 500.0f );
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Camp, ordinary, guardContext, guardState, 0.0f ),
             "ordinary authored arrivals never become implicit camps" );
    AgPBLocalGoalCandidate authoredCamp = ordinary; authoredCamp.camp = true;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Offense, authoredCamp, guardContext, guardState, 0.0f ) &&
             !AgPB_LocalMayCamp( AgPBLocalTactic::Objective, authoredCamp, guardContext, guardState, 0.0f ),
             "CAMP flag alone does not force every arrival to camp" );
    Require( AgPB_LocalMayCamp( AgPBLocalTactic::Camp, authoredCamp, guardContext, guardState, 0.99f ),
             "camp tactic uses authored camp nodes" );
    AgPBLocalGoalCandidate guardArea = ordinary; guardArea.defendArea = true;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.0f ),
             "objective proximity without a defending map role never grants guard camp" );
    guardContext.bombMap = true; guardContext.defending = true;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Defense, ordinary, guardContext, guardState, 0.0f ),
             "defender role does not grant camp at ordinary patrol arrivals" );
    Require( AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.5f ),
             "defense can guard a real objective area without a CAMP flag" );
    guardState.fear = 0.2f;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.5f ),
             "defensive guard arrival remains conditional on internal fear" );
    guardState.fear = 0.8f; guardArea.occupancy = 0.5f;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.0f ),
             "friendly body or reservation prevents crowded objective guarding" );
    guardArea.occupancy = 0.0f; guardState.lastCampAt = 80.001f;
    Require( !AgPB_LocalCampReady( guardState, 100.0f ) &&
             !AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.0f ),
             "guard cooldown is at least twenty seconds from actual camp start" );
    guardState.lastCampAt = 80.0f;
    Require( AgPB_LocalCampReady( guardState, 100.0f ) &&
             AgPB_LocalMayCamp( AgPBLocalTactic::Defense, guardArea, guardContext, guardState, 0.0f ),
             "twenty-second guard cooldown boundary permits a new decision" );
    AgPBLocalGoalCandidate sniperCamp = ordinary; sniperCamp.sniper = true;
    guardContext.sniper = true;
    Require( AgPB_LocalMayCamp( AgPBLocalTactic::Camp, sniperCamp, guardContext, guardState, 0.5f ),
             "sniper camp tactic accepts an authored sniper node" );
    guardContext.sniper = false;
    Require( !AgPB_LocalMayCamp( AgPBLocalTactic::Camp, sniperCamp, guardContext, guardState, 0.5f ),
             "sniper-only camps require a sniper loadout" );

    // A map without CAMP flags still exercises normal SyPB objective defense;
    // dominant camp desire falls back to this role, not arbitrary arrival camp.
    candidates.clear(); candidates.push_back( guardArea );
    candidates.push_back( Candidate( 2, 1000.0f, true ) );
    guardState.lastCampAt = -1000.0f; guardState.fear = 0.9f; guardState.aggression = 0.1f;
    int naturalGuards = 0;
    for ( unsigned int seed = 1; seed <= 256; ++seed )
    {
        const AgPBLocalGoalChoice choice = AgPB_SelectLocalGoal( guardState, guardContext, candidates, seed * 7919 );
        if ( choice.camp )
        {
            ++naturalGuards;
            Require( choice.waypoint == guardArea.waypoint && choice.tactic == AgPBLocalTactic::Defense,
                     "implicit guard remains confined to defender objective-area tactic" );
        }
    }
    Require( naturalGuards > 0 && naturalGuards < 256, "guarding naturally occurs but remains conditional on no-CAMP maps" );
    guardState.lastCampAt = 90.0f;
    for ( unsigned int seed = 1; seed <= 256; ++seed )
        Require( !AgPB_SelectLocalGoal( guardState, guardContext, candidates, seed * 7919 ).camp,
                 "no-CAMP map policy respects actual guard-start cooldown" );
    std::puts( "local planner tests passed" );
}
