/*
 * AgPB ordinary-mode ground weapon pickup. GPL-3.0-only.
 * Based on SyPB 4c364fb source/basecode.cpp (RateGroundWeapon, FindItem,
 * SetConditions and TASK_PICKUPITEM). The upstream copyright and license
 * are retained in weapon_selection.h. CS-EBOT 2333562 removes this normal
 * weapon pickup behavior from its zombie-mode task flow.
 */

#include <cmath>
#include <const.h>
#include <mathlib/mathlib.h>
#include <engine/ICollideable.h>
#include <gametrace.h>
#include <icvar.h>
#include <in_buttons.h>
#include <iservernetworkable.h>
#include <tier1/convar.h>
#include <tier1/strtools.h>

#include "bot.h"
#include "game_mode.h"
#include "movement_control.h"
#include "waypoint.h"

extern ICvar *icvar;
extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();

namespace {
using AgPBWeaponSelect::Weapon;

constexpr float ScanRange = 400.0f;
constexpr float DropRange = 60.0f;
constexpr float DirectRange = 96.0f;
constexpr float TouchRange = 4.0f;
constexpr float ScanInterval = 2.5f;
constexpr float TaskSeconds = 6.0f;
constexpr float SwitchSeconds = 1.0f;
constexpr float SelectInterval = 0.2f;

bool ReadWeaponOwner( edict_t *weapon, uintp &owner )
{
	const BotNetVar *field = AgPB_FindEntityNetVar( weapon, "m_hOwner" );
	const void *base = AgPB_EntityBase( weapon );
	if ( field == NULL || field->type != DPT_Int || base == NULL ) return false;
	owner = NetVar_GetHandle( base, *field );
	return true;
}

bool GroundWeaponPosition( const BotEngineContext &ctx, edict_t *weapon, Vector &position )
{
	uintp owner;
	if ( !ReadWeaponOwner( weapon, owner ) || NetVar_HandleIsValid( owner ) ||
	     ( AgPB_ReadNetVarInt( weapon, "m_fEffects", EF_NODRAW ) & EF_NODRAW ) != 0 ) return false;
	IServerNetworkable *network = weapon->GetNetworkable();
	IHandleEntity *handle = network != NULL ? network->GetEntityHandle() : NULL;
	ICollideable *collision = handle != NULL ? ctx.pTrace->GetCollideable( handle ) : NULL;
	if ( collision == NULL || ( collision->GetSolidFlags() & FSOLID_TRIGGER ) == 0 ) return false;
	position = collision->GetCollisionOrigin();
	return position.IsValid();
}

int WeaponRank( AgPBLocalPersonality personality, Weapon kind, bool firstMatch )
{
	const auto &order = AgPBWeaponSelect::PersonalityOrder( personality );
	int rank = -1;
	for ( int i = 0; i < AgPBWeaponSelect::WeaponCount; ++i )
	{
		if ( order[i] != kind ) continue;
		rank = i;
		if ( firstMatch ) break;
	}
	return rank;
}

class CPickupTraceFilter : public CTraceFilter
{
public:
	CPickupTraceFilter( IHandleEntity *bot, IHandleEntity *weapon ) : m_Bot( bot ), m_Weapon( weapon ) {}
	bool ShouldHitEntity( IHandleEntity *entity, int ) override { return entity != m_Bot && entity != m_Weapon; }
private:
	IHandleEntity *m_Bot;
	IHandleEntity *m_Weapon;
};

Vector PickupEye( const CAgPB &bot )
{
	float eyeHeight = bot.GetNetVarFloat( "m_vecViewOffset[2]" );
	if ( eyeHeight <= 0.0f ) eyeHeight = bot.GetNetVarVector( "m_vecViewOffset" ).z;
	if ( eyeHeight <= 0.0f )
		eyeHeight = ( ( bot.GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0
		            ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() ) - 4.0f;
	return bot.PlayerInfo()->GetAbsOrigin() + Vector( 0, 0, eyeHeight );
}

bool WeaponVisible( const BotEngineContext &ctx, const CAgPB &bot, edict_t *weapon,
                    const Vector &position )
{
	IServerNetworkable *self = bot.Edict()->GetNetworkable();
	IServerNetworkable *other = weapon->GetNetworkable();
	if ( self == NULL || other == NULL ) return false;
	CPickupTraceFilter filter( self->GetEntityHandle(), other->GetEntityHandle() );
	Ray_t ray;
	ray.Init( PickupEye( bot ), position + Vector( 0, 0, 4 ) );
	trace_t hit;
	ctx.pTrace->TraceRay( ray, MASK_SOLID, &filter, &hit );
	return !hit.startsolid && !hit.allsolid && hit.fraction >= 1.0f;
}

void AimAtPickup( const CAgPB &bot, const Vector &position, float interval, CBotCmd &cmd )
{
	const Vector delta = position - PickupEye( bot );
	if ( !delta.IsValid() || delta.LengthSqr() < 0.01f ) return;
	QAngle target;
	VectorAngles( delta, target );
	const AgPBMovement::View view = AgPBMovement::ApproachView(
		bot.GetNetVarFloat( "m_angEyeAngles[0]", cmd.viewangles.x ),
		bot.GetNetVarFloat( "m_angEyeAngles[1]", cmd.viewangles.y ), target.x, target.y, interval );
	// SyPB aims at the pickup destination. Preserve the Source navigation wish
	// in world space while the head turns toward the ground weapon.
	Vector forward, right;
	AngleVectors( QAngle( 0, cmd.viewangles.y, 0 ), &forward, &right, NULL );
	const Vector wish = forward * cmd.forwardmove + right * cmd.sidemove;
	const AgPBMovement::LocalWish local = AgPBMovement::WorldToLocal( wish.x, wish.y, view.yaw );
	cmd.viewangles.Init( view.pitch, view.yaw, 0 );
	cmd.forwardmove = local.forward;
	cmd.sidemove = local.side;
}

void HoldForWeaponSelect( CBotCmd &cmd )
{
	cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
	cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP );
}
}

