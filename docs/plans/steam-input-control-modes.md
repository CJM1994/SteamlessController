# Plan: Default behaviour, game profiles, and global Steam Input blocking

Goal: replace the tray's control modes (Manual + three Auto modes) with a simpler
model, **one default behaviour plus optional per-game profiles**, each able to
choose Xbox, PlayStation, Lizard Mode or Steam Input. "Blocking Steam Input" uses
the gate from
[steam-input-lease](https://github.com/KillerPixelCrew/steam-input-lease)
(MIT, reviewed in `reference/steam-input-lease/`, which git ignores).

## Status (supersedes parts of §0 and §3 below)

Implemented on `feature/steam-input-modes`. Where the build and the sections
below differ, the build wins:

- **Three modes**, not four behaviours: Steamless Mode, Steam Input Mode and
  Lizard Mode. Xbox vs PlayStation is each profile's "Appear to games as", set
  in its editor; game profiles have no "follow the default" mode.
- **Tray menu:** Enabled; Default Behaviour (the three modes, plus "Use Steam
  Input Mode while Steam is running"); Game Profiles (Use Game Profiles, each
  game's mode plus "Edit Profile...", "Edit Game Profiles..."); "Edit Default
  Profile...".
- **Editor scopes:** the default profile alone, game profiles only (no
  default), or a single game.
- **"In front"** means what is physically visible: the focused window, unless
  another app's window that covers 90% or more of that monitor shows at its
  centre. Re-checked every 750 ms, because a change in stacking order raises no
  foreground event.
- **Mode notices** are drawn by `ModeOverlay` (an always-on-top, click-through
  layered window), not Windows notifications, which Do Not Disturb suppresses
  while games run. Raised only once a switch has fully landed, Steam's reopen
  included.
- **Handing over to programs that read the controller directly** (emulators
  via SDL):
  - Switching to Lizard after an exclusive hold restarts the device, so they
    see an arrival.
  - While the gate blocks Steam, a device another program already holds is
    shared rather than cycled, and released without a restart. A restart would
    be vetoed by that program's handle and end with PnP yanking the device out
    from under it.
  - Input leaks into the sharer only if it reads input while unfocused; this
    is logged as `SHARING:`.

## 0. Behaviour spec

### Behaviours

Every decision resolves to exactly one of these:

| Behaviour | Steam | Virtual pad (ViGEm) | Tray icon |
| --- | --- | --- | --- |
| Xbox Controller | Blocked | X360 | Blue |
| PlayStation Controller | Blocked | DS4 | Blue |
| Lizard Mode | Blocked | None, firmware lizard mode | Red |
| Steam Input | Allowed | None | **Green (new icon)** |

"Blocked" is global by construction. The gate runs inside `steam.exe` and denies
Steam's own HID opens/reads and XInput queries, so Steam Input stops for every
game whether Steam launched it or not. This also hides other controllers
(DualSense, Xbox pads) from Steam while blocked, though games still see them
directly. **Accepted.**

### Resolution

Highest priority first:

1. **App disabled** (tray toggle off) → do nothing. Release the pad, release the
   lease, and leave the controller entirely to Steam/firmware. The icon is gray.
2. **Game profiles on**, and the foreground app has a game profile → that
   profile's behaviour.
3. **"Use Steam Input while Steam is running"** is checked and `steam.exe` is
   running (this covers Big Picture) → Steam Input.
4. Otherwise → the **default behaviour**.

**Strictly follows the foreground.** Nothing is held for a game running in the
background. The existing `GameLiveness` pad-type hold is removed, so alt-tabbing
away from a game applies the new app's behaviour, pad type included. Interruptions
happen only when the two behaviours differ, and quick alt-tabs under ~1.4 s cause
none (debounce + settle):

| Switching between | Effect | Interruption |
| --- | --- | --- |
| Same behaviour | Nothing | None |
| Xbox ↔ PlayStation | Pad rebuilt as the other type | < ~0.5 s, game sees an unplug |
| Xbox/PS ↔ Lizard | Pad unplugged/replugged; Steam stays blocked | < ~0.5 s |
| Anything ↔ Steam Input | Lease released or taken; Steam rescans | ~1–3 s |

Accepted. A per-profile opt-in "keep behaviour while running" can be added later
if a specific game handles unplugs badly.

Bindings (paddles/trackpads) still come from the foreground's profile as today,
or from the default mappings when profiles are off.

### Tray menu

```
☑ Enabled                                   (disables the whole app, stays in tray)
─────────────
Default Behaviour ▸  ○ Xbox Controller
                     ○ PlayStation Controller
                     ○ Lizard Mode
                     ○ Steam Input
                     ─────────────
                     ☐ Use Steam Input while Steam is running
Game Profiles ▸      ☑ Use Game Profiles
                     ─────────────
                     Hades            ▸ ○ Default ○ Xbox ○ PlayStation ○ Lizard ○ Steam Input
                     Celeste          ▸ ...
                     ─────────────
                     Edit Game Profiles...   (opens Customize Controls)
Customize Controls...
─────────────
Steam Input blocker: Active ✓ | Restart Steam to activate | Install…
─────────────
Start with Windows / Open Event Log / Show Notifications / Exit
```

- Each game's submenu repeats the behaviour dropdown from the Customize window,
  for quick changes. The game list is the existing `m_gameProfiles`, labelled the
  same way the Customize window labels them. With profiles off the list is greyed
  out but stays visible.
- **Game profile behaviour includes "Default"** (follow the default behaviour).
  This covers profiles made only for bindings. Existing profiles migrate to
  Default when `useDefaultMappings` is set, and to their stored Xbox/PS platform
  otherwise.
- The "Use Steam Input while Steam is running" item is the first of a planned
  family. Later siblings: "…while a Steam game is running" (`SteamState::InGame`)
  and "…while Steam is focused" (foreground is `steam.exe`/`steamwebhelper.exe`).
  Store it as an enum (`SteamOverride { Off, SteamRunning }`) so those slot in
  without a settings migration.

### Customize Controls window (`RemapWindow`)
- The **default profile no longer has an "Appear to games as" dropdown.** That
  lives in the tray only. The default profile is now just the default bindings.
- Game profiles get the dropdown with **Default / Xbox / PlayStation / Lizard
  Mode / Steam Input**.
- When a profile's behaviour has no pad (Lizard or Steam Input), the bindings and
  trackpad sections are dimmed with a one-line note.

### Removed
- `AutoMode` and all four modes, along with the Steam-state yielding in
  `EvaluateControl`/`ApplySteamState`. `SteamWatcher` stays: it drives the Steam
  override and tells the lease client when Steam starts or restarts.
- "Enable on Launch" (`IDM_ENABLE_ON_LAUNCH`). The Enabled toggle is persisted,
  so the app comes back up in whatever state it was left in.

### Switching delays (the 5 s question)

The 5 s `RELEASE_GRACE_MS` existed for the old "Off unless profile" mode, so that
alt-tabbing out of a game did not unplug its controller straight away. Here a
change should land soon after the foreground changes, so it is cut to a short
settle:

| Trigger | Delay |
| --- | --- |
| **Menu clicks** (Enabled, default behaviour, a profile's behaviour) | **Immediate**, no grace. The user asked for it |
| **Engaging** (block on, pad on) from foreground/Steam changes | Immediate, as today |
| **Any foreground-driven change that disengages** (block off, pad off, pad type change) | **~1 s** settle on top of the existing 400 ms foreground debounce |
| **Steam starts/stops** (override) | SteamWatcher's existing 2 s poll and debounce, unchanged |

Why not zero for disengaging: every lease release makes Steam run controller
rediscovery, and the gate's second pass lands about 2.2 s later. Every pad drop
is an unplug that any game holding the pad can see. A short settle stops one
stray focus change, like a launcher splash or a UAC prompt, from causing a
release and re-acquire round trip. The gate reference-counts and copes with
quick flips, so the cost of a short settle is cosmetic, not app fighting.
`RELEASE_GRACE_MS` becomes a constant to tune on real hardware, starting at
1000.

---

## 1. How the lease works

Summarised from the reference README and `crates/steam-input-lease-core`.

- **Gate DLL** (`steam_input_gate.dll`, Rust + MinHook) is copied into Steam's
  folder as `XInput1_4.dll`, with `dinput8.dll` as the fallback name. Steam loads
  it on its next **cold start**. While no lease is held it only forwards calls.
- **Control pipe** `\\.\pipe\SteamInputGate-<steam pid>`. `AcquireLease` on the
  0→1 transition closes Steam's HID handles and denies new ones. Blocking lasts
  **as long as the pipe stays open**: if we crash or exit, blocking ends and Steam
  rediscovers controllers.
- **Release** (explicit, or by closing the pipe) restores pass-through and runs
  the gate's two-pass rediscovery when it advertises
  `CAPABILITY_INTERNAL_RECOVERY`.
- **Wire protocol v1.** The request is 8 bytes `{u32 magic 0x53494754, u16 version=1,
  u16 cmd}` and the response is 24 bytes `{u32 magic, u16 version, u16 caps, u32 result,
  u32 lease_count, u32 hid_handle_count, u32 last_revoked}`. Commands: 1 Acquire,
  2 Status, 3 Release.

### Decisions
- **Native C++ client** (~250 lines), not `steam_input_lease_ffi.dll`, which
  keeps Rust out of the build. It keeps upstream's hardening: find `steam.exe`
  only in our own session, open the pipe with `SECURITY_SQOS_PRESENT |
  SECURITY_IDENTIFICATION`, and check the server with `GetNamedPipeServerProcessId`.
  The host-side recovery resolver is not ported. If the gate ever lacks internal
  recovery, fall back to the existing `SteamlessDeviceCycle` device cycle.
- **Prebuilt upstream gate `v0.1.0`**, pinned by SHA-256 and vendored the same
  way as the ViGEmBus installer.
  - zip `25979e1333dce6a49cfaa9b57cbd63e2e8a5b1188ba565771fa458d43315fb2c`
  - `XInput1_4.dll` `11d9c4acc9771e39a986dc5e627b0285d5830a85f730af39abea2e7b903507cd`

## 2. Gate deployment (one-time, needs admin)

Steam's folder (`HKCU\Software\Valve\Steam\SteamPath`) normally needs admin to
write to, and the tray app is never elevated. Deploy from the **Inno installer**,
with a `--deploy-gate` / `--park-gate` verb on `SteamlessDeviceCycle` for repair
from the tray. Upstream's rules:

- Only replace an `XInput1_4.dll`/`dinput8.dll` that exports
  `WsgmSteamInputGateProxy`. ValvePlug and Special K use the same names. Fall
  back to `dinput8.dll`, and report it if both names belong to other programs.
- Never overwrite a mapped image. If Steam is running, the change applies on its
  next cold start.
- Uninstall renames the file to `.dlld` rather than deleting it.

The tray status line comes from a pipe status query.

## 3. Code changes

### New: `src/app/SteamInputLease.{h,cpp}`
```cpp
class SteamInputLease {
public:
    enum class Status { NoSteam, GateMissing, Idle, Blocking, Error };
    void SetWanted(bool block);           // idempotent; the single source of intent
    Status GetStatus() const;
    void OnSteamStateChanged(SteamState); // Steam (re)started -> new pid -> re-lease
};
```
- A worker thread owns the pipe, so nothing blocks the UI thread. State comes
  back as `WM_LEASESTATE`.
- On EOF while a lease is still wanted, re-acquire when Steam reappears, with
  backoff for about 30 s while the gate's pipe comes up. Logs use a `LEASE:`
  prefix.

### Model
- `enum class Behavior { Default, Xbox, PlayStation, Lizard, SteamInput };`
  `Default` is only valid on game profiles. Add `ControllerProfile::behavior` and
  keep `platform` for `VirtualController`, derived from the behaviour whenever a
  pad is wanted.
- TrayApp settings (registry): `Enabled`, `DefaultBehavior`, `UseGameProfiles`,
  `SteamOverride`. One-time migration from `AutoSteamMode`/`ControllerPlatform`:
  - `DefaultBehavior` comes from the old platform (Xbox/PS).
  - `SteamOverride = SteamRunning` if the old mode was OffWhileSteam.
  - `UseGameProfiles = on`.
  - `Enabled = EnableOnLaunch` for old Manual users, otherwise on.
- Game profiles: a new `Behavior` DWORD per profile, migrated as described in §0.

### `TrayApp`
- **`ResolveBehavior()`** replaces `WantControlNow()`/`SteamAllowsControl()`
  and implements the §0 resolution order. It is logged once per change, along with
  which rule decided.
- **`EvaluateControl()`** derives `wantPad` and `wantBlock` and applies them in
  the order that keeps the handoff clean:
  - Block on: **lease first**. The gate closes Steam's handles, so
    `EnableGameMode` gets an exclusive handle with no device cycle, pounce or
    elevated helper.
  - Block off: `ReleaseControl()` first, **then** the lease, so Steam's
    rediscovery finds the device free. Skip `ReleaseControl`'s own device cycle
    when the lease release will trigger rediscovery anyway.
  - Pad ↔ Lizard: the lease stays, and only the pad comes up or goes down.
  - Xbox ↔ PlayStation: an existing `SetProfile` platform rebuild.
- **No gate yet:** wanting the pad falls back to today's cycle-based takeover,
  and Lizard just releases the device (Steam may grab it). One balloon says Steam
  Input is not blocked until the gate is active. **Steam not running:** the lease
  client waits at `NoSteam` and takes the lease when Steam appears.
- Remove the running-game hold: `m_platformHold`, `UpdatePlatformHold`,
  `ReleasePlatformHold`, `IDT_GAME_LIVENESS`, and `EffectiveProfile`'s platform
  override (`EffectiveProfile` collapses into `ActiveProfile`). Delete
  `GameLiveness.{h,cpp}` if nothing else uses them.
- The menu handlers set the setting, call `SaveSettings`, then
  `EvaluateControl(/*immediate=*/true)`, which bypasses the settle timer.
- Remove the mode branches in `Init`, `WM_COMMAND` and `ApplySteamState`.
  `Init` runs `EvaluateControl()` once the first Steam state and profile are known.
- The remap window's "Enable Steamless Mode" banner turns on Enabled.

### Tray icon (`UpdateTrayIcon`)
- New icons: `SteamControllerSTEAM.ico` (green) and `SteamControllerDISABLED.ico`
  (gray), recoloured from the existing one at the same sizes, plus `IDI_ICON_STEAM`
  and `IDI_ICON_DISABLED`.
- Priority: disabled → gray. Pad active → blue, or the existing shared icon when
  sharing. Steam not blocked → green. Otherwise → red.
- Tooltip names the behaviour and the rule, for example "Hades profile — Steam
  Input" or "Default — Xbox Controller".

### IDs
New: `IDM_ENABLED=1018`, `IDM_DEFAULT_BASE=1100` (4 behaviours),
`IDM_STEAM_OVERRIDE=1110`, `IDM_USE_PROFILES=1111`, `IDM_EDIT_PROFILES=1112`,
`IDM_GATE=1113`, and `IDM_PROFILE_BASE=2000` (profile index × 8 + behaviour, with
an index→id table rebuilt on every menu open). Retired IDs (1001, 1010–1012,
1016, 1017) stay unused, following the file's existing convention.

## 4. Build and packaging
- `CMakeLists.txt`: add `SteamInputLease.cpp`, plus a console
  `SteamInputLeaseProbe` with `--status`, `--acquire` (held until Enter) and
  `--release`.
- `InnoInstallerScript.iss`: ship the gate, deploy it at install, park it at
  uninstall. Add the upstream MIT notice to the README.
- `release.yml`: download the pinned upstream zip and verify its hash.

## 5. Implementation order
1. `SteamInputLease` + `SteamInputLeaseProbe`. **Checkpoint on Windows:** put
   upstream's standalone `XInput1_4.dll` in Steam's folder by hand, restart Steam,
   and confirm the probe makes Steam lose and regain the controller.
2. Settings model + migration, `ResolveBehavior`, and the `EvaluateControl`
   rewrite (lease ordering, immediate vs settle).
3. Tray menu (Enabled, Default Behaviour, Game Profiles) and the new icons.
   *(Done together with step 2: removing the old modes forced the menu change.)*
4. Behaviour in game profiles, its persistence, and the Customize window
   (dropdown moved off the default profile).
5. Gate deployment (installer + helper verb) and the tray status line.
6. Fallback paths: no gate, Steam restarting while a lease is held, Steam not
   running at launch.

## 6. Test matrix (Windows; puck / wired / BT)
- Every default behaviour, switched from the menu → the change is immediate and
  the icon colour, Steam/Big Picture visibility and lizard mouse are all correct.
- Enabled off → gray icon, Steam has the controller. On → back to the resolved
  behaviour.
- Profiles on: default Xbox → foreground a game set to Steam Input (green) →
  alt-tab to the desktop (blue within ~1.5 s) → back to the game (green, and Steam
  Input works in the game again within ~3 s). Alt-tab out and back in under 1 s
  → nothing changes.
- Lease churn: alt-tab between a Steam Input game and an Xbox desktop 20 times →
  Steam rediscovers the controller every time.
- Profiles off → profiled games use the default.
- Steam override on: start/quit Steam → flips to Steam Input and back. A
  profiled game in front still wins.
- Kill the app while a lease is held → Steam regains the controller. Restart Steam
  while a lease is held → the lease is re-acquired.
- Gate absent → Xbox/PS still work through the cycle path, and the warning
  balloon shows once.
