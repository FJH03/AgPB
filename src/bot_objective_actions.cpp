/*
 * AgPB normal-mode objective actions.
 *
 * Behavior follows SyPB source/basecode.cpp (normal, plant, defuse, pickup,
 * hostage and bomb escape tasks) and source/navigate.cpp (objective goals).
 * CS-EBOT's corresponding files were checked; its zombie-only objective
 * branches are not used here. Source interaction rules come from the local
 * source-engine-czero game code. Actions are CUserCmd inputs only.
 *
 * SyPB copyright (c) 2003-2023 HsK-Dev Blog.
 * YaPB copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 * AgPB adaptation: GPL-3.0-only.
 */

#include <in_buttons.h>
#include <const.h>
#include <mathlib/mathlib.h>

#include "bot.h"
#include "objectives.h"
#include "tasks.h"
#include "waypoint.h"
#include "bomb_escape_policy.h"

// The task stack may change during navigation or completion. Work from a
// copy, and never retain references to Current() across either operation.
static bool TaskDestination( const AgPBTask &task, bool preferWaypoint,
                             Vector &position )
{
	const AgPBPath *waypoint = BotWaypoints().Get( task.waypoint );
	if ( preferWaypoint && waypoint != NULL )
	{
		position = waypoint->origin;
		return position.IsValid();
	}
	if ( task.hasPosition )
	{
		position.Init( task.position[0], task.position[1], task.position[2] );
		if ( position.IsValid() ) return true;
	}
	if ( waypoint != NULL )
	{
		position = waypoint->origin;
		return position.IsValid();
	}
	return false;
}

static const AgPBObjectiveHostage *FindTaskHostage( const AgPBObjectiveSnapshot &world,
                                                    const AgPBTask &task )
{
	if ( !task.hasEntity || !NetVar_HandleIsValid( (uintp)task.entityHandle ) ) return NULL;
	for ( int i = 0; i < world.hostages.Count(); ++i )
	{
		if ( world.hostages[i].handle == (uintp)task.entityHandle )
			return &world.hostages[i];
	}
	return NULL;
}

static void StopObjectiveMovement( CBotCmd &cmd )
{
	cmd.forwardmove = 0.0f;
	cmd.sidemove = 0.0f;
	cmd.upmove = 0.0f;
	cmd.buttons &= ~( IN_JUMP | IN_SPEED );
}

