/**
 * AgPB - read-only adaptation of Source ZBot path obstacle detection.
 * Algorithm reference: Valve Corporation / Michael S. Booth (2003),
 * game/server/cstrike/bot/cs_bot_nav.cpp and game/shared/obstacle_pushaway.cpp.
 * AgPB implementation is GPL-3.0; it uses only public engine interfaces.
 */
#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <engine/ICollideable.h>
#include <gametrace.h>
#include <shareddefs.h>
#include <iservernetworkable.h>
#include <toolframework/itoolentity.h>

#include "obstacle.h"
#include "waypoint.h"

// Metamod supplies the globals at Load(); avoid querying the manager again.
extern CGlobalVars *gpGlobals;

static const float kNearDistance = 20.0f;
static const float kFarDistance = 300.0f;
// props_shared.h: PHYSICS_MULTIPLAYER_SOLID; avoid game-internal header dependencies.
static const int kSolidPhysicsMode = 1;
// Source ZBot's vertical probe starts above walkable step-height obstacles.
static const float kProbeBottom = 18.1f;

AgPBObstacle::AgPBObstacle()
{
	found = false;
	entity = NULL;
	handle = (uintp)INVALID_EHANDLE_INDEX;
	hitPosition.Init();
	center.Init();
	aimPosition.Init();
	pathDistance = 0.0f;
	waypoint = -1;
	collisionGroup = -1;
	nearProbe = false;
	expanded = false;
	visible = false;
	healthKnown = false;
	health = 0;
	damageKnown = false;
	damage = 0;
	broken = false;
	breakableCandidate = false;
	kind = "none";
}

static IHandleEntity *EntityHandle( edict_t *entity )
{
	IServerNetworkable *net = entity != NULL ? entity->GetNetworkable() : NULL;
	return net != NULL ? net->GetEntityHandle() : NULL;
}

// Read only explicitly reflected integer scalars. Missing fields stay unknown.
static bool ReadInt( edict_t *entity, const char *name, int &value )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
	if ( field == NULL || field->type != DPT_Int )
		return false;
	value = AgPB_ReadNetVarInt( entity, name );
	return true;
}

static bool ReadKeyInt( const BotEngineContext &ctx, edict_t *entity, const char *key, int &value )
{
	CBaseEntity *base = static_cast<CBaseEntity *>( AgPB_EntityBase( entity ) );
	char text[64];
	if ( ctx.pServerTools == NULL || base == NULL ||
	     !ctx.pServerTools->GetKeyValue( base, key, text, sizeof( text ) ) )
		return false;
	// Reject absent/non-numeric values rather than converting them to zero.
	const char *p = text;
	if ( *p == '-' || *p == '+' ) ++p;
	if ( *p < '0' || *p > '9' ) return false;
	while ( *p >= '0' && *p <= '9' ) ++p;
	if ( *p != '\0' ) return false;
	value = Q_atoi( text );
	return true;
}

