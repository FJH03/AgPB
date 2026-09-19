/**
 * AgPB - 路点（waypoint）系统，移植自 EBot 的 Waypoint 类。
 *
 * 为什么换成路点：.nav 描述的是"哪块地面能站"，"怎么从 A 到 B"要靠几何去推，
 * 推错了就会出现"nav 说连通、物理上过不去"的连接（实测 cs_office 上有相邻
 * area 中心差 85~108 单位、却没标 NAV_MESH_JUMP 的连接）。
 * 路点图的每一条连边是**人验证过的动作指令**，不存在推导这一步。
 *
 * 数据模型 1:1 照搬 EBot（refs/CS-EBOT/include/core.h）：
 *   core.h:267   Const_MaxPathIndex = 8     每个路点最多 8 条连边（定长数组！）
 *   core.h:502   struct Path                路点本体
 *   core.h:183   enum WaypointFlag          WAYPOINT_* 路点标志
 *   core.h:213   enum PathFlag              PATHFLAG_* 连边标志
 *   core.h:496   struct WaypointHeader      存盘文件头
 *
 * 寻路照搬 EBot source/navigate.cpp:1534 RunAsyncAStar（去掉线程/异步那层壳）。
 *
 * 存盘用 tier1 的 CUtlBuffer + IFileSystem：
 *   字段逐个 Put / Get，不直接落结构体 —— 省得以后改结构把老文件全读坏。
 *   文件放在 addons/AgPB/waypoints/<map>.agpw。
 */

#ifndef _INCLUDE_AGPB_WAYPOINT_H_
#define _INCLUDE_AGPB_WAYPOINT_H_

#include <mathlib/vector.h>
#include <utlvector.h>

class IFileSystem;

// 照 EBot core.h:265-272
#define AgPB_WP_MAGIC            0x50424741u   // 'AGPB'，小端写入
#define AgPB_WP_VERSION          1
#define AgPB_WP_MAX_PATH_INDEX   8             // EBot Const_MaxPathIndex
#define AgPB_WP_MAX              8192          // EBot Const_MaxWaypoints

// 路径距离里的"不可达"。EBot 那边是 int16 饱和值 32766（waypoint.cpp:2007 INF），
// 因为要压进 N×N 的 int16 矩阵；我们只算单源，直接用 float 大值。
#define AgPB_WP_DIST_INF         999999.0f

// 路点标志，照 EBot core.h:183-209 WaypointFlag（只留我们用得上的）
#define AgPB_WP_LIFT          ( 1u << 1 )    // 等电梯降落再靠近
#define AgPB_WP_CROUCH        ( 1u << 2 )    // 必须蹲着才能到
#define AgPB_WP_CROSSING      ( 1u << 3 )    // 穿越点（目标是路点本身）
#define AgPB_WP_GOAL          ( 1u << 4 )    // 任务目标点（炸弹点/人质点）
#define AgPB_WP_LADDER        ( 1u << 5 )    // 这个路点在梯子上
#define AgPB_WP_RESCUE        ( 1u << 6 )    // 人质救援点
#define AgPB_WP_CAMP          ( 1u << 7 )    // 蹲守点
#define AgPB_WP_DJUMP         ( 1u << 9 )    // 连跳点
#define AgPB_WP_AVOID         ( 1u << 11 )   // 尽量避开
#define AgPB_WP_USEBUTTON     ( 1u << 12 )   // 需要按按钮
#define AgPB_WP_FALLRISK      ( 1u << 17 )   // 在这个点上不要侧移
#define AgPB_WP_FALLCHECK     ( 1u << 26 )   // 需要检查脚下
#define AgPB_WP_JUMP          ( 1u << 27 )   // 跳跃点
#define AgPB_WP_TERRORIST     ( 1u << 29 )   // T 专用
#define AgPB_WP_COUNTER       ( 1u << 30 )   // CT 专用

// 连边标志，照 EBot core.h:213-219 PathFlag
#define AgPB_PATH_JUMP        ( 1u << 0 )    // 这条连接必须跳
#define AgPB_PATH_DOUBLE      ( 1u << 1 )    // 需要队友叠罗汉（连跳）
#define AgPB_PATH_VISIBLE     ( 1u << 2 )    // 只保证通视，不一定能走

/** 一个路点，照 EBot core.h:502-514 struct Path。纯 POD，可以直接放进 CUtlVector。 */
struct AgPBPath
{
	Vector          origin;
	unsigned int    flags;
	unsigned char   radius;          // 到达判定半径
	unsigned char   mesh;            // 所属分组（EBot 用来做区域划分）
	short           index[AgPB_WP_MAX_PATH_INDEX];              // 邻居路点下标，-1 = 空槽
	unsigned short  connectionFlags[AgPB_WP_MAX_PATH_INDEX];    // 每个邻居对应的 PATHFLAG_*
	float           gravity;         // 该点的重力，弹道跳要用
};

/**
 * 路点集合。
 *
 * 连接是**双向**的：AddLink(a,b) 会同时写进 a 和 b 的槽位
 * （EBot 的编辑器也是成对调用 AddPath 的）。
 */
