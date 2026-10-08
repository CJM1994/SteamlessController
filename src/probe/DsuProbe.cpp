// Console DSU client, for checking the tray app's gyro server without an
// emulator: asks for the protocol version and slot 0's info, subscribes to its
// data, checks every packet's CRC, and prints the motion a few times a second.
//
//   DsuProbe [seconds] [port]   listen to a running server (default 15 s, 26760)
//   DsuProbe --selftest         serve synthetic samples on a spare port and
//                               read them back — the protocol, end to end,
//                               with no controller and no tray app
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "app/DsuMotion.h"
#include "app/DsuServer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

uint32_t Crc32(const uint8_t* d, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

bool CrcOk(const uint8_t* msg, size_t len) {
    if (len < 20) return false;
    uint8_t copy[2048];
    memcpy(copy, msg, len);
    uint32_t claimed;
    memcpy(&claimed, copy + 8, 4);
    memset(copy + 8, 0, 4);
    return Crc32(copy, len) == claimed;
}

// A client message: header, type, payload, CRC.
size_t Build(uint8_t* out, uint32_t type, const uint8_t* payload, size_t payloadLen) {
    memcpy(out, "DSUC", 4);
    const uint16_t version = 1001;
    memcpy(out + 4, &version, 2);
    const uint16_t length = static_cast<uint16_t>(4 + payloadLen);  // type + payload
    memcpy(out + 6, &length, 2);
    memset(out + 8, 0, 4);
    const uint32_t clientId = 0x5052424Fu;  // "OBRP"
    memcpy(out + 12, &clientId, 4);
    memcpy(out + 16, &type, 4);
    memcpy(out + 20, payload, payloadLen);
    const size_t total = 20 + payloadLen;
    const uint32_t crc = Crc32(out, total);
    memcpy(out + 8, &crc, 4);
    return total;
}

int Listen(int seconds, uint16_t port, bool quiet, int* dataPackets, float* lastGyroX) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
    auto send = [&](uint32_t type, const uint8_t* p, size_t n) {
        uint8_t msg[64];
        const size_t len = Build(msg, type, p, n);
        sendto(s, reinterpret_cast<const char*>(msg), static_cast<int>(len), 0,
               reinterpret_cast<sockaddr*>(&server), sizeof(server));
    };
    const uint8_t portsReq[5] = { 1, 0, 0, 0, 0 };        // one slot: 0
    const uint8_t dataReq[8]  = { 1, 0, 0, 0, 0, 0, 0, 0 }; // by slot, slot 0
    send(0x100000, nullptr, 0);
    send(0x100001, portsReq, sizeof(portsReq));
    send(0x100002, dataReq, sizeof(dataReq));

    int data = 0, bad = 0;
    float gx = 0;
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    ULONGLONG lastPrint = 0, lastRenew = GetTickCount64();
    while (GetTickCount64() < end) {
        if (GetTickCount64() - lastRenew > 1000) {  // renew well inside the 5 s timeout
            send(0x100002, dataReq, sizeof(dataReq));
            lastRenew = GetTickCount64();
        }
        uint8_t buf[2048];
        const int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) continue;
        if (n < 20 || memcmp(buf, "DSUS", 4) != 0 || !CrcOk(buf, static_cast<size_t>(n))) {
            ++bad;
            continue;
        }
        uint32_t type;
        memcpy(&type, buf + 16, 4);
        if (type == 0x100000 && !quiet) {
            uint16_t v; memcpy(&v, buf + 20, 2);
            printf("version: %u\n", v);
        } else if (type == 0x100001 && !quiet) {
            printf("slot %u: %s\n", buf[20], buf[21] == 2 ? "connected" : "not connected");
        } else if (type == 0x100002 && n >= 100) {
            ++data;
            float m[6];
            memcpy(m, buf + 20 + 11 + 45, sizeof(m));
            gx = m[3];
            if (!quiet && GetTickCount64() - lastPrint >= 200) {
                lastPrint = GetTickCount64();
                const uint8_t* t = buf + 20 + 11 + 25;  // first touch point
                uint16_t tx, ty;
                memcpy(&tx, t + 2, 2);
                memcpy(&ty, t + 4, 2);
                printf("accel g %6.2f %6.2f %6.2f   gyro dps %7.1f %7.1f %7.1f   buttons %02X %02X"
                       "   touch %s %4u,%3u (id %u)\n",
                       m[0], m[1], m[2], m[3], m[4], m[5], buf[20 + 11 + 5], buf[20 + 11 + 6],
                       t[0] ? "on " : "off", tx, ty, t[1]);
            }
        }
    }
    closesocket(s);
    if (!quiet)
        printf("\n%d data packets in %d s (%.0f/s), %d rejected\n", data, seconds,
               static_cast<double>(data) / seconds, bad);
    if (dataPackets) *dataPackets = data;
    if (lastGyroX) *lastGyroX = gx;
    return data > 0 && bad == 0 ? 0 : 1;
}

