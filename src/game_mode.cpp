/**
 * AgPB - ZR availability and the current map select the runtime profile.
 */

#include <climits>
#include <cmath>

#include <edict.h>
#include <tier1/convar.h>
#include <tier1/strtools.h>
#include <tier1/utlvector.h>

#include "game_mode.h"

extern CGlobalVars *gpGlobals;

static AgPBGameMode g_AppliedMode = AgPB_MODE_NORMAL;
static bool g_ZRSeenOnMap = false;
static bool g_AppliedZRAvailable = false;

struct AgPBZRRoleEntry
{
	int userId;
	AgPBZRRole role;
};

static CUtlVector<AgPBZRRoleEntry> g_ZRRoles;
static float g_ZRRoleSnapshotTime = -1.0f;
static constexpr float kZRRoleSnapshotLifetime = 1.5f;

static void ClearZRRoles()
{
	g_ZRRoles.RemoveAll();
	g_ZRRoleSnapshotTime = -1.0f;
}

static void OnZRAvailabilityChanged( IConVar *pVar, const char *, float )
{
	if ( static_cast<ConVar *>( pVar )->GetBool() )
		g_ZRSeenOnMap = true;
	else
		ClearZRRoles();
}

static ConVar agpb_mode( "agpb_mode", "auto", FCVAR_GAMEDLL,
                         "AgPB game mode: auto, normal, zm, or ze. zombie uses the map prefix." );
static ConVar agpb_zr_available( "agpb_zr_available", "0", FCVAR_GAMEDLL | FCVAR_HIDDEN | FCVAR_DONTRECORD,
                                 "Availability of the Zombie:Reloaded API, published by the SourceMod bridge.",
                                 OnZRAvailabilityChanged );

static bool ReadPositiveNumber( const char *&cursor, int &number )
{
	while ( *cursor == ' ' )
		++cursor;

	int value = 0;
	while ( *cursor >= '0' && *cursor <= '9' )
	{
		const int digit = *cursor++ - '0';
		if ( value > ( INT_MAX - digit ) / 10 )
			return false;
		value = value * 10 + digit;
	}

	number = value;
	return value > 0 && ( *cursor == ' ' || *cursor == '\0' );
}

static int FindZRRoleEntry( const CUtlVector<AgPBZRRoleEntry> &roles, int userId )
{
	for ( int i = 0; i < roles.Count(); ++i )
	{
		if ( roles[i].userId == userId )
			return i;
	}
	return roles.InvalidIndex();
}

static void OnZRRoleSnapshotChanged( IConVar *pVar, const char *, float )
{
	if ( !AgPB_IsZRAvailable() || gpGlobals == NULL || !std::isfinite( gpGlobals->realtime ) )
	{
		ClearZRRoles();
		return;
	}

	const char *cursor = static_cast<ConVar *>( pVar )->GetString();
	if ( V_strncmp( cursor, "drop ", 5 ) == 0 )
	{
		int userId;
		cursor += 5;
		if ( !ReadPositiveNumber( cursor, userId ) || *cursor != '\0' )
		{
			ClearZRRoles();
			return;
		}
		const int entry = FindZRRoleEntry( g_ZRRoles, userId );
		if ( entry != g_ZRRoles.InvalidIndex() )
			g_ZRRoles.FastRemove( entry );
		return;
	}

	// A fresh heartbeat replaces the roster. Empty or malformed packets clear it.
	ClearZRRoles();
	int heartbeat;
	if ( !ReadPositiveNumber( cursor, heartbeat ) )
		return;

	CUtlVector<AgPBZRRoleEntry> roles;
	while ( *cursor != '\0' )
	{
		int userId, role;
		if ( roles.Count() >= gpGlobals->maxClients ||
		     !ReadPositiveNumber( cursor, userId ) || !ReadPositiveNumber( cursor, role ) ||
		     ( role != 1 && role != 2 ) || FindZRRoleEntry( roles, userId ) != roles.InvalidIndex() )
			return;

		AgPBZRRoleEntry entry = { userId, role == 1 ? AgPBZRRole::Human : AgPBZRRole::Zombie };
		roles.AddToTail( entry );
	}

	g_ZRRoles.Swap( roles );
	g_ZRRoleSnapshotTime = gpGlobals->realtime;
}

