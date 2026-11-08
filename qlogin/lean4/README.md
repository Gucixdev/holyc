# qlogin / lean4

Machine-checked models of the pure logic of `qlogin` (greeter) and `qlogind` (login1). Core Lean only (no Mathlib),
no `sorry`.

    export PATH=$HOME/.elan/bin:$PATH
    lake build

**Honesty first: the C is not verified.** Each file is a hand-written model made by reading the named C function; nothing
mechanically ties the Lean to the C. A theorem says "the model has the property" and is evidence about the C only as far
as the model is faithful. The last column says where it is not. Sources: `../src/mods/greeter.h`, `../src/mods/login1.h`.

| File / theorem | Proves | Mirrors | NOT covered |
|---|---|---|---|
| Lockout `units_le_cap`, `lockMs_le_cap` | delay <= 30 units for any failure count | `ql_lock_ms` | `long` overflow of `units * unit_ms` with a huge `QLOGIN_DELAY_UNIT_MS` (test mode only); `int` wrap of `++*failures` after 2^31 failures |
| Lockout `units_mono`, `lockMs_mono`, `waits_nondecreasing` | delay is monotone non-decreasing in failures (1,2,4,8,16,30,30..) | `ql_lock_ms` | |
| Lockout `success_resets`, `counter_counts`, `fail_always_waits`, `run_waits_capped` | success sets the counter to 0 and the next failure waits 1 unit again; n failures = counter n; every failure waits >= 1 unit; no sleep in any event sequence exceeds the cap | `*failures = 0`, `ql_lock_ms(++*failures, ..)` in `ql_attempt` | the "PAM unavailable" path (returns 1, no count, `sleep(1)`); user-name changes between attempts (counter is global, not per user, which the model matches); `nanosleep` EINTR loop |
| Inhibit `mem_mask`, `mask_nodup`, `mask_sorted`, `has_iff_mem_mask` | `BlockInhibited`/`DelayInhibited` = the kinds held by some inhibitor of that mode, once each, canonical order | `l1_inh_mask`, `l1_inh_has` | string formatting (":" joins), name parsing, `what` strings longer than the buffers |
| Inhibit `acquire_mask`, `release_mask`, `release_subset`, `release_only_own`, `release_sole_holder`, `release_absent` | taking adds exactly its kinds; releasing removes exactly its own contribution (a kind leaves only if the released one held it, and does leave if nobody else holds it) | `m_inhibit`, fd close path | how the fd close is noticed (poll/HUP), the `L1_MAXINH` limit |
| Inhibit `delay_mask_small` | delay locks only ever cover shutdown/sleep | `k > 1` check in `m_inhibit` | |
| Inhibit `request_iff`, `request_busy` | a power action is accepted iff no other is pending and no BLOCK inhibitor holds its category | `l1_power` | authorisation (`l1_authorized`), the PrepareFor* signals |
| Inhibit `runs_iff`, `runs_at_deadline`, `runs_without_delay`, `late_block_ignored` | the pending action runs unless a DELAY lock holds the category and the deadline has not passed; at the deadline it always runs; a block lock taken AFTER the request does not stop it (as in the C and logind) | `l1_tick_action` | monotonic clock source, the dry-run/real command execution |
| Session `pick_mem`, `seatActive_mem` | the seat's active session is always an existing, non-closing session of that seat | `l1_seat_active` | |
| Session `refresh_atMostOne`, `refresh_consistent`, `refresh_state_sound` | after a refresh at most one session per seat is active; active implies seat, not closing, and being the named one; State "active" never goes with closing | `l1_refresh_active`, `State` property | files in /run/systemd (`l1_sync_files`), signals/PropertiesChanged |
| Session `create_inv`, `drop_inv`, `activate_inv` | the invariant holds after CreateSession, ReleaseSession, Activate/SwitchTo (each ends with a refresh) | `m_createsession`, `l1_drop_session`, `l1_set_vt` | holds because refresh recomputes from scratch, not because ops are clever (that is the C's design too) |
| Session `drop_no_dangling`, `drop_active_alive`, `drop_successor` | after removal the seat never names the removed (or any dead) session; if a survivor is VT-less or on the current VT the seat still has an active session | `l1_drop_session` | |
| Session `activate_selects`, `activate_unique`, `activate_vtless` | Activate(vt) makes a session on that VT the seat's active one, exactly the target if its VT is unique; vt 0 changes nothing | `l1_set_vt`, `m_activatesession` | real VT switching (dry-run in this build), authorisation |
| Device `takeDevice_needs_controller`, `takeDevice_refused`, `takeDevice_twice_refused` | TakeDevice by a non-controller (or on an uncontrolled session) is refused; a second take of the same device is refused | `m_s_takedevice` | `l1_dev_open`, fd passing, `L1_MAXDEV` |
| Device `inv_*` (takeControl, releaseControl, takeDevice, releaseDevice, setActive), `takeDevice_paused_flag` | invariant "every device's paused flag = session not active; devices only with a controller; no duplicate keys" is preserved by every operation; a new device starts paused iff the session is not active | `m_s_take*`, `m_s_release*`, `l1_apply_active` | controller loss when the peer leaves the bus (not in the model), PauseDeviceComplete |
| Device `pause_resume`, `resume_pause`, `setActive_keeps_keys` | pause then resume restores the session exactly; the device set is never changed by pause/resume | `l1_apply_active` | the signals themselves (PauseDevice/ResumeDevice), that the fd really works after resume |
| Device `unpaused_unique` | with at most one active session per seat (Session) a device key is held UNPAUSED by at most one session of the seat | combination of the two | |
| Device `two_holders_possible` | counterexample to "a device is held by at most one session": two sessions can both hold the same major:minor (one paused) | `l1_find_dev` is per session | see Findings 3 |

## Findings (nothing in the C was changed)

1. `closing` is never set. `login1.h` reads `s->closing` in `l1_seat_active` (line 176), `l1_refresh_active` (427), the State
   property (269, 361, 607) and `l1_by_pid` (240), but no code assigns it (sessions are `memset` to 0 at `m_createsession`, 861 ff.,
   and ReleaseSession calls `l1_drop_session` directly). The "closing" state in the online/active/closing machine is unreachable
   in the C; the model keeps the flag and the proofs hold for any value of it, so they stay valid if it gets wired in.
2. Two sessions can share a VT on the same seat: `m_createsession` (login1.h:806-875) checks only leader pid + uid for
   reuse, not `vtnr`. `l1_seat_active` returns the FIRST on the current VT, so `Activate` on the second leaves the first active
   (`dup_vt_first_wins`: sessions 1 and 2, seat 1, vt 2: `activate 2` names session 1). Minimal call: CreateSession twice with
   different leader pids, same seat0 and vtnr=2, then Activate the later one.
3. "A device is held by at most one session" is not enforced: `l1_find_dev` (login1.h:1056) looks only inside the calling
   session. Two sessions (even of different seats, since the device is not checked against the seat) can TakeDevice the same
   maj:min; one of them is paused (`two_holders_possible`). What does hold, per seat, is that at most one holder is unpaused
   (`unpaused_unique`). This matches logind's behaviour, so it is a note rather than a bug.
4. Smaller: `m_activatesession` / `m_s_activate` on a session with `vtnr == 0` returns success and does nothing (`l1_set_vt`
   returns at once, login1.h:888); a seat can have NO active session (e.g. the active VT session exits while `curvt` matches no
   remaining session): the model proves "at most one", not "exactly one", because the C allows zero.
