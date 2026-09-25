/**
 * AgPB - 路点 / 连线绘制。
 *
 * 配色语义照抄 EBot（refs/CS-EBOT/source/waypoint.cpp:3284-3462），
 * 僵尸模式的几个标志 CS:S 用不上，直接不画；JUMP / CROUCH / LIFT 是
 * EBot 没给颜色的常用标志，这里补了三个扩展色（都排在 EBot 规则之后）。
 *
 * 点：竖线一根，下半截是"基础色"，上半截是"附加标志色"（没有附加标志就整根基础色）
 *   基础色优先级（高到低）：CAMP=青 GOAL=紫 LADDER=棕 RESCUE=白 AVOID=红
 *                            FALLCHECK=灰 USEBUTTON=蓝 FALLRISK=灰
 *                            JUMP=黄 CROUCH=紫罗兰 LIFT=墨绿 其它=绿
 *   附加色优先级（高到低）：SNIPER=暗金 T=红 CT=蓝 FALLRISK=粉
 * 连线（以"离你最近的点"为观察中心）：
 *   JUMP=红  DOUBLE(叠罗汉)=蓝  VISIBLE=通视绿/被挡橙
 *   双向=黄  只有出边=白  只有入边=墨绿  其它点的连线=蓝（压暗）
 * 半径：蓝色方框（半径 <= 4 画个小叉）
 * 缓存点=黄箭头  准星指向点=白箭头
 * FALLCHECK 点：往下探 60 单位，有地面=蓝，悬空=红
 *
 * 关于"线太细/太浅"：Source 的 IVDebugOverlay 没有线宽参数，所以这里用两个办法：
 *   1. 同一根线按**观察者视角的 right/up** 做世界空间小偏移，叠画 N 遍（agpb_wp_thick）
 *      —— 屏幕上看起来就是一根粗线；
 *   2. 颜色走 AddLineOverlayAlpha，重点元素 alpha=255，背景连线压到 110；
 *      再用 agpb_wp_xray 决定要不要穿墙画（noDepthTest）。
 */

#include <tier1/strtools.h>
#include <trace.h>
#include <engine/ivdebugoverlay.h>
#include <engine/IEngineTrace.h>
#include <ihandleentity.h>
#include <iserverunknown.h>

#include "plugin.h"
#include "waypoint.h"
#include "wpedit.h"
#include "wpdraw.h"

extern IVEngineServer *engine;
extern IEngineTrace *enginetrace;
extern CGlobalVars *gpGlobals;

// wpedit.cpp 里定义
extern ConVar agpb_wp_thick;
extern ConVar agpb_wp_xray;

// duration 0 = NDEBUG_PERSIST_TILL_NEXT_SERVER（活到下一次服务器刷新，逐帧重画）
#define AgPB_DRAW_DURATION 0.0f

// 背景连线（非最近点）的透明度，压暗一点免得糊成一团
#define AgPB_ALPHA_BACK  110
#define AgPB_ALPHA_FULL  255

struct AgPBColor
{
	unsigned char r;
	unsigned char g;
	unsigned char b;
};

static IVDebugOverlay *s_pOverlay = NULL;

// 观察者视角的 right / up，用来给"加粗"做世界空间偏移
static Vector s_vViewRight( 0.0f, 0.0f, 0.0f );
static Vector s_vViewUp( 0.0f, 0.0f, 0.0f );

void AgPB_DrawSetOverlay( IVDebugOverlay *pOverlay )
{
	s_pOverlay = pOverlay;
}

bool AgPB_DrawAvailable()
{
	return ( s_pOverlay != NULL );
}

// ---------------------------------------------------------------------------
// 颜色表（EBot 的对应关系）
// ---------------------------------------------------------------------------

static AgPBColor NodeBasicColor( unsigned int uFlags )
{
	AgPBColor c;

	if ( uFlags & AgPB_WP_CAMP )      { c.r = 0;   c.g = 255; c.b = 255; return c; }
	if ( uFlags & AgPB_WP_GOAL )      { c.r = 128; c.g = 0;   c.b = 255; return c; }
	if ( uFlags & AgPB_WP_LADDER )    { c.r = 128; c.g = 64;  c.b = 0;   return c; }
	if ( uFlags & AgPB_WP_RESCUE )    { c.r = 255; c.g = 255; c.b = 255; return c; }
	if ( uFlags & AgPB_WP_AVOID )     { c.r = 255; c.g = 0;   c.b = 0;   return c; }
	if ( uFlags & AgPB_WP_FALLCHECK ) { c.r = 128; c.g = 128; c.b = 128; return c; }
	if ( uFlags & AgPB_WP_USEBUTTON ) { c.r = 0;   c.g = 0;   c.b = 255; return c; }
	if ( uFlags & AgPB_WP_FALLRISK )  { c.r = 128; c.g = 128; c.b = 128; return c; }

	// AgPB 扩展（EBot 里没有单独给色）
	if ( uFlags & AgPB_WP_JUMP )      { c.r = 255; c.g = 255; c.b = 0;   return c; }
	if ( uFlags & AgPB_WP_CROUCH )    { c.r = 170; c.g = 0;   c.b = 255; return c; }
	if ( uFlags & AgPB_WP_LIFT )      { c.r = 0;   c.g = 128; c.b = 128; return c; }

	// EBot 默认色：ebot_waypoint_r/g/b（绿）
	c.r = 0; c.g = 255; c.b = 0;
	return c;
}

