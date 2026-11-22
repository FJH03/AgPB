// AgPB SyPB normal-mode combat with read-only Source physics checks. GPL-3.0-only.
//
// Behavior is adapted from SyPB source/combat.cpp ResetCheckEnemy,
// LookupEnemy, GetAimPosition, FocusEnemy and FireWeapon, with the matching
// CS-EBOT source/combat.cpp target/visibility/firing paths compared. Zombie
// branches and hidden-target acquisition are not included. Source-verified
// penetration checks apply only to a currently visually sensed enemy. Last-observed
// memory and bounded authored-waypoint investigation live in bot_search.cpp.
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
#include <filesystem.h>
#include <tier1/KeyValues.h>
#include <mathlib/IceKey.H>
#include <vphysics_interface.h>
#include "plugin.h"
#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "obstacle.h"
#include "waypoint.h"
#include "combat_skill.h"
#include "combat_visibility.h"
#include "bullet_penetration.h"
#include "movement_control.h"

extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();
extern IFileSystem *filesystem;
extern IPhysicsSurfaceProps *AgPB_SurfaceProps();
extern unsigned int AgPB_MapSerial();

ConVar agpb_combat( "agpb_combat", "1", FCVAR_GAMEDLL,
    "Enable local weapon combat in normal and ZM profiles; 0 leaves navigation running." );
ConVar agpb_difficulty( "agpb_difficulty", "3", FCVAR_GAMEDLL,
    "SyPB creation difficulty: -1=skill1-100, 0=1-29, 1=30-49, 2=50-79, 3=80-99, 4=100." );
static ConVar agpb_combat_debug( "agpb_combat_debug", "0", FCVAR_GAMEDLL,
    "Log combat target changes and bounded bullet feasibility decisions." );
static ConVar agpb_strafe_log( "agpb_strafe_log", "0", FCVAR_GAMEDLL,
    "Sample combat strafe decisions and executed movement: 0 off, -1 all, positive client slot." );

