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
| Device `two_holders_possible` | the OLD per-session `l1_find_dev` design let two sessions hold the same major:minor (kept as the counterexample) | old `m_s_takedevice` | see Findings 3 |
| Device `two_holders_refused`, `takeDeviceG_free` | FIXED design: a take succeeds only if no OTHER session holds the key (system-wide) | `l1_dev_holder` + `m_s_takedevice` | |
| Session `dup_vt_refused`, `other_vt_accepted` | the old `dup_vt_first_wins` witness is now refused at create; other VTs still accepted | `vtFree` guard in `m_createsession` | |
| Session `markClosing_state` | a session being dropped is not active and reads State "closing" | first half of `l1_drop_session` | |

## Findings (all four fixed in the C, 2026-11-08; the original text is kept in the git history)

1. `closing` was never set: FIXED. `l1_drop_session` now sets it, refreshes the seat (another session may become active, the
   dying one's devices are paused) and sends PropertiesChanged State="closing" before SessionRemoved. Test: login1_test checks the
   signal precedes SessionRemoved. Model: `markClosing_state`.
2. Two sessions on one seat+VT: FIXED. `m_createsession` refuses a NEW session whose seat+VT is used by a non-closing session
   (System.Error.EBUSY); the same-leader "existing session" path is unchanged. Model: `dup_vt_refused`.
3. Device held by two sessions: FIXED, not just a note. `l1_dev_holder` checks every session; TakeDevice on a device another
   session holds fails with System.Error.EBUSY (a device frees up on ReleaseDevice / ReleaseControl / session drop). This is
   stricter than logind. Model: `takeDeviceG_free`, `two_holders_refused`.
4. Activate on a VT-less session: FIXED. `Session.Activate`, `Manager.ActivateSession` and `Seat.ActivateSession` now return
   System.Error.ENOTSUP instead of succeeding silently. Still true and by design: a seat may have NO active session; the model
   proves "at most one", not "exactly one".
