/**
 * AgPB - 网络字段（netvar）反射层。
 *
 * 数据全部来自引擎自己构建的 SendTable。整条链路都是编译器解析的
 * 虚调用 / 成员访问，**没有任何硬编码的 vtable 索引，也不需要特征码扫描**：
 *
 *   edict_t*
 *     -> edict->GetNetworkable()   // edict.h:174，直接读成员 m_pUnk->GetNetworkable()
 *     -> IServerNetworkable*
 *     -> pNet->GetServerClass()    // iservernetworkable.h:94，编译器解析的虚调用
 *     -> ServerClass*              // server_class.h
 *     -> pClass->m_pTable          // 数据成员
 *     -> SendTable*                // dt_send.h:438
 *     -> pTable->m_pProps[i]       // SendProp，dt_send.h:186
 *
 * 每个 SendProp 都带 m_Offset（相对实体对象基址的字节偏移）与类型，
 * 所以拿到名字就能直接读写字段。
 *
 * ---------------------------------------------------------------------------
 * 关于递归（dt_send.h:553 是决定性证据）
 * ---------------------------------------------------------------------------
 *
 *   SendPropDataTable( "baseclass", 0, className::BaseClass::m_pClassSendTable, SendProxy_DataTableToDataTable ),
 *
 * 基类表以 "baseclass" + offset 0 挂在派生表里，因此统一用
 * `offset += pProp->GetOffset()` 递归即可：基类加 0，嵌套成员加真实偏移。
 *
 * 典型嵌套子表名与属性名不同，例如
 *   SendPropDataTable( SENDINFO_DT(m_HL2Local), &DT_HL2Local )   // 属性 "m_HL2Local" / 表 "DT_HL2Local"
 *   SendPropDataTable( "localdata", 0, &DT_LocalPlayerExclusive ) // 属性 "localdata"  / 表 "DT_LocalPlayerExclusive"
 *
 * ---------------------------------------------------------------------------
 * 关于数组：Source 有**两套完全不同的数组机制**，必须分别处理
 * ---------------------------------------------------------------------------
 *
 * A) `SendPropArray` / `SendPropVariableLengthArray`（例如 m_hViewModel）
 *    宏展开成两个连续属性：
 *        元素模板 SendPropXxx(...),          // 下标 i-1，名字与数组同名
 *        InternalSendPropArray(...)          // 下标 i，m_Type = DPT_Array
 *    引擎在 SetupArrayProps_R（engine/dt.h:501）里给**元素模板**打上
 *    SPROP_INSIDEARRAY，并把数组属性的 m_pArrayProp 指回元素模板。
 *
 *    -> 元素模板必须**跳过**（它的名字和数组一样，不跳过会导致重复条目，
 *       而且 Find() 会先撞上元素，拿到的类型是标量而不是 DPT_Array）。
 *    -> 数组属性自己带 m_nElements / m_ElementStride，直接用。
 *
 * B) `SendPropArray3`（例如 m_iAmmo）
 *    public/dt_send.cpp:691 里**不是** DPT_Array，而是：
 *        ret.m_Type = DPT_DataTable;
 *        ret.m_pVarName = pVarName;                  // "m_iAmmo"
 *        pProps[i].SetOffset( i*sizeofVar );         // 相对数组起点的偏移
 *        pProps[i].m_pVarName = s_ElementNames[i];   // "000".."031"
 *        pProps[i].m_pParentArrayPropName = pVarName;
 *        ret.SetDataTable( new SendTable( pProps, elements, pVarName ) );
 *
 *    -> 天真地按子表递归会吐出 32 个叫 "000".."031" 的匿名标量，而数组名丢失。
 *    -> 必须识别出这种"合成数组表"并**折叠成一个 DPT_Array 条目**：
 *       判据 = 子表名 == 属性名（真实 DT_* 表名永远带 "DT_" 前缀，不会撞）
 *             且子表第一个元素带 m_pParentArrayPropName。
 *       步长 = 元素[1].offset - 元素[0].offset。
 *
 * ---------------------------------------------------------------------------
 * 关于负偏移（engine/dt_send_eng.cpp:732）
 * ---------------------------------------------------------------------------
 *
 * SENDINFO_VECTORELEM 传的是**负偏移**（dt_send.h:597），引擎用它标记
 * SENDPROP_VECTORELEM，并在 SendTable_CalcNextVectorElems() 里取绝对值修正。
 * 插件在游戏 DLL 初始化之后才加载，此时已经修正，但这里仍然做一次防御性 abs()。
 *
 * 限制：SendTable 只覆盖**网络字段**。非网络字段需要走 datamap 路径
 * （GetDataDescMap），那条路要 vtable 索引，留待后续。
 */