int SelfTest() {
    const uint16_t port = 26799;
    DsuServer server;
    std::wstring error;
    if (!server.Start(port, false, error)) {
        printf("selftest: server did not start: %ls\n", error.c_str());
        return 1;
    }
    std::atomic<bool> run{true};
    std::thread feeder([&] {
        uint64_t t = 0;
        while (run) {
            DsuSample s;
            s.timestampUs = t += 4000;
            s.accelG[1]   = 1.0f;
            s.gyroDps[0]  = 12.5f;
            s.buttons2    = 0x20;  // A
            server.Publish(0, s);
            Sleep(4);
        }
    });
    int packets = 0;
    float gx = 0;
    const int rc = Listen(2, port, true, &packets, &gx);
    run = false;
    feeder.join();
    const bool interest = server.Subscribers() > 0;
    server.Stop();
    const bool ok = rc == 0 && packets > 100 && std::fabs(gx - 12.5f) < 1e-4f && interest;
    printf("selftest: %d packets, gyro x %.2f (want 12.50), subscriber seen %s -> %s\n",
           packets, gx, interest ? "yes" : "no", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// sc2dsu's calibration tests: a still controller's bias is learned, and
// shaking it does neither lock on nor wipe what was learned.
int CalibrationTest() {
    const float dt = 1.0f / 250.0f;
    const float gravity[3] = { 0, 1, 0 };
    const float bias[3]    = { 0.5f, -0.3f, 0.1f };
    bool ok = true;

    DsuGyroCalibration cal;
    for (int i = 0; i < static_cast<int>(3.0f / dt); ++i) {
        float g[3] = { bias[0], bias[1], bias[2] };
        cal.Correct(g, gravity, dt);
    }
    float out[3] = { bias[0], bias[1], bias[2] };
    cal.Correct(out, gravity, dt);
    const bool locked = cal.Steady() && std::fabs(out[0]) < 0.05f && std::fabs(out[1]) < 0.05f
                     && std::fabs(out[2]) < 0.05f;
    printf("calibration: still for 3 s -> corrected %.3f %.3f %.3f, steady %d -> %s\n",
           out[0], out[1], out[2], cal.Steady(), locked ? "PASS" : "FAIL");
    ok = ok && locked;

    for (int i = 0; i < 50; ++i) {
        const float w = std::sin(i * 0.9f) * 80.0f;
        float g[3] = { w + bias[0], w + bias[1], w + bias[2] };
        cal.Correct(g, gravity, dt);
    }
    float after[3] = { bias[0], bias[1], bias[2] };
    cal.Correct(after, gravity, dt);
    const bool kept = !cal.Steady() && std::fabs(after[0]) < 0.05f;
    printf("calibration: shaken after lock -> bias kept %s -> %s\n", kept ? "yes" : "no",
           kept ? "PASS" : "FAIL");
    ok = ok && kept;

    DsuGyroCalibration moving;
    for (int i = 0; i < 2000; ++i) {
        const float w = std::sin(i * 0.3f) * 50.0f;
        float g[3] = { w, w, w };
        moving.Correct(g, gravity, dt);
    }
    float probe[3] = { 0, 0, 0 };
    moving.Correct(probe, gravity, dt);
    const bool noLock = !moving.Steady() && probe[0] == 0.0f;
    printf("calibration: never still -> no bias learned -> %s\n", noLock ? "PASS" : "FAIL");
    ok = ok && noLock;

    DsuAxisMap map;
    const bool parsed = DsuAxisMap::Parse(L"x,-z,y", map) && map.ToString() == L"x,-z,y"
                     && !DsuAxisMap::Parse(L"x,q,y", map);
    printf("axis map: parse/print round trip -> %s\n", parsed ? "PASS" : "FAIL");
    return ok && parsed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    int rc;
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        rc = SelfTest() | CalibrationTest();
    } else {
        const int seconds = argc > 1 ? atoi(argv[1]) : 15;
        const uint16_t port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 26760;
        printf("Listening to 127.0.0.1:%u slot 0 for %d s. Put the app in Steamless Mode "
               "and move the controller.\n", port, seconds > 0 ? seconds : 15);
        rc = Listen(seconds > 0 ? seconds : 15, port, false, nullptr, nullptr);
    }
    WSACleanup();
    return rc;
}
