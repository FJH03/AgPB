// Owned-weapon execution for the SyPB policy in weapon_selection.h.
// AgPB: GPL-3.0-only. Reads public SendTables; submits CUserCmd only.
#include <tier1/strtools.h>
#include <in_buttons.h>
#include <const.h>
#include <cmath>
#include "plugin.h"
#include "bot.h"
#include "waypoint.h"
#include "game_mode.h"

extern ConVar agpb_combat;
extern bool AgPB_IsRoundFrozen();

ConVar agpb_weapons_debug( "agpb_weapons_debug", "0", FCVAR_GAMEDLL,
    "Log submitted weapon-select, reload and secondary-attack commands and engine equipment readback." );

namespace {
using AgPBWeaponSelect::Weapon;
struct WeaponInfo { const char *name; Weapon weapon; int stockClip; };
// Stock Source CS magazine sizes for the supported weapon classes.
const WeaponInfo weapons[] = {
    { "weapon_knife", Weapon::Knife, -1 }, { "weapon_usp", Weapon::USP, 12 },
    { "weapon_glock", Weapon::Glock, 20 }, { "weapon_deagle", Weapon::Deagle, 7 },
    { "weapon_p228", Weapon::P228, 13 }, { "weapon_elite", Weapon::Elite, 30 },
    { "weapon_fiveseven", Weapon::FiveSeven, 20 }, { "weapon_m3", Weapon::M3, 8 },
    { "weapon_xm1014", Weapon::XM1014, 7 }, { "weapon_mp5navy", Weapon::MP5, 30 },
    { "weapon_tmp", Weapon::TMP, 30 }, { "weapon_p90", Weapon::P90, 50 },
    { "weapon_mac10", Weapon::MAC10, 30 }, { "weapon_ump45", Weapon::UMP45, 25 },
    { "weapon_ak47", Weapon::AK47, 30 }, { "weapon_sg552", Weapon::SG552, 30 },
    { "weapon_m4a1", Weapon::M4A1, 30 }, { "weapon_galil", Weapon::Galil, 35 },
    { "weapon_famas", Weapon::Famas, 25 }, { "weapon_aug", Weapon::AUG, 30 },
    { "weapon_scout", Weapon::Scout, 10 }, { "weapon_awp", Weapon::AWP, 10 },
    { "weapon_g3sg1", Weapon::G3SG1, 20 }, { "weapon_sg550", Weapon::SG550, 30 },
    { "weapon_m249", Weapon::M249, 100 }
};
const WeaponInfo *FindWeaponInfo( const char *name )
{
    if ( name != NULL ) for ( const WeaponInfo &entry : weapons )
        if ( V_stricmp( name, entry.name ) == 0 ) return &entry;
    return NULL;
}

struct OwnedWeapon {
    AgPBWeaponSelect::Candidate candidate;
    const char *name;
};

bool OwnedBy( const CAgPB &bot, edict_t *weapon )
{
    if ( weapon == NULL || weapon->IsFree() ) return false;
    const BotNetVar *owner = AgPB_FindEntityNetVar( weapon, "m_hOwner" );
    void *base = AgPB_EntityBase( weapon );
    return owner != NULL && owner->type == DPT_Int && base != NULL &&
        NetVar_GetHandle( base, *owner ) == AgPB_RefEHandle( bot.Edict() );
}

void OwnedInventory( const CAgPB &bot, CUtlVector<OwnedWeapon> &inventory, uintp &active )
{
    inventory.RemoveAll(); active = (uintp)INVALID_EHANDLE_INDEX;
    bot.GetNetVarHandleValue( "m_hActiveWeapon", &active );
    const BotNetVar *field = bot.FindNetVar( "m_hMyWeapons" );
    void *base = bot.NetVarBase();
    if ( base == NULL || field == NULL || field->type != DPT_Array ||
         field->elementType != DPT_Int || field->stride != sizeof( uintp ) ) return;
    for ( int i = 0; i < field->elements; ++i )
    {
        const uintp handle = NetVar_GetArrayHandle( base, *field, i );
        if ( !NetVar_HandleIsValid( handle ) ) continue;
        edict_t *weapon = bot.HandleToEdict( handle );
        if ( !OwnedBy( bot, weapon ) ) continue;
        const char *name = weapon->GetClassName();
        const WeaponInfo *info = FindWeaponInfo( name );
        if ( info == NULL ) continue;
        bool duplicate = false;
        for ( int j = 0; j < inventory.Count(); ++j )
            if ( inventory[j].candidate.key == handle ) duplicate = true;
        if ( duplicate ) continue;
        OwnedWeapon item;
        item.name = name; item.candidate.key = handle; item.candidate.weapon = info->weapon;
        if ( info->weapon != Weapon::Knife )
        {
            const BotNetVar *clip = AgPB_FindEntityNetVar( weapon, "m_iClip1" );
            // The public clip proxy transmits stored clip plus one.
            if ( clip != NULL && clip->type == DPT_Int )
                item.candidate.clip = AgPB_ReadNetVarInt( weapon, "m_iClip1", 0 ) - 1;
            const int ammo = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
            if ( ammo >= 0 ) item.candidate.reserve = bot.GetNetVarArrayInt( "m_iAmmo", ammo, -1 );
            // [game-diff] CS:S-family stock data. CS:GO has no confirmed table.
#if defined( AGPB_GAME_CSCZS ) || defined( AGPB_GAME_CSS )
            item.candidate.maxClip = info->stockClip;
#endif
        }
        inventory.AddToTail( item );
    }
}

bool ReloadReady( const CAgPB &bot, edict_t *weapon, float now )
{
    const BotNetVar *playerAttack = bot.FindNetVar( "m_flNextAttack" );
    const BotNetVar *weaponAttack = AgPB_FindEntityNetVar( weapon, "m_flNextPrimaryAttack" );
    if ( playerAttack == NULL || playerAttack->type != DPT_Float || weaponAttack == NULL || weaponAttack->type != DPT_Float ) return false;
    const float playerTime = bot.GetNetVarFloat( "m_flNextAttack" );
    const float weaponTime = AgPB_ReadNetVarFloat( weapon, "m_flNextPrimaryAttack" );
    return std::isfinite( playerTime ) && std::isfinite( weaponTime ) && now >= playerTime && now >= weaponTime;
}

int ReadSilencerState( edict_t *weapon )
{
    const WeaponInfo *info = FindWeaponInfo( weapon != NULL ? weapon->GetClassName() : NULL );
    if ( info == NULL || ( info->weapon != Weapon::USP && info->weapon != Weapon::M4A1 ) ) return -1;
    const BotNetVar *field = AgPB_FindEntityNetVar( weapon, "m_bSilencerOn" );
    const void *base = AgPB_EntityBase( weapon );
    return field != NULL && field->type == DPT_Int && base != NULL ? NetVar_GetInt( base, *field ) : -1;
}

bool SilencerTimersReady( edict_t *weapon, float now )
{
    const BotNetVar *secondary = AgPB_FindEntityNetVar( weapon, "m_flNextSecondaryAttack" );
    const BotNetVar *switchDone = AgPB_FindEntityNetVar( weapon, "m_flDoneSwitchingSilencer" );
    const void *base = AgPB_EntityBase( weapon );
    if ( base == NULL || secondary == NULL || secondary->type != DPT_Float ||
         switchDone == NULL || switchDone->type != DPT_Float ) return false;
    const float nextAttack = NetVar_GetFloat( base, *secondary );
    const float done = NetVar_GetFloat( base, *switchDone );
    return std::isfinite( nextAttack ) && std::isfinite( done ) && now > nextAttack && now >= done;
}

bool NeedsIdleReload( const AgPBWeaponSelect::Candidate &weapon, const AgPBWeaponSelect::Input &input,
                      bool continuing )
{
    if ( !AgPBWeaponSelect::Firearm( weapon.weapon ) || !AgPBWeaponSelect::Allowed( weapon, input ) ||
         weapon.clip < 0 || weapon.reserve <= 0 )
        return false;
    if ( weapon.maxClip <= 0 ) return weapon.clip == 0;
    // Once started, finish reloading, including shell-fed shotguns.
    return continuing ? weapon.clip < weapon.maxClip : weapon.clip * 5 < weapon.maxClip * 4;
}
}

