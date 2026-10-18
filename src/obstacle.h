/**
 * AgPB - read-only path obstacle inspection.
 *
 * Algorithm reference: Valve / Michael S. Booth, Source CCSBot
 * cs_bot_nav.cpp (FindEntitiesOnPath / BreakablesCheck) and
 * obstacle_pushaway.cpp (IsBreakableEntity).
 * Independently implemented against public SDK interfaces; no CCSBot AI calls.
 * See also SyPB FindBreakable and CS-EBOT CheckTouchEntity.
 */
#ifndef _INCLUDE_AGPB_OBSTACLE_H_
#define _INCLUDE_AGPB_OBSTACLE_H_

#include "bot.h"

struct AgPBObstacle
{
	AgPBObstacle();

	bool found;
	edict_t *entity;             // Only valid during the calling server tick.
	uintp handle;
	Vector hitPosition;
	Vector center;
	Vector aimPosition;         // FindPathBreakable: a point with a verified shot to this entity.
	float pathDistance;
	int waypoint;
	int collisionGroup;
	bool nearProbe;
	bool expanded;
	bool visible;
	bool healthKnown;
	int health;
	bool damageKnown;
	int damage;
	bool broken;                // Surface: m_bIsBroken; ordinary brush/prop: also known health <= 0.
	bool breakableCandidate;     // Not proof that bullets can destroy it.
	const char *kind;
};

// Refresh identity, geometry and readable breakability properties each tick.
void AgPB_ClassifyObstacle( const BotEngineContext &ctx, edict_t *entity, AgPBObstacle &result );
// Strict bullet line of sight: first hit must be the specified entity.
bool AgPB_ObstacleAimClear( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                            const Vector &eye, const Vector &point );
// SDK-style short ray / fallback hull; first hit must remain this target.
bool AgPB_ObstacleMeleeClear( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                            const Vector &eye, const Vector &point );

// points are the remaining manual route, beginning at the next waypoint.
// ids may be NULL. No graph changes, entity writes, or bot inputs are generated.
bool AgPB_FindPathObstacle( const BotEngineContext &ctx, edict_t *self,
                           const Vector &origin, const Vector &eye,
                           const Vector *points, const int *ids, int count,
                           bool crouched, bool stuck, AgPBObstacle &result,
                           uintp onlyHandle = (uintp)INVALID_EHANDLE_INDEX );

// Find a breakable candidate with a clear shot, near first and then farther along
// the route. Non-breakable/occluded entities do not end the search. A window sill
// may block the movement hull without blocking the shot. Success sets visible
// and aimPosition; firing must recheck the shot each tick. Use FindPathObstacle
// with onlyHandle for active-target geometry checks, even while its shot is blocked.
bool AgPB_FindPathBreakable( const BotEngineContext &ctx, edict_t *self,
                            const Vector &origin, const Vector &eye,
                            const Vector *points, const int *ids, int count,
                            bool crouched, bool stuck, AgPBObstacle &result );

#endif
