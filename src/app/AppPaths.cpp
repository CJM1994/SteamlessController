#include "AppPaths.h"
#include <Windows.h>
#include <mutex>

namespace {

std::once_flag g_once;
std::wstring   g_dataDir;
bool           g_portable    = false;
bool           g_mayCreate   = false;

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void Resolve() {
    const std::wstring portable = AppPaths::ExeDir() + L"\\Data";
    if (IsDirectory(portable)
            || (g_mayCreate && CreateDirectoryW(portable.c_str(), nullptr))) {
        g_dataDir  = portable;
        g_portable = true;
        return;
    }
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    g_dataDir = (n && n < MAX_PATH ? std::wstring(local) : AppPaths::ExeDir())
              + L"\\SteamlessController";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
}

}  // namespace

namespace AppPaths {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

void InitDataDir(bool mayCreatePortable) {
    g_mayCreate = mayCreatePortable;
    std::call_once(g_once, Resolve);
}

std::wstring DataDir() {
    std::call_once(g_once, Resolve);
    // Recreated if it was deleted while running; cheap when it exists.
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    return g_dataDir;
}

std::wstring DataFile(const wchar_t* name) {
    return DataDir() + L"\\" + name;
}

bool IsPortable() {
    std::call_once(g_once, Resolve);
    return g_portable;
}

}  // namespace AppPaths
