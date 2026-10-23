/**
 * AgPB - read-only normal-mode objective observation.
 *
 * SyPB (HsK-Dev Blog / Yet Another POD-Bot Development Team) is the
 * normal-mode behavior reference. CS-EBOT (Yet Another POD-Bot Development
 * Team) was checked for the corresponding objective entities. This observer
 * contains no GoldSrc entity access or zombie-specific branches.
 *
 * GPL-3.0 project code. Entity fields and their meaning are checked against
 * Valve's Source SDK SendTables; no game DLL symbols are linked.
 */
#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <engine/ICollideable.h>
#include <iservernetworkable.h>
#include <convar.h>

#include "bot.h"
#include "objectives.h"
#include "bomb_escape_policy.h"

static const float kObjectiveRefreshInterval = 0.1f;
static const uintp kInvalidHandle = (uintp)INVALID_EHANDLE_INDEX;

ConVar agpb_bomb_blast_radius( "agpb_bomb_blast_radius", "0", FCVAR_GAMEDLL,
	"C4 damage-radius override in units; 0 uses map entity data. Synchronize with runtime map_setbombradius changes.",
	true, 0.0f, false, 0.0f );

const char *AgPB_BombRadiusSourceName( AgPBBombRadiusSource source )
{
	switch ( source )
	{
	case AGPB_BOMB_RADIUS_MAP_DEFAULT: return "map-default";
	case AGPB_BOMB_RADIUS_MAP_ENTITY: return "map-entity";
	case AGPB_BOMB_RADIUS_OVERRIDE: return "override";
	default: return "unknown";
	}
}

// CMapInfo only implements a KeyValue setter; bombradius is neither a
// SendProp nor a datadesc keyfield, so IServerTools::GetKeyValue cannot read
// its live value. Use the public pristine entity lump, never a member offset.
// mapinfo.cpp: first info_map_parameters wins, default 500, integer map key
// capped at 2048. weapon_c4.cpp applies damage out to that value * 3.5.
// Administrators changing the live value must set the explicit override too.
static bool ReadMapBombRadius( IVEngineServer *engine, float &radius, AgPBBombRadiusSource &source )
{
	const char *cursor = engine != NULL ? engine->GetMapEntitiesString() : NULL;
	if ( cursor == NULL || cursor[0] == '\0' ) return false;
	char token[2048], key[2048];
	while ( cursor != NULL )
	{
		token[0] = '\0';
		cursor = engine->ParseFile( cursor, token, sizeof( token ) );
		if ( token[0] == '\0' ) break;
		if ( V_strcmp( token, "{" ) != 0 ) return false;
		bool mapInfo = false, hasRadius = false, closed = false;
		int damage = 500;
		while ( cursor != NULL )
		{
			key[0] = '\0';
			cursor = engine->ParseFile( cursor, key, sizeof( key ) );
			if ( V_strcmp( key, "}" ) == 0 ) { closed = true; break; }
			if ( cursor == NULL || key[0] == '\0' ) return false;
			token[0] = '\0';
			cursor = engine->ParseFile( cursor, token, sizeof( token ) );
			if ( cursor == NULL || V_strcmp( token, "}" ) == 0 ) return false;
			if ( V_strcmp( key, "classname" ) == 0 ) mapInfo = V_strcmp( token, "info_map_parameters" ) == 0;
			else if ( V_strcmp( key, "bombradius" ) == 0 ) { damage = V_atoi( token ); hasRadius = true; }
		}
		if ( !closed ) return false;
		if ( mapInfo )
		{
			if ( damage < 0 ) return false;
			radius = MIN( damage, 2048 ) * 3.5f;
			source = hasRadius ? AGPB_BOMB_RADIUS_MAP_ENTITY : AGPB_BOMB_RADIUS_MAP_DEFAULT;
			return true;
		}
	}
	radius = 500.0f * 3.5f;
	source = AGPB_BOMB_RADIUS_MAP_DEFAULT;
	return true;
}

