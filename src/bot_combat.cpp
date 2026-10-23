// AgPB minimum normal-mode combat. GPL-3.0-only.
//
// Behavior is adapted from SyPB source/combat.cpp ResetCheckEnemy,
// LookupEnemy, GetAimPosition, FocusEnemy and FireWeapon, with the matching
// CS-EBOT source/combat.cpp target/visibility/firing paths compared. Zombie
// branches, wall shooting, pursuit and weapon tactics are not included.
// Source SDK code is used only to verify public interfaces and weapon rules.
//
// Copyright (c) 2003-2023, by HsK-Dev Blog.
// Copyright (c) 2003-2009, by Yet Another POD-Bot Development Team.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <tier1/strtools.h>
#include <mathlib/mathlib.h>
#include <in_buttons.h>
#include <const.h>
#include <gametrace.h>
#include <iservernetworkable.h>
#include <iserverunknown.h>
#include <cmath>
#include <cfloat>
#include <worldsize.h>
#include <vstdlib/random.h>
#include "plugin.h"
#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "obstacle.h"
#include "waypoint.h"
#include "combat_skill.h"

extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();

ConVar agpb_combat( "agpb_combat", "1", FCVAR_GAMEDLL,
    "Enable local SyPB normal-mode combat; 0 preserves navigation-only baselines." );
ConVar agpb_difficulty( "agpb_difficulty", "3", FCVAR_GAMEDLL,
    "SyPB creation difficulty: 0=skill1-29, 1=30-49, 2=50-79, 3=80-99, 4=100." );
static ConVar agpb_combat_debug( "agpb_combat_debug", "0", FCVAR_GAMEDLL,
    "Log minimum-combat target acquisition and release." );