AgPBWeaponSelect::Weapon CAgPB::ClassifyWeapon( edict_t *weapon )
{
    const WeaponInfo *info = FindWeaponInfo( weapon != NULL ? weapon->GetClassName() : NULL );
    return info != NULL ? info->weapon : Weapon::Unknown;
}

void CAgPB::ResetWeaponSelection( const char *reason )
{
    AgPBWeaponSelect::Reset( m_WeaponSelection );
    m_uIdleReloadKey = 0;
    m_flIdleReloadDeadline = m_flIdleReloadRetryAt = 0.0f;
    m_bCombatReloadRequested = false;
    m_iSilencerCheckTick = -1;
    m_uWeaponSelected = (uintp)INVALID_EHANDLE_INDEX;
    m_pszWeaponSelection = reason;
}

bool CAgPB::UpdateWeaponSelection( CGlobalVars *globals, CBotCmd &cmd, edict_t *visibleEnemy, float distance,
                                    bool jumpApproach )
{
    const AgPBTask &task = m_Tasks.Current();
    const bool intent = m_bAutonomous || m_bRoam || HasRoute() || task.id != AgPBTaskId::Normal;
    const AgPBGameMode mode = AgPB_GameMode();
    const AgPBZRRole zmRole = mode == AgPB_MODE_ZM && m_bAutonomous && m_pInfo != NULL ?
        AgPB_ZRRoleForUserId( m_pInfo->GetUserID() ) : AgPBZRRole::Unknown;
    const bool zmZombie = zmRole == AgPBZRRole::Zombie;
    const bool ordinary = task.id == AgPBTaskId::Normal || task.id == AgPBTaskId::MoveToPosition ||
        task.id == AgPBTaskId::Investigate || task.id == AgPBTaskId::Camp || task.id == AgPBTaskId::Pause ||
        task.id == AgPBTaskId::AgentIntention ||
        ( task.id == AgPBTaskId::PickupWeapon && visibleEnemy != NULL );
    unsigned int leg = 0, next = 0;
    if ( m_iRouteIndex > 0 ) BotWaypoints().IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
    BotWaypoints().IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &next );
    const bool jumpRoute = ( ( leg | next ) & AgPB_PATH_JUMP ) != 0;
    if ( jumpApproach != jumpRoute ) return false;

    m_bCombatReloadRequested = false;
    m_iSilencerCheckTick = -1;
    const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
    const int flags = GetNetVarInt( "m_fFlags" );
    if ( globals == NULL || !IsAlive() || !intent || !ordinary || !m_bCombatEnabled || !agpb_combat.GetBool() ||
         ( mode != AgPB_MODE_NORMAL && zmRole == AgPBZRRole::Unknown ) || AgPB_IsRoundFrozen() || ( flags & FL_FROZEN ) != 0 ||
         ( flags & FL_ONGROUND ) == 0 || GetNetVarInt( "movetype" ) == MOVETYPE_LADDER ||
         ( node != NULL && ( node->flags & AgPB_WP_LADDER ) != 0 ) ||
         NetVar_HandleIsValid( m_uBreakHandle ) || ( cmd.buttons & ( IN_USE | IN_JUMP ) ) != 0 || cmd.weaponselect != 0 ||
         ( task.deadline > 0.0f && globals->curtime >= task.deadline ) )
    { ResetWeaponSelection( "priority-or-inactive" ); return false; }

    const bool holdCrouch = ( flags & FL_DUCKING ) != 0 || TaskRequiresCrouch();

    CUtlVector<OwnedWeapon> owned;
    uintp active;
    OwnedInventory( *this, owned, active );
    CUtlVector<AgPBWeaponSelect::Candidate> candidates;
    AgPBWeaponSelect::Input input;
    input.now = globals->curtime; input.enemyVisible = visibleEnemy != NULL; input.distance = distance;
    input.underwater = GetNetVarInt( "m_nWaterLevel" ) >= 2;
    // Jump traversal owns the command and doesn't run a fresh combat scan.
    if ( jumpApproach && m_Search.visual.valid && input.now - m_Search.visual.observedAt <= 0.25f )
        return false;
    const KnifeIntent knifeIntent = mode == AgPB_MODE_ZM ? KnifeIntent::None :
        ChooseKnifeIntent( globals, cmd, input.enemyVisible, visibleEnemy, HandleToEdict( active ) );
    for ( int i = 0; i < owned.Count(); ++i )
    {
        candidates.AddToTail( owned[i].candidate );
        if ( owned[i].candidate.key == active ) input.current = i;
        if ( owned[i].candidate.weapon == Weapon::Knife && ( zmZombie || knifeIntent != KnifeIntent::None ) )
            input.forcedKey = owned[i].candidate.key;
    }
    if ( zmZombie && input.forcedKey == 0 ) return false;
    if ( jumpApproach && ( knifeIntent != KnifeIntent::Jump || input.forcedKey == 0 ) )
        return false;

    // Check the primary before the sidearm. A fresh enemy observation
    // releases any pending idle reload choice.
    const bool recentThreat = ( m_Search.visual.valid && input.now - m_Search.visual.observedAt <= 5.0f ) ||
                              ( m_Search.sound.valid && input.now - m_Search.sound.observedAt <= 5.0f );
    if ( input.enemyVisible || recentThreat || input.forcedKey != 0 )
        m_uIdleReloadKey = 0;
    else
    {
        int pending = 0;
        while ( pending < candidates.Count() && candidates[pending].key != m_uIdleReloadKey )
            ++pending;
        if ( pending == candidates.Count() || !NeedsIdleReload( candidates[pending], input, true ) )
            m_uIdleReloadKey = 0;
        else if ( input.now >= m_flIdleReloadDeadline )
        {
            m_uIdleReloadKey = 0;
            m_flIdleReloadRetryAt = input.now + 3.0f;
        }
        if ( m_uIdleReloadKey == 0 && input.now >= m_flIdleReloadRetryAt )
        {
            int bestRank = -1;
            for ( int i = 0; i < candidates.Count(); ++i )
            {
                const AgPBWeaponSelect::Candidate &candidate = candidates[i];
                if ( !NeedsIdleReload( candidate, input, false ) ) continue;
                const int rank = AgPBWeaponSelect::Preference( candidate.weapon );
                if ( rank <= bestRank ) continue;
                bestRank = rank;
                m_uIdleReloadKey = candidate.key;
            }
            if ( m_uIdleReloadKey != 0 ) m_flIdleReloadDeadline = input.now + 8.0f;
        }
        input.forcedKey = m_uIdleReloadKey;
    }

    const AgPBWeaponSelect::Selection choice = AgPBWeaponSelect::Step( m_WeaponSelection, candidates.Base(), candidates.Count(), input );
    m_pszWeaponSelection = AgPBWeaponSelect::ReasonName( choice.reason );
    m_uWeaponSelected = choice.index >= 0 ? (uintp)candidates[choice.index].key : (uintp)INVALID_EHANDLE_INDEX;
    if ( choice.switchNow )
    {
        // Re-resolve the complete handle and owner at command submission.
        edict_t *weapon = HandleToEdict( m_uWeaponSelected );
        if ( !OwnedBy( *this, weapon ) ) { ResetWeaponSelection( "ownership-changed" ); return false; }
        cmd.weaponselect = m_Ctx.pEngine->IndexOfEdict( weapon );
        ++m_uWeaponSelectCommands; m_iWeaponSelectTick = globals->tickcount;
    }
    if ( choice.waiting )
    {
        if ( knifeIntent == KnifeIntent::Spawn || knifeIntent == KnifeIntent::Jump )
            return false; // Travel and takeoff alignment keep their movement owner.

        cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE );
        cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
        if ( holdCrouch ) cmd.buttons |= IN_DUCK;
        m_flRouteSettleSince = -1.0f;
        m_pszCombatState = "weapon-switch";
        return true;
    }
    m_bCombatReloadRequested = input.enemyVisible && choice.reload;
    if ( choice.reload && !input.enemyVisible && input.now >= m_flIdleReloadRetryAt )
    {
        edict_t *weapon = HandleToEdict( m_uWeaponSelected );
        if ( OwnedBy( *this, weapon ) && ReloadReady( *this, weapon, input.now ) ) cmd.buttons |= IN_RELOAD;
        cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_USE );
        cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
        if ( holdCrouch ) cmd.buttons |= IN_DUCK;
        m_flRouteSettleSince = -1.0f;
        m_pszWeaponSelection = "idle-reload";
        m_pszCombatState = "idle-reload";
        return true;
    }
    if ( !jumpApproach && !input.enemyVisible && !choice.reload && knifeIntent == KnifeIntent::None )
        m_iSilencerCheckTick = globals->tickcount;
    return false;
}

