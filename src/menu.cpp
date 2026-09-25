/**
 * AgPB - 菜单框架实现，见 menu.h 的说明。
 */

#include <tier1/strtools.h>
#include <irecipientfilter.h>

#include "plugin.h"
#include "menu.h"

// plugin.cpp 里的全局（MMS 的 GET_V_IFACE_* 宏展开出来的）
extern IVEngineServer *engine;
extern CGlobalVars *gpGlobals;

// edict 下标上限（MAX_PLAYERS = 64，多留一格防越界）
#define AgPB_MENU_MAX_CLIENTS 64

/**
 * 续命间隔。
 *
 * 客户端 CHudMenu 5 秒没有输入就把菜单收掉（MENU_SELECTION_TIMEOUT），
 * 所以这里每 3 秒重发一份一模一样的内容，把它的计时器续上 ——
 * 菜单会一直挂在屏幕上，直到选了一项或按 0 退出（用户要求，别自己关）。
 *
 * 客户端重发时会重新 ProcessText 并重置 m_flSelectionTime，
 * 但不会播打开动画，所以视觉上是静的。
 */
#define AgPB_MENU_KEEPALIVE 3.0f

struct AgPBClientMenuState
{
	int iMenuId;
	int iAction[AgPB_MENU_MAX_SLOTS];
	int  iShowSerial;        // 每次下发菜单 +1：用来判断"这次动作有没有重开菜单"
	unsigned short usSlots;
	float flNextRefresh;
	char  szText[512];
};

static AgPBClientMenuState s_MenuStates[AgPB_MENU_MAX_CLIENTS + 1];

/** 1..64，拿不到返回 0。 */
static int ClientIndexOf( edict_t *pClient )
{
	if ( pClient == NULL || pClient->IsFree() || engine == NULL )
		return 0;

	const int iIndex = engine->IndexOfEdict( pClient );

	if ( iIndex < 1 || iIndex > AgPB_MENU_MAX_CLIENTS )
		return 0;

	return iIndex;
}

CAgPBMenu::CAgPBMenu()
{
	Reset( "" );
}

void CAgPBMenu::Reset( const char *pszTitle, const char *pszMessage )
{
	Q_strncpy( m_szTitle, ( pszTitle != NULL ) ? pszTitle : "", sizeof( m_szTitle ) );
	Q_strncpy( m_szMessage, ( pszMessage != NULL ) ? pszMessage : "", sizeof( m_szMessage ) );

	m_iCount = 0;

	for ( int i = 0; i < AgPB_MENU_MAX_SLOTS; ++i )
	{
		m_szText[i][0] = '\0';
		m_iAction[i] = 0;
	}
}

int CAgPBMenu::AddItem( const char *pszText, int iAction )
{
	if ( m_iCount >= AgPB_MENU_MAX_ITEMS )
		return 0;

	++m_iCount;

	Q_strncpy( m_szText[m_iCount], ( pszText != NULL ) ? pszText : "", AgPB_MENU_MAX_TEXT );
	m_iAction[m_iCount] = iAction;

	return m_iCount;
}

const char *CAgPBMenu::Text( int i ) const
{
	if ( i < 1 || i > m_iCount )
		return "";

	return m_szText[i];
}

int CAgPBMenu::Action( int i ) const
{
	if ( i < 1 || i > m_iCount )
		return 0;

	return m_iAction[i];
}

void AgPB_MenuPrintConsole( edict_t *pClient, const CAgPBMenu &menu )
{
	if ( pClient == NULL || engine == NULL )
		return;

	char szLine[256];

	Q_snprintf( szLine, sizeof( szLine ), "===== %s =====\n", menu.Title() );
	engine->ClientPrintf( pClient, szLine );

	if ( menu.Message()[0] != '\0' )
	{
		Q_snprintf( szLine, sizeof( szLine ), "%s\n", menu.Message() );
		engine->ClientPrintf( pClient, szLine );
	}

	for ( int i = 1; i <= menu.Count(); ++i )
	{
		Q_snprintf( szLine, sizeof( szLine ), "  %d. %s\n", i, menu.Text( i ) );
		engine->ClientPrintf( pClient, szLine );
	}

	engine->ClientPrintf( pClient, "  0. Exit\n" );
	engine->ClientPrintf( pClient, "type: agpb_menu <n>\n" );
}

