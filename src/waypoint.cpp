/**
 * AgPB - 路点系统实现。数据模型移植自 EBot 的 Waypoint 类
 * （refs/CS-EBOT/include/core.h:502 struct Path / source/waypoint.cpp）。
 *
 * 存盘走 tier1：CUtlBuffer 逐字段 Put / Get + IFileSystem::WriteFile。
 * 不直接落结构体 —— 以后改结构不会把老文件全读坏。
 *
 * 连接是双向的（EBot 的编辑器也是成对 AddPath）。
 */

#include <tier1/utlbuffer.h>
#include <tier1/strtools.h>
#include <filesystem.h>
#include <trace.h>
#include <engine/IEngineTrace.h>
#include <ihandleentity.h>
#include <iserverunknown.h>
#include <mathlib/mathlib.h>

#include "bot.h"
#include "waypoint.h"

extern IEngineTrace *enginetrace;
extern IServerGameEnts *gameents;

// 插件链接不到 game DLL 里的 filesystem 全局，由 plugin.cpp 在 Load 时注入。
static IFileSystem *s_pFileSystem = NULL;

void CAgPBWaypoints::SetFileSystem( IFileSystem *pFileSystem )
{
	s_pFileSystem = pFileSystem;
}

// 已经为哪张图尝试过读盘 —— 读不到（新图还没打点）是正常情况，
// 不能每帧都去碰磁盘。
static char s_szLoadAttempted[64] = { 0 };

CAgPBWaypoints &BotWaypoints()
{
	static CAgPBWaypoints s_Waypoints;
	return s_Waypoints;
}

static const char *AgPB_WaypointAuthor = "AgPB";

// ---------------------------------------------------------------------------
// 基础
// ---------------------------------------------------------------------------

void CAgPBWaypoints::SetStatus( const char *pszText )
{
	Q_strncpy( m_szStatus, ( pszText != NULL ) ? pszText : "", sizeof( m_szStatus ) );
}

void CAgPBWaypoints::RebuildFile()
{
	if ( m_szMapName[0] == '\0' )
	{
		m_szFile[0] = '\0';
		return;
	}

	Q_snprintf( m_szFile, sizeof( m_szFile ), "addons/AgPB/waypoints/%s.agpw", m_szMapName );
}

void CAgPBWaypoints::Clear()
{
	m_Paths.RemoveAll();
	m_szMapName[0] = '\0';
	m_szFile[0] = '\0';
	m_szStatus[0] = '\0';
}

bool CAgPBWaypoints::SetMapName( const char *pszMapName )
{
	if ( pszMapName == NULL || pszMapName[0] == '\0' )
		return false;

	// 同一张图只处理一次（plugin 每帧都会调它）
	if ( V_strcmp( s_szLoadAttempted, pszMapName ) == 0 )
		return ( m_Paths.Count() > 0 );

	m_Paths.RemoveAll();

	Q_strncpy( m_szMapName, pszMapName, sizeof( m_szMapName ) );
	Q_strncpy( s_szLoadAttempted, pszMapName, sizeof( s_szLoadAttempted ) );

	RebuildFile();

	return Load();
}

const AgPBPath *CAgPBWaypoints::Get( int i ) const
{
	return IsValid( i ) ? &m_Paths[i] : NULL;
}

AgPBPath *CAgPBWaypoints::GetMutable( int i )
{
	return IsValid( i ) ? &m_Paths[i] : NULL;
}

// ---------------------------------------------------------------------------
// 增删查
// ---------------------------------------------------------------------------

int CAgPBWaypoints::Add( const Vector &vOrigin, unsigned int flags )
{
	if ( m_Paths.Count() >= AgPB_WP_MAX )
	{
		SetStatus( "waypoint limit reached" );
		return -1;
	}

	AgPBPath path;
	path.origin = vOrigin;
	path.flags = flags;
	path.radius = 0;
	path.mesh = 0;
	path.gravity = 0.0f;

	for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
	{
		path.index[s] = -1;
		path.connectionFlags[s] = 0;
	}

	m_Paths.AddToTail( path );

	return m_Paths.Count() - 1;
}

void CAgPBWaypoints::Delete( int i )
{
	if ( !IsValid( i ) )
		return;

	m_Paths.Remove( i );

	// 修所有还在的连边：指向 i 的清掉，大于 i 的整体减一
	for ( int p = 0; p < m_Paths.Count(); ++p )
	{
		AgPBPath &path = m_Paths[p];

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			if ( path.index[s] < 0 )
				continue;

			if ( path.index[s] == i )
			{
				path.index[s] = -1;
				path.connectionFlags[s] = 0;
			}
			else if ( path.index[s] > i )
			{
				path.index[s] = (short)( path.index[s] - 1 );
			}
		}
	}
}

