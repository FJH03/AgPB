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
	if ( entity == NULL || entity->IsFree() || entity->GetUnknown() == NULL ) return NULL;
	IServerNetworkable *net = entity->GetNetworkable();
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
	if ( result.entity == NULL || result.entity->IsFree() || result.entity->GetUnknown() == NULL )
	{
		result.kind = "world-or-static";
		return;
	}

	const char *name = result.entity->GetClassName();
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
	if ( !surface && brush && result.healthKnown && result.health <= 0 )
		result.broken = true;
	int physicsMode = 0, minDamage = 0;
	const bool nonSolidPhysics = Q_stricmp( name, "prop_physics_multiplayer" ) == 0 &&
		ReadKeyInt( ctx, result.entity, "physicsmode", physicsMode ) && physicsMode != kSolidPhysicsMode;
	const bool largeDamageRequired = ReadKeyInt( ctx, result.entity, "minhealthdmg", minDamage ) && minDamage >= 50;
	// Physics health zero also means an intact, indestructible prop. Missing
	// public health is not proof that shooting this physics object helps.
	result.breakableCandidate = ( brush || ( physics && AgPBPropAvoid::KnownBreakHealth( result.healthKnown, result.health ) ) ) && groupAllowed && !result.broken &&
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

// SyPB CheckTerrain uses full local collision probes and finite collision memory;
// CS-EBOT is the comparison for alternate side/back recovery. Source SDK
// StepMove supplies only the physical step/floor contract, not bot decisions.
static bool NavigationSolid( ICollideable *collide )
{
	if ( collide == NULL || collide->GetSolid() == SOLID_NONE ) return false;
	const int group = collide->GetCollisionGroup();
	if ( group != COLLISION_GROUP_PUSHAWAY &&
		( collide->GetSolidFlags() & ( FSOLID_TRIGGER | FSOLID_NOT_SOLID ) ) != 0 ) return false;
	switch ( group )
	{
	case COLLISION_GROUP_DEBRIS:
	case COLLISION_GROUP_DEBRIS_TRIGGER:
	case COLLISION_GROUP_INTERACTIVE_DEBRIS:
	case COLLISION_GROUP_IN_VEHICLE:
	case COLLISION_GROUP_WEAPON:
	case COLLISION_GROUP_VEHICLE_CLIP:
	case COLLISION_GROUP_DOOR_BLOCKER:
	case COLLISION_GROUP_DISSOLVING:
		return false;
	default:
		return true;
	}
}

static bool IsPhysicsClass( const char *name )
{
	return name != NULL && ( Q_stricmp( name, "prop_physics" ) == 0 ||
		Q_stricmp( name, "prop_physics_multiplayer" ) == 0 || Q_stricmp( name, "prop_physics_override" ) == 0 );
}

class CNavigationHullFilter : public CTraceFilter
{
public:
	CNavigationHullFilter( IEngineTrace *trace, IHandleEntity *self, IHandleEntity *exitContact )
		: m_Trace( trace ), m_Self( self ), m_ExitContact( exitContact ) {}
	bool ShouldHitEntity( IHandleEntity *entity, int )
	{
		return entity != m_Self && entity != m_ExitContact && NavigationSolid( m_Trace->GetCollideable( entity ) );
	}
private:
	IEngineTrace *m_Trace;
	IHandleEntity *m_Self;
	IHandleEntity *m_ExitContact;
};

class CNavigationPropEnumerator : public IEntityEnumerator
{
public:
	CNavigationPropEnumerator( const BotEngineContext &ctx, IHandleEntity *self,
		const Ray_t &ray, trace_t &hit, IHandleEntity *exitContact )
		: m_Ctx( ctx ), m_Self( self ), m_Ray( ray ), m_Hit( hit ), m_ExitContact( exitContact ) {}
	bool EnumEntity( IHandleEntity *entity )
	{
		if ( entity == NULL || entity == m_Self || entity == m_ExitContact ) return true;
		ICollideable *collide = m_Ctx.pTrace->GetCollideable( entity );
		if ( !NavigationSolid( collide ) ) return true;
		const uintp handle = (uintp)entity->GetRefEHandle().ToInt();
		const int index = NetVar_HandleEntry( handle );
		edict_t *edict = NetVar_HandleIsValid( handle ) && index > 0 && index < MAX_EDICTS
			? m_Ctx.pEngine->PEntityOfEntIndex( index ) : NULL;
		if ( edict == NULL || edict->IsFree() || AgPB_RefEHandle( edict ) != handle ||
			( collide->GetCollisionGroup() != COLLISION_GROUP_PUSHAWAY && !IsPhysicsClass( edict->GetClassName() ) ) ) return true;
		trace_t candidate;
		// Pushaway objects can affect the player while missing MASK_PLAYERSOLID.
		// Clip their real collision shape, never their visual/OBB bounding box.
		m_Ctx.pTrace->ClipRayToCollideable( m_Ray, MASK_ALL, collide, &candidate );
		if ( candidate.DidHit() && ( !m_Hit.DidHit() ||
			( !m_Hit.startsolid && !m_Hit.allsolid &&
			( candidate.startsolid || candidate.allsolid || candidate.fraction < m_Hit.fraction ) ) ) ) m_Hit = candidate;
		return true;
	}
private:
	const BotEngineContext &m_Ctx;
	IHandleEntity *m_Self;
	const Ray_t &m_Ray;
	trace_t &m_Hit;
	IHandleEntity *m_ExitContact;
};