namespace {
const char *StrafePlanName( AgPBCombatMove::StrafePlan plan )
{
    using AgPBCombatMove::StrafePlan;
    switch ( plan )
    {
    case StrafePlan::Distance: return "distance";
    case StrafePlan::View: return "view";
    case StrafePlan::Reload: return "reload";
    case StrafePlan::Crouch: return "crouch";
    case StrafePlan::Retreat: return "retreat";
    case StrafePlan::Moving: return "strafe";
    case StrafePlan::Precision: return "precision-stop";
    default: return "weapon";
    }
}

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

edict_t *OwnedActiveWeapon( const CAgPB &bot, uintp &handle )
{
    handle = (uintp)INVALID_EHANDLE_INDEX;
    if ( !bot.GetNetVarHandleValue( "m_hActiveWeapon", &handle ) ) return NULL;
    edict_t *weapon = bot.HandleToEdict( handle );
    if ( weapon == NULL ) return NULL;
    const BotNetVar *owner = AgPB_FindEntityNetVar( weapon, "m_hOwner" );
    void *base = AgPB_EntityBase( weapon );
    return owner != NULL && owner->type == DPT_Int && base != NULL &&
        NetVar_GetHandle( base, *owner ) == AgPB_RefEHandle( bot.Edict() ) ? weapon : NULL;
}

AgPBCombatMove::Weapon CombatWeaponKind( edict_t *weapon )
{
    using AgPBCombatMove::Weapon;
    if ( NamedWeapon( weapon, "weapon_knife" ) ) return Weapon::Knife;
    if ( SniperWeapon( weapon ) ) return Weapon::Sniper;
    if ( NamedWeapon( weapon, "weapon_m3" ) || NamedWeapon( weapon, "weapon_xm1014" ) ) return Weapon::Shotgun;
    if ( NamedWeapon( weapon, "weapon_mp5navy" ) || NamedWeapon( weapon, "weapon_tmp" ) ||
         NamedWeapon( weapon, "weapon_p90" ) || NamedWeapon( weapon, "weapon_mac10" ) ||
         NamedWeapon( weapon, "weapon_ump45" ) ) return Weapon::SMG;
    if ( NamedWeapon( weapon, "weapon_m249" ) ) return Weapon::MachineGun;
    if ( NamedWeapon( weapon, "weapon_usp" ) || NamedWeapon( weapon, "weapon_glock" ) ||
         NamedWeapon( weapon, "weapon_deagle" ) || NamedWeapon( weapon, "weapon_p228" ) ||
         NamedWeapon( weapon, "weapon_fiveseven" ) || NamedWeapon( weapon, "weapon_elite" ) ) return Weapon::Pistol;
    if ( NamedWeapon( weapon, "weapon_m4a1" ) || NamedWeapon( weapon, "weapon_ak47" ) ||
         NamedWeapon( weapon, "weapon_aug" ) || NamedWeapon( weapon, "weapon_sg552" ) ||
         NamedWeapon( weapon, "weapon_famas" ) || NamedWeapon( weapon, "weapon_galil" ) ) return Weapon::Rifle;
    return Weapon::Unknown;
}

bool ReadFloat( edict_t *entity, const char *name, float &value )
{
    const BotNetVar *field = AgPB_FindEntityNetVar( entity, name );
    if ( field == NULL || field->type != DPT_Float ) return false;
    value = AgPB_ReadNetVarFloat( entity, name );
    return std::isfinite( value );
}

struct WeaponZoom {
    AgPBFiring::Zoom level = AgPBFiring::Zoom::Unknown;
    bool resuming = false;
    bool settling = false;
};

WeaponZoom ReadWeaponZoom( const CAgPB &bot, AgPBFiring::Weapon weapon, float now )
{
    using namespace AgPBFiring;
    WeaponZoom zoom;
    if ( !SupportsZoom( weapon ) || bot.GetNetVarInt( "m_bHasShield", 0 ) != 0 ) return zoom;
    zoom.level = ZoomForFov( weapon, bot.GetNetVarInt( "m_iFOV", -1 ), bot.GetNetVarInt( "m_iDefaultFOV", 90 ) );
    if ( zoom.level == Zoom::Unknown ) return zoom;
    // Native sniper reload and bolt cycling may temporarily show default FOV.
    zoom.resuming = zoom.level == Zoom::Unzoomed && ScopedSniper( weapon ) &&
        bot.GetNetVarInt( "m_bResumeZoom", 0 ) != 0;
    float changedAt, rate;
    if ( ReadFloat( bot.Edict(), "m_flFOVTime", changedAt ) &&
         ReadFloat( bot.Edict(), "m_flFOVRate", rate ) && rate > 0.0f )
    {
        // CSBaseGun uses a 0.05s rate for automatic rescope. Manual sniper
        // zoom accuracy takes longer than the visual interpolation.
        if ( IsZoomed( zoom.level ) && ScopedSniper( weapon ) && rate > 0.05f )
            rate = MAX( rate, BoltSniper( weapon ) ? 0.15f : 0.30f );
        zoom.settling = now < changedAt + rate;
    }
    return zoom;
}

bool SecondaryAttackReady( edict_t *weapon, float now )
{
    float secondaryAttack;
    return ReadFloat( weapon, "m_flNextSecondaryAttack", secondaryAttack ) && now >= secondaryAttack;
}

bool BurstSwitchReady( const CAgPB &bot, edict_t *weapon, const AgPBFiring::Input &fire, float playerAttack )
{
    if ( !AgPBFiring::SupportsBurstMode( fire.weapon ) || fire.now < playerAttack ||
         bot.GetNetVarInt( "m_bHasShield", 0 ) != 0 )
        return false;

    const int burst = AgPB_ReadNetVarInt( weapon, "m_bBurstMode", -1 );
    if ( ( burst != 0 && burst != 1 ) || !AgPBFiring::ToggleBurstMode( fire.weapon, fire.distance, burst == 1 ) ||
         AgPB_ReadNetVarInt( weapon, "m_iBurstShotsRemaining", -1 ) != 0 )
        return false;

    return SecondaryAttackReady( weapon, fire.now );
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
    explicit CombatTraceFilter( IHandleEntity *self, IHandleEntity *ignore = NULL ) : m_Self( self ), m_Ignore( ignore ) {}
    bool ShouldHitEntity( IHandleEntity *entity, int ) { return entity != m_Self && entity != m_Ignore; }
private:
    IHandleEntity *m_Self;
    IHandleEntity *m_Ignore;
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

bool VisibleSpot( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                  const Vector &eye, const Vector &point )
{
    Vector direction = point - eye;
    return point.IsValid() && VectorNormalize( direction ) >= 0.01f &&
        FirstHit( ctx, self, target, eye, point + direction * 8.0f );
}

// Physics below mirrors only the verified current game source, never its AI.
// cs_player_shared.cpp FireBullet/TraceToExit/GetMaterialParameters and
// fx_cs_shared.cpp supply the rules; all entity/material queries remain public.
static const unsigned int BulletTraceMask = MASK_SOLID | CONTENTS_DEBRIS | CONTENTS_HITBOX;

IHandleEntity *EntityHandle( edict_t *entity )
{
    IServerNetworkable *network = entity != NULL && !entity->IsFree() && entity->GetUnknown() != NULL
        ? entity->GetNetworkable() : NULL;
    return network != NULL ? network->GetEntityHandle() : NULL;
}

KeyValues *LoadBulletScript( const char *name )
{
#if defined( AGPB_GAME_CSCZS )
    // [game-diff] The current CSCZS source's script format and ICE key are
    // verified in weapon_parse.cpp and cs_gamerules.h. Other target sources
    // have not been verified for this read-only penetration estimator.
    if ( filesystem == NULL ) return NULL;
    KeyValues *data = new KeyValues( "WeaponDatafile" );
    char path[128]; V_snprintf( path, sizeof( path ), "scripts/%s.txt", name );
    if ( data->LoadFromFile( filesystem, path, "MOD" ) ) return data;
    V_snprintf( path, sizeof( path ), "scripts/%s.ctx", name );
    FileHandle_t file = filesystem->Open( path, "rb", "MOD" );
    if ( file == FILESYSTEM_INVALID_HANDLE ) { data->deleteThis(); return NULL; }
    const int size = filesystem->Size( file );
    CUtlVector<unsigned char> buffer;
    if ( size <= 0 || size > 1024 * 1024 ) { filesystem->Close( file ); data->deleteThis(); return NULL; }
    buffer.SetCount( size + 1 );
    const int read = filesystem->Read( buffer.Base(), size, file );
    filesystem->Close( file );
    if ( read != size ) { data->deleteThis(); return NULL; }
    IceKey ice( 0 ); ice.set( (const unsigned char *)"d7NSuLq2" );
    const int blockSize = ice.blockSize();
    if ( blockSize != 8 ) { data->deleteThis(); return NULL; }
    for ( int at = 0; at + blockSize <= size; at += blockSize )
    {
        unsigned char decoded[8]; ice.decrypt( buffer.Base() + at, decoded );
        V_memcpy( buffer.Base() + at, decoded, sizeof( decoded ) );
    }
    buffer[size] = 0;
    if ( data->LoadFromBuffer( path, (const char *)buffer.Base(), filesystem, "MOD" ) ) return data;
    data->deleteThis();
#else
    (void)name;
#endif
    return NULL;
}

bool ReadBulletProfile( edict_t *weapon, AgPBBullet::Profile &profile, const char **ammo = NULL )
{
    if ( ammo != NULL ) *ammo = "unknown";
    const char *name = weapon != NULL ? weapon->GetClassName() : NULL;
    if ( name == NULL || Q_strnicmp( name, "weapon_", 7 ) != 0 || Q_strlen( name ) >= 64 ) return false;
    struct CachedProfile { char name[64], ammo[64]; bool valid; AgPBBullet::Profile profile; };
    static CUtlVector<CachedProfile> cache;
    static unsigned int mapSerial = 0;
    if ( mapSerial != AgPB_MapSerial() ) { cache.RemoveAll(); mapSerial = AgPB_MapSerial(); }
    int index = -1;
    for ( int i = 0; i < cache.Count(); ++i ) if ( Q_stricmp( cache[i].name, name ) == 0 ) { index = i; break; }
    if ( index < 0 )
    {
        CachedProfile entry;
        V_strncpy( entry.name, name, sizeof( entry.name ) ); V_strncpy( entry.ammo, "unknown", sizeof( entry.ammo ) ); entry.valid = false;
        KeyValues *data = LoadBulletScript( name );
        if ( data != NULL )
        {
            struct AmmoName { const char *name; AgPBBullet::Ammo type; };
            const AmmoName names[] = {
                { "BULLET_PLAYER_50AE", AgPBBullet::Ammo::AE50 }, { "BULLET_PLAYER_762MM", AgPBBullet::Ammo::MM762 },
                { "BULLET_PLAYER_556MM", AgPBBullet::Ammo::MM556 }, { "BULLET_PLAYER_556MM_BOX", AgPBBullet::Ammo::MM556 },
                { "BULLET_PLAYER_338MAG", AgPBBullet::Ammo::Mag338 }, { "BULLET_PLAYER_9MM", AgPBBullet::Ammo::MM9 },
                { "BULLET_PLAYER_BUCKSHOT", AgPBBullet::Ammo::Buckshot }, { "BULLET_PLAYER_45ACP", AgPBBullet::Ammo::ACP45 },
                { "BULLET_PLAYER_357SIG", AgPBBullet::Ammo::Sig357 }, { "BULLET_PLAYER_57MM", AgPBBullet::Ammo::MM57 }
            };
            const char *ammoName = data->GetString( "primary_ammo", "None" );
            V_strncpy( entry.ammo, ammoName, sizeof( entry.ammo ) );
            AgPBBullet::Ammo ammo = AgPBBullet::Ammo::Unknown;
            for ( const AmmoName &known : names ) if ( Q_stricmp( ammoName, known.name ) == 0 ) { ammo = known.type; break; }
            const AgPBBullet::AmmoParameters parameters = AgPBBullet::AmmoProfile( ammo );
            entry.profile.power = parameters.power; entry.profile.penetrationDistance = parameters.distance;
            entry.profile.count = data->GetInt( "Penetration", 1 ); entry.profile.damage = float( data->GetInt( "Damage", 42 ) );
            entry.profile.range = data->GetFloat( "Range", 8192.0f ); entry.profile.rangeModifier = data->GetFloat( "RangeModifier", 0.98f );
            entry.valid = ammo != AgPBBullet::Ammo::Unknown && entry.profile.count >= 0 && entry.profile.count <= 16 &&
                std::isfinite( entry.profile.range ) && entry.profile.range > 0.0f && entry.profile.range <= MAX_TRACE_LENGTH &&
                std::isfinite( entry.profile.damage ) && entry.profile.damage > 0.0f &&
                std::isfinite( entry.profile.rangeModifier ) && entry.profile.rangeModifier > 0.0f && entry.profile.rangeModifier <= 1.0f;
            data->deleteThis();
        }
        index = cache.AddToTail( entry );
        if ( agpb_combat_debug.GetBool() )
            META_CONPRINTF( "[AgPB] penetration profile %s: %s range=%.0f damage=%.1f count=%d power=%.1f distance=%.0f\n",
                name, entry.valid ? "loaded" : "unavailable", entry.profile.range, entry.profile.damage,
                entry.profile.count, entry.profile.power, entry.profile.penetrationDistance );
    }
    if ( ammo != NULL ) *ammo = cache[index].ammo;
    if ( !cache[index].valid ) return false;
    profile = cache[index].profile;
    const BotNetVar *modeField = AgPB_FindEntityNetVar( weapon, "m_weaponMode" );
    if ( modeField == NULL || modeField->type != DPT_Int ) return false;
    const bool secondary = AgPB_ReadNetVarInt( weapon, "m_weaponMode", 0 ) != 0;
    if ( secondary )
    {
        // Glock PrimaryAttack passes Primary_Mode even in burst setting;
        // its later scheduled FireBurst rounds own the secondary18/.9 change.
        if ( NamedWeapon( weapon, "weapon_m4a1" ) ) profile.rangeModifier = 0.95f;
        else if ( NamedWeapon( weapon, "weapon_usp" ) ) profile.damage = 30.0f;
    }
    return true;
}

void TraceBulletSegment( const BotEngineContext &ctx, edict_t *self, const Vector &start,
                         const Vector &end, trace_t &trace )
{
    CombatTraceFilter filter( EntityHandle( self ) );
    Ray_t ray; ray.Init( start, end );
    ctx.pTrace->TraceRay( ray, BulletTraceMask, &filter, &trace );
    // Match FireBullet's public ClipTraceToPlayers step for hitboxes extending
    // outside collision bounds. No target search/memory is refreshed here.
    Vector direction = end - start;
    const float length = VectorNormalize( direction );
    Ray_t playerRay; playerRay.Init( start, end + direction * 40.0f );
    for ( int i = 1; gpGlobals != NULL && i <= gpGlobals->maxClients; ++i )
    {
        edict_t *player = ctx.pEngine->PEntityOfEntIndex( i );
        if ( player == NULL || player == self || player->IsFree() ) continue;
        IPlayerInfo *info = ctx.pPlayerInfoManager->GetPlayerInfo( player );
        IHandleEntity *handle = EntityHandle( player );
        if ( info == NULL || !info->IsPlayer() || !info->IsConnected() || info->IsDead() || handle == NULL ) continue;
        const Vector center = info->GetAbsOrigin() + ( info->GetPlayerMins() + info->GetPlayerMaxs() ) * 0.5f;
        const float along = DotProduct( center - start, direction );
        if ( along < 0.0f || along > length + 40.0f ||
             ( center - start - direction * along ).LengthSqr() > 60.0f * 60.0f ) continue;
        trace_t playerTrace;
        ctx.pTrace->ClipRayToEntity( playerRay, BulletTraceMask, handle, &playerTrace );
        // The player clip ray is40 units longer. Compare physical distances
        // and restore the primary fraction so extension normalization cannot
        // promote a player just behind an opaque wall ahead of that wall.
        if ( AgPBCombatVisibility::PlayerClipIsCloser( playerTrace.fraction, length + 40.0f, trace.fraction, length ) )
        { playerTrace.fraction *= ( length + 40.0f ) / length; trace = playerTrace; }
    }
}

bool SurfaceMaterial( IPhysicsSurfaceProps *properties, const trace_t &trace, int &material )
{
    if ( properties == NULL || trace.surface.surfaceProps < 0 ||
         trace.surface.surfaceProps >= properties->SurfacePropCount() ) return false;
    const surfacedata_t *surface = properties->GetSurfaceData( trace.surface.surfaceProps );
    if ( surface == NULL ) return false;
    material = surface->game.material;
    return true;
}

struct BulletReadback
{
    uintp owner = (uintp)INVALID_EHANDLE_INDEX, target = (uintp)INVALID_EHANDLE_INDEX;
    const char *reason = "none";
    bool allowed = false, requireTarget = false, knownProfile = false;
    int walls = 0, enterMaterial = 0, exitMaterial = 0;
    float thickness = 0.0f, damage = 0.0f, observedAt = -1.0f, nextLog = 0.0f;
};
static BulletReadback s_BulletReadback[MAX_EDICTS];

bool ProbeBullet( const BotEngineContext &ctx, edict_t *self, edict_t *target,
                  const Vector &eye, const Vector &direction, const AgPBBullet::Profile *profile,
                  bool requireTarget, float targetFarDepth )
{
    AgPBBullet::State bullet;
    if ( profile != NULL ) bullet = AgPBBullet::State( *profile );
    else bullet.range = float( MAX_TRACE_LENGTH );
    IPhysicsSurfaceProps *properties = AgPB_SurfaceProps();
    const int selfIndex = ctx.pEngine->IndexOfEdict( self );
    BulletReadback result;
    result.owner = AgPB_RefEHandle( self ); result.target = AgPB_RefEHandle( target );
    result.requireTarget = requireTarget; result.knownProfile = profile != NULL;
    auto finish = [&]( bool allowed, const char *reason ) {
        result.allowed = allowed; result.reason = reason; result.damage = profile != NULL ? bullet.damage : -1.0f;
        result.observedAt = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
        if ( selfIndex >= 1 && selfIndex < MAX_EDICTS )
        {
            const BulletReadback old = s_BulletReadback[selfIndex];
            result.nextLog = old.nextLog;
            if ( agpb_combat_debug.GetBool() && ( result.walls > 0 || !allowed ) &&
                 result.observedAt >= old.nextLog && ( old.owner != result.owner || old.allowed != allowed || old.reason != reason ) )
            {
                META_CONPRINTF( "[AgPB] bullet probe ent=%d target=%d path=%s result=%s reason=%s walls=%d material=%c/%c thickness=%.2f base-damage=%.2f\n",
                    selfIndex, ctx.pEngine->IndexOfEdict( target ), requireTarget ? "ideal" : "command",
                    allowed ? "allowed" : "blocked", reason, result.walls,
                    result.enterMaterial >= 32 && result.enterMaterial < 127 ? result.enterMaterial : '?',
                    result.exitMaterial >= 32 && result.exitMaterial < 127 ? result.exitMaterial : '?', result.thickness, result.damage );
                result.nextLog = result.observedAt + 1.0f;
            }
            s_BulletReadback[selfIndex] = result;
        }
        return allowed;
    };
    Vector start = eye;
    // Script count is bounded to16; one additional impact/clear segment is
    // sufficient, including Source's special count0 grate continuation.
    const int surfaceBudget = profile != NULL ? MIN( profile->count + 2, 18 ) : 1;
    for ( int surface = 0; surface < surfaceBudget; ++surface )
    {
        if ( !std::isfinite( bullet.range ) || bullet.range <= 0.0f ) return finish( false, "range-exhausted" );
        const Vector end = start + direction * bullet.range;
        trace_t hit; TraceBulletSegment( ctx, self, start, end, hit );
        if ( hit.startsolid || hit.allsolid || !hit.endpos.IsValid() ) return finish( false, "solid-start" );
        if ( profile != NULL && hit.fraction < 1.0f && !AgPBBullet::ApplyImpact( bullet, hit.fraction ) ) return finish( false, "damage-exhausted" );
        edict_t *entity = hit.m_pEnt != NULL ? ctx.pGameEnts->BaseEntityToEdict( hit.m_pEnt ) : NULL;
        if ( entity == target ) return finish( true, result.walls > 0 ? "penetration-reaches-target" : "direct-target" );
        if ( entity != NULL )
        {
            const int index = ctx.pEngine->IndexOfEdict( entity );
            if ( gpGlobals != NULL && index >= 1 && index <= gpGlobals->maxClients ) return finish( false, "other-player" );
        }
        if ( hit.fraction >= 1.0f ) return finish( !requireTarget && DotProduct( end - eye, direction ) >= targetFarDepth, "clear-miss" );
        const bool world = entity != NULL && ctx.pEngine->IndexOfEdict( entity ) == 0;
        if ( !requireTarget && world && AgPBCombatVisibility::ShotAllowed( true, false,
             AgPBCombatVisibility::RayHit::World, DotProduct( hit.endpos - eye, direction ), targetFarDepth ) )
            return finish( true, "world-behind-target" );
        if ( profile == NULL || properties == NULL ) return finish( false, "penetration-data-unavailable" );
        int enterMaterial;
        if ( !SurfaceMaterial( properties, hit, enterMaterial ) ) return finish( false, "entry-material-unavailable" );
        result.enterMaterial = enterMaterial;
        // The source probes in 24-unit steps while distance<=128 (therefore
        // its last possible probe is 144), then reverse-traces the exact exit.
        Vector freePoint; bool exited = false;
        for ( float distance = 0.0f; distance <= 128.0f; )
        {
            distance += 24.0f; freePoint = hit.endpos + direction * distance;
            if ( ( ctx.pTrace->GetPointContents( freePoint ) & MASK_SOLID ) == 0 ) { exited = true; break; }
        }
        if ( !exited ) return finish( false, "no-exit-within-probe-budget" );
        Ray_t reverse; reverse.Init( freePoint, hit.endpos );
        trace_t exit;
        CombatTraceFilter all( NULL ); ctx.pTrace->TraceRay( reverse, BulletTraceMask, &all, &exit );
        if ( exit.m_pEnt != hit.m_pEnt && exit.m_pEnt != NULL )
        {
            edict_t *blocker = ctx.pGameEnts->BaseEntityToEdict( exit.m_pEnt );
            IHandleEntity *ignore = EntityHandle( blocker );
            if ( ignore == NULL ) return finish( false, "exit-blocker-unavailable" );
            CombatTraceFilter skip( ignore ); ctx.pTrace->TraceRay( reverse, BulletTraceMask, &skip, &exit );
        }
        int exitMaterial;
        if ( exit.startsolid || exit.allsolid || exit.fraction >= 1.0f || !exit.endpos.IsValid() ||
             !SurfaceMaterial( properties, exit, exitMaterial ) ) return finish( false, "exit-material-unavailable" );
        const float thickness = ( exit.endpos - hit.endpos ).Length();
        result.thickness = thickness; result.exitMaterial = exitMaterial;
        if ( !AgPBBullet::Penetrate( bullet, thickness, enterMaterial, ( hit.contents & CONTENTS_GRATE ) != 0, exitMaterial ) )
            return finish( false, "material-thickness-or-ammo-budget" );
        ++result.walls; start = exit.endpos;
    }
    return finish( false, "surface-budget-exhausted" );
}

bool CanReachAimPoint( const BotEngineContext &ctx, edict_t *self, edict_t *target, edict_t *weapon,
                       const Vector &eye, const Vector &point, bool allowPenetration )
{
    Vector direction = point - eye;
    if ( VectorNormalize( direction ) < 0.01f ) return false;
    if ( !allowPenetration ) return VisibleSpot( ctx, self, target, eye, point );
    // Actual bullet mask includes grates. A clear visual MASK_SHOT ray alone
    // is not proof that this weapon can traverse a railing.
    CombatTraceFilter filter( EntityHandle( self ) );
    Ray_t ray; ray.Init( eye, point + direction * 8.0f );
    trace_t direct; ctx.pTrace->TraceRay( ray, BulletTraceMask, &filter, &direct );
    if ( !direct.startsolid && !direct.allsolid && direct.m_pEnt != NULL &&
         ctx.pGameEnts->BaseEntityToEdict( direct.m_pEnt ) == target ) return true;
    AgPBBullet::Profile profile;
    const bool known = ReadBulletProfile( weapon, profile );
    return ProbeBullet( ctx, self, target, eye, direction, known ? &profile : NULL, true, 0.0f );
}

bool FireRayAllowed( const BotEngineContext &ctx, edict_t *self, edict_t *target, edict_t *weapon,
                     const Vector &eye, const Vector &direction, const Vector &idealPoint )
{
    if ( ctx.pTrace == NULL || ctx.pGameEnts == NULL || ctx.pEngine == NULL ||
         ctx.pPlayerInfoManager == NULL || EntityHandle( self ) == NULL || target == NULL ||
         !direction.IsValid() || !eye.IsValid() || !CanReachAimPoint( ctx, self, target, weapon, eye, idealPoint, true ) ) return false;
    IPlayerInfo *info = ctx.pPlayerInfoManager->GetPlayerInfo( target );
    if ( info == NULL ) return false;
    const Vector origin = info->GetAbsOrigin(), mins = info->GetPlayerMins(), maxs = info->GetPlayerMaxs();
    if ( !origin.IsValid() || !mins.IsValid() || !maxs.IsValid() ) return false;
    Vector shotDirection = direction;
    if ( VectorNormalize( shotDirection ) < 0.01f ) return false;
    const Vector center = origin + ( mins + maxs ) * 0.5f, half = ( maxs - mins ) * 0.5f;
    const float farDepth = DotProduct( center - eye, shotDirection ) +
        fabsf( shotDirection.x ) * half.x + fabsf( shotDirection.y ) * half.y + fabsf( shotDirection.z ) * half.z;
    if ( !std::isfinite( farDepth ) || farDepth <= 0.0f ) return false;
    AgPBBullet::Profile profile;
    const bool known = ReadBulletProfile( weapon, profile );
    return ProbeBullet( ctx, self, target, eye, shotDirection, known ? &profile : NULL, false, farDepth );
}
}

void CAgPB::InitializeCombatSkill( int skill )
{
    if ( skill == -1 )
    {
        const int difficulty = agpb_difficulty.GetInt();
        if ( difficulty != -1 )
        {
            SetDifficulty( clamp( difficulty, 0, 4 ) );
            return;
        }
        skill = RandomInt( 1, 100 );
    }

    if ( !SetSkill( skill ) )
        return;
    for ( int difficulty = 0; difficulty <= 4; ++difficulty )
    {
        if ( skill <= AgPBCombatSkill::DifficultyRange( difficulty ).maximum )
        {
            m_iCombatDifficulty = difficulty;
            break;
        }
    }
}

bool CAgPB::SetSkill( int skill )
{
    if ( skill < 1 || skill > 100 ) return false;
    m_iCombatSkill = skill;
    m_iCombatDifficulty = -1;
    const AgPBCombatSkill::Row row = AgPBCombatSkill::SkillRow( skill );
    m_flCombatIdealReaction = m_flCombatActualReaction = row.minReaction;
    m_flCombatReactionLastTime = -1.0f; m_flCombatReactionNextAt = 0.0f;
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
    // Every exit from an owned fight may resume from a displaced position,
    // including empty weapons and movement/objective priority changes.
    if ( m_uCombatOwnerToken != 0 )
    {
        ResetRouteProgress( gpGlobals != NULL ? gpGlobals->curtime : 0.0f );
        m_flRouteSettleSince = -1.0f;
    }
    m_uCombatEnemy = m_uCombatWeapon = (uintp)INVALID_EHANDLE_INDEX;
    m_iCombatEnemyUserId = 0;
    m_iCombatLastClip = -1;
    m_uCombatOwnerToken = 0;
    m_flCombatLastTime = -1.0f;
    m_flCombatAimError = 0.0f;
    m_bCombatRelease = false;
    m_flCombatSurpriseUntil = m_flCombatRearCheckAt = m_flCombatFirePause = 0.0f;
    m_flNextBurstRequest = 0.0f;
    m_vCombatAimDeviation.Init();
    m_bCombatPerfectAim = false;
    m_pszCombatState = reason;
    AgPBCombatMove::Reset( m_CombatMovement );
    m_bCombatMovePlanValid = m_bCombatSniperStop = false;
    AgPBFiring::Reset( m_CombatFire );
    m_pszCombatFire = "idle";
    m_bCombatFireHold = false;
    m_pszCombatMotion = "idle";
    m_flCombatMoveForward = m_flCombatMoveSide = 0.0f;
    m_flCombatMoveDistance = m_flCombatMoveSpeed = 0.0f;
    m_bCombatMoveBlocked = false;
    m_bCombatDuckWanted = false;
    m_flCombatEyeZ = -1.0f; m_flCombatEyeStableSince = 0.0f;
}

void CAgPB::SetCombat( bool enable )
{
    m_bCombatEnabled = enable;
    edict_t *breakWeapon = HandleToEdict( m_uBreakWeapon );
    if ( breakWeapon != NULL ) m_iBreakClip = WeaponClip( breakWeapon );
    m_bBreakKnifePending = false;
    if ( m_uBreakTaskToken != 0 ) m_bBreakRelease = true;
    ClearCombat( enable ? "enabled" : "disabled" );
    ResetWeaponSelection( enable ? "enabled" : "disabled" );
    ResetSearch();
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
    using namespace AgPBCombatVisibility;
    if ( visibility != NULL ) *visibility = 0;
    IPlayerInfo *info = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    if ( info == NULL ) return false;
    const Vector origin = info->GetAbsOrigin(), mins = info->GetPlayerMins(), maxs = info->GetPlayerMaxs();
    if ( !origin.IsValid() || !mins.IsValid() || !maxs.IsValid() ||
         maxs.x <= mins.x || maxs.y <= mins.y || maxs.z <= mins.z ) return false;
    const Vector body = origin + ( mins + maxs ) * 0.5f;
    if ( requireViewCone && !InsideViewCone( *this, eye, body ) ) return false;
    Vector headOffset;
    const bool headKnown = ViewOffset( target, headOffset );
    Vector spots[SpotCount];
    spots[Body] = body; spots[Head] = headKnown ? origin + headOffset : body;
    // Bounded partial-body proxies use only public collision bounds/view
    // offset. A sample is useful only when its own MASK_SHOT trace first hits
    // this target; bounds alone never imply an exposed hitbox.
    const float height = maxs.z - mins.z;
    const float width = MIN( maxs.x - mins.x, maxs.y - mins.y ) * 0.5f;
    const Vector feet = origin + Vector( ( mins.x + maxs.x ) * 0.5f, ( mins.y + maxs.y ) * 0.5f, mins.z );
    Vector side( -( body - eye ).y, ( body - eye ).x, 0.0f );
    if ( VectorNormalize( side ) < 0.01f ) side.Init( 1.0f, 0.0f, 0.0f );
    spots[UpperBody] = feet + Vector( 0, 0, height * 0.70f );
    spots[LeftShoulder] = feet + Vector( 0, 0, height * 0.72f ) + side * width * 0.80f;
    spots[RightShoulder] = feet + Vector( 0, 0, height * 0.72f ) - side * width * 0.80f;
    spots[LeftTorso] = feet + Vector( 0, 0, height * 0.48f ) + side * width * 0.80f;
    spots[RightTorso] = feet + Vector( 0, 0, height * 0.48f ) - side * width * 0.80f;
    spots[LowerBody] = feet + Vector( 0, 0, height * 0.28f );
    spots[LeftLeg] = feet + Vector( 0, 0, height * 0.16f ) + side * width * 0.35f;
    spots[RightLeg] = feet + Vector( 0, 0, height * 0.16f ) - side * width * 0.35f;
    unsigned int visible = 0;
    if ( VisibleSpot( m_Ctx, m_pEdict, target, eye, spots[Body] ) ) visible |= Bit( Body );
    if ( headKnown && VisibleSpot( m_Ctx, m_pEdict, target, eye, spots[Head] ) ) visible |= Bit( Head );
    // Center/head remain the cheap common path. Only probe partial body when
    // its center is covered, retaining the first proven fallback position.
    if ( ( visible & Bit( Body ) ) == 0 )
        for ( int i = UpperBody; i < SpotCount; ++i )
            if ( VisibleSpot( m_Ctx, m_pEdict, target, eye, spots[i] ) ) { visible |= Bit( i ); break; }
    if ( visibility != NULL ) *visibility = Summary( visible );
    const int selected = SelectSpot( visible, false );
    if ( selected < 0 ) return false;
    point = spots[selected];
    return true;
}

edict_t *CAgPB::CurrentCombatWeapon( uintp &handle, bool &knife, int &clip, int &reserve ) const
{
    knife = false; clip = reserve = -1;
    edict_t *weapon = OwnedActiveWeapon( *this, handle );
    if ( weapon == NULL ) return NULL;
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
    // Task deadlines and agent leases remain real game time, including CAMP.
    // Firing must not keep extending a finite holding task indefinitely.
    m_flRouteSettleSince = -1.0f;
}

void CAgPB::UpdateCombatMovement( CGlobalVars *globals, CBotCmd &cmd, edict_t *target,
                                  edict_t *weapon, bool fireIntent, bool reloadIntent, bool decisionTick,
                                  Vector &blockedCombatWish )
{
    using namespace AgPBCombatMove;
    IPlayerInfo *enemy = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    const Vector enemyOrigin = enemy->GetAbsOrigin();
    const Vector origin = m_pInfo->GetAbsOrigin();
    Vector toward = enemyOrigin - origin;
    const float distance = toward.Length();
    toward.z = 0.0f;
    Input input;
    input.distance = distance;
    const bool horizontalDirection = VectorNormalize( toward ) >= 0.01f;
    if ( !horizontalDirection ) toward.Init();
    const Vector right( toward.y, -toward.x, 0 );
    input.now = globals->curtime;
    input.weapon = CombatWeaponKind( weapon );
    input.health = float( m_pInfo->GetHealth() );
    input.aggression = m_Autonomy.aggression;
    input.skill = m_iCombatSkill;
    input.maxSpeed = GetNetVarFloat( "m_flMaxspeed", 250.0f );
    if ( !std::isfinite( input.maxSpeed ) || input.maxSpeed <= 0.0f ) input.maxSpeed = 250.0f;
    input.attacking = fireIntent;
    // The local core reloads only empty firearms. Retain reload retreat until
    // the public clip observation refills, without reading private reload data.
    input.reloading = reloadIntent;
    input.mobilePrimary = NamedWeapon( weapon, "weapon_ump45" ) || NamedWeapon( weapon, "weapon_mac10" ) ||
        NamedWeapon( weapon, "weapon_tmp" ) || NamedWeapon( weapon, "weapon_p90" );
    input.bombPlanted = BotObjectives().Snapshot().bomb.state == AGPB_BOMB_PLANTED;
    Vector enemyRight;
    AngleVectors( QAngle( AgPB_ReadNetVarFloat( target, "m_angEyeAngles[0]" ),
        AgPB_ReadNetVarFloat( target, "m_angEyeAngles[1]" ), 0 ), NULL, &enemyRight, NULL );
    Vector enemyToSelf = origin - enemyOrigin;
    VectorNormalize( enemyToSelf );
    input.enemySide = DotProduct( enemyToSelf, enemyRight );
    const AgPBPath *node = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
    input.crouchRequired = TaskRequiresCrouch() ||
        !CanWalkToTaskPosition( m_pInfo->GetAbsOrigin(), false );
    const bool ducking = ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0;
    // SyPB GetWalkSpeed; engine speed limits still own actual displacement.
    const int maximum = int( input.maxSpeed );
    const unsigned int postureButtons = cmd.buttons | m_LastSubmittedCmd.buttons;
    input.walkSpeed = input.maxSpeed <= 180.0f || ducking || input.crouchRequired ||
        ( postureButtons & ( IN_DUCK | IN_JUMP ) ) != 0 || GetNetVarInt( "m_nWaterLevel" ) >= 2
        ? input.maxSpeed : float( maximum ) * 0.5f + float( maximum / 50 ) - 18.0f;
    Vector targetPoint, actualEye;
    const bool knownEye = ViewOffset( m_pEdict, actualEye );
    // SyPB CombatFight passes its own edict to IsInViewCone.
    input.enemyInViewCone = knownEye && InsideViewCone( *this, origin + actualEye, enemyOrigin );
    // Eye height is not hull height minus a constant. Only an already
    // observed duck view may authorize optional continued combat crouch;
    // proximity/authored crouch remains available without inventing eye data.
    input.canDuck = ducking && knownEye &&
        CombatAimPoint( target, m_pInfo->GetAbsOrigin() + actualEye, targetPoint, false );
    Random earlyRandom, lateRandom;
    earlyRandom.style = RandomFloat( 0.0f, 1.0f ); earlyRandom.strafeFlip = RandomFloat( 0.0f, 1.0f );
    earlyRandom.duration = RandomFloat( 0.0f, 1.0f ); earlyRandom.duck = RandomFloat( 0.0f, 1.0f );
    lateRandom.style = RandomFloat( 0.0f, 1.0f ); lateRandom.strafeFlip = RandomFloat( 0.0f, 1.0f );
    lateRandom.duration = RandomFloat( 0.0f, 1.0f ); lateRandom.duck = RandomFloat( 0.0f, 1.0f );
    Output movement;
    if ( IsHoldingTaskPosition() )
    {
        // SyPB dispatch excludes TASK_CAMP from CombatFight. A standing or
        // crouched hold can aim/fire/reload without becoming a pursuit task.
        AgPBCombatMove::Reset( m_CombatMovement );
        m_bCombatMovePlanValid = false;
        movement.duck = input.crouchRequired;
        movement.motion = movement.duck ? Motion::Crouch : Motion::Hold;
        movement.stopForAccuracy = fireIntent && !reloadIntent &&
            input.weapon != Weapon::Shotgun && !( input.weapon == Weapon::SMG && input.mobilePrimary );
    }
    else
    {
        // CombatFight is a ThinkFrame decision in SyPB. Keep its proposal
        // between the same 15 Hz fire decisions; native hull safety and
        // command projection below still run for every submitted command.
        if ( decisionTick || !m_bCombatMovePlanValid )
        {
            // RunTask(FIGHTENEMY) precedes FireWeapon. Only skill>=60 gets
            // the additional post-fire CombatFight pass in SyPB BotAI.
            m_CombatMovePlan = StepFrame( m_CombatMovement, input, earlyRandom, lateRandom );
            m_bCombatMovePlanValid = true;
        }
        movement = m_CombatMovePlan;
        movement.duck = movement.duck || input.crouchRequired;
        movement.stopForAccuracy = movement.stopForAccuracy && fireIntent;
    }
    if ( m_bCombatSniperStop )
    {
        // SyPB m_sniperFire / MoveAction brakes even a low-health retreat
        // while waiting to issue the sniper shot. Preserve the zero plan
        // through this decision after the actual trigger clears the latch.
        movement.forward = movement.side = 0.0f;
        movement.motion = movement.duck ? Motion::Crouch : Motion::Hold;
        if ( m_bCombatMovePlanValid ) m_CombatMovePlan = movement;
    }
    m_iCombatMoveTick = globals->tickcount;
    m_vCombatMoveRight = right;
    m_flCombatMovePolicySide = movement.side;
    m_pszCombatMoveFilter = "clear";
    if ( movement.side != 0.0f && node != NULL && ( node->flags & AgPB_WP_FALLRISK ) != 0 )
    {
        movement.side = 0.0f;
        m_pszCombatMoveFilter = "fallrisk";
    }
    m_pszCombatMotion = MotionName( movement.motion );
    m_flCombatMoveForward = movement.forward; m_flCombatMoveSide = movement.side;
    m_flCombatMoveDistance = input.distance;
    m_bCombatMoveBlocked = !horizontalDirection && ( movement.forward != 0.0f || movement.side != 0.0f );
    if ( movement.motion == Motion::Invalid ) cmd.buttons &= ~IN_ATTACK;
    if ( movement.duck ) cmd.buttons |= IN_DUCK;
    else cmd.buttons &= ~IN_DUCK;
    Vector wish = toward * movement.forward + right * movement.side;
    if ( wish.Length2DSqr() > 0.01f && !CanCombatStep( wish, movement.duck, globals->interval_per_tick ) )
    {
        m_bCombatMoveBlocked = true;
        // Try both sides before dropping the lateral part of the move.
        // Keep the requested approach/retreat sign in alternate steps.
        const Axes alternate = AlternateStep( movement, [&]( float forward, float side ) {
            return CanCombatStep( toward * forward + right * side, movement.duck, globals->interval_per_tick );
        } );
        if ( alternate.side != 0.0f && alternate.side == -movement.side )
        {
            m_CombatMovement.strafeSign = alternate.side > 0.0f ? 1 : -1;
            m_CombatMovement.strafeUntil = input.now + 0.7f;
            if ( m_bCombatMovePlanValid ) m_CombatMovePlan.side = alternate.side;
        }
        // Preserve the blocked intent only as a hint to teammate avoidance.
        if ( alternate.forward == 0.0f && alternate.side == 0.0f && !movement.stopForAccuracy ) blockedCombatWish = wish;
        wish = toward * alternate.forward + right * alternate.side;
        m_pszCombatMoveFilter = alternate.side != 0.0f ? "alternate" :
            alternate.forward != 0.0f ? "straight-only" : "blocked";
    }
    m_flCombatMoveSafeSide = DotProduct( wish, right );
    // Aim may still be turning. Project the world wish into the final command
    // yaw, so a view change cannot turn retreat or strafe into forward motion.
    const AgPBMovement::LocalWish local = AgPBMovement::WorldToLocal( wish.x, wish.y, cmd.viewangles.y );
    cmd.forwardmove = local.forward; cmd.sidemove = local.side; cmd.upmove = 0.0f;
    Vector velocity;
    const bool knownVelocity = ReadVector( m_pEdict, "m_vecVelocity", velocity );
    m_flCombatMoveSpeed = knownVelocity ? velocity.Length2D() : -1.0f;
    if ( movement.stopForAccuracy && ( !knownVelocity || m_flCombatMoveSpeed > MAX( 5.0f, input.maxSpeed * 0.1f ) ) )
    {
        cmd.buttons &= ~IN_ATTACK;
        m_pszCombatState = "braking";
    }
    // Movement executes before weapon PostThink. A duck transition changes
    // the actual eye height after the aim trace above; withhold fire until
    // public eye observations have settled at the requested posture.
    if ( !knownEye || movement.duck != m_bCombatDuckWanted || movement.duck != ducking ||
         GetNetVarInt( "m_bDucking" ) != 0 ||
         m_flCombatEyeZ < 0.0f || fabsf( actualEye.z - m_flCombatEyeZ ) > 0.01f )
        m_flCombatEyeStableSince = input.now;
    m_bCombatDuckWanted = movement.duck;
    m_flCombatEyeZ = knownEye ? actualEye.z : -1.0f;
    if ( !knownEye || input.now - m_flCombatEyeStableSince < 0.05f )
    {
        cmd.buttons &= ~IN_ATTACK;
        m_pszCombatState = "posture-settling";
    }
}

bool CAgPB::CombatStrafeLogEnabled() const
{
    const int slot = agpb_strafe_log.GetInt();
    return slot < 0 || ( slot > 0 && m_Ctx.pEngine->IndexOfEdict( m_pEdict ) == slot );
}

void CAgPB::ReportCombatStrafe( CGlobalVars *globals, const CBotCmd &cmd, const Vector &start )
{
    const bool evaluated = m_iCombatMoveTick == cmd.tick_count;
    const char *phase = !evaluated ? "no-combat-move" : !m_bCombatMovePlanValid ? "holding" :
        StrafePlanName( m_CombatMovePlan.strafe );
    const float now = globals->curtime;
    const bool sampleDue = m_pszCombatStrafeLogPhase != phase || now < m_flCombatStrafeLogAt ||
        now - m_flCombatStrafeLogAt >= ( evaluated ? 0.25f : 1.0f );
    const bool attack = ( cmd.buttons & IN_ATTACK ) != 0;
    if ( !sampleDue && ( !evaluated || !attack ) ) return;

    uintp held;
    edict_t *weapon = OwnedActiveWeapon( *this, held );
    const int clip = weapon != NULL ? WeaponClip( weapon ) : -1;
    const int shots = held == m_uCombatWeapon && clip >= 0 ? MAX( 0, m_iCombatLastClip - clip ) : 0;
    if ( !sampleDue && shots == 0 ) return;

    float finalSide = 0.0f, deltaSide = 0.0f;
    if ( evaluated )
    {
        Vector forward, right;
        AngleVectors( QAngle( 0.0f, cmd.viewangles.y, 0.0f ), &forward, &right, NULL );
        finalSide = DotProduct( forward * cmd.forwardmove + right * cmd.sidemove, m_vCombatMoveRight );
        deltaSide = DotProduct( m_pInfo->GetAbsOrigin() - start, m_vCombatMoveRight );
    }
    m_pszCombatStrafeLogPhase = phase;
    m_flCombatStrafeLogAt = now;
    RouteReport( "strafe sample v=1 tick=%d time=%.3f slot=%d userid=%d mode=%s task=%s skill=%d weapon=%s enemy=%d "
        "phase=%s state=%s motion=%s distance=%.1f side=(%.2f,%.2f,%.2f) filter=%s cmd=(%.2f,%.2f) "
        "yaw=%.1f delta-side=%.3f attack=%d shots=%d clip=%d",
        cmd.tick_count, now, m_Ctx.pEngine->IndexOfEdict( m_pEdict ), m_pInfo->GetUserID(),
        AgPB_GameModeName( AgPB_GameMode() ), AgPBTaskName( m_Tasks.Current().id ), m_iCombatSkill,
        weapon != NULL ? weapon->GetClassName() : "none", m_iCombatEnemyUserId, phase, m_pszCombatState,
        evaluated ? m_pszCombatMotion : "idle", evaluated ? m_flCombatMoveDistance : -1.0f,
        evaluated ? m_flCombatMovePolicySide : 0.0f, evaluated ? m_flCombatMoveSafeSide : 0.0f, finalSide,
        evaluated ? m_pszCombatMoveFilter : "none", cmd.forwardmove, cmd.sidemove, cmd.viewangles.y,
        deltaSide, attack ? 1 : 0, shots, clip );
}

void CAgPB::UpdateIdleWeaponZoom( CGlobalVars *globals, CBotCmd &cmd )
{
    using namespace AgPBFiring;
    const float now = globals->curtime;
    const AgPBGameMode mode = AgPB_GameMode();
    const bool active = m_bAutonomous || m_bRoam || HasRoute() || m_Tasks.Current().id != AgPBTaskId::Normal;
    if ( !IdleZoomDue( now, m_flCombatZoomCheckAt ) || !m_bCombatEnabled || !agpb_combat.GetBool() ||
         !active || ( mode != AgPB_MODE_NORMAL && ( mode != AgPB_MODE_ZM || m_ZM.role != AgPBZRRole::Human ) ) ||
         AgPB_IsRoundFrozen() ||
         ( GetNetVarInt( "m_fFlags" ) & ( FL_ONGROUND | FL_FROZEN ) ) != FL_ONGROUND ||
         GetNetVarInt( "movetype" ) != MOVETYPE_WALK || m_Ladder.Active() || m_bPendingJumpLaunch ||
         ( cmd.buttons & ( IN_ATTACK | IN_ATTACK2 | IN_USE | IN_RELOAD | IN_JUMP ) ) != 0 || cmd.weaponselect != 0 ||
         ( m_LastSubmittedCmd.buttons & IN_ATTACK2 ) != 0 || m_WeaponSelection.pendingKey != 0 ||
         GetNetVarInt( "m_bIsDefusing" ) != 0 || NetVar_HandleIsValid( m_uBreakHandle ) ) return;

    // A temporary native unzoom must leave the cleanup eligible after rescope.
    const int fov = GetNetVarInt( "m_iFOV", -1 );
    if ( fov <= 0 || fov == GetNetVarInt( "m_iDefaultFOV", 90 ) ) return;

    uintp handle;
    edict_t *weapon = OwnedActiveWeapon( *this, handle );
    const WeaponZoom zoom = ReadWeaponZoom( *this, ClassifyWeapon( weapon ), now );
    if ( !IsZoomed( zoom.level ) || zoom.settling ) return;
    float playerAttack;
    if ( ReadFloat( m_pEdict, "m_flNextAttack", playerAttack ) && now >= playerAttack &&
         SecondaryAttackReady( weapon, now ) ) cmd.buttons |= IN_ATTACK2;
}

bool CAgPB::UpdateCombat( CGlobalVars *globals, CBotCmd &cmd, Vector &blockedCombatWish )
{
    ObserveCombatShots();
    const float now = globals->curtime;
    const AgPBCombatSkill::Row skillRow = AgPBCombatSkill::SkillRow( m_iCombatSkill );
    // These SyPB accumulators are ThinkFrame (15 Hz), not native command
    // frequency. Keep high-rate LOS/identity safety without speeding up the
    // lower difficulties' reaction recovery fourfold.
    if ( m_flCombatReactionLastTime < 0.0f || now < m_flCombatReactionLastTime || now >= m_flCombatReactionNextAt )
    {
        m_flCombatActualReaction = MIN( m_flCombatActualReaction + 0.2f, m_flCombatIdealReaction );
        m_flCombatIdealReaction = RandomFloat( skillRow.minReaction, skillRow.maxReaction );
        m_flCombatReactionLastTime = now;
        m_flCombatReactionNextAt = now + AgPBFiring::DecisionInterval;
    }
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
        ResetWeaponSelection( frozen ? "frozen" : !intent ? "stopped" : "disabled" );
        ClearCombat( frozen ? "frozen" : !intent ? "stopped" : "disabled" );
        // Freeze only releases the command overlay. Observation TTL remains
        // real game time; lifecycle/mode/stop and combat disable clear memory.
        if ( !m_bCombatEnabled || !agpb_combat.GetBool() || AgPB_GameMode() != AgPB_MODE_NORMAL ||
             !intent || !IsAlive() ) ResetSearch();
        return false;
    }
    // Keep the existing local objective fallback running during a fight;
    // this includes C4/hostage urgency and remembered planted observations.
    const std::uint64_t beforePlan = m_Tasks.Current().token;
    RefreshTaskPlan( globals );
    if ( m_Tasks.Current().token != beforePlan )
    {
        ClearCombat( "objective-replan" );
        ResetWeaponSelection( "objective-replan" );
        return false; // The canonical dispatcher reconciles route ownership.
    }
    const AgPBObjectiveSnapshot &world = BotObjectives().Snapshot();
    const AgPBTask &task = m_Tasks.Current();
    if ( task.id == AgPBTaskId::SeekCover ) return false;
    if ( hadEnemy && m_uCombatOwnerToken != task.token )
    { ClearCombat( "owner-changed" ); return false; }
    const AgPBPath *routeNode = BotWaypoints().Get( RouteNode( m_iRouteIndex ) );
    unsigned int legFlags = 0, nextLegFlags = 0;
    if ( m_iRouteIndex > 0 ) BotWaypoints().IsConnected( RouteNode( m_iRouteIndex - 1 ), RouteNode( m_iRouteIndex ), &legFlags );
    BotWaypoints().IsConnected( RouteNode( m_iRouteIndex ), RouteNode( m_iRouteIndex + 1 ), &nextLegFlags );
    const bool jumpRoute = ( ( legFlags | nextLegFlags ) & AgPB_PATH_JUMP ) != 0;
    const bool unsupportedMovement = ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 ||
        GetNetVarInt( "movetype", MOVETYPE_NONE ) == MOVETYPE_LADDER || jumpRoute ||
        ( routeNode != NULL && ( routeNode->flags & AgPB_WP_LADDER ) != 0 );
    const bool plantUse = task.id == AgPBTaskId::PlantBomb && GetNetVarInt( "m_bInBombZone" ) != 0;
    const bool defuseUse = task.id == AgPBTaskId::DefuseBomb &&
        ( GetNetVarInt( "m_bIsDefusing" ) != 0 || ( world.bomb.positionKnown &&
          ( world.bomb.position - m_pInfo->GetAbsOrigin() ).Length2DSqr() < 96.0f * 96.0f ) );
    const bool expired = task.deadline > 0.0f && now >= task.deadline;
    if ( unsupportedMovement || task.id == AgPBTaskId::EscapeBomb || plantUse || defuseUse || expired )
    {
        ClearCombat( unsupportedMovement ? "movement" : "objective-priority" );
        // The navigation pass owns a pending knife switch on a jump approach.
        if ( !jumpRoute || task.id == AgPBTaskId::EscapeBomb || plantUse || defuseUse || expired )
            ResetWeaponSelection( unsupportedMovement ? "movement" : "objective-priority" );
        return false;
    }
    Vector offset;
    if ( !ViewOffset( m_pEdict, offset ) ) { ClearCombat( "eye-unavailable" ); return false; }
    const Vector eye = m_pInfo->GetAbsOrigin() + offset;
    uintp weaponHandle; bool knife; int clip, reserve;
    edict_t *weapon = CurrentCombatWeapon( weaponHandle, knife, clip, reserve );
    // Acquire by vision even with a travel knife or an empty active item.
    // Knife reach is checked when proposing a strike, not when finding a target.
    edict_t *target = hadEnemy ? HandleToEdict( m_uCombatEnemy ) : NULL;
    Vector point;
    unsigned int visibility = 0;
    const bool validTarget = !hadEnemy || IsCombatEnemy( target, m_iCombatEnemyUserId );
    if ( hadEnemy && ( !validTarget || !CombatAimPoint( target, eye, point, false, &visibility ) ) )
    {
        const bool seekingCover = validTarget && TryStartCover( globals,
            !knife && reserve > 0 && ( clip == 0 || m_bCombatReloadRequested ) );
        PauseCombatOwner( globals );
        if ( agpb_combat_debug.GetBool() ) RouteReport( "combat released ent=%d; route goal=#%d hop=%d owner=%llu",
            NetVar_HandleEntry( m_uCombatEnemy ), m_iGoalWaypoint, m_iRouteIndex, (unsigned long long)m_uRouteTaskToken );
        if ( !validTarget )
        {
            if ( AgPBSearch::SameIdentity( m_Search.visual, m_uCombatEnemy, m_iCombatEnemyUserId ) ) m_Search.visual.valid = false;
            if ( AgPBSearch::SameIdentity( m_Search.sound, m_uCombatEnemy, m_iCombatEnemyUserId ) ) m_Search.sound.valid = false;
        }
        ClearCombat( seekingCover ? "seek-cover" : "target-lost" );
        cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE );
        UpdateEnemyMemoryLook( globals, cmd );
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
            if ( !IsCombatEnemy( candidate ) || !CombatAimPoint( candidate, eye, aim, false, &seen ) ) continue;
            if ( !InsideViewCone( *this, eye, aim ) && !AgPBCombatSkill::ImmediateRearRecognition( m_iCombatSkill ) )
            {
                if ( m_flCombatRearCheckAt == 0.0f ) m_flCombatRearCheckAt = now + RandomFloat( 0.3f, 0.7f );
                if ( now <= m_flCombatRearCheckAt ) continue;
            }
            const float distance = ( aim - eye ).LengthSqr();
            // Stable enumeration resolves equal distance by the lower slot.
            if ( distance < nearest ) { nearest = distance; target = candidate; point = aim; visibility = seen; }
        }
        if ( target == NULL )
        {
            if ( TryStartCover( globals, !knife && clip == 0 && reserve > 0 ) ) return false;
            if ( UpdateWeaponSelection( globals, cmd, NULL, 0.0f ) )
            { PauseCombatOwner( globals ); return true; }
            m_flCombatLastTime = -1.0f;
            m_pszCombatState = "scanning";
            return false;
        }
        m_uCombatEnemy = AgPB_RefEHandle( target );
        m_iCombatEnemyUserId = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target )->GetUserID();
        m_uCombatOwnerToken = task.token;
        m_flCombatSurpriseUntil = now + AgPBCombatSkill::ReactionDelay(
            m_flCombatActualReaction, m_iCombatSkill, RandomInt( 0, 100 ) );
        m_flCombatRearCheckAt = 0.0f;
        m_bCombatRelease = true; // Acquire/align first, never fire on stale input.
        if ( m_bAutonomous )
            SendRadio( m_pInfo->GetHealth() <= 40 ? AgPBRadio::NeedBackup : AgPBRadio::EnemySpotted, now );
        if ( agpb_combat_debug.GetBool() ) RouteReport( "combat acquired ent=%d userid=%d; preserving goal=#%d hop=%d owner=%llu",
            NetVar_HandleEntry( m_uCombatEnemy ), m_iCombatEnemyUserId, m_iGoalWaypoint, m_iRouteIndex,
            (unsigned long long)m_uRouteTaskToken );
    }
    // Update only after this tick proved an actual target spot in LOS. Gaze/search use this
    // snapshot after loss; neither follows the hidden target nor authorizes fire.
    RememberVisibleEnemy( target, point, now );
    // SyPB CombatFight hands a low-approach retreat to ActionForEnemy.
    const AgPBCombatMove::Weapon weaponKind = CombatWeaponKind( weapon );
    const float approach = AgPBCombatMove::CombatApproach( weaponKind, float( m_pInfo->GetHealth() ),
        m_Autonomy.aggression, false );
    IPlayerInfo *threat = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    const bool withdrawing = threat != NULL && weapon != NULL && !knife && AgPBCombatMove::KnownWeapon( weaponKind ) &&
        AgPBCombatMove::WantsToWithdraw( approach, world.bomb.state == AGPB_BOMB_PLANTED,
            InsideViewCone( *this, eye, threat->GetAbsOrigin() ) );
    if ( TryStartCover( globals, false, withdrawing ) )
    {
        ClearCombat( "seek-cover" );
        return false;
    }
    m_flCombatActualReaction = 0.0f;
    PauseCombatOwner( globals );
    TrackCombatWeapon( weaponHandle, clip );
    cmd.forwardmove = cmd.sidemove = cmd.upmove = 0.0f;
    cmd.buttons &= ~( IN_ATTACK | IN_ATTACK2 | IN_RELOAD | IN_USE | IN_JUMP );
    cmd.weaponselect = 0;
    if ( ( GetNetVarInt( "m_fFlags" ) & FL_DUCKING ) != 0 ) cmd.buttons |= IN_DUCK;
    if ( UpdateWeaponSelection( globals, cmd, target, ( point - eye ).Length() ) ) return true;
    if ( weapon == NULL ) { ClearCombat( "weapon-unavailable" ); return false; }
    if ( !knife && reserve > 0 && ( clip == 0 || m_bCombatReloadRequested ) &&
         TryStartCover( globals, true ) )
    {
        ClearCombat( "seek-cover" );
        return false;
    }
    AimAndFire( globals, cmd, target, eye, point, visibility, weapon, knife, clip, reserve, &blockedCombatWish );
    return true;
}