static void Classify( const BotEngineContext &ctx, AgPBObstacle &result )
{
	if ( result.entity == NULL )
	{
		result.kind = "world-or-static";
		return;
	}

	IServerNetworkable *net = result.entity->GetNetworkable();
	const char *name = net != NULL ? net->GetClassName() : "";
	if ( name == NULL )
		name = "";

	// GetPlayerInfo assumes CBasePlayer internally: never call it for props.
	CGlobalVars *globals = gpGlobals;
	const int entry = NetVar_HandleEntry( result.handle );
	if ( Q_stricmp( name, "player" ) == 0 ||
	     ( globals != NULL && entry > 0 && entry <= globals->maxClients ) )
	{
		result.kind = "player";
		return;
	}

	if ( Q_strncmp( name, "func_door", 9 ) == 0 || Q_strncmp( name, "prop_door", 9 ) == 0 )
	{
		result.kind = "door";
		return;
	}

	result.healthKnown = ReadInt( result.entity, "m_iHealth", result.health );
	result.damageKnown = ReadInt( result.entity, "m_takedamage", result.damage );
	if ( !result.healthKnown )
		result.healthKnown = ReadKeyInt( ctx, result.entity, "health", result.health );
	const bool surface = Q_stricmp( name, "func_breakable_surf" ) == 0;
	const bool brush = Q_stricmp( name, "func_breakable" ) == 0 || surface;
	const bool physics = Q_stricmp( name, "prop_physics" ) == 0 ||
	                     Q_stricmp( name, "prop_physics_multiplayer" ) == 0 ||
	                     Q_stricmp( name, "prop_physics_override" ) == 0;
	result.kind = physics ? "physics-prop" : ( brush ? "breakable-brush" : "entity" );

	// Source IsBreakableEntity's public / reflected checks. Its damage modifiers,
	// minimum damage, and non-network health / takedamage cannot be assumed.
	const bool groupAllowed = result.collisionGroup == COLLISION_GROUP_PUSHAWAY ||
	                          result.collisionGroup == COLLISION_GROUP_BREAKABLE_GLASS ||
	                          result.collisionGroup == COLLISION_GROUP_NONE;
	int broken = 0;
	result.broken = ReadInt( result.entity, "m_bIsBroken", broken ) && broken != 0;
	// CBreakableSurface::TraceAttack breaks an intact pane independently of
	// its health. Only m_bIsBroken confirms that surface is already shattered.
	if ( !surface && ( brush || physics ) && result.healthKnown && result.health <= 0 )
		result.broken = true;
	int physicsMode = 0, minDamage = 0;
	const bool nonSolidPhysics = Q_stricmp( name, "prop_physics_multiplayer" ) == 0 &&
		ReadKeyInt( ctx, result.entity, "physicsmode", physicsMode ) && physicsMode != kSolidPhysicsMode;
	const bool largeDamageRequired = ReadKeyInt( ctx, result.entity, "minhealthdmg", minDamage ) && minDamage >= 50;
	result.breakableCandidate = ( brush || physics ) && groupAllowed && !result.broken &&
		!nonSolidPhysics && !largeDamageRequired &&
		( !result.healthKnown || ( ( surface || result.health > 0 ) && result.health <= 200 ) ) &&
		( !result.damageKnown || result.damage == DAMAGE_YES );
}

class CObstacleTraceFilter : public CTraceFilter
{
public:
	explicit CObstacleTraceFilter( IHandleEntity *ignore ) : m_Ignore( ignore ) {}
	bool ShouldHitEntity( IHandleEntity *entity, int ) { return entity != m_Ignore; }
private:
	IHandleEntity *m_Ignore;
};

void AgPB_ClassifyObstacle( const BotEngineContext &ctx, edict_t *entity, AgPBObstacle &result )
{
	result = AgPBObstacle();
	if ( entity == NULL || entity->IsFree() || ctx.pTrace == NULL ) return;
	IHandleEntity *handle = EntityHandle( entity );
	ICollideable *collide = handle != NULL ? ctx.pTrace->GetCollideable( handle ) : NULL;
	if ( collide == NULL ) return;
	result.found = true;
	result.entity = entity;
	result.handle = AgPB_RefEHandle( entity );
	result.collisionGroup = collide->GetCollisionGroup();
	VectorTransform( ( collide->OBBMins() + collide->OBBMaxs() ) * 0.5f,
	                 collide->CollisionToWorldTransform(), result.center );
	Classify( ctx, result );
}

bool AgPB_ObstacleAimClear( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                            const Vector &eye, const Vector &point )
{
	if ( ctx.pTrace == NULL || ctx.pGameEnts == NULL || target == NULL ) return false;
	Vector direction = point - eye;
	if ( VectorNormalize( direction ) < 0.01f ) return false;
	Ray_t ray;
	ray.Init( eye, point + direction * 8.0f );
	CObstacleTraceFilter filter( EntityHandle( self ) );
	trace_t trace;
	ctx.pTrace->TraceRay( ray, MASK_SHOT, &filter, &trace );
	return !trace.startsolid && !trace.allsolid && trace.m_pEnt != NULL &&
		ctx.pGameEnts->BaseEntityToEdict( trace.m_pEnt ) == target;
}