static void NavigationHullTrace( const BotEngineContext &ctx, IHandleEntity *self,
                                 const Vector &from, const Vector &to,
                                 const Vector &mins, const Vector &maxs, trace_t &hit, IHandleEntity *exitContact = NULL )
{
	Ray_t ray; ray.Init( from, to, mins, maxs );
	CNavigationHullFilter filter( ctx.pTrace, self, exitContact );
	ctx.pTrace->TraceRay( ray, MASK_PLAYERSOLID, &filter, &hit );
	CNavigationPropEnumerator props( ctx, self, ray, hit, exitContact );
	ctx.pTrace->EnumerateEntities( ray, false, &props );
}

static edict_t *NavigationHitEntity( const BotEngineContext &ctx, const trace_t &hit )
{
	return ctx.pGameEnts != NULL && hit.m_pEnt != NULL ? ctx.pGameEnts->BaseEntityToEdict( hit.m_pEnt ) : NULL;
}

static bool NavigationFloor( const BotEngineContext &ctx, const trace_t &hit )
{
	if ( hit.startsolid || hit.allsolid || hit.fraction >= 1.0f || hit.plane.normal.z < 0.7f ) return false;
	edict_t *entity = NavigationHitEntity( ctx, hit );
	if ( entity == NULL ) return true; // World/static-prop collision.
	if ( entity->IsFree() ) return false;
	const char *name = entity->GetClassName();
	return name != NULL && Q_stricmp( name, "player" ) != 0 && !IsPhysicsClass( name ) &&
		Q_strncmp( name, "func_door", 9 ) != 0 && Q_strncmp( name, "prop_door", 9 ) != 0;
}

bool AgPB_FindGroundProp( const BotEngineContext &ctx, edict_t *self,
                          const Vector &from, const Vector &to, bool crouched,
                          AgPBObstacle &result )
{
	result = AgPBObstacle();
	if ( ctx.pTrace == NULL || ctx.pEngine == NULL || ctx.pGameEnts == NULL ||
		!from.IsValid() || !to.IsValid() || EntityHandle( self ) == NULL ) return false;
	const Vector mins( -AgPB_HULL_RADIUS, -AgPB_HULL_RADIUS, 0 );
	const Vector maxs( AgPB_HULL_RADIUS, AgPB_HULL_RADIUS,
		crouched ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() );
	trace_t hit;
	NavigationHullTrace( ctx, EntityHandle( self ), from + Vector( 0, 0, 0.1f ),
		to + Vector( 0, 0, 0.1f ), mins, maxs, hit );
	if ( !hit.DidHit() ) return false;
	AgPB_ClassifyObstacle( ctx, NavigationHitEntity( ctx, hit ), result );
	if ( !result.found ) { result.found = true; result.kind = "world-or-static"; }
	result.hitPosition = hit.endpos;
	result.pathDistance = ( to - from ).Length2D() * ( hit.startsolid || hit.allsolid ? 0.0f : hit.fraction );
	result.nearProbe = true; result.expanded = true;
	return Q_stricmp( result.kind, "physics-prop" ) == 0;
}

// Pushaway forces can leave a shallow current player/prop overlap. VPhysics
// does not supply fractionleftsolid, so prove exit using its real collision
// shape from the clear end back toward the overlapping start. This exception
// never crosses ordinary solid props and never certifies a raised step path.
static IHandleEntity *NavigationExitContact( const BotEngineContext &ctx, edict_t *contact,
                                             const Vector &from, const Vector &to,
                                             const Vector &mins, const Vector &maxs )
{
	IHandleEntity *handle = EntityHandle( contact );
	ICollideable *collide = handle != NULL ? ctx.pTrace->GetCollideable( handle ) : NULL;
	if ( !NavigationSolid( collide ) || collide->GetCollisionGroup() != COLLISION_GROUP_PUSHAWAY ||
		!IsPhysicsClass( contact->GetClassName() ) ) return NULL;
	const Vector delta = to - from;
	const float distance = delta.Length2D();
	if ( distance < 0.1f || fabsf( delta.z ) > 0.1f ) return NULL;
	Vector center;
	VectorTransform( ( collide->OBBMins() + collide->OBBMaxs() ) * 0.5f,
		collide->CollisionToWorldTransform(), center );
	const Vector away( from.x - center.x, from.y - center.y, 0 );
	if ( DotProduct( delta, away ) < 0 ||
		( away + delta ).Length2DSqr() <= away.Length2DSqr() + 0.01f ) return NULL;
	const Vector begin = from + Vector( 0, 0, 0.1f ), end = to + Vector( 0, 0, 0.1f );
	Ray_t ray; trace_t hit;
	ray.Init( begin, begin, mins, maxs );
	ctx.pTrace->ClipRayToCollideable( ray, MASK_ALL, collide, &hit );
	if ( !hit.startsolid && !hit.allsolid ) return NULL;
	ray.Init( end, end, mins, maxs );
	ctx.pTrace->ClipRayToCollideable( ray, MASK_ALL, collide, &hit );
	if ( hit.DidHit() ) return NULL;
	ray.Init( end, begin, mins, maxs );
	ctx.pTrace->ClipRayToCollideable( ray, MASK_ALL, collide, &hit );
	if ( !hit.DidHit() || hit.startsolid || hit.allsolid || !IsFinite( hit.fraction ) ||
		hit.fraction <= 0 || hit.fraction >= 1 ) return NULL;
	const float exitFraction = 1.0f - hit.fraction;
	if ( exitFraction * distance > AgPB_HULL_RADIUS ) return NULL;
	const float afterExit = MIN( 1.0f, exitFraction + 0.1f / distance );
	ray.Init( begin + delta * afterExit, end, mins, maxs );
	ctx.pTrace->ClipRayToCollideable( ray, MASK_ALL, collide, &hit );
	return hit.DidHit() ? NULL : handle;
}