void CAgPB::UpdateCoverFire( CGlobalVars *globals, CBotCmd &cmd, edict_t *enemy, int userId )
{
    const int flags = GetNetVarInt( "m_fFlags" );
    if ( !IsCombatEnemy( enemy, userId ) || m_Ladder.Active() || m_bPendingJumpLaunch || cmd.weaponselect != 0 ||
         ( cmd.buttons & ( IN_USE | IN_RELOAD | IN_JUMP ) ) != 0 ||
         ( flags & ( FL_ONGROUND | FL_FROZEN ) ) != FL_ONGROUND ||
         ( ( cmd.buttons & IN_DUCK ) != 0 ) != ( ( flags & FL_DUCKING ) != 0 ) ||
         GetNetVarInt( "m_bDucking" ) != 0 || GetNetVarInt( "movetype", MOVETYPE_NONE ) != MOVETYPE_WALK ) return;

    uintp weaponHandle;
    bool knife;
    int clip, reserve;
    edict_t *weapon = CurrentCombatWeapon( weaponHandle, knife, clip, reserve );
    if ( weapon == NULL || knife || clip <= 0 || !AgPBFiring::Supported( ClassifyWeapon( weapon ) ) ) return;
    if ( weaponHandle == m_uCoverReloadWeapon && clip <= m_iCoverReloadClip ) return;
    const float playerReady = GetNetVarFloat( "m_flNextAttack", FLT_MAX );
    float weaponReady = 0.0f;
    if ( !std::isfinite( playerReady ) || globals->curtime < playerReady ||
         !ReadFloat( weapon, "m_flNextPrimaryAttack", weaponReady ) || globals->curtime < weaponReady ) return;
    Vector offset;
    if ( !ViewOffset( m_pEdict, offset ) ) return;
    const Vector eye = m_pInfo->GetAbsOrigin() + offset;
    Vector point;
    unsigned int visibility = 0;
    if ( !CombatAimPoint( enemy, eye, point, false, &visibility ) )
    {
        if ( NetVar_HandleIsValid( m_uCombatEnemy ) ) ClearCombat( "cover-target-hidden" );
        return;
    }

    const uintp targetHandle = AgPB_RefEHandle( enemy );
    if ( m_uCombatEnemy != targetHandle || m_iCombatEnemyUserId != userId )
    {
        m_uCombatEnemy = targetHandle;
        m_iCombatEnemyUserId = userId;
        m_uCombatOwnerToken = m_Tasks.Current().token;
        const AgPBCombatSkill::Row skill = AgPBCombatSkill::SkillRow( m_iCombatSkill );
        m_flCombatSurpriseUntil = globals->curtime + AgPBCombatSkill::ReactionDelay(
            RandomFloat( skill.minReaction, skill.maxReaction ), m_iCombatSkill, RandomInt( 0, 100 ) );
        m_bCombatRelease = true;
    }
    TrackCombatWeapon( weaponHandle, clip );
    RememberVisibleEnemy( enemy, point, globals->curtime );

    // SyPB aims and fires after ActionForEnemy. Keep the cover route's
    // world movement while projecting it into the newly chosen view yaw.
    Vector forward, right;
    AngleVectors( QAngle( 0.0f, cmd.viewangles.y, 0.0f ), &forward, &right, NULL );
    const Vector wish = forward * cmd.forwardmove + right * cmd.sidemove;
    AimAndFire( globals, cmd, enemy, eye, point, visibility, weapon, false, clip, reserve, NULL );
    const AgPBMovement::LocalWish move = AgPBMovement::WorldToLocal( wish.x, wish.y, cmd.viewangles.y );
    cmd.forwardmove = move.forward;
    cmd.sidemove = move.side;
}

