// AgPB bot control menu. GPL-3.0-only.
// Menu flow follows the g_menus and ClientCommand in SyPB (CCNHsK-Dev
// and contributors, GPL-3.0). CS-EBOT was checked for comparison.

#include <tier1/strtools.h>
#include <vstdlib/random.h>

#include "bot.h"
#include "game_target.h"
#include "menu.h"

extern IVEngineServer *engine;

namespace
{
	enum BotControlAction
	{
		QuickAdd = 1,
		SpecifiedAdd,
		KickRandom,
		KickAll,
		KickList,
	};

	enum KickListAction
	{
		PreviousPage = -1,
		NextPage = -2,
		BackToControl = -3,
	};

	struct SkillChoice
	{
		const char *label;
		int low;
		int high;
	};

	constexpr SkillChoice SkillChoices[] = {
		{ "入门 (1–20)", 1, 20 },
		{ "新手 (20–40)", 20, 40 },
		{ "普通 (40–60)", 40, 60 },
		{ "进阶 (60–80)", 60, 80 },
		{ "专家 (80–99)", 80, 99 },
		{ "神级 (100)", 100, 100 },
	};

	// Only the local listen host can use this menu. Each new wizard or kick
	// list resets the choices, so a previous host's selection can't carry over.
	struct BotMenuState
	{
		AgPBBotAddOptions options;
		int team = 0;
		int kickPage = 0;
		bool filling = false;
		int fillSkillIndex = 0;
	};

	BotMenuState menuState;

	bool CanManageBots( edict_t *client )
	{
		if ( engine == NULL || client == NULL || client->IsFree() )
			return false;

		if ( !engine->IsDedicatedServer() && engine->IndexOfEdict( client ) == 1 )
			return true;

		engine->ClientPrintf( client, "[AgPB] Bot management is available to the listen host.\n" );
		return false;
	}

	void AddBot( edict_t *client, int team, const AgPBBotAddOptions &options )
	{
		char error[256] = {};
		const CAgPB *bot = AgPB_Bots().Add( team, error, sizeof( error ), options );
		char text[320];
		if ( bot != NULL )
			Q_snprintf( text, sizeof( text ), "[AgPB] Created bot '%s'.\n", bot->Name() );
		else
			Q_snprintf( text, sizeof( text ), "[AgPB] Add bot failed: %s\n", error );

		engine->ClientPrintf( client, text );
	}

	void FillServer( edict_t *client )
	{
		const SkillChoice &skill = SkillChoices[menuState.fillSkillIndex];
		char error[256] = {};
		const int count = AgPB_Bots().FillServer( menuState.team, skill.low, skill.high,
			menuState.options.personality, error, sizeof( error ) );
		char text[320];
		if ( count > 0 )
			Q_snprintf( text, sizeof( text ), "[AgPB] Queued %d bot(s) to fill the server.\n", count );
		else if ( count == 0 )
			Q_strncpy( text, "[AgPB] The server is already full.\n", sizeof( text ) );
		else
			Q_snprintf( text, sizeof( text ), "[AgPB] Cannot fill the server: %s\n", error );

		engine->ClientPrintf( client, text );
		menuState.filling = false;
	}

