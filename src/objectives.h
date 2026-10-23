#ifndef _INCLUDE_AGPB_OBJECTIVES_H_
#define _INCLUDE_AGPB_OBJECTIVES_H_

#include <basetypes.h>
#include <mathlib/vector.h>
#include <utlvector.h>

struct BotEngineContext;
class CGlobalVars;

// SyPB's normal-mode objective roles are the baseline here. CS-EBOT was
// checked for matching entity categories, but its zombie-only paths are not
// used. This observer reads the Source SDK's public interfaces and SendTables.
enum AgPBObjectiveMapFlags
{
	AGPB_MAP_NO_OBJECTIVE = 0,
	AGPB_MAP_BOMB = 1 << 0,
	AGPB_MAP_HOSTAGE = 1 << 1
};

enum AgPBBombState
{
	AGPB_BOMB_UNKNOWN = 0, // A bomb map with no conclusive entity state.
	AGPB_BOMB_NONE,        // No weapon_c4 or planted_c4 was observed.
	AGPB_BOMB_CARRIED,
	AGPB_BOMB_DROPPED,
	AGPB_BOMB_PLANTED,
	AGPB_BOMB_RESOLVED    // A planted C4 is present and its timer is stopped.
};

enum AgPBBombRadiusSource
{
	AGPB_BOMB_RADIUS_UNKNOWN = 0,
	AGPB_BOMB_RADIUS_MAP_DEFAULT,
	AGPB_BOMB_RADIUS_MAP_ENTITY,
	AGPB_BOMB_RADIUS_OVERRIDE
};
const char *AgPB_BombRadiusSourceName( AgPBBombRadiusSource source );

struct AgPBObjectiveRegion
{
	AgPBObjectiveRegion();

	uintp handle;
	Vector center;
	Vector worldMins;
	Vector worldMaxs;
	bool positionKnown;
	bool boundsKnown;
	bool pointEntity; // info_* point, as opposed to a func_* trigger volume.
};

struct AgPBObjectiveHostage
{
	AgPBObjectiveHostage();

	uintp handle;
	Vector position;
	bool positionKnown;
	bool aliveKnown;
	bool alive;
	bool rescuedKnown;
	bool rescued;
	bool leaderKnown;
	uintp leader; // INVALID_EHANDLE_INDEX means no leader when leaderKnown.
};

struct AgPBObjectiveBomb
{
	AgPBObjectiveBomb();

	AgPBBombState state;
	uintp handle; // weapon_c4 or planted_c4, never a retained edict pointer.
	uintp owner;  // Player handle when carried; otherwise INVALID_EHANDLE_INDEX.
	bool ownerKnown;
	Vector position;
	bool positionKnown;
	bool tickingKnown;
	bool ticking;
	bool blowTimeKnown;
	float blowTime;
	bool defuseCountdownKnown;
	float defuseCountdown;
	bool plantedObserved; // Retained during missing/unknown observations until round reset.
};

struct AgPBObjectiveSnapshot
{
	AgPBObjectiveSnapshot();

	int mapFlags;
	float observedAt;
	unsigned int roundEpoch;
	AgPBObjectiveBomb bomb;
	bool bombBlastRadiusKnown;
	float bombBlastRadius;
	AgPBBombRadiusSource bombRadiusSource;
	CUtlVector<AgPBObjectiveRegion> bombSites;
	CUtlVector<AgPBObjectiveRegion> rescueZones;
	CUtlVector<AgPBObjectiveHostage> hostages;
};

class AgPBObjectiveWorld
{
public:
	AgPBObjectiveWorld();

	// Clears all map and round observations. Call on level shutdown or map load.
	void Reset();
	// Invalidates per-round observations immediately; static map type persists.
	void OnRoundStart();
	// Scans valid edicts at most once per 0.1 seconds; no entity pointers persist.
	void Refresh( const BotEngineContext &ctx, const CGlobalVars *globals );
	const AgPBObjectiveSnapshot &Snapshot() const { return m_Snapshot; }

private:
	AgPBObjectiveSnapshot m_Snapshot;
	int m_MapFlags;
	unsigned int m_RoundEpoch;
	float m_NextRefresh;
	bool m_BombRadiusParsed;
	bool m_MapBombRadiusKnown;
	float m_MapBombBlastRadius;
	AgPBBombRadiusSource m_MapBombRadiusSource;
};

AgPBObjectiveWorld &BotObjectives();

#endif