int CAgPBWaypoints::FindNearest( const Vector &vOrigin, float flMaxDist ) const
{
	const float flMaxSqr = flMaxDist * flMaxDist;

	int   iBest = -1;
	float flBestSqr = 0.0f;

	for ( int i = 0; i < m_Paths.Count(); ++i )
	{
		const float flSqr = ( m_Paths[i].origin - vOrigin ).LengthSqr();

		if ( flSqr > flMaxSqr )
			continue;

		if ( iBest < 0 || flSqr < flBestSqr )
		{
			iBest = i;
			flBestSqr = flSqr;
		}
	}

	return iBest;
}

int CAgPBWaypoints::FindFarthest( const Vector &vOrigin, float flMinDist ) const
{
	const float flMinSqr = flMinDist * flMinDist;

	int   iBest = -1;
	float flBestSqr = 0.0f;

	for ( int i = 0; i < m_Paths.Count(); ++i )
	{
		const float flSqr = ( m_Paths[i].origin - vOrigin ).LengthSqr();

		if ( flSqr < flMinSqr )
			continue;

		if ( iBest < 0 || flSqr > flBestSqr )
		{
			iBest = i;
			flBestSqr = flSqr;
		}
	}

	return iBest;
}

// ---------------------------------------------------------------------------
// 连边
// ---------------------------------------------------------------------------

/** 返回 path 里指向 iTarget 的槽位下标；没有返回 -1。 */
static int FindLinkSlot( const AgPBPath &path, int iTarget )
{
	for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
	{
		if ( path.index[s] == iTarget )
			return s;
	}

	return -1;
}

/** 返回第一个空槽；满了返回 -1。 */
static int FindFreeSlot( const AgPBPath &path )
{
	for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
	{
		if ( path.index[s] < 0 )
			return s;
	}

	return -1;
}

bool CAgPBWaypoints::AddLink( int iFrom, int iTo, unsigned int flags )
{
	if ( !IsValid( iFrom ) || !IsValid( iTo ) || iFrom == iTo )
		return false;

	AgPBPath &a = m_Paths[iFrom];
	AgPBPath &b = m_Paths[iTo];

	// 已经连过 → 当成"更新标志"，不算新增
	const int iExisting = FindLinkSlot( a, iTo );

	if ( iExisting >= 0 )
	{
		a.connectionFlags[iExisting] = (unsigned short)flags;

		const int iBack = FindLinkSlot( b, iFrom );

		if ( iBack >= 0 )
			b.connectionFlags[iBack] = (unsigned short)flags;

		return false;
	}

	const int iSlotA = FindFreeSlot( a );
	const int iSlotB = FindFreeSlot( b );

	if ( iSlotA < 0 || iSlotB < 0 )
	{
		SetStatus( "no free link slot (8 max per waypoint)" );
		return false;
	}

	a.index[iSlotA] = (short)iTo;
	a.connectionFlags[iSlotA] = (unsigned short)flags;

	b.index[iSlotB] = (short)iFrom;
	b.connectionFlags[iSlotB] = (unsigned short)flags;

	return true;
}

bool CAgPBWaypoints::AddLinkDirected( int iFrom, int iTo, unsigned int flags )
{
	if ( !IsValid( iFrom ) || !IsValid( iTo ) || iFrom == iTo )
		return false;

	AgPBPath &a = m_Paths[iFrom];

	// 已经连过 → 只更新标志（和 AddLink 一致）
	const int iExisting = FindLinkSlot( a, iTo );

	if ( iExisting >= 0 )
	{
		a.connectionFlags[iExisting] = (unsigned short)flags;
		return false;
	}

	const int iSlot = FindFreeSlot( a );

	if ( iSlot < 0 )
	{
		SetStatus( "no free link slot (8 max per waypoint)" );
		return false;
	}

	a.index[iSlot] = (short)iTo;
	a.connectionFlags[iSlot] = (unsigned short)flags;

	return true;
}

bool CAgPBWaypoints::RemoveLink( int iFrom, int iTo )
{
	if ( !IsValid( iFrom ) || !IsValid( iTo ) )
		return false;

	bool bAny = false;

	const int iSlotA = FindLinkSlot( m_Paths[iFrom], iTo );

	if ( iSlotA >= 0 )
	{
		m_Paths[iFrom].index[iSlotA] = -1;
		m_Paths[iFrom].connectionFlags[iSlotA] = 0;
		bAny = true;
	}

	const int iSlotB = FindLinkSlot( m_Paths[iTo], iFrom );

	if ( iSlotB >= 0 )
	{
		m_Paths[iTo].index[iSlotB] = -1;
		m_Paths[iTo].connectionFlags[iSlotB] = 0;
		bAny = true;
	}

	return bAny;
}

