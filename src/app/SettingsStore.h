#pragma once
#include <Windows.h>
#include <string>
#include <utility>
#include <vector>

// The app's settings and game profiles, as one INI-style text file in the data
// folder (see AppPaths) — so they travel with a portable copy, and can be read
// and fixed by hand.
//
//   [Settings]
//   DefaultBehavior=4
//   [GameProfile 0]
//   Id=C:\Games\Example\game.exe
//
// UTF-8, one "key=value" per line, split at the first '='. Values are numbers
// or single-line strings; nothing the app stores contains a line break.
// Sections and keys keep the order they were first written in, so a diff of
// the file after a change shows only that change.
//
// Single-threaded: every caller is on the UI thread.
class SettingsStore {
public:
    // The store, loaded from the data folder on first use. A first run with no
    // file imports whatever the registry held (see ImportedFromRegistry).
    static SettingsStore& Instance();

    bool  HasSection(const std::wstring& section) const;
    DWORD GetDw(const std::wstring& section, const std::wstring& key, DWORD def) const;
    std::wstring GetSz(const std::wstring& section, const std::wstring& key,
                       const std::wstring& def = {}) const;

    void SetDw(const std::wstring& section, const std::wstring& key, DWORD value);
    void SetSz(const std::wstring& section, const std::wstring& key, const std::wstring& value);
    void Remove(const std::wstring& section, const std::wstring& key);
    // Every section whose name starts with prefix — for numbered sections
    // rewritten whole, so a shrunk list leaves no stale tail behind.
    void RemoveSections(const std::wstring& prefix);

    // Writes the file: to a temporary beside it, then moved over it, so a
    // crash mid-write leaves the old settings rather than half of the new.
    bool Save();

    // Whether this run started by copying settings out of the registry —
    // logged, since it is a one-time event worth being able to see.
    bool ImportedFromRegistry() const { return m_imported; }
    const std::wstring& Path() const { return m_path; }

private:
    SettingsStore();
    bool Load();
    // Everything under HKCU\Software\SteamlessController: its values become
    // [Settings], each GameProfiles\N subkey becomes [GameProfile N]. Copied
    // generically, value by value, so nothing has to be taught about it field
    // by field. The registry is left as it was — an installed older version
    // may still be reading it.
    bool ImportRegistry();

    struct Section {
        std::wstring name;
        std::vector<std::pair<std::wstring, std::wstring>> values;
    };
    Section*       Find(const std::wstring& name);
    const Section* Find(const std::wstring& name) const;
    Section&       FindOrAdd(const std::wstring& name);

    std::vector<Section> m_sections;
    std::wstring         m_path;
    bool                 m_imported = false;
};