AgPBObjectiveRegion::AgPBObjectiveRegion()
{
	handle = kInvalidHandle;
	center.Init();
	worldMins.Init();
	worldMaxs.Init();
	positionKnown = false;
	boundsKnown = false;
	pointEntity = false;
}

AgPBObjectiveHostage::AgPBObjectiveHostage()
{
	handle = kInvalidHandle;
	position.Init();
	positionKnown = false;
	aliveKnown = false;
	alive = false;
	rescuedKnown = false;
	rescued = false;
	leaderKnown = false;
	leader = kInvalidHandle;
}

AgPBObjectiveBomb::AgPBObjectiveBomb()
{
	state = AGPB_BOMB_UNKNOWN;
	handle = kInvalidHandle;
	owner = kInvalidHandle;
	ownerKnown = false;
	position.Init();
	positionKnown = false;
	tickingKnown = false;
	ticking = false;
	blowTimeKnown = false;
	blowTime = 0.0f;
	defuseCountdownKnown = false;
	defuseCountdown = 0.0f;
	plantedObserved = false;
}

AgPBObjectiveSnapshot::AgPBObjectiveSnapshot()
{
	mapFlags = AGPB_MAP_NO_OBJECTIVE;
	observedAt = 0.0f;
	roundEpoch = 0;
	bombBlastRadiusKnown = false;
	bombBlastRadius = 0.0f;
	bombRadiusSource = AGPB_BOMB_RADIUS_UNKNOWN;
}

AgPBObjectiveWorld::AgPBObjectiveWorld()
{
	Reset();
}

void AgPBObjectiveWorld::Reset()
{
	m_Snapshot = AgPBObjectiveSnapshot();
	m_MapFlags = AGPB_MAP_NO_OBJECTIVE;
	m_RoundEpoch = 0;
	m_NextRefresh = 0.0f;
	m_BombRadiusParsed = false;
	m_MapBombRadiusKnown = false;
	m_MapBombBlastRadius = 0.0f;
	m_MapBombRadiusSource = AGPB_BOMB_RADIUS_UNKNOWN;
}

void AgPBObjectiveWorld::OnRoundStart()
{
	++m_RoundEpoch;
	m_Snapshot = AgPBObjectiveSnapshot();
	m_Snapshot.mapFlags = m_MapFlags;
	m_Snapshot.roundEpoch = m_RoundEpoch;
	m_NextRefresh = 0.0f;
}

AgPBObjectiveWorld &BotObjectives()
{
	static AgPBObjectiveWorld world;
	return world;
}

static bool ClassIs( const char *actual, const char *wanted )
{
	return actual != NULL && V_strcmp( actual, wanted ) == 0;
}

// Every stored identity is an engine ref handle. A slot alone is insufficient
// because a freed edict can be recycled for a different entity next tick.
static edict_t *ResolveHandle( const BotEngineContext &ctx, uintp handle,
                              const CGlobalVars *globals )
{
	if ( ctx.pEngine == NULL || globals == NULL || !NetVar_HandleIsValid( handle ) )
		return NULL;
	const int entry = NetVar_HandleEntry( handle );
	if ( entry <= 0 || entry >= globals->maxEntities || entry >= MAX_EDICTS )
		return NULL;
	edict_t *entity = ctx.pEngine->PEntityOfEntIndex( entry );
	return entity != NULL && !entity->IsFree() && AgPB_RefEHandle( entity ) == handle
		? entity : NULL;
}

static ICollideable *EntityCollideable( const BotEngineContext &ctx, edict_t *entity )
{
	if ( ctx.pTrace == NULL || entity == NULL || entity->IsFree() || entity->GetUnknown() == NULL )
		return NULL;
	IServerNetworkable *networkable = entity->GetNetworkable();
	IHandleEntity *handleEntity = networkable != NULL ? networkable->GetEntityHandle() : NULL;
	return handleEntity != NULL ? ctx.pTrace->GetCollideable( handleEntity ) : NULL;
}