void CAgPB::ObserveCombatShots()
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
}

void CAgPB::TrackCombatWeapon( uintp weaponHandle, int clip )
{
    if ( m_uCombatWeapon != weaponHandle )
    {
        m_uCombatWeapon = weaponHandle; m_iCombatLastClip = clip; m_bCombatRelease = true;
        AgPBCombatMove::Reset( m_CombatMovement );
        AgPBFiring::Reset( m_CombatFire );
        m_flCombatFirePause = 0.0f;
        m_flNextBurstRequest = 0.0f;
        m_bCombatMovePlanValid = m_bCombatSniperStop = false;
    }
}

bool CAgPB::ZombieModeAimPoint( edict_t *target, Vector &point ) const
{
    Vector offset;
    return ViewOffset( m_pEdict, offset ) &&
        CombatAimPoint( target, m_pInfo->GetAbsOrigin() + offset, point, false );
}

void CAgPB::UpdateZMCombat( CGlobalVars *globals, CBotCmd &cmd, edict_t *target, AgPBZRRole role )
{
    ObserveCombatShots();
    if ( !m_bCombatEnabled || !agpb_combat.GetBool() ||
         ( GetNetVarInt( "m_fFlags" ) & FL_ONGROUND ) == 0 || GetNetVarInt( "movetype" ) == MOVETYPE_LADDER )
    {
        ClearCombat( "zm-inactive" );
        return;
    }

    const bool zombie = role == AgPBZRRole::Zombie;
    IPlayerInfo *enemy = ZMPlayer( target, zombie ? AgPBZRRole::Human : AgPBZRRole::Zombie );
    Vector offset;
    if ( !ViewOffset( m_pEdict, offset ) ) { ClearCombat( "eye-unavailable" ); return; }

    const Vector eye = m_pInfo->GetAbsOrigin() + offset;
    Vector point;
    unsigned int visibility = 0;
    if ( enemy == NULL || !CombatAimPoint( target, eye, point, false, &visibility ) )
    {
        if ( NetVar_HandleIsValid( m_uCombatEnemy ) ) ClearCombat( "zm-target-lost" );
        UpdateWeaponSelection( globals, cmd, NULL, 0.0f );
        return;
    }

    const uintp targetHandle = AgPB_RefEHandle( target );
    if ( !NetVar_HandleIsValid( targetHandle ) ) return;
    if ( targetHandle != m_uCombatEnemy || enemy->GetUserID() != m_iCombatEnemyUserId )
    {
        ClearCombat( "zm-acquired" );
        m_uCombatEnemy = targetHandle;
        m_iCombatEnemyUserId = enemy->GetUserID();
        const AgPBCombatSkill::Row row = AgPBCombatSkill::SkillRow( m_iCombatSkill );
        m_flCombatSurpriseUntil = globals->curtime + AgPBCombatSkill::ReactionDelay(
            RandomFloat( row.minReaction, row.maxReaction ), m_iCombatSkill, RandomInt( 0, 100 ) );
        m_bCombatRelease = true;
        SendRadio( !zombie && m_pInfo->GetHealth() <= 40 ? AgPBRadio::NeedBackup : AgPBRadio::EnemySpotted, globals->curtime );
    }

    uintp weaponHandle;
    bool knife;
    int clip, reserve;
    edict_t *weapon = CurrentCombatWeapon( weaponHandle, knife, clip, reserve );
    TrackCombatWeapon( weaponHandle, clip );
    if ( UpdateWeaponSelection( globals, cmd, target, ( point - eye ).Length() ) ) return;
    if ( weapon == NULL || !AgPBFiring::Supported( ClassifyWeapon( weapon ) ) || ( zombie && !knife ) )
    {
        m_pszCombatState = "weapon-unavailable";
        return;
    }
    AimAndFire( globals, cmd, target, eye, point, visibility, weapon, knife, clip, reserve, NULL );
}

