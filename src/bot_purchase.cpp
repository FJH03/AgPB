/*
 * AgPB native purchases for ordinary mode and ZM humans. GPL-3.0-only.
 * Based on SyPB 4c364fb basecode.cpp (PerformWeaponPurchase) and control.cpp
 * (CheckTeamEconomics). The upstream copyright and license are retained in
 * weapon_selection.h. CS-EBOT 2333562 omits ordinary-mode buying.
 */

#include <cmath>
#include <icvar.h>
#include <tier1/convar.h>
#include <tier1/strtools.h>
#include <vstdlib/random.h>

#include "bot.h"
#include "game_mode.h"
#include "objectives.h"
#include "purchase_policy.h"

extern ICvar *icvar;
extern CGlobalVars *gpGlobals;

namespace {
using AgPBWeaponSelect::Weapon;
using Stage = AgPBPurchaseStage;

struct BuyWeapon
{
	Weapon kind;
	const char *alias;
	int price;
	int team;
	int buffer;
	const char *ammoLimit;
};

// Default script prices and allowed teams. The buffer leaves cash for
// supplies after buying a weapon, following SyPB's BuyWeaponMode.
const BuyWeapon weapons[] =
{
	{ Weapon::USP,       "usp",        500, 0,             120, "ammo_45acp_max" },
	{ Weapon::Glock,     "glock",      400, 0,             120, "ammo_9mm_max" },
	{ Weapon::Deagle,    "deagle",     650, 0,             120, "ammo_50AE_max" },
	{ Weapon::P228,      "p228",      600, 0,             120, "ammo_357sig_max" },
	{ Weapon::Elite,     "elite",      800, AgPB_TEAM_T,   120, "ammo_9mm_max" },
	{ Weapon::FiveSeven, "fiveseven",  750, AgPB_TEAM_CT,  120, "ammo_57mm_max" },
	{ Weapon::M3,        "m3",        1700, 0,             500, "ammo_buckshot_max" },
	{ Weapon::XM1014,    "xm1014",    3000, 0,             400, "ammo_buckshot_max" },
	{ Weapon::MP5,       "mp5navy",   1500, 0,             300, "ammo_9mm_max" },
	{ Weapon::TMP,       "tmp",       1250, AgPB_TEAM_CT,  500, "ammo_9mm_max" },
	{ Weapon::P90,       "p90",       2350, 0,             500, "ammo_57mm_max" },
	{ Weapon::MAC10,     "mac10",     1400, AgPB_TEAM_T,   500, "ammo_45acp_max" },
	{ Weapon::UMP45,     "ump45",     1700, 0,             500, "ammo_45acp_max" },
	{ Weapon::AK47,      "ak47",      2500, AgPB_TEAM_T,   100, "ammo_762mm_max" },
	{ Weapon::SG552,     "sg552",     3500, AgPB_TEAM_T,   400, "ammo_556mm_max" },
	{ Weapon::M4A1,      "m4a1",      3100, AgPB_TEAM_CT,  100, "ammo_556mm_max" },
	{ Weapon::Galil,     "galil",     2000, AgPB_TEAM_T,   400, "ammo_556mm_max" },
	{ Weapon::Famas,     "famas",     2250, AgPB_TEAM_CT,  300, "ammo_556mm_max" },
	{ Weapon::AUG,       "aug",       3500, AgPB_TEAM_CT,  400, "ammo_556mm_max" },
	{ Weapon::Scout,     "scout",     2750, 0,             500, "ammo_762mm_max" },
	{ Weapon::AWP,       "awp",       4750, 0,             200, "ammo_338mag_max" },
	{ Weapon::G3SG1,     "g3sg1",     5000, AgPB_TEAM_T,   400, "ammo_762mm_max" },
	{ Weapon::SG550,     "sg550",     4200, AgPB_TEAM_CT,  400, "ammo_556mm_max" },
	{ Weapon::M249,      "m249",      5750, 0,             500, "ammo_556mm_box_max" }
};

struct BuyGrenade
{
	const char *alias;
	const char *classname;
	int price;
};

const BuyGrenade grenades[] =
{
	{ "hegrenade", "weapon_hegrenade", 300 },
	{ "flashbang", "weapon_flashbang", 200 },
	{ "smokegrenade", "weapon_smokegrenade", 300 }
};

constexpr int KevlarPrice = 650;
constexpr int HelmetPrice = 350;
constexpr int VestHelmetPrice = 1000;
constexpr int DefuserPrice = 200;

const BuyWeapon *WeaponForPurchase( Weapon weapon )
{
	for ( const BuyWeapon &item : weapons )
		if ( item.kind == weapon ) return &item;
	return NULL;
}

const BuyWeapon *ChooseWeapon( AgPBLocalPersonality personality, int team, int money, bool pistol,
                              Weapon excluded, bool allowStartingPistol )
{
	const auto &order = AgPBWeaponSelect::PersonalityOrder( personality );
	const BuyWeapon *preferred = NULL;
	for ( int i = AgPBWeaponSelect::WeaponCount - 1; i >= 0; --i )
	{
		const Weapon weapon = order[i];
		if ( weapon == excluded || AgPBWeaponSelect::Pistol( weapon ) != pistol ||
		     ( !allowStartingPistol && ( weapon == Weapon::Glock || weapon == Weapon::USP ) ) )
			continue;
		const BuyWeapon *item = WeaponForPurchase( weapon );
		if ( item == NULL || item == preferred || ( item->team != 0 && item->team != team ) ||
		     money < item->price + item->buffer )
			continue;
		if ( preferred == NULL ) preferred = item;
		else return !pistol && excluded == Weapon::Unknown && RandomInt( 1, 7 ) <= 3 ? item : preferred;
	}
	return preferred;
}

// Capture both teams once before the first transaction changes any balance.
bool TeamCanBuy( const AgPBPurchaseRound &round, int team )
{
	static unsigned int epoch = 0;
	static bool good[2] = { true, true };
	if ( epoch != round.epoch )
	{
		int known[2] = {};
		int poor[2] = {};
		CAgPBManager &bots = AgPB_Bots();
		for ( int i = 0; i < bots.Count(); ++i )
		{
			CAgPB *bot = bots.Get( i );
			IPlayerInfo *info = bot != NULL ? bot->PlayerInfo() : NULL;
			if ( info == NULL || !info->IsConnected() ) continue;
			const int onTeam = info->GetTeamIndex();
			if ( onTeam != AgPB_TEAM_T && onTeam != AgPB_TEAM_CT ) continue;
			const int account = bot->GetNetVarInt( "m_iAccount", -1 );
			if ( account < 0 ) continue;
			const int slot = onTeam == AgPB_TEAM_CT ? 1 : 0;
			++known[slot];
			if ( account <= 1500 ) ++poor[slot];
		}
		for ( int slot = 0; slot < 2; ++slot )
		{
			const int onTeam = slot == 0 ? AgPB_TEAM_T : AgPB_TEAM_CT;
			good[slot] = known[slot] <= 1 || poor[slot] * 5 < known[slot] * 4 || round.lastWinner == onTeam;
		}
		epoch = round.epoch;
	}
	return good[team == AgPB_TEAM_CT ? 1 : 0];
}

ConVar *EngineCvar( const char *name )
{
	return icvar != NULL ? icvar->FindVar( name ) : NULL;
}

bool BuyTimeOpen( const AgPBPurchaseRound &round, float now )
{
	const ConVar *buyTime = EngineCvar( "mp_buytime" );
	if ( buyTime == NULL ) return false;
	const float seconds = buyTime->GetFloat() * 60.0f;
	return std::isfinite( seconds ) && seconds >= 0.0f && ( round.freeze || now - round.liveAt <= seconds );
}

bool InBuyZone( const CAgPB &bot )
{
	const ConVar *anywhere = EngineCvar( "mp_buy_anywhere" );
	const int allowed = anywhere != NULL ? anywhere->GetInt() : 0;
	return allowed == 1 || ( allowed > 1 && bot.PlayerInfo() != NULL && allowed == bot.PlayerInfo()->GetTeamIndex() ) ||
		bot.GetNetVarInt( "m_bInBuyZone", 0 ) == 1;
}

int ReserveAmmo( const CAgPB &bot, edict_t *weapon )
{
	if ( weapon == NULL ) return -1;
	const int index = AgPB_ReadNetVarInt( weapon, "m_iPrimaryAmmoType", -1 );
	return index >= 0 ? bot.GetNetVarArrayInt( "m_iAmmo", index, -1 ) : -1;
}

// Use the scalar SendProp proxy for CCSPlayer's adjacent one-byte bools.
const char *CurrentGate( const CAgPB &bot, AgPBGameMode mode, const AgPBPurchaseRound &round, float now )
{
	if ( !bot.IsAutonomous() || ( mode != AgPB_MODE_NORMAL && mode != AgPB_MODE_ZM ) ) return "inactive";
	if ( !bot.IsAlive() ) return "dead";
	if ( mode == AgPB_MODE_ZM )
	{
		IPlayerInfo *info = bot.PlayerInfo();
		const AgPBZRRole role = info != NULL ? AgPB_ZRRoleForUserId( info->GetUserID() ) : AgPBZRRole::Unknown;
		if ( role != AgPBZRRole::Human ) return role == AgPBZRRole::Zombie ? "zr-zombie" : "waiting-zr-role";
	}
	if ( round.epoch == 0 ) return "waiting-round";
	if ( !round.active ) return "round-ended";
	if ( !BuyTimeOpen( round, now ) ) return "buy-time-closed";
	if ( !InBuyZone( bot ) ) return "outside-buy-zone";
	if ( bot.GetNetVarInt( "m_bIsVIP", 0 ) == 1 ) return "vip";
	return NULL;
}

const char *StageName( Stage stage )
{
	switch ( stage )
	{
	case Stage::Primary: return "primary";
	case Stage::Armor: return "armor";
	case Stage::Pistol: return "pistol";
	case Stage::HE: return "he";
	case Stage::Flash: return "flash";
	case Stage::Smoke: return "smoke";
	case Stage::Defuser: return "defuser";
	case Stage::PrimaryAmmo: return "primary-ammo";
	case Stage::PistolAmmo: return "pistol-ammo";
	case Stage::Complete: return "complete";
	}
	return "unknown";
}
}

