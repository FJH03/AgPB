/*
 * Ordinary-mode communication behavior adapted from SyPB 4c364fb,
 * Project SyPB/SyPB_BOT/source/{chatlib,interface,basecode,combat}.cpp.
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 * SyPB permission notice: tasks.cpp. AgPB adaptation: GPL-3.0-only.
 * CS-EBOT 2333562 was compared for fake-client command transport.
 */
#include <tier1/strtools.h>
#include <tier1/utlvector.h>
#include <irecipientfilter.h>
#include <mathlib/mathlib.h>
#include <shareddefs.h>
#include <vstdlib/random.h>
#include <climits>
#include "plugin.h"
#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "waypoint.h"

extern IVEngineServer *engine;
extern IPlayerInfoManager *playerinfomanager;
extern CGlobalVars *gpGlobals;
extern bool AgPB_IsRoundFrozen();
extern bool AgPB_IsEnabled();
extern unsigned int AgPB_MapSerial();

#if !defined( AGPB_GAME_CSGO )
bool g_AgPBNativeRadioAudienceReady = false;
#endif

namespace
{
ConVar agpb_chat( "agpb_chat", "1", FCVAR_GAMEDLL, "Enable bot chat in normal and ZM modes." );
ConVar agpb_radio( "agpb_radio", "1", FCVAR_GAMEDLL, "Enable normal and ZM radio calls and supported orders." );

void OnAgentChatOverrideChanged( IConVar *var, const char *, float )
{
	if ( static_cast<ConVar *>( var )->GetBool() ) return;

	CAgPBManager &bots = AgPB_Bots();
	for ( int i = 0; i < bots.Count(); ++i )
		bots.Get( i )->ReleaseExternalChatDecision();
}

ConVar agpb_agent_chat_override( "agpb_agent_chat_override", "0", FCVAR_GAMEDLL,
	"Give a chat harness a bounded window to supply the bot response.",
	OnAgentChatOverrideChanged );

CUtlVector<int> g_ChatDispatch;
std::uint64_t g_NextChatId = 0;
std::uint64_t g_PendingIdleChatId = 0;
float g_NextChat = 0.0f;
float g_NextIdleChat = 0.0f;
int g_LastIdleLine = -1;
struct RadioCooldown
{
	float next = 0.0f;
	float repeat[25] = {};
};
RadioCooldown g_RadioCooldown[4]; // Normal T and CT, ZM human and zombie.
int g_RadioIgnoredBy[256] = {};

bool CanCommunicate( AgPBGameMode mode )
{
	return AgPB_IsEnabled() && ( mode == AgPB_MODE_NORMAL || mode == AgPB_MODE_ZM );
}

bool IsInfectionRadioMode( AgPBGameMode mode )
{
	return mode == AgPB_MODE_ZM || mode == AgPB_MODE_ZE;
}

bool CanUseRadio( AgPBGameMode mode )
{
	return AgPB_IsEnabled() && ( mode == AgPB_MODE_NORMAL || IsInfectionRadioMode( mode ) );
}

bool PlayingTeam( int team )
{
	return team == AgPB_TEAM_T || team == AgPB_TEAM_CT;
}

IPlayerInfo *ClientInfo( edict_t *client )
{
	if ( client == NULL || client->IsFree() || playerinfomanager == NULL ) return NULL;
	IPlayerInfo *info = playerinfomanager->GetPlayerInfo( client );
	return info != NULL && info->IsConnected() && info->IsPlayer() &&
		!info->IsHLTV() && !info->IsReplay() ? info : NULL;
}

bool LivingPlayer( edict_t *client, IPlayerInfo *info )
{
	return info != NULL && PlayingTeam( info->GetTeamIndex() ) && !info->IsObserver() &&
		AgPB_ReadNetVarInt( client, "m_lifeState", LIFE_DEAD ) == LIFE_ALIVE;
}

bool HasZRRole( edict_t *client, IPlayerInfo *info, AgPBZRRole role )
{
	return role != AgPBZRRole::Unknown && LivingPlayer( client, info ) &&
		AgPB_ZRRoleForUserId( info->GetUserID() ) == role;
}

bool HasLivingChatListener()
{
	if ( gpGlobals == NULL || engine == NULL ) return false;
	for ( int slot = 1; slot <= gpGlobals->maxClients; ++slot )
	{
		edict_t *client = engine->PEntityOfEntIndex( slot );
		IPlayerInfo *info = ClientInfo( client );
		if ( LivingPlayer( client, info ) && !info->IsFakeClient() ) return true;
	}
	return false;
}

edict_t *FindPlayer( int userId )
{
	if ( userId <= 0 || gpGlobals == NULL || engine == NULL || playerinfomanager == NULL ) return NULL;
	for ( int slot = 1; slot <= gpGlobals->maxClients; ++slot )
	{
		edict_t *client = engine->PEntityOfEntIndex( slot );
		IPlayerInfo *info = ClientInfo( client );
		if ( info != NULL && info->GetUserID() == userId ) return client;
	}
	return NULL;
}

const char *ChatCue( AgPBChat cue )
{
	switch ( cue )
	{
	case AgPBChat::Reply: return "reply";
	case AgPBChat::Join: return "join";
	case AgPBChat::Kill: return "kill";
	case AgPBChat::Death: return "death";
	case AgPBChat::Plant: return "plant";
	case AgPBChat::Idle: return "idle";
	default: return "none";
	}
}

bool ContainsWord( const char *text, const char *word )
{
	const int length = V_strlen( word );
	for ( const char *match = text; ( match = V_stristr( match, word ) ) != NULL; ++match )
	{
		if ( ( match == text || !V_isalnum( static_cast<unsigned char>( match[-1] ) ) ) &&
		     !V_isalnum( static_cast<unsigned char>( match[length] ) ) ) return true;
	}
	return false;
}

const char *LocalReply( const char *text )
{
	if ( V_strstr( text, "你好" ) || ContainsWord( text, "hi" ) ||
	     ContainsWord( text, "hello" ) || ContainsWord( text, "hey" ) )
		return RandomInt( 0, 1 ) == 0 ? "hey" : "hello";
	if ( ContainsWord( text, "gg" ) || ContainsWord( text, "good game" ) ) return "gg";
	if ( V_strstr( text, "谢谢" ) || ContainsWord( text, "thanks" ) || ContainsWord( text, "thank you" ) ) return "no problem";
	if ( V_strstr( text, "机器人" ) || ContainsWord( text, "bot" ) ||
	     ContainsWord( text, "bots" ) || ContainsWord( text, "agpb" ) )
		return RandomInt( 0, 1 ) == 0 ? "ready when you are" : "I'm here";
	const char *const replies[] = { "fair enough", "gotcha", "heh", "all right" };
	return RandomInt( 1, 100 ) <= 35 ? replies[RandomInt( 0, ARRAYSIZE( replies ) - 1 )] : "";
}

const char *IdleChat()
{
	const char *const lines[] = {
		"anyone else still warming up?", "still here", "how's everyone doing?",
		"this map keeps me on my toes", "good to have some company", "ready for another round?"
	};
	int line = RandomInt( 0, ARRAYSIZE( lines ) - ( g_LastIdleLine < 0 ? 1 : 2 ) );
	if ( g_LastIdleLine >= 0 && line >= g_LastIdleLine ) ++line;
	g_LastIdleLine = line;
	return lines[line];
}

const char *LocalChat( const AgPBChatState &chat )
{
	switch ( chat.cue )
	{
	case AgPBChat::Reply: return LocalReply( chat.input );
	case AgPBChat::Join: return RandomInt( 1, 100 ) <= 20 ? "gl hf" : "";
	case AgPBChat::Kill: return RandomInt( 1, 100 ) <= 15 ? "good fight" : "";
	case AgPBChat::Death: return RandomInt( 1, 100 ) <= 15 ? "nice shot" : "";
	case AgPBChat::Plant: return RandomInt( 0, 1 ) == 0 ? "bomb planted" : "cover the site";
	case AgPBChat::Idle: return IdleChat();
	default: return "";
	}
}

bool ReadUnsigned( const char *text, std::uint64_t &value )
{
	value = 0;
	if ( !text[0] ) return false;
	for ( const char *at = text; *at; ++at )
	{
		if ( *at < '0' || *at > '9' || value > ( UINT64_MAX - ( *at - '0' ) ) / 10 ) return false;
		value = value * 10 + ( *at - '0' );
	}
	return true;
}

int HexDigit( char digit )
{
	if ( digit >= '0' && digit <= '9' ) return digit - '0';
	if ( digit >= 'a' && digit <= 'f' ) return digit - 'a' + 10;
	if ( digit >= 'A' && digit <= 'F' ) return digit - 'A' + 10;
	return -1;
}

bool DecodeReply( const char *hex, char ( &text )[121] )
{
	if ( V_strcmp( hex, "-" ) == 0 ) { text[0] = '\0'; return true; }
	const int length = V_strlen( hex );
	if ( length == 0 || length > 240 || length % 2 != 0 ) return false;
	bool hasText = false;
	for ( int i = 0; i < length; i += 2 )
	{
		const int high = HexDigit( hex[i] ), low = HexDigit( hex[i + 1] );
		if ( high < 0 || low < 0 ) return false;
		const int byte = high * 16 + low;
		if ( byte < 32 || byte == 127 || byte == '"' || byte == ';' || byte == '\\' ) return false;
		text[i / 2] = static_cast<char>( byte );
		hasText |= byte != ' ';
	}
	text[length / 2] = '\0';
	wchar_t wide[121];
	char canonical[121];
	return hasText && V_UTF8ToUnicode( text, wide, sizeof( wide ), STRINGCONVERT_FAIL ) > 0 &&
		V_UnicodeToUTF8( wide, canonical, sizeof( canonical ), STRINGCONVERT_FAIL ) > 0 && V_strcmp( canonical, text ) == 0;
}

bool IsFollow( AgPBRadio call )
{
	return call == AgPBRadio::FollowMe || call == AgPBRadio::CoverMe;
}

bool IsActionOrder( AgPBRadio call )
{
	switch ( call )
	{
	case AgPBRadio::CoverMe: case AgPBRadio::HoldPosition: case AgPBRadio::Regroup:
	case AgPBRadio::FollowMe: case AgPBRadio::TakingFire: case AgPBRadio::Go:
	case AgPBRadio::FallBack: case AgPBRadio::GetInPosition: case AgPBRadio::StormFront:
	case AgPBRadio::ReportTeam: case AgPBRadio::NeedBackup: case AgPBRadio::GetOut:
		return true;
	default: return false;
	}
}

#if !defined( AGPB_GAME_CSGO )
const char *NativeRadioCommand( AgPBRadio call )
{
	// The game maps the alias to a line and chooses the speaker's audio.
	static const char *const commands[] = {
		NULL, NULL, "coverme", "takepoint", "holdpos", "regroup", "followme", "takingfire",
		NULL, "go", "fallback", "sticktog", "getinpos", "stormfront", "report",
		NULL, "roger", "enemyspot", "needbackup", "sectorclear", "inposition", "reportingin",
		"getout", "negative", "enemydown"
	};
	const int slot = static_cast<int>( call );
	return slot >= 0 && slot < ARRAYSIZE( commands ) ? commands[slot] : NULL;
}

class CommunicationRecipients : public IRecipientFilter
{
public:
	bool IsReliable() const override { return true; }
	bool IsInitMessage() const override { return false; }
	int GetRecipientCount() const override { return slots.Count(); }
	int GetRecipientIndex( int i ) const override { return slots[i]; }
	CUtlVector<int> slots;
};

struct NativeRadioDispatch
{
	edict_t *speaker;
	IRecipientFilter *recipients;
	int radioText, sendAudio;
	bool iconPending;
};
NativeRadioDispatch *g_NativeRadioDispatch = NULL;

bool NativeRadioSafe()
{
#if defined( AGPB_GAME_CSCZS )
	// [game-diff] csczs Radio invokes CCSBot after sending the native messages.
	// The existing guard registers its status command after patch validation.
	const ConVar *enabled = g_pCVar != NULL ? g_pCVar->FindVar( "sm_agpb_fakeclient_guard" ) : NULL;
	return enabled != NULL && enabled->GetBool() && g_pCVar->FindCommand( "sm_agpb_guard_status" ) != NULL;
#else
	return true;
#endif
}

void SendZMTeamChat( int sender, int team, AgPBZRRole role, const char *name, const char *text )
{
	if ( engine == NULL || gpGlobals == NULL || sender <= 0 || sender > 255 ) return;
	const int sayText = g_SMAPI->FindUserMessage( "SayText2", NULL );
	if ( sayText < 0 ) return;
	CommunicationRecipients recipients;
	for ( int slot = 1; slot <= gpGlobals->maxClients; ++slot )
	{
		edict_t *client = engine->PEntityOfEntIndex( slot );
		IPlayerInfo *info = ClientInfo( client );
		if ( HasZRRole( client, info, role ) && !info->IsFakeClient() ) recipients.slots.AddToTail( slot );
	}
	if ( recipients.GetRecipientCount() == 0 ) return;
	if ( bf_write *message = engine->UserMessageBegin( &recipients, sayText ) )
	{
		message->WriteByte( sender );
		message->WriteByte( 1 );
		message->WriteString( team == AgPB_TEAM_CT ? "Cstrike_Chat_CT" : "Cstrike_Chat_T" );
		message->WriteString( name );
		message->WriteString( text );
		message->WriteString( "" );
		message->WriteString( "" );
		engine->MessageEnd();
	}
}
#endif
}

