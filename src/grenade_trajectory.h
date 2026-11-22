// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>

namespace AgPBGrenade
{
struct Vec3 { float x, y, z; };

struct Input
{
	Vec3 eye;
	Vec3 velocity;
	Vec3 target;
	float gravity; // Projectile acceleration in world units per second squared.
	float maxFlightTime;
};

struct Solution { float pitch, yaw, flightTime; };
struct State { Vec3 position, velocity; };

namespace Detail
{
constexpr double MuzzleDistance = 16.0;
constexpr double DegreesPerRadian = 57.29577951308232;

inline bool Finite( const Vec3 &point )
{
	return std::isfinite( point.x ) && std::isfinite( point.y ) && std::isfinite( point.z );
}

inline double ThrowSpeed( double pitch )
{
	const double speed = ( 90.0 - pitch ) * 6.0;
	return speed < 750.0 ? speed : 750.0;
}

struct Path
{
	double x, y, z, vx, vy, vz;
};

inline Path Launch( const Input &in, const Solution &aim )
{
	// [game-diff] CS:CZS remaps commanded view pitch before deriving launch speed.
	const double pitch = -10.0 + aim.pitch * ( aim.pitch < 0.0f ? 80.0 / 90.0 : 100.0 / 90.0 );
	const double yaw = aim.yaw / DegreesPerRadian;
	const double horizontal = std::cos( pitch / DegreesPerRadian );
	const double x = horizontal * std::cos( yaw ), y = horizontal * std::sin( yaw );
	const double z = -std::sin( pitch / DegreesPerRadian );
	const double speed = ThrowSpeed( pitch );
	return { in.eye.x + x * MuzzleDistance, in.eye.y + y * MuzzleDistance, in.eye.z + z * MuzzleDistance,
		in.velocity.x + x * speed, in.velocity.y + y * speed, in.velocity.z + z * speed };
}

inline Vec3 PositionAt( const Path &path, double gravity, double time )
{
	return { static_cast<float>( path.x + path.vx * time ), static_cast<float>( path.y + path.vy * time ),
		static_cast<float>( path.z + path.vz * time - 0.5 * gravity * time * time ) };
}

struct Flight
{
	double x, y, length, pitch, speed;
};

inline Flight ToTarget( const Input &in, double time )
{
	const double x = static_cast<double>( in.target.x ) - in.eye.x - in.velocity.x * time;
	const double y = static_cast<double>( in.target.y ) - in.eye.y - in.velocity.y * time;
	const double z = static_cast<double>( in.target.z ) - in.eye.z - in.velocity.z * time + 0.5 * in.gravity * time * time;
	const double horizontal = std::sqrt( x * x + y * y );
	const double pitch = std::atan2( -z, horizontal ) * DegreesPerRadian;
	return { x, y, std::sqrt( horizontal * horizontal + z * z ), pitch, ThrowSpeed( pitch ) };
}

inline double Miss( const Input &in, double time )
{
	const Flight flight = ToTarget( in, time );
	return flight.length - MuzzleDistance - flight.speed * time;
}

template <class Clear>
bool TraceSolution( const Input &in, double time, Solution &out, Clear &clear )
{
	const Flight flight = ToTarget( in, time );
	if ( time <= 0.0 || flight.length < MuzzleDistance || flight.speed <= 0.0 ) return false;
	// [game-diff] Invert CS:CZS CBaseCSGrenade::ThrowGrenade's view-pitch remapping.
	const double pitch = ( flight.pitch + 10.0 ) * ( flight.pitch < -10.0 ? 90.0 / 80.0 : 90.0 / 100.0 );
	if ( pitch < -89.0 || pitch > 89.0 ) return false;
	const Solution candidate = { static_cast<float>( pitch ),
		static_cast<float>( std::atan2( flight.y, flight.x ) * DegreesPerRadian ), static_cast<float>( time ) };
	const Path path = Launch( in, candidate );
	const Vec3 muzzle = PositionAt( path, in.gravity, 0.0 );
	// This solver requires a full muzzle sweep and a clear arc through the target.
	if ( !clear( in.eye, muzzle ) ) return false;
	const int steps = time >= 3.2 ? 64 : static_cast<int>( std::ceil( time / 0.05 ) );
	Vec3 from = muzzle;
	for ( int step = 1; step <= steps; ++step )
	{
		const Vec3 to = PositionAt( path, in.gravity, static_cast<double>( candidate.flightTime ) * step / steps );
		if ( !clear( from, to ) ) return false;
		from = to;
	}
	out = candidate;
	return true;
}
}

// Evaluate the unobstructed trajectory. Elapsed time starts at muzzle emission.
inline State Evaluate( const Input &in, const Solution &aim, float elapsed )
{
	const Detail::Path path = Detail::Launch( in, aim );
	const double time = elapsed;
	return { Detail::PositionAt( path, in.gravity, time ),
		{ static_cast<float>( path.vx ), static_cast<float>( path.vy ), static_cast<float>( path.vz - in.gravity * time ) } };
}

// CS:CZS CBaseCSGrenade launches from eye + 16 * forward at a pitch-dependent speed,
// plus the thrower's velocity. At a fixed flight time, gravity and that inherited
// velocity uniquely fix the required forward direction. Search time in increasing
// order so a blocked short throw can fall back to a lofted trajectory.
template <class Clear>
bool Solve( const Input &in, Solution &out, Clear clear )
{
	if ( !Detail::Finite( in.eye ) || !Detail::Finite( in.velocity ) || !Detail::Finite( in.target ) ||
	     !std::isfinite( in.gravity ) || in.gravity < 0.0f ||
	     !std::isfinite( in.maxFlightTime ) || in.maxFlightTime <= 0.0f ) return false;

	constexpr int samples = 64;
	double previousTime = 0.0;
	double previousMiss = Detail::Miss( in, previousTime );
	for ( int sample = 1; sample <= samples; ++sample )
	{
		const double time = static_cast<double>( in.maxFlightTime ) * sample / samples;
		const double miss = Detail::Miss( in, time );
		if ( miss == 0.0 )
		{
			if ( Detail::TraceSolution( in, time, out, clear ) ) return true;
		}
		else if ( previousMiss != 0.0 && ( previousMiss < 0.0 ) != ( miss < 0.0 ) )
		{
			double low = previousTime, high = time;
			for ( int refinement = 0; refinement < 20; ++refinement )
			{
				const double middle = ( low + high ) * 0.5;
				if ( ( Detail::Miss( in, middle ) < 0.0 ) == ( previousMiss < 0.0 ) ) low = middle;
				else high = middle;
			}
			if ( Detail::TraceSolution( in, ( low + high ) * 0.5, out, clear ) ) return true;
		}
		previousTime = time;
		previousMiss = miss;
	}
	return false;
}
}