#ifndef _INCLUDE_AGPB_NETVARS_H_
#define _INCLUDE_AGPB_NETVARS_H_

#include <eiface.h>
#include <edict.h>
#include <interface.h>
#include <basetypes.h>
#include <const.h>
#include <mathlib/vector.h>
#include <utlvector.h>
#include <dt_common.h>
#include <dt_send.h>
#include <server_class.h>

/**
 * 一条网络字段的描述。
 * name 指向引擎内部的常量字符串，生命周期与模块相同，不需要拷贝。
 */
struct BotNetVar
{
	const char  *name;
	int          offset;      // 相对实体对象基址
	SendPropType type;        // DPT_* ；数组为 DPT_Array
	SendPropType elementType; // 数组元素类型；非数组时等于 type
	int          elements;    // 元素个数
	int          stride;      // 元素间距（不能假设等于 sizeof(int)）
	const char  *parentArray; // 保留：SendPropArray3 的合成元素会带，折叠后为 NULL
	const SendProp *pProp;    // 引擎的属性对象；int 取值靠它自己的 proxy
};

/**
 * 某个 ServerClass 的扁平化字段表。
 */
class CNetVarTable
{
public:
	CNetVarTable();

	bool Build( ServerClass *pClass );

	int Count() const { return m_Props.Count(); }
	const BotNetVar &Prop( int index ) const { return m_Props[index]; }

	/** 按名字查找；找不到返回 NULL。 */
	const BotNetVar *Find( const char *name ) const;

	const char *ClassName() const { return m_ClassName; }
	ServerClass *ServerClassPtr() const { return m_pClass; }

private:
	void Walk( SendTable *pTable, int baseOffset );

	/** 标量字段。 */
	void AddProp( const SendProp *pProp, int baseOffset );

	/**
	 * 数组字段（两套机制折叠后都走这里，offset 已是绝对偏移）。
	 * pElement 是元素模板（DPT_Array 时是下标 i-1 的那条；
	 * SendPropArray3 时是合成子表的第一个元素），它的 proxy 才能正确地读元素。
	 */
	void AddArray( const char *name, int offset, SendPropType elementType, int elements, int stride,
	               const SendProp *pElement );

	/** 是否为 SendPropArray3 生成的“合成数组表”。 */
	static bool IsSyntheticArrayTable( const SendProp *pProp, SendTable *pTable );

	CUtlVector<BotNetVar> m_Props;
	const char  *m_ClassName;
	ServerClass *m_pClass;
};

/**
 * 按 ServerClass 缓存的字段表注册表。
 * SendTable 是引擎启动时静态构建的，所以缓存一次即可，不需要失效逻辑。
 */
class CNetVarRegistry
{
public:
	CNetVarRegistry();

	/** 从 edict 一路走到字段表；失败返回 NULL。 */
	const CNetVarTable *GetForEdict( edict_t *pEdict );

	const CNetVarTable *GetForClass( ServerClass *pClass );

	int TableCount() const { return m_Tables.Count(); }
	const CNetVarTable *TableAt( int index ) const { return m_Tables[index]; }

	void Clear();

private:
	CUtlVector<CNetVarTable *> m_Tables;
};

