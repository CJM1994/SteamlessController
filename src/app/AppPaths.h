#pragma once
#include <string>

// Where the app keeps everything it writes.
//
// Portable first: a "Data" folder beside the executable, so the whole app —
// settings, profiles, logs — moves with its folder and leaves nothing behind
// on the machine. Only when that folder cannot be created (an install under
// Program Files, run unelevated) does it fall back to
// %LOCALAPPDATA%\SteamlessController, which is where every build before this
// kept its files.
//
// The tray app decides, once, at startup (InitDataDir(true)): it is the only
// process that may create the portable folder. The elevated helper must not —
// elevated, it could create one under Program Files that the unelevated app
// could then not write to — so it only ever follows the decision, by finding
// the folder there or not.
namespace AppPaths {

// The folder the running executable is in, without a trailing separator.
std::wstring ExeDir();

// Settles the data folder. Call first thing in wWinMain; anything that asks
// for DataDir before this is treated as not allowed to create it.
void InitDataDir(bool mayCreatePortable);

// The data folder, created if missing. No trailing separator.
std::wstring DataDir();

// DataDir() + "\" + name.
std::wstring DataFile(const wchar_t* name);

// True when DataDir is the portable folder beside the executable.
bool IsPortable();

}  // namespace AppPaths
