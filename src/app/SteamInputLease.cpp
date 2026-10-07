#include "SteamInputLease.h"
#include "EventLog.h"
#include <Dbt.h>
#include <TlHelp32.h>
#include <string>

namespace SteamInputGate {

namespace {

struct Request {
    uint32_t magic;
    uint16_t version;
    uint16_t command;
};

struct Response {
    uint32_t magic;
    uint16_t version;
    uint16_t capabilities;
    uint32_t result;
    uint32_t leaseCount;
    uint32_t hidHandleCount;
    uint32_t lastRevokedHandleCount;
};

static_assert(sizeof(Request) == 8, "gate protocol v1 request is 8 bytes");
static_assert(sizeof(Response) == 24, "gate protocol v1 response is 24 bytes");

std::wstring PipeName(DWORD pid) {
    return L"\\\\.\\pipe\\SteamInputGate-" + std::to_wstring(pid);
}

// One overlapped read or write, bounded by timeoutMs. The pipe is opened
// overlapped precisely so a gate that stops answering costs a timeout here
// rather than a worker thread wedged in ReadFile forever.
bool TransferWithTimeout(HANDLE pipe, bool write, void* buffer, DWORD length,
                         DWORD timeoutMs, DWORD& transferred) {
    transferred = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    BOOL ok = write ? WriteFile(pipe, buffer, length, nullptr, &ov)
                    : ReadFile(pipe, buffer, length, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        // Wait for the cancellation to land: `ov` lives on this stack frame,
        // and the kernel must be done with it before the frame goes.
        GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        CloseHandle(ov.hEvent);
        SetLastError(WAIT_TIMEOUT);
        return false;
    }
    ok = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
    const DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    SetLastError(err);
    return ok != FALSE;
}

}  // namespace

const char* Describe(Error error) {
    switch (error) {
    case Error::None:            return "ok";
    case Error::SteamNotRunning: return "Steam is not running";
    case Error::SteamAmbiguous:  return "more than one steam.exe in this session";
    case Error::GateNotLoaded:   return "no gate answered (not installed, or Steam not restarted since)";
    case Error::AccessDenied:    return "the gate's pipe refused access";
    case Error::ForeignServer:   return "the gate's pipe is served by a process other than Steam";
    case Error::Rejected:        return "the gate refused the request (hook installation failed?)";
    case Error::BadResponse:     return "the gate sent an invalid response";
    case Error::Io:              return "pipe I/O failed or timed out";
    }
    return "unknown";
}

DWORD FindSteam(Error& error) {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        error = Error::SteamNotRunning;
        return 0;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        error = Error::SteamNotRunning;
        return 0;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    error = Error::SteamNotRunning;
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD candidateSession = 0;
            if (_wcsicmp(pe.szExeFile, L"steam.exe") != 0) continue;
            if (!ProcessIdToSessionId(pe.th32ProcessID, &candidateSession)
                    || candidateSession != session)
                continue;
            if (found != 0) {
                error = Error::SteamAmbiguous;
                CloseHandle(snap);
                return 0;
            }
            found = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (found) error = Error::None;
    return found;
}

HANDLE Connect(DWORD steamPid, DWORD timeoutMs, Error& error) {
    const std::wstring name = PipeName(steamPid);
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        // Identification-level QoS: whatever serves this name can learn who
        // we are but never act as us.
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT
                                      | SECURITY_IDENTIFICATION,
                                  nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            // The name is derived from Steam's pid, but any process of this
            // user could have created it first. A lease from an impostor would
            // report blocking that never happened.
            ULONG server = 0;
            if (!GetNamedPipeServerProcessId(pipe, &server) || server != steamPid) {
                CloseHandle(pipe);
                error = Error::ForeignServer;
                return INVALID_HANDLE_VALUE;
            }
            error = Error::None;
            return pipe;
        }

        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            error = Error::AccessDenied;
            return INVALID_HANDLE_VALUE;
        }
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PIPE_BUSY) {
            error = Error::Io;
            return INVALID_HANDLE_VALUE;
        }
        if (GetTickCount64() >= deadline) {
            error = Error::GateNotLoaded;
            return INVALID_HANDLE_VALUE;
        }
        // WaitNamedPipe returns at once when no instance exists at all, so it
        // only backs off the busy case; a missing pipe needs a sleep of its own.
        if (code == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
        else                         Sleep(50);
    }
}