static bool EntityPosition( const BotEngineContext &ctx, edict_t *entity, Vector &position )
{
	ICollideable *collideable = EntityCollideable( ctx, entity );
	if ( collideable != NULL )
	{
		position = collideable->GetCollisionOrigin();
		if ( position.IsValid() )
			return true;
	}
	// A point entity without collision can still expose its origin in the
	// reflected base entity. Brush regions require collision bounds below.
	const BotNetVar *origin = AgPB_FindEntityNetVar( entity, "m_vecOrigin" );
	if ( origin == NULL || origin->type != DPT_Vector )
		return false;
	position = AgPB_ReadNetVarVector( entity, "m_vecOrigin" );
	return position.IsValid();
}

static bool ReadInt( edict_t *entity, const char *name, int &value )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
	if ( field == NULL || field->type != DPT_Int )
		return false;
	value = AgPB_ReadNetVarInt( entity, name );
	return true;
}

static bool ReadFloat( edict_t *entity, const char *name, float &value )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
	if ( field == NULL || field->type != DPT_Float )
		return false;
	value = AgPB_ReadNetVarFloat( entity, name );
	return true;
}

static bool ReadHandle( const BotEngineContext &ctx, edict_t *entity, const char *name, uintp &value )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
	void *base = AgPB_EntityBase( entity );
	if ( base == NULL || field == NULL || field->type != DPT_Int )
		return false;
	// Ask the field's own SendProxy for its public network representation.
	// Null player references may contain an index-zero sentinel in memory.
	// Resolve through the engine and retain the full ref handle, never the
	// reduced network serial or an unverified raw pointer across frames.
	const unsigned int encoded = (unsigned int)NetVar_GetInt( base, *field );
	value = kInvalidHandle;
	if ( encoded == (unsigned int)INVALID_NETWORKED_EHANDLE_VALUE ) return true;
	const unsigned int entryMask = ( 1u << MAX_EDICT_BITS ) - 1u;
	const int entry = (int)( encoded & entryMask );
	if ( entry == 0 ) return true; // Neither an inventory owner nor a hostage leader is worldspawn.
	if ( ctx.pEngine == NULL || entry >= MAX_EDICTS ) return false;
	edict_t *target = ctx.pEngine->PEntityOfEntIndex( entry );
	if ( target == NULL || target->IsFree() ) return false;
	const uintp handle = AgPB_RefEHandle( target );
	if ( !NetVar_HandleIsValid( handle ) ) return false;
	const unsigned int serialMask = ( 1u << NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS ) - 1u;
	const unsigned int resolved = (unsigned int)NetVar_HandleEntry( handle ) |
		( ( (unsigned int)NetVar_HandleSerial( handle ) & serialMask ) << MAX_EDICT_BITS );
	if ( resolved != encoded ) return false;
	value = handle;
	return true;
}

static AgPBObjectiveRegion ReadRegion( const BotEngineContext &ctx,
                                       edict_t *entity, bool pointEntity )
{
	AgPBObjectiveRegion region;
	region.handle = AgPB_RefEHandle( entity );
	region.pointEntity = pointEntity;
	ICollideable *collideable = EntityCollideable( ctx, entity );
	if ( collideable != NULL )
	{
		if ( !pointEntity )
		{
			collideable->WorldSpaceSurroundingBounds( &region.worldMins, &region.worldMaxs );
			region.boundsKnown = region.worldMins.IsValid() && region.worldMaxs.IsValid() &&
				region.worldMins.x <= region.worldMaxs.x &&
				region.worldMins.y <= region.worldMaxs.y &&
				region.worldMins.z <= region.worldMaxs.z;
			if ( region.boundsKnown )
			{
				region.center = ( region.worldMins + region.worldMaxs ) * 0.5f;
				region.positionKnown = true;
			}
		}
		if ( !region.positionKnown )
		{
			region.center = collideable->GetCollisionOrigin();
			region.positionKnown = region.center.IsValid();
		}
	}
	if ( !region.positionKnown )
		region.positionKnown = EntityPosition( ctx, entity, region.center );
	return region;
}