bool AgPB_AgentChatOverrideEnabled()
{
	return agpb_agent_chat_override.GetBool();
}

void AgPB_BeginChatDispatch( bool teamOnly )
{
	g_ChatDispatch.AddToTail( teamOnly ? 1 : 0 );
}

void AgPB_EndChatDispatch()
{
	if ( g_ChatDispatch.Count() != 0 ) g_ChatDispatch.Remove( g_ChatDispatch.Count() - 1 );
}

void AgPB_ToggleIgnoreRadio( edict_t *client )
{
	IPlayerInfo *info = ClientInfo( client );
	if ( info == NULL || info->IsFakeClient() || engine == NULL ) return;
	const int slot = engine->IndexOfEdict( client );
	if ( slot <= 0 || slot >= ARRAYSIZE( g_RadioIgnoredBy ) ) return;
	const int userId = info->GetUserID();
	g_RadioIgnoredBy[slot] = g_RadioIgnoredBy[slot] == userId ? 0 : userId;
}

#if !defined( AGPB_GAME_CSGO )
IRecipientFilter *AgPB_NativeRadioMessageFilter( int message )
{
	if ( g_NativeRadioDispatch == NULL || g_NativeRadioDispatch->recipients == NULL ) return NULL;
	if ( message == g_NativeRadioDispatch->radioText ) g_NativeRadioDispatch->radioText = -1;
	else if ( message == g_NativeRadioDispatch->sendAudio ) g_NativeRadioDispatch->sendAudio = -1;
	else return NULL;
	return g_NativeRadioDispatch->recipients;
}

