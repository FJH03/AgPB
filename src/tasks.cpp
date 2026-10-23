/*
 * AgPB task scheduling, adapted from the SyPB task-stack design.
 *
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 *
 * The SyPB source/basecode.cpp and include/core.h permission notice:
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * CS-EBOT's source/ssm/ssm.cpp was cross-checked: its remembered process
 * retains an absolute deadline. This implementation keeps SyPB's desire
 * arbitration and allows more than one suspended normal-mode task. AgPB's
 * agent authority is a separate goal arbitration layer; local execution
 * children do not acquire authority over their owning goal.
 * AgPB adaptation: GPL-3.0-only.
 */
#include "tasks.h"

#include <algorithm>
#include <cmath>

namespace
{
AgPBTask Normalize( const AgPBTask &request )
{
	AgPBTask task = request;
	task.token = 0;
	if ( task.id == AgPBTaskId::Normal )
	{
		task.desire = AgPBTaskPriority::Normal;
		task.canResume = true;
		task.deadline = 0.0f;
	}
	if ( !task.hasEntity )
		task.entityHandle = 0;
	if ( !task.hasPosition )
		task.position[0] = task.position[1] = task.position[2] = 0.0f;
	return task;
}

bool SamePayload( const AgPBTask &a, const AgPBTask &b )
{
	return a.id == b.id && a.desire == b.desire &&
		a.waypoint == b.waypoint &&
		a.entityHandle == b.entityHandle && a.hasEntity == b.hasEntity &&
		a.position[0] == b.position[0] &&
		a.position[1] == b.position[1] &&
		a.position[2] == b.position[2] &&
		a.hasPosition == b.hasPosition && a.deadline == b.deadline &&
		a.canResume == b.canResume && a.parentToken == b.parentToken;
}
}

AgPBTask::AgPBTask()
	: id( AgPBTaskId::Normal ),
	  desire( AgPBTaskPriority::Normal ),
	  waypoint( -1 ),
	  entityHandle( 0 ),
	  hasEntity( false ),
	  position{ 0.0f, 0.0f, 0.0f },
	  hasPosition( false ),
	  deadline( 0.0f ),
	  canResume( true ),
	  token( 0 ),
	  parentToken( 0 )
{
}

const char *AgPBTaskName( AgPBTaskId id )
{
	switch ( id )
	{
	case AgPBTaskId::Normal: return "normal";
	case AgPBTaskId::MoveToPosition: return "move_to_position";
	case AgPBTaskId::Investigate: return "investigate";
	case AgPBTaskId::Camp: return "camp";
	case AgPBTaskId::Pause: return "pause";
	case AgPBTaskId::DestroyBreakable: return "destroy_breakable";
	case AgPBTaskId::PlantBomb: return "plant_bomb";
	case AgPBTaskId::DefuseBomb: return "defuse_bomb";
	case AgPBTaskId::PickupBomb: return "pickup_bomb";
	case AgPBTaskId::RescueHostage: return "rescue_hostage";
	case AgPBTaskId::EscortHostages: return "escort_hostages";
	case AgPBTaskId::EscapeBomb: return "escape_bomb";
	case AgPBTaskId::AgentIntention: return "agent_intention";
	}
	return "unknown";
}

AgPBTaskStack::AgPBTaskStack()
	: m_currentIndex( 0 ), m_activeToken( 0 ), m_nextToken( 0 )
{
	Reset();
}

AgPBTask &AgPBTaskStack::Current()
{
	return m_tasks[m_currentIndex];
}

const AgPBTask &AgPBTaskStack::Current() const
{
	return m_tasks[m_currentIndex];
}

int AgPBTaskStack::Count() const
{
	return static_cast<int>( m_tasks.size() );
}

const AgPBTask *AgPBTaskStack::At( int index ) const
{
	if ( index < 0 || static_cast<std::size_t>( index ) >= m_tasks.size() )
		return nullptr;
	return &m_tasks[static_cast<std::size_t>( index )];
}

const AgPBTask *AgPBTaskStack::Find( AgPBTaskId id ) const
{
	for ( const AgPBTask &task : m_tasks )
	{
		if ( task.id == id )
			return &task;
	}
	return nullptr;
}

bool AgPBTaskStack::Contains( AgPBTaskId id ) const
{
	return Find( id ) != nullptr;
}

AgPBTaskState AgPBTaskStack::State( std::uint64_t token ) const
{
	if ( token == 0 )
		return AgPBTaskState::Missing;
	for ( const AgPBTask &task : m_tasks )
	{
		if ( task.token == token )
			return token == m_activeToken ? AgPBTaskState::Active :
				AgPBTaskState::Suspended;
	}
	return AgPBTaskState::Missing;
}

std::uint64_t AgPBTaskStack::NextToken()
{
	++m_nextToken;
	if ( m_nextToken == 0 )
		++m_nextToken;
	return m_nextToken;
}

bool AgPBTaskStack::HasValidParent( const AgPBTask &task ) const
{
	if ( task.parentToken == 0 )
		return true;
	if ( task.id != AgPBTaskId::DestroyBreakable )
		return false;
	for ( const AgPBTask &parent : m_tasks )
	{
		if ( parent.token == task.parentToken && parent.parentToken == 0 &&
			parent.id != AgPBTaskId::DestroyBreakable )
			return true;
	}
	return false;
}

void AgPBTaskStack::RemoveChildren( std::uint64_t token )
{
	m_tasks.erase( std::remove_if( m_tasks.begin() + 1, m_tasks.end(),
		[token]( const AgPBTask &task ) { return task.parentToken == token; } ),
		m_tasks.end() );
}

