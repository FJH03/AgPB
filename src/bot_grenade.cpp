// SPDX-License-Identifier: GPL-3.0-only
// Throw decisions reference SyPB 4c364fb basecode.cpp and CS-EBOT 2333562
// basecode.cpp / source/ssm/throw*.cpp, Copyright (c) 2003-2009 Yet Another
// POD-Bot Development Team and Copyright (c) 2003-2023 HsK-Dev Blog (SyPB).
// AgPB solves and traces the native Source arc before submitting the throw.
#include <cfloat>
#include <const.h>
#include <in_buttons.h>
#include <iservernetworkable.h>
#include <mathlib/mathlib.h>
#include <vstdlib/random.h>
#include "plugin.h"
#include "bot.h"
#include "grenade_trajectory.h"
#include "movement_control.h"
#include "waypoint.h"

extern ConVar agpb_combat;
extern bool AgPB_IsRoundFrozen();

namespace
{
#if !defined( AGPB_GAME_CSGO )
ConVarRef &NativeGrenadeRadio()
{
	static ConVarRef cvar( "sv_ignoregrenaderadio", true );
	return cvar;
}

struct GrenadeChoice
{
	const char *classname;
	float minRange, maxRange, radius, flight;
	int normalChance, zmChance;
};

const GrenadeChoice grenades[] = {
	{ "weapon_hegrenade",    600.0f, 1000.0f, 350.0f, 1.45f, 45, 36 },
	{ "weapon_flashbang",    320.0f,  800.0f, 256.0f, 1.40f, 60, 36 },
	{ "weapon_smokegrenade", 400.0f, 1000.0f, 256.0f, 1.60f, 45, 10 },
};

bool GrenadeOwnedBy( edict_t *weapon, edict_t *player )
{
	if ( weapon == NULL || weapon->IsFree() ) return false;
	const BotNetVar *field = AgPB_FindEntityNetVar( weapon, "m_hOwner" );
	const void *base = AgPB_EntityBase( weapon );
	return field != NULL && field->type == DPT_Int && base != NULL &&
		NetVar_GetHandle( base, *field ) == AgPB_RefEHandle( player );
}

class GrenadeTrace : public CTraceFilter
{
public:
	explicit GrenadeTrace( IHandleEntity *owner ) : m_Owner( owner ) {}
	bool ShouldHitEntity( IHandleEntity *entity, int ) override { return entity != m_Owner; }
private:
	IHandleEntity *m_Owner;
};

bool SolveThrow( const CAgPB &bot, const BotEngineContext &ctx, const Vector &velocity,
	const Vector &landing, float maxFlight, QAngle &aim, Vector &effect, float &padding )
{
	const Vector offset = bot.GetNetVarVector( "m_vecViewOffset" );
	IServerNetworkable *network = bot.Edict()->GetNetworkable();
	if ( !offset.IsValid() || offset.z <= 0.0f || ctx.pTrace == NULL || network == NULL ) return false;
	IHandleEntity *owner = network->GetEntityHandle();
	static ConVarRef gravity( "sv_gravity", true );
	if ( owner == NULL || !gravity.IsValid() ) return false;
	const Vector eye = bot.PlayerInfo()->GetAbsOrigin() + offset;
	// [game-diff] csczs and CS:S use the native fixed-strength Source arc.
	const AgPBGrenade::Input input = {
		{ eye.x, eye.y, eye.z }, { velocity.x, velocity.y, velocity.z }, { landing.x, landing.y, landing.z },
		gravity.GetFloat() * 0.4f, maxFlight
	};
	GrenadeTrace filter( owner );
	const auto traceHull = [&]( AgPBGrenade::Vec3 from, AgPBGrenade::Vec3 to, trace_t &hit ) {
		Ray_t ray;
		ray.Init( Vector( from.x, from.y, from.z ), Vector( to.x, to.y, to.z ),
			Vector( -2, -2, -2 ), Vector( 2, 2, 2 ) );
		ctx.pTrace->TraceRay( ray, MASK_SOLID, &filter, &hit );
	};
	AgPBGrenade::Solution solution;
	if ( !AgPBGrenade::Solve( input, solution, [&]( AgPBGrenade::Vec3 from, AgPBGrenade::Vec3 to ) {
		trace_t hit;
		traceHull( from, to, hit );
		return !hit.DidHit();
	} ) ) return false;

	// The aim point can be reached before the stock fuse. Inspect the
	// continuation up to the first impact or timed detonation window.
	constexpr float horizon = 1.70f;
	AgPBGrenade::State state = AgPBGrenade::Evaluate( input, solution, solution.flightTime );
	effect.Init( state.position.x, state.position.y, state.position.z );
	padding = 0.0f;
	for ( float time = solution.flightTime; time < horizon; )
	{
		const float next = MIN( time + 0.05f, horizon );
		const AgPBGrenade::State after = AgPBGrenade::Evaluate( input, solution, next );
		trace_t hit;
		traceHull( state.position, after.position, hit );
		if ( hit.startsolid || hit.allsolid ) return false;
		effect = hit.endpos;
		if ( hit.DidHit() )
		{
			// Leave room for post-impact travel without predicting later
			// ricochets or server-side custom grenade modes.
			const Vector incoming( after.velocity.x, after.velocity.y, after.velocity.z );
			padding = incoming.Length() * 0.45f * ( horizon - time );
			break;
		}
		state = after;
		time = next;
	}
	if ( !effect.IsValid() || ( effect - landing ).LengthSqr() > 160.0f * 160.0f ) return false;
	aim.Init( solution.pitch, solution.yaw, 0.0f );
	return true;
}
#endif
}