// A floor witness from this inset box remains within the full player hull
// throughout an adjacent <=8-unit slice (8 inset +8 travel <=16 full radius).
// Full-width endpoint floor probes alone can bridge disjoint tiny supports.
static bool NavigationSupport( const BotEngineContext &ctx, IHandleEntity *self,
                                IHandleEntity *exitContact, const Vector &position, float stepSize )
{
	const float inset = AgPB_HULL_RADIUS * 0.5f;
	const Vector mins( -inset, -inset, 0 ), maxs( inset, inset, 1 );
	trace_t hit;
	NavigationHullTrace( ctx, self, position, position - Vector( 0, 0, stepSize + 0.2f ),
		mins, maxs, hit, exitContact );
	return NavigationFloor( ctx, hit ) && position.z - hit.endpos.z <= stepSize + 0.1f;
}

bool AgPB_CanTraverseGround( const BotEngineContext &ctx, edict_t *self,
                             const Vector &from, const Vector &to, bool crouched, float stepSize, edict_t *exitContact )
{
	IHandleEntity *handle = EntityHandle( self );
	if ( ctx.pTrace == NULL || ctx.pEngine == NULL || ctx.pGameEnts == NULL || handle == NULL ||
		!from.IsValid() || !to.IsValid() || !IsFinite( stepSize ) || stepSize <= 0.0f ||
		( to - from ).Length2D() > 128.0f ) return false;
	const float epsilon = 0.1f;
	const Vector mins( -AgPB_HULL_RADIUS, -AgPB_HULL_RADIUS, 0 );
	const Vector maxs( AgPB_HULL_RADIUS, AgPB_HULL_RADIUS,
		crouched ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() );
	IHandleEntity *exitHandle = NavigationExitContact( ctx, exitContact, from, to, mins, maxs );
	Vector current = from + Vector( 0, 0, epsilon );
	trace_t hit;
	NavigationHullTrace( ctx, handle, current, from - Vector( 0, 0, stepSize ), mins, maxs, hit, exitHandle );
	if ( !NavigationFloor( ctx, hit ) ) return false;
	current = hit.endpos + Vector( 0, 0, epsilon );
	if ( exitHandle != NULL && fabsf( current.z - epsilon - from.z ) > 0.5f ) return false;
	if ( !NavigationSupport( ctx, handle, exitHandle, current, stepSize ) ) return false;
	const Vector delta = to - from;
	const int steps = MAX( 1, (int)ceilf( delta.Length2D() / ( AgPB_HULL_RADIUS * 0.5f ) ) );
	for ( int i = 1; i <= steps; ++i )
	{
		Vector next = from + delta * ( (float)i / steps ); next.z = current.z;
		NavigationHullTrace( ctx, handle, current, next, mins, maxs, hit, exitHandle );
		if ( hit.startsolid || hit.allsolid ) return false;
		if ( hit.fraction < 1.0f )
		{
			if ( exitHandle != NULL ) return false; // The exit proof covers flat separation only.
			NavigationHullTrace( ctx, handle, current, current + Vector( 0, 0, stepSize ), mins, maxs, hit, exitHandle );
			if ( hit.startsolid || hit.allsolid ) return false;
			const Vector raised = hit.endpos; next.z = raised.z;
			NavigationHullTrace( ctx, handle, raised, next, mins, maxs, hit, exitHandle );
			if ( hit.startsolid || hit.allsolid || hit.fraction < 1.0f ) return false;
		}
		NavigationHullTrace( ctx, handle, next, next - Vector( 0, 0, stepSize + 2 * epsilon ), mins, maxs, hit, exitHandle );
		if ( !NavigationFloor( ctx, hit ) || fabsf( hit.endpos.z + epsilon - current.z ) > stepSize + epsilon ) return false;
		current = hit.endpos + Vector( 0, 0, epsilon );
		if ( exitHandle != NULL && fabsf( current.z - epsilon - from.z ) > 0.5f ) return false;
		if ( !NavigationSupport( ctx, handle, exitHandle, current, stepSize ) ) return false;
	}
	return true;
}