// CS:CZS FindUseEntity has a special planted-C4 path: the bomb can be used
// within 96 eye units while facing it even if an ordinary trace is blocked.
// Therefore a strict first-hit trace is deliberately not required for defuse.
static bool FaceObjectiveBomb( const CAgPB &bot, const Vector &position, CBotCmd &cmd )
{
	if ( bot.PlayerInfo() == NULL || !position.IsValid() ) return false;
	Vector offset = bot.GetNetVarVector( "m_vecViewOffset" );
	if ( offset.z <= 0.0f && bot.FindNetVar( "m_vecViewOffset[2]" ) != NULL )
		offset.Init( bot.GetNetVarFloat( "m_vecViewOffset[0]" ),
		             bot.GetNetVarFloat( "m_vecViewOffset[1]" ),
		             bot.GetNetVarFloat( "m_vecViewOffset[2]" ) );
	if ( offset.z <= 0.0f )
		offset.z = ( ( bot.GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0
		             ? AgPB_HullDuckHeight() : AgPB_HullStandHeight() ) - 4.0f;
	const Vector toBomb = position - ( bot.PlayerInfo()->GetAbsOrigin() + offset );
	if ( toBomb.LengthSqr() >= 88.0f * 88.0f || toBomb.LengthSqr() < 1.0f )
		return false;
	QAngle angles;
	VectorAngles( toBomb, angles );
	angles.x = clamp( AngleNormalize( angles.x ), -89.0f, 89.0f );
	angles.y = AngleNormalize( angles.y );
	angles.z = 0.0f;
	cmd.viewangles = angles;
	return true;
}

bool CAgPB::ExecuteObjectiveTask( CGlobalVars *globals, CBotCmd &cmd )
{
	if ( globals == NULL ) return false;
	const AgPBTask task = m_Tasks.Current();
	if ( task.id != AgPBTaskId::PlantBomb && task.id != AgPBTaskId::DefuseBomb &&
	     task.id != AgPBTaskId::PickupBomb && task.id != AgPBTaskId::RescueHostage &&
	     task.id != AgPBTaskId::EscortHostages && task.id != AgPBTaskId::EscapeBomb )
		return false;

	const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
	const int team = m_pInfo != NULL ? m_pInfo->GetTeamIndex() : 0;
	const float now = globals->curtime;
	const uintp selfHandle = AgPB_RefEHandle( m_pEdict );
	Vector target;

	switch ( task.id )
	{
	case AgPBTaskId::PlantBomb:
	{
		if ( team != AgPB_TEAM_T || ( world.mapFlags & AGPB_MAP_BOMB ) == 0 )
		{
			CompleteCurrentTask( "plant task has no T bomb objective", true );
			return true;
		}
		if ( world.bomb.state == AGPB_BOMB_PLANTED || world.bomb.state == AGPB_BOMB_RESOLVED )
		{
			CompleteCurrentTask( "bomb is planted" );
			return true;
		}
		edict_t *c4 = FindOwnedWeapon( "weapon_c4" );
		if ( c4 == NULL )
		{
			// Planting removes the weapon before the throttled world snapshot
			// observes planted_c4. Release input and allow that confirmation to
			// arrive instead of misreporting a successful plant as item loss.
			StopObjectiveMovement( cmd );
			cmd.buttons &= ~( IN_ATTACK | IN_USE );
			if ( now < m_flNextTaskUse ) return true;
			CompleteCurrentTask( "C4 is no longer owned", true );
			return true;
		}
		if ( FindNetVar( "m_bInBombZone" ) == NULL )
		{
			CompleteCurrentTask( "bomb zone SendProp unavailable", true );
			return true;
		}
		if ( GetNetVarInt( "m_bInBombZone" ) == 0 )
		{
			if ( !TaskDestination( task, true, target ) )
				CompleteCurrentTask( "plant target unavailable", true );
			else
				NavigateTaskTo( target, task.waypoint, 8.0f, globals, cmd );
			return true;
		}
		StopObjectiveMovement( cmd );
		cmd.buttons &= ~( IN_USE | IN_ATTACK | IN_ATTACK2 );
		if ( ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ) return true;
		uintp activeHandle;
		if ( !GetNetVarHandleValue( "m_hActiveWeapon", &activeHandle ) )
		{
			CompleteCurrentTask( "active weapon SendProp unavailable", true );
			return true;
		}
		const uintp c4Handle = AgPB_RefEHandle( c4 );
		if ( !NetVar_HandleIsValid( c4Handle ) )
		{
			CompleteCurrentTask( "owned C4 handle unavailable", true );
			return true;
		}
		if ( activeHandle != c4Handle )
		{
			cmd.weaponselect = NetVar_HandleEntry( c4Handle );
			return true;
		}
		const AgPBPath *waypoint = BotWaypoints().Get( task.waypoint );
		if ( waypoint != NULL && ( waypoint->flags & AgPB_WP_CROUCH ) != 0 )
			cmd.buttons |= IN_DUCK;
		// CC4::ItemPostFrame calls PrimaryAttack on every held attack tick;
		// its three-second arming sequence aborts if attack is released.
		cmd.buttons |= IN_ATTACK;
		m_flNextTaskUse = now + 0.5f;
		return true;
	}

	case AgPBTaskId::DefuseBomb:
	{
		if ( team != AgPB_TEAM_CT || ( world.mapFlags & AGPB_MAP_BOMB ) == 0 )
		{
			CompleteCurrentTask( "defuse task has no CT bomb objective", true );
			return true;
		}
		if ( world.bomb.state != AGPB_BOMB_PLANTED )
		{
			CompleteCurrentTask( "planted bomb is gone" );
			return true;
		}
		if ( !world.bomb.positionKnown || !NetVar_HandleIsValid( world.bomb.handle ) ||
		     ( task.hasEntity && world.bomb.handle != (uintp)task.entityHandle ) )
		{
			CompleteCurrentTask( "planted bomb target changed", true );
			return true;
		}
		if ( FindNetVar( "m_bIsDefusing" ) == NULL )
		{
			CompleteCurrentTask( "defuse state SendProp unavailable", true );
			return true;
		}
		const bool ownDefuse = GetNetVarInt( "m_bIsDefusing" ) != 0;
		if ( !ownDefuse && world.bomb.defuseCountdownKnown &&
		     world.bomb.defuseCountdown > now + 0.1f )
		{
			// Another CT owns the continuous-use slot. Treat this as an
			// unavailable target so planning does not reissue defuse every tick.
			CompleteCurrentTask( "teammate is defusing", true );
			return true;
		}
		if ( !ownDefuse && world.bomb.blowTimeKnown )
		{
			const bool kit = FindNetVar( "m_bHasDefuser" ) != NULL &&
			                 GetNetVarInt( "m_bHasDefuser" ) != 0;
			const float required = kit ? 5.0f : 10.0f;
			if ( now + required + 0.3f >= world.bomb.blowTime )
			{
				// Let the planner immediately select SyPB's escape task when
				// the timer no longer admits a full defuse.
				CompleteCurrentTask( "insufficient time to defuse" );
				return true;
			}
		}
		if ( !ownDefuse && !NavigateTaskTo( world.bomb.position, task.waypoint,
		                                  44.0f, globals, cmd ) )
			return true;
		StopObjectiveMovement( cmd );
		cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 );
		if ( ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ) return true;
		if ( !FaceObjectiveBomb( *this, world.bomb.position, cmd ) )
		{
			if ( ownDefuse ) CompleteCurrentTask( "lost bomb use range", true );
			return true;
		}
		// CPlantedC4::Use refreshes m_flNextDefuse for only 0.5s. Holding
		// IN_USE drives FCAP_CONTINUOUS_USE every tick until completion.
		cmd.buttons |= IN_USE;
		return true;
	}

	case AgPBTaskId::PickupBomb:
	{
		if ( team != AgPB_TEAM_T || ( world.mapFlags & AGPB_MAP_BOMB ) == 0 )
		{
			CompleteCurrentTask( "pickup task has no T bomb objective", true );
			return true;
		}
		if ( FindOwnedWeapon( "weapon_c4" ) != NULL )
		{
			CompleteCurrentTask( "C4 recovered" );
			return true;
		}
		if ( world.bomb.state == AGPB_BOMB_CARRIED ||
		     world.bomb.state == AGPB_BOMB_PLANTED ||
		     world.bomb.state == AGPB_BOMB_RESOLVED )
		{
			CompleteCurrentTask( "C4 is no longer dropped" );
			return true;
		}
		if ( world.bomb.state != AGPB_BOMB_DROPPED || !world.bomb.positionKnown ||
		     !NetVar_HandleIsValid( world.bomb.handle ) ||
		     ( task.hasEntity && world.bomb.handle != (uintp)task.entityHandle ) )
		{
			CompleteCurrentTask( "dropped C4 target unavailable", true );
			return true;
		}
		// Source drops weapon_c4 itself. Walking into its pickup touch is the
		// action; IN_USE cannot collect it. A near-zero range lets navigation
		// keep approaching until the inventory confirms ownership.
		NavigateTaskTo( world.bomb.position, task.waypoint, 4.0f, globals, cmd );
		return true;
	}

	case AgPBTaskId::RescueHostage:
	{
		if ( team != AgPB_TEAM_CT || ( world.mapFlags & AGPB_MAP_HOSTAGE ) == 0 )
		{
			CompleteCurrentTask( "hostage task has no CT hostage objective", true );
			return true;
		}
		const AgPBObjectiveHostage *hostage = FindTaskHostage( world, task );
		if ( hostage == NULL || !hostage->positionKnown )
		{
			CompleteCurrentTask( "hostage target unavailable", true );
			return true;
		}
		if ( hostage->rescuedKnown && hostage->rescued )
		{
			CompleteCurrentTask( "hostage already rescued" );
			return true;
		}
		if ( hostage->aliveKnown && !hostage->alive )
		{
			CompleteCurrentTask( "hostage is dead", true );
			return true;
		}
		if ( hostage->leaderKnown && NetVar_HandleIsValid( hostage->leader ) )
		{
			CompleteCurrentTask( hostage->leader == selfHandle
			                     ? "hostage following bot" : "hostage following another CT",
			                     hostage->leader != selfHandle );
			return true;
		}
		if ( !NavigateTaskTo( hostage->position, task.waypoint, 40.0f, globals, cmd ) )
			return true;
		StopObjectiveMovement( cmd );
		cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_USE );
		edict_t *targetEdict = HandleToEdict( hostage->handle );
		if ( targetEdict == NULL ||
		     !TaskAimAt( targetEdict, hostage->position + Vector( 0, 0, 36 ), cmd ) )
			return true;
		// A hostage has FCAP_IMPULSE_USE. Pulse only once, then release and
		// wait for m_leader feedback; another pulse after success toggles off.
		if ( now >= m_flNextTaskUse )
		{
			cmd.buttons |= IN_USE;
			m_uLastTaskUseHandle = hostage->handle;
			m_flNextTaskUse = now + 1.25f;
		}
		return true;
	}

	case AgPBTaskId::EscortHostages:
	{
		if ( team != AgPB_TEAM_CT || ( world.mapFlags & AGPB_MAP_HOSTAGE ) == 0 )
		{
			CompleteCurrentTask( "escort task has no CT hostage objective", true );
			return true;
		}
		const AgPBObjectiveHostage *hostage = FindTaskHostage( world, task );
		if ( hostage == NULL )
		{
			CompleteCurrentTask( "escorted hostage target unavailable", true );
			return true;
		}
		if ( hostage->rescuedKnown && hostage->rescued )
		{
			CompleteCurrentTask( "hostage reached rescue zone" );
			return true;
		}
		if ( hostage->aliveKnown && !hostage->alive )
		{
			CompleteCurrentTask( "escorted hostage is dead", true );
			return true;
		}
		if ( !hostage->leaderKnown || hostage->leader != selfHandle )
		{
			CompleteCurrentTask( "hostage stopped following bot", true );
			return true;
		}
		// SyPB treats a follower more than 600 units away as lost. Pause
		// before that point so Source's hostage path follower can catch up.
		if ( hostage->positionKnown &&
		     ( hostage->position - m_pInfo->GetAbsOrigin() ).Length2D() > 500.0f )
		{
			StopObjectiveMovement( cmd );
			cmd.buttons &= ~( IN_ATTACK | IN_USE );
			ResetRouteProgress( now );
			return true;
		}
		if ( !TaskDestination( task, true, target ) )
		{
			CompleteCurrentTask( "rescue zone target unavailable", true );
			return true;
		}
		if ( NavigateTaskTo( target, task.waypoint, 12.0f, globals, cmd ) )
		{
			StopObjectiveMovement( cmd );
			cmd.buttons &= ~( IN_ATTACK | IN_USE );
			// func_hostage_rescue fires on the hostage's touch, not the CT's.
			// The task remains active while the follower catches up.
		}
		return true;
	}

	case AgPBTaskId::EscapeBomb:
	{
		if ( AgPB_BombThreatEnded( world.roundEpoch == m_uEscapeRoundEpoch,
			world.bomb.handle == (uintp)task.entityHandle, world.bomb.state == AGPB_BOMB_RESOLVED ) )
		{
			CompleteCurrentTask( "bomb resolved or round changed" );
			ResetBombEscape();
			return true;
		}
		if ( !TaskDestination( task, true, target ) )
		{
			CompleteCurrentTask( "escape target unavailable", true );
			return true;
		}
		if ( NavigateTaskTo( target, task.waypoint, 32.0f, globals, cmd ) )
		{
			StopObjectiveMovement( cmd );
			cmd.buttons &= ~( IN_ATTACK | IN_USE );
			const float distance = world.bomb.positionKnown && m_pInfo != NULL
				? ( m_pInfo->GetAbsOrigin() - world.bomb.position ).Length() : -1.0f;
			const bool safe = AgPB_BombPositionSafe( distance, world.bombBlastRadiusKnown, world.bombBlastRadius );
			if ( task.waypoint >= 0 && !safe )
			{
				CompleteCurrentTask( "best-effort retreat reached; safety boundary not confirmed", true );
				return true;
			}
			if ( now >= m_flNextEscapeReport )
			{
				RouteReport( "escape holding: bomb-state=%d left=%.2f distance=%.1f radius=%.1f safe=%s",
					(int)world.bomb.state, world.bomb.blowTimeKnown ? world.bomb.blowTime - now : -1.0f,
					distance, world.bombBlastRadiusKnown ? world.bombBlastRadius : -1.0f, safe ? "yes" : "NO-unknown-or-blocked" );
				m_flNextEscapeReport = now + 5.0f;
			}
			// UNKNOWN/missing observations retain this refuge. Only a confirmed
			// resolution or a new round releases the emergency task.
		}
		return true;
	}

	default:
		return false;
	}
}
