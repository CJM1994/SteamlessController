// PowerOffProbe — finds out whether the Steam Controller 2026 has a firmware
// command that turns it off, so Steam + Y (issue #106) can power the pad down
// the way Steam does.
//
// What is known: the older Steam hardware has "turn off controller" command
// 0x9F in the same feature-report channel the rest of the app already uses (the
// Linux hid-steam driver and SDL both list it, and the original Steam Controller
// took the ASCII payload "off!"). What is not known: whether the SC2026 firmware
// honours it, with which payload, or over which transport. Neither SDL nor Linux
// ever sends it to a Triton pad, so there is nothing to copy; this tool tries the
// candidates and watches what the pad does.
//
// "Did it work" is read two ways, because a write that returns success only means
// the host handed the report over:
//   - the write result (a refused report is a clear no), and
//   - whether the pad's input reports stop. A pad that powered off goes quiet and,
//     over Bluetooth, drops its connection; one that ignored the command keeps
//     streaming. The tool prints the silence time after every send.
//
// Like the other probes it opens the device shared and never touches lizard mode,
// so Steam can keep running. The one thing that changes the pad's state is the
// command you choose to send — and a successful one switches it off, which is the
// point. Press the Steam button to wake it, then re-run the tool (the device path
// can change on reconnect).
//
// Run it on each transport you care about (USB cable, puck, Bluetooth): the
// firmware path may differ, and a command the pad ignores over one can work over
// another.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <conio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "hid/HidDevice.h"
#include "steam/SteamController.h"

namespace {

constexpr uint8_t kCmdTurnOff = 0x9F;

struct Variant {
    const char*          name;
    uint8_t              reportId;  // feature report ID the command rides in
    std::vector<uint8_t> payload;
};

// The order is most to least likely, so a first success stops the search early.
// The Steam Controller 2015 form is first: 9F, length 4, "off!".
const Variant kVariants[] = {
    { "9F \"off!\"      (report 01)", SteamController::FEATURE_REPORT_CMD,  { 'o', 'f', 'f', '!' } },
    { "9F no payload  (report 01)", SteamController::FEATURE_REPORT_CMD,  {} },
    { "9F \"off\"       (report 01)", SteamController::FEATURE_REPORT_CMD,  { 'o', 'f', 'f' } },
    { "9F 01          (report 01)", SteamController::FEATURE_REPORT_CMD,  { 0x01 } },
    { "9F \"off!\"      (report 02)", SteamController::FEATURE_REPORT_CMD2, { 'o', 'f', 'f', '!' } },
    { "9F no payload  (report 02)", SteamController::FEATURE_REPORT_CMD2, {} },
};
constexpr int kVariantCount = static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]));

double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

std::atomic<bool>     g_quit{false};
std::atomic<uint64_t> g_stateReports{0};
std::atomic<double>   g_lastStateMs{0.0};
std::atomic<uint8_t>  g_lastButtons[4]{};

// Reads state reports so the tool can tell "pad went quiet" from "pad is still
// talking". Also keeps the latest button bytes so `watch` can show Steam + Y.
void ReaderThread(HidDevice* dev) {
    while (!g_quit.load()) {
        uint8_t buf[64] = {};
        const size_t n = dev->ReadInputReport(buf, sizeof(buf), 100);
        if (n == 0) continue;
        if (!SteamController::IsStateReportId(buf[0]) || n < 6) continue;
        for (int i = 0; i < 4; ++i) g_lastButtons[i].store(buf[2 + i]);
        g_lastStateMs.store(NowMs());
        ++g_stateReports;
    }
}

double SilenceMs() {
    const double last = g_lastStateMs.load();
    return last > 0.0 ? NowMs() - last : -1.0;
}