bool CAgPBWaypoints::IsConnected( int iFrom, int iTo, unsigned int *pFlags ) const
{
	if ( !IsValid( iFrom ) || !IsValid( iTo ) )
		return false;

	const int iSlot = FindLinkSlot( m_Paths[iFrom], iTo );

	if ( iSlot < 0 )
		return false;

	if ( pFlags != NULL )
		*pFlags = m_Paths[iFrom].connectionFlags[iSlot];

	return true;
}

int CAgPBWaypoints::LinkCount() const
{
	int nTotal = 0;

	for ( int i = 0; i < m_Paths.Count(); ++i )
	{
		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iTo = m_Paths[i].index[s];

			if ( iTo < 0 )
				continue;

			// 只数一次：i -> iTo 且 iTo > i，或者 iTo 没有回边（单向）
			if ( iTo > i || !IsConnected( iTo, i ) )
				++nTotal;
		}
	}

	return nTotal;
}

// ---------------------------------------------------------------------------
// 寻路（移植 EBot source/navigate.cpp）
// ---------------------------------------------------------------------------
//
// EBot 那一套在 GoldSrc 上是异步的：界面线程 RequestPath -> 工作线程跑
// RunAsyncAStar -> 主线程 IsPathReady 取结果。MSM 插件里我们就在 GameFrame
// 里同步跑（点数量级只有几百，一次 A* 是微秒级），省掉线程和锁。

/**
 * 二叉最小堆，照抄 EBot navigate.cpp:1462 LocalPriorityQueue
 * （navigate.cpp:1127 PriorityQueue::RemoveLowest 是同一份代码）。
 *
 * 注意它的比较用的是 `<` 而不是 `<=`：优先级相等时也会继续下沉。
 * 不影响正确性，照抄即可。
 */
class CAgPBWaypointHeap
{
public:
	bool Setup( int nCapacity )
	{
		if ( nCapacity < 1 )
			return false;

		m_Nodes.SetSize( nCapacity );
		m_nSize = 0;
		return true;
	}

	bool IsEmpty() const { return ( m_nSize < 1 ); }

	void Insert( int iId, float flPriority )
	{
		if ( m_nSize >= m_Nodes.Count() )
			return;

		m_Nodes[m_nSize].id = (short)iId;
		m_Nodes[m_nSize].priority = flPriority;

		int iChild = m_nSize++;

		while ( iChild > 0 )
		{
			const int iParent = ( iChild - 1 ) / 2;

			if ( m_Nodes[iParent].priority < m_Nodes[iChild].priority )
				break;

			Swap( iParent, iChild );
			iChild = iParent;
		}
	}

	int RemoveLowest()
	{
		const int iResult = m_Nodes[0].id;

		--m_nSize;
		m_Nodes[0] = m_Nodes[m_nSize];

		int iParent = 0;
		int iChild = 1;

		const AgPBHeapNode ref = m_Nodes[0];

		while ( iChild < m_nSize )
		{
			const int iRight = iChild + 1;

			if ( iRight < m_nSize && m_Nodes[iRight].priority < m_Nodes[iChild].priority )
				iChild = iRight;

			if ( ref.priority < m_Nodes[iChild].priority )
				break;

			m_Nodes[iParent] = m_Nodes[iChild];
			iParent = iChild;
			iChild = ( 2 * iParent ) + 1;
		}

		m_Nodes[iParent] = ref;

		return iResult;
	}

private:
	struct AgPBHeapNode
	{
		short id;
		float priority;
	};

	void Swap( int a, int b )
	{
		const AgPBHeapNode tmp = m_Nodes[a];
		m_Nodes[a] = m_Nodes[b];
		m_Nodes[b] = tmp;
	}

	CUtlVector< AgPBHeapNode > m_Nodes;
	int m_nSize;
};

/**
 * 这个路点能不能走。
 * 对应 EBot GF_CostNormal（navigate.cpp:1233）开头那几个 65355.0f —— 那边是
 * "代价大到不可能被选中"，我们直接当不通。
 *
 *   AVOID      避开点
 *   DJUMP      需要队友叠罗汉（EBot 里只有僵尸用，我们没有配合逻辑）
 *   TERRORIST  阵营专用（EBot 是在选目标点时过滤的，这里一起做掉）
 *   COUNTER    同上；iTeam == 0 表示不过滤
 */