namespace {
bool ReadVector( edict_t *entity, const char *name, Vector &value )
{
    const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
    if ( field != NULL && field->type == DPT_Vector )
    {
        value = AgPB_ReadNetVarVector( entity, name );
        if ( value.IsValid() ) return true;
    }
    // Local Source player data can send punch/view vectors as three float
    // elements. Missing reflection must not silently become zero recoil.
    for ( int i = 0; i < 3; ++i )
    {
        char element[96]; V_snprintf( element, sizeof( element ), "%s[%d]", name, i );
        field = AgPB_FindEntityNetVar( entity, element );
        if ( field == NULL || field->type != DPT_Float ) return false;
        value[i] = AgPB_ReadNetVarFloat( entity, element );
    }
    return value.IsValid();
}

bool ViewOffset( edict_t *entity, Vector &offset )
{
    return ReadVector( entity, "m_vecViewOffset", offset ) && offset.z > 0.0f;
}

bool InsideViewCone( const CAgPB &bot, const Vector &eye, const Vector &point )
{
    Vector direction = point - eye, forward;
    if ( VectorNormalize( direction ) < 0.01f ) return false;
    AngleVectors( QAngle( bot.GetNetVarFloat( "m_angEyeAngles[0]" ), bot.GetNetVarFloat( "m_angEyeAngles[1]" ), 0 ), &forward );
    const int reflected = bot.GetNetVarInt( "m_iFOV", 0 );
    const float fov = reflected > 0 && reflected < 180 ? float( reflected ) : 90.0f;
    return DotProduct( forward, direction ) >= cosf( DEG2RAD( fov * 0.5f ) );
}

bool NamedWeapon( edict_t *weapon, const char *name )
{
    const char *actual = weapon != NULL ? weapon->GetClassName() : NULL;
    return actual != NULL && Q_stricmp( actual, name ) == 0;
}

bool SniperWeapon( edict_t *weapon )
{
    return NamedWeapon( weapon, "weapon_awp" ) || NamedWeapon( weapon, "weapon_scout" ) ||
        NamedWeapon( weapon, "weapon_g3sg1" ) || NamedWeapon( weapon, "weapon_sg550" );
}

bool ReadFloat( edict_t *entity, const char *name, float &value )
{
    const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
    if ( field == NULL || field->type != DPT_Float ) return false;
    value = AgPB_ReadNetVarFloat( entity, name );
    return std::isfinite( value );
}

int WeaponClip( edict_t *weapon )
{
    const BotNetVar *field = AgPB_FindEntityNetVar( weapon, "m_iClip1" );
    // The SDK's SendPropIntWithMinusOneFlag proxy sends stored clip + 1.
    return field != NULL && field->type == DPT_Int
        ? AgPB_ReadNetVarInt( weapon, "m_iClip1" ) - 1 : -1;
}

class CombatTraceFilter : public CTraceFilter
{
public:
    explicit CombatTraceFilter( IHandleEntity *self ) : m_Self( self ) {}
    bool ShouldHitEntity( IHandleEntity *entity, int ) { return entity != m_Self; }
private:
    IHandleEntity *m_Self;
};

bool FirstHit( const BotEngineContext &ctx, edict_t *self, edict_t *target,
               const Vector &eye, const Vector &end )
{
    if ( ctx.pTrace == NULL || ctx.pGameEnts == NULL || self == NULL ||
         self->IsFree() || self->GetUnknown() == NULL || target == NULL ||
         !eye.IsValid() || !end.IsValid() ) return false;
    IServerNetworkable *network = self->GetNetworkable();
    if ( network == NULL || network->GetEntityHandle() == NULL ) return false;
    CombatTraceFilter filter( network->GetEntityHandle() );
    Ray_t ray; ray.Init( eye, end );
    trace_t hit;
    ctx.pTrace->TraceRay( ray, MASK_SHOT, &filter, &hit );
    // Include every player. A friendly or another enemy in front blocks fire;
    // never ignore players in a visibility trace then fire through them.
    return !hit.startsolid && !hit.allsolid && hit.m_pEnt != NULL &&
        ctx.pGameEnts->BaseEntityToEdict( hit.m_pEnt ) == target;
}

bool FireRayAllowed( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                     const Vector &eye, const Vector &direction )
{
    if ( ctx.pTrace == NULL || ctx.pGameEnts == NULL || ctx.pEngine == NULL ||
         self == NULL || self->IsFree() || self->GetUnknown() == NULL ) return false;
    IServerNetworkable *network = self->GetNetworkable();
    if ( network == NULL || network->GetEntityHandle() == NULL ) return false;
    CombatTraceFilter filter( network->GetEntityHandle() );
    Ray_t ray; ray.Init( eye, eye + direction * float( MAX_TRACE_LENGTH ) );
    trace_t hit;
    ctx.pTrace->TraceRay( ray, MASK_SHOT, &filter, &hit );
    if ( hit.startsolid || hit.allsolid ) return false;
    if ( hit.m_pEnt == NULL ) return hit.fraction >= 1.0f;
    edict_t *entity = ctx.pGameEnts->BaseEntityToEdict( hit.m_pEnt );
    // Preserve SyPB low-skill misses. A clear/world-hit miss is permitted
    // only while the target is separately visible; any unrelated entity,
    // including a friendly behind the target, blocks this entire long ray.
    return entity == target || ( entity != NULL && ctx.pEngine->IndexOfEdict( entity ) == 0 );
}
}

void CAgPB::InitializeCombatSkill()
{
    SetDifficulty( clamp( agpb_difficulty.GetInt(), 0, 4 ) );
}

bool CAgPB::SetSkill( int skill )
{
    if ( skill < 1 || skill > 100 ) return false;
    m_iCombatSkill = skill;
    m_iCombatDifficulty = -1;
    const AgPBCombatSkill::Row row = AgPBCombatSkill::SkillRow( skill );
    m_flCombatIdealReaction = m_flCombatActualReaction = row.minReaction;
    ClearCombat( "skill" );
    return true;
}

bool CAgPB::SetDifficulty( int difficulty )
{
    if ( difficulty < 0 || difficulty > 4 ) return false;
    const AgPBCombatSkill::Range range = AgPBCombatSkill::DifficultyRange( difficulty );
    SetSkill( RandomInt( range.minimum, range.maximum ) );
    m_iCombatDifficulty = difficulty;
    return true;
}