void CAgPB::ResetGrenadeThrow()
{
#if !defined( AGPB_GAME_CSGO )
	// Stock Source prevents holstering a grenade with the pin pulled.
	// Preserve the committed aim across an intervening task reset.
	if ( ( m_Grenade.phase == GrenadePhase::Prime || m_Grenade.phase == GrenadePhase::Release ) && IsAlive() )
	{
		uintp active = (uintp)INVALID_EHANDLE_INDEX;
		GetNetVarHandleValue( "m_hActiveWeapon", &active );
		edict_t *weapon = HandleToEdict( m_Grenade.weapon );
		if ( active == m_Grenade.weapon && GrenadeOwnedBy( weapon, m_pEdict ) &&
		     ( AgPB_ReadNetVarInt( weapon, "m_bPinPulled", 0 ) == 1 ||
		       AgPB_ReadNetVarFloat( weapon, "m_fThrowTime", 0.0f ) > 0.0f ) ) return;
	}
#endif
	m_Grenade = {};
	m_flNextGrenadeCheck = m_flGrenadeRetryUntil = 0.0f;
	m_pszGrenadeState = "idle";
}

bool CAgPB::GrenadeLandingSafe( CGlobalVars *globals, const Vector &landing, float radius ) const
{
	const float limit = radius * radius;
	if ( ( m_pInfo->GetAbsOrigin() - landing ).LengthSqr() < limit ) return false;
	const bool zm = AgPB_GameMode() == AgPB_MODE_ZM;
	for ( int slot = 1; slot <= globals->maxClients; ++slot )
	{
		edict_t *client = m_Ctx.pEngine->PEntityOfEntIndex( slot );
		if ( client == NULL || client == m_pEdict || client->IsFree() ) continue;
		IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( client );
		if ( info == NULL || !info->IsConnected() || !info->IsPlayer() || info->IsObserver() || info->IsDead() ) continue;
		if ( zm ? AgPB_ZRRoleForUserId( info->GetUserID() ) == AgPBZRRole::Zombie :
		          info->GetTeamIndex() != m_pInfo->GetTeamIndex() ) continue;
		if ( ( info->GetAbsOrigin() - landing ).LengthSqr() < limit ) return false;
	}
	return true;
}

bool CAgPB::CoverGrenadeThreatHidden( edict_t *enemy ) const
{
	const Vector offset = GetNetVarVector( "m_vecViewOffset" );
	Vector visiblePoint;
	return offset.IsValid() && offset.z > 0.0f &&
		!CombatAimPoint( enemy, m_pInfo->GetAbsOrigin() + offset, visiblePoint, false );
}