void CAgPB::AimAndFire( CGlobalVars *globals, CBotCmd &cmd, edict_t *target, const Vector &eye, Vector point,
                       unsigned int visibility, edict_t *weapon, bool knife, int clip, int reserve, Vector *blockedCombatWish )
{
    const float now = globals->curtime;
    const AgPBFiring::Weapon weaponKind = ClassifyWeapon( weapon );
    const WeaponZoom zoom = ReadWeaponZoom( *this, weaponKind, now );
    if ( AgPBFiring::SupportsZoom( weaponKind ) ) m_flCombatZoomCheckAt = now;
    IPlayerInfo *enemyInfo = m_Ctx.pPlayerInfoManager->GetPlayerInfo( target );
    const Vector enemyOrigin = enemyInfo->GetAbsOrigin();
    const Vector enemyMins = enemyInfo->GetPlayerMins(), enemyMaxs = enemyInfo->GetPlayerMaxs();
    // SyPB GetAimPosition's any-visible-bit branch can select an occluded
    // head and makes its head-only fallback unreachable. Preserve its skill /
    // AWP preference only among individually viable regions. A covered head
    // requires verified weapon/material penetration; otherwise retain the
    // actual visible body/side/leg fallback, or a head-only observation.
    const bool preferHead = AgPBCombatSkill::AimAtHead( m_iCombatSkill, RandomInt( 30, 80 ),
        NamedWeapon( weapon, "weapon_awp" ), enemyInfo->GetHealth() );
    Vector headOffset;
    if ( preferHead && ViewOffset( target, headOffset ) )
    {
        const Vector head = enemyOrigin + headOffset;
        const bool headReachable = CanReachAimPoint( m_Ctx, m_pEdict, target, weapon, eye, head, !knife );
        if ( AgPBCombatVisibility::SelectPreferredSpot( visibility, preferHead, headReachable ) == AgPBCombatVisibility::Head && headReachable ) point = head;
    }
    if ( AgPB_ReadNetVarInt( target, "m_fFlags" ) & FL_DUCKING )
    {
        const Vector adjusted = point - Vector( 0, 0, 1.0f );
        if ( VisibleSpot( m_Ctx, m_pEdict, target, eye, adjusted ) ) point = adjusted;
    }
    const Vector idealPoint = point;
    if ( AgPBCombatSkill::HasAimNoise( m_iCombatSkill ) )
        for ( int axis = 0; axis < 3; ++axis )
            point[axis] += AgPBCombatSkill::RelativeNoise( enemyMins[axis], enemyMaxs[axis], RandomFloat( 0.0f, 1.0f ) );
    Vector punch( 0, 0, 0 );
    if ( !knife && !ReadVector( m_pEdict, "m_vecPunchAngle", punch ) )
    { m_pszCombatState = "punch-unavailable"; return; }
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
        previousDeviation, AgPBFiring::IsZoomed( zoom.level ) && !zoom.resuming );
    m_bCombatPerfectAim = AgPBCombatSkill::PerfectAim( m_iCombatSkill, wantsFire, sniper );
    m_vCombatAimDeviation.Init( AngleDiff( wanted.x + predictedPitch, current.pitch ),
        AngleDiff( wanted.y + predictedYaw, current.yaw ), 0 );
    cmd.viewangles.Init( view.pitch, view.yaw, 0 );
    m_flCombatAimError = MAX( fabsf( AngleDiff( wanted.x, view.pitch ) ), fabsf( AngleDiff( wanted.y, view.yaw ) ) );
    m_pszCombatState = now < m_flCombatSurpriseUntil ? "reacting" : "aiming";
    const bool reloadIntent = !knife && reserve > 0 && ( clip == 0 || m_bCombatReloadRequested );
    float playerAttack = 0.0f, weaponAttack = 0.0f;
    const bool knownPlayerAttack = ReadFloat( m_pEdict, "m_flNextAttack", playerAttack );
    const bool knownTimers = knownPlayerAttack &&
        ReadFloat( weapon, "m_flNextPrimaryAttack", weaponAttack );
    const bool nativeReady = knownTimers && now >= playerAttack && now >= weaponAttack;
    const bool aligning = m_bCombatRelease;
    m_bCombatRelease = false; // Only acquisition/equipment ownership releases a command.
    AgPBFiring::Input fire;
    fire.weapon = weaponKind; fire.now = now; fire.distance = ( point - eye ).Length();
    fire.skill = m_iCombatSkill; fire.previousAttack = ( m_LastSubmittedCmd.buttons & IN_ATTACK ) != 0;
    fire.canAttack = nativeReady; fire.reacted = wantsFire && !aligning && knownTimers;
    fire.clipReady = knife || clip > 0; fire.reloading = reloadIntent;
    fire.visibility = AgPBCombatVisibility::Summary( visibility ); fire.randomUnit = RandomFloat( 0.0f, 1.0f );
    if ( reloadIntent && nativeReady ) cmd.buttons |= IN_RELOAD;
    // Existing DoFirePause timers are consulted by FireWeapon on a fresh
    // ThinkFrame only. A successful sniper request sets now+0.1 but retains
    // this decision's button until the next frame, including native cooldown.
    fire.paused = AgPBFiring::DecisionDue( m_CombatFire, fire ) &&
        !knife && fire.distance >= 256.0f && now < m_flCombatFirePause;
    Vector fireDirection;
    AngleVectors( QAngle( view.pitch + 2.0f * punch.x, view.yaw + 2.0f * punch.y, 0 ), &fireDirection );
    if ( knife && blockedCombatWish == NULL )
        fire.lineClear = AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, eye + fireDirection );
    else if ( knife )
        fire.lineClear = FirstHit( m_Ctx, m_pEdict, target, eye, eye + fireDirection * 40.0f ) &&
            AgPB_ObstacleMeleeClear( m_Ctx, m_pEdict, target, eye, point );
    else fire.lineClear = FireRayAllowed( m_Ctx, m_pEdict, target, weapon, eye, fireDirection, idealPoint );
    const bool shotEligible = fire.reacted && fire.lineClear && fire.clipReady && !reloadIntent;
    const bool burstSwitch = shotEligible &&
        now >= m_flNextBurstRequest && now >= m_CombatFire.nextShotAt &&
        ( m_LastSubmittedCmd.buttons & IN_ATTACK2 ) == 0 &&
        AgPBFiring::DecisionDue( m_CombatFire, fire ) && BurstSwitchReady( *this, weapon, fire, playerAttack );
    const auto zoomAction = AgPBFiring::CombatZoomAction( fire.weapon, ( idealPoint - eye ).Length(), fire.skill, zoom.level );
    const bool closeZoom = zoomAction == AgPBFiring::ZoomAction::Close;
    // A confirmed close target can warrant unzooming even before a clear shot.
    const bool zoomWaiting = ( closeZoom && !reloadIntent ) ||
        ( shotEligible && ( zoomAction == AgPBFiring::ZoomAction::Open || zoom.resuming || zoom.settling ) );
    // Do not let a held rifle repeatedly restart the native cooldown before
    // the requested zoom edge can reach ItemPostFrame. This also allows a
    // two-stage sniper zoom to settle before the first shot.
    const bool zoomReady = closeZoom ? knownPlayerAttack && now >= playerAttack :
        nativeReady && now >= m_CombatFire.nextShotAt;
    const bool zoomSwitch = zoomWaiting && zoomAction != AgPBFiring::ZoomAction::None &&
        !zoom.resuming && !zoom.settling && zoomReady && ( m_LastSubmittedCmd.buttons & IN_ATTACK2 ) == 0 &&
        SecondaryAttackReady( weapon, now );
    // The mode command consumes this decision. Drop the primary latch so
    // the next shot gets a fresh recoil check with the observed mode.
    fire.paused = fire.paused || burstSwitch || zoomWaiting;
    AgPBFiring::Output proposal = AgPBFiring::Step( m_CombatFire, fire );
    const bool decisionTick = proposal.freshDecision;
    if ( !wantsFire ) m_bCombatSniperStop = false;
    // DoFirePause belongs to a valid distant fire attempt. In particular, an
    // unreacted/blocked/semi-delay tick must not restart the sniper pause.
    if ( decisionTick && proposal.attack && !knife && fire.distance >= 256.0f )
    {
        if ( AgPBCombatSkill::RecoilExceeded( m_iCombatSkill, punch.x, punch.y, fire.distance ) )
        {
            if ( m_flCombatFirePause < now - 0.4f )
                m_flCombatFirePause = now + RandomFloat( 0.4f, AgPBCombatSkill::RecoilPauseMaximum( m_iCombatSkill ) );
            fire.paused = true;
        }
        if ( sniper && !fire.paused )
        {
            Vector velocity;
            m_bCombatSniperStop = true;
            m_flCombatFirePause = now + 0.1f;
            fire.paused = !ReadVector( m_pEdict, "m_vecVelocity", velocity ) || velocity.Length() > 2.0f;
        }
        if ( fire.paused ) proposal = AgPBFiring::Step( m_CombatFire, fire );
    }
    m_pszCombatFire = AgPBFiring::ReasonName( proposal.reason ); m_bCombatFireHold = proposal.hold;
    if ( burstSwitch )
    {
        // Source consumes secondary attack before primary in ItemPostFrame.
        // Future switch decisions read the actual mode. Native weapon code
        // schedules the remaining bullets when primary attack starts.
        cmd.buttons |= IN_ATTACK2;
        m_flNextBurstRequest = now + 0.3f;
        m_pszCombatFire = "burst-switch";
        m_bCombatFireHold = false;
        m_pszCombatState = "burst-switch";
    }
    else if ( zoomWaiting )
    {
        if ( zoomSwitch ) cmd.buttons |= IN_ATTACK2;
        m_pszCombatState = m_pszCombatFire = zoomSwitch ? "zoom-switch" : "zoom-wait";
        m_bCombatFireHold = false;
    }
    else if ( proposal.attack ) { cmd.buttons |= IN_ATTACK; m_pszCombatState = "firing"; }
    else if ( reloadIntent ) m_pszCombatState = "reloading";
    else if ( fire.paused ) m_pszCombatState = "recoil-pause";
    else if ( !fire.lineClear ) m_pszCombatState = "fire-blocked";
    // CombatFight uses this command's actual attack proposal. An auto trigger
    // remains held through native cooldown; semi-auto release/AI-pause commands
    // may move. Braking/posture can still withhold the final submitted shot.
    if ( blockedCombatWish != NULL )
        UpdateCombatMovement( globals, cmd, target, weapon, ( cmd.buttons & IN_ATTACK ) != 0,
                              reloadIntent, decisionTick, *blockedCombatWish );
    const bool submittedAttack = ( cmd.buttons & IN_ATTACK ) != 0;
    AgPBFiring::Commit( m_CombatFire, fire, proposal, submittedAttack );
    if ( submittedAttack ) m_bCombatSniperStop = false;
    if ( submittedAttack ) ++m_uCombatAttackCommands;
}