/**
 * 只发给一个客户端的 recipient filter（UserMessageBegin 需要它）。
 */
class CAgPBSingleRecipientFilter : public IRecipientFilter
{
public:
	CAgPBSingleRecipientFilter( int iClient ) : m_iClient( iClient ) {}

	virtual bool IsReliable( void ) const { return true; }
	virtual bool IsInitMessage( void ) const { return false; }
	virtual int  GetRecipientCount( void ) const { return ( m_iClient > 0 ) ? 1 : 0; }
	virtual int  GetRecipientIndex( int slot ) const { return ( slot == 0 ) ? m_iClient : 0; }

private:
	int m_iClient;
};

/**
 * "ShowMenu" usermessage 的 id。
 *
 * 由游戏 DLL 注册（cs_usermessages.cpp:27），id 由注册顺序决定，不能硬编码 ——
 * 这里用 MMS 的 FindUserMessage 按名字取，取一次就缓存。
 */
static int s_iShowMenuId = -2;

static int ShowMenuId()
{
	if ( s_iShowMenuId == -2 )
	{
		s_iShowMenuId = -1;

		if ( g_SMAPI != NULL )
		{
			int iSize = 0;
			const int iId = g_SMAPI->FindUserMessage( "ShowMenu", &iSize );

			if ( iId >= 0 )
				s_iShowMenuId = iId;
		}
	}

	return s_iShowMenuId;
}

/**
 * 拼 HUD 菜单文本。
 *
 * 客户端 CHudMenu::ProcessText（game/client/menu.cpp:268）约定：
 *   行首 "->N" 是"菜单项标记"，客户端把 N 读去当槽位号，**同时**从 "->" 之后
 *   开始把整行当显示文本。所以每项写 "->i. 文本"（i 既是槽位也是屏幕上那个编号，
 *   不要再写第二遍数字，否则会显示成 "11. xxx"）。
 *   没有标记的行（标题、"0. Exit"）用普通字体。
 */
static void BuildMenuText( const CAgPBMenu &menu, char *pszOut, int iMaxLen )
{
	pszOut[0] = '\0';

	Q_strncat( pszOut, menu.Title(), iMaxLen );
	Q_strncat( pszOut, "\n", iMaxLen );

	if ( menu.Message()[0] != '\0' )
	{
		Q_strncat( pszOut, menu.Message(), iMaxLen );
		Q_strncat( pszOut, "\n", iMaxLen );
	}

	for ( int i = 1; i <= menu.Count(); ++i )
	{
		char szLine[AgPB_MENU_MAX_TEXT + 16];

		Q_snprintf( szLine, sizeof( szLine ), "->%d. %s\n", i, menu.Text( i ) );
		Q_strncat( pszOut, szLine, iMaxLen );
	}

	Q_strncat( pszOut, "0. 退出", iMaxLen );
}

/** 发一段 ShowMenu（bMore = 后面还有一段，客户端会拼起来）。 */
static bool SendMenuChunk( int iClient, unsigned short usSlots, const char *pszText, bool bMore )
{
	CAgPBSingleRecipientFilter filter( iClient );
	bf_write *pBuf = engine->UserMessageBegin( &filter, ShowMenuId() );

	if ( pBuf == NULL )
		return false;

	pBuf->WriteWord( (int)usSlots );
	pBuf->WriteChar( 0 );              // 显示时长 <= 0：不按时间自动关
	pBuf->WriteByte( bMore ? 1 : 0 );  // needMore
	pBuf->WriteString( pszText );

	engine->MessageEnd();
	return true;
}

