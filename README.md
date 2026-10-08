# SteamlessController

A Windows tray app that decides, app by app, who gets your **Steam Controller (2026)**: a
virtual Xbox or PlayStation pad, the controller's own keyboard-and-mouse mode, or Steam Input.
It can also shut Steam Input out completely, and serve the controller's gyro to emulators.

This is a fork of [SteamlessController](https://github.com/ddeverill/SteamlessController) by
Dylan Deverill. It adds per-app control modes, a global Steam Input blocker built on
[steam-input-lease](https://github.com/KillerPixelCrew/steam-input-lease), a gyro server
ported from [sc2dsu](https://github.com/KillerPixelCrew/sc2dsu), and a fully portable install.
See [Credits and licences](#credits-and-licences).

- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Installing](#installing)
- [Using it](#using-it)
- [Emulators](#emulators)
- [How it works](#how-it-works)
- [Settings and files](#settings-and-files)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [Versioning and releasing](#versioning-and-releasing)
- [Credits and licences](#credits-and-licences)

## What it does

**Three control modes**, chosen per app:

| Mode | What the controller does | Steam Input | Tray icon |
| --- | --- | --- | --- |
| **Steamless Mode** | Appears to games as an Xbox 360 or DualShock 4 controller, with your remaps | Blocked | Blue |
| **Lizard Mode** | The controller's own keyboard and mouse mode, or raw access for apps that read it directly (Dolphin, Eden) | Blocked | Red |
| **Steam Input Mode** | Steam has the controller, as if this app were not running | Allowed | Green |

A gray icon means the app is disabled and hands everything to Steam.

- **Global Steam Input blocking.** While a mode blocks Steam Input, Steam cannot see the
  controller at all, for every game whether Steam launched it or not. Nothing is done per process.
  Close SteamlessController, or crash it, and Steam gets the controller straight back.
- **Per-game profiles.** A default behaviour, plus a mode for any app you make a profile for. Each
  profile also has its own button remaps, trackpad modes, dual-stage triggers, and Xbox or
  PlayStation pad type.
- **Follows what is on screen.** The mode follows the app that is visibly in front, not just the
  one with keyboard focus. That handles launchers such as LaunchBox Big Box, which keep focus on
  their splash screen while the game draws over it.
- **Gyro for emulators (DSU).** In Steamless Mode the controller's motion, and the right trackpad as
  a touch point, are served over the Cemuhook/DSU protocol to emulators that cannot read the
  controller themselves.
- **On-screen mode notices** when a switch happens on its own (focus change, Steam starting or
  closing). They're drawn over borderless and windowed games, and aren't silenced by Do Not Disturb.
- **Battery level** in the tray menu and tooltip in every mode, with low-battery notices at 15% and 5%.
- **Portable.** Settings, profiles and logs live beside the executable. The only thing installed
  outside its folder is the Steam Input blocker, which has to sit in Steam's folder.
- Everything from upstream, still here:
  - Puck, wired and Bluetooth connections.
  - Paddles and trackpads remappable to pad buttons, keys, shortcuts or mouse buttons.
  - Trackpads as mouse, scroll wheel, d-pad or DS4 touchpad.
  - Steam + Y turns the controller off.

## Requirements

- **Windows 10 or 11, 64-bit.**
- **[ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/latest)** driver, for Steamless Mode's
  virtual pad. The app tells you if it is missing.
- **Steam Controller (2026)**: wireless puck (`28DE:1304`), USB (`28DE:1302`) or Bluetooth (`28DE:1303`).
- **Steam** is optional. It is needed only for Steam Input Mode, and the blocker only matters while
  Steam is running.

## Installing

1. Download `SteamlessController-vX.Y.Z-win-x64.zip` from
   [Releases](https://github.com/CJM1994/SteamlessController/releases).
2. Extract it to a folder you can write to, e.g. `C:\Tools\SteamlessController`. Avoid
   Program Files: the app keeps its settings beside itself.
3. Install [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/latest) if you have not already.
4. Run `SteamlessController.exe`. It lives in the tray.
5. **Install the Steam Input blocker:** tray menu → **Steam Input Blocker → Install...**, approve the
   administrator prompt, then let it restart Steam. The status should read **Active**.

The first time the app needs to restart the controller's device, Windows asks for administrator
approval once. The app registers a small on-demand helper task (`SteamlessDeviceCycle.exe`) for
this, and it never asks again unless the folder moves.

> The executables are not code-signed, so Windows SmartScreen may warn the first time you run
> them. Choose **More info → Run anyway**, or build from source.

**Upgrading from upstream SteamlessController:** on first run this build copies your settings and
game profiles out of the registry into its own `Data\settings.ini`, and leaves the registry
untouched. The old control modes convert like this:

- "Off while Steam running" becomes the Steam Input checkbox.
- "Off unless a game profile is running" becomes Default = Steam Input, with your game profiles set
  to Steamless.
- Manual without "Enable on Launch" starts disabled.

## Using it

### The tray menu

```
Controller battery: 82%
──────────────
✓ Enabled
──────────────
Default Behaviour        ▸ ● Steamless Mode / Steam Input Mode / Lizard Mode
                           ☐ Use Steam Input Mode while Steam is running
Game Profiles            ▸ ✓ Use Game Profiles
                           <each game>  ▸ ● Steamless / Steam Input / Lizard,  Edit Profile...
                           Edit Game Profiles...
──────────────
Edit Default Profile...
Gyro for Emulators (DSU) ▸ status, Enabled, Recalibrate Gyro
Steam Input Blocker      ▸ status, Install / Update / Uninstall, Restart Steam
──────────────
Start with Windows / Open Event Log / Show Notifications / Exit
```

### Which mode applies

Highest priority first:

1. **Enabled** is off → nothing: Steam has the controller (gray icon).
2. **Use Game Profiles** is on and the app in front has a profile → that profile's mode.
3. **Use Steam Input Mode while Steam is running** is ticked and Steam is running → Steam Input Mode.
4. Otherwise → the **Default Behaviour**.

Changes you make in the menu apply at once. Changes caused by switching apps wait about a second
before letting go of anything, so a stray focus change doesn't cost a reconnect. Alt-tabbing back
within that second changes nothing.

### Profiles

- **Edit Default Profile** sets what Steamless Mode does when no game profile applies: the pad type
  ("Appear to games as"), paddle and trackpad bindings, and triggers.
- **Edit Game Profiles** / **Edit Profile...** do the same for one game. A game profile can follow
  the default's mappings ("Use my default mappings") and still have its own mode.
- Games are found from Start Menu and desktop shortcuts, Store apps, Steam, Epic, GOG, Ubisoft
  Connect and Battle.net. Anything else (an emulator, a portable game) can be added from the
  running apps or by browsing to its `.exe`.

## Emulators

| Emulator | Mode | Why |
| --- | --- | --- |
| Dolphin | Lizard | Reads the controller directly through SDL, gyro included |
| Eden (builds with SDL3, May 2026 and later) | Lizard | Same |
| Azahar 2126.x, Cemu, Ryujinx, older Eden | Steamless + DSU | Their SDL predates the 2026 controller: buttons come from the virtual pad, gyro from DSU |
| Anything that takes an Xbox controller | Steamless | |

**DSU setup** (Azahar shown; others are similar): Emulation → Configure → Controls → Motion/Touch.
Set **Motion Provider** to **CemuhookUDP**, server `127.0.0.1`, port `26760`, **Pad 1**, then click
**Test**. Set the controller down for a couple of seconds so drift calibration settles, or use
**Recalibrate Gyro**.

- If an axis turns the wrong way, edit `DsuGyroAxes` in `Data\settings.ini`.
- For the touchscreen, the simplest choice is **Touch Provider: Emulator Window** with a trackpad
  as the mouse pointer. Alternatively, **Touch Provider: CemuhookUDP** takes the right trackpad as
  an absolute touch point.

**Front ends such as Big Box.** Give the front end a Steamless profile and each emulator its own
mode. While the blocker is active, the controller is opened shared, so an emulator launched from
the front end can still open it fully. Switching between them is just letting go: nothing is
restarted, and nothing is pulled out from under the emulator. An emulator that reads input in the
background (Eden does) also sees presses made in the front end's menu, so let the front end pause
it.

**The Steam button** is the virtual pad's Guide/PS button. If Steam is running it may react to it.
Turn off **Steam → Settings → Controller → Guide Button Focuses Steam** if you don't want that.

## How it works

**The controller.** The Steam Controller exposes a vendor HID collection (usage page `0xFF00`)
carrying all input in a 54-byte state report (`0x42`). By default the firmware runs **lizard mode**,
emulating a keyboard and mouse. In Steamless Mode the app turns lizard mode off with feature
reports (re-sent every two seconds as a keepalive), reads the state reports, and drives a virtual
Xbox 360 or DualShock 4 through ViGEmBus. The report layout is in
[`src/steam/SteamController.h`](src/steam/SteamController.h).

**Blocking Steam Input.**
- **The gate:** steam-input-lease's gate is a proxy DLL that Steam loads from its own folder as
  `XInput1_4.dll` (or `dinput8.dll` if another program owns that name). It does nothing until a
  client takes a *lease* over its named pipe.
- **While a lease is held:** the gate closes Steam's controller handles and denies new ones, and
  fails Steam's XInput queries. Steam then sees no controllers.
- **Crash safety:** a lease is an open pipe connection. When the app releases it, exits or crashes,
  Steam gets everything back and is told to look for controllers again.
- **The app's side:** it holds one lease (`src/app/SteamInputLease.*`) whenever the mode in effect
  blocks Steam. It waits for Steam's reopen before announcing that Steam Input Mode is ready.
- **Installing it:** the tray installs, updates and removes the gate in Steam's folder through the
  elevated helper. It replaces a file there only after proving it is the gate (by its
  `WsgmSteamInputGateProxy` export), and uninstalling moves it aside as `.dlld` rather than deleting
  it.

**Handing over.** Engaging takes the lease first, so the app can open the controller without a
fight. Disengaging releases the controller first and the lease last, so Steam's rediscovery finds
it free.

- **Switching to Lizard** after holding the controller exclusively restarts its device node, so
  programs that read it directly see it arrive.
- **While the gate blocks Steam,** the controller is opened shared instead. Nothing then needs
  restarting, and programs already holding it keep their connection.
- **Without the gate,** the app falls back to upstream's method: restart the device and win the
  reopen race against Steam.

**What is "in front".** A foreground hook, plus a check of the window stacking order every 750 ms.
A different app's window that covers 90% or more of the focused window's monitor, and is what
shows at its centre, counts as in front even without focus.

**Gyro (DSU).**
- A Cemuhook protocol (version 1001) server on UDP `127.0.0.1:26760`, ported from sc2dsu.
- It's fed from the Steamless read loop: motion in g and degrees per second, sc2dsu's axis
  defaults, and drift calibration from GamepadMotionHelpers.
- The motion sensors are switched on only while an emulator is subscribed. Timestamps come from
  the PC's clock.

**Battery.** The controller sends a battery report every few seconds to half a minute. The
Steamless read loop picks them up. Otherwise, a read-only listener on the open interfaces does; it
never writes to the controller.

## Settings and files

Everything lives in `Data\` beside the executable. If that folder can't be created, the app uses
`%LOCALAPPDATA%\SteamlessController`.

| File | What |
| --- | --- |
| `settings.ini` | Settings (`[Settings]`) and game profiles (`[GameProfile N]`), UTF-8, editable by hand while the app is closed |
| `events.log` | The app's log: tray menu → Open Event Log. Rotates at 512 KB |
| `cycle.log` | The elevated helper's log: device restarts, blocker installs, which programs hold the controller |
| `WebView2\` | Cache for the settings window |

Settings with no UI yet, in `[Settings]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `DsuPort` | `26760` | DSU server port |
| `DsuExposeToNetwork` | `0` | `1` serves DSU on all interfaces, not just this PC |
| `DsuGyroSensitivityPercent` | `100` | Gyro scale, 10–300 |
| `DsuAutoCalibrate` | `1` | Gyro drift calibration |
| `DsuGyroAxes` / `DsuAccelAxes` | `x,-z,y` / `-x,-z,y` | Output axes from raw axes; `-` inverts |

Outside the folder: "Start with Windows" (the per-user Run key), the helper's scheduled task
`SteamlessControllerDeviceCycle`, and the blocker in Steam's folder. To remove everything:
1. Uninstall the blocker from the tray.
2. Untick Start with Windows.
3. Run `SteamlessDeviceCycle.exe --unregister` from an administrator prompt.
4. Delete the folder.

## Troubleshooting

| Symptom | Try |
| --- | --- |
| Blocker says "Installed - restart Steam to activate" | **Restart Steam** in the same menu. Steam loads the gate only at startup |
| Blocker can't install: another program uses both DLL names | ValvePlug or Special K already occupy `XInput1_4.dll` and `dinput8.dll` in Steam's folder |
| "Port 26760 is already in use" under Gyro | Another DSU server (sc2dsu) is running. Close it; this app replaces it |
| An emulator in Lizard Mode doesn't see the controller | Check it reads the 2026 controller natively (see [Emulators](#emulators)). If it was launched while another app held the controller, check `cycle.log` for its name under `HOLDERS` |
| Steam opens when pressing the Steam button | Turn off Steam's **Guide Button Focuses Steam** |
| Notices don't appear over a game | Exclusive-fullscreen games can't be drawn over. Use borderless |
| Anything else | Tray → Open Event Log. `CONTROL:`, `LEASE:`, `STEAM:`, `DSU:` and `BATTERY:` lines show each decision |

Diagnostic tools (build from source):
- `SteamInputLeaseProbe`: gate status, take and release leases.
- `DsuProbe`: a DSU client. `--selftest` checks the server with no controller.
- `ForegroundProbe --zorder`: what counts as in front.
- `SteamProbe`: raw HID reports.

## Building from source

Requirements:
- Visual Studio 2022 Build Tools with **Desktop development with C++**.
- CMake 3.20 or later (bundled with the Build Tools).
- Windows SDK 10.0.22000 or later.

```bat
git clone https://github.com/CJM1994/SteamlessController.git
cd SteamlessController
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

- Outputs land in `build\Release\`. A copy of that folder runs as a portable install.
- Configuring downloads the WebView2 SDK and the pinned steam-input-lease v0.1.0 gate. The gate is
  checked against its SHA-256 hash and copied beside the app as `steam_input_gate.dll`.

**From WSL:** `scripts/winbuild.sh [Debug|Release] [targets...]` mirrors the tree to
`%USERPROFILE%\build` and builds it with the Windows toolchain.

**Installer (optional):** `resources\InnoInstallerScript.iss` builds an Inno Setup installer. It
needs `resources\ViGEmBus_1.22.0_x64_x86_arm64.exe`, which isn't in the repository; download it from
ViGEmBus releases. Releases of this fork are portable zips instead.

| Target | What |
| --- | --- |
| `SteamlessController` | The tray app |
| `SteamlessDeviceCycle` | Elevated helper: device restarts, blocker install and uninstall |
| `ViGEmBusProbe` | Checks for the ViGEm bus |
| `SteamInputLeaseProbe`, `DsuProbe`, `ForegroundProbe`, `SteamProbe`, … | Diagnostics |

## Versioning and releasing

Releases follow [Semantic Versioning](https://semver.org) and are tagged `vMAJOR.MINOR.PATCH`:

- **MAJOR** for changes that break settings or behaviour people rely on.
- **MINOR** for new features.
- **PATCH** for fixes.

2.0.0 is this fork's first release. It jumps from upstream's 1.25 because the control modes and
settings storage changed incompatibly.

To release:

1. Set the version in `resources/resource.h` (`APP_VERSION_*`) and `resources/InnoInstallerScript.iss`
   (`MyAppVersion`), and commit.
2. `scripts/package-release.sh` builds Release and writes
   `dist/SteamlessController-vX.Y.Z-win-x64.zip` plus its `.sha256`.
3. Tag and publish:
   `git tag vX.Y.Z && git push origin main vX.Y.Z &&
   gh release create vX.Y.Z dist/SteamlessController-vX.Y.Z-win-x64.zip* --title "vX.Y.Z"`.

`.github/workflows/release.yml` is upstream's signed-release workflow. It needs SignPath secrets
this fork doesn't have, so it no longer runs on tags.

## Credits and licences

- **[SteamlessController](https://github.com/ddeverill/SteamlessController)** by Dylan Deverill:
  the application this fork is built on (MIT).
- **[steam-input-lease](https://github.com/KillerPixelCrew/steam-input-lease)** (MIT):
  - the Steam Input gate, shipped unmodified as `steam_input_gate.dll`, with its licence and
    third-party notices beside it;
  - the pipe protocol the app speaks to it.
- **[sc2dsu](https://github.com/KillerPixelCrew/sc2dsu)** (MIT): the DSU server, motion conversion
  and axis defaults, ported to C++.
- **[GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers)** by Julian "Jibb"
  Smart (MIT): gyro drift calibration, ported via sc2dsu.
- **[ViGEmClient](https://github.com/nefarius/ViGEmClient)** (MIT): compiled in, for the virtual pad.
- **Microsoft Edge WebView2 SDK** (BSD-3-Clause): the settings window.

This project is MIT-licensed; see [`LICENSE`](LICENSE). The full licence text of every component
above is in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), which ships in every release.

**Privacy:** no data about you or your system leaves your PC. The settings window loads its fonts
from Google Fonts. The DSU server listens on this PC only, unless `DsuExposeToNetwork` is set.
Building from source downloads the WebView2 SDK and the steam-input-lease gate.
