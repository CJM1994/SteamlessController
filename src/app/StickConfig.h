#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Stick deadzone (#117).
//
// The 2026 controller's sticks are reported exactly as the hardware reads them,
// and a hand-off stick does not read exactly zero. Anything that takes the pad's
// word for it — an emulator waiting for "press the input to bind", a gamepad
// tester — sees the sticks moving all the time, and a bind prompt latches onto
// them before a button can be pressed. XInput games mostly hide this behind a
// deadzone of their own, which is why it only shows up elsewhere.
//
// So the virtual pad applies one blanket deadzone, here, to both platforms. It
// is fixed rather than a setting: a stick does not need tuning per game the way
// a trigger threshold does, and StickRestProbe measures what it has to clear.
//
// What it measured, hands off, on two units: the resting reading is a steady
// offset from zero rather than noise — jitter about the mean is under 0.5% —
// and the offset is as large as 5.9% of the range on one stick, wandering by
// over a point from one settle to the next. A deadzone has to clear the offset,
// not the jitter, and has to leave room for the stick to come to rest somewhere
// a little worse than it was measured.

// Raw stick range in the state report: -0x8000 .. 0x7FFF, centre at zero.
inline constexpr int kStickRawMax = 0x7FFF;

// Percent of full deflection that reads as no input. About 1.4 times the worst
// resting reading measured (5.87%); wider costs fine control for no benefit.
inline constexpr int kStickDeadzonePercent = 8;
inline constexpr int kStickDeadzoneRaw     = kStickRawMax * kStickDeadzonePercent / 100;

struct StickPos {
    int16_t x = 0;
    int16_t y = 0;
};

// Radial deadzone with the remaining travel rescaled to the full range.
//
// Radial rather than per-axis: a per-axis deadzone turns a stick held nearly
// straight up into a perfectly straight one, and snaps diagonals toward the
// axes. A circle leaves the direction alone and only decides whether there is
// any input.
//
// Rescaled rather than just cut: otherwise the output jumps from nothing to the
// size of the deadzone the instant the stick leaves it, and the top of the range
// is lost. The scale is applied to each axis rather than to the vector's length,
// and each axis is clamped on its own, so a diagonal pushed into a stick's
// corner — whose length is past a circle's edge — still reaches full on both
// axes as it did before.
inline StickPos ApplyStickDeadzone(int16_t x, int16_t y,
                                   int deadzone = kStickDeadzoneRaw) {
    if (deadzone <= 0) return { x, y };
    // A deadzone as wide as the whole range would leave nothing to rescale into.
    const double dz = static_cast<double>(std::min(deadzone, kStickRawMax - 1));

    const double mag = std::hypot(static_cast<double>(x), static_cast<double>(y));
    if (mag <= dz) return {};

    const double full  = static_cast<double>(kStickRawMax);
    const double scale = (mag - dz) / (full - dz) * full / mag;

    auto axis = [&](int16_t v) {
        const long scaled = std::lround(static_cast<double>(v) * scale);
        return static_cast<int16_t>(std::clamp<long>(scaled, -32768L, 32767L));
    };
    return { axis(x), axis(y) };
}