static bool NodeFlagColor( unsigned int uFlags, AgPBColor &out )
{
	if ( uFlags & AgPB_WP_SNIPER )    { out.r = 130; out.g = 87;  out.b = 0;   return true; }
	if ( uFlags & AgPB_WP_TERRORIST ) { out.r = 255; out.g = 0;   out.b = 0;   return true; }
	if ( uFlags & AgPB_WP_COUNTER )   { out.r = 0;   out.g = 0;   out.b = 255; return true; }
	if ( uFlags & AgPB_WP_FALLRISK )  { out.r = 250; out.g = 75;  out.b = 150; return true; }

	return false;
}

// ---------------------------------------------------------------------------
// 基础绘制
// ---------------------------------------------------------------------------

static void DrawLineOnce( const Vector &vStart, const Vector &vEnd, const AgPBColor &color,
                          bool bNoDepthTest, int iAlpha )
{
	if ( s_pOverlay == NULL )
		return;

	s_pOverlay->AddLineOverlayAlpha( vStart, vEnd, color.r, color.g, color.b, iAlpha,
	                                 bNoDepthTest, AgPB_DRAW_DURATION );
}

/**
 * 粗线：同一根线沿观察者视角的 right / up 偏移后叠画几遍。
 *
 * 偏移量是**世界单位**（所以近处看着更粗、远处更细，这和真实线宽的手感一致）；
 * agpb_wp_thick 1..5 决定画几遍（1 = 就是原来的细线）。
 */
static void DrawLine( const Vector &vStart, const Vector &vEnd, const AgPBColor &color,
                      bool bNoDepthTest, int iAlpha = AgPB_ALPHA_FULL )
{
	static const float s_flPass[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f };

	int iPasses = agpb_wp_thick.GetInt();

	if ( iPasses < 1 )
		iPasses = 1;

	if ( iPasses > 5 )
		iPasses = 5;

	for ( int i = 0; i < iPasses; ++i )
	{
		const float flOffset = s_flPass[i];
		const Vector vOffset = s_vViewRight * flOffset + s_vViewUp * ( flOffset * 0.5f );

		DrawLineOnce( vStart + vOffset, vEnd + vOffset, color, bNoDepthTest, iAlpha );
	}
}

static void DrawTextAt( const Vector &vOrigin, int iLine, const char *pszFormat, ... )
{
	if ( s_pOverlay == NULL )
		return;

	char szText[192];
	va_list args;
	va_start( args, pszFormat );
	V_vsnprintf( szText, sizeof( szText ), pszFormat, args );
	va_end( args );

	s_pOverlay->AddTextOverlay( vOrigin, iLine, AgPB_DRAW_DURATION, "%s", szText );
}

// ---------------------------------------------------------------------------
// 主绘制
// ---------------------------------------------------------------------------

static void DrawNode( const AgPBPath &path, bool bXray )
{
	// 高度用当前引擎真实的体积（站立 62/72、蹲姿 45/54，随 sv_cs_use_legacy_viewvectors 变），
	// 不再用 GoldSrc 的 72/36
	const float flHeight = ( ( path.flags & AgPB_WP_CROUCH ) != 0 )
	                       ? AgPB_HullDuckHeight()
	                       : AgPB_HullStandHeight();
	const float flHalf   = flHeight * 0.5f;

	const Vector vBottom = path.origin - Vector( 0.0f, 0.0f, flHalf );
	const Vector vSplit  = path.origin - Vector( 0.0f, 0.0f, flHalf - flHeight * 0.75f );
	const Vector vTop    = path.origin + Vector( 0.0f, 0.0f, flHalf );

	const AgPBColor basic = NodeBasicColor( path.flags );

	AgPBColor flagColor;

	if ( NodeFlagColor( path.flags, flagColor ) )
	{
		DrawLine( vBottom, vSplit, basic, bXray );
		DrawLine( vSplit, vTop, flagColor, bXray );
	}
	else
	{
		DrawLine( vBottom, vTop, basic, bXray );
	}
}

