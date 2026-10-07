#pragma once
#include <string>

// Putting the Steam Input gate (steam-input-lease's proxy DLL) into Steam's
// folder, and taking it out again.
//
// The gate ships beside our executable as steam_input_gate.dll and goes into
// Steam's folder as XInput1_4.dll — or dinput8.dll when another program
// (ValvePlug, Special K) already owns that name. Steam maps it at its next cold
// start. Upstream's deployment rules, all followed here:
//   - a file is only replaced after proving it is ours, by the marker export
//     WsgmSteamInputGateProxy; another program's DLL is never touched
//   - a mapped image is never overwritten: the old file is moved aside first,
//     which Windows allows even while Steam has it loaded
//   - uninstalling parks the file as <name>.dlld rather than deleting it
//
// Inspect needs no rights beyond reading Steam's folder. Deploy and Park write
// to it, which under Program Files needs elevation — they run in the helper.
namespace SteamGate {

enum class Status {
    SteamNotFound,  // no Steam install recorded for this user
    NotInstalled,   // neither name holds our gate
    Installed,      // our gate, identical to the copy we ship
    Outdated,       // our gate, but not the copy we ship
    Blocked,        // both names belong to other programs
};

struct Inspection {
    Status       status = Status::SteamNotFound;
    std::wstring steamDir;
    std::wstring installedPath;  // our gate's path when Installed/Outdated
    bool         bundled = false;  // we have a gate to install
};

// Steam's install folder for this user, from the registry, or empty.
std::wstring SteamDir();
// The gate we ship, beside our executable.
std::wstring BundledPath();
// Whether the file at path exports the gate's ownership marker. Read from the
// file's export table; the DLL is never loaded.
bool IsOurGate(const std::wstring& path);

Inspection Inspect();

// Elevated side. Both return 0 on success, and describe what happened (or why
// not) in `message`, which is shown to the user.
int Deploy(const std::wstring& steamDir, std::wstring& message);
int Park(const std::wstring& steamDir, std::wstring& message);

}  // namespace SteamGate