void CAgPB::UpdatePurchases( CGlobalVars *globals )
{
	if ( globals == NULL || m_pInfo == NULL ) return;
	const AgPBPurchaseRound &round = AgPB_PurchaseRound();
	AgPBPurchaseState &state = m_Purchases;
	const int team = m_pInfo->GetTeamIndex();
	const float now = globals->curtime;
	if ( team != AgPB_TEAM_T && team != AgPB_TEAM_CT ) return;
	if ( state.epoch != round.epoch || state.team != team )
	{
		state = AgPBPurchaseState();
		state.epoch = round.epoch;
		state.team = team;
	}
	if ( state.stage == Stage::Complete || now < state.nextAt ) return;

	// Poll for entry and engine state at a lower rate than the movement tick.
	state.nextAt = now + 0.15f;
	const AgPBGameMode mode = AgPB_GameMode();
	const char *gate = CurrentGate( *this, mode, round, now );
	if ( gate != NULL )
	{
		state.status = gate;
		return;
	}
	if ( !round.freeze && ( m_Tasks.Current().id == AgPBTaskId::PickupWeapon || NetVar_HandleIsValid( m_uCombatEnemy ) ||
	     m_Tasks.Current().desire >= AgPBTaskPriority::Objective ) )
	{
		state.status = "action-priority";
		return;
	}
	if ( m_Ctx.pHelpers == NULL )
	{
		state.status = "client-commands-unavailable";
		return;
	}
	const int cash = GetNetVarInt( "m_iAccount", -1 );
	const BotNetVar *inventory = FindNetVar( "m_hMyWeapons" );
	void *base = NetVarBase();
	if ( cash < 0 || base == NULL || inventory == NULL || inventory->type != DPT_Array ||
	     inventory->elementType != DPT_Int || inventory->stride != sizeof( uintp ) )
	{
		state.status = "equipment-state-unavailable";
		return;
	}

	// Infection roles are independent of the original T/CT team economy.
	state.teamCanBuy = mode == AgPB_MODE_ZM || TeamCanBuy( round, team );
	state.status = "shopping";
	edict_t *primary = NULL;
	edict_t *pistol = NULL;
	Weapon primaryKind = Weapon::Unknown;
	Weapon pistolKind = Weapon::Unknown;
	for ( int i = 0; i < inventory->elements; ++i )
	{
		edict_t *owned = HandleToEdict( NetVar_GetArrayHandle( base, *inventory, i ) );
		if ( owned == NULL ) continue;
		const Weapon kind = ClassifyWeapon( owned );
		if ( !AgPBWeaponSelect::Firearm( kind ) ) continue;
		if ( AgPBWeaponSelect::Pistol( kind ) )
		{
			pistol = owned;
			pistolKind = kind;
		}
		else
		{
			primary = owned;
			primaryKind = kind;
		}
	}
	const bool fundedLoadout = state.teamCanBuy && primary != NULL;

	while ( state.stage != Stage::Complete )
	{
		const Stage stage = state.stage;
		state.stage = static_cast<Stage>( static_cast<int>( stage ) + 1 );
		const BuyWeapon *gun = NULL;
		const char *alias = NULL;
		const char *classname = NULL;
		edict_t *ammoWeapon = NULL;
		int reserveBefore = -1;
		int armorBefore = -1;
		int helmetBefore = -1;
		bool ammoSingle = false;
		switch ( stage )
		{
		case Stage::Primary:
			if ( primary == NULL && state.teamCanBuy )
				gun = ChooseWeapon( m_Autonomy.personality, team, cash, false, state.rejectedPrimary, false );
			state.plan = primary != NULL ? "keep-primary" : ( gun != NULL ? "buy-primary" : "save" );
			break;
		case Stage::Armor:
			armorBefore = GetNetVarInt( "m_ArmorValue", -1 );
			helmetBefore = GetNetVarInt( "m_bHasHelmet", -1 );
			if ( fundedLoadout && armorBefore >= 0 )
			{
				const bool refresh = armorBefore < RandomInt( 50, 80 );
				if ( helmetBefore == 0 && armorBefore >= 100 && cash >= HelmetPrice )
					alias = "vesthelm";
				else if ( refresh && helmetBefore == 0 && cash >= VestHelmetPrice )
					alias = "vesthelm";
				else if ( refresh && cash >= KevlarPrice )
					alias = "vest";
			}
			break;
		case Stage::Pistol:
		{
			const bool stockPistol = pistolKind == Weapon::USP || pistolKind == Weapon::Glock;
			// Allow an affordable upgrade on the opening pistol round. Once
			// there is a round winner, keep the stock pistol on team saves.
			const bool pistolLoadout = primary == NULL && ( state.teamCanBuy || round.lastWinner == 0 );
			const bool fundedUpgrade = fundedLoadout && cash > RandomInt( 7500, 9000 );
			if ( pistol == NULL || ( stockPistol && ( pistolLoadout || fundedUpgrade ) ) )
				gun = ChooseWeapon( m_Autonomy.personality, team, cash, true, pistolKind, pistol == NULL );
			break;
		}
		case Stage::HE:
		case Stage::Flash:
		case Stage::Smoke:
		{
			const int index = static_cast<int>( stage ) - static_cast<int>( Stage::HE );
			const BuyGrenade &grenade = grenades[index];
			if ( fundedLoadout && cash >= grenade.price && cash >= AgPBWeaponSelect::GrenadeMoney( index ) &&
			     FindOwnedWeapon( grenade.classname ) == NULL && RandomInt( 1, 100 ) <= AgPBWeaponSelect::GrenadeChance( index ) )
			{
				alias = grenade.alias;
				classname = grenade.classname;
			}
			break;
		}
		case Stage::Defuser:
		{
			const AgPBObjectiveSnapshot &objectives = BotObjectives().Snapshot();
			if ( mode == AgPB_MODE_NORMAL && fundedLoadout && team == AgPB_TEAM_CT &&
			     ( objectives.mapFlags & AGPB_MAP_BOMB ) != 0 &&
			     cash >= DefuserPrice && GetNetVarInt( "m_bHasDefuser", -1 ) == 0 && RandomInt( 1, 100 ) <= 80 )
				alias = "defuser";
			break;
		}
		case Stage::PrimaryAmmo:
		case Stage::PistolAmmo:
		{
			const bool primarySlot = stage == Stage::PrimaryAmmo;
			ammoWeapon = primarySlot ? primary : pistol;
			const BuyWeapon *item = WeaponForPurchase( primarySlot ? primaryKind : pistolKind );
			if ( item == NULL || ammoWeapon == NULL ) break;
			reserveBefore = ReserveAmmo( *this, ammoWeapon );
			const ConVar *limit = EngineCvar( item->ammoLimit );
			if ( fundedLoadout && limit != NULL && reserveBefore >= 0 && reserveBefore < limit->GetInt() )
				alias = primarySlot ? "primammo" : "secammo";
			else if ( reserveBefore == 0 && ( limit == NULL || limit->GetInt() > 0 ) )
			{
				ammoSingle = true;
				alias = primarySlot ? "buyammo1" : "buyammo2";
			}
			break;
		}
		case Stage::Complete:
			break;
		}

		char weaponClass[32];
		if ( gun != NULL )
		{
			alias = gun->alias;
			V_snprintf( weaponClass, sizeof( weaponClass ), "weapon_%s", alias );
			classname = weaponClass;
		}
		if ( alias == NULL ) continue;

		char command[48];
		if ( ammoSingle ) V_strncpy( command, alias, sizeof( command ) );
		else V_snprintf( command, sizeof( command ), "buy %s", alias );
		// IServerPluginHelpers dispatches the command locally for fake clients.
		// Source purchases finish synchronously, including account and inventory.
		m_Ctx.pHelpers->ClientCommand( m_pEdict, command );
		const int after = GetNetVarInt( "m_iAccount", -1 );
		const int paid = after >= 0 && after < cash ? cash - after : 0;
		bool confirmed = classname != NULL && FindOwnedWeapon( classname ) != NULL;
		if ( stage == Stage::Armor )
			confirmed = GetNetVarInt( "m_ArmorValue", -1 ) > armorBefore ||
			    ( helmetBefore == 0 && GetNetVarInt( "m_bHasHelmet", -1 ) == 1 );
		if ( stage == Stage::Defuser ) confirmed = GetNetVarInt( "m_bHasDefuser", -1 ) == 1;
		if ( reserveBefore >= 0 ) confirmed = ReserveAmmo( *this, ammoWeapon ) > reserveBefore;

		++state.attempted;
		if ( confirmed ) ++state.confirmed;
		state.spent += paid;
		state.lastItem = alias;
		state.lastResult = confirmed ? "confirmed" : ( paid > 0 ? "charged" : "rejected" );
		if ( stage == Stage::Primary && !confirmed && paid == 0 && gun != NULL &&
		     state.rejectedPrimary == Weapon::Unknown )
		{
			state.rejectedPrimary = gun->kind;
			state.stage = Stage::Primary;
		}
		else if ( stage == Stage::Primary && !confirmed ) state.plan = "save";
		state.nextAt = now + RandomFloat( 0.20f, 0.35f );
		break;
	}
	if ( state.stage == Stage::Complete ) state.status = "complete";
}

void CAgPB::ReportPurchases() const
{
	const AgPBPurchaseRound &round = AgPB_PurchaseRound();
	const AgPBPurchaseState &state = m_Purchases;
	const char *gate = gpGlobals != NULL ? CurrentGate( *this, AgPB_GameMode(), round, gpGlobals->curtime ) : "inactive";
	RouteReport( "buy round=%u stage=%s status=%s cash=%d zone=%d team-economy=%s plan=%s attempts=%d confirmed=%d spent=%d last=%s result=%s",
		round.epoch, StageName( state.stage ), gate != NULL ? gate : state.status,
		GetNetVarInt( "m_iAccount", -1 ), InBuyZone( *this ) ? 1 : 0,
		state.teamCanBuy ? "ready" : "save", state.plan, state.attempted, state.confirmed, state.spent, state.lastItem, state.lastResult );
}