// ---------------------------------------------------------------------------
// 取值辅助
//
// 关于 int 字段的宽度：**SendTable 里拿不到**。
// SetElementStride 在整个 dt_send.cpp 里一次都没被调用，标量的
// m_ElementStride 永远是 SIZEOF_IGNORE(-1)；而 SendPropInt 是根据
// sizeofVar 选 proxy 的（1 -> SendProxy_Int8ToInt32，2 -> Int16，4 -> Int32）。
//
// 所以 int 必须走**引擎自己的 proxy**：那正是引擎编码时走的同一条路，
// 宽度 / 符号 / 代理计算全部自然正确。
// 反例：m_lifeState 只有 1 字节（c_baseentity.h:1353 是 char），
// 按 4 字节读会得到 512（真实值 0 = LIFE_ALIVE，剩下的 0x200 是邻字段）。
//
// float / vec3 宽度无歧义（4 / 12 字节），直接内存读更简单也更安全
// （避开 SendProxy_Origin 之类会改数值的代理）。
// ---------------------------------------------------------------------------

/**
 * 调用引擎的 proxy 读一个元素。
 * pData 用 nv.offset（Walk 已算成绝对偏移）而不是 pProp->GetOffset()：
 * SendPropArray3 的合成元素偏移是相对数组起点的（0, 4, 8...），不能直接用。
 */
inline void NetVar_CallProxy( const BotNetVar &nv, const void *pBase, int iElement, int objectID, DVariant *pOut )
{
	if ( nv.pProp == NULL )
		return;

	SendVarProxyFn fn = nv.pProp->GetProxyFn();
	if ( fn == NULL )
		return;

	fn( nv.pProp, pBase,
	    (const char *)pBase + nv.offset + nv.stride * iElement,
	    pOut, iElement, objectID );
}

inline int NetVar_GetInt( const void *pBase, const BotNetVar &nv )
{
	if ( nv.pProp == NULL )
		return *(const int *)( (const char *)pBase + nv.offset );

	DVariant out;
	out.m_Int = 0;
	NetVar_CallProxy( nv, pBase, 0, 0, &out );
	return out.m_Int;
}

inline bool NetVar_GetBool( const void *pBase, const BotNetVar &nv )
{
	return ( *(const int *)( (const char *)pBase + nv.offset ) != 0 );
}

inline float NetVar_GetFloat( const void *pBase, const BotNetVar &nv )
{
	return *(const float *)( (const char *)pBase + nv.offset );
}

inline Vector NetVar_GetVector( const void *pBase, const BotNetVar &nv )
{
	return *(const Vector *)( (const char *)pBase + nv.offset );
}

/** 指向引擎内部常量串或实体内的 char[]，调用方自己判断（不要盲目解引用）。 */
inline const char *NetVar_GetString( const void *pBase, const BotNetVar &nv )
{
	return (const char *)( (const char *)pBase + nv.offset );
}

/** 读数组元素：stride 由 SendTable 给出，不能假设是 sizeof(int)。 */
inline int NetVar_GetArrayInt( const void *pBase, const BotNetVar &nv, int index )
{
	if ( nv.pProp == NULL )
		return *(const int *)( (const char *)pBase + nv.offset + nv.stride * index );

	DVariant out;
	out.m_Int = 0;
	NetVar_CallProxy( nv, pBase, index, 0, &out );
	return out.m_Int;
}

inline float NetVar_GetArrayFloat( const void *pBase, const BotNetVar &nv, int index )
{
	if ( nv.pProp == NULL )
		return *(const float *)( (const char *)pBase + nv.offset + nv.stride * index );

	DVariant out;
	out.m_Float = 0.0f;
	NetVar_CallProxy( nv, pBase, index, 0, &out );
	return out.m_Float;
}

