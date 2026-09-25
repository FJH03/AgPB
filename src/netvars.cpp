/**
 * AgPB - netvar 反射层实现。
 *
 * 数组处理的细节见 netvars.h 顶部注释（两套机制 + 引擎源码行号）。
 */

#include <tier1/strtools.h>

#include "netvars.h"

// 单个实体最多能展开多少条字段（防御性上限）
#define AgPB_MAX_NETVARS 2048

// ---------------------------------------------------------------------------
// CNetVarTable
// ---------------------------------------------------------------------------

CNetVarTable::CNetVarTable()
{
	m_ClassName = NULL;
	m_pClass = NULL;
}

bool CNetVarTable::IsSyntheticArrayTable( const SendProp *pProp, SendTable *pTable )
{
	if ( pProp == NULL || pTable == NULL )
		return false;

	if ( pProp->GetName() == NULL || pTable->m_pNetTableName == NULL )
		return false;

	// SendPropArray3 用属性名当子表名（dt_send.cpp:728）。
	// 真实的 DT_* 表名永远带 "DT_" 前缀，不会和变量名相同。
	if ( V_strcmp( pTable->m_pNetTableName, pProp->GetName() ) != 0 )
		return false;

	if ( pTable->GetNumProps() <= 0 || pTable->GetProp( 0 ) == NULL )
		return false;

	// SendPropArray3 会给每个合成元素填 m_pParentArrayPropName（dt_send.cpp:726）；
	// 真实的嵌套子表不会。这是第二道保险。
	return ( pTable->GetProp( 0 )->GetParentArrayPropName() != NULL );
}

void CNetVarTable::AddProp( const SendProp *pProp, int baseOffset )
{
	if ( pProp == NULL || pProp->GetName() == NULL )
		return;

	if ( m_Props.Count() >= AgPB_MAX_NETVARS )
		return;

	int offset = pProp->GetOffset();

	// SENDINFO_VECTORELEM 传负偏移做标记，引擎已取过绝对值，这里再防一次。
	if ( offset < 0 )
		offset = -offset;

	BotNetVar &nv = m_Props[m_Props.AddToTail()];

	nv.name = pProp->GetName();
	nv.offset = baseOffset + offset;
	nv.type = pProp->GetType();
	nv.elementType = pProp->GetType();
	nv.elements = pProp->GetNumElements();
	nv.stride = pProp->GetElementStride();
	nv.parentArray = NULL;
	nv.pProp = pProp;
}

void CNetVarTable::AddArray( const char *name, int offset,
                             SendPropType elementType, int elements, int stride,
                             const SendProp *pElement )
{
	if ( name == NULL )
		return;

	if ( m_Props.Count() >= AgPB_MAX_NETVARS )
		return;

	BotNetVar &nv = m_Props[m_Props.AddToTail()];

	nv.name = name;
	nv.offset = offset;
	nv.type = DPT_Array;
	nv.elementType = elementType;
	nv.elements = elements;
	nv.stride = stride;
	nv.parentArray = NULL;
	nv.pProp = pElement;
}