std::string Hex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[4];
    for (uint8_t x : v) {
        snprintf(b, sizeof(b), "%02X ", x);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

// Frame the command the way every other firmware command in the app is framed:
// [report id | command | payload size | payload...], zero padded to 64.
bool SendCommand(HidDevice& dev, uint8_t reportId, uint8_t cmd, const std::vector<uint8_t>& payload) {
    if (payload.size() > 61) {
        puts("Payload too long (max 61 bytes).");
        return false;
    }
    uint8_t buf[64] = {};
    buf[0] = reportId;
    buf[1] = cmd;
    buf[2] = static_cast<uint8_t>(payload.size());
    if (!payload.empty()) std::memcpy(buf + 3, payload.data(), payload.size());
    if (!dev.SendFeatureReport(buf, sizeof(buf))) {
        const DWORD err = GetLastError();
        printf("  write FAILED (Win32 error %lu) - the report was refused%s\n", err,
               err == ERROR_DEVICE_NOT_CONNECTED ? " (1167: nothing is connected behind this interface)" : "");
        return false;
    }
    puts("  write ok");
    return true;
}

// After a send, watch for the pad going quiet. A powered-off pad stops reporting
// within a report interval or two, so a few seconds is plenty; a pad that is
// merely idle still streams state, so silence is a real signal.
void Observe(double sentAtMs) {
    constexpr double kWatchMs = 4000.0;
    constexpr double kQuietMs = 1000.0;
    const uint64_t before = g_stateReports.load();
    while (NowMs() - sentAtMs < kWatchMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (SilenceMs() >= kQuietMs) {
            // The last report is the moment the pad actually went quiet.
            printf("  -> input reports STOPPED %.2f s after the send. The pad likely powered off.\n"
                   "     Press the Steam button to wake it, then restart this tool.\n",
                   std::max(g_lastStateMs.load() - sentAtMs, 0.0) / 1000.0);
            return;
        }
    }
    printf("  -> still streaming (%llu reports in the %.0f s since the send). The pad ignored it.\n",
           static_cast<unsigned long long>(g_stateReports.load() - before), kWatchMs / 1000.0);
}

bool Confirm(const char* what) {
    printf("%s\nThis may switch the controller off. Type 'yes' to send: ", what);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return false;
    return std::strncmp(line, "yes", 3) == 0;
}

std::vector<std::string> Split(const std::string& line) {
    std::istringstream is(line);
    std::vector<std::string> out;
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

bool ParseHex(const std::string& s, uint8_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (end == s.c_str() || *end != '\0' || v > 0xFF) return false;
    out = static_cast<uint8_t>(v);
    return true;
}

void PrintHelp() {
    puts(
        "\nPowerOffProbe - does the pad have a software power-off command?\n"
        "\n"
        "  list                       the built-in candidates\n"
        "  send <n>                   send candidate n, then watch whether the pad goes quiet\n"
        "  raw <cmd> [payload hex..]  send an arbitrary command in feature report 01, e.g.\n"
        "                             'raw 9f 6f 66 66 21'. The size byte is filled in.\n"
        "  rawid <id> <cmd> [hex..]   same, but in a feature report ID of your choosing\n"
        "  watch                      show the live button bytes and report silence; press\n"
        "                             Steam + Y to see which bits Steam itself would see\n"
        "  status                     reports seen and how long the pad has been silent\n"
        "  q                          quit\n"
        "\n"
        "A write that succeeds only means the host handed the report over. What counts is\n"
        "whether the pad then stops reporting. If it powers off, press Steam to wake it.\n");
}

void PrintList() {
    puts("\nCandidates:");
    for (int i = 0; i < kVariantCount; ++i) {
        const Variant& v = kVariants[i];
        printf("  %d  %-28s  frame: %02X %02X %02X %s\n", i + 1, v.name, v.reportId,
               kCmdTurnOff, static_cast<unsigned>(v.payload.size()), Hex(v.payload).c_str());
    }
    puts("");
}

void Watch() {
    puts("Watching - hold Steam + Y, then any key to stop. Bytes are buf[2..5].");
    while (!_kbhit() && !g_quit.load()) {
        const uint8_t b2 = g_lastButtons[0].load();
        const uint8_t b4 = g_lastButtons[2].load();
        printf("\r %02X %02X %02X %02X   %s %s   silent %5.0f ms   ",
               b2, g_lastButtons[1].load(), b4, g_lastButtons[3].load(),
               (b4 & SteamController::BTN_STEAM) ? "STEAM" : "     ",
               (b2 & SteamController::BTN_Y) ? "Y" : " ", std::max(SilenceMs(), 0.0));
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    while (_kbhit()) (void)_getch();
    puts("");
}

}  // namespace

int main(int argc, char** argv) {
    auto paths = SteamController::EnumerateAll();
    if (paths.empty()) {
        puts("No Steam Controller found (wired PID=1302, BT 1303 or dongle PID=1304).");
        return 1;
    }

    // A puck publishes an interface per slot, and an empty slot rejects feature
    // reports with error 1167 (device not connected). So rather than trust #0,
    // listen on each interface and take the first one that is streaming state.
    printf("%zu interface(s):\n", paths.size());
    size_t which = paths.size();
    for (size_t i = 0; i < paths.size(); ++i) {
        bool live = false;
        {
            HidDevice t;
            if (t.Open(paths[i])) {
                uint8_t buf[64] = {};
                const size_t n = t.ReadInputReport(buf, sizeof(buf), 500);
                live = n > 0 && SteamController::IsStateReportId(buf[0]);
            }
        }
        printf("  #%zu  %-9s  %s   %ls\n", i,
               SteamController::TransportName(SteamController::TransportFromPath(paths[i])),
               live ? "LIVE" : "no reports", paths[i].c_str());
        if (live && which == paths.size()) which = i;
    }

    if (argc > 1) {
        which = static_cast<size_t>(std::strtoul(argv[1], nullptr, 10));
        if (which >= paths.size()) {
            printf("Interface %zu does not exist (%zu found).\n", which, paths.size());
            return 1;
        }
    } else if (which == paths.size()) {
        puts("\nNo interface is producing state reports. Wake the controller (press Steam) and\n"
             "run again, or force one with 'PowerOffProbe <n>'.");
        return 1;
    }

    HidDevice dev;
    if (!dev.Open(paths[which])) {
        puts("Failed to open HID device.");
        return 1;
    }
    printf("\nUsing #%zu (%s)\n", which,
           SteamController::TransportName(SteamController::TransportFromPath(paths[which])));
    puts("Reading shared - lizard mode and Steam are untouched.");

    std::thread reader(ReaderThread, &dev);

    // Give the reader a moment so the first `status` is meaningful.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (g_stateReports.load() == 0)
        puts("WARNING: no state reports yet - a 'went quiet' result would be meaningless.\n"
             "         Move a stick; if still nothing, try another interface number.");

    PrintHelp();
    PrintList();

    char lineBuf[256];
    while (true) {
        printf("> ");
        fflush(stdout);
        if (!fgets(lineBuf, sizeof(lineBuf), stdin)) break;

        const auto toks = Split(lineBuf);
        if (toks.empty()) continue;
        const std::string& cmd = toks[0];

        if (cmd == "q" || cmd == "quit") break;
        if (cmd == "?" || cmd == "help") { PrintHelp(); continue; }
        if (cmd == "list") { PrintList(); continue; }
        if (cmd == "watch") { Watch(); continue; }

        if (cmd == "status") {
            printf("State reports: %llu, silent for %.0f ms\n",
                   static_cast<unsigned long long>(g_stateReports.load()),
                   std::max(SilenceMs(), 0.0));
            continue;
        }

        if (cmd == "send") {
            const int n = toks.size() >= 2 ? std::atoi(toks[1].c_str()) : 0;
            if (n < 1 || n > kVariantCount) { printf("usage: send <1-%d>\n", kVariantCount); continue; }
            const Variant& v = kVariants[n - 1];
            if (!Confirm(v.name)) { puts("Not sent."); continue; }
            const double t = NowMs();
            if (SendCommand(dev, v.reportId, kCmdTurnOff, v.payload)) Observe(t);
            continue;
        }

        if (cmd == "raw" || cmd == "rawid") {
            const bool withId = cmd == "rawid";
            const size_t first = withId ? 2 : 1;
            if (toks.size() < first + 1) {
                puts(withId ? "usage: rawid <id> <cmd> [payload hex..]" : "usage: raw <cmd> [payload hex..]");
                continue;
            }
            uint8_t id = SteamController::FEATURE_REPORT_CMD;
            uint8_t c = 0;
            bool ok = (!withId || ParseHex(toks[1], id)) && ParseHex(toks[first], c);
            std::vector<uint8_t> payload;
            for (size_t i = first + 1; ok && i < toks.size(); ++i) {
                uint8_t b = 0;
                ok = ParseHex(toks[i], b);
                payload.push_back(b);
            }
            if (!ok) { puts("Bytes are hex, 00..FF."); continue; }
            char what[96];
            snprintf(what, sizeof(what), "raw: report %02X cmd %02X, %zu payload byte(s)", id, c, payload.size());
            if (!Confirm(what)) { puts("Not sent."); continue; }
            const double t = NowMs();
            if (SendCommand(dev, id, c, payload)) Observe(t);
            continue;
        }

        puts("Unknown command. 'help' lists them.");
    }

    g_quit.store(true);
    reader.join();
    return 0;
}
