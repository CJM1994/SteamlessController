#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

// Client for the Steam Input gate from steam-input-lease
// (https://github.com/KillerPixelCrew/steam-input-lease, MIT).
//
// The gate is a proxy DLL that Steam loads from its own folder (as
// XInput1_4.dll) at a cold start. It sits idle until a client takes a lease
// over its named pipe; while at least one lease is held, Steam's HID opens and
// reads and its XInput queries fail inside steam.exe, so Steam Input lets go of
// every controller for every game at once. A lease IS an open pipe connection:
// closing it, or this process dying, ends the block, and the gate then asks
// Steam to rediscover controllers. That is what makes holding one safe — a
// crash here can never leave Steam locked out.
//
// Nothing here deploys the gate or injects anything into Steam. A Steam that
// was started without the gate in its folder simply has no pipe, which reports
// as GateNotLoaded.

// The synchronous protocol layer. Every call may block for up to its timeout;
// keep it off the UI thread (SteamInputLease below does).
namespace SteamInputGate {

// Wire protocol v1, shared with the gate's #[repr(C)] structs. Fixed-width and
// naturally aligned, so no packing pragma is needed — the static_asserts in the
// .cpp hold the layout to the 8 and 24 bytes the gate expects.
constexpr uint32_t kMagic   = 0x53494754;  // "SIGT"
constexpr uint16_t kVersion = 1;

enum class Command : uint16_t {
    AcquireLease = 1,
    QueryStatus  = 2,
    ReleaseLease = 3,
};

// The gate runs its own two-pass controller rediscovery when the last lease
// goes. Without it, Steam needs a nudge from outside to look again.
constexpr uint16_t kCapInternalRecovery = 1 << 0;

struct Status {
    uint16_t capabilities           = 0;
    uint32_t leaseCount             = 0;  // block leases held across all clients
    uint32_t hidHandleCount         = 0;  // Steam HID handles the gate is tracking
    uint32_t lastRevokedHandleCount = 0;  // handles closed when blocking last began
    bool InternalRecovery() const { return (capabilities & kCapInternalRecovery) != 0; }
};

enum class Error {
    None,
    SteamNotRunning,  // no steam.exe in this Windows session
    SteamAmbiguous,   // more than one; refusing to guess which one to block
    GateNotLoaded,    // Steam is up but nothing answers on its pipe
    AccessDenied,     // the pipe exists but its DACL refused us
    ForeignServer,    // someone other than Steam created the pipe name
    Rejected,         // the gate answered with a failure (hook install failed)
    BadResponse,      // short, malformed or wrong-version response
    Io,               // a pipe read or write failed or timed out
};
const char* Describe(Error error);

// steam.exe in our own session, or 0 with `error` saying why not. Session-
// scoped like upstream: another signed-in user's Steam is not ours to block.
DWORD FindSteam(Error& error);

// Opens the gate's pipe in `steamPid`, retrying for up to timeoutMs while the
// pipe has not come up yet (it appears shortly after Steam starts). The handle
// is overlapped and verified to be served by steamPid itself. Returns
// INVALID_HANDLE_VALUE on failure.
HANDLE Connect(DWORD steamPid, DWORD timeoutMs, Error& error);

// One request and its response on a pipe from Connect.
Error Exchange(HANDLE pipe, Command command, Status& status, DWORD timeoutMs);

// Connect + QueryStatus + close. Never changes the lease count. A connect
// timeout of 0 makes one attempt — "is the gate loaded", answered at once.
Error QueryStatus(DWORD steamPid, Status& status, DWORD connectTimeoutMs = 500);

// The fallback when a release lifted blocking but the gate could not run its
// own recovery: post the same harmless device-change notice upstream's host
// sends, so Steam has a reason to look for controllers again.
void NudgeSteamRediscovery(DWORD steamPid);

}  // namespace SteamInputGate