bool CAgPBWaypoints::IsWaypointPassable( int i, int iTeam ) const
{
	const unsigned int flags = m_Paths[i].flags;

	if ( flags & AgPB_WP_AVOID )
		return false;

	if ( flags & AgPB_WP_DJUMP )
		return false;

	if ( ( flags & AgPB_WP_TERRORIST ) != 0 && iTeam != 0 && iTeam != AgPB_TEAM_T )
		return false;

	if ( ( flags & AgPB_WP_COUNTER ) != 0 && iTeam != 0 && iTeam != AgPB_TEAM_CT )
		return false;

	return true;
}

/**
 * 从 iFrom 走到 iTo 的代价。返回 false = 这条边不能走。
 *
 * EBot 把代价分成 GF_CostNormal / GF_CostCareful / GF_CostRusher 三套人格
 * （navigate.cpp:1173-1445），区别只在 AVOID/LADDER/CROUCH/DJUMP 的取舍上。
 * 我们先只留一套"稳妥版"：LADDER 和 CROUCH 都 ×2（Normal 只给 LADDER ×2、
 * Careful 给 CROUCH ×2），这样 A* 会优先走好走的路，但不是不能走。
 *
 * 注意 EBot 的 cost 是对**邻居**（它函数签名里的 parent 参数）判断的，
 * 所以这里也只看 iTo 的标志位。
 */
bool CAgPBWaypoints::GetLinkCost( int iFrom, int iTo, unsigned int uLinkFlags, float &flCost ) const
{
	if ( uLinkFlags & AgPB_PATH_DOUBLE )
		return false;      // 需要队友叠罗汉

	const AgPBPath &from = m_Paths[iFrom];
	const AgPBPath &to = m_Paths[iTo];

	flCost = ( to.origin - from.origin ).Length();

	if ( to.flags & AgPB_WP_LADDER )
		flCost *= 2.0f;

	if ( to.flags & AgPB_WP_CROUCH )
		flCost *= 2.0f;

	return true;
}

bool CAgPBWaypoints::FindPath( int iStart, int iGoal, CUtlVector< int > &outPath,
                               int iTeam, int iAvoidWaypoint )
{
	outPath.RemoveAll();

	if ( !IsValid( iStart ) || !IsValid( iGoal ) )
		return false;

	if ( iStart == iGoal )
	{
		outPath.AddToTail( iStart );
		return true;
	}

	const int nCount = m_Paths.Count();

	// EBot 的 AStar 结构（navigate.cpp:1446）
	struct AgPBPathNode
	{
		float g;
		float f;
		short parent;
		bool  isClosed;
	};

	CUtlVector< AgPBPathNode > nodes;
	nodes.SetSize( nCount );

	for ( int i = 0; i < nCount; ++i )
	{
		nodes[i].g = 0.0f;
		nodes[i].f = 0.0f;
		nodes[i].parent = -1;
		nodes[i].isClosed = false;
	}

	// 启发式：欧氏距离（EBot 没算距离矩阵时用的就是 HF_Distance）。
	// EBot 有矩阵时换成 HF_Matrix（沿图的路径距离），那只是让 A* 少展开几个点，
	// 结果是同一条最短路，所以先不背 128MB 的矩阵。
	const Vector vGoal = m_Paths[iGoal].origin;

	nodes[iStart].g = 0.0f;
	nodes[iStart].f = ( m_Paths[iStart].origin - vGoal ).Length();

	CAgPBWaypointHeap heap;

	// 堆不维护 decrease-key，同一个点可能被压进多次（弹出时用 isClosed 过滤），
	// 上界是 起点 + 每条边一次松弛 = 点数 * (最大连边数 + 1)。
	if ( !heap.Setup( nCount * ( AgPB_WP_MAX_PATH_INDEX + 1 ) + 1 ) )
		return false;

	heap.Insert( iStart, nodes[iStart].f );

	while ( !heap.IsEmpty() )
	{
		const int iCurrent = heap.RemoveLowest();

		if ( iCurrent == iGoal )
		{
			// 回溯 + 反转，和 EBot 一样
			for ( int i = iCurrent; IsValid( i ); i = (int)nodes[i].parent )
				outPath.AddToTail( i );

			outPath.Reverse();
			return true;
		}

		if ( nodes[iCurrent].isClosed )
			continue;

		nodes[iCurrent].isClosed = true;

		const AgPBPath &path = m_Paths[iCurrent];

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iNext = path.index[s];

			if ( !IsValid( iNext ) )
				continue;

			if ( iNext == iAvoidWaypoint )
				continue;

			if ( !IsWaypointPassable( iNext, iTeam ) )
				continue;

			float flCost = 0.0f;

			if ( !GetLinkCost( iCurrent, iNext, path.connectionFlags[s], flCost ) )
				continue;

			const float flG = nodes[iCurrent].g + flCost;
			const float flF = flG + ( m_Paths[iNext].origin - vGoal ).Length();

			// f == 0 当"还没来过"，照抄 EBot
			if ( !nodes[iNext].isClosed && ( nodes[iNext].f == 0.0f || nodes[iNext].f > flF ) )
			{
				nodes[iNext].parent = (short)iCurrent;
				nodes[iNext].g = flG;
				nodes[iNext].f = flF;
				heap.Insert( iNext, flF );
			}
		}
	}

	return false;
}