void CAgPB::ClearCombat( const char *reason )
{
    m_uCombatEnemy = m_uCombatWeapon = (uintp)INVALID_EHANDLE_INDEX;
    m_iCombatEnemyUserId = 0;
    m_iCombatLastClip = -1;
    m_uCombatOwnerToken = 0;
    m_flCombatLastTime = -1.0f;
    m_flCombatNextAttack = 0.0f;
    m_flCombatAimError = 0.0f;
    m_bCombatRelease = false;
    m_flCombatSurpriseUntil = m_flCombatRearCheckAt = m_flCombatFirePause = 0.0f;
    m_vCombatAimDeviation.Init();
    m_bCombatPerfectAim = false;
    m_pszCombatState = reason;
}

void CAgPB::SetCombat( bool enable )
{
    m_bCombatEnabled = enable;
    edict_t *breakWeapon = HandleToEdict( m_uBreakWeapon );
    if ( breakWeapon != NULL ) m_iBreakClip = WeaponClip( breakWeapon );
    m_bBreakKnifePending = false;
    if ( m_uBreakTaskToken != 0 ) m_bBreakRelease = true;
    ClearCombat( enable ? "enabled" : "disabled" );
    // The next command starts with released attack; a partially settled route
    // must still satisfy a full uninterrupted navigation settling interval.
    m_flRouteSettleSince = -1.0f;
    ResetRouteProgress( gpGlobals != NULL ? gpGlobals->curtime : 0.0f );
}

bool CAgPB::IsCombatEnemy( edict_t *target, int expectedUserId ) const
{
    if ( target == NULL || target == m_pEdict || target->IsFree() ||
         m_Ctx.pEngine == NULL || m_Ctx.pPlayerInfoManager == NULL || m_pInfo == NULL ) return false;
    const int index = m_Ctx.pEngine->IndexOfEdict( target );
    if ( gpGlobals == NULL || index < 1 || index > gpGlobals->maxClients ||
         !NetVar_HandleIsValid( AgPB_RefEHandle( target ) ) ) return false;
    IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    if ( info == NULL || !info->IsConnected() || !info->IsPlayer() || info->IsHLTV() ||
         info->IsReplay() || info->IsObserver() || info->IsDead() || info->GetHealth() <= 0 ||
         ( expectedUserId != 0 && info->GetUserID() != expectedUserId ) ) return false;
    const int team = info->GetTeamIndex();
    if ( ( team != AgPB_TEAM_T && team != AgPB_TEAM_CT ) || team == m_pInfo->GetTeamIndex() ) return false;
    const BotNetVar *life = AgPB_FindEntityNetVar( target, "m_lifeState" );
    const BotNetVar *teamField = AgPB_FindEntityNetVar( target, "m_iTeamNum" );
    void *base = AgPB_EntityBase( target );
    return life != NULL && life->type == DPT_Int && teamField != NULL &&
        teamField->type == DPT_Int && base != NULL && NetVar_GetInt( base, *life ) == LIFE_ALIVE &&
        NetVar_GetInt( base, *teamField ) == team;
}

bool CAgPB::CombatAimPoint( edict_t *target, const Vector &eye, Vector &point, bool requireViewCone, unsigned int *visibility ) const
{
    IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    if ( info == NULL ) return false;
    const Vector origin = info->GetAbsOrigin(), mins = info->GetPlayerMins(), maxs = info->GetPlayerMaxs();
    if ( !origin.IsValid() || !mins.IsValid() || !maxs.IsValid() ) return false;
    const Vector body = origin + ( mins + maxs ) * 0.5f;
    if ( visibility != NULL ) *visibility = 0;
    if ( requireViewCone && !InsideViewCone( *this, eye, body ) ) return false;
    Vector headOffset;
    const Vector head = ViewOffset( target, headOffset ) ? origin + headOffset : body;
    const Vector spots[] = { body, head };
    unsigned int visible = 0;
    for ( int i = 0; i < 2; ++i )
    {
        Vector direction = spots[i] - eye;
        if ( VectorNormalize( direction ) < 0.01f ) continue;
        if ( FirstHit( m_Ctx, m_pEdict, target, eye, spots[i] + direction * 8.0f ) ) visible |= 1u << i;
    }
    if ( visibility != NULL ) *visibility = visible;
    if ( visible != 0 )
    {
        point = ( visible & 1u ) != 0 ? body : head;
        return true;
    }
    return false;
}