void CAgPB::UpdateSilencer( CGlobalVars *globals, CBotCmd &cmd )
{
    // ZBot checks the held weapon after reloading and waits 3.5 seconds after sighting an enemy.
    // The ordinary weapon pass above supplies a fresh no-target check before we add the button.
    const AgPBTask &task = m_Tasks.Current();
    if ( m_iSilencerCheckTick != globals->tickcount || cmd.weaponselect != 0 ||
         ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP ) ) != 0 ||
         ( m_LastSubmittedCmd.buttons & IN_ATTACK2 ) != 0 || m_Ladder.Active() ||
         NetVar_HandleIsValid( m_uBreakHandle ) || task.desire >= AgPBTaskPriority::Objective ||
         task.id == AgPBTaskId::PickupWeapon ||
         GetNetVarInt( "m_bIsDefusing" ) != 0 || GetNetVarInt( "m_bHasShield" ) != 0 )
        return;

    const float now = globals->curtime;
    const float lastSeen = m_Search.visual.observedAt;
    // Keep the wait even when target death invalidates the seek record. Local hearing
    // also reports nearby enemies without inspecting an occluded player's position.
    if ( ( lastSeen >= 0.0f && now - lastSeen < 3.5f ) ||
         ( AgPBSearch::Alive( m_Search.sound, now ) && now - m_Search.sound.observedAt < 3.0f ) )
        return;

    uintp active;
    if ( !GetNetVarHandleValue( "m_hActiveWeapon", &active ) ) return;
    edict_t *weapon = HandleToEdict( active );
    if ( !OwnedBy( *this, weapon ) ) return;
    const int state = ReadSilencerState( weapon );
    const int desired = ( m_bPrefersSilencer || m_iCombatSkill > 70 ) ? 1 : 0;
    // USP/M4A1 reloads advance the player and weapon attack timers. Read those
    // and the silencer timers before requesting a single secondary-attack edge.
    if ( ( state == 0 || state == 1 ) && state != desired && ReloadReady( *this, weapon, now ) &&
         SilencerTimersReady( weapon, now ) )
        cmd.buttons |= IN_ATTACK2;
}

