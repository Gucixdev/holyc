# org.freedesktop.login1 surface implemented by qlogind

Rule: this file is the ONLY compatibility claim. Anything not listed returns
`org.freedesktop.DBus.Error.UnknownMethod` / is absent from introspection.
Sources read (2026-11-08): libseat `libseat/backend/logind.c` (seatd, master) and elogind
`src/login/logind-dbus.c`, `logind-session-dbus.c`, `logind-seat-dbus.c`, `logind-user-dbus.c` (main), for
the exact member names and signatures. wlroots, Xorg and dwl do NOT talk to login1 themselves: they go through
libseat (logind backend) or, on Xorg, the `systemd-logind` module (same Session methods as libseat). polkit
(logind-using authority) and dbus-daemon were not re-read; their needs are listed from the elogind API they use
(ListSessions/GetSessionByPID/GetUserByPID/session properties) and are marked "unverified consumer" below.

Status column: `T` = covered by an automated test in tests/ (see readme for what the tests actually prove),
`-` = planned, not implemented yet. Updated at each commit.

## Not on D-Bus but required: the sd_login state files

libseat's logind backend (and anything linked against libsystemd/libelogind) first calls `sd_session_is_active`,
`sd_session_get_seat`, `sd_uid_get_display`, `sd_pid_get_session`. Those read plain files, not D-Bus:
`/run/systemd/{sessions,seats,users}/<id>` (KEY=VALUE). qlogind writes them (root dir overridable with
`QLOGIND_RUN`). `sd_pid_get_session` resolves via **cgroup**, which qlogind does not have: such callers only work
when `XDG_SESSION_ID` is exported by the session starter (qlogin does this). This is a hard, documented limit.

## Manager  /org/freedesktop/login1  (interface org.freedesktop.login1.Manager)

| Member | Sig | Consumer | Status |
|---|---|---|---|
| GetSession | s -> o | libseat (every start) | - |
| GetSeat | s -> o | libseat | - |
| GetSessionByPID | u -> o | polkit/portals (unverified consumer); pid match = leader or ancestor via /proc | - |
| GetUser, GetUserByPID | u -> o | polkit/portals (unverified) | - |
| ListSessions | -> a(susso) | polkit, loginctl-likes | - |
| ListUsers | -> a(uso) | loginctl-likes | - |
| ListSeats | -> a(so) | loginctl-likes | - |
| ListInhibitors | -> a(ssssuu) | desktops (unverified) | - |
| CreateSession | uusssssussbssa(sv) -> soshusub | pam_elogind-like callers, qlogin itself | - |
| ReleaseSession | s | pam_elogind-like callers | - |
| ActivateSession | s | desktops / loginctl | - |
| LockSession, UnlockSession, LockSessions, UnlockSessions | s / - | desktops | - |
| KillSession | ssi | loginctl-likes | - |
| Inhibit | ssss -> h | every desktop/media player (fd semantics: closing the fd releases) | - |
| PowerOff, Reboot, Halt, Suspend, Hibernate, HybridSleep, SuspendThenHibernate | b | desktops (DRY RUN by default) | - |
| CanPowerOff, CanReboot, CanHalt, CanSuspend, CanHibernate, CanHybridSleep, CanSuspendThenHibernate | -> s | desktops | - |
| Properties: IdleHint, IdleSinceHint(+Monotonic), BlockInhibited, DelayInhibited, InhibitDelayMaxUSec, PreparingForShutdown, PreparingForSleep, NAutoVTs, KillUserProcesses, HandlePowerKey, HandleLidSwitch* (read-only constants), NCurrentSessions, NCurrentInhibitors | | desktops | - |
| Signals: SessionNew(so) SessionRemoved(so) UserNew(uo) UserRemoved(uo) SeatNew(so) SeatRemoved(so) PrepareForShutdown(b) PrepareForSleep(b) | | desktops, power managers | - |

Everything else in elogind's Manager (ScheduleShutdown, SetRebootParameter, *WithFlags, Terminate*, AttachDevice,
FlushDevices, SetWallMessage, ...) is NOT implemented.

## Session  /org/freedesktop/login1/session/<escaped id>  and  /session/auto, /session/self

| Member | Sig | Consumer | Status |
|---|---|---|---|
| Activate, Lock, Unlock | - | desktops | - |
| SetIdleHint, SetLockedHint | b | desktops | - |
| TakeControl | b | libseat, Xorg (session controller, released if the caller drops off the bus) | - |
| ReleaseControl | - | libseat | - |
| SetType | s | libseat (XDG_SESSION_TYPE) | - |
| TakeDevice | uu -> hb | libseat/wlroots/dwl, Xorg | - |
| ReleaseDevice | uu | libseat | - |
| PauseDeviceComplete | uu | libseat | - |
| Signals PauseDevice(uus), ResumeDevice(uuh), Lock, Unlock | | libseat, desktops | - |
| Properties Id Name User(uo) Seat(so) VTNr Type Class Active State Remote Leader TTY Display Service Desktop Scope IdleHint LockedHint CanIdle CanLock | | libseat (Active, via Get + PropertiesChanged), desktops | - |
| org.freedesktop.DBus.Properties Get/GetAll + PropertiesChanged for Active, State, IdleHint, LockedHint | | libseat | - |

Not implemented: Terminate, Kill, SetClass/SetDisplay/SetTTY, SetBrightness, AuditSessionID values.

## Seat  /org/freedesktop/login1/seat/<id>  and  /seat/auto

Properties Id, ActiveSession(so), Sessions(a(so)), CanGraphical, CanTTY, CanMultiSession, IdleHint. Methods
SwitchTo(u) (libseat switch_session), ActivateSession(s). Not implemented: Terminate, SwitchToNext/Previous.

## User  /org/freedesktop/login1/user/<uid>  and  /user/self

Properties UID, GID, Name, RuntimePath, State, Sessions(a(so)), Display(so), Linger(always false). Not implemented: Terminate, Kill.

## Standard interfaces on every object

org.freedesktop.DBus.Introspectable (generated from the same tables that dispatch calls, so XML and behaviour
cannot drift), org.freedesktop.DBus.Properties (Get, GetAll, Set rejected), org.freedesktop.DBus.Peer
(Ping, GetMachineId; libseat pings login1 to know it is alive).

## Hard limits (also in the readme)

- No cgroups: session membership is by leader PID and its descendants in /proc, not by cgroup. Processes that
  double-fork and re-parent to init leave their session in `GetSessionByPID`.
- No PAM module is shipped (pam_elogind/pam_systemd calls CreateSession; qlogin calls it itself).
- No DRM master revoke / EVIOCREVOKE on pause; PauseDevice/ResumeDevice are delivered, kernel-side revocation is not done.
- No polkit: authorisation is by uid (root, or user of an active local session). No ScheduleShutdown, no wall.