	void BuildKickList( CAgPBMenu &menu )
	{
		constexpr int pageSize = AgPB_MENU_MAX_ITEMS - 1;
		CAgPBManager &bots = AgPB_Bots();
		int highestSlot = 1;
		for ( int index = 0; index < bots.Count(); ++index )
			highestSlot = MAX( highestSlot, bots.Get( index )->Index() );
		const int lastPage = ( highestSlot - 1 ) / pageSize;
		menuState.kickPage = clamp( menuState.kickPage, 0, lastPage );

		char title[AgPB_MENU_MAX_TEXT];
		Q_snprintf( title, sizeof( title ), "踢出指定机器人 (%d/%d)", menuState.kickPage + 1, lastPage + 1 );
		menu.Reset( title, bots.Count() == 0 ? "当前没有机器人" : NULL );

		const int pageOffset = menuState.kickPage * pageSize;
		for ( int index = 0; index < bots.Count(); ++index )
		{
			const CAgPB *bot = bots.Get( index );
			const int key = bot->Index() - pageOffset;
			if ( key < 1 || key > pageSize )
				continue;

			const int userid = engine->GetPlayerUserId( bot->Edict() );
			if ( userid > 0 )
				menu.SetItem( key, bot->Name(), userid );
		}

		if ( menuState.kickPage < lastPage )
			menu.SetItem( AgPB_MENU_MAX_ITEMS, "下一页", NextPage );
		if ( menuState.kickPage > 0 )
			menu.SetCancel( "上一页", PreviousPage );
		else
			menu.SetCancel( "返回机器人管理", BackToControl );
	}

	void ShowBotMenu( edict_t *client, int id )
	{
		CAgPBMenu menu;
		switch ( id )
		{
			case AgPB_MENU_BOT_CONTROL:
				menu.Reset( "机器人管理" );
				menu.AddItem( "快速添加", QuickAdd );
				menu.AddItem( "指定添加", SpecifiedAdd );
				menu.AddItem( "随机踢出一个", KickRandom );
				menu.AddItem( "踢出全部", KickAll );
				menu.AddItem( "按名单踢出", KickList );
				break;

			case AgPB_MENU_BOT_SKILL:
				menu.Reset( "选择机器人难度", menuState.filling ? "填满服务器" : NULL );
				for ( int i = 0; i < ARRAYSIZE( SkillChoices ); ++i )
					menu.AddItem( SkillChoices[i].label, i + 1 );
				break;

			case AgPB_MENU_BOT_PERSONALITY:
				menu.Reset( "选择机器人性格", menuState.filling ? "填满服务器" : NULL );
				menu.AddItem( "随机", 1 );
				menu.AddItem( "普通", 2 );
				menu.AddItem( "激进", 3 );
				menu.AddItem( "谨慎", 4 );
				break;

			case AgPB_MENU_BOT_TEAM:
				menu.Reset( "选择机器人队伍", menuState.filling ? "填满服务器" : NULL );
				menu.AddItem( "匪方", AgPB_TEAM_T );
				menu.AddItem( "反恐方", AgPB_TEAM_CT );
				menu.SetItem( 5, "自动选择", 1 );
				break;

			case AgPB_MENU_BOT_APPEARANCE:
			{
				static const char *const namesT[] = {
					"凤凰战士", "精英部队", "北极复仇者", "游击队", "民兵",
				};
				static const char *const namesCT[] = {
					"海豹六队", "德国边防第九大队", "英国特种空勤团", "法国宪兵干预组", "俄罗斯特种部队",
				};
				const bool isT = menuState.team == AgPB_TEAM_T;
				menu.Reset( isT ? "选择匪方外观" : "选择反恐方外观" );
				const char *const *names = isT ? namesT : namesCT;
				for ( int i = 0; i < MIN( 4, AgPB_TEAM_APPEARANCE_COUNT ); ++i )
					menu.AddItem( names[i], i + 1 );
				menu.SetItem( 5, "随机", AgPB_TEAM_APPEARANCE_COUNT + 1 );
#if defined( AGPB_GAME_CSCZS )
				// [game-diff] CS:CZS has a fifth appearance and keeps SyPB auto-select on key 5.
				menu.AddItem( names[4], 5 );
#endif
				break;
			}

			case AgPB_MENU_BOT_KICK:
				BuildKickList( menu );
				break;
		}
		AgPB_MenuShow( client, id, menu );
	}