bool AgPB_ObstacleMeleeClear( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                            const Vector &eye, const Vector &point )
{
	if ( ctx.pTrace == NULL || ctx.pGameEnts == NULL || target == NULL ) return false;
	Vector direction = point - eye;
	if ( VectorNormalize( direction ) < 0.01f ) return false;
	Ray_t ray;
	// Use the SDK default 40-unit slash reach (cs_weapon_parse.cpp).
	// The engine still performs the actual knife trace and damage via CUserCmd.
	ray.Init( eye, eye + direction * 40.0f );
	CObstacleTraceFilter filter( EntityHandle( self ) );
	trace_t trace;
	ctx.pTrace->TraceRay( ray, MASK_SOLID, &filter, &trace );
	if ( trace.fraction >= 1.0f && !trace.startsolid && !trace.allsolid )
	{
		// Same fallback hull as CKnife::SwingOrStab, only after a clear ray.
		// First hit must still be the target, including for nearby players.
		ray.Init( eye, eye + direction * 40.0f, Vector( -16, -16, -18 ), Vector( 16, 16, 18 ) );
		ctx.pTrace->TraceRay( ray, MASK_SOLID, &filter, &trace );
	}
	return !trace.startsolid && !trace.allsolid && trace.m_pEnt != NULL &&
		ctx.pGameEnts->BaseEntityToEdict( trace.m_pEnt ) == target;
}

static bool SelectObstacleAimPoint( const BotEngineContext &ctx, edict_t *self,
                                    const Vector &eye, const Ray_t &ray,
                                    ICollideable *collide, AgPBObstacle &candidate )
{
	candidate.aimPosition = candidate.center;
	if ( AgPB_ObstacleAimClear( ctx, self, candidate.entity, eye, candidate.aimPosition ) )
		return true;

	candidate.aimPosition = candidate.hitPosition;
	if ( AgPB_ObstacleAimClear( ctx, self, candidate.entity, eye, candidate.aimPosition ) )
		return true;

	// A swept-hull endpos is its reference origin, not the contact point. Move
	// its center into the target's local bounds to find a nearby surface point.
	// OBB bounds only propose an aim point: the bullet trace must still hit the
	// actual entity first, including for non-box collision models.
	Vector localPoint, closestPoint;
	VectorITransform( candidate.hitPosition - ray.m_StartOffset,
	                  collide->CollisionToWorldTransform(), localPoint );
	CalcClosestPointOnAABB( collide->OBBMins(), collide->OBBMaxs(), localPoint, closestPoint );
	VectorTransform( closestPoint, collide->CollisionToWorldTransform(), candidate.aimPosition );
	return AgPB_ObstacleAimClear( ctx, self, candidate.entity, eye, candidate.aimPosition );
}

// EnumerateEntities uses the same solid-edict spatial partition as Source ZBot.
// Enumeration order is unspecified: clip each candidate and select the first
// actual intersection along the segment, rather than the first enumeration hit.
class CObstacleEnumerator : public IEntityEnumerator
{
public:
	CObstacleEnumerator( const BotEngineContext &ctx, edict_t *self, const Vector &eye,
	                     const Ray_t &ray, AgPBObstacle &result, uintp onlyHandle, bool breakablesOnly )
		: m_Ctx( ctx ), m_Self( self ), m_Ignore( EntityHandle( self ) ), m_Eye( eye ),
		  m_Ray( ray ), m_Result( result ), m_Fraction( 1.0f ),
		  m_OnlyHandle( onlyHandle ), m_BreakablesOnly( breakablesOnly ) {}