void CAgPB::ResetWeaponPickup()
{
	m_uPickupActionToken = 0;
	m_flPickupActionDeadline = m_flPickupNextSelectAt = 0.0f;
	m_bPickupDropIssued = m_bPickupEquipping = false;
	m_uDeclinedPickupHandle = (uintp)INVALID_EHANDLE_INDEX;
	m_flPickupRetryAfter = m_flNextPickupScan = 0.0f;
}

void CAgPB::FinishWeaponPickup( const AgPBTask &task, bool failed )
{
	m_uPickupActionToken = 0;
	if ( !failed || !task.hasEntity ) return;
	m_uDeclinedPickupHandle = (uintp)task.entityHandle;
	m_flPickupRetryAfter = ( gpGlobals != NULL ? gpGlobals->curtime : 0.0f ) + 5.0f;
}

bool CAgPB::CanSeekWeaponPickup( float now ) const
{
	if ( !m_bAutonomous || m_AgentIntent.token != 0 || AgPB_GameMode() != AgPB_MODE_NORMAL ||
	     m_pInfo == NULL || m_Ctx.pEngine == NULL || m_Ctx.pTrace == NULL || AgPB_IsRoundFrozen() ||
	     ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 || GetNetVarInt( "m_bIsVIP" ) != 0 ||
	     GetNetVarInt( "m_bHasShield" ) != 0 || NetVar_HandleIsValid( m_uCombatEnemy ) ) return false;
	const int team = m_pInfo->GetTeamIndex();
	if ( team != AgPB_TEAM_T && team != AgPB_TEAM_CT ) return false;

	// Finish an available purchase sequence before choosing an alternative gun.
	const AgPBPurchaseRound &round = AgPB_PurchaseRound();
	const ConVar *buyTime = icvar != NULL ? icvar->FindVar( "mp_buytime" ) : NULL;
	if ( round.active && m_Purchases.stage != AgPBPurchaseStage::Complete &&
	     buyTime != NULL && now - round.liveAt <= buyTime->GetFloat() * 60.0f )
	{
		const ConVar *buyAnywhere = icvar->FindVar( "mp_buy_anywhere" );
		const int allowed = buyAnywhere != NULL ? buyAnywhere->GetInt() : 0;
		if ( allowed == 1 || allowed == team || GetNetVarInt( "m_bInBuyZone" ) == 1 ) return false;
	}
	return true;
}

