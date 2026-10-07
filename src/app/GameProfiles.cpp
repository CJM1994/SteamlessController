#include "GameProfiles.h"
#include "SettingsStore.h"
#include <Windows.h>
#include <cstdio>

namespace {

// Each profile is a numbered section ("GameProfile 0", "GameProfile 1", ...)
// rather than one named after the game, whose id — an exe path, a URI — would
// make an unwieldy and fragile section name.
constexpr wchar_t kSectionPrefix[] = L"GameProfile ";

DWORD ReadDw(const std::wstring& section, const wchar_t* name, DWORD def) {
    return SettingsStore::Instance().GetDw(section, name, def);
}

std::wstring ReadSz(const std::wstring& section, const wchar_t* name) {
    return SettingsStore::Instance().GetSz(section, name);
}

void WriteDw(const std::wstring& section, const wchar_t* name, DWORD val) {
    SettingsStore::Instance().SetDw(section, name, val);
}

void WriteSz(const std::wstring& section, const wchar_t* name, const std::wstring& val) {
    SettingsStore::Instance().SetSz(section, name, val);
}

DWORD Packed(BackButtonAction a) {
    return BackButtonBinding::FromAction(a).Pack();
}

// One pad's values. Named rather than spelled out twice because the two pads
// carry the same twelve settings and only the prefix differs, and a
// copy-pasted second copy is where a left-pad name ends up reading a
// right-pad value.
void ReadPad(const std::wstring& key, const wchar_t* prefix, TrackpadSettings& pad) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    auto binding = [&](const wchar_t* suffix, BackButtonAction def) {
        return BackButtonBinding::Unpack(ReadDw(key, name(suffix).c_str(), Packed(def)));
    };

    pad.mode      = TrackpadModeFromDword(ReadDw(key, name(L"Mode").c_str(), 0));
    pad.click     = binding(L"Click", BackButtonAction::None);
    pad.scrollDir = ScrollDirectionFromDword(ReadDw(key, name(L"ScrollDir").c_str(), 0));
    // Zero means absent, which ScrollSpeedFromDword reads as the default.
    pad.scrollSpeed = ScrollSpeedFromDword(ReadDw(key, name(L"ScrollSpeed").c_str(), 0));
    // Absent from every profile written before the directional modes existed.
    // The defaults match TrackpadSettings' own, so those profiles read back as
    // an unconfigured directional pad rather than a broken one.
    pad.touch     = binding(L"Touch", BackButtonAction::None);
    pad.up        = binding(L"Up",    BackButtonAction::DPadUp);
    pad.down      = binding(L"Down",  BackButtonAction::DPadDown);
    pad.left      = binding(L"Left",  BackButtonAction::DPadLeft);
    pad.right     = binding(L"Right", BackButtonAction::DPadRight);
    pad.diagonals = DiagonalModeFromDword(ReadDw(key, name(L"Diagonals").c_str(), 0));
}

void WritePad(const std::wstring& key, const wchar_t* prefix, const TrackpadSettings& pad) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };

    WriteDw(key, name(L"Mode").c_str(),      static_cast<DWORD>(pad.mode));
    WriteDw(key, name(L"Click").c_str(),     pad.click.Pack());
    WriteDw(key, name(L"ScrollDir").c_str(), static_cast<DWORD>(pad.scrollDir));
    WriteDw(key, name(L"ScrollSpeed").c_str(), pad.scrollSpeed);
    WriteDw(key, name(L"Touch").c_str(),     pad.touch.Pack());
    WriteDw(key, name(L"Up").c_str(),        pad.up.Pack());
    WriteDw(key, name(L"Down").c_str(),      pad.down.Pack());
    WriteDw(key, name(L"Left").c_str(),      pad.left.Pack());
    WriteDw(key, name(L"Right").c_str(),     pad.right.Pack());
    WriteDw(key, name(L"Diagonals").c_str(), static_cast<DWORD>(pad.diagonals));
}

}  // namespace

