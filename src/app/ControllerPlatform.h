#pragma once
#include <cstdint>

enum class ControllerPlatform {
    Xbox,
    PlayStation
};

// What the controller does while a given profile — or the tray's default
// behaviour — is in charge. The two pad behaviours hold a Steam Input lease
// and drive a virtual pad; Lizard holds the lease and drives nothing, leaving
// the firmware's own keyboard/mouse mode; SteamInput holds nothing at all and
// lets Steam have the controller.
//
// The values are persisted (tray "DefaultBehavior", per-profile "Behavior"),
// so they are fixed: add new ones at the end.
enum class ControllerBehavior : uint32_t {
    // Per-game profiles only: follow the tray's default behaviour.
    Default     = 0,
    Xbox        = 1,
    PlayStation = 2,
    Lizard      = 3,
    SteamInput  = 4,
};

inline bool BehaviorDrivesPad(ControllerBehavior b) {
    return b == ControllerBehavior::Xbox || b == ControllerBehavior::PlayStation;
}

inline ControllerBehavior BehaviorFromDword(uint32_t v, ControllerBehavior def) {
    return v <= static_cast<uint32_t>(ControllerBehavior::SteamInput)
               ? static_cast<ControllerBehavior>(v)
               : def;
}

inline ControllerBehavior BehaviorForPlatform(ControllerPlatform p) {
    return p == ControllerPlatform::PlayStation ? ControllerBehavior::PlayStation
                                                : ControllerBehavior::Xbox;
}

inline const char* BehaviorName(ControllerBehavior b) {
    switch (b) {
    case ControllerBehavior::Default:     return "Default";
    case ControllerBehavior::Xbox:        return "Xbox Controller";
    case ControllerBehavior::PlayStation: return "PlayStation Controller";
    case ControllerBehavior::Lizard:      return "Lizard Mode";
    case ControllerBehavior::SteamInput:  return "Steam Input";
    }
    return "?";
}