/** 连线端点抬高到身体高度再画（EBot 也是这么错的位）。 */
static Vector LinkPointOf( const AgPBPath &path )
{
	// 连线画在身体中段（高度取当前引擎真实值的一半）
	const float flZ = ( ( path.flags & AgPB_WP_CROUCH ) != 0 )
	                  ? ( AgPB_HullDuckHeight() * 0.5f )
	                  : ( AgPB_HullStandHeight() * 0.5f );
	return path.origin + Vector( 0.0f, 0.0f, flZ );
}

void AgPB_DrawWaypoints( edict_t *pViewer )
{
	if ( s_pOverlay == NULL )
		return;

	CAgPBWaypoints &wp = BotWaypoints();
	AgPBWaypointEditor &ed = BotWaypointEditor();

	const int nCount = wp.Count();

	if ( nCount <= 0 )
		return;

	const bool bXray = agpb_wp_xray.GetBool();

	// 加粗用的偏移方向：拿观察者视角算 right / up
	{
		Vector vForward;

		AngleVectors( ed.angHostView, &vForward, &s_vViewRight, &s_vViewUp );
	}

	// ---- 连线 ----------------------------------------------------------
	if ( ed.bAllLinks )
	{
		AgPBColor color;
		color.r = 0; color.g = 0; color.b = 255;

		for ( int i = 0; i < nCount; ++i )
		{
			const AgPBPath *pFrom = wp.Get( i );

			if ( pFrom == NULL )
				continue;

			for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
			{
				const int iTo = pFrom->index[s];

				if ( !wp.IsValid( iTo ) )
					continue;

				// 背景连线：压暗、不吃 xray（免得整屏都是穿墙线）
				DrawLine( LinkPointOf( *pFrom ), LinkPointOf( *wp.Get( iTo ) ), color,
				          false, AgPB_ALPHA_BACK );
			}
		}
	}

	// 最近点的连线：按 EBot 的规则上色
	const int iNearest = ed.iNearest;

	if ( wp.IsValid( iNearest ) )
	{
		const AgPBPath *pNear = wp.Get( iNearest );
		const Vector vNearLink = LinkPointOf( *pNear );

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iTo = pNear->index[s];

			if ( !wp.IsValid( iTo ) )
				continue;

			const AgPBPath *pTo = wp.Get( iTo );
			const Vector vStart = vNearLink;
			const Vector vEnd   = LinkPointOf( *pTo );

			AgPBColor color;

			if ( pNear->connectionFlags[s] & AgPB_PATH_JUMP )
			{
				color.r = 255; color.g = 0; color.b = 0;          // 跳
			}
			else if ( pNear->connectionFlags[s] & AgPB_PATH_DOUBLE )
			{
				color.r = 0; color.g = 0; color.b = 255;          // 叠罗汉
			}
			else if ( pNear->connectionFlags[s] & AgPB_PATH_VISIBLE )
			{
				// 只保证通视：现场 trace 一次，被挡画橙
				if ( AgPB_TraceClear( vStart, vEnd, pViewer ) )
				{
					color.r = 0; color.g = 255; color.b = 0;
				}
				else
				{
					color.r = 255; color.g = 165; color.b = 0;
				}
			}
			else if ( wp.IsConnected( iTo, iNearest ) )
			{
				color.r = 255; color.g = 255; color.b = 0;        // 双向
			}
			else
			{
				color.r = 250; color.g = 250; color.b = 250;      // 单向出边
			}

			DrawLine( vStart, vEnd, color, bXray );
		}

		// 单向入边（别人能过来、我过不去）
		AgPBColor inColor;
		inColor.r = 0; inColor.g = 192; inColor.b = 96;

		for ( int i = 0; i < nCount; ++i )
		{
			if ( i == iNearest )
				continue;

			if ( wp.IsConnected( i, iNearest ) && !wp.IsConnected( iNearest, i ) )
				DrawLine( vNearLink, LinkPointOf( *wp.Get( i ) ), inColor, bXray );
		}
	}

	// ---- 点本体 --------------------------------------------------------
	for ( int i = 0; i < nCount; ++i )
	{
		const AgPBPath *pPath = wp.Get( i );

		if ( pPath == NULL )
			continue;

		DrawNode( *pPath, bXray );

		if ( ed.bLabels )
			DrawTextAt( pPath->origin + Vector( 0.0f, 0.0f, 84.0f ), 0, "#%d", i );

		// FALLCHECK：往下探一脚，悬空画红
		if ( ( pPath->flags & ( AgPB_WP_FALLCHECK | AgPB_WP_FALLRISK ) ) != 0 )
		{
			const Vector vDown = pPath->origin - Vector( 0.0f, 0.0f, 60.0f );

			AgPBColor ground;

			if ( AgPB_TraceClear( pPath->origin, vDown, NULL ) )
			{
				ground.r = 255; ground.g = 0; ground.b = 0;   // 没砸到地面 = 悬空
			}
			else
			{
				ground.r = 0; ground.g = 0; ground.b = 255;
			}

			DrawLine( pPath->origin, vDown, ground, bXray );
		}
	}

	// ---- 最近点的半径 + 文字信息 ---------------------------------------
	if ( wp.IsValid( iNearest ) )
	{
		const AgPBPath *pNear = wp.Get( iNearest );
		const Vector vCenter = ( ( pNear->flags & AgPB_WP_CROUCH ) != 0 )
		                       ? pNear->origin
		                       : pNear->origin - Vector( 0.0f, 0.0f, 18.0f );

		AgPBColor radiusColor;
		radiusColor.r = 0; radiusColor.g = 0; radiusColor.b = 255;

		if ( pNear->radius > 4 )
		{
			const float flR = (float)pNear->radius;

			const Vector vA = vCenter + Vector(  flR,  flR, 0.0f );
			const Vector vB = vCenter + Vector( -flR,  flR, 0.0f );
			const Vector vC = vCenter + Vector( -flR, -flR, 0.0f );
			const Vector vD = vCenter + Vector(  flR, -flR, 0.0f );

			DrawLine( vA, vB, radiusColor, bXray );
			DrawLine( vB, vC, radiusColor, bXray );
			DrawLine( vC, vD, radiusColor, bXray );
			DrawLine( vD, vA, radiusColor, bXray );
		}
		else
		{
			const float flR = 5.0f;

			DrawLine( vCenter + Vector(  flR, -flR, 0.0f ), vCenter + Vector( -flR,  flR, 0.0f ), radiusColor, bXray );
			DrawLine( vCenter + Vector( -flR, -flR, 0.0f ), vCenter + Vector(  flR,  flR, 0.0f ), radiusColor, bXray );
		}

		char szFlags[192];
		AgPB_WaypointFlagsString( pNear->flags, szFlags, sizeof( szFlags ) );

		int nLinks = 0;

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			if ( pNear->index[s] >= 0 )
				++nLinks;
		}

		DrawTextAt( pNear->origin + Vector( 0.0f, 0.0f, 100.0f ), 0,
		            "#%d  r=%d  links=%d", iNearest, (int)pNear->radius, nLinks );
		DrawTextAt( pNear->origin + Vector( 0.0f, 0.0f, 100.0f ), 1,
		            "flags: %s", szFlags );
	}

	// ---- 缓存点 / 准星指向点：从点拉一条线到你（EBot 的箭头） ---------
	const Vector vViewer = ed.bHostValid ? ed.vHostEye : Vector( 0.0f, 0.0f, 0.0f );

	if ( ed.bHostValid && wp.IsValid( ed.iCache ) )
	{
		AgPBColor cacheColor;
		cacheColor.r = 255; cacheColor.g = 255; cacheColor.b = 0;

		DrawLine( wp.Get( ed.iCache )->origin, vViewer, cacheColor, true );
	}

	if ( ed.bHostValid && wp.IsValid( ed.iFacing ) )
	{
		AgPBColor facingColor;
		facingColor.r = 255; facingColor.g = 255; facingColor.b = 255;

		DrawLine( wp.Get( ed.iFacing )->origin, vViewer, facingColor, true );
	}
}