IRecipientFilter *AgPB_NativeRadioIconFilter()
{
	if ( g_NativeRadioDispatch == NULL || !g_NativeRadioDispatch->iconPending ) return NULL;
	g_NativeRadioDispatch->iconPending = false;
	return g_NativeRadioDispatch->recipients;
}
#endif

void AgPB_ResetCommunications()
{
	g_ChatDispatch.RemoveAll();
	g_PendingIdleChatId = 0;
	g_NextChat = 0.0f;
	g_NextIdleChat = 0.0f;
	g_LastIdleLine = -1;
	for ( RadioCooldown &cooldown : g_RadioCooldown ) cooldown = {};
	V_memset( g_RadioIgnoredBy, 0, sizeof( g_RadioIgnoredBy ) );
}

void AgPB_UpdateIdleChat( float now )
{
	if ( !agpb_chat.GetBool() || !CanCommunicate( AgPB_GameMode() ) || AgPB_IsRoundFrozen() ||
	     now < g_NextIdleChat || now < g_NextChat ) return;
	if ( g_NextIdleChat == 0.0f )
	{
		g_NextIdleChat = now + RandomFloat( 18.0f, 30.0f );
		return;
	}
	g_NextIdleChat = now + 3.0f;
	CAgPBManager &bots = AgPB_Bots();
	if ( bots.Count() == 0 || !HasLivingChatListener() ) return;
	const int first = RandomInt( 0, bots.Count() - 1 );
	for ( int i = 0; i < bots.Count(); ++i )
	{
		if ( !bots.Get( ( first + i ) % bots.Count() )->QueueIdleChat( now ) ) continue;
		g_NextIdleChat = now + RandomFloat( 18.0f, 30.0f );
		break;
	}
}