bool CAgPB::ReadWeaponPickupSlots( edict_t *(&slots)[2] ) const
{
	slots[0] = slots[1] = NULL;
	const BotNetVar *inventory = FindNetVar( "m_hMyWeapons" );
	const void *base = NetVarBase();
	const uintp self = AgPB_RefEHandle( m_pEdict );
	if ( !NetVar_HandleIsValid( self ) || base == NULL || inventory == NULL || inventory->type != DPT_Array ||
	     inventory->elementType != DPT_Int || inventory->stride != sizeof( uintp ) ) return false;
	for ( int i = 0; i < inventory->elements; ++i )
	{
		edict_t *weapon = HandleToEdict( NetVar_GetArrayHandle( base, *inventory, i ) );
		const Weapon kind = ClassifyWeapon( weapon );
		if ( !AgPBWeaponSelect::Firearm( kind ) ) continue;
		uintp owner;
		if ( !ReadWeaponOwner( weapon, owner ) || owner != self ) return false;
		edict_t *&slot = slots[AgPBWeaponSelect::Pistol( kind )];
		if ( slot != NULL && slot != weapon ) return false;
		slot = weapon;
	}
	return true;
}

bool CAgPB::PlanWeaponPickup( CGlobalVars *globals )
{
	if ( globals == NULL || globals->curtime < m_flNextPickupScan || m_Ladder.Active() ||
	     !CanSeekWeaponPickup( globals->curtime ) || ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ||
	     m_Tasks.Contains( AgPBTaskId::PickupWeapon ) ) return false;
	const AgPBTask &current = m_Tasks.Current();
	if ( current.id != AgPBTaskId::Normal && current.id != AgPBTaskId::MoveToPosition &&
	     current.id != AgPBTaskId::Camp ) return false;
	const float now = globals->curtime;
	m_flNextPickupScan = now + ScanInterval;
	edict_t *slots[2];
	if ( !ReadWeaponPickupSlots( slots ) ) return false;
	const ConVar *deathmatch = icvar != NULL ? icvar->FindVar( "mp_deathmatch" ) : NULL;
	const bool canSwap = m_Ctx.pHelpers != NULL && ( deathmatch == NULL || !deathmatch->GetBool() );
	const Vector here = m_pInfo->GetAbsOrigin();
	const bool crouch = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
	const AgPBLocalPersonality personality = m_Autonomy.personality;
	// SyPB uses the first ground-weapon or held-pistol match and the last
	// held-primary match. Its default personality lists contain duplicates.
	const int ranks[] = { WeaponRank( personality, ClassifyWeapon( slots[0] ), false ),
	                      WeaponRank( personality, ClassifyWeapon( slots[1] ), true ) };
	CAgPBManager &bots = AgPB_Bots();
	CUtlVector<uintp> claims;
	for ( int i = 0; i < bots.Count(); ++i )
	{
		const CAgPB *bot = bots.Get( i );
		const AgPBTask *task = bot != NULL && bot != this ? bot->m_Tasks.Find( AgPBTaskId::PickupWeapon ) : NULL;
		if ( task != NULL && task->hasEntity && task->deadline > now && bot->IsAlive() )
			claims.AddToTail( (uintp)task->entityHandle );
	}
	AgPBTask pickup;
	float bestDistance = ScanRange;
	for ( int index = globals->maxClients + 1; index < globals->maxEntities && index < MAX_EDICTS; ++index )
	{
		edict_t *weapon = m_Ctx.pEngine->PEntityOfEntIndex( index );
		const char *name = weapon != NULL && !weapon->IsFree() ? weapon->GetClassName() : NULL;
		if ( name == NULL || V_strnicmp( name, "weapon_", 7 ) != 0 ) continue;
		const Weapon kind = ClassifyWeapon( weapon );
		if ( !AgPBWeaponSelect::Firearm( kind ) ) continue;
		const bool pistol = AgPBWeaponSelect::Pistol( kind );
		if ( WeaponRank( personality, kind, true ) <= ranks[pistol] || ( slots[pistol] != NULL && !canSwap ) ) continue;
		const uintp handle = AgPB_RefEHandle( weapon );
		if ( !NetVar_HandleIsValid( handle ) || claims.Find( handle ) != claims.InvalidIndex() ||
		     ( handle == m_uDeclinedPickupHandle && now < m_flPickupRetryAfter ) ) continue;
		Vector position;
		if ( !GroundWeaponPosition( m_Ctx, weapon, position ) ) continue;
		const Vector delta = position - here;
		const float distance = delta.Length();
		const float desire = MIN( 60.0f, ( 500.0f - distance ) * 0.2f );
		if ( distance >= bestDistance || desire < current.desire || delta.z > AgPB_HullStandHeight() + 12.0f ||
		     !WeaponVisible( m_Ctx, *this, weapon, position ) ) continue;
		int waypoint = -1;
		if ( delta.Length2D() <= DirectRange && fabsf( delta.z ) < 32.0f )
		{
			if ( !CanWalkToTaskPosition( Vector( position.x, position.y, here.z ), crouch ) ) continue;
		}
		else
		{
			waypoint = PickTaskWaypoint( position );
			const AgPBPath *end = BotWaypoints().Get( waypoint );
			if ( end == NULL || ( end->origin - position ).LengthSqr() > DirectRange * DirectRange ) continue;
		}
		bestDistance = distance;
		pickup.id = AgPBTaskId::PickupWeapon;
		pickup.desire = desire;
		pickup.waypoint = waypoint;
		pickup.entityHandle = handle;
		pickup.hasEntity = pickup.hasPosition = true;
		for ( int axis = 0; axis < 3; ++axis ) pickup.position[axis] = position[axis];
	}
	if ( !pickup.hasEntity ) return false;
	pickup.deadline = now + TaskSeconds;
	pickup.canResume = false;
	return m_Tasks.Push( pickup ) != 0;
}

