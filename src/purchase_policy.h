#ifndef AGPB_PURCHASE_POLICY_H
#define AGPB_PURCHASE_POLICY_H

#include "weapon_selection.h"

// Observed by the round event listener. liveAt is the end of freeze time.
struct AgPBPurchaseRound
{
	unsigned int epoch = 0;
	float liveAt = 0.0f;
	bool active = false;
	bool freeze = false;
	int lastWinner = 0;
};

const AgPBPurchaseRound &AgPB_PurchaseRound();

enum class AgPBPurchaseStage
{
	Primary, Armor, Pistol, HE, Flash, Smoke, Defuser, PrimaryAmmo, PistolAmmo, Complete
};

struct AgPBPurchaseState
{
	unsigned int epoch = 0;
	int team = 0;
	float nextAt = 0.0f;
	AgPBPurchaseStage stage = AgPBPurchaseStage::Primary;
	AgPBWeaponSelect::Weapon rejectedPrimary = AgPBWeaponSelect::Weapon::Unknown;
	int attempted = 0;
	int confirmed = 0;
	int spent = 0;
	bool teamCanBuy = true;
	const char *plan = "undecided";
	const char *status = "waiting-round";
	const char *lastItem = "none";
	const char *lastResult = "none";
};

#endif