	bool EnumEntity( IHandleEntity *entity )
	{
		if ( entity == NULL || entity == m_Ignore )
			return true;
		if ( NetVar_HandleIsValid( m_OnlyHandle ) && (uintp)entity->GetRefEHandle().ToInt() != m_OnlyHandle )
			return true;
		ICollideable *collide = m_Ctx.pTrace->GetCollideable( entity );
		if ( collide == NULL || collide->GetSolid() == SOLID_NONE )
			return true;
		const int group = collide->GetCollisionGroup();
		// Keep pushaway props, which ZBot deliberately detects outside player
		// collision traces; omit pickups, debris, and non-solid trigger volumes.
		if ( group == COLLISION_GROUP_WEAPON || group == COLLISION_GROUP_DEBRIS ||
		     group == COLLISION_GROUP_DEBRIS_TRIGGER ||
		     ( group != COLLISION_GROUP_PUSHAWAY &&
		       ( collide->GetSolidFlags() & ( FSOLID_TRIGGER | FSOLID_NOT_SOLID ) ) != 0 ) )
			return true;

		trace_t trace;
		// Physics pushaway objects may be excluded by the player movement mask.
		// We inspect geometry here; the result is a candidate, not a damage verdict.
		m_Ctx.pTrace->ClipRayToCollideable( m_Ray, MASK_ALL, collide, &trace );
		if ( !trace.DidHit() )
			return true;
		const float fraction = trace.startsolid || trace.allsolid ? 0.0f : trace.fraction;
		if ( m_Result.found && fraction >= m_Fraction )
			return true;

		const uintp handle = (uintp)entity->GetRefEHandle().ToInt();
		const int entry = NetVar_HandleEntry( handle );
		edict_t *edict = NetVar_HandleIsValid( handle ) && entry > 0 && entry < MAX_EDICTS
			? m_Ctx.pEngine->PEntityOfEntIndex( entry ) : NULL;
		if ( edict == NULL || edict->IsFree() || AgPB_RefEHandle( edict ) != handle )
			return true;

		AgPBObstacle candidate;
		candidate.found = true;
		candidate.entity = edict;
		candidate.handle = handle;
		candidate.collisionGroup = collide->GetCollisionGroup();
		candidate.hitPosition = trace.endpos;
		VectorTransform( ( collide->OBBMins() + collide->OBBMaxs() ) * 0.5f,
		                 collide->CollisionToWorldTransform(), candidate.center );
		if ( m_BreakablesOnly )
		{
			// As in ZBot's CBotBreakableEnumerator, filter before accepting a
			// candidate. Also skip unsafe shots instead of losing later targets.
			Classify( m_Ctx, candidate );
			if ( !candidate.breakableCandidate ||
			     !SelectObstacleAimPoint( m_Ctx, m_Self, m_Eye, m_Ray, collide, candidate ) )
				return true;
			candidate.visible = true;
		}
		m_Fraction = fraction;
		m_Result = candidate;
		return true;
	}

	float Fraction() const { return m_Fraction; }
private:
	const BotEngineContext &m_Ctx;
	edict_t *m_Self;
	IHandleEntity *m_Ignore;
	const Vector &m_Eye;
	const Ray_t &m_Ray;
	AgPBObstacle &m_Result;
	float m_Fraction;
	uintp m_OnlyHandle;
	bool m_BreakablesOnly;
};

