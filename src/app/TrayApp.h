#pragma once
#include <Windows.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ControllerPlatform.h"
#include "DeviceRestart.h"
#include "ForegroundWatcher.h"
#include "GameProfiles.h"
#include "RemapWindow.h"
#include "SteamAppLocator.h"
#include "SteamInputLease.h"
#include "SteamWatcher.h"

class ControllerManager;

// When Steam's own presence replaces the default behaviour with Steam Input.
// Persisted as "SteamOverride", so values are fixed; the intent is to grow
// siblings (only while a Steam game runs, only while Steam is focused).
enum class SteamOverride : uint32_t {
    Off          = 0,
    SteamRunning = 1,  // steam.exe is running, which includes Big Picture
};

class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool Init(HINSTANCE hInstance);
    int  Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon(bool connected, bool gameModeActive, bool sharedHandle,
                        bool padUnavailable = false);
    void ShowViGEmBalloon();

    // What clicking the balloon should do. Balloons are transient and the click
    // arrives long after the call that raised one, so the action travels with
    // it rather than being inferred at click time.
    enum class BalloonAction { None, ViGEmDownload, EnableDisabledDevice };
    // Doing nothing is the default on purpose. It used to be ViGEmDownload,
    // which meant every caller that did not think about the question sent the
    // user to a driver download page — a dropped Bluetooth connection, Steam
    // refusing to hand the controller over, a devnode needing Device Manager.
    // None of those are ViGEmBus problems and none of them are fixed by
    // installing it. Opening something has to be asked for.
    //
    // infoFlags picks the icon Windows draws: a warning by default, because
    // most of these report something the user has to act on. Purely
    // informational notices pass NIIF_INFO.
    void ShowAlertBalloon(const std::wstring& title, const std::wstring& text,
                          BalloonAction action = BalloonAction::None,
                          DWORD infoFlags = NIIF_WARNING);
    void OpenEventLog();
    void ShowContextMenu();
    void LoadSettings();
    void SaveSettings();
    void OpenRemapWindow();

    // The tray app never runs elevated (an elevated foreground window blocks
    // unelevated SendInput — Steam Input's desktop cursor — via UIPI).
    // Privileged work (cycling the device node) is delegated to the
    // SteamlessDeviceCycle helper, registered as an on-demand highest-
    // privileges scheduled task by the installer or, failing that, by a
    // one-time UAC prompt here.
    static bool IsProcessElevated();
    bool EnsureCycleTaskRegistered();
    bool RunKeyExists() const;
    void SetRunKey(bool enabled);
    void DeleteLegacyStartupTask();
    void UpdateStartupRegistration();

    // Per-game profile switching. The foreground application decides which
    // profile is live, and strictly so: nothing is held for a game that is
    // still running behind something else.
    void OnForegroundChanged(const ForegroundIdentity& id);
    // The profile id matching an application, or empty when none does — and
    // always empty while game profiles are switched off, which is all it takes
    // for every other path to fall back to the default.
    std::wstring MatchProfile(const ForegroundIdentity& id) const;
    // Record `gameId`'s profile as the selected one (empty selects the
    // default), and nothing else. Returns whether the selection changed.
    //
    // Deliberately separate from applying it: what the controller should be
    // doing depends on which profile is live, so the selection has to be on
    // record before EvaluateControl can read it.
    bool SelectProfile(const std::wstring& gameId);
    // The selected profile's bindings — a game's if one is selected and has
    // controls of its own, else the default's.
    const ControllerProfile& ActiveProfile() const;
    // What to actually apply: ActiveProfile's bindings, wearing the pad type
    // the resolved behaviour asks for.
    ControllerProfile EffectiveProfile() const;
    // Push the effective profile into ControllerManager. Safe while the
    // controller is not ours, and called before acquiring rather than after so
    // the pad comes up already carrying the right bindings.
    void PushActiveProfile();
    // Re-resolve the foreground now, for the moments where the answer can
    // have changed while nobody was listening — taking the controller back,
    // or closing the window that suppresses switching.
    void RefreshActiveProfile();
    // Re-read Steam's install manifests. Driven by events rather than a
    // timer: startup, and whenever the user opens the window where profiles
    // are created. A game installed after that is invisible until one of
    // those happens again, which is the price of not polling the disk.
    void RefreshSteamApps();

    // What the controller should be doing right now, and which rule decided
    // it (for the log and the tooltip). Never Default. Meaningless while the
    // app is disabled, which EvaluateControl checks first.
    ControllerBehavior ResolveBehavior(const char** rule = nullptr) const;
    // The behaviour a game's profile asks for, or Default when there is no
    // such profile or it follows the default.
    ControllerBehavior ProfileBehavior(const std::wstring& gameId) const;
    // Act on ResolveBehavior — take or drop the Steam Input lease, bring the
    // virtual pad up or down — in the order that keeps the handoff clean. The
    // one place that turns intent into action, so every caller that can change
    // the answer routes through here.
    //
    // Turning anything off in response to the foreground waits out a short
    // settle, so one stray focus change does not cost a release and re-acquire
    // round trip. `immediate` skips it, for changes the user asked for
    // directly from the menu.
    void EvaluateControl(bool immediate = false);
    // Lease first, then pad: whether the lease has got as far as it is going
    // to for now, so an acquire will not find Steam still holding the device.
    bool LeaseSettled() const;
    void ApplySteamState(SteamState state);
    // Settings changed from the menu. Each saves, then re-evaluates at once.
    void SetEnabled(bool enabled);
    void SetDefaultBehavior(ControllerBehavior behavior);
    void SetGameBehavior(const std::wstring& gameId, ControllerBehavior behavior);
    void SetUseGameProfiles(bool use);
    void SetSteamOverride(SteamOverride value);
    // Re-derive the tray icon and tooltip from everything that feeds them —
    // the controller, the lease, and the settings.
    void RefreshTrayIcon();
    void TryAcquireController(uint32_t stateWaitMs = 250);
    // Take a dock Steam still holds, once the controller is ours.
    void CycleUnheldDocks();
    // skipCycle: Steam will be told to rediscover controllers some other way
    // (the gate does it when the lease goes), or is blocked from them anyway,
    // so the device cycle that hands the controller back is not needed.
    void ReleaseControl(bool skipCycle = false);
    void RecoverStrandedDevices();
    void WriteHeartbeat();
    // Carry out whatever the pending-disable record asks for: in process when
    // elevated, through the helper's task otherwise. Shared so there is one
    // place that knows how to get a devnode switched back on.
    bool RunPendingRecovery();
    // User-initiated repair of a devnode this app did not disable.
    void EnableDisabledControllerDevice();
    bool RestartControllerDevices();
    // Follow a cycle we asked for using enumeration alone, so that watching it
    // cannot veto it. Called in place of ControllerManager::OnDeviceChange for
    // as long as m_cycleInFlight is up.
    void TrackCycleProgress();
    // The one way out of the in-flight state: lowers the flag, cancels the
    // watchdog and resyncs the slots that were held off. Safe to call when no
    // cycle is running.
    void EndCycle(const char* why, bool resync = true);
    bool RefreshCycleStatus();
    bool LastCycleBrokeNothing();
    void ReportAcquireFailure();
    void ShowElevationBalloon();

    HWND                               m_hwnd       = nullptr;
    HINSTANCE                          m_hInstance  = nullptr;
    UINT                               m_wmTaskbar  = 0;
    HICON                              m_iconOff    = nullptr;
    HICON                              m_iconOn     = nullptr;
    HICON                              m_iconShared = nullptr;
    HICON                              m_iconSteam    = nullptr;
    HICON                              m_iconDisabled = nullptr;

    // Settings. Enabled off means hands off entirely: no pad, no lease.
    bool                               m_enabled         = true;
    ControllerBehavior                 m_defaultBehavior = ControllerBehavior::Xbox;
    bool                               m_useGameProfiles = true;
    SteamOverride                      m_steamOverride   = SteamOverride::Off;

    // Holds Steam off the controllers while the resolved behaviour wants it.
    SteamInputLease                    m_lease;
    // The last resolution logged, so a line is written on a change rather
    // than on every foreground switch.
    std::string                        m_lastResolution;

    // Do we want the virtual pad right now? Acquiring is asynchronous — a
    // blocked claim escalates to a device cycle that lands seconds later as a
    // WM_DEVICECHANGE — so intent has to outlive the call that started it.
    bool                               m_wantControl    = false;
    int                                m_acquireRetries = 0;
    // Dock-only cycles spent on the current takeover; see CycleUnheldDocks.
    int                                m_dockCycles     = 0;
    ULONGLONG                          m_lastCycleTick  = 0;
    // Verdict of the most recent device cycle, read back from the helper.
    DeviceRestart::CycleResult         m_lastCycleStatus;
    bool                               m_elevationBalloonShown = false;
    // Latched for the lifetime of one attempt, and deliberately not cleared by
    // an ordinary state notification: the acquire path notifies several times
    // per attempt, and clearing it there let a single attempt raise a balloon
    // on every pass. Reported from the field as toasts Windows was still
    // replaying minutes after the app had been closed (#68).
    bool                               m_vigemBalloonShown     = false;
    bool                               m_startupEnabled   = false;
    int                                m_startupMechanism = 0;  // 0 none, 1 Run key, 2 elevated task
    // Every balloon this app raises, not just the disconnect and stall ones
    // it started out covering — a per-game profile loading is announced
    // through it too.
    bool                               m_notificationsEnabled = true;
    BalloonAction                      m_balloonAction = BalloonAction::None;
    // Unbiased interrupt time at the last heartbeat, and the tick this instance
    // started. Unbiased so that time the machine spent asleep does not read as
    // the app having been blocked.
    ULONGLONG                          m_lastHeartbeatUnbiased = 0;
    ULONGLONG                          m_startTick = 0;
    // A cycle we fired is still running. While set, the device is deliberately
    // left unopened: our own handle vetoes the cycle just as readily as anyone
    // else's. Cleared by the arrival that ends the cycle, or by the tick
    // backstop when no arrival comes.
    bool                               m_cycleInFlight = false;
    bool                               m_inFlightLogged = false;  // one line per cycle
    // The interfaces the running cycle has to bring back before it counts as
    // finished, and which of them has been seen to go yet. Presence alone
    // proves nothing: a receiver publishes four interfaces and a narrowed cycle
    // takes down one, so the other three enumerate for the whole cycle. Only
    // gone-and-back is honest, per interface — see TrackCycleProgress.
    std::vector<std::wstring>          m_cyclePaths;
    std::vector<bool>                  m_cycleGone;
    // Narrowing request for the next cycle, consumed by RestartControllerDevices.
    // Empty means cycle everything, which is what a caller that never sets it
    // gets — so one path's narrowing cannot leak into another path's cycle.
    std::vector<std::wstring>          m_cycleRequestPaths;
    // Devnode found disabled when the menu was last built, so the command
    // handler and the menu agree on what "re-enable" refers to.
    std::wstring                       m_disabledDeviceNode;
    // The game profile each per-game menu item refers to, captured when the
    // menu was built so a command cannot land on a different profile.
    std::vector<std::wstring>          m_menuProfileIds;
    std::mutex                         m_alertMutex;   // guards the two alert strings
    std::wstring                       m_alertTitle;   // set on read threads,
    std::wstring                       m_alertText;    // consumed on WM_ALERT
    std::unique_ptr<ControllerManager> m_controller;
    SteamWatcher                       m_steamWatcher;
    // The Steam state we have actually acted on. The watcher announces changes
    // through a single PostMessage and only ever announces an edge, so a
    // message that is lost, dropped or arrives while nothing is pumping is
    // never repeated: the app then sits on a stale view of Steam until the
    // next change, which is a user watching a game launch and the controller
    // never being handed back. A timer compares this against the watcher and
    // applies the difference, so any single lost edge costs seconds, not the
    // rest of the session.
    SteamState                         m_lastAppliedSteamState = SteamState::NoSteam;
    ForegroundWatcher                  m_foregroundWatcher;
    // Bridges a Steam profile's app id to the running executable. Refreshed
    // at the few moments the answer can have changed — see RefreshSteamApps.
    SteamAppLocator                    m_steamApps;
    RemapWindow                        m_remapWindow;
    // Per-game overrides, keyed by GameLibrary's opaque game id.
    // Most users never populate this; empty by default.
    std::map<std::wstring, ControllerProfile> m_gameProfiles;
    // The user's own settings, and the only profile that is ever persisted to
    // the app's registry key. Deliberately not the same thing as whatever
    // ControllerManager is running: while a game profile is active that is a
    // per-game override, and writing it back would quietly replace the user's
    // defaults with some game's bindings.
    ControllerProfile                  m_defaultProfile;
    // Which game's profile is live, or empty for the default. The key into
    // m_gameProfiles, so it is also the answer to "did the active profile
    // actually change" without comparing whole profiles.
    std::wstring                       m_activeGameId;
    // The game the "profile loaded" balloon last named, so alt-tabbing in and
    // out of one game does not raise it over and over.
    std::wstring                       m_toastedGameId;
    // What was last reported as applied, so the line is written on a change
    // rather than on every foreground switch.
    std::wstring                       m_lastAppliedDescription;

    // 1001 was the retired "Enable/Disable Steamless Mode" toggle, and 1010-
    // 1012, 1016 and 1017 the retired control modes and "Enable on Launch".
    // All left unused rather than reassigned, like 1003/1005 below.
    static constexpr UINT IDM_EXIT          = 1002;
    // 1003 and 1005 were the retired "Enable Trackpad Mouse" and "Use Left
    // Trackpad Instead" items — left unused rather than reassigned, so a
    // stale menu message from an old build can't land on a new command.
    static constexpr UINT IDM_REMAP_BACK    = 1004;
    static constexpr UINT IDM_STARTUP       = 1006;
    // 1008 and 1009 were the retired "Controller Platform" submenu items,
    // now a dropdown in the customization window. Left unused rather than
    // reassigned, like 1003/1005 above.
    static constexpr UINT IDM_OPENLOG       = 1013;
    static constexpr UINT IDM_NOTIFICATIONS = 1014;
    static constexpr UINT IDM_ENABLE_DEVICE = 1015;
    static constexpr UINT IDM_ENABLED          = 1018;
    static constexpr UINT IDM_USE_PROFILES     = 1019;
    static constexpr UINT IDM_STEAM_OVERRIDE   = 1020;
    static constexpr UINT IDM_EDIT_PROFILES    = 1021;
    // One per ControllerBehavior value, offset by the value itself.
    static constexpr UINT IDM_DEFAULT_BASE     = 1100;
    // Game profile i, behaviour b: IDM_PROFILE_BASE + i * IDM_PROFILE_STRIDE + b,
    // with i indexing m_menuProfileIds as it was when the menu was built.
    static constexpr UINT IDM_PROFILE_BASE     = 2000;
    static constexpr UINT IDM_PROFILE_STRIDE   = 8;
    static constexpr UINT IDM_PROFILE_MAX      = 500;
    static constexpr UINT WM_TRAY           = WM_APP + 1;
    static constexpr UINT WM_STEAMSTATE     = WM_APP + 2;
    static constexpr UINT WM_ALERT          = WM_APP + 3;
    // Posted rather than acted on directly: the controller notifies from its
    // read thread, and the remap window is WebView2, which must only be
    // touched on the UI thread.
    static constexpr UINT WM_CONTROLSTATE   = WM_APP + 4;
    // The lease worker changed state; posted from its thread.
    static constexpr UINT WM_LEASESTATE     = WM_APP + 5;
    static constexpr UINT TRAY_UID          = 1;
    static constexpr UINT_PTR IDT_ACQUIRE         = 1;
    static constexpr UINT_PTR IDT_ACQUIRE_VERDICT = 2;
    static constexpr UINT_PTR IDT_WAKE_POLL       = 3;
    static constexpr UINT_PTR IDT_HEARTBEAT       = 4;
    static constexpr UINT_PTR IDT_RELEASE_GRACE   = 5;
    static constexpr UINT_PTR IDT_STEAM_RECONCILE = 6;
    static constexpr UINT_PTR IDT_CYCLE_WATCHDOG  = 7;
    // 8 was IDT_GAME_LIVENESS, retired with the running-game hold.
    static constexpr UINT_PTR IDT_DOCK_CYCLE      = 9;
    // Long enough that idling for a month costs a fraction of the log's 512 KB,
    // short enough to bound when the app stopped responding to within a
    // quarter hour. Resolution only has to beat "somewhere in the last 33
    // hours", which is what the log offered the last time this mattered.
    static constexpr UINT HEARTBEAT_MS = 15 * 60 * 1000;
    // How often to check that we actually acted on Steam's current state.
    // See m_lastAppliedSteamState for why that needs checking at all.
    static constexpr UINT STEAM_RECONCILE_MS = 5000;
    // Slack before a late beat is called a stall. Window timers are low
    // priority and coalesced, so a beat is routinely a little late; a full
    // minute of drift is not.
    static constexpr UINT HEARTBEAT_SLACK_MS = 60 * 1000;
    // A multi-slot receiver publishes every slot interface permanently, so a
    // controller waking up produces no device-change event — the only way to
    // notice is to keep asking. The probe is short because a live slot streams
    // continuously and answers within a few reports.
    static constexpr UINT WAKE_POLL_MS  = 2000;
    static constexpr UINT WAKE_PROBE_MS = 80;
    // A dock that could not be claimed is cycled on its own shortly after the
    // takeover — late enough for the helper run that won the slot to have
    // exited, since the task drops a second run while one is going — and at
    // most this many times per takeover before leaving Steam holding it.
    static constexpr UINT DOCK_CYCLE_DELAY_MS = 500;
    static constexpr int  MAX_DOCK_CYCLES     = 2;
    static constexpr int  MAX_ACQUIRE_CYCLES = 3;
    // Must outlast a full device cycle: the helper waits a second between
    // disable and enable, then a multi-slot receiver re-enumerates every
    // interface. At 2500 the app declared failure while the last cycle's
    // arrival was still in flight.
    static constexpr UINT ACQUIRE_RETRY_MS   = 4000;
    // Grace after the final cycle before reporting failure to the user.
    static constexpr UINT ACQUIRE_VERDICT_MS = 3000;
    // Retry spacing when the controller is fine but no virtual pad can be
    // created. Nothing here is a race, so the fast cadence above buys nothing:
    // what it is waiting for is a human installing a driver. Slow enough that
    // waiting through the whole thing costs a handful of log lines, quick
    // enough that the app picks a new driver up on its own.
    static constexpr UINT VIGEM_RETRY_MS = 30000;
    // Settle before a foreground-driven change turns anything off — dropping
    // the pad, changing its type, or releasing the lease. On top of the
    // ForegroundWatcher's own 400 ms debounce. Short, because the change is
    // what the user is waiting to see; non-zero, because each release costs
    // Steam a rediscovery and each pad drop is an unplug the game can see, and
    // one stray focus change (a launcher splash, a UAC prompt) should not buy
    // a round trip of both. Engaging is never delayed.
    static constexpr UINT RELEASE_GRACE_MS = 1000;
    // Minimum spacing between device cycles. A cycle is asynchronous, so
    // without this the arrivals it generates re-enter the acquire path and
    // fire another one on top of it.
    static constexpr ULONGLONG CYCLE_MIN_GAP_MS = 4000;
    // Ceiling on a single cycle. Nothing else lowers the in-flight flag if the
    // helper dies, the device never comes back, or the arrival lands while
    // nothing is pumping — and a flag left raised means the controller is never
    // opened again. Generous, because an unnarrowed cycle of a four-interface
    // receiver legitimately runs several seconds.
    static constexpr UINT CYCLE_MAX_MS = 20000;
};