void AgPB_OnCommunicationEvent( IGameEvent *event )
{
	const AgPBGameMode mode = AgPB_GameMode();
	const char *name = event->GetName();
	const bool radio = V_strcmp( name, "player_radio" ) == 0;
	if ( gpGlobals == NULL || !( radio ? CanUseRadio( mode ) : CanCommunicate( mode ) ) ) return;
	const float now = gpGlobals->curtime;
	CAgPBManager &bots = AgPB_Bots();
	if ( V_strcmp( name, "player_say" ) == 0 && agpb_chat.GetBool() && g_ChatDispatch.Count() != 0 )
	{
		edict_t *speaker = FindPlayer( event->GetInt( "userid" ) );
		IPlayerInfo *info = ClientInfo( speaker );
		const char *text = event->GetString( "text" );
		if ( info == NULL || info->IsFakeClient() || !text[0] || bots.Count() == 0 ) return;
		const bool teamOnly = g_ChatDispatch.Tail() != 0;
		const int first = RandomInt( 0, bots.Count() - 1 );
		for ( int i = 0; i < bots.Count(); ++i )
		{
			if ( !bots.Get( ( first + i ) % bots.Count() )->QueueChat( AgPBChat::Reply, speaker, teamOnly, now, text ) ) continue;
			if ( g_NextIdleChat > 0.0f ) g_NextIdleChat = MAX( g_NextIdleChat, now + 5.0f );
			break;
		}
	}
	else if ( radio && agpb_radio.GetBool() && !AgPB_IsRoundFrozen() )
	{
		const AgPBRadio call = static_cast<AgPBRadio>( event->GetInt( "slot" ) );
		if ( !IsActionOrder( call ) || ( IsInfectionRadioMode( mode ) && call != AgPBRadio::ReportTeam ) ) return;
		edict_t *issuer = FindPlayer( event->GetInt( "userid" ) );
#if !defined( AGPB_GAME_CSGO )
		// Bot orders from SendRadio have already been routed internally.
		if ( g_NativeRadioDispatch != NULL && g_NativeRadioDispatch->speaker == issuer ) return;
#endif
		if ( !LivingPlayer( issuer, ClientInfo( issuer ) ) ) return;
		int followers = 0;
		const uintp handle = AgPB_RefEHandle( issuer );
		if ( IsFollow( call ) )
			for ( int i = 0; i < bots.Count(); ++i )
				followers += bots.Get( i )->FollowsRadioIssuer( handle ) ? 1 : 0;
		for ( int i = 0; i < bots.Count(); ++i )
		{
			CAgPB *bot = bots.Get( i );
			const bool following = IsFollow( call ) && bot->FollowsRadioIssuer( handle );
			if ( ( !IsFollow( call ) || following || followers < 2 ) && bot->QueueRadioOrder( call, issuer, now ) )
			{
				if ( call == AgPBRadio::ReportTeam ) break;
				if ( IsFollow( call ) && !following ) ++followers;
			}
		}
	}
	else if ( V_strcmp( name, "player_death" ) == 0 ||
	          ( mode == AgPB_MODE_NORMAL && V_strcmp( name, "bomb_planted" ) == 0 ) )
	{
		const int userid = event->GetInt( "userid" );
		const bool plant = V_strcmp( name, "bomb_planted" ) == 0;
		const int attacker = plant ? 0 : event->GetInt( "attacker" );
		IPlayerInfo *victim = plant ? NULL : ClientInfo( FindPlayer( userid ) );
		for ( int i = 0; i < bots.Count(); ++i )
		{
			CAgPB *bot = bots.Get( i );
			IPlayerInfo *info = bot->PlayerInfo();
			if ( info == NULL ) continue;
			if ( info->GetUserID() == userid )
			{
				if ( plant ) bot->QueueChat( AgPBChat::Plant, NULL, true, now, "bomb planted" );
				else bot->QueueChat( AgPBChat::Death, NULL, false, now, "bot died" );
			}
			else if ( mode == AgPB_MODE_NORMAL && info->GetUserID() == attacker && victim != NULL &&
			          victim->GetTeamIndex() != info->GetTeamIndex() )
			{
				bot->SendRadio( AgPBRadio::EnemyDown, now );
				bot->QueueChat( AgPBChat::Kill, NULL, false, now, "bot got a kill" );
			}
		}
	}
}

bool CAgPB::QueueChat( AgPBChat cue, edict_t *speaker, bool teamOnly, float now, const char *text )
{
	const AgPBGameMode mode = AgPB_GameMode();
	if ( !CanCommunicate( mode ) || !agpb_chat.GetBool() || !m_bAutonomous || m_Ctx.pHelpers == NULL ||
	     m_pInfo == NULL || !PlayingTeam( m_pInfo->GetTeamIndex() ) || now < m_Chat.nextAt ) return false;
	if ( m_Chat.id != 0 && now < m_Chat.expiresAt && ChatScopeValid() )
	{
		const bool canReplaceIdle = cue != AgPBChat::Idle && m_Chat.cue == AgPBChat::Idle;
		const bool canReplaceEvent = cue == AgPBChat::Reply && m_Chat.cue != AgPBChat::Reply && !m_Chat.externallyResolved;
		if ( !canReplaceIdle && !canReplaceEvent ) return false;
	}
	IPlayerInfo *source = ClientInfo( speaker );
	const bool alive = cue != AgPBChat::Death && IsAlive();
	const bool zmTeam = mode == AgPB_MODE_ZM && teamOnly;
#if defined( AGPB_GAME_CSGO )
	// [game-diff] ZR team chat uses the Source bit-buffer SayText2.
	if ( zmTeam ) return false;
#endif
	if ( zmTeam && !alive ) return false;
	if ( cue == AgPBChat::Reply && ( source == NULL || !PlayingTeam( source->GetTeamIndex() ) ||
	     alive != LivingPlayer( speaker, source ) ||
	     ( teamOnly && !zmTeam && source->GetTeamIndex() != m_pInfo->GetTeamIndex() ) ) ) return false;
	AgPBZRRole role = AgPBZRRole::Unknown, speakerRole = AgPBZRRole::Unknown;
	if ( mode == AgPB_MODE_ZM && alive )
	{
		role = AgPB_ZRRoleForUserId( m_pInfo->GetUserID() );
		if ( role == AgPBZRRole::Unknown ) return false;
		if ( source != NULL )
		{
			speakerRole = AgPB_ZRRoleForUserId( source->GetUserID() );
			if ( speakerRole == AgPBZRRole::Unknown || ( teamOnly && role != speakerRole ) ) return false;
		}
	}
	const uintp handle = source != NULL ? AgPB_RefEHandle( speaker ) : 0;
	if ( source != NULL && !NetVar_HandleIsValid( handle ) ) return false;
	if ( ++g_NextChatId == 0 ) ++g_NextChatId;
	m_Chat.id = g_NextChatId;
	// A real player's reply gets the next opportunity across all bots.
	if ( cue == AgPBChat::Idle ) g_PendingIdleChatId = m_Chat.id;
	else if ( cue == AgPBChat::Reply ) g_PendingIdleChatId = 0;
	m_Chat.cue = cue;
	m_Chat.mode = mode;
	m_Chat.role = role;
	m_Chat.speakerRole = speakerRole;
	m_Chat.speaker = handle;
	m_Chat.userId = source != NULL ? source->GetUserID() : 0;
	m_Chat.speakerTeam = source != NULL ? source->GetTeamIndex() : 0;
	m_Chat.botUserId = m_pInfo->GetUserID();
	m_Chat.team = m_pInfo->GetTeamIndex();
	m_Chat.alive = alive;
	m_Chat.teamOnly = teamOnly;
	m_Chat.map = AgPB_MapSerial();
	m_Chat.round = BotObjectives().Snapshot().roundEpoch;
	m_Chat.readyAt = now + RandomFloat( 0.8f, 1.4f );
	m_Chat.externalUntil = AgPB_AgentChatOverrideEnabled() ? now + 2.5f : 0.0f;
	m_Chat.expiresAt = now + 8.0f;
	m_Chat.externallyResolved = false;
	m_Chat.output[0] = '\0';
	V_strncpy( m_Chat.input, text, sizeof( m_Chat.input ) );
	return true;
}