edict_t *CAgPB::CurrentCombatWeapon( uintp &handle, bool &knife, int &clip, int &reserve ) const
{
    handle = (uintp)INVALID_EHANDLE_INDEX; knife = false; clip = reserve = -1;
    if ( !GetNetVarHandleValue( "m_hActiveWeapon", &handle ) ) return NULL;
    edict_t *weapon = HandleToEdict( handle );
    if ( weapon == NULL ) return NULL;
    const BotNetVar *owner = AgPB_FindEntityNetVar( weapon, "m_hOwner" );
    void *base = AgPB_EntityBase( weapon );
    if ( owner == NULL || owner->type != DPT_Int || base == NULL ||
         NetVar_GetHandle( base, *owner ) != AgPB_RefEHandle( m_pEdict ) ) return NULL;
    const char *name = weapon->GetClassName();
    knife = name != NULL && Q_stricmp( name, "weapon_knife" ) == 0;
    if ( knife ) return weapon;
    clip = WeaponClip( weapon );
    const int ammoType = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
    reserve = ammoType >= 0 ? GetNetVarArrayInt( "m_iAmmo", ammoType, -1 ) : -1;
    // Grenades/C4/unknown weapons are deliberately not fired as firearms.
    return clip > 0 || ( clip == 0 && reserve > 0 ) ? weapon : NULL;
}

void CAgPB::PauseCombatOwner( CGlobalVars *globals )
{
    const float now = globals->curtime;
    const float elapsed = m_flCombatLastTime >= 0.0f ? MAX( 0.0f, now - m_flCombatLastTime ) : globals->interval_per_tick;
    m_flCombatLastTime = now;
    // Only local action-time budgets pause. Objective deadlines, bomb blow
    // time, round clocks, engine weapon cooldowns and retry epochs stay real.
    if ( m_flStuckCheckTime > 0.0f ) m_flStuckCheckTime += elapsed;
    if ( m_flStuckRecoveryUntil > 0.0f ) m_flStuckRecoveryUntil += elapsed;
    if ( m_flNextObstacleCheck > 0.0f ) m_flNextObstacleCheck += elapsed;
    if ( m_flTaskActionStarted > 0.0f ) m_flTaskActionStarted += elapsed;
    if ( m_flNextTaskUse > 0.0f ) m_flNextTaskUse += elapsed;
    if ( m_flBreakDeadline > 0.0f ) m_flBreakDeadline += elapsed;
    if ( m_flBreakNextAttack > 0.0f ) m_flBreakNextAttack += elapsed;
    // Combat ammunition is not progress against the suspended breakable.
    // Rebaseline its observer so resumption cannot charge player shots to it.
    edict_t *breakWeapon = HandleToEdict( m_uBreakWeapon );
    if ( breakWeapon != NULL ) m_iBreakClip = WeaponClip( breakWeapon );
    m_bBreakKnifePending = false;
    if ( m_uBreakTaskToken != 0 ) m_bBreakRelease = true;
    AgPBTask &task = m_Tasks.Current();
    if ( task.deadline > 0.0f && ( task.id == AgPBTaskId::Camp || task.id == AgPBTaskId::Pause ) )
        task.deadline += elapsed;
    m_flRouteSettleSince = -1.0f;
}

