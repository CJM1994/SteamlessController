#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// A Cemuhook / DSU motion server, ported from sc2dsu's dsu.rs
// (https://github.com/KillerPixelCrew/sc2dsu, MIT).
//
// Emulators that want gyro but cannot read the Steam Controller themselves —
// Cemu, Azahar/Citra, Ryujinx — ask a DSU server for it over UDP on
// 127.0.0.1:26760. Here it is fed from the read loop that already drives the
// virtual pad in Steamless Mode, so the emulator takes buttons from the
// Xbox/PlayStation pad and motion from this.
//
// Protocol version 1001: 20-byte headers with a CRC32 over the whole message,
// four controller slots, clients subscribing per slot or per MAC and renewing
// at least every five seconds.
//
// The socket has a thread of its own for requests. Samples are sent from the
// controllers' read threads as they arrive — Publish takes the subscriber lock
// briefly and sends straight away, so motion is never queued behind anything.

// One frame of controller state, already in DSU's units.
struct DsuSample {
    uint64_t timestampUs = 0;
    float    accelG[3]   = {};  // g
    float    gyroDps[3]  = {};  // degrees per second
    uint8_t  buttons1 = 0, buttons2 = 0, home = 0, touchButton = 0;
    uint8_t  leftX = 128, leftY = 128, rightX = 128, rightY = 128;
    // D-pad left/down/right/up, Y, B, A, X, R1, L1, R2, L2.
    uint8_t  analog[12] = {};
    // Up to two touch contacts, in a DualShock 4 touchpad's coordinates
    // (0..1919 across, 0..942 down) — the range Citra-family emulators
    // calibrate a DSU touch provider against by default. The id changes with
    // each new contact.
    struct Touch {
        bool     active = false;
        uint8_t  id     = 0;
        uint16_t x = 0, y = 0;
    } touch[2];
};

class DsuServer {
public:
    static constexpr int MAX_SLOTS    = 4;
    static constexpr uint16_t DEFAULT_PORT = 26760;

    ~DsuServer() { Stop(); }

    // Binds and starts serving. On failure — the port taken, most likely by
    // a standalone sc2dsu still running — returns false with the reason.
    bool Start(uint16_t port, bool exposeToNetwork, std::wstring& error);
    void Stop();
    bool Running() const { return m_running.load(); }
    uint16_t Port() const { return m_port; }

    // Whether anyone is asking for motion: a subscription, or a client that
    // looked at the slots in the last few seconds. The read loops switch the
    // controller's motion sensors on only while this holds.
    bool WantsMotion() const;
    int  Subscribers() const;

    // From the read threads. A slot is a DSU port number, 0..3.
    void SetConnected(int slot, bool connected);
    void Publish(int slot, const DsuSample& sample);

private:
    struct Key {
        uint32_t clientId;
        uint8_t  regType, slot;
        uint8_t  mac[6];
        bool operator<(const Key& o) const;
    };
    struct Subscriber {
        // sockaddr_in6 sized storage, kept opaque here to keep Winsock out of
        // this header.
        unsigned char addr[28] = {};
        int           addrLen  = 0;
        std::chrono::steady_clock::time_point lastRequest;
        uint32_t      packetCounters[MAX_SLOTS] = {};
    };

    void Run();
    void HandleRequest(const uint8_t* msg, size_t len, const void* from, int fromLen);
    void SendVersion(const void* to, int toLen);
    void SendSlotInfo(const void* to, int toLen, uint8_t slot);
    void Cleanup();
    void MarkInterest();
    bool WantsSlot(const Key& key, uint8_t slot) const;
    void Header(uint8_t* out, size_t len, uint32_t type) const;
    void WriteControllerHeader(uint8_t* out, uint8_t slot, bool connected) const;

    uintptr_t          m_socket  = ~uintptr_t(0);  // SOCKET
    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    uint16_t           m_port    = DEFAULT_PORT;
    uint32_t           m_serverId = 0;

    mutable std::mutex m_mutex;  // the subscribers and the interest time
    std::map<Key, Subscriber> m_subscribers;
    std::chrono::steady_clock::time_point m_lastInterest{};
    std::atomic<bool>  m_connected[MAX_SLOTS] = {};
};