/**
 * 发送整份菜单文本。
 *
 * ShowMenu 单条消息有 255 字节上限，超了引擎会在 DLL_MessageEnd 里直接拒发
 * （"Refusing to send user message ShowMenu of N bytes to client"）。
 * 所以长菜单按行切成多段 —— 客户端 CHudMenu 用 m_fWaitingForMore 把多段拼成
 * 一份，本来就是为此设计的。
 */
static bool SendMenuText( int iClient, unsigned short usSlots, const char *pszText )
{
	const int iChunkMax = 240;   // 240 + 4 字节头 + '\0' = 245 字节，留了余量
	const int iLen = Q_strlen( pszText );

	if ( iLen <= 0 )
		return SendMenuChunk( iClient, usSlots, "", false );

	int iOffset = 0;

	while ( iOffset < iLen )
	{
		int iChunk = iLen - iOffset;

		if ( iChunk > iChunkMax )
		{
			// 尽量切在行尾；实在找不到就硬切（客户端只是拼接，不会坏行）
			iChunk = iChunkMax;

			const char *pCut = pszText + iOffset + iChunk;

			while ( pCut > pszText + iOffset && *pCut != '\n' )
				--pCut;

			if ( *pCut == '\n' )
				iChunk = (int)( pCut - ( pszText + iOffset ) ) + 1;
		}

		char szChunk[256];
		Q_memcpy( szChunk, pszText + iOffset, iChunk );
		szChunk[iChunk] = '\0';

		const bool bMore = ( iOffset + iChunk ) < iLen;

		if ( !SendMenuChunk( iClient, usSlots, szChunk, bMore ) )
			return false;

		iOffset += iChunk;
	}

	return true;
}

/**
 * 让客户端把 HUD 菜单收起来。
 *
 * bits = 0 时客户端走 HideMenu()。退出 / 取消时必须发一次，
 * 否则客户端那边菜单还挂着（它只等 5 秒），这段时间里的数字键会漏给
 * CS 自己的无线电 / 武器菜单，看起来像"bot 突然说了一句话"。
 */
static void AgPB_MenuHideClient( edict_t *pClient )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 || engine == NULL || ShowMenuId() < 0 )
		return;

	CAgPBSingleRecipientFilter filter( iIndex );
	bf_write *pBuf = engine->UserMessageBegin( &filter, ShowMenuId() );

	if ( pBuf == NULL )
		return;

	pBuf->WriteWord( 0 );
	pBuf->WriteChar( 0 );
	pBuf->WriteByte( 0 );
	pBuf->WriteString( "" );

	engine->MessageEnd();
}

bool AgPB_MenuShow( edict_t *pClient, int iMenuId, const CAgPBMenu &menu )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 || menu.Count() <= 0 )
		return false;

	AgPBClientMenuState &state = s_MenuStates[iIndex];

	state.iMenuId = iMenuId;
	++state.iShowSerial;

	for ( int i = 0; i < AgPB_MENU_MAX_SLOTS; ++i )
		state.iAction[i] = menu.Action( i );

	if ( engine != NULL && ShowMenuId() >= 0 )
	{
		// 有效键位：1..count，外加第 10 位（数字键 0 = 退出；
		// 客户端 weapon_selection.cpp:254 会把 0 键当成 slot10 发 menuselect 10）
		unsigned short usSlots = (unsigned short)( 1 << 9 );

		for ( int i = 1; i <= menu.Count(); ++i )
			usSlots |= (unsigned short)( 1 << ( i - 1 ) );

		char szText[512];
		BuildMenuText( menu, szText, sizeof( szText ) );

		if ( !SendMenuText( iIndex, usSlots, szText ) )
		{
			AgPB_MenuPrintConsole( pClient, menu );
			return false;
		}

		// 记下来，好让 AgPB_MenuTick 每隔几秒续一次命
		state.usSlots = usSlots;
		Q_strncpy( state.szText, szText, sizeof( state.szText ) );
		state.flNextRefresh = ( ( gpGlobals != NULL ) ? gpGlobals->curtime : 0.0f ) + AgPB_MENU_KEEPALIVE;

		return true;
	}

	AgPB_MenuPrintConsole( pClient, menu );
	return false;
}