bool CAgPB::CanIdleChat() const
{
	return !AgPB_IsRoundFrozen() && ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) == 0 &&
		!NetVar_HandleIsValid( m_uCombatEnemy ) && !m_ZM.retreating &&
		m_Tasks.Current().desire < AgPBTaskPriority::Objective;
}

bool CAgPB::QueueIdleChat( float now )
{
	return IsAlive() && CanIdleChat() && QueueChat( AgPBChat::Idle, NULL, false, now, "casual chat during a quiet moment" );
}

bool CAgPB::ChatScopeValid() const
{
	if ( !CanCommunicate( m_Chat.mode ) || m_Chat.mode != AgPB_GameMode() || !agpb_chat.GetBool() ||
	     !m_bAutonomous || !IsValid() || m_pInfo == NULL ||
	     m_Chat.botUserId != m_pInfo->GetUserID() || m_Chat.team != m_pInfo->GetTeamIndex() ||
	     m_Chat.map != AgPB_MapSerial() || m_Chat.round != BotObjectives().Snapshot().roundEpoch ) return false;
	if ( m_Chat.cue != AgPBChat::Death && m_Chat.alive != IsAlive() ) return false;
	if ( m_Chat.cue == AgPBChat::Idle && ( m_Chat.id != g_PendingIdleChatId || !CanIdleChat() ) ) return false;
	const bool liveZM = m_Chat.mode == AgPB_MODE_ZM && m_Chat.alive;
	if ( liveZM && !HasZRRole( m_pEdict, m_pInfo, m_Chat.role ) ) return false;
	if ( m_Chat.userId == 0 ) return true;
	edict_t *speaker = HandleToEdict( m_Chat.speaker );
	IPlayerInfo *source = ClientInfo( speaker );
	return source != NULL && !source->IsFakeClient() && source->GetUserID() == m_Chat.userId &&
		source->GetTeamIndex() == m_Chat.speakerTeam && LivingPlayer( speaker, source ) == m_Chat.alive &&
		( !liveZM || AgPB_ZRRoleForUserId( m_Chat.userId ) == m_Chat.speakerRole );
}

void CAgPB::ReleaseExternalChatDecision()
{
	m_Chat.externalUntil = 0.0f;
	m_Chat.externallyResolved = false;
	m_Chat.output[0] = '\0';
}

void CAgPB::ReportPendingChat( float now ) const
{
	if ( m_Chat.id == 0 || m_Chat.externallyResolved || m_Chat.externalUntil <= 0.0f ||
	     now >= m_Chat.externalUntil || !ChatScopeValid() ) return;
	static const char digits[] = "0123456789abcdef";
	char hex[sizeof( m_Chat.input ) * 2];
	int i = 0;
	for ( ; m_Chat.input[i]; ++i )
	{
		const unsigned char byte = static_cast<unsigned char>( m_Chat.input[i] );
		hex[2 * i] = digits[byte >> 4];
		hex[2 * i + 1] = digits[byte & 15];
	}
	hex[2 * i] = '\0';
	META_CONPRINTF( "[AgPB] chat-event id=%llu map=%u round=%u userid=%d bot=%d botuserid=%d team=%d teamonly=%d ttl=%.3f cue=%s text_hex=%s\n",
		(unsigned long long)m_Chat.id, m_Chat.map, m_Chat.round, m_Chat.userId, m_iIndex, m_Chat.botUserId,
		m_Chat.team, m_Chat.teamOnly ? 1 : 0, m_Chat.externalUntil - now, ChatCue( m_Chat.cue ), hex );
}

const char *CAgPB::AcceptChatReply( std::uint64_t id, unsigned int map, unsigned int round,
	int userId, int botUserId, const char *text, float now )
{
	if ( m_Chat.id != id ) return "unknown";
	if ( m_Chat.externallyResolved ) return "duplicate";
	if ( m_Chat.externalUntil <= 0.0f || now >= m_Chat.externalUntil || !ChatScopeValid() ) return "stale";
	if ( map != m_Chat.map || round != m_Chat.round || userId != m_Chat.userId || botUserId != m_Chat.botUserId )
		return "scope";
	V_strncpy( m_Chat.output, text, sizeof( m_Chat.output ) );
	m_Chat.externallyResolved = true;
	return "ok";
}

// Keep these commands in the server/local console path, including RCON.
CON_COMMAND( agpb_agent_chat_events, "Read pending chat events for the agent harness." )
{
	if ( args.ArgC() == 1 && gpGlobals != NULL && AgPB_AgentChatOverrideEnabled() )
		for ( int i = 0; i < AgPB_Bots().Count(); ++i ) AgPB_Bots().Get( i )->ReportPendingChat( gpGlobals->curtime );
	META_CONPRINTF( "[AgPB] chat-events end\n" );
}

CON_COMMAND( agpb_agent_chat_reply, "Resolve a pending chat event with a hex UTF-8 response." )
{
	std::uint64_t id = 0, map, round, user, bot;
	char text[121];
	const char *reason = "syntax";
	if ( args.ArgC() > 1 ) ReadUnsigned( args.Arg( 1 ), id );
	if ( args.ArgC() == 7 && ReadUnsigned( args.Arg( 1 ), id ) && id != 0 &&
	     ReadUnsigned( args.Arg( 2 ), map ) && map > 0 && map <= UINT_MAX &&
	     ReadUnsigned( args.Arg( 3 ), round ) && round <= UINT_MAX &&
	     ReadUnsigned( args.Arg( 4 ), user ) && user <= INT_MAX &&
	     ReadUnsigned( args.Arg( 5 ), bot ) && bot > 0 && bot <= INT_MAX && DecodeReply( args.Arg( 6 ), text ) )
	{
		reason = "disabled";
		if ( gpGlobals != NULL && AgPB_AgentChatOverrideEnabled() && CanCommunicate( AgPB_GameMode() ) )
		{
			reason = "unknown";
			for ( int i = 0; i < AgPB_Bots().Count(); ++i )
			{
				reason = AgPB_Bots().Get( i )->AcceptChatReply( id, static_cast<unsigned int>( map ),
					static_cast<unsigned int>( round ), static_cast<int>( user ), static_cast<int>( bot ), text, gpGlobals->curtime );
				if ( V_strcmp( reason, "unknown" ) != 0 ) break;
			}
		}
	}
	META_CONPRINTF( "[AgPB] chat-reply id=%llu accepted=%d reason=%s\n",
		(unsigned long long)id, V_strcmp( reason, "ok" ) == 0 ? 1 : 0, reason );
}

