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
    "Log submitted weapon-select/reload commands and engine equipment readback." );

namespace {
using AgPBWeaponSelect::Weapon;
struct WeaponName { const char *name; Weapon weapon; };
const WeaponName names[] = {
    { "weapon_knife", Weapon::Knife }, { "weapon_usp", Weapon::USP },
    { "weapon_glock", Weapon::Glock }, { "weapon_deagle", Weapon::Deagle },
    { "weapon_p228", Weapon::P228 }, { "weapon_elite", Weapon::Elite },
    { "weapon_fiveseven", Weapon::FiveSeven }, { "weapon_m3", Weapon::M3 },
    { "weapon_xm1014", Weapon::XM1014 }, { "weapon_mp5navy", Weapon::MP5 },
    { "weapon_tmp", Weapon::TMP }, { "weapon_p90", Weapon::P90 },
    { "weapon_mac10", Weapon::MAC10 }, { "weapon_ump45", Weapon::UMP45 },
    { "weapon_ak47", Weapon::AK47 }, { "weapon_sg552", Weapon::SG552 },
    { "weapon_m4a1", Weapon::M4A1 }, { "weapon_galil", Weapon::Galil },
    { "weapon_famas", Weapon::Famas }, { "weapon_aug", Weapon::AUG },
    { "weapon_scout", Weapon::Scout }, { "weapon_awp", Weapon::AWP },
    { "weapon_g3sg1", Weapon::G3SG1 }, { "weapon_sg550", Weapon::SG550 },
    { "weapon_m249", Weapon::M249 }
};
Weapon ClassWeapon( const char *name )
{
    if ( name != NULL ) for ( const WeaponName &entry : names )
        if ( V_stricmp( name, entry.name ) == 0 ) return entry.weapon;
    return Weapon::Unknown;
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
        const Weapon kind = ClassWeapon( name );
        if ( kind == Weapon::Unknown ) continue;
        bool duplicate = false;
        for ( int j = 0; j < inventory.Count(); ++j )
            if ( inventory[j].candidate.key == handle ) duplicate = true;
        if ( duplicate ) continue;
        OwnedWeapon item;
        item.name = name; item.candidate.key = handle; item.candidate.weapon = kind;
        if ( kind != Weapon::Knife )
        {
            const BotNetVar *clip = AgPB_FindEntityNetVar( weapon, "m_iClip1" );
            // The public clip proxy transmits stored clip plus one.
            if ( clip != NULL && clip->type == DPT_Int )
                item.candidate.clip = AgPB_ReadNetVarInt( weapon, "m_iClip1", 0 ) - 1;
            const int ammo = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
            if ( ammo >= 0 ) item.candidate.reserve = bot.GetNetVarArrayInt( "m_iAmmo", ammo, -1 );
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
}

AgPBWeaponSelect::Weapon CAgPB::ClassifyWeapon( edict_t *weapon )
{
    return ClassWeapon( weapon != NULL ? weapon->GetClassName() : NULL );
}

void CAgPB::ResetWeaponSelection( const char *reason )
{
    AgPBWeaponSelect::Reset( m_WeaponSelection );
    m_uWeaponSelected = (uintp)INVALID_EHANDLE_INDEX;
    m_pszWeaponSelection = reason;
}

bool CAgPB::UpdateWeaponSelection( CGlobalVars *globals, CBotCmd &cmd, bool enemyVisible, float distance )
{
    const AgPBTask &task = m_Tasks.Current();
    const bool intent = m_bAutonomous || m_bRoam || HasRoute() || task.id != AgPBTaskId::Normal;
    const bool ordinary = task.id == AgPBTaskId::Normal || task.id == AgPBTaskId::MoveToPosition ||
        task.id == AgPBTaskId::Investigate || task.id == AgPBTaskId::Camp || task.id == AgPBTaskId::Pause ||
        task.id == AgPBTaskId::AgentIntention;
    unsigned int leg = 0, next = 0;
    if ( m_iRouteIndex > 0 ) BotWaypoints().IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &leg );
    BotWaypoints().IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &next );
    const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
    const int flags = GetNetVarInt( "m_fFlags" );
    if ( globals == NULL || !IsAlive() || !intent || !ordinary || !m_bCombatEnabled || !agpb_combat.GetBool() ||
         AgPB_GameMode() != AgPB_MODE_NORMAL || AgPB_IsRoundFrozen() || ( flags & FL_FROZEN ) != 0 ||
         ( flags & FL_ONGROUND ) == 0 || GetNetVarInt( "movetype" ) == MOVETYPE_LADDER ||
         ( ( leg | next ) & AgPB_PATH_JUMP ) != 0 || ( node != NULL && ( node->flags & AgPB_WP_LADDER ) != 0 ) ||
         NetVar_HandleIsValid( m_uBreakHandle ) || ( cmd.buttons & ( IN_USE | IN_JUMP ) ) != 0 || cmd.weaponselect != 0 ||
         ( task.deadline > 0.0f && globals->curtime >= task.deadline ) )
    { ResetWeaponSelection( "priority-or-inactive" ); return false; }

    const bool holdCrouch = ( flags & FL_DUCKING ) != 0 || TaskRequiresCrouch();

    CUtlVector<OwnedWeapon> owned;
    uintp active;
    OwnedInventory( *this, owned, active );
    CUtlVector<AgPBWeaponSelect::Candidate> candidates;
    AgPBWeaponSelect::Input input;
    input.now = globals->curtime; input.enemyVisible = enemyVisible; input.distance = distance;
    input.underwater = GetNetVarInt( "m_nWaterLevel" ) >= 3;
    for ( int i = 0; i < owned.Count(); ++i )
    {
        candidates.AddToTail( owned[i].candidate );
        if ( owned[i].candidate.key == active ) input.current = i;
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
        cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE );
        cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
        if ( holdCrouch ) cmd.buttons |= IN_DUCK;
        m_flRouteSettleSince = -1.0f;
        m_pszCombatState = "weapon-switch";
        return true;
    }
    if ( choice.reload && !enemyVisible )
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
    return false;
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
    for ( int i = 0; i < owned.Count(); ++i )
        RouteReport( "owned weapon: class=%s handle=0x%llX entry=%d active=%d clip=%d reserve=%d preference=%d",
            owned[i].name, (unsigned long long)owned[i].candidate.key, NetVar_HandleEntry( (uintp)owned[i].candidate.key ),
            owned[i].candidate.key == active ? 1 : 0, owned[i].candidate.clip, owned[i].candidate.reserve,
            AgPBWeaponSelect::Preference( owned[i].candidate.weapon ) );
}