void AgPBTaskStack::RemoveOrphanChildren()
{
	for ( std::size_t i = 1; i < m_tasks.size(); )
	{
		if ( !HasValidParent( m_tasks[i] ) )
			m_tasks.erase( m_tasks.begin() + i );
		else
			++i;
	}
}

void AgPBTaskStack::Select()
{
	RemoveOrphanChildren();
	std::size_t best = 0;
	const AgPBTask *agent = Find( AgPBTaskId::AgentIntention );
	if ( agent != nullptr )
	{
		// Goal authority is independent of SyPB's local desire scale. Only an
		// execution child of this exact lease may run in place of its owner.
		const std::uint64_t owner = agent->token;
		bool childSelected = false;
		for ( std::size_t i = 1; i < m_tasks.size(); ++i )
		{
			if ( m_tasks[i].token == owner && !childSelected )
				best = i;
			if ( m_tasks[i].parentToken != owner )
				continue;
			if ( !childSelected || m_tasks[i].desire > m_tasks[best].desire ||
				( m_tasks[i].desire == m_tasks[best].desire &&
				  m_tasks[i].token > m_tasks[best].token ) )
				best = i;
			childSelected = true;
		}
	}
	else
	{
		for ( std::size_t i = 1; i < m_tasks.size(); ++i )
		{
			if ( m_tasks[i].desire > m_tasks[best].desire ||
				( m_tasks[i].desire == m_tasks[best].desire &&
				  m_tasks[i].token > m_tasks[best].token ) )
				best = i;
		}
	}
	const std::uint64_t chosen = m_tasks[best].token;
	const std::uint64_t parent = m_tasks[best].parentToken;

	// SyPB discards tasks marked !canContinue on a priority change. Keep
	// resumable tasks as suspended entries; a nonresumable loser is discarded.
	// A running child suspends its owner for execution, so keep that owner even
	// when it would normally be discarded on a different goal's interruption.
	for ( std::size_t i = 1; i < m_tasks.size(); )
	{
		if ( m_tasks[i].token != chosen && m_tasks[i].token != parent && !m_tasks[i].canResume )
			m_tasks.erase( m_tasks.begin() + i );
		else
			++i;
	}
	RemoveOrphanChildren();

	for ( std::size_t i = 0; i < m_tasks.size(); ++i )
	{
		if ( m_tasks[i].token == chosen )
		{
			m_currentIndex = i;
			m_activeToken = chosen;
			return;
		}
	}
	// The selected task itself is never erased.
}

std::uint64_t AgPBTaskStack::Push( const AgPBTask &request )
{
	AgPBTask task = Normalize( request );
	if ( !HasValidParent( task ) )
		return 0;
	for ( std::size_t i = 0; i < m_tasks.size(); ++i )
	{
		AgPBTask &existing = m_tasks[i];
		if ( existing.id != task.id )
			continue;
		if ( SamePayload( existing, task ) )
			return existing.token;
		const std::uint64_t replaced = existing.token;
		task.token = NextToken();
		existing = task;
		RemoveChildren( replaced );
		Select();
		return State( task.token ) == AgPBTaskState::Missing ? 0 : task.token;
	}
	task.token = NextToken();
	m_tasks.push_back( task );
	Select();
	return State( task.token ) == AgPBTaskState::Missing ? 0 : task.token;
}

bool AgPBTaskStack::Complete( std::uint64_t token )
{
	if ( token == 0 || token != m_activeToken )
		return false;
	if ( Current().id == AgPBTaskId::Normal )
	{
		AgPBTask normal;
		normal.token = NextToken();
		Current() = normal;
		RemoveChildren( token );
	}
	else
	{
		m_tasks.erase( m_tasks.begin() + m_currentIndex );
		RemoveChildren( token );
	}
	Select();
	return true;
}

bool AgPBTaskStack::Remove( AgPBTaskId id )
{
	if ( id == AgPBTaskId::Normal )
		return false;
	for ( std::size_t i = 1; i < m_tasks.size(); ++i )
	{
		if ( m_tasks[i].id == id )
		{
			const std::uint64_t token = m_tasks[i].token;
			m_tasks.erase( m_tasks.begin() + i );
			RemoveChildren( token );
			Select();
			return true;
		}
	}
	return false;
}

bool AgPBTaskStack::RemoveToken( std::uint64_t token )
{
	if ( token == 0 )
		return false;
	for ( std::size_t i = 1; i < m_tasks.size(); ++i )
	{
		if ( m_tasks[i].token == token )
		{
			m_tasks.erase( m_tasks.begin() + i );
			RemoveChildren( token );
			Select();
			return true;
		}
	}
	return false;
}

bool AgPBTaskStack::RenewDeadline( std::uint64_t token, float deadline )
{
	if ( token == 0 || !std::isfinite( deadline ) || deadline <= 0.0f ) return false;
	for ( std::size_t i = 1; i < m_tasks.size(); ++i )
	{
		if ( m_tasks[i].token != token ) continue;
		m_tasks[i].deadline = deadline;
		return true;
	}
	return false;
}

bool AgPBTaskStack::Expire( float now )
{
	const std::size_t oldSize = m_tasks.size();
	m_tasks.erase( std::remove_if( m_tasks.begin() + 1, m_tasks.end(),
		[now]( const AgPBTask &task ) {
			return task.deadline > 0.0f && now >= task.deadline;
		} ), m_tasks.end() );
	RemoveOrphanChildren();
	if ( m_tasks.size() == oldSize )
		return false;
	Select();
	return true;
}

void AgPBTaskStack::Reset()
{
	m_tasks.clear();
	AgPBTask normal;
	normal.token = NextToken();
	m_tasks.push_back( normal );
	m_currentIndex = 0;
	m_activeToken = normal.token;
}