// ---------------------------------------------------------------------------
// EHANDLE（CBaseHandle）
//
// 重要：这个 x64 移植版里 CBaseHandle::m_Index 是 **uintp**（basehandle.h:66），
// 即指针宽度 = 8 字节，而不是 4。读 EHandle 字段必须读 8 字节。
//
// 打包规则（basehandle.h:98）：
//     m_Index = entry | (serial << NUM_ENT_ENTRY_BITS)
// NUM_ENT_ENTRY_BITS = MAX_EDICT_BITS + 1 = 12（const.h:68 / const.h:78），
// 即 entry 占低 12 位、ENT_ENTRY_MASK = 0xFFF。
//
// 实例：m_hViewModel 的两个元素读出来是 0x068AB05F / 0x0639605E，
// 解包后 entry = 95 / 94 —— 两个连续的实体索引，正是两个 viewmodel。
// ---------------------------------------------------------------------------

/** 读 8 字节句柄。仅当该字段确实是 8 字节存储时才有效。 */
inline uintp NetVar_GetHandle( const void *pBase, const BotNetVar &nv )
{
	return *(const uintp *)( (const char *)pBase + nv.offset );
}

inline uintp NetVar_GetArrayHandle( const void *pBase, const BotNetVar &nv, int index )
{
	return *(const uintp *)( (const char *)pBase + nv.offset + nv.stride * index );
}

inline int  NetVar_HandleEntry( uintp h ) { return (int)( h & ENT_ENTRY_MASK ); }
inline int  NetVar_HandleSerial( uintp h ) { return (int)( h >> NUM_ENT_ENTRY_BITS ); }
inline bool NetVar_HandleIsValid( uintp h ) { return h != (uintp)INVALID_EHANDLE_INDEX; }

/** 该字段是否看起来是 8 字节存储（仅供参考，不要当硬判据）。 */
inline bool NetVar_IsHandleSized( const BotNetVar &nv )
{
	return nv.type == DPT_Int && nv.stride >= (int)sizeof( uintp );
}

// ---------------------------------------------------------------------------
// 写字段
//
// **只在确认过内存宽度时用** —— 网络表示 != 内存表示：
//   - 1 字节成员（`m_lifeState` 是 char）按 4 字节写会踩到后面三个字段；
//   - 发送时被位压缩的 int（`m_fFlags` 走 SendProxy_CropFlags、`m_iAmmo` 之类）
//     内存宽度也未必是 4，但**多数 CBaseEntity/CBasePlayer 成员仍是 4 字节 int**；
//   - float 类（`m_flMaxspeed`）、Vector / VectorXY 的三个 float、真数组元素
//     （stride >= 4）都可以安全按 4 字节写。
//
// 已知可安全写的（本项目用到）：`m_vecVelocity[0..2]`、`m_flMaxspeed`、`m_fFlags`、
// `m_nButtons`。要写别的字段前先 `agpb_netlist` 核对类型与 stride。
// ---------------------------------------------------------------------------

/** 按 float 写（4 字节）。 */
inline void NetVar_WriteFloat( void *pBase, const BotNetVar &nv, float flValue )
{
	*(float *)( (char *)pBase + nv.offset ) = flValue;
}

/** 按 int 写（4 字节）。 */
inline void NetVar_WriteInt( void *pBase, const BotNetVar &nv, int iValue )
{
	*(int *)( (char *)pBase + nv.offset ) = iValue;
}

/**
 * 按 field 的 stride 决定读取宽度：x64 的 CBaseHandle 是 8 字节，
 * 但早期 32 位构建是 4 字节。
 *
 * 注意：**不要用 stride 做硬判据**。数组的 stride 由 InternalSendPropArray
 * 显式写入，而标量的 m_ElementStride 未必被填。
 * 真正可靠的判据是语义校验（能不能解成实体 + serial 对不对）。
 */
inline uintp NetVar_ReadHandleRaw( const void *pBase, const BotNetVar &nv )
{
	const char *p = (const char *)pBase + nv.offset;

	if ( nv.stride >= (int)sizeof( uintp ) )
		return *(const uintp *)p;

	return (uintp)( *(const uint32 *)p );
}

#endif // _INCLUDE_AGPB_NETVARS_H_
