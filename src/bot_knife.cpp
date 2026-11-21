/*
 * AgPB normal-mode knife selection. GPL-3.0-only.
 * Behavior baseline: SyPB 4c364fb source/basecode.cpp BotAI and
 * IsOnAttackDistance, source/navigate.cpp HeadTowardWaypoint, and
 * source/combat.cpp FireWeapon and IsGroupOfEnemies. Compared with CS-EBOT
 * 2333562 source/basecode.cpp, source/navigate.cpp and source/combat.cpp.
 *
 * Copyright (c) 2003-2023, by HsK-Dev Blog.
 * Copyright (c) 2003-2009, by Yet Another POD-Bot Development Team.
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <const.h>
#include <in_buttons.h>
#include <tier1/strtools.h>
#include <vstdlib/random.h>
#include <cmath>
#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "waypoint.h"

extern CGlobalVars *gpGlobals;

namespace
{
bool SpawnKnifeMap()
{
	const int flags = BotObjectives().Snapshot().mapFlags;
	const char *map = BotWaypoints().MapName();
	return ( flags & ( AGPB_MAP_BOMB | AGPB_MAP_HOSTAGE ) ) != 0 ||
		V_strnicmp( map, "es_", 3 ) == 0 || V_strnicmp( map, "as_", 3 ) == 0;
}

Vector PlayerCenter( IPlayerInfo &info )
{
	return info.GetAbsOrigin() + ( info.GetPlayerMins() + info.GetPlayerMaxs() ) * 0.5f;
}

bool IsJumpEdge( int from, int to, Vector &delta )
{
	const auto &waypoints = BotWaypoints();
	const AgPBPath *start = waypoints.Get( from );
	const AgPBPath *end = waypoints.Get( to );
	unsigned int flags = 0;
	if ( start == NULL || end == NULL || ( ( start->flags | end->flags ) & AgPB_WP_LADDER ) != 0 ||
	     !waypoints.IsConnected( from, to, &flags ) || ( flags & AgPB_PATH_JUMP ) == 0 ) return false;
	delta = end->origin - start->origin;
	return delta.IsValid();
}

bool KnifeHelpsJump( const Vector &delta, float maxSpeed )
{
	const float distance = delta.LengthSqr();
	return distance > 210.0f * 210.0f || ( delta.z > -32.0f && distance > 150.0f * 150.0f ) ||
		( delta.Length2DSqr() < 50.0f * 50.0f && distance > 60.0f * 60.0f ) ||
		( maxSpeed > 0.0f && maxSpeed <= 210.0f );
}
}

void CAgPB::BeginKnifeSpawn( float now )
{
	m_uKnifeSpawnRound = AgPB_PurchaseRound().epoch;
	m_flKnifeSpawnAt = now + RandomFloat( 4.0f, 6.5f );
	m_bKnifeSpawnChecked = m_bKnifeSpawnTravel = false;
	m_iKnifeJumpFrom = m_iKnifeJumpTo = -1;
	m_uKnifeJumpOwner = 0;
}

bool CAgPB::ShouldUseCombatKnife( edict_t *enemy, bool holdingKnife ) const
{
	if ( m_iCombatSkill < 80 || GetNetVarInt( "movetype" ) == MOVETYPE_LADDER ||
	     !IsCombatEnemy( enemy ) ) return false;
	IPlayerInfo *target = m_Ctx.pPlayerInfoManager->GetPlayerInfo( enemy );
	const int health = target->GetHealth();
	if ( health > 30 && m_pInfo->GetHealth() <= health ) return false;

	// GoldSrc's player origin is the hull center. Source's feet origin needs
	// the public hull offsets to preserve the same three-dimensional test.
	constexpr float reach = 120.0f;
	const Vector center = PlayerCenter( *m_pInfo );
	const Vector offset = PlayerCenter( *target ) - center;
	if ( !offset.IsValid() ) return false;
	if ( offset.LengthSqr() >= reach * reach )
	{
		if ( !holdingKnife ) return false;
		Vector bottom = offset, head = offset;
		const float height = target->GetPlayerMaxs().z - target->GetPlayerMins().z;
		bottom.z -= height * 0.5f + 1.0f;
		head.z += height * ( 0.81f - 0.5f ) + 1.0f; // SyPB's hull-based head estimate.
		if ( bottom.LengthSqr() >= reach * reach && head.LengthSqr() >= reach * reach ) return false;
	}

	// The default SyPB group parameters are two opponents within 600 units.
	// Count the qualifying target first, then look for another opponent.
	constexpr float groupRadius = 600.0f;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		edict_t *other = m_Ctx.pEngine->PEntityOfEntIndex( i );
		if ( other == enemy || !IsCombatEnemy( other ) ) continue;
		IPlayerInfo *player = m_Ctx.pPlayerInfoManager->GetPlayerInfo( other );
		if ( ( PlayerCenter( *player ) - center ).LengthSqr() < groupRadius * groupRadius ) return false;
	}
	return true;
}

CAgPB::KnifeIntent CAgPB::ChooseKnifeIntent( CGlobalVars *globals, const CBotCmd &cmd,
	bool enemyVisible, edict_t *enemy, edict_t *active )
{
	if ( globals == NULL || AgPB_GameMode() != AgPB_MODE_NORMAL ) return KnifeIntent::None;
	const AgPBPurchaseRound &round = AgPB_PurchaseRound();
	if ( m_flKnifeSpawnAt < 0.0f || m_uKnifeSpawnRound != round.epoch ) BeginKnifeSpawn( globals->curtime );
	const auto weapon = ClassifyWeapon( active );
	const bool holdingKnife = weapon == AgPBWeaponSelect::Weapon::Knife;
	if ( enemyVisible )
	{
		m_bKnifeSpawnChecked = true;
		m_bKnifeSpawnTravel = false;
		m_uKnifeJumpOwner = 0;
		const bool submergedShotgun = AgPBWeaponSelect::Shotgun( weapon ) && GetNetVarInt( "m_nWaterLevel" ) >= 2;
		return !submergedShotgun && ShouldUseCombatKnife( enemy, holdingKnife ) ? KnifeIntent::Combat : KnifeIntent::None;
	}

	const bool navigating = HasRoute() && !IsHoldingTaskPosition() && !m_Ladder.Active() &&
		GetNetVarInt( "movetype" ) != MOVETYPE_LADDER;
	if ( !navigating )
	{
		m_bKnifeSpawnTravel = false;
		m_uKnifeJumpOwner = 0;
		return KnifeIntent::None;
	}

	// The public player timer covers deployment and magazine reloads. The
	// shotguns publish their own state for the rest of the shell reload cycle.
	const float readyAt = GetNetVarFloat( "m_flNextAttack", -1.0f );
	const bool shellReload = AgPBWeaponSelect::Shotgun( weapon ) && AgPB_ReadNetVarInt( active, "m_reloadState" ) != 0;
	const bool canDraw = holdingKnife || ( AgPBWeaponSelect::Firearm( weapon ) &&
		!shellReload && std::isfinite( readyAt ) && readyAt >= 0.0f && globals->curtime >= readyAt &&
		( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE ) ) == 0 );
	const bool grounded = ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) != 0;
	const float maxSpeed = GetNetVarFloat( "m_flMaxspeed" );
	for ( int hop = 0; hop < 2; ++hop )
	{
		const int from = RouteNode( m_iRouteIndex - 1 + hop );
		const int to = RouteNode( m_iRouteIndex + hop );
		Vector delta;
		if ( !IsJumpEdge( from, to, delta ) || ( hop == 0 && grounded && m_bRouteLegAirborne ) ) continue;
		const bool sameEdge = m_uKnifeJumpOwner == m_uRouteTaskToken &&
			from == m_iKnifeJumpFrom && to == m_iKnifeJumpTo;
		if ( !sameEdge && ( !grounded || !canDraw || !KnifeHelpsJump( delta, maxSpeed ) ) ) continue;
		m_uKnifeJumpOwner = m_uRouteTaskToken;
		m_iKnifeJumpFrom = from;
		m_iKnifeJumpTo = to;
		m_bKnifeSpawnTravel = false;
		return KnifeIntent::Jump;
	}
	m_uKnifeJumpOwner = 0;

	const bool buyingFinished = m_Purchases.stage == AgPBPurchaseStage::Complete ||
		GetNetVarInt( "m_bInBuyZone", 1 ) == 0;
	if ( !m_bKnifeSpawnChecked && m_bAutonomous && round.active && buyingFinished && grounded &&
	     globals->curtime >= m_flKnifeSpawnAt )
	{
		m_bKnifeSpawnChecked = true;
		const int chance = m_Autonomy.personality == AgPBLocalPersonality::Rusher ? 99 : 80;
		m_bKnifeSpawnTravel = m_iCombatSkill > 75 && canDraw && SpawnKnifeMap() && RandomInt( 0, 100 ) < chance;
	}
	return m_bKnifeSpawnTravel ? KnifeIntent::Spawn : KnifeIntent::None;
}