void CAgPBWaypoints::ComputeDistances( int iSource, CUtlVector< float > &vecDist,
                                       CUtlVector< short > &vecParent,
                                       int iTeam, int iAvoidWaypoint )
{
	const int nCount = m_Paths.Count();

	vecDist.SetSize( nCount );
	vecParent.SetSize( nCount );

	for ( int i = 0; i < nCount; ++i )
	{
		vecDist[i] = AgPB_WP_DIST_INF;
		vecParent[i] = -1;
	}

	if ( !IsValid( iSource ) )
		return;

	CUtlVector< unsigned char > visited;
	visited.SetSize( nCount );

	for ( int i = 0; i < nCount; ++i )
		visited[i] = 0;

	CAgPBWaypointHeap heap;

	if ( !heap.Setup( nCount * ( AgPB_WP_MAX_PATH_INDEX + 1 ) + 1 ) )
		return;

	vecDist[iSource] = 0.0f;
	heap.Insert( iSource, 0.0f );

	while ( !heap.IsEmpty() )
	{
		const int iCurrent = heap.RemoveLowest();

		if ( visited[iCurrent] )
			continue;

		visited[iCurrent] = 1;

		const AgPBPath &path = m_Paths[iCurrent];

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
		{
			const int iNext = path.index[s];

			if ( !IsValid( iNext ) || visited[iNext] )
				continue;

			if ( iNext == iAvoidWaypoint )
				continue;

			if ( !IsWaypointPassable( iNext, iTeam ) )
				continue;

			float flCost = 0.0f;

			if ( !GetLinkCost( iCurrent, iNext, path.connectionFlags[s], flCost ) )
				continue;

			const float flNew = vecDist[iCurrent] + flCost;

			if ( flNew < vecDist[iNext] )
			{
				vecDist[iNext] = flNew;
				vecParent[iNext] = (short)iCurrent;
				heap.Insert( iNext, flNew );
			}
		}
	}
}

float CAgPBWaypoints::PathDistance( int iFrom, int iTo, int iTeam, int iAvoidWaypoint )
{
	if ( !IsValid( iFrom ) || !IsValid( iTo ) )
		return AgPB_WP_DIST_INF;

	if ( iFrom == iTo )
		return 0.0f;

	CUtlVector< float > vecDist;
	CUtlVector< short > vecParent;

	ComputeDistances( iFrom, vecDist, vecParent, iTeam, iAvoidWaypoint );

	return vecDist[iTo];
}

// ---------------------------------------------------------------------------
// 存盘
// ---------------------------------------------------------------------------
//
// 格式（全部小端，字段逐个 Put / Get）：
//   u32 magic 'AGPB'   u32 version   u32 count
//   char[32] mapname   char[32] author
//   count × {
//       float x,y,z  |  u32 flags  |  u8 radius  u8 mesh
//       8 × i16 index  |  8 × u16 connectionFlags  |  float gravity
//   }
//
// mapname / author 用定长数组 + Put / Get，不用 PutString：
// PutString 会写一个长度前缀，以后想手改文件会很难看。

bool CAgPBWaypoints::Save()
{
	if ( s_pFileSystem == NULL )
	{
		SetStatus( "no IFileSystem" );
		return false;
	}

	if ( m_szFile[0] == '\0' )
	{
		SetStatus( "no map name" );
		return false;
	}

	CUtlBuffer buf( 4096, 0 );

	buf.PutUnsignedInt( AgPB_WP_MAGIC );
	buf.PutUnsignedInt( AgPB_WP_VERSION );
	buf.PutUnsignedInt( (unsigned int)m_Paths.Count() );

	char szMap[32] = { 0 };
	Q_strncpy( szMap, m_szMapName, sizeof( szMap ) );
	buf.Put( szMap, sizeof( szMap ) );

	char szAuthor[32] = { 0 };
	Q_strncpy( szAuthor, AgPB_WaypointAuthor, sizeof( szAuthor ) );
	buf.Put( szAuthor, sizeof( szAuthor ) );

	for ( int i = 0; i < m_Paths.Count(); ++i )
	{
		const AgPBPath &p = m_Paths[i];

		buf.PutFloat( p.origin.x );
		buf.PutFloat( p.origin.y );
		buf.PutFloat( p.origin.z );

		buf.PutUnsignedInt( p.flags );
		buf.PutUnsignedChar( p.radius );
		buf.PutUnsignedChar( p.mesh );

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
			buf.PutShort( p.index[s] );

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
			buf.PutUnsignedShort( p.connectionFlags[s] );

		buf.PutFloat( p.gravity );
	}

	s_pFileSystem->CreateDirHierarchy( "addons/AgPB/waypoints", "GAME" );

	if ( !s_pFileSystem->WriteFile( m_szFile, "GAME", buf ) )
	{
		SetStatus( "WriteFile failed (see console for FS errors)" );
		return false;
	}

	char szMsg[176];
	Q_snprintf( szMsg, sizeof( szMsg ), "saved %d point(s), %d link(s) -> %s",
	            m_Paths.Count(), LinkCount(), m_szFile );
	SetStatus( szMsg );

	return true;
}

