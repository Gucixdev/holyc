# netman / lean4

Machine-checked models of the pure logic of `netman`. Core Lean only (no Mathlib), no `sorry`.

    export PATH=$HOME/.elan/bin:$PATH
    lake build

**Honesty first: the C is not verified. Only these hand-written models are.** Each model was
written by reading the C (`../src/mods/net.h`, `../src/mods/vpn.h`) and mirrors it, but nothing
mechanically ties the Lean definitions to the C source. A theorem here says "the model has the property";
it is evidence about the C only as far as the model is faithful. The "Left out" lists say where it is not.

## Finding: T1 <= T2 can fail in the C (`nmbn__finalize`)

If a server sends a valid T1 above 7/8 of the lease and no valid T2 (e.g. lease 100, T1 95, T2 absent),
the C sets T2 = 87 < T1 = 95 (`finalize_counterexample`). `nmbn_dhcpc_step` then goes straight to a broadcast
rebind at T1 and never does the unicast renew. T2 < lease and expiry ordering still hold. The proposed fix
(`finalizeFixed`, clamp T1 to `min(T1, T2)`) is proven to give 0 < lease, T1 <= T2 < lease. Not applied to the C.

## Netman/Dhcp.lean (model of `nmbn__finalize`, `nmbn_dhcpc_step`)

| Theorem | Proves | C |
|---|---|---|
| `finalize_lease_pos`, `finalize_t1_lt`, `finalize_t2_lt` | lease > 0, T1 < lease, T2 < lease for any server input | `nmbn__finalize` |
| `finalize_defaults_ok` | with no server timers: T1 = L/2 <= T2 = 7L/8 | `nmbn__finalize` |
| `finalize_order` | T1 <= T2 holds except exactly when a valid server T1 > 7L/8 and no valid T2 | `nmbn__finalize` |
| `finalize_counterexample` | the violation is real (100/95/0 gives T1 95, T2 87) | `nmbn__finalize` |
| `finalizeFixed_ok` | the fixed version gives T1 <= T2 < lease | (proposed) |
| `act_monotone`, `renew_before_rebind`, `rebind_before_expire`, `renew_before_expire`, `phases` | the action by lease age only moves wait, renew, rebind, expire; with T1 < T2 < L all phases occur | thresholds in `nmbn_dhcpc_step` |
| `bound_needs_ack` | INIT goes to BOUND only on an ACK from connect | `nmbn_dhcpc_step` st==0 branch |
| `nak_returns_init` | NAK on renew/rebind: INIT and address unapplied | `nmbn_dhcpc_step`, `NMBN_E_NAK` branch |
| `expiry_drops` | at age >= lease: INIT and address dropped | `nmbn_dhcpc_step`, expiry branch |
| `bound_not_expired` | after any step, a BOUND lease satisfies now < got + lease (clock monotone) | `nmbn_dhcpc_step` |
| `step_inv` | invariant "address held iff BOUND, BOUND timers well-formed" is preserved | `nmbn_dhcpc_step` |
| `retry_in_future` | a failed renew schedules its retry strictly later (no busy loop) | `next_try = now + max(1, rest/2)` |

Left out: infinite leases (`0xffffffff`); sockets, packet parsing, options; `nmbn_lease_apply` failing after
an ACK (model assumes apply succeeds); lease cache / INIT-REBOOT (`nmbn_dhcp_connect`); IP-changed on rebind
(`ackChanged` is folded into `ack`); clock jumps backwards (hypothesis `got <= now`); the model's `step` is one
call, the C relies on `nmbn__due` to call it in time (a late call still drops the address, `expiry_drops`).

## Netman/Roaming.lean (model of `nmbn__best`, `nmbn__penalize`, the loop in `nmbn__pass`)

| Theorem | Proves | C |
|---|---|---|
| `le2_refl/trans/total`, `le3_refl/trans/total` | ranking by (priority, last success) and the 3-key extension with signal are total preorders | `nmbn__best` comparison |
| `better_iff` | C's strict replace condition is exactly "not le2" | `nmbn__best` |
| `best_mem` | the chosen candidate is in the list and not in backoff | `nmbn__best` |
| `best_isSome` | a candidate is chosen iff some candidate is eligible | `nmbn__best` |
| `best_max` | the chosen candidate is a maximum of the ranking among eligible ones | `nmbn__best` |
| `delay_pos`, `delay_le`, `delay_mono` | backoff 1, 2, 4, then 300 s: >= 1, <= 300, monotone in consecutive failures | `nmbn__penalize` |
| `penalize_ineligible`, `penalize_bounded` | a penalised slot is in backoff at once, and at most 300 s ahead | `nmbn__penalize` |
| `run_terminates` | with fuel > #eligible the loop never runs out of fuel (each failure strictly shrinks the eligible set) | `for (tries = 0; tries <= ns; ...)` |
| `run_connected_eligible` | a connection is only made to a network that was eligible | `nmbn__pass` |
| `run_first_try` | if any candidate is eligible, `best` returns one, and if connecting works it is connected | `nmbn__pass` |

Left out: **the C does not use signal strength at all** (ties after priority and last_ok go to the first
slot); `le3` is the requested ranking, `le2` is what the C does. `best` is written as a right-recursive
function, the C as a left scan with strict `>`; they agree on ties (earlier wins) by inspection, not by proof.
The loop is modelled with frozen time (the C re-reads the clock; termination there is by the `tries` bound
anyway). Not modelled: the "keep the current network unless a strictly higher priority exists" rule, the
captive-portal probe branch, interface/carrier handling, `fails` overflow (an `int` in C, unbounded Nat here),
and that `fails` resets only on success.

## Netman/KillSwitch.lean (model of `nmbv_killswitch`)

The ruleset is the output chain: loopback, tunnel device, one accept per endpoint (address, protocol, port),
DHCP (68 to 67, 546 to 547), a DNS/DoT drop, optionally LAN accepts, policy drop. First match wins.

| Theorem | Proves | C |
|---|---|---|
| `accepted_cases` | an accepted packet is loopback, via the tunnel, to an endpoint (addr+proto+port), DHCP, or (only with LAN on) a LAN address | emitted ruleset |
| `leak_dropped` | contrapositive: anything else is dropped | emitted ruleset |
| `tunnel_accepted` | non-vacuity: tunnel traffic is accepted | emitted ruleset |
| `arm_preserves_armed`, `armed_safe`, `replace_safe` | replace is ONE step (`table X; delete table X; table X {..}` in one `nft -f -` transaction): from an armed state the next state is armed again, never "no table", and any packet accepted in old or new state satisfies the property for that ruleset | `nmbv__nft`, `nmbv_killswitch` |
| `nonatomic_leaks` | why atomicity matters: with no table every packet passes | n/a |

So "neither to the endpoint nor via the tunnel is accepted" holds with the listed exceptions, which are real
and by design: loopback, DHCP, and LAN when `NMBV_KS_LAN=1`. DNS to port 53/853 is dropped unless it goes via
the tunnel or to an endpoint.

Left out: the ruleset TEXT (`compile` is a hand translation, nft parsing is not modelled); that nft really applies
the transaction atomically (assumed, it is documented nft behaviour); endpoint resolution and `tun*` globbing
(abstract predicate `isTun`); IPv4/IPv6 are folded into one address space; only the output chain exists (no
input/forward; the property is about packets this machine sends); the state when the switch is off (`none`,
everything passes, by definition); the C refusing to arm with zero endpoints is modelled in `arm`.
