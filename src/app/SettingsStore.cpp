#include "SettingsStore.h"
#include "AppPaths.h"
#include <cstdio>
#include <cwchar>

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

constexpr wchar_t kRegKey[] = L"Software\\SteamlessController";

}  // namespace

SettingsStore& SettingsStore::Instance() {
    static SettingsStore store;
    return store;
}

SettingsStore::SettingsStore() {
    m_path = AppPaths::DataFile(L"settings.ini");
    if (!Load() && ImportRegistry()) {
        m_imported = true;
        Save();
    }
}

bool SettingsStore::Load() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, m_path.c_str(), L"rb") != 0 || !f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    const std::wstring all = FromUtf8(text);
    Section* current = nullptr;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(L'\n', pos);
        if (end == std::wstring::npos) end = all.size();
        const std::wstring line = Trim(all.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            current = &FindOrAdd(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find(L'=');
        if (!current || eq == std::wstring::npos) continue;
        current->values.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return true;
}

bool SettingsStore::Save() {
    std::wstring all;
    for (const auto& section : m_sections) {
        if (!all.empty()) all += L"\r\n";
        all += L"[" + section.name + L"]\r\n";
        for (const auto& [key, value] : section.values)
            all += key + L"=" + value + L"\r\n";
    }
    const std::string bytes = ToUtf8(all);

    const std::wstring temp = m_path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) return false;
    const bool written = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed  = fclose(f) == 0;
    if (!written || !closed) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return MoveFileExW(temp.c_str(), m_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

SettingsStore::Section* SettingsStore::Find(const std::wstring& name) {
    for (auto& s : m_sections) if (s.name == name) return &s;
    return nullptr;
}

const SettingsStore::Section* SettingsStore::Find(const std::wstring& name) const {
    for (const auto& s : m_sections) if (s.name == name) return &s;
    return nullptr;
}

SettingsStore::Section& SettingsStore::FindOrAdd(const std::wstring& name) {
    if (Section* s = Find(name)) return *s;
    m_sections.push_back({ name, {} });
    return m_sections.back();
}

bool SettingsStore::HasSection(const std::wstring& section) const {
    return Find(section) != nullptr;
}

std::wstring SettingsStore::GetSz(const std::wstring& section, const std::wstring& key,
                                  const std::wstring& def) const {
    if (const Section* s = Find(section))
        for (const auto& [k, v] : s->values)
            if (k == key) return v;
    return def;
}

DWORD SettingsStore::GetDw(const std::wstring& section, const std::wstring& key,
                           DWORD def) const {
    const std::wstring v = GetSz(section, key);
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    const unsigned long n = wcstoul(v.c_str(), &end, 10);
    // Anything but a whole number reads as absent rather than as whatever
    // prefix of it happened to parse.
    return end && *end == L'\0' ? static_cast<DWORD>(n) : def;
}

void SettingsStore::SetSz(const std::wstring& section, const std::wstring& key,
                          const std::wstring& value) {
    // Line breaks would end the value early on the next load.
    std::wstring clean = value;
    for (wchar_t& c : clean) if (c == L'\r' || c == L'\n') c = L' ';
    Section& s = FindOrAdd(section);
    for (auto& [k, v] : s.values)
        if (k == key) { v = clean; return; }
    s.values.emplace_back(key, clean);
}

void SettingsStore::SetDw(const std::wstring& section, const std::wstring& key, DWORD value) {
    SetSz(section, key, std::to_wstring(value));
}

void SettingsStore::Remove(const std::wstring& section, const std::wstring& key) {
    if (Section* s = Find(section))
        std::erase_if(s->values, [&](const auto& kv) { return kv.first == key; });
}

void SettingsStore::RemoveSections(const std::wstring& prefix) {
    std::erase_if(m_sections, [&](const Section& s) {
        return s.name.compare(0, prefix.size(), prefix) == 0;
    });
}

// One registry key's DWORD and string values, copied into a section.
static void CopyValues(HKEY key, SettingsStore& store, const std::wstring& section) {
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nameLen = 256, type = 0;
        BYTE data[2048];
        DWORD size = sizeof(data);
        const LONG r = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type == REG_DWORD && size == sizeof(DWORD)) {
            DWORD v;
            memcpy(&v, data, sizeof(v));
            store.SetDw(section, name, v);
        } else if (type == REG_SZ) {
            std::wstring v(reinterpret_cast<const wchar_t*>(data), size / sizeof(wchar_t));
            while (!v.empty() && v.back() == L'\0') v.pop_back();
            store.SetSz(section, name, v);
        }
    }
}

bool SettingsStore::ImportRegistry() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return false;
    CopyValues(root, *this, L"Settings");

    HKEY profiles;
    if (RegOpenKeyExW(root, L"GameProfiles", 0, KEY_READ, &profiles) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            const std::wstring sub = std::to_wstring(i);
            HKEY child;
            if (RegOpenKeyExW(profiles, sub.c_str(), 0, KEY_READ, &child) != ERROR_SUCCESS)
                break;
            CopyValues(child, *this, L"GameProfile " + sub);
            RegCloseKey(child);
        }
        RegCloseKey(profiles);
    }
    RegCloseKey(root);
    return true;
}
