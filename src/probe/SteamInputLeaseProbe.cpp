// Console diagnostic for the Steam Input gate. Exercises the lease client
// against a live Steam before anything in the tray app depends on it.
//
// Needs the gate in Steam's folder and a Steam cold start since it was put
// there — see docs/plans/steam-input-control-modes.md. Without it every
// command reports that no gate answered, which is itself the answer to "is it
// installed".
//
//   SteamInputLeaseProbe                      status (same as --status)
//   SteamInputLeaseProbe --acquire            hold a lease until Enter
//   SteamInputLeaseProbe --cycle N [hold gap] N acquire/release rounds (ms)
//   SteamInputLeaseProbe --async              drive the tray's lease class;
//                                             Enter toggles, q quits
#include "app/SteamInputLease.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace SteamInputGate;

static void PrintStatus(const char* tag, const Status& s) {
    printf("%-10s leases=%u hidHandles=%u lastRevoked=%u internalRecovery=%s\n", tag,
           s.leaseCount, s.hidHandleCount, s.lastRevokedHandleCount,
           s.InternalRecovery() ? "yes" : "no");
    fflush(stdout);
}

static DWORD FindSteamOrSay() {
    Error error = Error::None;
    const DWORD pid = FindSteam(error);
    if (!pid) printf("No Steam to talk to: %s\n", Describe(error));
    else      printf("steam.exe pid %lu\n", pid);
    fflush(stdout);
    return pid;
}

static void WaitForEnter() {
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) Sleep(INFINITE);
}

static int Status_() {
    const DWORD pid = FindSteamOrSay();
    if (!pid) return 1;
    Status s;
    const Error error = QueryStatus(pid, s);
    if (error != Error::None) {
        printf("Gate: %s\n", Describe(error));
        return 1;
    }
    PrintStatus("gate", s);
    return 0;
}

// Acquire, optionally hold for holdMs (or until Enter when holdMs is 0), and
// release. Returns false on any failure, already reported.
static bool Round(DWORD pid, DWORD holdMs, bool quiet) {
    Error error = Error::None;
    HANDLE pipe = Connect(pid, 2000, error);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Connect: %s\n", Describe(error));
        return false;
    }
    Status s;
    const ULONGLONG t0 = GetTickCount64();
    error = Exchange(pipe, Command::AcquireLease, s, 10000);
    if (error != Error::None) {
        printf("Acquire: %s\n", Describe(error));
        CloseHandle(pipe);
        return false;
    }
    if (!quiet) {
        printf("Acquired in %llu ms.\n", GetTickCount64() - t0);
        PrintStatus("acquired", s);
    }

    if (holdMs) {
        Sleep(holdMs);
    } else {
        printf("Steam Input is blocked. Check that Steam has lost the controller,\n"
               "then press Enter to release.\n");
        fflush(stdout);
        WaitForEnter();
    }

    const ULONGLONG t1 = GetTickCount64();
    error = Exchange(pipe, Command::ReleaseLease, s, 10000);
    CloseHandle(pipe);
    if (error != Error::None) {
        printf("Release handshake: %s (pipe closed, block lifted anyway)\n", Describe(error));
        NudgeSteamRediscovery(pid);
        return false;
    }
    if (!quiet) {
        printf("Released in %llu ms.\n", GetTickCount64() - t1);
        PrintStatus("released", s);
    }
    if (s.leaseCount == 0 && !s.InternalRecovery()) {
        printf("Gate has no internal recovery; nudged Steam to rescan.\n");
        NudgeSteamRediscovery(pid);
    }
    return true;
}

static int Acquire() {
    const DWORD pid = FindSteamOrSay();
    if (!pid) return 1;
    if (!Round(pid, 0, false)) return 1;
    printf("Steam should pick the controller back up within ~3 s.\n");
    return 0;
}

// Repeated rounds, for the "does Steam rediscover every time" question. After
// each gap the gate's tracked HID handle count is printed: the hooks stay
// installed after the first lease, so Steam reopening its controllers shows up
// as that count rising again. Zero after a gap means Steam did not reopen
// anything (yet).
static int Cycle(int rounds, DWORD holdMs, DWORD gapMs) {
    const DWORD pid = FindSteamOrSay();
    if (!pid) return 1;
    int failures = 0, noReopen = 0;
    for (int i = 1; i <= rounds; ++i) {
        if (!Round(pid, holdMs, true)) { ++failures; Sleep(gapMs); continue; }
        Sleep(gapMs);
        Status s;
        const Error error = QueryStatus(pid, s);
        if (error != Error::None) {
            printf("round %3d: status failed: %s\n", i, Describe(error));
            ++failures;
            continue;
        }
        if (s.hidHandleCount == 0) ++noReopen;
        printf("round %3d: ok, Steam HID handles after %lu ms: %u%s\n", i, gapMs,
               s.hidHandleCount, s.hidHandleCount == 0 ? "  <-- not reopened" : "");
        fflush(stdout);
    }
    printf("\n%d round(s): %d failed, %d with no HID handles reopened after the gap.\n",
           rounds, failures, noReopen);
    return failures ? 1 : 0;
}

static int Async() {
    SteamInputLease lease;
    lease.Start([](SteamInputLease::State state) {
        printf("  [state] %s\n", SteamInputLease::Describe(state));
        fflush(stdout);
    });
    printf("Enter toggles blocking, q then Enter quits.\n");
    fflush(stdout);
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'q' || line[0] == 'Q') break;
        const bool want = !lease.Wanted();
        printf("-> %s\n", want ? "want block" : "want release");
        fflush(stdout);
        lease.SetWanted(want);
    }
    lease.Stop();
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--status") == 0) return Status_();
    if (strcmp(argv[1], "--acquire") == 0) return Acquire();
    if (strcmp(argv[1], "--async") == 0) return Async();
    if (strcmp(argv[1], "--cycle") == 0) {
        const int rounds  = argc > 2 ? atoi(argv[2]) : 10;
        const DWORD hold  = argc > 3 ? static_cast<DWORD>(atoi(argv[3])) : 3000;
        const DWORD gap   = argc > 4 ? static_cast<DWORD>(atoi(argv[4])) : 4000;
        return Cycle(rounds > 0 ? rounds : 10, hold ? hold : 3000, gap);
    }
    printf("usage: SteamInputLeaseProbe [--status | --acquire | --async | "
           "--cycle N [holdMs gapMs]]\n");
    return 2;
}
