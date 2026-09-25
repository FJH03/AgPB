/**
 * AgPB - bot 生命周期与底层控制。
 *
 * 当前阶段：
 *   1. 用 IBotManager("BotManager001") 创建无 AI 假客户端
 *   2. 用 IBotController::RunPlayerMove() 逐 tick 注入 usercmd
 *   3. 队伍切换 / 出生（ChangeTeam + joinclass）
 *   4. netvar 反射（读实体网络字段）—— EBot 移植的 Entity 底座
 *
 * 输入生成（移动、瞄准、战斗）将由 EBot 移植过来的
 * control / navigate / combat 模块接管。
 *
 * 只依赖引擎对外暴露的虚接口，不需要特征码扫描，也不需要链接 server.dll。
 */

#ifndef _INCLUDE_AGPB_BOT_H_
#define _INCLUDE_AGPB_BOT_H_

#include <eiface.h>
#include <edict.h>
#include <interface.h>
#include <mathlib/vector.h>
#include <utlvector.h>
#include <game/server/iplayerinfo.h>
#include <engine/iserverplugin.h>
#include <engine/IEngineTrace.h>

#include "netvars.h"

// CS:S 队伍编号（见 game/shared/cstrike/cs_shareddefs.h）
#define AgPB_TEAM_SPECTATOR 1
#define AgPB_TEAM_T         2
#define AgPB_TEAM_CT        3

/**
 * 引擎上下文：插件 Load() 时填一次，之后只读。
 */
struct BotEngineContext
{
	BotEngineContext()
	{
		pBotManager = NULL;
		pEngine = NULL;
		pPlayerInfoManager = NULL;
		pHelpers = NULL;
		pTrace = NULL;
		pGameEnts = NULL;
	}

	IBotManager         *pBotManager;
	IVEngineServer      *pEngine;
	IPlayerInfoManager  *pPlayerInfoManager;
	IServerPluginHelpers *pHelpers;
	IEngineTrace        *pTrace;
	IServerGameEnts     *pGameEnts;

	bool IsReady() const
	{
		return pBotManager != NULL && pEngine != NULL && pPlayerInfoManager != NULL;
	}
};

class CAgPB
{
public:
	CAgPB();

	bool Create( const BotEngineContext &ctx,
				 const char *name,
				 int team,
				 char *error,
				 size_t maxlen );

	void Destroy();

	/** 每 tick 调用一次：处理入队/出生，并驱动引擎的命令链。 */
	void Think( CGlobalVars *pGlobals );

	/**
	 * 设置目标队伍并立即开始切换。
	 * 合法值：1 = 观察者，2 = T，3 = CT。
	 */
	bool SetTeam( int team );

	bool IsValid() const { return m_pEdict != NULL && m_pController != NULL; }

	edict_t *Edict() const { return m_pEdict; }
	const char *Name() const { return m_Name; }
	int Index() const { return m_iIndex; }
	int DesiredTeam() const { return m_iTeam; }
	IPlayerInfo *PlayerInfo() const { return m_pInfo; }

	// -----------------------------------------------------------------------
	// netvar 访问（M2）
	//
	// 字段表按 ServerClass 缓存，查找是线性扫描（单类约 300 条），
	// 够用；EBot 移植时会在自己的 Entity 层做一次缓存。
	// -----------------------------------------------------------------------

	/** 实体对象基址（CBaseEntity*），netvar 偏移相对它计算。 */
	void *NetVarBase() const;

	/** 本实体所属类的扁平化字段表；失败返回 NULL。 */
	const CNetVarTable *NetVarTable() const;

	/** 按名字查字段；找不到返回 NULL。 */
	const BotNetVar *FindNetVar( const char *name ) const;

	bool   GetNetVarBool( const char *name, bool defaultValue = false ) const;
	int    GetNetVarInt( const char *name, int defaultValue = 0 ) const;
	float  GetNetVarFloat( const char *name, float defaultValue = 0.0f ) const;
	Vector GetNetVarVector( const char *name ) const;

	/** size 超出 GetNumElements() 时返回 defaultValue。 */
	int    GetNetVarArrayInt( const char *name, int index, int defaultValue = 0 ) const;
	float  GetNetVarArrayFloat( const char *name, int index, float defaultValue = 0.0f ) const;

	/** 读 EHANDLE 字段的原始 8 字节值；字段不是 DPT_Int 标量时返回 false。 */
	bool GetNetVarHandleValue( const char *name, uintp *pHandle ) const;

	/** 便捷版：同时给出解包后的 entry / serial（仅用于显示与诊断）。 */
	bool GetNetVarHandle( const char *name, int *pEntry, int *pSerial ) const;

	// -----------------------------------------------------------------------
	// netvar 写入（**只对 bot 自己**，改的是引擎里的真成员）
	//
	// 注意：网络表示 != 内存表示 —— 1 字节成员（m_lifeState）和发送时位压缩的
	// 字段不能乱写。目前只开放确认过宽度的那几种，详见 netvars.h 的说明。
	// -----------------------------------------------------------------------

	bool SetNetVarFloat( const char *name, float flValue );
	bool SetNetVarInt( const char *name, int iValue );

	/** 写一个 Vector：优先按 name[0..2] 三个元素写，退路是整条 Vector 字段。 */
	bool SetNetVarVector( const char *name, const Vector &vValue );