static bool ScanPath( const BotEngineContext &ctx, edict_t *self,
                      const Vector &origin, const Vector &eye,
                      const Vector *points, const int *ids, int count,
                      float distance, bool crouched, bool expanded,
                      AgPBObstacle &result, uintp onlyHandle, bool breakablesOnly )
{
	IHandleEntity *ignore = EntityHandle( self );
	Vector start = origin;
	float walked = 0.0f;
	const float height = crouched ? AgPB_HullDuckHeight() : AgPB_HullStandHeight();
	// ZBot also handles PushawayTouch while crouched or stuck. Approximate
	// that near contact through the public swept hull: include low props at
	// the feet, but keep the long lookahead above walkable step obstacles.
	const bool contact = distance == kNearDistance && ( crouched || expanded );
	const float width = ( expanded || contact ) ? AgPB_HULL_RADIUS : 0.0f;
	const Vector mins( -width, -width, contact ? 0.1f : kProbeBottom );
	const Vector maxs( width, width, height - 1.0f );

	for ( int i = 0; i < count && walked < distance; ++i )
	{
		Vector delta = points[i] - start;
		const float length = delta.Length();
		if ( length < 0.01f )
		{
			start = points[i];
			continue;
		}
		const float segment = MIN( length, distance - walked );
		const Vector end = start + delta * ( segment / length );
		Ray_t ray;
		ray.Init( start, end, mins, maxs );
		CObstacleEnumerator enumerator( ctx, self, eye, ray, result, onlyHandle, breakablesOnly );
		ctx.pTrace->EnumerateEntities( ray, false, &enumerator );

		float fraction = enumerator.Fraction();
		// Diagnostics report the first movement obstruction. Breakable selection
		// uses the verified bullet line instead: a sill below a visible window
		// must not replace that window with a world-hull hit.
		if ( !breakablesOnly )
		{
			CTraceFilterWorldAndPropsOnly worldFilter;
			trace_t worldTrace;
			ctx.pTrace->TraceRay( ray, MASK_PLAYERSOLID, &worldFilter, &worldTrace );
			const float worldFraction = worldTrace.startsolid || worldTrace.allsolid ? 0.0f : worldTrace.fraction;
			if ( !NetVar_HandleIsValid( onlyHandle ) && worldTrace.DidHit() && ( !result.found || worldFraction < fraction ) )
			{
				result.found = true;
				result.entity = NULL;
				result.handle = (uintp)INVALID_EHANDLE_INDEX;
				result.hitPosition = worldTrace.endpos;
				result.center = worldTrace.endpos;
				result.collisionGroup = -1;
				fraction = worldFraction;
			}
		}

		if ( result.found )
		{
			result.pathDistance = walked + fraction * segment;
			result.waypoint = ids != NULL ? ids[i] : -1;
			result.nearProbe = distance == kNearDistance;
			result.expanded = expanded || contact;
			if ( !breakablesOnly )
			{
				Classify( ctx, result );

				Ray_t sight;
				sight.Init( eye, result.center );
				CObstacleTraceFilter sightFilter( ignore );
				trace_t sightTrace;
				ctx.pTrace->TraceRay( sight, MASK_SHOT, &sightFilter, &sightTrace );
				edict_t *hit = sightTrace.m_pEnt != NULL && ctx.pGameEnts != NULL
					? ctx.pGameEnts->BaseEntityToEdict( sightTrace.m_pEnt ) : NULL;
				result.visible = !sightTrace.startsolid && !sightTrace.allsolid &&
					( sightTrace.fraction >= 1.0f || ( result.entity != NULL && hit == result.entity ) );
			}
			return true;
		}
		walked += segment;
		start = end;
	}
	return false;
}

bool AgPB_FindPathObstacle( const BotEngineContext &ctx, edict_t *self,
                           const Vector &origin, const Vector &eye,
                           const Vector *points, const int *ids, int count,
                           bool crouched, bool stuck, AgPBObstacle &result, uintp onlyHandle )
{
	result = AgPBObstacle();
	if ( ctx.pTrace == NULL || ctx.pEngine == NULL || self == NULL ||
	     points == NULL || count <= 0 )
		return false;
	if ( ScanPath( ctx, self, origin, eye, points, ids, count,
	               kNearDistance, crouched, stuck, result, onlyHandle, false ) )
		return true;
	return ScanPath( ctx, self, origin, eye, points, ids, count,
	                 kFarDistance, crouched, false, result, onlyHandle, false );
}

bool AgPB_FindPathBreakable( const BotEngineContext &ctx, edict_t *self,
                            const Vector &origin, const Vector &eye,
                            const Vector *points, const int *ids, int count,
                            bool crouched, bool stuck, AgPBObstacle &result )
{
	result = AgPBObstacle();
	if ( ctx.pTrace == NULL || ctx.pEngine == NULL || ctx.pGameEnts == NULL ||
	     self == NULL || points == NULL || count <= 0 )
		return false;
	const uintp anyHandle = (uintp)INVALID_EHANDLE_INDEX;
	if ( ScanPath( ctx, self, origin, eye, points, ids, count,
	               kNearDistance, crouched, stuck, result, anyHandle, true ) )
		return true;
	return ScanPath( ctx, self, origin, eye, points, ids, count,
	                 kFarDistance, crouched, false, result, anyHandle, true );
}