bool CAgPB::UpdateCombat( CGlobalVars *globals, CBotCmd &cmd )
{
    // Confirm ammunition consumption before a killed/removed enemy releases
    // the overlay. Command counts alone are not evidence of an actual shot.
    edict_t *observedWeapon = HandleToEdict( m_uCombatWeapon );
    if ( observedWeapon != NULL && m_iCombatLastClip >= 0 )
    {
        const int clip = WeaponClip( observedWeapon );
        if ( clip >= 0 && clip < m_iCombatLastClip ) m_uCombatConfirmedShots += m_iCombatLastClip - clip;
        m_iCombatLastClip = clip;
    }
    const float now = globals->curtime;
    const AgPBCombatSkill::Row skillRow = AgPBCombatSkill::SkillRow( m_iCombatSkill );
    m_flCombatActualReaction = MIN( m_flCombatActualReaction + 0.2f, m_flCombatIdealReaction );
    m_flCombatIdealReaction = RandomFloat( skillRow.minReaction, skillRow.maxReaction );
    const bool hadEnemy = NetVar_HandleIsValid( m_uCombatEnemy );
    if ( hadEnemy )
    {
        edict_t *breakWeapon = HandleToEdict( m_uBreakWeapon );
        if ( breakWeapon != NULL ) m_iBreakClip = WeaponClip( breakWeapon );
        m_bBreakKnifePending = false;
    }
    const bool intent = m_bAutonomous || m_bRoam || HasRoute() || m_Tasks.Current().id != AgPBTaskId::Normal;
    const bool frozen = AgPB_IsRoundFrozen() || ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0;
    if ( !m_bCombatEnabled || !agpb_combat.GetBool() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
         !intent || frozen || !IsAlive() )
    {
        ClearCombat( frozen ? "frozen" : !intent ? "stopped" : "disabled" );
        return false;
    }
    // Keep the existing local objective fallback running during a fight;
    // this includes C4/hostage urgency and remembered planted observations.
    const std::uint64_t beforePlan = m_Tasks.Current().token;
    RefreshTaskPlan( globals );
    if ( m_Tasks.Current().token != beforePlan )
    {
        ClearCombat( "objective-replan" );
        return false; // The canonical dispatcher reconciles route ownership.
    }
    const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
    const AgPBTask &task = m_Tasks.Current();
    if ( hadEnemy && m_uCombatOwnerToken != task.token )
    { ClearCombat( "owner-changed" ); return false; }
    const AgPBPath *routeNode = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
    const bool unsupportedMovement = ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ||
        GetNetVarInt( "movetype", MOVETYPE_NONE ) == MOVETYPE_LADDER ||
        ( routeNode != NULL && ( routeNode->flags & AgPB_WP_LADDER ) != 0 );
    const bool plantUse = task.id == AgPBTaskId::PlantBomb && GetNetVarInt( "m_bInBombZone" ) != 0;
    const bool defuseUse = task.id == AgPBTaskId::DefuseBomb &&
        ( GetNetVarInt( "m_bIsDefusing" ) != 0 || ( world.bomb.positionKnown &&
          ( world.bomb.position - m_pInfo->GetAbsOrigin() ).Length2DSqr() < 96.0f * 96.0f ) );
    const bool expired = task.deadline > 0.0f && now >= task.deadline;
    if ( unsupportedMovement || task.id == AgPBTaskId::EscapeBomb || plantUse || defuseUse || expired )
    {
        ClearCombat( unsupportedMovement ? "movement" : "objective-priority" );
        return false;
    }
    Vector offset;
    if ( !ViewOffset( m_pEdict, offset ) ) { ClearCombat( "eye-unavailable" ); return false; }
    const Vector eye = m_pInfo->GetAbsOrigin() + offset;
    uintp weaponHandle; bool knife; int clip, reserve;
    edict_t *weapon = CurrentCombatWeapon( weaponHandle, knife, clip, reserve );
    if ( weapon == NULL ) { ClearCombat( "weapon-unavailable" ); return hadEnemy; }
    edict_t *target = hadEnemy ? HandleToEdict( m_uCombatEnemy ) : NULL;
    Vector point;
    unsigned int visibility = 0;
    if ( hadEnemy && ( !IsCombatEnemy( target, m_iCombatEnemyUserId ) ||
         !CombatAimPoint( target, eye, point, false, &visibility ) ||
         ( knife && !AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, point ) ) ) )
    {
        PauseCombatOwner( globals );
        if ( agpb_combat_debug.GetBool() ) RouteReport( "combat released ent=%d; route goal=#%d hop=%d owner=%llu",
            NetVar_HandleEntry( m_uCombatEnemy ), m_iGoalWaypoint, m_iRouteIndex, (unsigned long long)m_uRouteTaskToken );
        ClearCombat( "target-lost" );
        // Exactly one released command before either navigation, obstacle
        // attack or another enemy can own the command chain.
        return true;
    }
    if ( !hadEnemy )
    {
        float nearest = FLT_MAX;
        for ( int i = 1; i <= globals->maxClients; ++i )
        {
            edict_t *candidate = m_Ctx.pEngine->PEntityOfEntIndex( i );
            Vector aim;
            unsigned int seen = 0;
            if ( !IsCombatEnemy( candidate ) || !CombatAimPoint( candidate, eye, aim, false, &seen ) ||
                 ( knife && !AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, candidate, eye, aim ) ) ) continue;
            if ( !InsideViewCone( *this, eye, aim ) && !AgPBCombatSkill::ImmediateRearRecognition( m_iCombatSkill ) )
            {
                if ( m_flCombatRearCheckAt == 0.0f ) m_flCombatRearCheckAt = now + RandomFloat( 0.3f, 0.7f );
                if ( now <= m_flCombatRearCheckAt ) continue;
            }
            const float distance = ( aim - eye ).LengthSqr();
            // Stable enumeration resolves equal distance by the lower slot.
            if ( distance < nearest ) { nearest = distance; target = candidate; point = aim; visibility = seen; }
        }
        if ( target == NULL ) { m_pszCombatState = "scanning"; return false; }
        m_uCombatEnemy = AgPB_RefEHandle( target );
        m_iCombatEnemyUserId = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target )->GetUserID();
        m_uCombatOwnerToken = task.token;
        m_flCombatSurpriseUntil = now + AgPBCombatSkill::ReactionDelay(
            m_flCombatActualReaction, m_iCombatSkill, RandomInt( 0, 100 ) );
        m_flCombatRearCheckAt = 0.0f;
        m_bCombatRelease = true; // Acquire/align first, never fire on stale input.
        if ( agpb_combat_debug.GetBool() ) RouteReport( "combat acquired ent=%d userid=%d; preserving goal=#%d hop=%d owner=%llu",
            NetVar_HandleEntry( m_uCombatEnemy ), m_iCombatEnemyUserId, m_iGoalWaypoint, m_iRouteIndex,
            (unsigned long long)m_uRouteTaskToken );
    }
    m_flCombatActualReaction = 0.0f;
    PauseCombatOwner( globals );
    if ( m_uCombatWeapon != weaponHandle )
    {
        m_uCombatWeapon = weaponHandle; m_iCombatLastClip = clip; m_bCombatRelease = true;
    }
    cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
    cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP );
    cmd.weaponselect = 0;
    if ( ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0 ) cmd.buttons |= IN_DUCK;
    IPlayerInfo *enemyInfo = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    const Vector enemyOrigin = enemyInfo->GetAbsOrigin();
    const Vector enemyMins = enemyInfo->GetPlayerMins(), enemyMaxs = enemyInfo->GetPlayerMaxs();
    const Vector body = enemyOrigin + ( enemyMins + enemyMaxs ) * 0.5f;
    Vector headOffset;
    const Vector head = ViewOffset( target, headOffset ) ? enemyOrigin + headOffset : body;
    // Preserve GetAimPosition's head-selection test, including its AWP health
    // exception. Visibility is independently checked before any aim error.
    if ( visibility != 0 && AgPBCombatSkill::AimAtHead( m_iCombatSkill, RandomInt( 30, 80 ),
         NamedWeapon( weapon, "weapon_awp" ), enemyInfo->GetHealth() ) ) point = head;
    else point = body;
    if ( AgPB_ReadNetVarInt( target, "m_fFlags" ) & FL_DUCKING ) point.z -= 1.0f;
    if ( AgPBCombatSkill::HasAimNoise( m_iCombatSkill ) )
        for ( int axis = 0; axis < 3; ++axis )
            point[axis] += AgPBCombatSkill::RelativeNoise( enemyMins[axis], enemyMaxs[axis], RandomFloat( 0.0f, 1.0f ) );
    Vector punch( 0, 0, 0 );
    if ( !knife && !ReadVector( m_pEdict, "m_vecPunchAngle", punch ) )
    { m_pszCombatState = "punch-unavailable"; return true; }
    QAngle wanted; VectorAngles( point - eye, wanted );
    // Source CS fires along EyeAngles + 2 * punch; preserve the verified
    // Source compensation while porting SyPB's difficulty decision policy.
    wanted.x = clamp( AngleNormalize( wanted.x - 2.0f * punch.x ), -89.0f, 89.0f );
    wanted.y = AngleNormalize( wanted.y - 2.0f * punch.y );
    const AgPBCombatSkill::View current = { GetNetVarFloat( "m_angEyeAngles[0]" ), GetNetVarFloat( "m_angEyeAngles[1]" ) };
    Vector toAim = point - eye, currentForward;
    VectorNormalize( toAim );
    AngleVectors( QAngle( current.pitch, current.yaw, 0 ), &currentForward );
    const bool wantsFire = AgPBCombatSkill::WantsFire( now >= m_flCombatSurpriseUntil, DotProduct( currentForward, toAim ) );
    const bool sniper = SniperWeapon( weapon );
    const bool inViewCone = InsideViewCone( *this, eye, point );
    const int fov = GetNetVarInt( "m_iFOV", 0 );
    const float previousDeviation = m_vCombatAimDeviation.Length();
    float predictedPitch = 0.0f, predictedYaw = 0.0f;
    // The cast after integer skill/100 is intentional in SyPB: anticipation
    // applies at100 only and only during the spring (not the perfect) branch.
    if ( m_iCombatSkill == 100 )
    {
        Vector enemyVelocity;
        if ( ReadVector( target, "m_vecVelocity", enemyVelocity ) )
        {
            QAngle oldDirection, newDirection;
            VectorAngles( point - m_pInfo->GetAbsOrigin(), oldDirection );
            VectorAngles( point - m_pInfo->GetAbsOrigin() + 1.5f * globals->interval_per_tick * enemyVelocity, newDirection );
            predictedPitch = AngleDiff( newDirection.x, oldDirection.x ) * 0.45f * 2.2f;
            predictedYaw = AngleDiff( newDirection.y, oldDirection.y ) * 0.45f * 2.2f;
        }
    }
    const AgPBCombatSkill::View view = AgPBCombatSkill::StepAim( current,
        AgPBCombatSkill::View{ wanted.x, wanted.y }, predictedPitch, predictedYaw,
        globals->interval_per_tick, m_iCombatSkill, wantsFire, sniper, inViewCone,
        previousDeviation, fov > 0 && fov < 90 );
    m_bCombatPerfectAim = AgPBCombatSkill::PerfectAim( m_iCombatSkill, wantsFire, sniper );
    m_vCombatAimDeviation.Init( AngleDiff( wanted.x + predictedPitch, current.pitch ),
        AngleDiff( wanted.y + predictedYaw, current.yaw ), 0 );
    cmd.viewangles.Init( view.pitch, view.yaw, 0 );
    m_flCombatAimError = MAX( fabsf( AngleDiff( wanted.x, view.pitch ) ), fabsf( AngleDiff( wanted.y, view.yaw ) ) );
    m_pszCombatState = now < m_flCombatSurpriseUntil ? "reacting" : "aiming";
    if ( m_bCombatRelease ) { m_bCombatRelease = false; return true; }
    float playerAttack, weaponAttack;
    if ( !ReadFloat( m_pEdict, "m_flNextAttack", playerAttack ) ||
         !ReadFloat( weapon, "m_flNextPrimaryAttack", weaponAttack ) ||
         now < playerAttack || now < weaponAttack || now < m_flCombatNextAttack ) return true;
    if ( !knife && clip == 0 )
    {
        if ( reserve > 0 ) { cmd.buttons |= IN_RELOAD; m_pszCombatState = "reloading"; }
        return true;
    }
    if ( !wantsFire ) return true;
    const float distance = ( point - eye ).Length();
    if ( !knife && distance >= 256.0f )
    {
        if ( now < m_flCombatFirePause ) { m_pszCombatState = "recoil-pause"; return true; }
        if ( AgPBCombatSkill::RecoilExceeded( m_iCombatSkill, punch.x, punch.y, distance ) )
        {
            if ( m_flCombatFirePause < now - 0.4f )
                m_flCombatFirePause = now + RandomFloat( 0.4f, AgPBCombatSkill::RecoilPauseMaximum( m_iCombatSkill ) );
            m_pszCombatState = "recoil-pause"; return true;
        }
    }
    Vector fireDirection;
    AngleVectors( QAngle( view.pitch + 2.0f * punch.x, view.yaw + 2.0f * punch.y, 0 ), &fireDirection );
    // Body/head visibility was revalidated before aim noise. Preserve missed
    // shots without allowing an actual ray through an unrelated player/prop.
    const bool clearFire = knife
        ? FirstHit( m_Ctx, m_pEdict, target, eye, eye + fireDirection * 40.0f ) &&
          AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, point )
        : FireRayAllowed( m_Ctx, m_pEdict, target, eye, fireDirection );
    if ( !clearFire ) { m_pszCombatState = "fire-blocked"; return true; }
    cmd.buttons |= IN_ATTACK;
    ++m_uCombatAttackCommands;
    m_bCombatRelease = true;
    m_flCombatNextAttack = now + 0.15f;
    m_pszCombatState = "firing";
    return true;
}