	/**
	 * 【弹道跳/调试】下一 tick 直接把 `m_vecVelocity` 灌成这个值。
	 *
	 * 时序很关键：值会存在 bot 上，在 `Think()` 里**紧挨着 RunPlayerMove 之前**
	 * 写进引擎（这样引擎的 GroundMove/AirMove 会在这个速度基础上继续算：
	 * 摩擦、重力、CheckJumpButton 的 `+=` 冲量都作用在它之上），写完立刻清空。
	 */
	void SetVelocityOverride( const Vector &vVelocity );

	/**
	 * 句柄 -> edict_t*。
	 *
	 * 校验方式是拿 engine 自己为那个实体维护的 ref ehandle 做**整值比对**
	 * （`IHandleEntity::GetRefEHandle()`，ihandleentity.h:23 是公开接口）。
	 * 这样就不需要 `CBaseHandle::Get()`（那个要 `g_pEntityList` 全局），
	 * 也不需要猜 serial 的口径 —— 实测里 `edict_t::m_NetworkSerialNumber`
	 * 和句柄里的 serial 就**对不上**。
	 */
	edict_t *HandleToEdict( uintp handle ) const;

	// -----------------------------------------------------------------------
	// 最小导航（M3 验证用"会不会走路"）
	//
	// 不是 EBot 的 navigate/control 移植版，只做四件事：
	//   朝着路线上的下一个点转 yaw、forwardmove 前进、
	//   边带 PATH_JUMP 就按跳、点带 CROUCH 就按蹲，另外加一个卡住检测。
	// 目的是先用真实数据把"路点图 + ucmd 注入"这条链验证透。
	// -----------------------------------------------------------------------

	/** A* 出路线并开始走。失败时把原因写进 error。 */
	bool StartRoute( int iGoalWaypoint, char *error, size_t maxlen );

	/** 停止行走（清路线与输入）。 */
	void StopRoute();

	bool HasRoute() const { return m_vecRoute.Count() > 0; }
	int  RouteCount() const { return m_vecRoute.Count(); }
	int  RouteGoal() const { return m_iGoalWaypoint; }
	int  RouteIndex() const { return m_iRouteIndex; }
	int  RouteNode( int i ) const;

	/** 行走过程的状态回显（服务器控制台）。 */
	void RouteReport( const char *pszFormat, ... ) const;

private:
	void TryJoinTeam( float flCurTime );

	/** 每 tick 的"走路"部分：把路线翻译成 CUserCmd。 */
	void UpdateRoute( CGlobalVars *pGlobals, CBotCmd &cmd );

	char                 m_Name[64];
	edict_t             *m_pEdict;
	IBotController      *m_pController;
	IPlayerInfo         *m_pInfo;
	BotEngineContext     m_Ctx;

	int   m_iIndex;            // edict 索引
	int   m_iTeam;             // 目标队伍（1/2/3）
	int   m_iCommandNumber;
	bool  m_bClassRequested;   // 是否已经补发过 joinclass

	float m_flNextJoinAttempt;
	float m_flJoinDeadline;    // 小于 0 表示尚未开始计时

	// 【弹道跳】下一 tick 的 m_vecVelocity 覆盖值（写一次就清）
	Vector m_vVelOverride;
	bool   m_bHasVelOverride;

	// 最小导航的路線
	CUtlVector<int> m_vecRoute;
	int    m_iRouteIndex;
	int    m_iGoalWaypoint;
	Vector m_vStuckAnchor;
	float  m_flStuckCheckTime;
};

class CAgPBManager
{
public:
	CAgPBManager();

	void Init( const BotEngineContext &ctx, CreateInterfaceFn pServerFactory );
	void Shutdown();

	bool IsReady() const { return m_Ctx.IsReady(); }
	IBotManager *BotManager() { return m_Ctx.pBotManager; }

	CAgPB *Add( int team, char *error, size_t maxlen );
	bool Remove( int listIndex );
	void RemoveAll();
	void RemoveByEdict( edict_t *pEdict );

	void ThinkAll( CGlobalVars *pGlobals );

	int Count() const { return m_Bots.Count(); }
	CAgPB *Get( int listIndex );

private:
	BotEngineContext m_Ctx;

	CUtlVector<CAgPB *> m_Bots;
	int m_iNextSerial;
};

/** netvar 字段表注册表（按 ServerClass 缓存，进程内单例）。 */
CNetVarRegistry &BotNetVarRegistry();

/** 插件里唯一的 bot 管理器（plugin.cpp 里那个单例的访问器，菜单要用）。 */
CAgPBManager &AgPB_Bots();

/**
 * edict -> ServerClass 名（如 "CCSPlayer" / "CWeaponUSP45"）。
 * 抽出来是为了让 plugin.cpp 不必引入 iserverunknown.h。
 */
const char *AgPB_EntityClassName( edict_t *pEdict );

/**
 * 引擎为该实体维护的 ref ehandle 的整值（`CBaseHandle::ToInt()`）。
 * 取不到时返回 INVALID_EHANDLE_INDEX。
 */
uintp AgPB_RefEHandle( edict_t *pEdict );

#endif // _INCLUDE_AGPB_BOT_H_
