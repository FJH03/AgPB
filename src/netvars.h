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
// 写字段（2026-09-25 重写）
//
// 纪律：网络表示 != 内存表示，所以**写入宽度问引擎要，不猜**。
// 口径照抄 SourceMod（core/smn_entities.cpp，SetEntProp 的 int 分支）：
//
//   pProp->m_nBits >= 17 -> 4 字节；>= 9 -> 2 字节；>= 2 -> 1 字节；否则 1 字节 bool
//   同一个文件 :1706 的 SPROP_VARINT 特例 -> 恒按 4 字节
//   同一个文件 :1837（SetEntPropFloat） -> float 恒 4 字节
//
// 为什么这套能对上内存 —— CS:S 自己的声明（game/server/player.cpp）：
//   SendPropInt( SENDINFO(m_iHealth), -1, SPROP_VARINT|SPROP_CHANGES_OFTEN )  :7997 -> 写 4 字节 ✓（它是 int）
//   SendPropInt( SENDINFO(m_lifeState), 3, SPROP_UNSIGNED )                   :7998 -> 写 1 字节 ✓（它是 char）
//   SendPropInt( SENDINFO(m_fFlags), PLAYER_FLAG_BITS, SPROP_UNSIGNED )       :8002 -> 写 1~2 字节（高位字节本来就是 0）
//
// 另外两条同样来自 SourceMod 的纪律：
//   1. 写之前校验类型与下标（它那边是 ThrowNativeError，我们这边是错误码）；
//   2. **写完通知引擎**（HalfLife2.cpp:531 SetEdictStateChanged）—— 不通知的话，
//      引擎手里那份「这个字段没变」的记录会让它继续用旧值。
//      我们 SDK 里就是 CBaseEdict::StateChanged(offset)（edict.h:286），它内部会
//      解引用 g_pSharedChangeInfo（edict.h:295），所以调用前必须先判空。
// ---------------------------------------------------------------------------

/** 写入结果；失败原因要在控制台里有回显，不然调试时只能猜。 */
enum NetVarWriteResult
{
	NETVAR_WRITE_OK = 0,
	NETVAR_WRITE_NO_BASE,   // 实体基址为空（实体无效 / 没有 CBaseEntity）
	NETVAR_WRITE_NOT_FOUND, // 字段不存在（名字写错，或该字段不在网络表里）
	NETVAR_WRITE_TYPE,      // 字段类型不是要写的那种（例如往 float 字段写 int）
	NETVAR_WRITE_ELEMENT,   // 非数组却给了下标 / 下标越界 / stride 不合法
	NETVAR_WRITE_VECTORXY,  // VectorXY 只有两个 float，不能按 Vector 写
};

const char *NetVarWriteResultName( NetVarWriteResult result );

/** 由网络位宽推出"内存里按几字节写"：1 / 2 / 4。 */
inline int NetVar_WriteWidth( const BotNetVar &nv )
{
	if ( nv.pProp == NULL )
		return 4;

	// SPROP_VARINT：网络上是变长编码，内存里仍是完整 int（smn_entities.cpp:1706 同样处理）
	if ( ( nv.pProp->GetFlags() & SPROP_VARINT ) != 0 )
		return 4;

	const int iBits = nv.pProp->m_nBits;

	if ( iBits >= 17 )
		return 4;
	if ( iBits >= 9 )
		return 2;
	if ( iBits >= 1 )
		return 1;      // 1 位 bool 在内存里也是 1 字节

	return 4;          // 引擎没给位宽 -> 按 4 字节（SourceMod 的默认口径）
}

inline bool NetVar_CanWriteInt( const BotNetVar &nv )
{
	return ( nv.type == DPT_Int ) || ( nv.type == DPT_Array && nv.elementType == DPT_Int );
}

inline bool NetVar_CanWriteFloat( const BotNetVar &nv )
{
	return ( nv.type == DPT_Float ) || ( nv.type == DPT_Array && nv.elementType == DPT_Float );
}

/**
 * 取写入地址：标量只接受 element 0；数组要 stride > 0 且 0 <= element < elements。
 * 失败返回 NULL。
 */
