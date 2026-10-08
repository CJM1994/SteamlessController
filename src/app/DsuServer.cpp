// Winsock before Windows.h, or Windows.h drags in the old winsock.h first.
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "DsuServer.h"
#include "EventLog.h"
#include <cstring>

namespace {

constexpr uint16_t kProtocolVersion = 1001;
constexpr size_t   kHeaderLen       = 16;   // magic, version, length, crc, id
constexpr size_t   kHeaderLenFull   = 20;   // ... plus the message type
constexpr size_t   kControllerHeaderLen = 11;
constexpr size_t   kCrcOffset       = 8;
constexpr size_t   kMaxSubscribers  = 16;
constexpr auto     kClientTimeout   = std::chrono::seconds(5);

constexpr uint32_t kMsgVersion = 0x100000;
constexpr uint32_t kMsgPorts   = 0x100001;
constexpr uint32_t kMsgData    = 0x100002;

uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    });
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// CRC of a message with its own CRC field taken as zero.
uint32_t CrcOverZeroed(const uint8_t* msg, size_t len) {
    uint8_t copy[2048];
    if (len > sizeof(copy)) return 0;
    memcpy(copy, msg, len);
    memset(copy + kCrcOffset, 0, 4);
    return Crc32(copy, len);
}

void FinalizeCrc(uint8_t* out, size_t len) {
    const uint32_t crc = CrcOverZeroed(out, len);
    memcpy(out + kCrcOffset, &crc, 4);
}

void SlotMac(uint8_t slot, uint8_t mac[6]) {
    const uint8_t base[6] = { 0x02, 0x28, 0xDE, 0x13, 0x04, slot };
    memcpy(mac, base, 6);
}

template <typename T> T ReadLe(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

}  // namespace

bool DsuServer::Key::operator<(const Key& o) const {
    if (clientId != o.clientId) return clientId < o.clientId;
    if (regType != o.regType) return regType < o.regType;
    if (slot != o.slot) return slot < o.slot;
    return memcmp(mac, o.mac, 6) < 0;
}

bool DsuServer::Start(uint16_t port, bool exposeToNetwork, std::wstring& error) {
    Stop();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = L"Windows networking could not be started.";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        error = L"No UDP socket could be created.";
        WSACleanup();
        return false;
    }
    // Loopback only unless asked otherwise: nothing else on the network has
    // any business reading the controller.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(exposeToNetwork ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        error = err == WSAEADDRINUSE
                    ? L"Port " + std::to_wstring(port) + L" is already in use - is sc2dsu "
                      L"or another DSU server running?"
                    : L"Port " + std::to_wstring(port) + L" could not be opened (error "
                      + std::to_wstring(err) + L").";
        closesocket(s);
        WSACleanup();
        return false;
    }
    // The receive loop wakes this often to expire subscribers and notice Stop.
    DWORD timeoutMs = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof(timeoutMs));

    m_socket   = static_cast<uintptr_t>(s);
    m_port     = port;
    m_serverId = static_cast<uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() << 16));
    for (auto& c : m_connected) c = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_subscribers.clear();
        m_lastInterest = {};
    }
    m_running = true;
    m_thread  = std::thread(&DsuServer::Run, this);
    EventLog::Write("DSU: serving motion on %s:%u", exposeToNetwork ? "0.0.0.0" : "127.0.0.1",
                    port);
    return true;
}

void DsuServer::Stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    closesocket(static_cast<SOCKET>(m_socket));
    m_socket = ~uintptr_t(0);
    WSACleanup();
    std::lock_guard<std::mutex> lk(m_mutex);
    m_subscribers.clear();
    EventLog::Write("DSU: stopped");
}

bool DsuServer::WantsMotion() const {
    if (!m_running.load()) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    return !m_subscribers.empty()
        || std::chrono::steady_clock::now() - m_lastInterest < kClientTimeout;
}

int DsuServer::Subscribers() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_subscribers.size());
}

