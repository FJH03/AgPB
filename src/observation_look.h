// AgPB idle and investigation observation angles. GPL-3.0-only.
// SyPB navigate.cpp FacePosition and CS-EBOT basecode.cpp normalize view
// angles after turning. Source mathlib confirms negative pitch looks up.
#ifndef _INCLUDE_AGPB_OBSERVATION_LOOK_H_
#define _INCLUDE_AGPB_OBSERVATION_LOOK_H_

#include <algorithm>
#include <cmath>

namespace AgPBObservationLook
{
struct Angles { float pitch, yaw; };

inline float Clamp( float value, float low, float high )
{
    return std::max( low, std::min( value, high ) );
}

inline float Normalize( float angle )
{
    if ( !std::isfinite( angle ) ) return 0.0f;
    angle = std::fmod( angle, 360.0f );
    if ( angle > 180.0f ) angle -= 360.0f;
    else if ( angle < -180.0f ) angle += 360.0f;
    return angle;
}

inline float Pitch( float angle )
{
    // Normalize before clamping: 356 degrees means -4 degrees, not +89.
    return Clamp( Normalize( angle ), -89.0f, 89.0f );
}

inline Angles Sanitize( Angles view )
{
    Angles result = { Pitch( view.pitch ), Normalize( view.yaw ) };
    return result;
}

inline float StepAngle( float current, float wanted, float step )
{
    current = Normalize( current );
    const float delta = Normalize( Normalize( wanted ) - current );
    return Normalize( current + Clamp( delta, -step, step ) );
}

inline Angles Step( Angles current, Angles wanted, float degreesPerSecond, float interval )
{
    current = Sanitize( current ); wanted = Sanitize( wanted );
    const float rate = std::isfinite( degreesPerSecond ) ? std::max( 0.0f, degreesPerSecond ) : 0.0f;
    const float time = std::isfinite( interval ) ? Clamp( interval, 0.0f, 0.1f ) : 0.0f;
    const float step = rate * time;
    Angles result = { Pitch( StepAngle( current.pitch, wanted.pitch, step ) ),
                      Normalize( StepAngle( current.yaw, wanted.yaw, step ) ) };
    return result;
}

inline bool ScanCandidate( float dx, float dy, float dz, float maximumRange )
{
    if ( !std::isfinite( dx ) || !std::isfinite( dy ) || !std::isfinite( dz ) ||
         !std::isfinite( maximumRange ) || maximumRange < 128.0f ) return false;
    const float flatSquared = dx * dx + dy * dy;
    const float distanceSquared = flatSquared + dz * dz;
    // Random scans should observe a distant authored player's eye position,
    // not the floor under our own feet. The sixty-degree cone retains genuine
    // upper/lower floors; specific remembered observations use the full view.
    return flatSquared >= 128.0f * 128.0f && distanceSquared <= maximumRange * maximumRange &&
        dz * dz <= flatSquared * 3.0f;
}
}

#endif // _INCLUDE_AGPB_OBSERVATION_LOOK_H_