class CAgPBWaypoints
{
public:
	CAgPBWaypoints() { Clear(); }

	/** 由 plugin.cpp 在加载时注入（插件链接不到 game DLL 的 filesystem 全局）。 */
	static void SetFileSystem( IFileSystem *pFileSystem );

	/** 由 plugin.cpp 在进图时调用。换图会顺便重载路点文件。 */
	bool SetMapName( const char *pszMapName );

	void Clear();

	int  Count() const { return m_Paths.Count(); }
	bool IsValid( int i ) const { return ( i >= 0 && i < m_Paths.Count() ); }

	const AgPBPath *Get( int i ) const;
	AgPBPath       *GetMutable( int i );

	/** 加一个路点，返回下标；失败返回 -1。不自动连线（手工连更精准）。 */
	int  Add( const Vector &vOrigin, unsigned int flags = 0 );

	/** 删一个路点，并修正其它路点里指向它/大于它的下标。 */
	void Delete( int i );

	/** 最近的 / 最远的路点下标。这是**欧氏**距离，只用来把"人站的位置"吸附到
	 *  路点图上；要按"沿路点图走多远"来挑点，用 ComputeDistances。 */
	int  FindNearest( const Vector &vOrigin, float flMaxDist = 99999.0f ) const;
	int  FindFarthest( const Vector &vOrigin, float flMinDist = 0.0f ) const;

	/** 连边。flags 是 PATHFLAG_* 的组合。已存在则返回 false。 */
	bool AddLink( int iFrom, int iTo, unsigned int flags = 0 );
	bool RemoveLink( int iFrom, int iTo );
	bool IsConnected( int iFrom, int iTo, unsigned int *pFlags = NULL ) const;
	int  LinkCount() const;

	// ------------------------------------------------------------------
	// 寻路（移植 EBot RunAsyncAStar / MatrixWorker）
	// ------------------------------------------------------------------

	/**
	 * A* 寻路，沿路点图的连边找 iStart -> iGoal。
	 * outPath 里**含起点和终点**；找不到就返回 false 并清空 outPath。
	 *
	 * iTeam 用来过滤 AgPB_WP_TERRORIST / AgPB_WP_COUNTER 阵营专用点，
	 * 传 0 表示不过滤（观察者/还没分阵营时）。
	 * iAvoidWaypoint 是"刚放弃的那个点"，路点图里遇到就绕开
	 * （EBot 的 lastDeclineWaypoint，navigate.cpp:1614）。
	 */
	bool FindPath( int iStart, int iGoal, CUtlVector< int > &outPath,
	               int iTeam = 0, int iAvoidWaypoint = -1 );

	/**
	 * 单源最短路径：沿路点图从 iSource 出发到所有点的代价。
	 *
	 * 这是 EBot 距离矩阵的**单源版**（waypoint.cpp:2000 MatrixWorker 的循环体）。
	 * EBot 给每个源都跑一遍拼成 N×N 的 int16 矩阵：
	 * Const_MaxWaypoints = 8192 -> 8192*8192*2B = 128MB，还得开线程慢慢算。
	 * 我们只在真要用的时候算一源，代价 O(边数 log 点数)，几百个点就是微秒级。
	 *
	 * vecDist / vecParent 会被填成 Count() 大小：
	 *   vecDist[i]   = 到 i 的路径代价，不可达是 AgPB_WP_DIST_INF
	 *   vecParent[i] = 最短路上的前驱，-1 = 起点或不可达
	 *
	 * 代价用的是和 A* 同一套（LADDER / CROUCH 会 ×2），不是纯几何长度，
	 * 这样"路径距离"和"实际要走的路"才一致。
	 */
	void ComputeDistances( int iSource, CUtlVector< float > &vecDist,
	                       CUtlVector< short > &vecParent,
	                       int iTeam = 0, int iAvoidWaypoint = -1 );

	/** 沿路点图的实际路径代价；不可达返回 AgPB_WP_DIST_INF。 */
	float PathDistance( int iFrom, int iTo, int iTeam = 0, int iAvoidWaypoint = -1 );

	/** 从文件读。地图名没设置或文件不存在都会返回 false，原因写进 Status()。 */
	bool Load();
	/** 写回文件（目录不存在会自动建）。 */
	bool Save();

	const char *Status() const { return m_szStatus; }
	const char *MapName() const { return m_szMapName; }
	const char *FileName() const { return m_szFile; }

private:
	void SetStatus( const char *pszText );
	void RebuildFile();          // 根据 m_szMapName 拼出 m_szFile

	bool IsWaypointPassable( int i, int iTeam ) const;
	bool GetLinkCost( int iFrom, int iTo, unsigned int uLinkFlags, float &flCost ) const;

	CUtlVector< AgPBPath > m_Paths;

	char m_szMapName[64];
	char m_szFile[192];
	char m_szStatus[192];
};

/** 全局单例：按当前地图缓存一份。 */
CAgPBWaypoints &BotWaypoints();

#endif // _INCLUDE_AGPB_WAYPOINT_H_
