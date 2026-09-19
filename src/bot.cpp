/**
 * AgPB - bot 生命周期、队伍切换与底层命令注入。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iserverunknown.h>
#include <ihandleentity.h>

#include "bot.h"

/**
 * 在"服务端本地"执行一条客户端命令。
 *
 * 绝对不要用 IVEngineServer::ClientCommand()：它的实现是 stuffcmd
 * （engine/vengineserver_impl.cpp:1006），通过 NET_StringCmd + SendNetMsg
 * 把命令发给客户端控制台，假客户端没有 netchannel，命令会石沉大海。
 *
 * IServerPluginHelpers::ClientCommand 走的是
 *   CServerPlugin::ClientCommand -> CBaseClient::ExecuteStringCommand
 * 完全在服务端本地执行，假客户端也有效。
 */
static void RunClientCommand( IServerPluginHelpers *pHelpers, edict_t *pEdict, const char *pszCmd )
{
	if ( pHelpers == NULL || pEdict == NULL || pszCmd == NULL )
		return;

	pHelpers->ClientCommand( pEdict, pszCmd );
}

CAgPB::CAgPB()
{
	m_Name[0] = '\0';
	m_pEdict = NULL;
	m_pController = NULL;
	m_pInfo = NULL;
	m_iIndex = 0;
	m_iTeam = 0;
	m_iCommandNumber = 0;
	m_bClassRequested = false;
	m_flNextJoinAttempt = 0.0f;
	m_flJoinDeadline = -1.0f;
	m_flTestForward = 0.0f;
	m_flTestYaw = 0.0f;
}

bool CAgPB::Create( const BotEngineContext &ctx,
						const char *name,
						int team,
						char *error,
						size_t maxlen )
{
	if ( !ctx.IsReady() )
	{
		if ( error && maxlen )
			snprintf( error, maxlen, "IBotManager / IVEngineServer / IPlayerInfoManager unavailable" );
		return false;
	}

	// server.dll 的实现（CPluginBotManager::CreateBot）会：
	//   1. engine->CreateFakeClient(name)
	//   2. 打上 FL_CLIENT | FL_FAKECLIENT
	//   3. ChangeTeam(TEAM_UNASSIGNED) + RemoveAllItems(true) + Spawn()
	// 全程不创建 CCSBot，因此引擎自带的 bot AI 不会介入。
	edict_t *pEdict = ctx.pBotManager->CreateBot( name );
	if ( pEdict == NULL )
	{
		if ( error && maxlen )
			snprintf( error, maxlen, "CreateBot() failed: no free client slot" );
		return false;
	}

	m_Ctx = ctx;
	m_pEdict = pEdict;
	m_iIndex = ctx.pEngine->IndexOfEdict( pEdict );

	strncpy( m_Name, name, sizeof( m_Name ) - 1 );
	m_Name[sizeof( m_Name ) - 1] = '\0';

	m_pController = ctx.pBotManager->GetBotController( pEdict );
	if ( m_pController == NULL )
	{
		if ( error && maxlen )
			snprintf( error, maxlen, "GetBotController() returned NULL (fake client not recognised as a bot?)" );
		return false;
	}

	m_pInfo = ctx.pPlayerInfoManager->GetPlayerInfo( pEdict );

	// 非法队伍值等于"不主动入队"，交给 SetTeam 校验。
	m_iTeam = 0;
	SetTeam( team );

	return true;
}

void CAgPB::Destroy()
{
	if ( m_Ctx.pEngine != NULL && m_pEdict != NULL )
	{
		// Kick the fake client so the engine recycles the slot.
		// IVEngineServer::ServerCommand() 不是变参函数，必须先格式化。
		char szCmd[64];
		snprintf( szCmd, sizeof( szCmd ), "kickid %d\n", m_Ctx.pEngine->GetPlayerUserId( m_pEdict ) );
		m_Ctx.pEngine->ServerCommand( szCmd );
	}

	m_pEdict = NULL;
	m_pController = NULL;
	m_pInfo = NULL;
}