bool CAgPB::PlanGrenadeThrow( CGlobalVars *globals, bool zm )
{
#if defined( AGPB_GAME_CSGO )
	return false;
#else
	const float now = globals->curtime;
	m_flNextGrenadeCheck = now + 1.0f;
	const AgPBSearch::Observation &seen = m_Search.visual;
	const uintp enemy = zm ? m_ZM.sight.handle : (uintp)seen.handle;
	const int userId = zm ? m_ZM.sight.userId : seen.userId;
	const float age = now - ( zm ? m_ZM.sight.seenAt : seen.observedAt );
	if ( userId <= 0 || age < 0.0f || age > 4.0f || ( !zm && ( !seen.valid || age < 0.2f ) ) ) return false;
	edict_t *targetEntity = HandleToEdict( enemy );
	if ( zm ? ZMPlayer( targetEntity, AgPBZRRole::Zombie, userId ) == NULL : !IsCombatEnemy( targetEntity, userId ) )
		return false;
	if ( m_Tasks.Current().id == AgPBTaskId::SeekCover && !CoverGrenadeThreatHidden( targetEntity ) )
		return false;

	const Vector target = zm ? m_ZM.sight.origin : Vector( seen.position.x, seen.position.y, seen.position.z );
	const Vector delta = target - m_pInfo->GetAbsOrigin();
	if ( !delta.IsValid() || delta.z > 650.0f ) return false;
	const float range = delta.Length();
	Vector side( -delta.y, delta.x, 0.0f );
	if ( VectorNormalize( side ) < 1.0f ) return false;
	const Vector aimPoints[] = {
		target + side * 52.0f + Vector( 0, 0, 24 ),
		target - side * 52.0f + Vector( 0, 0, 24 )
	};
	for ( const GrenadeChoice &choice : grenades )
	{
		const int chance = zm ? choice.zmChance : choice.normalChance;
		if ( range <= choice.minRange || range >= choice.maxRange ) continue;
		edict_t *weapon = FindOwnedWeapon( choice.classname );
		if ( !GrenadeOwnedBy( weapon, m_pEdict ) ) continue;
		const uintp handle = AgPB_RefEHandle( weapon );
		const int type = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
		const int ammo = type >= 0 ? GetNetVarArrayInt( "m_iAmmo", type, -1 ) : -1;
		if ( !NetVar_HandleIsValid( handle ) || ammo <= 0 || RandomInt( 1, 100 ) > chance ) continue;
		for ( const Vector &landing : aimPoints )
		{
			QAngle aim;
			Vector effect;
			float padding;
			if ( !GrenadeLandingSafe( globals, landing, choice.radius ) ||
			     !SolveThrow( *this, m_Ctx, Vector( 0, 0, 0 ), landing, choice.flight, aim, effect, padding ) ||
			     !GrenadeLandingSafe( globals, effect, choice.radius + padding ) ) continue;
			m_Grenade.phase = GrenadePhase::Prepare;
			m_Grenade.weapon = handle;
			m_Grenade.enemy = enemy;
			m_Grenade.userId = userId;
			m_Grenade.ammoType = type;
			m_Grenade.ammoBefore = ammo;
			m_Grenade.deadline = now + 2.5f;
			m_Grenade.safetyRadius = choice.radius;
			m_Grenade.maxFlight = choice.flight;
			m_Grenade.landing = landing;
			m_Grenade.aim = aim;
			m_pszGrenadeState = "prepare";
			return true;
		}
	}
	return false;
#endif
}