void CAgPB::SendRadio( AgPBRadio call, float now )
{
	const AgPBGameMode mode = AgPB_GameMode();
	if ( !CanUseRadio( mode ) || !agpb_radio.GetBool() || !m_bAutonomous || !IsAlive() ||
	     AgPB_IsRoundFrozen() || gpGlobals == NULL || now < m_flNextRadio ) return;
	const bool infection = IsInfectionRadioMode( mode );
	const int team = m_pInfo->GetTeamIndex();
	const AgPBZRRole role = infection ? AgPB_ZRRoleForUserId( m_pInfo->GetUserID() ) : AgPBZRRole::Unknown;
	if ( infection && role == AgPBZRRole::Unknown ) return;
	const int audience = infection ? ( role == AgPBZRRole::Human ? 2 : 3 ) : team - AgPB_TEAM_T;
	RadioCooldown &cooldown = g_RadioCooldown[audience];
	const int slot = static_cast<int>( call );
	if ( slot <= 0 || slot >= ARRAYSIZE( cooldown.repeat ) || now < cooldown.next || now < cooldown.repeat[slot] ) return;
	m_flNextRadio = now + ( infection ? 4.0f : 2.0f );
	cooldown.next = now + ( infection ? 2.0f : 1.0f );
	cooldown.repeat[slot] = now + ( infection ? 7.0f : 2.5f );

	// Keep AgPB's local order handling independent of the engine radio quota.
	if ( !infection && call == AgPBRadio::NeedBackup )
		for ( int i = 0; i < AgPB_Bots().Count(); ++i )
			AgPB_Bots().Get( i )->QueueRadioOrder( call, m_pEdict, now );

#if !defined( AGPB_GAME_CSGO )
	const char *command = NativeRadioCommand( call );
	if ( command == NULL || m_Ctx.pHelpers == NULL || g_NativeRadioDispatch != NULL ) return;
	CommunicationRecipients recipients;
	NativeRadioDispatch dispatch = { m_pEdict, NULL, -1, -1, false };
	if ( infection )
	{
		if ( !g_AgPBNativeRadioAudienceReady || m_Ctx.pEngine == NULL ) return;
		dispatch.radioText = g_SMAPI->FindUserMessage( "RadioText", NULL );
		dispatch.sendAudio = g_SMAPI->FindUserMessage( "SendAudio", NULL );
		if ( dispatch.radioText < 0 || dispatch.sendAudio < 0 ) return;
		for ( int i = 1; i <= gpGlobals->maxClients && i < ARRAYSIZE( g_RadioIgnoredBy ); ++i )
		{
			edict_t *client = m_Ctx.pEngine->PEntityOfEntIndex( i );
			IPlayerInfo *info = ClientInfo( client );
			if ( HasZRRole( client, info, role ) && !info->IsFakeClient() &&
			     g_RadioIgnoredBy[i] != info->GetUserID() ) recipients.slots.AddToTail( i );
		}
		if ( recipients.GetRecipientCount() == 0 ) return;
		dispatch.recipients = &recipients;
		dispatch.iconPending = true;
	}
	if ( !NativeRadioSafe() )
	{
		static bool warned = false;
		if ( !warned ) META_CONPRINTF( "[AgPB] native radio unavailable: enable the csczs Radio Guard and check its status\n" );
		warned = true;
		return;
	}
	// The public helper executes on the server, including for fake clients.
	g_NativeRadioDispatch = &dispatch;
	m_Ctx.pHelpers->ClientCommand( m_pEdict, command );
	g_NativeRadioDispatch = NULL;
#endif
}

bool CAgPB::FollowsRadioIssuer( uintp handle ) const
{
	return ( IsFollow( m_PendingRadio.call ) && m_PendingRadio.issuer.handle == handle ) ||
		( IsFollow( m_RadioOrder.call ) && m_RadioOrder.issuer.handle == handle &&
		  m_Tasks.State( m_RadioOrder.task ) != AgPBTaskState::Missing );
}

bool CAgPB::QueueRadioOrder( AgPBRadio call, edict_t *issuer, float now )
{
	const AgPBGameMode mode = AgPB_GameMode();
	if ( issuer == m_pEdict || !CanUseRadio( mode ) || !agpb_radio.GetBool() || !m_bAutonomous || !IsAlive() ) return false;
	if ( call == AgPBRadio::ReportTeam && now < m_flNextRadio ) return false;
	IPlayerInfo *source = ClientInfo( issuer );
	if ( !LivingPlayer( issuer, source ) ) return false;
	const bool infection = IsInfectionRadioMode( mode );
	const AgPBZRRole role = infection ? AgPB_ZRRoleForUserId( m_pInfo->GetUserID() ) : AgPBZRRole::Unknown;
	if ( infection ? call != AgPBRadio::ReportTeam || !HasZRRole( issuer, source, role )
	        : source->GetTeamIndex() != m_pInfo->GetTeamIndex() ) return false;
	const uintp handle = AgPB_RefEHandle( issuer );
	if ( !NetVar_HandleIsValid( handle ) ) return false;
	if ( call != AgPBRadio::ReportTeam )
	{
		if ( m_AgentIntent.token != 0 || m_Tasks.Current().desire >= AgPBTaskPriority::Objective ) return false;
		if ( call == AgPBRadio::HoldPosition && !FollowsRadioIssuer( handle ) ) return false;
		const float range = ( call == AgPBRadio::NeedBackup || call == AgPBRadio::TakingFire ||
			call == AgPBRadio::Go || call == AgPBRadio::GetOut ) ? 2048.0f : 1024.0f;
		const Vector origin = source->GetAbsOrigin();
		const bool close = ( origin - m_pInfo->GetAbsOrigin() ).LengthSqr() <= range * range;
		CBotCmd view;
		view.Reset();
		if ( ( IsFollow( call ) || !close ) &&
		     !TaskAimAt( issuer, origin + Vector( 0.0f, 0.0f, 36.0f ), view ) ) return false;
	}
	m_PendingRadio = {};
	m_PendingRadio.call = call;
	m_PendingRadio.issuer = { mode, role, handle, source->GetUserID(), source->GetTeamIndex() };
	m_PendingRadio.expiresAt = now + 2.0f;
	return true;
}