void DsuServer::SetConnected(int slot, bool connected) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    m_connected[slot] = connected;
}

void DsuServer::Run() {
    uint8_t buf[2048];
    auto lastCleanup = std::chrono::steady_clock::now();
    while (m_running.load()) {
        sockaddr_storage from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(static_cast<SOCKET>(m_socket), reinterpret_cast<char*>(buf),
                               sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n > 0) HandleRequest(buf, static_cast<size_t>(n), &from, fromLen);
        if (std::chrono::steady_clock::now() - lastCleanup >= std::chrono::seconds(1)) {
            Cleanup();
            lastCleanup = std::chrono::steady_clock::now();
        }
    }
}

void DsuServer::HandleRequest(const uint8_t* msg, size_t len, const void* from, int fromLen) {
    if (len < kHeaderLenFull || memcmp(msg, "DSUC", 4) != 0) return;
    if (ReadLe<uint16_t>(msg + 4) != kProtocolVersion) return;
    const size_t length = ReadLe<uint16_t>(msg + 6);
    if (len < kHeaderLen + length) return;
    if (ReadLe<uint32_t>(msg + kCrcOffset) != CrcOverZeroed(msg, kHeaderLen + length)) return;

    const uint32_t clientId = ReadLe<uint32_t>(msg + 12);
    const uint32_t type     = ReadLe<uint32_t>(msg + 16);
    const uint8_t* body     = msg + kHeaderLenFull;
    const size_t   bodyLen  = kHeaderLen + length - kHeaderLenFull;

    if (type == kMsgVersion) {
        SendVersion(from, fromLen);
    } else if (type == kMsgPorts) {
        if (bodyLen < 4) return;
        const uint32_t amount = (std::min)(ReadLe<uint32_t>(body), uint32_t(MAX_SLOTS));
        if (bodyLen < 4 + amount) return;
        if (amount) MarkInterest();
        for (uint32_t i = 0; i < amount; ++i) SendSlotInfo(from, fromLen, body[4 + i]);
    } else if (type == kMsgData) {
        if (bodyLen < 8) return;
        Key key{};
        key.clientId = clientId;
        key.regType  = body[0];
        key.slot     = body[1];
        memcpy(key.mac, body + 2, 6);
        bool wantsUs = key.regType == 0 || ((key.regType & 1) && key.slot < MAX_SLOTS);
        for (uint8_t s = 0; !wantsUs && (key.regType & 2) && s < MAX_SLOTS; ++s) {
            uint8_t mac[6];
            SlotMac(s, mac);
            wantsUs = memcmp(mac, key.mac, 6) == 0;
        }
        if (!wantsUs) return;

        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_subscribers.find(key);
        if (it == m_subscribers.end()) {
            if (m_subscribers.size() >= kMaxSubscribers) return;
            it = m_subscribers.emplace(key, Subscriber{}).first;
            EventLog::Write("DSU: emulator subscribed (client %08X, slot %u)", clientId, key.slot);
        }
        memcpy(it->second.addr, from, (std::min)(fromLen, int(sizeof(it->second.addr))));
        it->second.addrLen     = fromLen;
        it->second.lastRequest = std::chrono::steady_clock::now();
        m_lastInterest         = it->second.lastRequest;
    }
}

void DsuServer::Header(uint8_t* out, size_t len, uint32_t type) const {
    memcpy(out, "DSUS", 4);
    memcpy(out + 4, &kProtocolVersion, 2);
    const uint16_t payload = static_cast<uint16_t>(len - kHeaderLen);
    memcpy(out + 6, &payload, 2);
    memset(out + kCrcOffset, 0, 4);
    memcpy(out + 12, &m_serverId, 4);
    memcpy(out + 16, &type, 4);
}

void DsuServer::WriteControllerHeader(uint8_t* out, uint8_t slot, bool connected) const {
    out[0] = slot;
    if (!connected) return;
    out[1] = 2;  // connected
    out[2] = 2;  // full gyro
    out[3] = 1;  // USB
    SlotMac(slot, out + 4);
    out[10] = 0;  // battery not reported
}

