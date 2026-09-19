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

	/**
	 * 【临时】直接指定下一个 CUserCmd 的 forwardmove / viewangles.y。
	 * 仅用于验证 IBotController::RunPlayerMove() 真的驱动了玩家；
	 * M3 移植过来的 control 模块会接管这里，届时可以删掉。
	 */
	void SetTestInput( float forward, float yaw );

private:
	void TryJoinTeam( float flCurTime );

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

	// 【临时】ucmd 注入验证用，默认 0 即行为与之前完全一致
	float m_flTestForward;
	float m_flTestYaw;
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
