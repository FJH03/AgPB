// Production observation policy regression. No SDK or game process required.
#include "../src/observation_look.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static void Bounded(AgPBObservationLook::Angles view) {
    Require(std::isfinite(view.pitch) && view.pitch >= -89.0f && view.pitch <= 89.0f,
            "every observation command has a normalized bounded pitch");
    Require(std::isfinite(view.yaw) && view.yaw >= -180.0f && view.yaw <= 180.0f,
            "every observation command has a normalized yaw");
}
int main() {
    using namespace AgPBObservationLook;
    const float tick = 1.0f / 64.0f;
    Angles view = {0, 0};
    for (int i = 0; i < 64; ++i) {
        const float previous = view.pitch;
        view = Step(view, {-10, 0}, 120, tick); Bounded(view);
        Require(view.pitch < 0 && view.pitch <= previous && view.pitch >= -10,
                "upward target remains upward after the second tick and converges monotonically");
    }
    Require(Near(view.pitch, -10), "negative pitch reaches target instead of clamping to ground");
    view = Step({-1.875f, 0}, {-10, 0}, 120, tick);
    Require(Near(view.pitch, -3.75f), "original second-tick unsigned angle regression");
    view = Step({356.25f, 720}, {350, -360}, 120, tick);
    Require(Near(view.pitch, -5.625f) && Near(view.yaw, 0), "encoded negative pitch normalizes before interpolation and clamp");
    view = Step({0, 179}, {0, -179}, 64, tick);
    Require(Near(view.yaw, 180), "yaw +180 wrap uses shortest path");
    view = Step(view, {0, -179}, 64, tick);
    Require(Near(view.yaw, -179), "yaw wraps to normalized negative side on next tick");
    view = Step({0, -179}, {0, 179}, 64, tick);
    Require(Near(view.yaw, -180), "reverse yaw wrap remains continuous");
    Require(Near(Pitch(350), -10) && Near(Pitch(-370), -10) && Near(Pitch(360), 0),
            "pitch representation 360 wraps preserve signed direction");
    Require(Near(Pitch(180), 89) && Near(Pitch(-180), -89), "invalid vertical half turns remain physically bounded");

    const float wraps[] = {-1080, -720, -360, -181, -180, -179, -90, -89, -1, 0, 1, 89, 90, 179, 180, 181, 271, 350, 360, 720, 1080};
    for (float pitch : wraps) for (float yaw : wraps) {
        view = {pitch, yaw};
        for (int i = 0; i < 80; ++i) {
            const Angles before = Sanitize(view);
            view = Step(view, {-25, -179}, 180, tick); Bounded(view);
            Require(std::fabs(view.pitch - before.pitch) <= 180 * tick + 0.001f &&
                    std::fabs(Normalize(view.yaw - before.yaw)) <= 180 * tick + 0.001f,
                    "signed shortest-angle steps remain bounded across arbitrary encodings");
        }
        Require(Near(view.pitch, -25) && Near(view.yaw, -179), "all encodings converge to same normalized target");
    }

    // Source VectorAngles uses -atan2(z, horizontal); AngleVectors returns
    // forward.z=-sin(pitch). Real targets on either floor keep that sign.
    const float elevatedPitch = -static_cast<float>(std::atan2(128.0, 256.0) * 180.0 / 3.14159265358979323846);
    view = {0, 0};
    for (int i = 0; i < 80; ++i) { view = Step(view, {elevatedPitch, 45}, 120, tick); Bounded(view); }
    Require(Near(view.pitch, elevatedPitch) && view.pitch < 0, "legitimate elevated observation remains upward");
    for (int i = 0; i < 80; ++i) { view = Step(view, {-elevatedPitch, -45}, 180, tick); Bounded(view); }
    Require(Near(view.pitch, -elevatedPitch) && view.pitch > 0, "legitimate lower-floor observation remains downward");
    view = Step({350, 361}, {-30, 80}, 180, 0);
    Require(Near(view.pitch, -10) && Near(view.yaw, 1), "zero interval sanitizes without turning");
    view = Step({350, 361}, {-30, 80}, 180, -1);
    Require(Near(view.pitch, -10) && Near(view.yaw, 1), "negative interval cannot reverse a turn");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Bounded(Step({nan, inf}, {-inf, nan}, inf, nan));

    Require(!ScanCandidate(0, 0, -64, 1400) && !ScanCandidate(127, 0, 0, 1400),
            "random scans reject own feet and near-duplicate local points");
    Require(ScanCandidate(128, 0, 0, 1400) && ScanCandidate(256, 0, 128, 1400) &&
            ScanCandidate(256, 0, -128, 1400), "authored eye-height targets retain upper and lower floors");
    Require(!ScanCandidate(128, 0, -512, 1400) && !ScanCandidate(1401, 0, 0, 1400) &&
            !ScanCandidate(nan, 0, 0, 1400), "random scans bound extreme vertical, range and invalid observations");
    std::puts("observation look tests passed");
}