bool CAgPBWaypoints::Load()
{
	if ( s_pFileSystem == NULL )
	{
		SetStatus( "no IFileSystem" );
		return false;
	}

	if ( m_szFile[0] == '\0' )
	{
		SetStatus( "no map name" );
		return false;
	}

	FileHandle_t fp = s_pFileSystem->Open( m_szFile, "rb", "GAME" );

	if ( fp == 0 )
	{
		SetStatus( "no waypoint file for this map yet (use agpb_wp_save to create one)" );
		return false;
	}

	const int nSize = (int)s_pFileSystem->Size( fp );

	if ( nSize <= 64 )
	{
		s_pFileSystem->Close( fp );
		SetStatus( "waypoint file too small" );
		return false;
	}

	// 先整块读进来，再用只读 CUtlBuffer 解析 —— 比 ReadFile(CUtlBuffer&)
	// 那条"最优 IO"路径（会按对齐搬动读写下标）确定得多。
	CUtlVector< unsigned char > data;
	data.SetSize( nSize );

	const int nRead = s_pFileSystem->Read( data.Base(), nSize, fp );

	s_pFileSystem->Close( fp );

	if ( nRead != nSize )
	{
		SetStatus( "short read" );
		return false;
	}

	CUtlBuffer buf( (const void *)data.Base(), nSize, CUtlBuffer::READ_ONLY );

	const unsigned int iMagic = buf.GetUnsignedInt();
	const unsigned int iVersion = buf.GetUnsignedInt();

	if ( !buf.IsValid() || iMagic != AgPB_WP_MAGIC )
	{
		SetStatus( "bad waypoint file magic (not an .agpw?)" );
		return false;
	}

	if ( iVersion != AgPB_WP_VERSION )
	{
		char szMsg[128];
		Q_snprintf( szMsg, sizeof( szMsg ),
		            "unsupported waypoint file version %u (this build expects %u)",
		            iVersion, (unsigned int)AgPB_WP_VERSION );
		SetStatus( szMsg );
		return false;
	}

	const unsigned int iCount = buf.GetUnsignedInt();

	if ( !buf.IsValid() || iCount > AgPB_WP_MAX )
	{
		SetStatus( "bad waypoint count" );
		return false;
	}

	char szMap[32] = { 0 };
	char szAuthor[32] = { 0 };
	buf.Get( szMap, sizeof( szMap ) );
	buf.Get( szAuthor, sizeof( szAuthor ) );

	m_Paths.RemoveAll();
	m_Paths.EnsureCapacity( (int)iCount );

	for ( unsigned int i = 0; i < iCount && buf.IsValid(); ++i )
	{
		AgPBPath p;

		p.origin.x = buf.GetFloat();
		p.origin.y = buf.GetFloat();
		p.origin.z = buf.GetFloat();

		p.flags = buf.GetUnsignedInt();
		p.radius = buf.GetUnsignedChar();
		p.mesh = buf.GetUnsignedChar();

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
			p.index[s] = buf.GetShort();

		for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
			p.connectionFlags[s] = buf.GetUnsignedShort();

		p.gravity = buf.GetFloat();

		if ( !buf.IsValid() )
			break;

		m_Paths.AddToTail( p );
	}

	if ( !buf.IsValid() || (unsigned int)m_Paths.Count() != iCount )
	{
		m_Paths.RemoveAll();
		SetStatus( "waypoint file truncated" );
		return false;
	}

	char szMsg[176];
	Q_snprintf( szMsg, sizeof( szMsg ), "loaded %d point(s), %d link(s) from %s",
	            m_Paths.Count(), LinkCount(), m_szFile );
	SetStatus( szMsg );

	return true;
}

// ---------------------------------------------------------------------------
// 显示 / 菜单用的名字表（对应 EBot 的 GetWaypointInfo）
// ---------------------------------------------------------------------------

struct AgPBWaypointFlagNameEntry
{
	unsigned int uFlag;
	const char  *pszName;
};

