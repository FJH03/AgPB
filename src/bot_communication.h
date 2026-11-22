#ifndef _INCLUDE_AGPB_BOT_COMMUNICATION_H_
#define _INCLUDE_AGPB_BOT_COMMUNICATION_H_

#include <cstdint>
#include "game_mode.h"

class IGameEvent;
class IRecipientFilter;
struct edict_t;

enum class AgPBChat { None, Reply, Join, Kill, Death, Plant, Idle };

// Source player_radio event slots. The three menu separators are not events.
enum class AgPBRadio
{
	None = 0, CoverMe = 2, TakePoint, HoldPosition, Regroup, FollowMe, TakingFire,
	Go = 9, FallBack, StickTogether, GetInPosition, StormFront, ReportTeam,
	Affirmative = 16, EnemySpotted, NeedBackup, SectorClear, InPosition, ReportingIn,
	GetOut, Negative, EnemyDown
};

struct AgPBChatState
{
	std::uint64_t id = 0;
	std::uintptr_t speaker = 0;
	unsigned int map = 0, round = 0;
	int userId = 0, botUserId = 0, speakerTeam = 0, team = 0;
	float readyAt = 0.0f, externalUntil = 0.0f, expiresAt = 0.0f, nextAt = 0.0f;
	AgPBChat cue = AgPBChat::None;
	AgPBGameMode mode = AgPB_MODE_NORMAL;
	AgPBZRRole role = AgPBZRRole::Unknown, speakerRole = AgPBZRRole::Unknown;
	bool teamOnly = false, alive = false, joined = false, externallyResolved = false;
	char input[128] = {};
	char output[121] = {};
};

struct AgPBRadioIssuer
{
	AgPBGameMode mode = AgPB_MODE_NORMAL;
	AgPBZRRole role = AgPBZRRole::Unknown;
	std::uintptr_t handle = 0;
	int userId = 0, team = 0;
};

struct AgPBRadioOrder
{
	AgPBRadio call = AgPBRadio::None;
	AgPBRadioIssuer issuer;
	int lastGoal = -1;
	std::uint64_t task = 0;
	float expiresAt = 0.0f, replanAt = 0.0f;
	bool holding = false;
};

struct AgPBRadioReply
{
	AgPBRadio call = AgPBRadio::None;
	AgPBRadioIssuer issuer;
	float readyAt = 0.0f;
};

void AgPB_OnCommunicationEvent( IGameEvent *event );
void AgPB_BeginChatDispatch( bool teamOnly );
void AgPB_EndChatDispatch();
void AgPB_ToggleIgnoreRadio( edict_t *client );
void AgPB_ResetCommunications();
void AgPB_UpdateIdleChat( float now );
bool AgPB_AgentChatOverrideEnabled();

#if !defined( AGPB_GAME_CSGO )
extern bool g_AgPBNativeRadioAudienceReady;
IRecipientFilter *AgPB_NativeRadioMessageFilter( int message );
IRecipientFilter *AgPB_NativeRadioIconFilter();
#endif

#endif
