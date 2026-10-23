// Standalone regression for bounded SyPB-style view tracking and Source
// CUserCmd movement projection, including the real terminal feedback policy.
#include "../src/movement_control.h"
#include "../src/route_control.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float a, float b, float tolerance = 0.0002f) {
    return std::fabs(a - b) <= tolerance;
}
static void Reconstruct(const AgPBMovement::LocalWish &wish, float yaw, float &x, float &y) {
    const float radians = yaw * 0.01745329251994329577f;
    const float cosine = std::cos(radians), sine = std::sin(radians);
    x = wish.forward * cosine + wish.side * sine;
    y = wish.forward * sine - wish.side * cosine;
}
static void BoundedTurns() {
    using namespace AgPBMovement;
    const float intervals[] = {1.0f / 30.0f, 1.0f / 64.0f, 1.0f / 128.0f};
    for (float dt : intervals) {
        View actual = {0, 170};
        const View targets[] = {{35, -170}, {-35, 90}, {89, -90}, {0, 179}};
        for (const View &target : targets) {
            for (int tick = 0; tick < int(3.0f / dt); ++tick) {
                const View next = ApproachView(actual.pitch, actual.yaw,
                    target.pitch, target.yaw, dt, 180, 120, 8);
                const float previousError = std::fabs(NormalizeYaw(target.yaw - actual.yaw));
                const float nextError = std::fabs(NormalizeYaw(target.yaw - next.yaw));
                Require(std::fabs(NormalizeYaw(next.yaw - actual.yaw)) <= 180 * dt + 0.0001f,
                    "yaw rate bounded across wrap and tick rates");
                Require(std::fabs(next.pitch - actual.pitch) <= 120 * dt + 0.0001f,
                    "pitch rate bounded at all tick rates");
                Require(nextError <= previousError + 0.0001f, "yaw moves along shortest arc without overshoot");
                Require(std::fabs(target.pitch - next.pitch) <= std::fabs(target.pitch - actual.pitch) + 0.0001f,
                    "pitch does not overshoot");
                actual = next;
            }
            Require(std::fabs(NormalizeYaw(target.yaw - actual.yaw)) < 0.01f &&
                    std::fabs(target.pitch - actual.pitch) < 0.01f, "view converges");
        }
    }
    const View wrap = ApproachView(0, 179, 0, -179, 1.0f / 64.0f);
    Require(NormalizeYaw(wrap.yaw - 179) > 0, "179 to -179 turns positive by shortest arc");
    const View longFrame = ApproachView(85, -170, -89, 170, 2, 180, 120);
    Require(Near(longFrame.pitch, -89) && Near(longFrame.yaw, 170), "long frames stop at target");
    const View networkPitch = ApproachView(359, 0, 358, 0, 1);
    Require(Near(networkPitch.pitch, -2), "network pitch normalizes before clamping");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const View hold = ApproachView(20, 60, nan, nan, 1.0f / 64.0f);
    Require(Near(hold.pitch, 20) && Near(hold.yaw, 60), "invalid target holds current finite view");
    const View invalidTime = ApproachView(20, 60, 80, -120, nan);
    Require(Near(invalidTime.pitch, 20) && Near(invalidTime.yaw, 60), "invalid interval holds view");
    const View invalidCurrent = ApproachView(nan, nan, 0, 0, 1);
    Require(std::isfinite(invalidCurrent.pitch) && std::isfinite(invalidCurrent.yaw), "invalid current has finite fallback");
}
static void MovementIndependentFromView() {
    using namespace AgPBMovement;
    const float headings[] = {-179, -90, -35, 0, 45, 90, 179};
    for (float heading : headings) {
        const float radians = heading * 0.01745329251994329577f;
        const float worldX = 250 * std::cos(radians), worldY = 250 * std::sin(radians);
        View view = {0, NormalizeYaw(heading + 160)};
        for (int tick = 0; tick < 180; ++tick) {
            view = ApproachView(view.pitch, view.yaw, 0, heading, 1.0f / 64.0f);
            const LocalWish local = WorldToLocal(worldX, worldY, view.yaw);
            float x, y;
            Reconstruct(local, view.yaw, x, y);
            Require(Near(x, worldX) && Near(y, worldY), "turning view preserves world route wish");
            Require(Near(std::sqrt(local.forward * local.forward + local.side * local.side), 250),
                    "projection preserves wish speed");
            const LocalWish stopped = WorldToLocal(0, 0, view.yaw);
            Require(stopped.forward == 0 && stopped.side == 0, "turning idle view cannot add movement");
        }
    }
    const LocalWish right = WorldToLocal(0, -250, 0);
    Require(Near(right.forward, 0) && Near(right.side, 250), "Source sidemove sign matches right vector");
    const LocalWish invalid = WorldToLocal(std::numeric_limits<float>::infinity(), 0, 0);
    Require(invalid.forward == 0 && invalid.side == 0, "invalid world wish cannot reach CUserCmd");
}
static void TerminalStopWhileTurning() {
    // Verify the actual feedback controller still settles if local command
    // axes turn every tick. No smoother is permitted to carry residual motion
    // over a terminal stop. Source friction precedes ground acceleration.
    float x = 80, y = -65, vx = 220, vy = -80, stable = 0;
    const float dt = 1.0f / 64.0f;
    AgPBMovement::View view = {0, 135};
    for (int tick = 0; tick < 12 * 64; ++tick) {
        const bool inside = AgPBTerminal::ControlPositionInside(-x, -y, 0, 4, true);
        const AgPBTerminal::Wish world = AgPBTerminal::ComputeWish(-x, -y, vx, vy, dt, 5, 4, 100, 250, inside);
        view = AgPBMovement::ApproachView(view.pitch, view.yaw, 0, tick < 80 ? -135.0f : 85.0f, dt);
        const AgPBMovement::LocalWish local = AgPBMovement::WorldToLocal(world.x, world.y, view.yaw);
        float wishX, wishY;
        Reconstruct(local, view.yaw, wishX, wishY);
        const float speed = std::sqrt(vx * vx + vy * vy);
        const float keep = speed > 0.01f ? AgPBTerminal::Max(0, speed - AgPBTerminal::Max(speed, 100) * 4 * dt) / speed : 0;
        vx *= keep; vy *= keep;
        const float wishSpeed = std::sqrt(wishX * wishX + wishY * wishY);
        if (wishSpeed > 0.001f) {
            const float nx = wishX / wishSpeed, ny = wishY / wishSpeed;
            const float add = wishSpeed - vx * nx - vy * ny;
            if (add > 0) {
                const float acceleration = AgPBTerminal::Min(add, 5 * dt * wishSpeed);
                vx += nx * acceleration; vy += ny * acceleration;
            }
        }
        x += vx * dt; y += vy * dt;
        if (AgPBTerminal::ControlPositionInside(-x, -y, 0, 4, true) &&
            AgPBTerminal::CanSettle(-x, -y, 0, 4, true, vx, vy, 0)) stable += dt;
        else stable = 0;
        if (stable >= AgPBTerminal::StableSeconds) {
            std::printf("PASS terminal stop while turning distance=%.3f speed=%.3f\n",
                std::sqrt(x*x + y*y), std::sqrt(vx*vx + vy*vy));
            return;
        }
    }
    Require(false, "view tracking preserves authored terminal settle");
}
int main() {
    BoundedTurns();
    MovementIndependentFromView();
    TerminalStopWhileTurning();
    std::puts("PASS bounded view tracking and world-space CUserCmd movement");
    return 0;
}