static ConVar agpb_zr_roles( "agpb_zr_roles", "", FCVAR_GAMEDLL | FCVAR_HIDDEN | FCVAR_DONTRECORD,
                             "Server-only ZR roster snapshot or drop <userid>.",
                             OnZRRoleSnapshotChanged );

AgPBZRRole AgPB_ZRRoleForUserId( int userId )
{
	if ( userId <= 0 || !AgPB_IsZRAvailable() || gpGlobals == NULL || g_ZRRoleSnapshotTime < 0.0f )
		return AgPBZRRole::Unknown;

	const float age = gpGlobals->realtime - g_ZRRoleSnapshotTime;
	if ( !std::isfinite( age ) || age < 0.0f || age > kZRRoleSnapshotLifetime )
		return AgPBZRRole::Unknown;

	const int entry = FindZRRoleEntry( g_ZRRoles, userId );
	return entry != g_ZRRoles.InvalidIndex() ? g_ZRRoles[entry].role : AgPBZRRole::Unknown;
}

static AgPBGameMode InfectionMapMode()
{
	const char *pszMap = gpGlobals != NULL ? STRING( gpGlobals->mapname ) : "";
	const char *pszBaseName = V_UnqualifiedFileName( pszMap );
	return pszBaseName != NULL && Q_strnicmp( pszBaseName, "ze_", 3 ) == 0 ? AgPB_MODE_ZE : AgPB_MODE_ZM;
}

const char *AgPB_RequestedGameMode()
{
	return agpb_mode.GetString();
}

bool AgPB_IsZRAvailable()
{
	return agpb_zr_available.GetBool();
}

AgPBGameMode AgPB_GameMode()
{
	const char *pszMode = AgPB_RequestedGameMode();
	if ( Q_stricmp( pszMode, "auto" ) == 0 )
	{
		// A bridge/ZR reload must not start native objectives in the middle
		// of an infection match.
		return ( AgPB_IsZRAvailable() || g_ZRSeenOnMap ) ? InfectionMapMode() : AgPB_MODE_NORMAL;
	}
	if ( Q_stricmp( pszMode, "zm" ) == 0 )
		return AgPB_MODE_ZM;
	if ( Q_stricmp( pszMode, "ze" ) == 0 )
		return AgPB_MODE_ZE;
	if ( Q_stricmp( pszMode, "zombie" ) == 0 || Q_strcmp( pszMode, "1" ) == 0 )
		return InfectionMapMode();
	return AgPB_MODE_NORMAL;
}

bool AgPB_UpdateGameMode()
{
	const bool zrAvailable = AgPB_IsZRAvailable();
	g_ZRSeenOnMap = g_ZRSeenOnMap || zrAvailable;
	const AgPBGameMode mode = AgPB_GameMode();
	const bool changed = mode != g_AppliedMode ||
	                     ( mode != AgPB_MODE_NORMAL && zrAvailable != g_AppliedZRAvailable );
	g_AppliedMode = mode;
	g_AppliedZRAvailable = zrAvailable;
	return changed;
}

void AgPB_ResetGameMode()
{
	ClearZRRoles();
	g_AppliedMode = AgPB_MODE_NORMAL;
	g_ZRSeenOnMap = false;
	g_AppliedZRAvailable = false;
}

const char *AgPB_GameModeName( AgPBGameMode eMode )
{
	switch ( eMode )
	{
		case AgPB_MODE_ZM: return "zm";
		case AgPB_MODE_ZE: return "ze";
		default: return "normal";
	}
}