IPlayerInfo *CAgPB::RadioIssuerInfo( edict_t *issuer, const AgPBRadioIssuer &context ) const
{
	if ( context.mode != AgPB_GameMode() || !CanUseRadio( context.mode ) || !agpb_radio.GetBool() ||
	     !m_bAutonomous || !IsAlive() || AgPB_IsRoundFrozen() ||
	     ( GetNetVarInt( "m_fFlags" ) & FL_FROZEN ) != 0 ) return NULL;
	IPlayerInfo *source = ClientInfo( issuer );
	if ( !LivingPlayer( issuer, source ) || source->GetUserID() != context.userId ) return NULL;
	if ( IsInfectionRadioMode( context.mode ) )
		return HasZRRole( m_pEdict, m_pInfo, context.role ) && HasZRRole( issuer, source, context.role ) ? source : NULL;
	return source->GetTeamIndex() == context.team && m_pInfo->GetTeamIndex() == context.team ? source : NULL;
}

AgPBRadio CAgPB::RadioStatus() const
{
	const bool holding = IsInfectionRadioMode( AgPB_GameMode() ) ? IsZMHoldingPosition() : IsHoldingTaskPosition();
	return NetVar_HandleIsValid( m_uCombatEnemy ) ? AgPBRadio::EnemySpotted :
		holding ? AgPBRadio::InPosition : AgPBRadio::SectorClear;
}

void CAgPB::RespondToRadio( AgPBRadio call, const AgPBRadioIssuer &issuer, IPlayerInfo &source, float now )
{
	if ( source.IsFakeClient() )
	{
		SendRadio( call == AgPBRadio::ReportTeam ? RadioStatus() : call, now );
		return;
	}
	// Give the human's call time to finish before speaking over the radio.
	m_RadioReply = { call, issuer, now + RandomFloat( 2.0f, 3.0f ) };
}

void CAgPB::UpdateCommunication( CGlobalVars *globals )
{
	const float now = globals->curtime;
	if ( !m_Chat.joined && IsAlive() && CanCommunicate( AgPB_GameMode() ) && agpb_chat.GetBool() )
		m_Chat.joined = QueueChat( AgPBChat::Join, NULL, false, now, "bot joined" );
	if ( m_Chat.id != 0 )
	{
		const bool external = AgPB_AgentChatOverrideEnabled() && m_Chat.externalUntil > 0.0f;
		const bool externalReply = external && m_Chat.externallyResolved;
		if ( now >= m_Chat.expiresAt || !ChatScopeValid() ) m_Chat.id = 0;
		else if ( now >= m_Chat.readyAt && now >= g_NextChat &&
		          ( !external || externalReply || now >= m_Chat.externalUntil ) )
		{
			const char *reply = "";
			if ( m_Chat.cue != AgPBChat::Idle || HasLivingChatListener() )
				reply = externalReply ? m_Chat.output : LocalChat( m_Chat );
			m_Chat.id = 0;
			if ( reply[0] && ( m_Chat.cue != AgPBChat::Death || !IsAlive() ) )
			{
#if !defined( AGPB_GAME_CSGO )
				if ( m_Chat.mode == AgPB_MODE_ZM && m_Chat.teamOnly )
					SendZMTeamChat( m_iIndex, m_Chat.team, m_Chat.role, Name(), reply );
				else
#endif
				{
					char command[136];
					V_snprintf( command, sizeof( command ), "%s \"%s\"", m_Chat.teamOnly ? "say_team" : "say", reply );
					m_Ctx.pHelpers->ClientCommand( m_pEdict, command );
				}
				m_Chat.nextAt = now + RandomFloat( 6.0f, 10.0f );
				g_NextChat = now + 3.0f;
			}
		}
	}
	if ( !agpb_radio.GetBool() && m_RadioOrder.task != 0 )
	{
		m_Tasks.RemoveToken( m_RadioOrder.task );
		m_RadioOrder = {};
	}
	if ( m_RadioReply.call != AgPBRadio::None )
	{
		const AgPBRadioReply &reply = m_RadioReply;
		if ( RadioIssuerInfo( HandleToEdict( reply.issuer.handle ), reply.issuer ) == NULL )
			m_RadioReply = {};
		else if ( now >= reply.readyAt )
		{
			const AgPBRadio call = reply.call;
			m_RadioReply = {};
			SendRadio( call == AgPBRadio::ReportTeam ? RadioStatus() : call, now );
		}
	}
	if ( m_PendingRadio.call == AgPBRadio::None ) return;
	AgPBRadioOrder order = m_PendingRadio;
	m_PendingRadio = {};
	if ( now >= order.expiresAt ) return;
	edict_t *issuer = HandleToEdict( order.issuer.handle );
	IPlayerInfo *source = RadioIssuerInfo( issuer, order.issuer );
	if ( source == NULL ) return;
	const AgPBRadio call = order.call;
	if ( call == AgPBRadio::ReportTeam )
	{
		RespondToRadio( call, order.issuer, *source, now );
		return;
	}
	if ( IsInfectionRadioMode( order.issuer.mode ) ) return;
	RefreshTaskPlan( globals );
	const AgPBTask current = m_Tasks.Current();
	if ( m_AgentIntent.token != 0 || current.desire >= AgPBTaskPriority::Objective ||
	     NetVar_HandleIsValid( m_uCombatEnemy ) || m_Ladder.Active() ) return;
	if ( call == AgPBRadio::GetOut || call == AgPBRadio::Regroup )
	{
		const int team = m_pInfo->GetTeamIndex();
		if ( ( ( call == AgPBRadio::GetOut && team == AgPB_TEAM_T ) ||
		       ( call == AgPBRadio::Regroup && team == AgPB_TEAM_CT ) ) && PlanPlantedBombTask( globals, true ) )
			RespondToRadio( AgPBRadio::Affirmative, order.issuer, *source, now );
		return;
	}
	if ( call == AgPBRadio::Go )
	{
		const bool following = FollowsRadioIssuer( order.issuer.handle );
		const bool holding = current.id == AgPBTaskId::Camp || current.id == AgPBTaskId::Pause ||
			( current.id == AgPBTaskId::RadioOrder && m_RadioOrder.holding && m_RadioOrder.issuer.handle == order.issuer.handle );
		if ( following || holding )
		{
			m_Tasks.RemoveToken( following ? m_RadioOrder.task : current.token );
			m_Autonomy.fear = MAX( 0.0f, m_Autonomy.fear - 0.3f );
			RespondToRadio( AgPBRadio::Affirmative, order.issuer, *source, now );
		}
		return;
	}
	AgPBTask task;
	task.id = AgPBTaskId::RadioOrder;
	task.desire = AgPBTaskPriority::RadioOrder;
	task.deadline = now + ( IsFollow( call ) ? 30.0f : 15.0f );
	order.holding = call == AgPBRadio::HoldPosition;
	Vector target = source->GetAbsOrigin();
	if ( call == AgPBRadio::StormFront )
	{
		Vector forward;
		const float yaw = AgPB_ReadNetVarFloat( issuer, "m_angEyeAngles[1]", source->GetAbsAngles().y );
		AngleVectors( QAngle( 0.0f, yaw, 0.0f ), &forward );
		target += forward * 1024.0f;
	}
	if ( !order.holding )
	{
		task.waypoint = PickTaskWaypoint( target, call == AgPBRadio::GetInPosition ? AgPB_WP_CAMP : 0 );
		const float near = ( target - m_pInfo->GetAbsOrigin() ).LengthSqr();
		if ( task.waypoint < 0 && ( !IsFollow( call ) || near > 128.0f * 128.0f ) )
		{
			RespondToRadio( AgPBRadio::Negative, order.issuer, *source, now );
			return;
		}
	}
	order.task = m_Tasks.Push( task );
	if ( order.task == 0 ) return;
	m_RadioOrder = order;
	if ( call == AgPBRadio::FallBack )
	{
		m_Autonomy.fear = MIN( 1.0f, m_Autonomy.fear + 0.5f );
		m_Autonomy.aggression = MAX( 0.0f, m_Autonomy.aggression - 0.5f );
	}
	else if ( call == AgPBRadio::StormFront || call == AgPBRadio::NeedBackup || call == AgPBRadio::TakingFire )
		m_Autonomy.fear = MAX( 0.0f, m_Autonomy.fear - 0.2f );
	if ( call != AgPBRadio::TakingFire && ( call != AgPBRadio::NeedBackup || !source->IsFakeClient() ) )
		RespondToRadio( AgPBRadio::Affirmative, order.issuer, *source, now );
}