inline char *NetVar_WriteAddress( void *pBase, const BotNetVar &nv, int iElement )
{
	if ( pBase == NULL )
		return NULL;

	char *p = (char *)pBase + nv.offset;

	if ( nv.type == DPT_Array )
	{
		if ( nv.stride <= 0 || iElement < 0 || iElement >= nv.elements )
			return NULL;

		return p + nv.stride * iElement;
	}

	return ( iElement == 0 ) ? p : NULL;
}

/** int 写入：宽度由 pProp->m_nBits 决定（1 / 2 / 4 字节）。 */
inline NetVarWriteResult NetVar_WriteInt( void *pBase, const BotNetVar &nv, int iValue, int iElement = 0 )
{
	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( !NetVar_CanWriteInt( nv ) )
		return NETVAR_WRITE_TYPE;

	char *p = NetVar_WriteAddress( pBase, nv, iElement );
	if ( p == NULL )
		return NETVAR_WRITE_ELEMENT;

	switch ( NetVar_WriteWidth( nv ) )
	{
		case 2:  *(int16 *)p = (int16)iValue; break;
		case 1:  *(int8 *)p  = (int8)iValue;  break;
		default: *(int32 *)p = (int32)iValue; break;
	}

	return NETVAR_WRITE_OK;
}

/** float 写入：恒 4 字节（smn_entities.cpp:1837 同样的处理）。 */
inline NetVarWriteResult NetVar_WriteFloat( void *pBase, const BotNetVar &nv, float flValue, int iElement = 0 )
{
	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( !NetVar_CanWriteFloat( nv ) )
		return NETVAR_WRITE_TYPE;

	char *p = NetVar_WriteAddress( pBase, nv, iElement );
	if ( p == NULL )
		return NETVAR_WRITE_ELEMENT;

	*(float *)p = flValue;

	return NETVAR_WRITE_OK;
}

/**
 * 整条 Vector 写入（内存里就是 3 个连续 float）。
 * VectorXY 只有 x/y 两个 float，按 Vector 写会踩到下一个字段，所以直接拒绝。
 */
inline NetVarWriteResult NetVar_WriteVector( void *pBase, const BotNetVar &nv, const Vector &vValue )
{
	if ( pBase == NULL )
		return NETVAR_WRITE_NO_BASE;

	if ( nv.type == DPT_VectorXY )
		return NETVAR_WRITE_VECTORXY;

	if ( nv.type != DPT_Vector )
		return NETVAR_WRITE_TYPE;

	*(Vector *)( (char *)pBase + nv.offset ) = vValue;

	return NETVAR_WRITE_OK;
}

/**
 * 告诉引擎"这个实体有字段被我改过了"。
 *
 * 等价于 SourceMod 的 SetEdictStateChanged（HalfLife2.cpp:531），但只能用它的
 * **兜底分支**：那边的首选路径 `pEdict->StateChanged(offset)`（逐字段记账）内部要
 *   ① 解引用 g_pSharedChangeInfo，② 调 CBaseEdict::GetChangeAccessor()
 * 这两个都是 server.dll 里的实现 —— 插件链接不到它们（实测就是
 * `LNK2019: 无法解析的外部符号 g_pSharedChangeInfo / CBaseEdict::GetChangeAccessor`）。
 * 要逐字段记账就得动态解析引擎符号或链接 server.dll，两条都违反本项目
 * "只依赖公开接口、不链接 server.dll"的原则，所以不做。
 *
 * 这里用的 `m_fStateFlags |= FL_EDICT_CHANGED` 正是 SourceMod 在
 * g_pSharedChangeInfo == NULL 时走的那一条（HalfLife2.cpp:548），
 * 纯 inline，无外部依赖。代价是粒度粗一点（引擎按"这个实体变了"处理，
 * 而不是"这个实体的这个字段变了"）；对一个 bot 来说可以忽略。
 *
 * offset 保留在签名里：命令回显要用它说明"改的是哪个偏移"，将来若真需要
 * 逐字段版，再在这里换成动态解析的 accessor 实现。
 */
inline void NetVar_NotifyChanged( edict_t *pEdict, int offset )
{
	(void)offset;

	if ( pEdict == NULL || pEdict->IsFree() )
		return;

	pEdict->m_fStateFlags |= FL_EDICT_CHANGED;
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