// 顺序 = 菜单里的显示顺序，也是状态串的拼接顺序
static const AgPBWaypointFlagNameEntry s_WaypointFlagNames[] =
{
	{ AgPB_WP_CAMP,      "CAMP" },
	{ AgPB_WP_GOAL,      "GOAL" },
	{ AgPB_WP_RESCUE,    "RESCUE" },
	{ AgPB_WP_AVOID,     "AVOID" },
	{ AgPB_WP_USEBUTTON, "USEBUTTON" },
	{ AgPB_WP_LADDER,    "LADDER" },
	{ AgPB_WP_CROUCH,    "CROUCH" },
	{ AgPB_WP_JUMP,      "JUMP" },
	{ AgPB_WP_LIFT,      "LIFT" },
	{ AgPB_WP_FALLCHECK, "FALLCHECK" },
	{ AgPB_WP_FALLRISK,  "FALLRISK" },
	{ AgPB_WP_SNIPER,    "SNIPER" },
	{ AgPB_WP_TERRORIST, "T" },
	{ AgPB_WP_COUNTER,   "CT" },
	{ AgPB_WP_DJUMP,     "DJUMP" },
	{ AgPB_WP_CROSSING,  "CROSSING" },
};

const char *AgPB_WaypointFlagName( unsigned int uFlag )
{
	for ( int i = 0; i < (int)ARRAYSIZE( s_WaypointFlagNames ); ++i )
	{
		if ( s_WaypointFlagNames[i].uFlag == uFlag )
			return s_WaypointFlagNames[i].pszName;
	}

	return NULL;
}

unsigned int AgPB_WaypointFlagByName( const char *pszName )
{
	if ( pszName == NULL || pszName[0] == '\0' )
		return 0;

	for ( int i = 0; i < (int)ARRAYSIZE( s_WaypointFlagNames ); ++i )
	{
		if ( V_stricmp( s_WaypointFlagNames[i].pszName, pszName ) == 0 )
			return s_WaypointFlagNames[i].uFlag;
	}

	return 0;
}

void AgPB_WaypointFlagsString( unsigned int uFlags, char *pszOut, int iMaxLen )
{
	if ( pszOut == NULL || iMaxLen <= 0 )
		return;

	pszOut[0] = '\0';

	bool bAny = false;

	for ( int i = 0; i < (int)ARRAYSIZE( s_WaypointFlagNames ); ++i )
	{
		if ( ( uFlags & s_WaypointFlagNames[i].uFlag ) == 0 )
			continue;

		if ( bAny )
			Q_strncat( pszOut, "|", iMaxLen );

		Q_strncat( pszOut, s_WaypointFlagNames[i].pszName, iMaxLen );

		bAny = true;
	}

  if ( !bAny )
    Q_strncpy( pszOut, "none", iMaxLen );
}

// ---------------------------------------------------------------------------
// 视线 / hull trace（编辑器、wayzone、绘制共用）
// ---------------------------------------------------------------------------

// GoldSrc 的 head_hull ≈ 蹲姿体积；Source 这边用同一套 extents
static const Vector s_vHeadHullMin( -16.0f, -16.0f, -18.0f );
static const Vector s_vHeadHullMax(  16.0f,  16.0f,  18.0f );

class CAgPBWaypointTraceFilter : public CTraceFilter
{
public:
	CAgPBWaypointTraceFilter( IHandleEntity *pIgnore ) : m_pIgnore( pIgnore ) {}

	virtual bool ShouldHitEntity( IHandleEntity *pEntity, int contentsMask )
	{
		return ( pEntity != m_pIgnore );
	}

private:
	IHandleEntity *m_pIgnore;
};

static IHandleEntity *AgPB_EdictHandleEntity( edict_t *pEdict )
{
	if ( pEdict == NULL )
		return NULL;

	IServerNetworkable *pNet = pEdict->GetNetworkable();

	if ( pNet == NULL )
		return NULL;

	return pNet->GetEntityHandle();
}

static void AgPB_Trace( const Vector &vStart, const Vector &vEnd, bool bHull,
                        edict_t *pIgnore, trace_t &tr )
{
	Ray_t ray;

	if ( bHull )
		ray.Init( vStart, vEnd, s_vHeadHullMin, s_vHeadHullMax );
	else
		ray.Init( vStart, vEnd );

	CAgPBWaypointTraceFilter filter( AgPB_EdictHandleEntity( pIgnore ) );

	enginetrace->TraceRay( ray, MASK_PLAYERSOLID, &filter, &tr );
}

bool AgPB_TraceClear( const Vector &vStart, const Vector &vEnd, edict_t *pIgnore )
{
	if ( enginetrace == NULL )
		return true;

	trace_t tr;
	AgPB_Trace( vStart, vEnd, false, pIgnore, tr );

	return ( tr.fraction >= 1.0f );
}

