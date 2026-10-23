// AgPB view/movement command policy. GPL-3.0-only.
// Behavior baseline: SyPB 4c364fb, Bot::FacePosition (navigate.cpp) and
// Bot::RunPlayerMovement (basecode.cpp); compared with CS-EBOT 2333562,
// Bot::UpdateLooking and its separate m_moveAngles command path.
// SyPB keeps proportional view tracking independent from its movement angles.
// Source carries both through CUserCmd, so preserve the world wish by projecting
// it into the submitted view yaw. This is command math, not entity state writes.
//
// Reference copyright (SyPB/YaPB):
// Copyright (c) 2003-2023, by HsK-Dev Blog
// https://ccnhsk-dev.blogspot.com/
// Copyright (c) 2003-2009, by Yet Another POD-Bot Development Team.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#ifndef AGPB_MOVEMENT_CONTROL_H
#define AGPB_MOVEMENT_CONTROL_H
#include <cmath>

namespace AgPBMovement {
struct View { float pitch; float yaw; };
struct LocalWish { float forward; float side; };

inline float Clamp(float value, float lower, float upper) {
    return value < lower ? lower : (value > upper ? upper : value);
}
inline float NormalizeYaw(float angle) {
    if (!std::isfinite(angle)) return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    if (angle < -180.0f) angle += 360.0f;
    return angle;
}
inline float Pitch(float angle) { return Clamp(NormalizeYaw(angle), -89.0f, 89.0f); }

inline float ApproachDelta(float error, float interval, float maxRate, float stiffness) {
    if (!std::isfinite(interval) || interval <= 0.0f ||
        !std::isfinite(maxRate) || maxRate <= 0.0f ||
        !std::isfinite(stiffness) || stiffness <= 0.0f) return 0.0f;
    // SyPB FacePosition applies stiffness * angular error * frame interval.
    // The rate cap bounds large turns; the error cap prevents a long frame
    // from overshooting. There is no random aim/motion or perfect-fire snap.
    const float fraction = Clamp(stiffness * interval, 0.0f, 1.0f);
    return Clamp(error * fraction, -maxRate * interval, maxRate * interval);
}
inline View ApproachView(float currentPitch, float currentYaw,
                         float targetPitch, float targetYaw, float interval,
                         float maxYawRate = 240.0f, float maxPitchRate = 180.0f,
                         float stiffness = 8.0f) {
    View result = {Pitch(currentPitch), NormalizeYaw(currentYaw)};
    if (std::isfinite(targetPitch))
        result.pitch += ApproachDelta(Pitch(targetPitch) - result.pitch,
                                      interval, maxPitchRate, stiffness);
    if (std::isfinite(targetYaw))
        result.yaw = NormalizeYaw(result.yaw + ApproachDelta(
            NormalizeYaw(NormalizeYaw(targetYaw) - result.yaw),
            interval, maxYawRate, stiffness));
    result.pitch = Pitch(result.pitch);
    return result;
}

inline LocalWish WorldToLocal(float worldX, float worldY, float viewYaw) {
    LocalWish result = {0.0f, 0.0f};
    if (!std::isfinite(worldX) || !std::isfinite(worldY) || !std::isfinite(viewYaw))
        return result;
    constexpr float ToRadians = 0.01745329251994329577f;
    const float yaw = NormalizeYaw(viewYaw) * ToRadians;
    const float cosine = std::cos(yaw), sine = std::sin(yaw);
    // Source AngleVectors at zero pitch/roll: forward=(cos,sin),
    // right=(sin,-cos). Preserve length and a zero wish exactly. The caller
    // owns speed limits, terminal braking, ladder and airborne policies.
    result.forward = worldX * cosine + worldY * sine;
    result.side = worldX * sine - worldY * cosine;
    if (!std::isfinite(result.forward) || !std::isfinite(result.side))
        result = LocalWish{0.0f, 0.0f};
    return result;
}
}
#endif