bool CAgPB::SetTeam( int team )
{
	switch ( team )
	{
		case AgPB_TEAM_SPECTATOR:
		case AgPB_TEAM_T:
		case AgPB_TEAM_CT:
			break;

		default:
			return false;
	}

	m_iTeam = team;

	// 重新开始一轮切换流程。
	m_bClassRequested = false;
	m_flJoinDeadline = -1.0f;
	m_flNextJoinAttempt = 0.0f;

	return true;
}

void CAgPB::TryJoinTeam( float flCurTime )
{
	if ( m_pInfo == NULL || m_iTeam <= 0 )
		return;

	const int iCurTeam = m_pInfo->GetTeamIndex();

	if ( iCurTeam == m_iTeam )
	{
		// 已经在目标队伍。
		// T / CT 还需要补一次"选模型"：CCSPlayer::ChangeTeam() 的
		// active-player 分支只把玩家停在 STATE_PICKINGCLASS，而
		// CCSGameRules::FPlayerCanRespawn() 要求
		// GetClass() != CS_CLASS_NONE，不发这一步不会出生。
		// 观察者不需要。
		if ( ( m_iTeam == AgPB_TEAM_T || m_iTeam == AgPB_TEAM_CT ) && !m_bClassRequested )
		{
			m_bClassRequested = true;
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, "joinclass 0" );
		}

		return;
	}

	if ( m_flJoinDeadline < 0.0f )
		m_flJoinDeadline = flCurTime + 15.0f;

	if ( flCurTime >= m_flJoinDeadline || flCurTime < m_flNextJoinAttempt )
		return;

	// 首选：直通 CCSPlayer::ChangeTeam()，不经命令通道，没有限流。
	m_pInfo->ChangeTeam( m_iTeam );

	// 兜底：若该队伍编号没有对应的 CTeam 对象，
	// CCSPlayer::ChangeTeam() 会打印 "invalid team index" 并直接返回。
	// 这种情况改走客户端命令通道（HandleCommand_JoinTeam 的检查更宽松）。
	if ( m_pInfo->GetTeamIndex() != m_iTeam )
	{
		if ( m_iTeam == AgPB_TEAM_SPECTATOR )
		{
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, "spectate" );
		}
		else
		{
			char szCmd[32];
			snprintf( szCmd, sizeof( szCmd ), "jointeam %d", m_iTeam );
			RunClientCommand( m_Ctx.pHelpers, m_pEdict, szCmd );
		}
	}

	m_flNextJoinAttempt = flCurTime + 0.5f;
}

void CAgPB::Think( CGlobalVars *pGlobals )
{
	if ( !IsValid() || pGlobals == NULL )
		return;

	TryJoinTeam( pGlobals->curtime );

	// 即使 bot 处于死亡/未出生状态，也必须每 tick 调用一次
	// RunPlayerMove()：武器逻辑、动画与 PostThink 都挂在
	// CPlayerMove::RunCommand 的调用链上。
	//
	// 目前不下发任何输入 —— 移动 / 瞄准 / 战斗将由移植过来的
	// control / navigate / combat 模块填充这里。
	CBotCmd cmd;
	cmd.Reset();

	// 【临时】ucmd 注入验证：agpb_testmove 设置这两个值。
	// 默认为 0，行为与之前完全一致；M3 的 control 模块接管后删掉这一段。
	if ( m_flTestForward != 0.0f || m_flTestYaw != 0.0f )
	{
		cmd.viewangles.y = m_flTestYaw;
		cmd.forwardmove = m_flTestForward;
	}

	cmd.command_number = ++m_iCommandNumber;
	cmd.tick_count = pGlobals->tickcount;
	cmd.random_seed = (int)rand();

	m_pController->RunPlayerMove( &cmd );
}

// ---------------------------------------------------------------------------
// CAgPBManager
// ---------------------------------------------------------------------------