Error Exchange(HANDLE pipe, Command command, Status& status, DWORD timeoutMs) {
    Request request{kMagic, kVersion, static_cast<uint16_t>(command)};
    DWORD transferred = 0;
    if (!TransferWithTimeout(pipe, true, &request, sizeof(request), timeoutMs, transferred))
        return Error::Io;
    if (transferred != sizeof(request)) return Error::Io;

    Response response{};
    if (!TransferWithTimeout(pipe, false, &response, sizeof(response), timeoutMs, transferred))
        return Error::Io;
    if (transferred != sizeof(response)
            || response.magic != kMagic || response.version != kVersion)
        return Error::BadResponse;

    status.capabilities           = response.capabilities;
    status.leaseCount             = response.leaseCount;
    status.hidHandleCount         = response.hidHandleCount;
    status.lastRevokedHandleCount = response.lastRevokedHandleCount;
    return response.result == 0 ? Error::None : Error::Rejected;
}

Error QueryStatus(DWORD steamPid, Status& status, DWORD connectTimeoutMs) {
    Error error = Error::None;
    HANDLE pipe = Connect(steamPid, connectTimeoutMs, error);
    if (pipe == INVALID_HANDLE_VALUE) return error;
    error = Exchange(pipe, Command::QueryStatus, status, 2000);
    CloseHandle(pipe);
    return error;
}

void NudgeSteamRediscovery(DWORD steamPid) {
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == static_cast<DWORD>(lp))
            PostMessageW(hwnd, WM_DEVICECHANGE, DBT_DEVNODES_CHANGED, 0);
        return TRUE;
    }, static_cast<LPARAM>(steamPid));
}

}  // namespace SteamInputGate

// ---------------------------------------------------------------------------

using SteamInputGate::Error;

const char* SteamInputLease::Describe(State state) {
    switch (state) {
    case State::Off:         return "off";
    case State::NoSteam:     return "waiting for Steam";
    case State::GateMissing: return "gate not loaded";
    case State::Failed:      return "failed";
    case State::Blocking:    return "blocking Steam Input";
    case State::Releasing:   return "handing back to Steam";
    }
    return "unknown";
}

void SteamInputLease::Held::Close() {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (process) CloseHandle(process);
    pipe    = INVALID_HANDLE_VALUE;
    process = nullptr;
    pid     = 0;
}

void SteamInputLease::Start(ChangedFn onChanged) {
    Stop();
    m_onChanged = std::move(onChanged);
    m_wake      = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running   = true;
    m_thread    = std::thread(&SteamInputLease::Worker, this);
}

void SteamInputLease::Stop() {
    if (m_running.exchange(false)) {
        SetEvent(m_wake);
        if (m_thread.joinable()) m_thread.join();
    }
    if (m_wake) {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
}

void SteamInputLease::SetWanted(bool block) {
    if (m_wanted.exchange(block) == block) return;
    EventLog::Write("LEASE: %s requested", block ? "block" : "release");
    Poke();
}

void SteamInputLease::Poke() {
    if (m_wake) SetEvent(m_wake);
}

SteamInputGate::Status SteamInputLease::LastStatus() const {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    return m_lastStatus;
}

void SteamInputLease::SetState(State state) {
    if (m_state.exchange(state) == state) return;
    EventLog::Write("LEASE: state -> %s", Describe(state));
    if (m_onChanged) m_onChanged(state);
}

void SteamInputLease::Worker() {
    Held held;
    while (m_running.load()) {
        const bool wanted = m_wanted.load();
        DWORD waitMs = INFINITE;
        if (wanted && !held.IsHeld()) {
            waitMs = TryAcquire(held);
        } else if (!wanted && held.IsHeld()) {
            const DWORD pid = held.pid;
            if (Release(held)) {
                SetState(State::Releasing);
                WaitForSteamReopen(pid);
            }
            // Wanted again while Steam was reopening: the next pass takes the
            // lease straight back, so there is no Off in between to report.
            if (!m_wanted.load()) SetState(State::Off);
            continue;
        } else if (!wanted) {
            // Wanted and then un-wanted before a lease was ever taken —
            // whatever we were waiting on no longer matters.
            m_lastError = Error::None;
            SetState(State::Off);
        }

        HANDLE handles[2] = {m_wake, held.process};
        const DWORD count = held.IsHeld() && held.process ? 2 : 1;
        const DWORD r = WaitForMultipleObjects(count, handles, FALSE, waitMs);
        if (r == WAIT_OBJECT_0 + 1) {
            // Steam exited, and the pipe — and with it the block — went too.
            // Not an error: the next pass looks for a Steam to block again,
            // which is how a Steam restart is followed.
            EventLog::Write("LEASE: Steam (pid %lu) exited while blocked; lease ended with it",
                            held.pid);
            held.Close();
            SetState(m_wanted.load() ? State::NoSteam : State::Off);
        }
    }
    if (held.IsHeld()) Release(held);
    SetState(State::Off);
}

DWORD SteamInputLease::TryAcquire(Held& held) {
    auto fail = [&](Error error, State state, DWORD retryMs) -> DWORD {
        if (error != m_lastError)
            EventLog::Write("LEASE: cannot block yet: %s", SteamInputGate::Describe(error));
        m_lastError = error;
        SetState(state);
        return retryMs;
    };

    Error error = Error::None;
    const DWORD pid = SteamInputGate::FindSteam(error);
    if (pid == 0)
        return error == Error::SteamNotRunning ? fail(error, State::NoSteam, NO_STEAM_RETRY_MS)
                                               : fail(error, State::Failed, FAILED_RETRY_MS);

    HANDLE pipe = SteamInputGate::Connect(pid, CONNECT_TIMEOUT_MS, error);
    if (pipe == INVALID_HANDLE_VALUE)
        return error == Error::GateNotLoaded ? fail(error, State::GateMissing, NO_GATE_RETRY_MS)
                                             : fail(error, State::Failed, FAILED_RETRY_MS);

    // Opened before acquiring so there is no moment where a lease is held
    // that we could not notice Steam exiting under.
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);

    SteamInputGate::Status status;
    error = SteamInputGate::Exchange(pipe, SteamInputGate::Command::AcquireLease, status,
                                     EXCHANGE_TIMEOUT_MS);
    if (error != Error::None) {
        // Closing is the release: if the gate did count us, EOF uncounts us.
        CloseHandle(pipe);
        if (process) CloseHandle(process);
        return fail(error, State::Failed, FAILED_RETRY_MS);
    }

    held.pipe    = pipe;
    held.process = process;
    held.pid     = pid;
    m_lastError  = Error::None;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        m_lastStatus = status;
    }
    EventLog::Write("LEASE: blocking Steam (pid %lu): leases=%u revokedHandles=%u "
                    "internalRecovery=%d",
                    pid, status.leaseCount, status.lastRevokedHandleCount,
                    status.InternalRecovery() ? 1 : 0);
    SetState(State::Blocking);
    return INFINITE;
}

