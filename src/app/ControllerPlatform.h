#pragma once
#include <cstdint>

enum class ControllerPlatform {
    Xbox,
    PlayStation
};

// What the controller does while a profile is in charge — the tray's default
// behaviour, or a game's profile. Steamless holds a Steam Input lease and
// drives a virtual pad, of whichever kind the profile's "Appear to games as"
// says; Lizard holds the lease and drives nothing, leaving the firmware's own
// keyboard/mouse mode; SteamInput holds nothing and lets Steam have the
// controller.
//
// Persisted (tray "DefaultBehavior", per-profile "Behavior"), so the values
// are fixed. 0 ("follow the default") and 2 (PlayStation) were written by a
// build that split Steamless by pad type; both read back as Steamless.
enum class ControllerBehavior : uint32_t {
    Steamless  = 1,
    Lizard     = 3,
    SteamInput = 4,
};

inline ControllerBehavior BehaviorFromDword(uint32_t v) {
    switch (v) {
    case 3:  return ControllerBehavior::Lizard;
    case 4:  return ControllerBehavior::SteamInput;
    default: return ControllerBehavior::Steamless;
    }
}

inline const char* BehaviorName(ControllerBehavior b) {
    switch (b) {
    case ControllerBehavior::Steamless:  return "Steamless Mode";
    case ControllerBehavior::Lizard:     return "Lizard Mode";
    case ControllerBehavior::SteamInput: return "Steam Input Mode";
    }
    return "?";
}

inline const char* PlatformName(ControllerPlatform p) {
    return p == ControllerPlatform::PlayStation ? "PlayStation Controller" : "Xbox Controller";
}
