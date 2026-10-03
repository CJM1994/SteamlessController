// StickRestProbe — measures what the Steam Controller 2026's sticks report when
// nobody is touching them, so a stick deadzone (issue #117) starts from numbers
// rather than a guess.
//
// A deadzone has to clear whatever the sticks do at rest, and that is two
// different things that need different cures:
//
//   - Noise: the reading jitters around the true centre. A deadzone wider than
//     the jitter hides it.
//   - Offset: the reading is steady but not zero. A deadzone hides it only if it
//     is wider than the offset, and the offset eats into the usable range on the
//     side it leans toward — a centre correction would be the real fix.
//
// Two stages per stick, because a stick that has just been moved comes back to
// rest differently from one that has been left alone:
//
//   1. REST     — hands off, as found.
//   2. RECENTRE — push to the edge, let go, wait a beat, then rest.
//
// Non-invasive by construction, like TriggerProbe: opens the device shared and
// only reads. Never touches lizard mode, so Steam can keep running.
//
// Over Bluetooth (~157 Hz) the sample count is lower than over USB or the puck
// (~250 Hz); run it on the transport the report came from.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "hid/HidDevice.h"
#include "steam/SteamController.h"

namespace {

constexpr double kFull = 32767.0;

double Pct(double raw) { return raw / kFull * 100.0; }

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

struct StickSample { int16_t x = 0, y = 0; };

// Reads state reports for `seconds`, calling `take` with each one's four axes.
template <class Take>
size_t Collect(HidDevice& dev, double seconds, Take&& take) {
    const auto end = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
    size_t count = 0;
    while (std::chrono::steady_clock::now() < end) {
        uint8_t buf[64] = {};
        const size_t n = dev.ReadInputReport(buf, sizeof(buf), 16);
        if (n < 18 || !SteamController::IsStateReportId(buf[0])) continue;
        StickSample l, r;
        std::memcpy(&l.x, buf + 10, 2);
        std::memcpy(&l.y, buf + 12, 2);
        std::memcpy(&r.x, buf + 14, 2);
        std::memcpy(&r.y, buf + 16, 2);
        take(l, r);
        ++count;
    }
    return count;
}

void Report(const char* label, const std::vector<StickSample>& s) {
    if (s.empty()) {
        printf("  %-6s no samples\n", label);
        return;
    }
    double sx = 0.0, sy = 0.0;
    for (const auto& p : s) { sx += p.x; sy += p.y; }
    const double mx = sx / static_cast<double>(s.size());
    const double my = sy / static_cast<double>(s.size());

    // Radius from the true zero is what a deadzone has to beat: the game sees
    // the report, not the report minus its own average.
    std::vector<double> fromZero, fromMean;
    int minX = 32767, maxX = -32768, minY = 32767, maxY = -32768;
    for (const auto& p : s) {
        fromZero.push_back(std::hypot(static_cast<double>(p.x), static_cast<double>(p.y)));
        fromMean.push_back(std::hypot(p.x - mx, p.y - my));
        minX = std::min<int>(minX, p.x); maxX = std::max<int>(maxX, p.x);
        minY = std::min<int>(minY, p.y); maxY = std::max<int>(maxY, p.y);
    }
    const double offset = std::hypot(mx, my);

    printf("  %-6s n=%-5zu offset (mean) x=%7.1f y=%7.1f  |%6.0f| = %.2f%%\n",
           label, s.size(), mx, my, offset, Pct(offset));
    printf("         x range [%6d, %6d]   y range [%6d, %6d]\n", minX, maxX, minY, maxY);
    printf("         radius from zero:   median %6.0f  p99 %6.0f  max %6.0f  (max = %.2f%%)\n",
           Percentile(fromZero, 50), Percentile(fromZero, 99), Percentile(fromZero, 100),
           Pct(Percentile(fromZero, 100)));
    printf("         jitter about mean:  median %6.0f  p99 %6.0f  max %6.0f  (max = %.2f%%)\n",
           Percentile(fromMean, 50), Percentile(fromMean, 99), Percentile(fromMean, 100),
           Pct(Percentile(fromMean, 100)));
}

void Countdown(const char* prompt, int seconds) {
    printf("\n%s\n", prompt);
    for (int i = seconds; i > 0; --i) {
        printf("  starting in %d...\r", i);
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    printf("  recording...        \n");
}

}  // namespace

int main() {
    auto paths = SteamController::EnumerateAll();
    if (paths.empty()) {
        puts("No Steam Controller found (wired PID=1302, Bluetooth 1303 or dongle 1304).");
        return 1;
    }

    HidDevice dev;
    if (!dev.Open(paths[0])) {
        puts("Failed to open HID device.");
        return 1;
    }
    printf("Opened %ls\n", paths[0].c_str());
    if (paths.size() > 1) {
        printf("NOTE: %zu interfaces found; this is the first. If no samples arrive,\n"
               "      the controller is on one of the others.\n", paths.size());
    }

    puts("\nStick Rest Probe — reads only; Steam can stay running.");
    puts("Values are raw int16 (full deflection = 32767).");

    std::vector<StickSample> restL, restR, recL, recR;

    Countdown("STAGE 1 — REST: put the controller down and take your thumbs OFF both sticks.", 3);
    Collect(dev, 5.0, [&](StickSample l, StickSample r) { restL.push_back(l); restR.push_back(r); });

    Countdown("STAGE 2 — RECENTRE: in the next 4 seconds push BOTH sticks to the edge\n"
              "and spin them around the rim, then LET GO and keep hands off.", 3);
    // The flick is not recorded, only what the stick settles to after it.
    Collect(dev, 4.0, [](StickSample, StickSample) {});
    puts("  hands off now — settling...");
    Collect(dev, 1.5, [](StickSample, StickSample) {});
    puts("  recording...");
    Collect(dev, 5.0, [&](StickSample l, StickSample r) { recL.push_back(l); recR.push_back(r); });

    puts("\n=== REST (as found) ===");
    Report("LEFT",  restL);
    Report("RIGHT", restR);
    puts("\n=== RECENTRE (after being moved) ===");
    Report("LEFT",  recL);
    Report("RIGHT", recR);

    puts("\nA deadzone must exceed the 'max' radius from zero in both tables to hide");
    puts("everything seen here. A large 'offset' with a small 'jitter' means the");
    puts("centre is off rather than noisy.");
    return 0;
}