	bool RemoveBotByUserid( int userid )
	{
		CAgPBManager &bots = AgPB_Bots();
		for ( int index = 0; index < bots.Count(); ++index )
		{
			const CAgPB *bot = bots.Get( index );
			if ( engine->GetPlayerUserId( bot->Edict() ) == userid )
				return bots.Remove( index );
		}
		return false;
	}

	void DispatchControlAction( edict_t *client, int action )
	{
		CAgPBManager &bots = AgPB_Bots();
		switch ( action )
		{
			case QuickAdd:
				AddBot( client, 0, AgPBBotAddOptions() );
				break;
			case SpecifiedAdd:
				menuState = BotMenuState();
				ShowBotMenu( client, AgPB_MENU_BOT_SKILL );
				break;
			case KickRandom:
				if ( bots.Count() > 0 )
				{
					bots.Remove( RandomInt( 0, bots.Count() - 1 ) );
					engine->ClientPrintf( client, "[AgPB] Removed one bot.\n" );
				}
				else
					engine->ClientPrintf( client, "[AgPB] There are no bots to remove.\n" );
				break;
			case KickAll:
				bots.RemoveAll();
				engine->ClientPrintf( client, "[AgPB] Removed all bots.\n" );
				break;
			case KickList:
				menuState.kickPage = 0;
				ShowBotMenu( client, AgPB_MENU_BOT_KICK );
				break;
		}
	}
}

void AgPB_OpenBotControlMenu( edict_t *client )
{
	if ( CanManageBots( client ) )
		ShowBotMenu( client, AgPB_MENU_BOT_CONTROL );
}

void AgPB_OpenBotFillMenu( edict_t *client )
{
	if ( !CanManageBots( client ) )
		return;

	menuState = BotMenuState();
	menuState.filling = true;
	ShowBotMenu( client, AgPB_MENU_BOT_TEAM );
}

void AgPB_BotMenuDispatch( edict_t *client, int id, int action )
{
	if ( !CanManageBots( client ) )
		return;

	switch ( id )
	{
		case AgPB_MENU_BOT_CONTROL:
			DispatchControlAction( client, action );
			break;
		case AgPB_MENU_BOT_SKILL:
		{
			if ( menuState.filling )
				menuState.fillSkillIndex = action - 1;
			else
			{
				const SkillChoice &choice = SkillChoices[action - 1];
				menuState.options.skill = RandomInt( choice.low, choice.high );
			}
			ShowBotMenu( client, AgPB_MENU_BOT_PERSONALITY );
			break;
		}
		case AgPB_MENU_BOT_PERSONALITY:
			menuState.options.personality = action - 2;
			if ( menuState.filling )
				FillServer( client );
			else
				ShowBotMenu( client, AgPB_MENU_BOT_TEAM );
			break;
		case AgPB_MENU_BOT_TEAM:
			menuState.team = action == AgPB_TEAM_T || action == AgPB_TEAM_CT ? action : 0;
			if ( menuState.filling )
				ShowBotMenu( client, AgPB_MENU_BOT_SKILL );
			else if ( menuState.team != 0 )
				ShowBotMenu( client, AgPB_MENU_BOT_APPEARANCE );
			else
				AddBot( client, menuState.team, menuState.options );
			break;
		case AgPB_MENU_BOT_APPEARANCE:
			menuState.options.appearance = action <= AgPB_TEAM_APPEARANCE_COUNT ? action : 0;
			AddBot( client, menuState.team, menuState.options );
			break;
		case AgPB_MENU_BOT_KICK:
			if ( action == BackToControl )
			{
				ShowBotMenu( client, AgPB_MENU_BOT_CONTROL );
				break;
			}
			if ( action == PreviousPage )
				--menuState.kickPage;
			else if ( action == NextPage )
				++menuState.kickPage;
			else if ( !RemoveBotByUserid( action ) )
				engine->ClientPrintf( client, "[AgPB] That bot is no longer available.\n" );

			ShowBotMenu( client, AgPB_MENU_BOT_KICK );
			break;
	}
}