CAgPBManager::CAgPBManager()
{
	m_iNextSerial = 1;
}

void CAgPBManager::Init( const BotEngineContext &ctx, CreateInterfaceFn pServerFactory )
{
	m_Ctx = ctx;

	// 关键：IBotManager 是 server.dll 通过 CreateInterface 暴露的
	// "BotManager001"（实现类为 CPluginBotManager），而不是引擎接口。
	if ( m_Ctx.pBotManager == NULL && pServerFactory != NULL )
	{
		m_Ctx.pBotManager = (IBotManager *)pServerFactory( INTERFACEVERSION_PLAYERBOTMANAGER, NULL );
	}
}

void CAgPBManager::Shutdown()
{
	RemoveAll();

	m_Ctx = BotEngineContext();
}

CAgPB *CAgPBManager::Add( int team, char *error, size_t maxlen )
{
	if ( !IsReady() )
	{
		if ( error && maxlen )
			snprintf( error, maxlen, "IBotManager ('%s') unavailable", INTERFACEVERSION_PLAYERBOTMANAGER );
		return NULL;
	}

	char szName[64];
	snprintf( szName, sizeof( szName ), "AgPB_%02d", m_iNextSerial );

	CAgPB *pBot = new CAgPB();
	if ( !pBot->Create( m_Ctx, szName, team, error, maxlen ) )
	{
		delete pBot;
		return NULL;
	}

	++m_iNextSerial;
	m_Bots.AddToTail( pBot );

	return pBot;
}

bool CAgPBManager::Remove( int listIndex )
{
	if ( listIndex < 0 || listIndex >= m_Bots.Count() )
		return false;

	CAgPB *pBot = m_Bots[listIndex];
	pBot->Destroy();
	delete pBot;

	m_Bots.Remove( listIndex );
	return true;
}

void CAgPBManager::RemoveAll()
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		m_Bots[i]->Destroy();
		delete m_Bots[i];
	}

	m_Bots.RemoveAll();
}

void CAgPBManager::RemoveByEdict( edict_t *pEdict )
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		if ( m_Bots[i]->Edict() == pEdict )
		{
			// 玩家已经断开，不需要再 kick。
			delete m_Bots[i];
			m_Bots.Remove( i );
			return;
		}
	}
}

void CAgPBManager::ThinkAll( CGlobalVars *pGlobals )
{
	for ( int i = 0; i < m_Bots.Count(); ++i )
	{
		m_Bots[i]->Think( pGlobals );
	}
}

CAgPB *CAgPBManager::Get( int listIndex )
{
	if ( listIndex < 0 || listIndex >= m_Bots.Count() )
		return NULL;

	return m_Bots[listIndex];
}

// ---------------------------------------------------------------------------
// netvar 反射（M2）
// ---------------------------------------------------------------------------

CNetVarRegistry &BotNetVarRegistry()
{
	// 字段表来自引擎启动时静态构建的 SendTable，生命周期覆盖整个进程，
	// 所以缓存一次就够，不需要任何失效逻辑。
	static CNetVarRegistry s_Registry;
	return s_Registry;
}

void *CAgPB::NetVarBase() const
{
	if ( m_pEdict == NULL )
		return NULL;

	IServerUnknown *pUnk = m_pEdict->GetUnknown();
	if ( pUnk == NULL )
		return NULL;

	// CBaseEntity*，公开头文件里只有前置声明，这里当不透明指针用。
	return (void *)pUnk->GetBaseEntity();
}

const CNetVarTable *CAgPB::NetVarTable() const
{
	if ( m_pEdict == NULL )
		return NULL;

	return BotNetVarRegistry().GetForEdict( m_pEdict );
}

const BotNetVar *CAgPB::FindNetVar( const char *name ) const
{
	const CNetVarTable *pTable = NetVarTable();
	if ( pTable == NULL )
		return NULL;

	return pTable->Find( name );
}