bool AgPB_TraceHullClear( const Vector &vStart, const Vector &vEnd, edict_t *pIgnore )
{
	if ( enginetrace == NULL )
		return true;

	trace_t tr;
	AgPB_Trace( vStart, vEnd, true, pIgnore, tr );

	return ( tr.fraction >= 1.0f );
}

bool AgPB_TraceHitsDoor( const Vector &vStart, const Vector &vEnd, edict_t *pIgnore )
{
	if ( enginetrace == NULL || gameents == NULL )
		return false;

	trace_t tr;
	AgPB_Trace( vStart, vEnd, false, pIgnore, tr );

	if ( tr.m_pEnt == NULL )
		return false;

	edict_t *pEdict = gameents->BaseEntityToEdict( tr.m_pEnt );

	if ( pEdict == NULL )
		return false;

	const char *pszClass = AgPB_EntityClassName( pEdict );

	if ( pszClass == NULL )
		return false;

	return ( V_stricmp( pszClass, "func_door" ) == 0 ||
	         V_stricmp( pszClass, "func_door_rotating" ) == 0 );
}

/**
 * 自动算 wayzone 半径（移植 EBot Waypoint::CalculateWayzone，waypoint.cpp:1733）。
 */
void CAgPBWaypoints::CalculateWayzone( int iIndex, edict_t *pIgnore )
{
	AgPBPath *pPath = GetMutable( iIndex );

	if ( pPath == NULL )
		return;

	// EBot：这些点不让半径散开
	if ( pPath->flags & ( AgPB_WP_LADDER | AgPB_WP_GOAL | AgPB_WP_CAMP |
	                      AgPB_WP_RESCUE | AgPB_WP_CROUCH ) )
	{
		pPath->radius = 0;
		return;
	}

	// 邻点带 LADDER / JUMP → 也不散开
	for ( int s = 0; s < AgPB_WP_MAX_PATH_INDEX; ++s )
	{
		const AgPBPath *pLink = Get( pPath->index[s] );

		if ( pLink == NULL )
			continue;

		if ( pLink->flags & ( AgPB_WP_LADDER | AgPB_WP_JUMP ) )
		{
			pPath->radius = 0;
			return;
		}
	}

	bool bBlocked = false;
	int  iFinalRadius = 0;

	for ( int iScan = 32; iScan < 128; iScan += 16 )
	{
		const float flScan = (float)iScan;

		iFinalRadius = iScan;

		for ( int iYaw = 0; iYaw < 360; iYaw += 20 )
		{
			const QAngle angDir( 0.0f, (float)iYaw, 0.0f );
			Vector vDir;
			AngleVectors( angDir, &vDir );

			const Vector vSide = pPath->origin + vDir * flScan;
			const Vector vBack = pPath->origin - vDir * flScan;

			// 1) 这个位置站得下吗（零长度 hull = 把体积放进去试）
			if ( !AgPB_TraceHullClear( vSide, vSide, pIgnore ) )
			{
				// EBot 撞到门就直接给 0（门会动，半径算不准）。它那边用的是
				// 零长度 trace，Source 下拿不到命中实体，所以这里顺着
				// "原点到采样点"的实际连线找是谁挡的。
				if ( AgPB_TraceHitsDoor( pPath->origin, vSide, pIgnore ) )
					iFinalRadius = 0;
				else
					iFinalRadius -= 16;

				bBlocked = true;
				break;
			}

			// 2) 前方采样点往下探（scan + 60）得有地面
			if ( !AgPB_TraceClear( vSide, vSide - Vector( 0.0f, 0.0f, flScan + 60.0f ), pIgnore ) )
			{
				iFinalRadius -= 16;
				bBlocked = true;
				break;
			}

			// 3) 反方向也一样
			if ( !AgPB_TraceClear( vBack, vBack - Vector( 0.0f, 0.0f, flScan + 60.0f ), pIgnore ) )
			{
				iFinalRadius -= 16;
				bBlocked = true;
				break;
			}

			// 4) 头顶 +34 得有空间（矮天花板/管道会挡）
			if ( !AgPB_TraceHullClear( vSide, vSide + Vector( 0.0f, 0.0f, 34.0f ), pIgnore ) )
			{
				iFinalRadius -= 16;
				bBlocked = true;
				break;
			}
		}

		if ( bBlocked )
			break;
	}

	iFinalRadius -= 16;

	if ( iFinalRadius < 0 )
		iFinalRadius = 0;

	if ( iFinalRadius > 255 )
		iFinalRadius = 255;

	pPath->radius = (unsigned char)iFinalRadius;
}