bool CAgPB::UpdateGrenadeThrow( CGlobalVars *globals, CBotCmd &cmd )
{
#if defined( AGPB_GAME_CSGO )
	// [game-diff] CS:GO's native variable throw strength is not calibrated here.
	return false;
#else
	const float now = globals->curtime;
	if ( m_Grenade.phase == GrenadePhase::Idle && now < m_flNextGrenadeCheck ) return false;
	if ( m_pInfo == NULL || m_Ctx.pEngine == NULL || m_Ctx.pPlayerInfoManager == NULL ) return false;
	const auto finish = [&]( const char *status ) {
		m_Grenade = {};
		m_pszGrenadeState = status;
		m_flNextGrenadeCheck = m_flGrenadeRetryUntil = now + 6.0f;
		ResetWeaponSelection( status );
	};
	uintp active = (uintp)INVALID_EHANDLE_INDEX;
	GetNetVarHandleValue( "m_hActiveWeapon", &active );
	edict_t *weapon = HandleToEdict( m_Grenade.weapon );
	const bool equipped = weapon != NULL && active == m_Grenade.weapon && GrenadeOwnedBy( weapon, m_pEdict );
	if ( m_Grenade.phase == GrenadePhase::Prime && equipped && AgPB_ReadNetVarInt( weapon, "m_bPinPulled", 0 ) == 1 )
	{
		m_Grenade.phase = GrenadePhase::Release;
		m_Grenade.deadline = now + 0.65f;
		m_pszGrenadeState = "release";
	}
	if ( m_Grenade.phase == GrenadePhase::Release )
	{
		const float throwTime = equipped ? AgPB_ReadNetVarFloat( weapon, "m_fThrowTime", -1.0f ) : -1.0f;
		if ( GetNetVarArrayInt( "m_iAmmo", m_Grenade.ammoType, m_Grenade.ammoBefore ) < m_Grenade.ammoBefore ||
		     ( m_Grenade.nativeThrowQueued && throwTime == 0.0f ) )
		{
			++m_uConfirmedGrenades;
			finish( "thrown" );
			return false;
		}
		m_Grenade.nativeThrowQueued |= throwTime > 0.0f;
		if ( !equipped || now >= m_Grenade.deadline )
		{
			finish( "unconfirmed" );
			return false;
		}
		// Source samples eye angles at emission, after releasing the pin.
		cmd.viewangles = m_Grenade.aim;
		if ( m_Grenade.coverCrouch ) cmd.buttons |= IN_DUCK;
		return true;
	}

	const AgPBGameMode mode = AgPB_GameMode();
	const bool zm = mode == AgPB_MODE_ZM;
	const bool enabled = m_bAutonomous && m_bCombatEnabled && agpb_combat.GetBool() &&
		( mode == AgPB_MODE_NORMAL || ( zm && AgPB_ZRRoleForUserId( m_pInfo->GetUserID() ) == AgPBZRRole::Human ) );
	if ( enabled && mode == AgPB_MODE_NORMAL ) RefreshTaskPlan( globals );
	const AgPBTask &task = m_Tasks.Current();
	unsigned int leg = 0, next = 0;
	CAgPBWaypoints &waypoints = BotWaypoints();
	waypoints.IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
	waypoints.IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &next );
	const AgPBPath *node = waypoints.Get( RouteNode( m_iRouteIndex ) );
	const bool available = enabled && !AgPB_IsRoundFrozen() && !m_Ladder.Active() && !m_bPendingJumpLaunch &&
		( GetNetVarInt( "m_fFlags" ) & ( FL_ONGROUND | FL_FROZEN | FL_BASEVELOCITY ) ) == FL_ONGROUND &&
		GetNetVarInt( "movetype" ) == MOVETYPE_WALK && GetNetVarInt( "m_nWaterLevel" ) < 2 &&
		( ( leg | next ) & AgPB_PATH_JUMP ) == 0 &&
		( node == NULL || ( node->flags & ( AgPB_WP_LADDER | AgPB_WP_LIFT ) ) == 0 ) &&
		task.desire < AgPBTaskPriority::Objective &&
		( task.id != AgPBTaskId::SeekCover || CanThrowFromCover( now ) ) &&
		( task.deadline <= 0.0f || now < task.deadline ) &&
		task.id != AgPBTaskId::DestroyBreakable &&
		( !zm || !m_ZM.retreating );
	if ( !available || !NativeGrenadeRadio().IsValid() )
	{
		if ( m_Grenade.phase != GrenadePhase::Idle ) finish( "interrupted" );
		return false;
	}

	if ( m_Grenade.phase == GrenadePhase::Idle )
	{
		if ( !PlanGrenadeThrow( globals, zm ) ) return false;
		weapon = HandleToEdict( m_Grenade.weapon );
	}
	edict_t *enemy = HandleToEdict( m_Grenade.enemy );
	const bool validEnemy = zm ? ZMPlayer( enemy, AgPBZRRole::Zombie, m_Grenade.userId ) != NULL :
		IsCombatEnemy( enemy, m_Grenade.userId );
	if ( now >= m_Grenade.deadline || !GrenadeOwnedBy( weapon, m_pEdict ) || !validEnemy )
	{
		finish( "cancelled" );
		return false;
	}
	if ( active != m_Grenade.weapon )
	{
		cmd.weaponselect = m_Ctx.pEngine->IndexOfEdict( weapon );
		return true;
	}

	const Vector velocity = GetNetVarVector( "m_vecVelocity" );
	if ( !velocity.IsValid() || velocity.LengthSqr() > 28.0f * 28.0f ) return true;
	const bool checkArc = now >= m_Grenade.nextAim;
	if ( checkArc )
	{
		if ( task.id == AgPBTaskId::SeekCover && !CoverGrenadeThreatHidden( enemy ) )
		{
			finish( "cover-throw-blocked" );
			return false;
		}
		Vector effect;
		float padding;
		if ( !GrenadeLandingSafe( globals, m_Grenade.landing, m_Grenade.safetyRadius ) ||
		     !SolveThrow( *this, m_Ctx, velocity, m_Grenade.landing, m_Grenade.maxFlight, m_Grenade.aim, effect, padding ) ||
		     !GrenadeLandingSafe( globals, effect, m_Grenade.safetyRadius + padding ) )
		{
			finish( "arc-lost" );
			return false;
		}
		m_Grenade.nextAim = now + 0.15f;
	}

	const AgPBMovement::View view = AgPBMovement::ApproachView(
		GetNetVarFloat( "m_angEyeAngles[0]" ), GetNetVarFloat( "m_angEyeAngles[1]" ),
		m_Grenade.aim.x, m_Grenade.aim.y, 360.0f, globals->interval_per_tick );
	cmd.viewangles.Init( view.pitch, view.yaw, 0.0f );
	if ( checkArc && std::abs( AngleDiff( view.pitch, m_Grenade.aim.x ) ) <= 1.0f &&
	     std::abs( AngleDiff( view.yaw, m_Grenade.aim.y ) ) <= 1.0f &&
	     now >= GetNetVarFloat( "m_flNextAttack", FLT_MAX ) &&
	     now >= AgPB_ReadNetVarFloat( weapon, "m_flNextPrimaryAttack", FLT_MAX ) &&
	     AgPB_ReadNetVarInt( weapon, "m_bRedraw", -1 ) == 0 &&
	     AgPB_ReadNetVarInt( weapon, "m_bPinPulled", -1 ) == 0 &&
	     AgPB_ReadNetVarFloat( weapon, "m_fThrowTime", -1.0f ) == 0.0f )
	{
		cmd.viewangles = m_Grenade.aim;
		cmd.buttons |= IN_ATTACK;
		m_Grenade.coverCrouch = task.id == AgPBTaskId::SeekCover &&
			( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
		m_Grenade.phase = GrenadePhase::Prime;
		m_pszGrenadeState = "pin";
	}
	return true;
#endif
}

void CAgPB::RunPlayerCommand( CBotCmd &cmd )
{
#if !defined( AGPB_GAME_CSGO )
	// [game-diff] CS:S and CS:CZS grenade emission calls CCSPlayer::Radio.
	// Suppress that call for this fake client's command only.
	ConVarRef &radio = NativeGrenadeRadio();
	if ( radio.IsValid() && !radio.GetBool() )
	{
		uintp active = (uintp)INVALID_EHANDLE_INDEX;
		GetNetVarHandleValue( "m_hActiveWeapon", &active );
		edict_t *weapon = HandleToEdict( active );
		if ( m_Grenade.phase == GrenadePhase::Release ||
		     ( weapon != NULL && AgPB_ReadNetVarFloat( weapon, "m_fThrowTime", 0.0f ) > 0.0f ) )
		{
			radio.SetValue( true );
			m_pController->RunPlayerMove( &cmd );
			radio.SetValue( false );
			return;
		}
	}
#endif
	m_pController->RunPlayerMove( &cmd );
}