bool CAgPB::GetNetVarBool( const char *name, bool defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetBool( pBase, *pVar );
}

int CAgPB::GetNetVarInt( const char *name, int defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetInt( pBase, *pVar );
}

float CAgPB::GetNetVarFloat( const char *name, float defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	return NetVar_GetFloat( pBase, *pVar );
}

Vector CAgPB::GetNetVarVector( const char *name ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return vec3_origin;

	return NetVar_GetVector( pBase, *pVar );
}

int CAgPB::GetNetVarArrayInt( const char *name, int index, int defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	if ( pVar->type != DPT_Array || pVar->elementType == DPT_Float )
		return defaultValue;

	if ( index < 0 || index >= pVar->elements || pVar->stride <= 0 )
		return defaultValue;

	return NetVar_GetArrayInt( pBase, *pVar, index );
}

float CAgPB::GetNetVarArrayFloat( const char *name, int index, float defaultValue ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL )
		return defaultValue;

	if ( pVar->type != DPT_Array || pVar->elementType != DPT_Float )
		return defaultValue;

	if ( index < 0 || index >= pVar->elements || pVar->stride <= 0 )
		return defaultValue;

	return NetVar_GetArrayFloat( pBase, *pVar, index );
}

bool CAgPB::GetNetVarHandleValue( const char *name, uintp *pHandle ) const
{
	const BotNetVar *pVar = FindNetVar( name );
	void *pBase = NetVarBase();

	if ( pVar == NULL || pBase == NULL || pHandle == NULL )
		return false;

	// EHandle 在网络上就是 DPT_Int；数组（DPT_Array）不走这条路。
	// 不用 stride 做门禁：标量的 m_ElementStride 永远是 SIZEOF_IGNORE(-1)。
	if ( pVar->type != DPT_Int )
		return false;

	*pHandle = NetVar_ReadHandleRaw( pBase, *pVar );
	return true;
}

bool CAgPB::GetNetVarHandle( const char *name, int *pEntry, int *pSerial ) const
{
	uintp h = 0;

	if ( !GetNetVarHandleValue( name, &h ) )
		return false;

	if ( !NetVar_HandleIsValid( h ) )
		return false;

	if ( pEntry != NULL )
		*pEntry = NetVar_HandleEntry( h );

	if ( pSerial != NULL )
		*pSerial = NetVar_HandleSerial( h );

	return true;
}

edict_t *CAgPB::HandleToEdict( uintp handle ) const
{
	if ( m_Ctx.pEngine == NULL || !NetVar_HandleIsValid( handle ) )
		return NULL;

	const int entry = NetVar_HandleEntry( handle );

	if ( entry < 0 || entry >= MAX_EDICTS )
		return NULL;

	edict_t *pEdict = m_Ctx.pEngine->PEntityOfEntIndex( entry );
	if ( pEdict == NULL || pEdict->IsFree() )
		return NULL;

	// 权威校验：拿引擎自己维护的 ref ehandle 整值比对。
	// 这同时验证了 entry 是否解对、以及内存里读到的值是否真是该实体的句柄。
	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL )
		return NULL;

	IHandleEntity *pHandleEntity = pNet->GetEntityHandle();
	if ( pHandleEntity == NULL )
		return NULL;

	if ( pHandleEntity->GetRefEHandle().ToInt() != (int)handle )
		return NULL;

	return pEdict;
}

void CAgPB::SetTestInput( float forward, float yaw )
{
	m_flTestForward = forward;
	m_flTestYaw = yaw;
}

const char *AgPB_EntityClassName( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return NULL;

	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL || pNet->GetServerClass() == NULL )
		return NULL;

	return pNet->GetServerClass()->GetName();
}

uintp AgPB_RefEHandle( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	IHandleEntity *pHandleEntity = pNet->GetEntityHandle();
	if ( pHandleEntity == NULL )
		return (uintp)INVALID_EHANDLE_INDEX;

	return (uintp)pHandleEntity->GetRefEHandle().ToInt();
}