void DsuServer::SendVersion(const void* to, int toLen) {
    uint8_t out[kHeaderLenFull + 2] = {};
    Header(out, sizeof(out), kMsgVersion);
    memcpy(out + kHeaderLenFull, &kProtocolVersion, 2);
    FinalizeCrc(out, sizeof(out));
    sendto(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(out), sizeof(out), 0,
           static_cast<const sockaddr*>(to), toLen);
}

void DsuServer::SendSlotInfo(const void* to, int toLen, uint8_t slot) {
    uint8_t out[kHeaderLenFull + 12] = {};
    Header(out, sizeof(out), kMsgPorts);
    WriteControllerHeader(out + kHeaderLenFull, slot,
                          slot < MAX_SLOTS && m_connected[slot].load());
    FinalizeCrc(out, sizeof(out));
    sendto(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(out), sizeof(out), 0,
           static_cast<const sockaddr*>(to), toLen);
}

void DsuServer::Cleanup() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto it = m_subscribers.begin(); it != m_subscribers.end();) {
        if (now - it->second.lastRequest >= kClientTimeout) {
            EventLog::Write("DSU: emulator stopped asking (client %08X)", it->first.clientId);
            it = m_subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

void DsuServer::MarkInterest() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lastInterest = std::chrono::steady_clock::now();
}

bool DsuServer::WantsSlot(const Key& key, uint8_t slot) const {
    if (key.regType == 0) return true;
    if ((key.regType & 1) && key.slot == slot) return true;
    if (key.regType & 2) {
        uint8_t mac[6];
        SlotMac(slot, mac);
        return memcmp(mac, key.mac, 6) == 0;
    }
    return false;
}

void DsuServer::Publish(int slot, const DsuSample& s) {
    if (!m_running.load() || slot < 0 || slot >= MAX_SLOTS) return;
    m_connected[slot] = true;

    // Header, controller header, then the 69-byte data body sc2dsu writes.
    uint8_t out[kHeaderLenFull + 80] = {};
    Header(out, sizeof(out), kMsgData);
    WriteControllerHeader(out + kHeaderLenFull, static_cast<uint8_t>(slot), true);
    uint8_t* body = out + kHeaderLenFull + kControllerHeaderLen;
    body[0] = 1;  // connected
    // body[1..4] is the per-subscriber packet number, filled in below.
    body[5] = s.buttons1;
    body[6] = s.buttons2;
    body[7] = s.home;
    body[8] = s.touchButton;
    body[9]  = s.leftX;
    body[10] = s.leftY;
    body[11] = s.rightX;
    body[12] = s.rightY;
    memcpy(body + 13, s.analog, 12);
    // body[25..36]: two touch points, six bytes each.
    for (int i = 0; i < 2; ++i) {
        uint8_t* t = body + 25 + i * 6;
        t[0] = s.touch[i].active ? 1 : 0;
        t[1] = s.touch[i].id;
        memcpy(t + 2, &s.touch[i].x, 2);
        memcpy(t + 4, &s.touch[i].y, 2);
    }
    memcpy(body + 37, &s.timestampUs, 8);
    const float motion[6] = { s.accelG[0], s.accelG[1], s.accelG[2],
                              s.gyroDps[0], s.gyroDps[1], s.gyroDps[2] };
    memcpy(body + 45, motion, sizeof(motion));

    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [key, sub] : m_subscribers) {
        if (!WantsSlot(key, static_cast<uint8_t>(slot))) continue;
        const uint32_t counter = ++sub.packetCounters[slot];
        memcpy(body + 1, &counter, 4);
        FinalizeCrc(out, sizeof(out));
        sendto(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(out), sizeof(out), 0,
               reinterpret_cast<const sockaddr*>(sub.addr), sub.addrLen);
    }
}