namespace GameProfiles {

std::map<std::wstring, ControllerProfile> Load() {
    std::map<std::wstring, ControllerProfile> profiles;
    const DWORD unbound = BackButtonBinding::FromAction(BackButtonAction::None).Pack();

    for (DWORD i = 0;; ++i) {
        const std::wstring child = kSectionPrefix + std::to_wstring(i);
        if (!SettingsStore::Instance().HasSection(child)) break;

        const std::wstring id = ReadSz(child, L"Id");
        if (!id.empty()) {
            ControllerProfile p;
            p.displayName = ReadSz(child, L"Name");
            // Absent on every profile written before followers existed, and 0
            // is "has its own controls" precisely so those keep theirs.
            p.useDefaultMappings = ReadDw(child, L"UseDefaultMappings", 0) != 0;
            p.platform = ReadDw(child, L"Platform", 0) != 0 ? ControllerPlatform::PlayStation
                                                            : ControllerPlatform::Xbox;
            // Absent on profiles written before behaviours existed, every one
            // of which drove a pad for its game — which is Steamless.
            p.behavior = BehaviorFromDword(ReadDw(child, L"Behavior",
                static_cast<DWORD>(ControllerBehavior::Steamless)));
            p.back.l4 = BackButtonBinding::Unpack(ReadDw(child, L"L4", unbound));
            p.back.l5 = BackButtonBinding::Unpack(ReadDw(child, L"L5", unbound));
            p.back.r4 = BackButtonBinding::Unpack(ReadDw(child, L"R4", unbound));
            p.back.r5 = BackButtonBinding::Unpack(ReadDw(child, L"R5", unbound));
            // Profiles written before per-pad settings existed have none of
            // these values; the defaults leave both pads unclaimed, which is
            // exactly how those profiles behaved.
            ReadPad(child, L"LeftPad",  p.leftPad);
            ReadPad(child, L"RightPad", p.rightPad);
            // Absent before dual-stage triggers existed, which reads back as
            // Standard: analog only, as those profiles always were.
            auto readDw = [&](const wchar_t* name, uint32_t def) -> uint32_t {
                return ReadDw(child, name, def);
            };
            p.leftTrigger  = ReadTriggerSettings(L"LeftTrigger",  readDw);
            p.rightTrigger = ReadTriggerSettings(L"RightTrigger", readDw);
            profiles[id] = p;
        }
    }
    return profiles;
}

void Save(const std::map<std::wstring, ControllerProfile>& profiles) {
    SettingsStore& store = SettingsStore::Instance();
    // Rewritten whole, so a deleted profile leaves no section behind.
    store.RemoveSections(kSectionPrefix);

    DWORD i = 0;
    for (const auto& [id, p] : profiles) {
        const std::wstring child = kSectionPrefix + std::to_wstring(i);
        {
            WriteSz(child, L"Id", id);
            WriteSz(child, L"Name", p.displayName);
            // The controls below are written either way, so a profile that
            // stops following the default still has whatever it had before.
            WriteDw(child, L"UseDefaultMappings", p.useDefaultMappings ? 1 : 0);
            WriteDw(child, L"Platform",
                    p.platform == ControllerPlatform::PlayStation ? 1 : 0);
            WriteDw(child, L"Behavior", static_cast<DWORD>(p.behavior));
            WriteDw(child, L"L4", p.back.l4.Pack());
            WriteDw(child, L"L5", p.back.l5.Pack());
            WriteDw(child, L"R4", p.back.r4.Pack());
            WriteDw(child, L"R5", p.back.r5.Pack());
            WritePad(child, L"LeftPad",  p.leftPad);
            WritePad(child, L"RightPad", p.rightPad);
            auto writeDw = [&](const wchar_t* name, uint32_t v) { WriteDw(child, name, v); };
            WriteTriggerSettings(L"LeftTrigger",  p.leftTrigger,  writeDw);
            WriteTriggerSettings(L"RightTrigger", p.rightTrigger, writeDw);
        }
        ++i;
    }
    store.Save();
}

}  // namespace GameProfiles
