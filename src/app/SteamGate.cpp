#include "SteamGate.h"
#include "AppPaths.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

constexpr const wchar_t* kNames[] = { L"XInput1_4.dll", L"dinput8.dll" };
constexpr char kMarker[] = "WsgmSteamInputGateProxy";

std::vector<unsigned char> ReadAll(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return bytes;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);
    return bytes;
}

bool Exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool SameContents(const std::wstring& a, const std::wstring& b) {
    const auto x = ReadAll(a);
    return !x.empty() && x == ReadAll(b);
}

// Whether a PE image's export name table contains `name`. Walked by hand
// rather than by loading the file: the loader keys modules by base name, and
// this process may already hold a real XInput1_4.dll under exactly that name.
bool ExportsName(const std::vector<unsigned char>& pe, const char* name) {
    auto in = [&](size_t off, size_t len) { return off + len <= pe.size() && off + len >= off; };
    if (!in(0, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER dos;
    memcpy(&dos, pe.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
    const size_t nt = static_cast<size_t>(dos.e_lfanew);
    if (!in(nt, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD))) return false;
    DWORD sig;
    memcpy(&sig, pe.data() + nt, sizeof(sig));
    if (sig != IMAGE_NT_SIGNATURE) return false;
    IMAGE_FILE_HEADER file;
    memcpy(&file, pe.data() + nt + sizeof(DWORD), sizeof(file));
    const size_t opt = nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    WORD magic;
    memcpy(&magic, pe.data() + opt, sizeof(magic));

    IMAGE_DATA_DIRECTORY exportDir{};
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && in(opt, sizeof(IMAGE_OPTIONAL_HEADER64))) {
        IMAGE_OPTIONAL_HEADER64 h;
        memcpy(&h, pe.data() + opt, sizeof(h));
        exportDir = h.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && in(opt, sizeof(IMAGE_OPTIONAL_HEADER32))) {
        IMAGE_OPTIONAL_HEADER32 h;
        memcpy(&h, pe.data() + opt, sizeof(h));
        exportDir = h.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return false;
    }
    if (!exportDir.VirtualAddress) return false;

    const size_t sections = opt + file.SizeOfOptionalHeader;
    auto rvaToOffset = [&](DWORD rva, size_t& off) {
        for (WORD i = 0; i < file.NumberOfSections; ++i) {
            const size_t at = sections + i * sizeof(IMAGE_SECTION_HEADER);
            if (!in(at, sizeof(IMAGE_SECTION_HEADER))) return false;
            IMAGE_SECTION_HEADER s;
            memcpy(&s, pe.data() + at, sizeof(s));
            const DWORD size = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size) {
                off = s.PointerToRawData + (rva - s.VirtualAddress);
                return true;
            }
        }
        return false;
    };

    size_t dirOff = 0;
    if (!rvaToOffset(exportDir.VirtualAddress, dirOff) || !in(dirOff, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return false;
    IMAGE_EXPORT_DIRECTORY ed;
    memcpy(&ed, pe.data() + dirOff, sizeof(ed));
    size_t namesOff = 0;
    if (!rvaToOffset(ed.AddressOfNames, namesOff)) return false;
    const size_t want = strlen(name);
    for (DWORD i = 0; i < ed.NumberOfNames && i < 65536; ++i) {
        if (!in(namesOff + i * sizeof(DWORD), sizeof(DWORD))) return false;
        DWORD nameRva;
        memcpy(&nameRva, pe.data() + namesOff + i * sizeof(DWORD), sizeof(nameRva));
        size_t nameOff = 0;
        if (!rvaToOffset(nameRva, nameOff) || !in(nameOff, want + 1)) continue;
        if (memcmp(pe.data() + nameOff, name, want + 1) == 0) return true;
    }
    return false;
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    return dir + L"\\" + name;
}

}  // namespace

namespace SteamGate {

std::wstring SteamDir() {
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    // Steam records it with forward slashes.
    std::wstring dir = buf;
    for (wchar_t& c : dir) if (c == L'/') c = L'\\';
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (dir.empty() || !Exists(Join(dir, L"steam.exe"))) return {};
    return dir;
}

std::wstring BundledPath() {
    return AppPaths::ExeDir() + L"\\steam_input_gate.dll";
}

bool IsOurGate(const std::wstring& path) {
    const auto bytes = ReadAll(path);
    return !bytes.empty() && ExportsName(bytes, kMarker);
}

Inspection Inspect() {
    Inspection result;
    result.bundled  = IsOurGate(BundledPath());
    result.steamDir = SteamDir();
    if (result.steamDir.empty()) return result;

    int foreign = 0;
    for (const wchar_t* name : kNames) {
        const std::wstring path = Join(result.steamDir, name);
        if (!Exists(path)) continue;
        if (!IsOurGate(path)) { ++foreign; continue; }
        result.installedPath = path;
        result.status = result.bundled && !SameContents(path, BundledPath()) ? Status::Outdated
                                                                              : Status::Installed;
        return result;
    }
    result.status = foreign == static_cast<int>(std::size(kNames)) ? Status::Blocked
                                                                     : Status::NotInstalled;
    return result;
}

int Deploy(const std::wstring& steamDir, std::wstring& message) {
    const std::wstring bundled = BundledPath();
    if (!IsOurGate(bundled)) {
        message = L"The Steam Input blocker (steam_input_gate.dll) is missing from "
                  L"SteamlessController's folder.";
        return 1;
    }
    if (steamDir.empty() || !Exists(Join(steamDir, L"steam.exe"))) {
        message = L"Steam's folder could not be found.";
        return 1;
    }

    // Ours already, under either name: update it where it is. Installing a
    // second copy under the other name would put two gates in Steam, both
    // trying to serve the same pipe.
    for (const wchar_t* name : kNames) {
        const std::wstring target = Join(steamDir, name);
        if (!Exists(target) || !IsOurGate(target)) continue;
        if (SameContents(target, bundled)) {
            message = std::wstring(L"Already installed as ") + name + L".";
            return 0;
        }
        // Steam may have the old one mapped, so it is moved aside rather than
        // overwritten; Steam keeps using it until it restarts.
        if (!MoveFileExW(target.c_str(), (target + L".dlld").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            message = L"Could not move the old blocker aside (error "
                    + std::to_wstring(GetLastError()) + L").";
            return 1;
        }
        if (!CopyFileW(bundled.c_str(), target.c_str(), TRUE)) {
            message = L"Could not copy the blocker into Steam's folder (error "
                    + std::to_wstring(GetLastError()) + L").";
            return 1;
        }
        message = std::wstring(L"Updated ") + name + L". The new version takes effect the "
                  L"next time Steam starts.";
        return 0;
    }

    // Otherwise the first name nobody owns.
    for (const wchar_t* name : kNames) {
        const std::wstring target = Join(steamDir, name);
        if (Exists(target)) continue;  // another program's
        if (!CopyFileW(bundled.c_str(), target.c_str(), TRUE)) {
            message = L"Could not copy the blocker into Steam's folder (error "
                    + std::to_wstring(GetLastError()) + L").";
            return 1;
        }
        message = std::wstring(L"Installed as ") + name + L". It takes effect the next time "
                  L"Steam starts.";
        return 0;
    }
    message = L"Steam's folder already has another program's XInput1_4.dll and dinput8.dll "
              L"(ValvePlug or Special K, for example), so the blocker cannot be installed "
              L"beside them.";
    return 1;
}

int Park(const std::wstring& steamDir, std::wstring& message) {
    int parked = 0;
    for (const wchar_t* name : kNames) {
        const std::wstring target = Join(steamDir, name);
        if (!Exists(target) || !IsOurGate(target)) continue;
        if (!MoveFileExW(target.c_str(), (target + L".dlld").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            message = std::wstring(L"Could not remove ") + name + L" (error "
                    + std::to_wstring(GetLastError()) + L").";
            return 1;
        }
        ++parked;
    }
    message = parked ? L"Uninstalled. Steam stops loading it the next time it starts."
                     : L"The blocker was not installed.";
    return 0;
}

}  // namespace SteamGate