void CAgPB::ExecuteRadioOrder( CGlobalVars *globals, CBotCmd &cmd )
{
	const AgPBTask task = m_Tasks.Current();
	edict_t *issuer = HandleToEdict( m_RadioOrder.issuer.handle );
	IPlayerInfo *source = ClientInfo( issuer );
	if ( task.token != m_RadioOrder.task || !LivingPlayer( issuer, source ) ||
	     source->GetUserID() != m_RadioOrder.issuer.userId || source->GetTeamIndex() != m_RadioOrder.issuer.team )
	{
		CompleteCurrentTask( "radio issuer unavailable" );
		return;
	}
	const bool follow = IsFollow( m_RadioOrder.call );
	const Vector target = source->GetAbsOrigin();
	const Vector here = m_pInfo->GetAbsOrigin();
	const Vector delta = target - here;
	const float horizontal = delta.Length2D();
	if ( follow ) m_RadioOrder.holding = horizontal <= 120.0f && fabsf( delta.z ) <= 54.0f;
	if ( m_RadioOrder.holding )
	{
		if ( HasRoute() ) ClearRoute();
		UpdateIdleLook( globals, cmd );
		return;
	}
	int goal = task.waypoint;
	if ( follow && globals->curtime >= m_RadioOrder.replanAt )
	{
		m_RadioOrder.replanAt = globals->curtime + 0.7f;
		if ( HasRoute() && ( m_vRadioPathTarget - target ).LengthSqr() > 176.0f * 176.0f ) ClearRoute();
		if ( !HasRoute() ) goal = PickTaskWaypoint( target );
	}
	if ( !HasRoute() )
	{
		if ( follow )
		{
			// The leader is a live moving entity. Stop short of their hull,
			// and query support for each short final approach.
			if ( horizontal > 120.0f && horizontal <= 256.0f && fabsf( delta.z ) < 18.0f )
			{
				const Vector leg = here + Vector( delta.x, delta.y, 0.0f ) * ( 1.0f - 112.0f / horizontal );
				if ( CanWalkToTaskPosition( leg, false ) )
				{
					QAngle view;
					VectorAngles( delta, view );
					cmd.viewangles.y = view.y;
					cmd.forwardmove = clamp( ( horizontal - 112.0f ) * 4.0f, 100.0f, 250.0f );
					return;
				}
			}
			if ( m_bRouteArrived && goal == m_RadioOrder.lastGoal ) return;
		}
		char error[192];
		if ( goal < 0 || !BuildRoute( goal, error, sizeof( error ) ) )
		{
			CompleteCurrentTask( goal < 0 ? "radio target unreachable" : error, true );
			return;
		}
		m_RadioOrder.lastGoal = goal;
		m_vRadioPathTarget = target;
		m_uRouteTaskToken = task.token;
	}
	UpdateRoute( globals, cmd );
	if ( HasRoute() || m_Tasks.Current().token != task.token ) return;
	if ( m_bRouteArrived && m_RadioOrder.call == AgPBRadio::GetInPosition )
	{
		m_RadioOrder.holding = true;
		SendRadio( AgPBRadio::InPosition, globals->curtime );
	}
	else if ( !m_bRouteArrived || !follow )
		CompleteCurrentTask( m_bRouteArrived ? "radio destination reached" : "radio route failed", !m_bRouteArrived );
}