void AgPB_MenuClose( edict_t *pClient )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 )
		return;

	s_MenuStates[iIndex].iMenuId = AgPB_MENU_NONE;
	s_MenuStates[iIndex].szText[0] = '\0';
	AgPB_MenuHideClient( pClient );
}

/** 只清状态，不碰客户端（断线时用 —— 客户端已经没了）。 */
void AgPB_MenuForget( edict_t *pClient )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 )
		return;

	s_MenuStates[iIndex].iMenuId = AgPB_MENU_NONE;
	s_MenuStates[iIndex].szText[0] = '\0';
}

/**
 * 逐帧续命：菜单开着就每 AgPB_MENU_KEEPALIVE 秒重发一份，
 * 客户端那边的 5 秒计时器就一直不会到点。
 */
void AgPB_MenuTick()
{
	if ( gpGlobals == NULL || engine == NULL || ShowMenuId() < 0 )
		return;

	for ( int i = 1; i <= AgPB_MENU_MAX_CLIENTS; ++i )
	{
		AgPBClientMenuState &state = s_MenuStates[i];

		if ( state.iMenuId == AgPB_MENU_NONE || state.szText[0] == '\0' )
			continue;

		if ( gpGlobals->curtime < state.flNextRefresh )
			continue;

		edict_t *pClient = engine->PEntityOfEntIndex( i );

		if ( pClient == NULL || pClient->IsFree() )
		{
			state.iMenuId = AgPB_MENU_NONE;
			state.szText[0] = '\0';
			continue;
		}

		SendMenuText( i, state.usSlots, state.szText );
		state.flNextRefresh = gpGlobals->curtime + AgPB_MENU_KEEPALIVE;
	}
}

int AgPB_MenuCurrent( edict_t *pClient )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 )
		return AgPB_MENU_NONE;

	return s_MenuStates[iIndex].iMenuId;
}

bool AgPB_MenuSelect( edict_t *pClient, int iChoice )
{
	const int iIndex = ClientIndexOf( pClient );

	if ( iIndex == 0 )
		return false;

	// 过期检查与 AgPB_MenuCurrent 同一套逻辑
	if ( AgPB_MenuCurrent( pClient ) == AgPB_MENU_NONE )
		return false;

	AgPBClientMenuState &state = s_MenuStates[iIndex];

	// 0 / 超范围 = 取消 / 退出（HUD 菜单里数字键 0 发的是 menuselect 10）
	if ( iChoice <= 0 || iChoice > AgPB_MENU_MAX_ITEMS )
	{
		state.iMenuId = AgPB_MENU_NONE;
		engine->ClientPrintf( pClient, "[AgPB] menu closed\n" );
		return true;
	}

	const int iAction = state.iAction[iChoice];

	// 槽位没有动作：菜单还挂在屏幕上，照样吞掉，
	// 不要漏给 CS 的无线电 / 武器菜单（那会变成"突然说了一句话"）
	if ( iAction == 0 )
		return true;

	const int iMenuId = state.iMenuId;
	const int iSerialBefore = state.iShowSerial;

	AgPB_MenuDispatch( pClient, iMenuId, iAction );

	// 动作本身没有重开菜单（开关类的一次性动作）→ 认为这次菜单用完了，收掉；
	// 多级菜单（标志开关、加点）会重开，就继续挂着。
	if ( state.iShowSerial == iSerialBefore )
		AgPB_MenuClose( pClient );

	return true;
}