void AgPB_DrawPrintLegend( edict_t *pClient )
{
	engine->ClientPrintf( pClient, "===== AgPB waypoint colors (EBot scheme) =====\n" );
	engine->ClientPrintf( pClient, "node body : CAMP=cyan GOAL=purple LADDER=brown RESCUE=white\n" );
	engine->ClientPrintf( pClient, "            AVOID=red FALLCHECK=gray USEBUTTON=blue FALLRISK=gray\n" );
	engine->ClientPrintf( pClient, "            JUMP=yellow CROUCH=violet LIFT=teal other=green\n" );
	engine->ClientPrintf( pClient, "node top  : SNIPER=darkgold T=red CT=blue FALLRISK=pink\n" );
	engine->ClientPrintf( pClient, "links     : JUMP=red BOOST=blue VISIBLE=green/orange\n" );
	engine->ClientPrintf( pClient, "            two-way=yellow outgoing-only=white incoming-only=green(0,192,96)\n" );
	engine->ClientPrintf( pClient, "            other waypoints' links = dim blue\n" );
	engine->ClientPrintf( pClient, "radius    : blue box (<=4 draws a small X)\n" );
	engine->ClientPrintf( pClient, "arrows    : cached=yellow facing=white (always through walls)\n" );
	engine->ClientPrintf( pClient, "fallcheck : line 60 units down -> ground=blue, air=red\n" );
	engine->ClientPrintf( pClient, "jump edge : link=red; the takeoff node gets JUMP + radius 4 (EBot's AddPath type 1)\n" );
	engine->ClientPrintf( pClient, "view      : agpb_wp_thick <1..5> line thickness, agpb_wp_xray <0|1> through walls\n" );
}
