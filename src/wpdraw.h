/**
 * AgPB - 路点 / 连线的调试叠加层绘制（对应 EBot 的 Waypoint::ShowWaypointMsg）。
 *
 * EBot 是在 GoldSrc 里用引擎的 DrawLine 画的；Source 这边换成
 * IVDebugOverlay（engine/ivdebugoverlay.h，VDEBUG_OVERLAY_INTERFACE_VERSION）。
 * 颜色语义**照抄 EBot**：点的"基础色"画下半截，"附加标志色"画上半截，
 * 连线按 跳 / 叠罗汉 / 仅通视 / 双向 / 单向 上色 —— 具体表见 wpdraw.cpp 顶部。
 */

#ifndef _INCLUDE_AGPB_WPDRAW_H_
#define _INCLUDE_AGPB_WPDRAW_H_

#include <mathlib/vector.h>

class IVDebugOverlay;
struct edict_t;

/** plugin.cpp 在 Load 时注入；拿不到就传 NULL（绘制自动变成空操作）。 */
void AgPB_DrawSetOverlay( IVDebugOverlay *pOverlay );
bool AgPB_DrawAvailable();

/** 给一个玩家画一遍路点 / 连线（逐帧调用）。 */
void AgPB_DrawWaypoints( edict_t *pViewer );

/** 把配色说明打到控制台，打点时对照用。 */
void AgPB_DrawPrintLegend( edict_t *pClient );

#endif // _INCLUDE_AGPB_WPDRAW_H_