bool SteamInputLease::Release(Held& held) {
    SteamInputGate::Status status;
    const Error error = SteamInputGate::Exchange(held.pipe, SteamInputGate::Command::ReleaseLease,
                                                 status, EXCHANGE_TIMEOUT_MS);
    const DWORD pid = held.pid;
    // The close is what actually guarantees the release: a failed handshake
    // still ends in EOF on the gate's side, which drops the count just the same.
    held.Close();

    if (error != Error::None) {
        EventLog::Write("LEASE: release handshake failed (%s); pipe closed, so the "
                        "block is lifted regardless", SteamInputGate::Describe(error));
        SteamInputGate::NudgeSteamRediscovery(pid);
        return false;
    }
    if (status.leaseCount > 0) {
        // Another client still blocks Steam; nothing for us to recover.
        EventLog::Write("LEASE: released; %u other lease(s) still block Steam",
                        status.leaseCount);
        return false;
    }
    if (status.InternalRecovery()) {
        EventLog::Write("LEASE: released; gate is running controller rediscovery");
    } else {
        EventLog::Write("LEASE: released; gate has no internal recovery, nudging Steam");
        SteamInputGate::NudgeSteamRediscovery(pid);
    }
    return true;
}

// The gate tracks every HID handle Steam opens once its hooks are in, and
// closed them all when blocking began — so the count rising from zero is Steam
// reopening devices, which is the rediscovery having landed.
void SteamInputLease::WaitForSteamReopen(DWORD pid) {
    const ULONGLONG start = GetTickCount64();
    while (m_running.load() && !m_wanted.load()) {
        SteamInputGate::Status status;
        if (SteamInputGate::QueryStatus(pid, status) == Error::None
                && status.leaseCount == 0 && status.hidHandleCount > 0) {
            EventLog::Write("LEASE: Steam reopened %u HID handle(s) after %llu ms",
                            status.hidHandleCount, GetTickCount64() - start);
            return;
        }
        if (GetTickCount64() - start >= REOPEN_TIMEOUT_MS) {
            EventLog::Write("LEASE: Steam had not reopened any HID device after %lu ms; "
                            "carrying on", REOPEN_TIMEOUT_MS);
            return;
        }
        // Woken early by SetWanted or Stop, which the loop condition then sees.
        WaitForSingleObject(m_wake, REOPEN_POLL_MS);
    }
}
