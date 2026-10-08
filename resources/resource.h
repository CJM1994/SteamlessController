#pragma once

#define IDI_ICON_OFF    101
#define IDI_ICON_ON     102
// Game mode is live, but on a handle shared with another writer (typically
// Steam) rather than an exclusive one — see ControllerManager::IsGameModeShared.
#define IDI_ICON_SHARED 103
// No virtual pad and no Steam Input lease: Steam has the controller.
#define IDI_ICON_STEAM    104
// The tray's Enabled item is off; the app is doing nothing at all.
#define IDI_ICON_DISABLED 105

// One definition of the version, for the VERSIONINFO resource in app.rc and for
// the startup line in the event log. Both matter: a build with no version stamp
// is indistinguishable from any other once it is installed, and a log that does
// not say which build wrote it costs a round trip with whoever reported the bug.
//
// Keep in step with MyAppVersion in resources/InnoInstallerScript.iss.
//
// Semantic versioning (MAJOR.MINOR.PATCH), tagged vMAJOR.MINOR.PATCH — see
// "Versioning" in README.md. 2.0.0 is this fork's first release; the jump
// from upstream's 1.25 marks the incompatible change of control modes and
// settings storage.
#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 1
#define APP_VERSION_STR   "2.0.1"
