/**
 * AgPB - 路点编辑器：状态、动作、菜单。
 *
 * 对应 EBot 的 Waypoint::ToggleFlags / SetRadius / CreateWaypointPath /
 * Delete / CacheWaypoint / TeleportWaypoint / NodesValid / GetWaypointInfo
 * 与那一整套 g_menus[] 路点菜单。
 *
 * 选点约定（和 EBot 一样）：
 *   - "起点" = 离房主最近的路点（75 单位内，找不到就拒绝）
 *   - "终点" = 房主**准星指向**的路点（GetFacingIndex），没有就用"缓存点"
 */

#ifndef _INCLUDE_AGPB_WPEDIT_H_
#define _INCLUDE_AGPB_WPEDIT_H_

#include <eiface.h>
#include <edict.h>
#include <tier1/convar.h>
#include <mathlib/vector.h>

/** 编辑器运行期状态（进程内单例）。 */
struct AgPBWaypointEditor
{
	AgPBWaypointEditor();

	bool bShow;       // 逐帧画路点（convar agpb_wp_show）
	bool bLabels;     // 每个点都画下标（convar agpb_wp_labels）
	bool bAllLinks;   // 1 = 画所有连线，0 = 只画最近点的（convar agpb_wp_alllinks）

	int iCache;       // 缓存点：连线的另一头
	int iNearest;     // 离房主最近的点
	int iFacing;      // 房主准星指向的点

	Vector vHostOrigin;
	Vector vHostEye;
	QAngle angHostView;
	bool   bHostValid;
};

AgPBWaypointEditor &BotWaypointEditor();

/** listen server 的"房主"：第一个非假客户端玩家（找不到返回 NULL）。 */
edict_t *AgPB_FindHost();

/** 刷新房主位置 / 视角 / nearest / facing。命令与逐帧绘制都会调。 */
void AgPB_RefreshHost();

/** 逐帧入口：同步 convar + 刷新房主 + 需要时绘制。 */
void AgPB_EditorFrame();

/** 打开菜单（1..9 号动作由 AgPB_MenuDispatch 解释）。 */
void AgPB_OpenMenu( edict_t *pClient, int iMenuId );

// ---------------------------------------------------------------------------
// 动作（菜单与 agpb_wp_* 命令共用）
// ---------------------------------------------------------------------------

void AgPB_EditToggleShow( edict_t *pClient );
void AgPB_EditToggleLabels( edict_t *pClient );
void AgPB_EditToggleAllLinks( edict_t *pClient );
void AgPB_EditCache( edict_t *pClient );
/** 按 agpb_wp_autowayzone 决定要不要用 EBot 的算法自动算半径。 */
void AgPB_EditAutoRadius( int iIndex, edict_t *pClient );
void AgPB_EditAddType( edict_t *pClient, unsigned int uFlags, const char *pszTypeName );
void AgPB_EditDeleteNearest( edict_t *pClient );
void AgPB_EditConnect( edict_t *pClient, int iMode );
void AgPB_EditCut( edict_t *pClient );
void AgPB_EditSetRadius( edict_t *pClient, int iRadius );
void AgPB_EditToggleFlag( edict_t *pClient, unsigned int uFlag, const char *pszFlagName );
void AgPB_EditClearFlags( edict_t *pClient );
void AgPB_EditTeleport( edict_t *pClient, int iIndex );
void AgPB_EditToggleNoclip( edict_t *pClient );
void AgPB_EditCheck( edict_t *pClient );
void AgPB_EditStats( edict_t *pClient );

/** 连线的"终点"：准星指向的点，否则缓存点；都没有返回 -1。 */
int AgPB_EditTargetWaypoint();

// ---------------------------------------------------------------------------
// 控制台命令（在 plugin.cpp 里注册成 ConCommand）
// ---------------------------------------------------------------------------

void AgPB_Cmd_Menu( const CCommand &args );
void AgPB_Cmd_Show( const CCommand &args );
void AgPB_Cmd_Labels( const CCommand &args );
void AgPB_Cmd_AllLinks( const CCommand &args );
void AgPB_Cmd_Cache( const CCommand &args );
void AgPB_Cmd_Type( const CCommand &args );
void AgPB_Cmd_Flag( const CCommand &args );
void AgPB_Cmd_Radius( const CCommand &args );
void AgPB_Cmd_Connect( const CCommand &args );
void AgPB_Cmd_Cut( const CCommand &args );
void AgPB_Cmd_Teleport( const CCommand &args );
void AgPB_Cmd_Noclip( const CCommand &args );
void AgPB_Cmd_Check( const CCommand &args );
void AgPB_Cmd_Stats( const CCommand &args );
void AgPB_Cmd_Legend( const CCommand &args );
void AgPB_Cmd_Wayzone( const CCommand &args );
void AgPB_Cmd_Reach( const CCommand &args );

#endif // _INCLUDE_AGPB_WPEDIT_H_
