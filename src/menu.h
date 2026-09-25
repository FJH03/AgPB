/**
 * AgPB - 通用 HUD 菜单框架（对应 EBot 的 g_menus[] + DisplayMenuToClient）。
 *
 * 展示走 "ShowMenu" usermessage（IVEngineServer::UserMessageBegin），也就是
 * CS:S 客户端 CHudMenu（game/client/menu.cpp）渲染的那种左上角数字菜单，
 * 和 EBot 在 GoldSrc 上的观感一致 —— **不是** VGUI 对话框。
 *   - 文本里行首 "->N" 标记的行走高亮字体（客户端的菜单项标记约定）
 *   - 选择由客户端发 menuselect <n> 回到服务器（数字键 0 发的是 menuselect 10）
 * 服务器侧我们钩 IServerGameClients::ClientCommand 收（这样拿得到是哪个客户端）。
 * `agpb_menu <n>` 走同一条分发，手输也行。
 *
 * helpers 不可用（或没拿到 VSP 回调）时自动回落成控制台文本菜单，
 * 选项编号规则完全一样，手输 agpb_menu <n> 也能走。
 *
 * 编号约定照 EBot / CS 数字键：1..9 是选项，0 = 返回 / 取消。
 */

#ifndef _INCLUDE_AGPB_MENU_H_
#define _INCLUDE_AGPB_MENU_H_

#include <eiface.h>
#include <edict.h>

#define AgPB_MENU_MAX_ITEMS  9
#define AgPB_MENU_MAX_TEXT   80
#define AgPB_MENU_MAX_SLOTS  ( AgPB_MENU_MAX_ITEMS + 1 )

/** 菜单 id（相当于 EBot 的 g_menus[] 下标）。 */
enum AgPBMenuId
{
	AgPB_MENU_NONE = 0,
	AgPB_MENU_MAIN,
	AgPB_MENU_WP_MAIN,
	AgPB_MENU_WP_PAGE2,
	AgPB_MENU_WP_ADD,
	AgPB_MENU_WP_ADD2,
	AgPB_MENU_WP_FLAGS1,
	AgPB_MENU_WP_FLAGS2,
	AgPB_MENU_WP_RADIUS,
	AgPB_MENU_WP_PATH,
};

/** 一份菜单内容。动作号由各菜单自己定义，分发时解释。 */
class CAgPBMenu
{
public:
	CAgPBMenu();

	void Reset( const char *pszTitle, const char *pszMessage = NULL );

	/** 追加一项，返回它的编号（1..9）；加不下返回 0。 */
	int AddItem( const char *pszText, int iAction );

	int Count() const { return m_iCount; }
	const char *Text( int i ) const;
	int Action( int i ) const;

	const char *Title() const { return m_szTitle; }
	const char *Message() const { return m_szMessage; }

private:
	char m_szTitle[AgPB_MENU_MAX_TEXT];
	char m_szMessage[256];
	char m_szText[AgPB_MENU_MAX_SLOTS][AgPB_MENU_MAX_TEXT];
	int  m_iAction[AgPB_MENU_MAX_SLOTS];
	int  m_iCount;
};

/** 打开菜单：HUD 可用就走 HUD，否则打控制台。 */
bool AgPB_MenuShow( edict_t *pClient, int iMenuId, const CAgPBMenu &menu );

/** 关掉本插件给这个客户端记的菜单状态（不改变客户端上已画出的那一份）。 */
void AgPB_MenuClose( edict_t *pClient );

/** 只清状态、不碰客户端（断线时用）。 */
void AgPB_MenuForget( edict_t *pClient );

/** 逐帧调用：给开着的菜单续命（客户端 5 秒不收，靠这个顶着）。 */
void AgPB_MenuTick();

/** 该客户端当前打开的菜单 id；没有则 AgPB_MENU_NONE。 */
int AgPB_MenuCurrent( edict_t *pClient );

/**
 * 处理一次选择（来自 menuselect n / agpb_menu n，n = 0 表示取消）。
 * 返回 true = 这次选择归我们，调用方把命令吞掉。
 */
bool AgPB_MenuSelect( edict_t *pClient, int iChoice );

/** 由 wpedit.cpp 提供：按菜单 id 分发动作。 */
void AgPB_MenuDispatch( edict_t *pClient, int iMenuId, int iChoice );

/** 控制台回落：把一份菜单打成文本。 */
void AgPB_MenuPrintConsole( edict_t *pClient, const CAgPBMenu &menu );

#endif // _INCLUDE_AGPB_MENU_H_