// Owns at most one lease and keeps it in step with what the caller wants,
// across Steam starting late, exiting and restarting. All pipe work happens on
// one worker thread; SetWanted only records intent and wakes it.
class SteamInputLease {
public:
    enum class State {
        Off,            // not wanted, nothing held
        NoSteam,        // wanted, but Steam is not running — nothing to block
        GateMissing,    // wanted, Steam is running without the gate loaded
        Failed,         // wanted, gate present but refused or misbehaved; retrying
        Blocking,       // lease held: Steam has no access to controllers
        Releasing,      // lease released; waiting for Steam to reopen its
                        // controllers (bounded), then Off
    };
    static const char* Describe(State state);

    // Runs on the worker thread. Marshal before touching UI (PostMessage).
    using ChangedFn = std::function<void(State)>;

    ~SteamInputLease() { Stop(); }

    void Start(ChangedFn onChanged);
    // Releases any lease (explicitly, so the gate reports its outcome) and
    // joins the worker.
    void Stop();

    // Idempotent. The single source of intent; the worker converges on it.
    void SetWanted(bool block);
    bool Wanted() const { return m_wanted.load(); }

    // Ask the worker to re-check now instead of at its next retry — e.g. the
    // Steam watcher has just seen Steam start.
    void Poke();

    State GetState() const { return m_state.load(); }
    // What the gate reported when the current lease was taken.
    SteamInputGate::Status LastStatus() const;

private:
    struct Held {
        HANDLE pipe    = INVALID_HANDLE_VALUE;
        HANDLE process = nullptr;  // steam.exe, SYNCHRONIZE — signalled on exit
        DWORD  pid     = 0;
        bool   IsHeld() const { return pipe != INVALID_HANDLE_VALUE; }
        void   Close();
    };

    void  Worker();
    // Returns how long to wait before trying again, INFINITE once held.
    DWORD TryAcquire(Held& held);
    // Returns whether Steam was asked to rediscover — this was the last
    // lease — and so whether there is a reopen worth waiting for.
    bool  Release(Held& held);
    // After the last lease goes: wait until Steam has HID handles open again,
    // so whoever is watching knows the handoff is finished rather than merely
    // started. Gives up early if blocking is wanted again or on Stop.
    void  WaitForSteamReopen(DWORD pid);
    void  SetState(State state);

    ChangedFn               m_onChanged;
    std::thread             m_thread;
    HANDLE                  m_wake = nullptr;  // auto-reset
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_wanted{false};
    std::atomic<State>      m_state{State::Off};
    // Logged once per distinct failure rather than on every retry.
    SteamInputGate::Error   m_lastError = SteamInputGate::Error::None;
    mutable std::mutex      m_statusMutex;
    SteamInputGate::Status  m_lastStatus;

    // Retry spacing while wanted but not held. Steam not running costs one
    // process snapshot per try; a missing gate one failed pipe open.
    static constexpr DWORD NO_STEAM_RETRY_MS = 2000;
    static constexpr DWORD NO_GATE_RETRY_MS  = 3000;
    static constexpr DWORD FAILED_RETRY_MS   = 5000;
    // A resident gate keeps its pipe for as long as Steam runs, so a short
    // wait is enough; only a Steam that is still starting needs longer, and
    // the retry loop covers that.
    static constexpr DWORD CONNECT_TIMEOUT_MS = 1000;
    // The first acquire installs the gate's hooks and sweeps Steam's handle
    // table, and the last release triggers rediscovery — both are real work
    // inside Steam, so allow them time before calling the gate unresponsive.
    static constexpr DWORD EXCHANGE_TIMEOUT_MS = 10000;
    // The gate's second discovery pass lands about 2.2 s after release; this
    // covers it with room to spare, and caps how long Releasing can last when
    // Steam has nothing to reopen (no controller connected, say).
    static constexpr DWORD REOPEN_TIMEOUT_MS = 4000;
    static constexpr DWORD REOPEN_POLL_MS    = 250;
};