static AgPBObjectiveHostage ReadHostage( const BotEngineContext &ctx, edict_t *entity )
{
	AgPBObjectiveHostage hostage;
	hostage.handle = AgPB_RefEHandle( entity );
	hostage.positionKnown = EntityPosition( ctx, entity, hostage.position );
	int lifeState = 0, health = 0, rescued = 0;
	const bool lifeKnown = ReadInt( entity, "m_lifeState", lifeState );
	const bool healthKnown = ReadInt( entity, "m_iHealth", health );
	hostage.aliveKnown = lifeKnown || healthKnown;
	hostage.alive = ( !lifeKnown || lifeState == 0 ) && ( !healthKnown || health > 0 );
	hostage.rescuedKnown = ReadInt( entity, "m_isRescued", rescued );
	hostage.rescued = hostage.rescuedKnown && rescued != 0;
	hostage.leaderKnown = ReadHandle( ctx, entity, "m_leader", hostage.leader );
	return hostage;
}

static AgPBObjectiveBomb ReadWeaponC4( const BotEngineContext &ctx,
                                       const CGlobalVars *globals, edict_t *entity )
{
	AgPBObjectiveBomb bomb;
	bomb.handle = AgPB_RefEHandle( entity );
	bomb.ownerKnown = ReadHandle( ctx, entity, "m_hOwner", bomb.owner );
	if ( !bomb.ownerKnown )
		return bomb;
	if ( NetVar_HandleIsValid( bomb.owner ) )
	{
		edict_t *owner = ResolveHandle( ctx, bomb.owner, globals );
		if ( owner == NULL || NetVar_HandleEntry( bomb.owner ) > globals->maxClients )
			return bomb;
		bomb.state = AGPB_BOMB_CARRIED;
		bomb.positionKnown = EntityPosition( ctx, owner, bomb.position );
	}
	else
	{
		bomb.state = AGPB_BOMB_DROPPED;
		bomb.positionKnown = EntityPosition( ctx, entity, bomb.position );
	}
	return bomb;
}

static AgPBObjectiveBomb ReadPlantedC4( const BotEngineContext &ctx, edict_t *entity )
{
	AgPBObjectiveBomb bomb;
	bomb.handle = AgPB_RefEHandle( entity );
	bomb.positionKnown = EntityPosition( ctx, entity, bomb.position );
	int ticking = 0;
	bomb.tickingKnown = ReadInt( entity, "m_bBombTicking", ticking );
	bomb.ticking = bomb.tickingKnown && ticking != 0;
	bomb.blowTimeKnown = ReadFloat( entity, "m_flC4Blow", bomb.blowTime );
	bomb.defuseCountdownKnown = ReadFloat( entity, "m_flDefuseCountDown", bomb.defuseCountdown );
	// An absent ticking field is unknown, not evidence that the bomb stopped.
	// The blow time is assigned when arming completes; before that, a false
	// ticking value does not establish that this entity was resolved.
	bomb.state = bomb.ticking ? AGPB_BOMB_PLANTED :
		( bomb.tickingKnown && bomb.blowTimeKnown && bomb.blowTime > 0.0f
		  ? AGPB_BOMB_RESOLVED : AGPB_BOMB_UNKNOWN );
	// CPlantedC4::ShootSatchelCharge creates this class when planting finishes.
	// Missing SendProps on a planted charge must retain an unknown threat.
	bomb.plantedObserved = true;
	return bomb;
}