void CAgPB::ReportWeaponSelection() const
{
    CUtlVector<OwnedWeapon> owned;
    uintp active;
    OwnedInventory( *this, owned, active );
    RouteReport( "weapons v=1 state=%s active=0x%llX selected=0x%llX pending=0x%llX pending-until=%.3f cooldown-until=%.3f failed=0x%llX retry-until=%.3f requests=%u requesttick=%d count=%d",
        m_pszWeaponSelection, (unsigned long long)active, (unsigned long long)m_uWeaponSelected,
        (unsigned long long)m_WeaponSelection.pendingKey, m_WeaponSelection.pendingUntil,
        m_WeaponSelection.nextSwitchAt, (unsigned long long)m_WeaponSelection.failedKey, m_WeaponSelection.failedUntil,
        m_uWeaponSelectCommands, m_iWeaponSelectTick, owned.Count() );
    edict_t *weapon = HandleToEdict( active );
    RouteReport( "silencer: preference=%d desired=%d active-state=%d",
        m_bPrefersSilencer ? 1 : 0, ( m_bPrefersSilencer || m_iCombatSkill > 70 ) ? 1 : 0,
        OwnedBy( *this, weapon ) ? ReadSilencerState( weapon ) : -1 );
    RouteReport( "grenade: state=%s weapon=0x%llX confirmed=%u retry-at=%.3f",
        m_pszGrenadeState, (unsigned long long)m_Grenade.weapon, m_uConfirmedGrenades, m_flNextGrenadeCheck );
    for ( int i = 0; i < owned.Count(); ++i )
        RouteReport( "owned weapon: class=%s handle=0x%llX entry=%d active=%d clip=%d reserve=%d preference=%d",
            owned[i].name, (unsigned long long)owned[i].candidate.key, NetVar_HandleEntry( (uintp)owned[i].candidate.key ),
            owned[i].candidate.key == active ? 1 : 0, owned[i].candidate.clip, owned[i].candidate.reserve,
            AgPBWeaponSelect::Preference( owned[i].candidate.weapon ) );
}