void CNetVarTable::Walk( SendTable *pTable, int baseOffset )
{
	if ( pTable == NULL || pTable->m_pProps == NULL )
		return;

	const int nProps = pTable->GetNumProps();

	for ( int i = 0; i < nProps; ++i )
	{
		SendProp *pProp = pTable->GetProp( i );
		if ( pProp == NULL )
			continue;

		if ( pProp->IsExcludeProp() )
			continue;

		// 引擎把 SPROP_INSIDEARRAY 打在“数组元素模板”上（engine/dt.h:509），
		// 而元素模板的名字和数组属性**完全一样**（都来自 SENDINFO_ARRAY）。
		// 不跳过就会产生重复条目，且 Find() 先撞上元素、拿到的是标量类型。
		//
		// 注意：SendProp 上永远拿不到 GetParentArrayPropName()（那个只在 dt_recv
		// 里设置），所以判据只能是 IsInsideArray() 本身。
		if ( pProp->IsInsideArray() )
			continue;

		if ( pProp->GetType() == DPT_DataTable )
		{
			SendTable *pChild = pProp->GetDataTable();
			if ( pChild == NULL )
				continue;

			if ( IsSyntheticArrayTable( pProp, pChild ) )
			{
				// SendPropArray3：折叠成一个真数组条目。
				SendProp *pFirst = pChild->GetProp( 0 );
				const int elements = pChild->GetNumProps();

				int stride = 0;
				if ( elements >= 2 )
				{
					SendProp *pSecond = pChild->GetProp( 1 );
					if ( pSecond != NULL )
						stride = pSecond->GetOffset() - pFirst->GetOffset();
				}

				int offset = pProp->GetOffset();
				if ( offset < 0 )
					offset = -offset;

				AddArray( pProp->GetName(), baseOffset + offset,
				          pFirst->GetType(), elements, stride, pFirst );
			}
			else
			{
				// 基类表（"baseclass"，offset 0）或嵌套成员子表，统一累加偏移。
				Walk( pChild, baseOffset + pProp->GetOffset() );
			}
			continue;
		}

		if ( pProp->GetType() == DPT_Array )
		{
			// SendPropArray / SendPropVariableLengthArray：真数组，
			// 元素模板是紧邻的前一个属性（下标 i-1），
			// 由 SetupArrayProps_R（engine/dt.h:508-510）挂到 m_pArrayProp。
			//
			// 关键：InternalSendPropArray（dt_send.cpp:480）**从不设置数组属性自己的
			// m_Offset，它永远是 0**（真实偏移只存在于元素模板上）。
			// 若用数组属性的 offset，会读到 base+0 —— 也就是实体的 vtable 指针。
			const SendProp *pElement = pProp->GetArrayProp();

			if ( pElement == NULL )
				continue;

			int offset = pElement->GetOffset();
			if ( offset < 0 )
				offset = -offset;

			AddArray( pProp->GetName(), baseOffset + offset, pElement->GetType(),
			          pProp->GetNumElements(), pProp->GetElementStride(), pElement );
			continue;
		}

		AddProp( pProp, baseOffset );
	}
}

bool CNetVarTable::Build( ServerClass *pClass )
{
	m_Props.RemoveAll();
	m_ClassName = NULL;
	m_pClass = NULL;

	if ( pClass == NULL || pClass->m_pTable == NULL )
		return false;

	m_pClass = pClass;
	m_ClassName = pClass->GetName();

	// 偏移从实体对象基址开始算，所以最外层传 0。
	Walk( pClass->m_pTable, 0 );

	return ( m_Props.Count() > 0 );
}

const BotNetVar *CNetVarTable::Find( const char *name ) const
{
	if ( name == NULL )
		return NULL;

	for ( int i = 0; i < m_Props.Count(); ++i )
	{
		if ( V_strcmp( m_Props[i].name, name ) == 0 )
			return &m_Props[i];
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// CNetVarRegistry
// ---------------------------------------------------------------------------

CNetVarRegistry::CNetVarRegistry()
{
}

const CNetVarTable *CNetVarRegistry::GetForClass( ServerClass *pClass )
{
	if ( pClass == NULL )
		return NULL;

	for ( int i = 0; i < m_Tables.Count(); ++i )
	{
		if ( m_Tables[i]->ServerClassPtr() == pClass )
			return m_Tables[i];
	}

	CNetVarTable *pTable = new CNetVarTable();
	if ( !pTable->Build( pClass ) )
	{
		delete pTable;
		return NULL;
	}

	m_Tables.AddToTail( pTable );
	return pTable;
}

const CNetVarTable *CNetVarRegistry::GetForEdict( edict_t *pEdict )
{
	if ( pEdict == NULL || pEdict->IsFree() )
		return NULL;

	// edict_t::GetNetworkable() 是纯成员访问（m_pUnk->GetNetworkable()），
	// 之后的 GetServerClass() 是编译器解析的虚调用。
	IServerNetworkable *pNet = pEdict->GetNetworkable();
	if ( pNet == NULL )
		return NULL;

	return GetForClass( pNet->GetServerClass() );
}

void CNetVarRegistry::Clear()
{
	for ( int i = 0; i < m_Tables.Count(); ++i )
		delete m_Tables[i];

	m_Tables.RemoveAll();
}

// ---------------------------------------------------------------------------
// 写入结果的名字（控制台回显用）
// ---------------------------------------------------------------------------

const char *NetVarWriteResultName( NetVarWriteResult result )
{
	switch ( result )
	{
		case NETVAR_WRITE_OK:        return "ok";
		case NETVAR_WRITE_NO_BASE:   return "no entity base pointer";
		case NETVAR_WRITE_NOT_FOUND: return "field not found in this entity's SendTable";
		case NETVAR_WRITE_TYPE:      return "field type mismatch";
		case NETVAR_WRITE_ELEMENT:   return "bad element index (not an array, or out of range)";
		case NETVAR_WRITE_VECTORXY:  return "VectorXY holds only x/y, refusing to write 3 floats";
	}

	return "unknown";
}