void CAgPB::ReportCombat() const
{
    const float now = gpGlobals != NULL ? gpGlobals->curtime : 0.0f;
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
    RouteReport( "combat movement: policy=sypb_ground_v1 motion=%s distance=%.3f desired=(%.3f,%.3f) blocked=%d speed=%.3f",
        m_pszCombatMotion, m_flCombatMoveDistance, m_flCombatMoveForward, m_flCombatMoveSide,
        m_bCombatMoveBlocked ? 1 : 0, m_flCombatMoveSpeed );
    RouteReport( "combat fire: policy=sypb_primary_v1 state=%s hold=%d next-ai=%.6f holding-position=%d task-crouch=%d decision=%llu decision-next=%.6f sniper-stop=%d",
        m_pszCombatFire, m_bCombatFireHold ? 1 : 0, m_CombatFire.nextShotAt,
        IsHoldingTaskPosition() ? 1 : 0, TaskRequiresCrouch() ? 1 : 0,
        (unsigned long long)m_CombatFire.decisionId, m_CombatFire.nextDecisionAt, m_bCombatSniperStop ? 1 : 0 );
    uintp heldHandle = (uintp)INVALID_EHANDLE_INDEX;
    edict_t *held = GetNetVarHandleValue( "m_hActiveWeapon", &heldHandle ) ? HandleToEdict( heldHandle ) : NULL;
    const AgPBFiring::Weapon heldKind = ClassifyWeapon( held );
    if ( AgPBFiring::SupportsBurstMode( heldKind ) )
        RouteReport( "combat burst: mode=%d remaining=%d retry-at=%.3f",
            AgPB_ReadNetVarInt( held, "m_bBurstMode", -1 ),
            AgPB_ReadNetVarInt( held, "m_iBurstShotsRemaining", -1 ), m_flNextBurstRequest );
    if ( AgPBFiring::SupportsZoom( heldKind ) )
    {
        const WeaponZoom zoom = ReadWeaponZoom( *this, heldKind, now );
        RouteReport( "combat zoom: fov=%d default-fov=%d scoped=%d resuming=%d settling=%d last-check=%.3f",
            GetNetVarInt( "m_iFOV", -1 ), GetNetVarInt( "m_iDefaultFOV", -1 ), AgPBFiring::IsZoomed( zoom.level ) ? 1 : 0,
            zoom.resuming ? 1 : 0, zoom.settling ? 1 : 0, m_flCombatZoomCheckAt );
    }
    AgPBBullet::Profile profile; const char *ammo = "unknown";
    const bool known = ReadBulletProfile( held, profile, &ammo );
    RouteReport( "weapon ballistics: known=%d class=%s ammo=%s handle=0x%llX mode=%d surfaces=%s power=%.2f penetration-distance=%.1f count=%d base-damage=%.2f range=%.1f range-modifier=%.4f",
        known ? 1 : 0, held != NULL ? held->GetClassName() : "none", ammo, (unsigned long long)heldHandle,
        held != NULL ? AgPB_ReadNetVarInt( held, "m_weaponMode", -1 ) : -1, AgPB_SurfaceProps() != NULL ? "ok" : "unavailable",
        profile.power, profile.penetrationDistance, profile.count, profile.damage, profile.range, profile.rangeModifier );
    if ( m_iIndex >= 1 && m_iIndex < MAX_EDICTS && s_BulletReadback[m_iIndex].owner == AgPB_RefEHandle( m_pEdict ) )
    {
        const BulletReadback &probe = s_BulletReadback[m_iIndex];
        RouteReport( "bullet estimate: path=%s allowed=%d reason=%s target=0x%llX profile=%d age=%.2f walls=%d material=%c/%c thickness=%.2f remaining-base-damage=%.2f",
            probe.requireTarget ? "ideal" : "command", probe.allowed ? 1 : 0, probe.reason, (unsigned long long)probe.target,
            probe.knownProfile ? 1 : 0, now - probe.observedAt, probe.walls,
            probe.enterMaterial >= 32 && probe.enterMaterial < 127 ? probe.enterMaterial : '?',
            probe.exitMaterial >= 32 && probe.exitMaterial < 127 ? probe.exitMaterial : '?', probe.thickness, probe.damage );
    }
    ReportSearch();
}