void AgPBObjectiveWorld::Refresh( const BotEngineContext &ctx, const CGlobalVars *globals )
{
	if ( ctx.pEngine == NULL || globals == NULL )
		return;
	const float now = globals->curtime;
	if ( now < m_Snapshot.observedAt )
		Reset();
	if ( now < m_NextRefresh )
		return;
	m_NextRefresh = now + kObjectiveRefreshInterval;
	if ( !m_BombRadiusParsed )
	{
		m_MapBombRadiusKnown = ReadMapBombRadius( ctx.pEngine, m_MapBombBlastRadius, m_MapBombRadiusSource );
		m_BombRadiusParsed = true;
	}

	AgPBObjectiveSnapshot next;
	next.observedAt = now;
	next.roundEpoch = m_RoundEpoch;
	next.bomb.state = AGPB_BOMB_NONE;
	next.bombBlastRadiusKnown = m_MapBombRadiusKnown;
	next.bombBlastRadius = m_MapBombBlastRadius;
	next.bombRadiusSource = m_MapBombRadiusSource;
	const float radiusOverride = agpb_bomb_blast_radius.GetFloat();
	if ( IsFinite( radiusOverride ) && radiusOverride > 0.0f )
	{
		next.bombBlastRadiusKnown = true;
		next.bombBlastRadius = radiusOverride;
		next.bombRadiusSource = AGPB_BOMB_RADIUS_OVERRIDE;
	}
	bool sawWeapon = false, sawPlanted = false;
	const int maxEntities = globals->maxEntities < MAX_EDICTS
		? globals->maxEntities : MAX_EDICTS;
	for ( int index = 1; index < maxEntities; ++index )
	{
		edict_t *entity = ctx.pEngine->PEntityOfEntIndex( index );
		if ( entity == NULL || entity->IsFree() )
			continue;
		// DetachEdict calls SetEdict(NULL, false): IsFree can still be false
		// while the cached networkable is stale. The public edict accessor
		// checks GetUnknown before calling it (SDK edict.h / ServerNetworkProperty.cpp).
		const char *name = entity->GetClassName();
		if ( name == NULL || name[0] == '\0' )
			continue;

		if ( ClassIs( name, "func_bomb_target" ) || ClassIs( name, "info_bomb_target" ) )
		{
			m_MapFlags |= AGPB_MAP_BOMB;
			next.bombSites.AddToTail( ReadRegion( ctx, entity, name[0] == 'i' ) );
		}
		else if ( ClassIs( name, "func_hostage_rescue" ) || ClassIs( name, "info_hostage_rescue" ) )
		{
			m_MapFlags |= AGPB_MAP_HOSTAGE;
			next.rescueZones.AddToTail( ReadRegion( ctx, entity, name[0] == 'i' ) );
		}
		else if ( ClassIs( name, "hostage_entity" ) )
		{
			m_MapFlags |= AGPB_MAP_HOSTAGE;
			next.hostages.AddToTail( ReadHostage( ctx, entity ) );
		}
		else if ( ClassIs( name, "weapon_c4" ) )
		{
			if ( !sawWeapon )
			{
				AgPBObjectiveBomb candidate = ReadWeaponC4( ctx, globals, entity );
				if ( !sawPlanted ) next.bomb = candidate;
				sawWeapon = true;
			}
		}
		else if ( ClassIs( name, "planted_c4" ) )
		{
			AgPBObjectiveBomb candidate = ReadPlantedC4( ctx, entity );
			if ( !sawPlanted || candidate.state == AGPB_BOMB_PLANTED )
				next.bomb = candidate;
			sawPlanted = true;
		}
	}
	// A temporarily missing entity or unreadable ticking field is not proof
	// of a resolved threat. Preserve its last identity/position until a real
	// resolved observation or OnRoundStart; unknown time means retreat now.
	if ( m_Snapshot.bomb.plantedObserved && m_Snapshot.bomb.state != AGPB_BOMB_RESOLVED &&
	     next.bomb.state != AGPB_BOMB_PLANTED && next.bomb.state != AGPB_BOMB_RESOLVED )
	{
		// Unknown ticking may coexist with a newly recovered position/handle.
		next.bomb = AgPB_BombMergeUnknownObservation( m_Snapshot.bomb, next.bomb,
			NetVar_HandleIsValid( m_Snapshot.bomb.handle ), NetVar_HandleIsValid( next.bomb.handle ) );
		next.bomb.state = AGPB_BOMB_UNKNOWN;
		next.bomb.tickingKnown = next.bomb.blowTimeKnown = next.bomb.defuseCountdownKnown = false;
	}
	next.mapFlags = m_MapFlags;
	m_Snapshot = next;
}