void CAgPB::ReportCombat() const
{
    edict_t *target = HandleToEdict( m_uCombatEnemy );
    RouteReport( "combat v=1 enabled=%d global=%d state=%s enemy=0x%llX ent=%d userid=%d enemyhp=%d weapon=0x%llX clip=%d aimerror=%.3f attackcmds=%u shots=%u release=%d task=%s token=%llu owner=%llu routeowner=%llu goal=%d hop=%d node=%d",
        m_bCombatEnabled ? 1 : 0, agpb_combat.GetBool() ? 1 : 0, m_pszCombatState,
        (unsigned long long)m_uCombatEnemy, NetVar_HandleIsValid( m_uCombatEnemy ) ? NetVar_HandleEntry( m_uCombatEnemy ) : -1,
        m_iCombatEnemyUserId, target != NULL ? AgPB_ReadNetVarInt( target, "m_iHealth", -1 ) : -1,
        (unsigned long long)m_uCombatWeapon, m_iCombatLastClip, m_flCombatAimError,
        m_uCombatAttackCommands, m_uCombatConfirmedShots, m_bCombatRelease ? 1 : 0,
        AgPBTaskName( m_Tasks.Current().id ), (unsigned long long)m_Tasks.Current().token,
        (unsigned long long)m_uCombatOwnerToken, (unsigned long long)m_uRouteTaskToken, m_iGoalWaypoint,
        m_iRouteIndex, RouteNode( m_iRouteIndex ) );
    const AgPBCombatSkill::Row row = AgPBCombatSkill::SkillRow( m_iCombatSkill );
    RouteReport( "combat skill: difficulty=%d skill=%d bucket=%d reaction=(%.3f,%.3f) ideal=%.3f actual=%.3f surprise=%.3f rear=%.3f perfect=%d recoil=%d pause=%.3f",
        m_iCombatDifficulty, m_iCombatSkill, AgPBCombatSkill::Bucket( m_iCombatSkill ), row.minReaction, row.maxReaction,
        m_flCombatIdealReaction, m_flCombatActualReaction, m_flCombatSurpriseUntil, m_flCombatRearCheckAt,
        m_bCombatPerfectAim ? 1 : 0, row.recoilAmount, m_flCombatFirePause );
}
