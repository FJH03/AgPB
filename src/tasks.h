/*
 * AgPB normal-mode task scheduler.
 *
 * Adapted from SyPB Project SyPB/SyPB_BOT/include/core.h and
 * source/basecode.cpp (task IDs, desire arbitration, resumable tasks).
 * Copyright (c) 2003-2023 HsK-Dev Blog.
 * Copyright (c) 2003-2009 Yet Another POD-Bot Development Team.
 * CS-EBOT source/ssm/ssm.cpp was compared for process deadlines and
 * remembered-process behavior; no zombie process behavior is included.
 * See tasks.cpp for the SyPB permission notice.
 *
 * AgPB adaptation: GPL-3.0-only.
 */
#ifndef _INCLUDE_AGPB_TASKS_H_
#define _INCLUDE_AGPB_TASKS_H_

#include <cstdint>
#include <cstddef>
#include <vector>

enum class AgPBTaskId
{
	Normal,
	MoveToPosition,
	Camp,
	Pause,
	DestroyBreakable,
	PlantBomb,
	DefuseBomb,
	PickupBomb,
	RescueHostage,
	EscortHostages,
	EscapeBomb
};

// SyPB's normal-mode desire scale. The objective variants added for AgPB
// share the original bomb-task level; callers may choose a different desire.
namespace AgPBTaskPriority
{
	constexpr float Normal = 35.0f;
	constexpr float Pause = 36.0f;
	constexpr float Camp = 37.0f;
	constexpr float MoveToPosition = 50.0f;
	constexpr float Objective = 89.0f;
	constexpr float DestroyBreakable = 100.0f;
	constexpr float EscapeBomb = 100.0f;
}

struct AgPBTask
{
	AgPBTask();

	AgPBTaskId id;
	float desire;
	int waypoint;                 // -1 when no waypoint is assigned.
	std::uintptr_t entityHandle;  // Full ref EHANDLE value, including serial.
	bool hasEntity;
	float position[3];            // Optional world position, engine-independent.
	bool hasPosition;
	float deadline;               // Absolute game time; <= 0 means no expiry.
	bool canResume;               // Keep this task when a higher desire interrupts it.
	std::uint64_t token;          // Stack-owned identity; never set by callers.
};

enum class AgPBTaskState
{
	Missing,    // Completed, removed, expired, or discarded on interruption.
	Active,
	Suspended   // Retained for later resumption with the same token.
};

const char *AgPBTaskName( AgPBTaskId id );

class AgPBTaskStack
{
public:
	AgPBTaskStack();

	// Normal is always present. References/pointers are invalidated by Push,
	// Complete, Remove, Expire, and Reset.
	AgPBTask &Current();
	const AgPBTask &Current() const;
	int Count() const;
	const AgPBTask *At( int index ) const;
	const AgPBTask *Find( AgPBTaskId id ) const;
	bool Contains( AgPBTaskId id ) const;
	AgPBTaskState State( std::uint64_t token ) const;

	// One entry per ID, as in SyPB. An identical push preserves the token;
	// a changed payload replaces it and invalidates pending completions.
	// Returns 0 when a nonresumable low-desire request is discarded.
	std::uint64_t Push( const AgPBTask &task );

	// Complete only the active task with this exact token. Stale completions
	// cannot finish a replacement task of the same ID.
	bool Complete( std::uint64_t token );
	bool Remove( AgPBTaskId id );
	bool RemoveToken( std::uint64_t token );
	bool Expire( float now );
	void Reset();

private:
	void Select();
	std::uint64_t NextToken();

	std::vector<AgPBTask> m_tasks;
	std::size_t m_currentIndex;
	std::uint64_t m_activeToken;
	std::uint64_t m_nextToken;
};

#endif // _INCLUDE_AGPB_TASKS_H_