void CAgPB::UpdateWeaponPickup( CGlobalVars *globals, CBotCmd &cmd )
{
	const AgPBTask task = m_Tasks.Current();
	const float now = globals->curtime;
	const uintp handle = (uintp)task.entityHandle;
	edict_t *weapon = task.hasEntity ? HandleToEdict( handle ) : NULL;
	const Weapon kind = ClassifyWeapon( weapon );
	uintp owner;
	if ( !AgPBWeaponSelect::Firearm( kind ) || !ReadWeaponOwner( weapon, owner ) )
	{
		CompleteCurrentTask( "ground weapon unavailable", true );
		return;
	}
	if ( m_uPickupActionToken != task.token )
	{
		m_uPickupActionToken = task.token;
		m_flPickupActionDeadline = m_flPickupNextSelectAt = 0.0f;
		m_bPickupDropIssued = m_bPickupEquipping = false;
	}
	const bool canSeek = CanSeekWeaponPickup( now );
	uintp active;
	if ( owner == AgPB_RefEHandle( m_pEdict ) && FindOwnedWeapon( weapon->GetClassName() ) == weapon )
	{
		if ( GetNetVarHandleValue( "m_hActiveWeapon", &active ) && active == handle )
		{
			CompleteCurrentTask( "weapon recovered and equipped" );
			return;
		}
		if ( !m_bPickupEquipping )
		{
			m_bPickupEquipping = true;
			m_flPickupActionDeadline = now + SwitchSeconds;
			m_flPickupNextSelectAt = now;
			if ( HasRoute() ) ClearRoute();
		}
		// Source verifies a weapon at pickup without necessarily activating it.
		if ( !canSeek || now >= m_flPickupActionDeadline )
		{
			CompleteCurrentTask( "weapon recovered, equip unconfirmed", true );
			return;
		}
		HoldForWeaponSelect( cmd );
		if ( now >= m_flPickupNextSelectAt )
		{
			cmd.weaponselect = NetVar_HandleEntry( handle );
			m_flPickupNextSelectAt = now + SelectInterval;
		}
		return;
	}
	if ( !canSeek )
	{
		CompleteCurrentTask( "weapon pickup interrupted", true );
		return;
	}
	Vector position;
	if ( NetVar_HandleIsValid( owner ) || !GroundWeaponPosition( m_Ctx, weapon, position ) ||
	     m_bPickupEquipping )
	{
		CompleteCurrentTask( "weapon no longer available for pickup", true );
		return;
	}
	const Vector original( task.position[0], task.position[1], task.position[2] );
	if ( ( position - original ).LengthSqr() > DirectRange * DirectRange )
	{
		CompleteCurrentTask( "ground weapon moved away", true );
		return;
	}
	edict_t *slots[2];
	if ( !ReadWeaponPickupSlots( slots ) )
	{
		CompleteCurrentTask( "weapon inventory unavailable", true );
		return;
	}
	const bool pistol = AgPBWeaponSelect::Pistol( kind );
	edict_t *slot = slots[pistol];
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector delta = position - here;
	if ( slot != NULL )
	{
		const AgPBLocalPersonality personality = m_Autonomy.personality;
		if ( m_bPickupDropIssued || WeaponRank( personality, kind, true ) <=
		     WeaponRank( personality, ClassifyWeapon( slot ), pistol ) )
		{
			CompleteCurrentTask( "weapon slot did not clear or no longer needs an upgrade", true );
			return;
		}
		if ( delta.LengthSqr() < DropRange * DropRange && !m_Ladder.Active() &&
		     ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) != 0 )
		{
			const bool crouch = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
			const bool sameLevel = fabsf( delta.z ) < 32.0f;
			if ( sameLevel && WeaponVisible( m_Ctx, *this, weapon, position ) &&
			     CanWalkToTaskPosition( Vector( position.x, position.y, here.z ), crouch ) )
			{
				const ConVar *deathmatch = icvar != NULL ? icvar->FindVar( "mp_deathmatch" ) : NULL;
				const uintp old = AgPB_RefEHandle( slot );
				if ( m_Ctx.pHelpers == NULL || ( deathmatch != NULL && deathmatch->GetBool() ) ||
				     !NetVar_HandleIsValid( old ) || !GetNetVarHandleValue( "m_hActiveWeapon", &active ) )
				{
					CompleteCurrentTask( "weapon drop unavailable", true );
					return;
				}
				if ( active != old )
				{
					if ( m_flPickupActionDeadline == 0.0f ) m_flPickupActionDeadline = now + SwitchSeconds;
					if ( now >= m_flPickupActionDeadline )
					{
						CompleteCurrentTask( "weapon to drop could not be selected", true );
						return;
					}
					if ( HasRoute() ) ClearRoute();
					HoldForWeaponSelect( cmd );
					AimAtPickup( *this, position, globals->interval_per_tick, cmd );
					if ( now >= m_flPickupNextSelectAt )
					{
						cmd.weaponselect = NetVar_HandleEntry( old );
						m_flPickupNextSelectAt = now + SelectInterval;
					}
					return;
				}
				// The local native command drops the confirmed active gun. The next
				// tick must see the slot empty or the new weapon in our inventory.
				m_bPickupDropIssued = true;
				m_Ctx.pHelpers->ClientCommand( m_pEdict, "drop" );
				if ( HasRoute() ) ClearRoute();
			}
		}
	}
	NavigateTaskTo( position, task.waypoint, TouchRange, globals, cmd );
	if ( m_Tasks.Current().token == task.token && !m_Ladder.Active() && !m_bPendingJumpLaunch &&
	     ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_USE | IN_JUMP ) ) == 0 &&
	     ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) != 0 && WeaponVisible( m_Ctx, *this, weapon, position ) )
		AimAtPickup( *this, position, globals->interval_per_tick, cmd );
}
