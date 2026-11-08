# Wayland protocols

Audit of every protocol the compositor (`desktop dwl`) advertises and the clients (`menu`, `dmenu`, `lemonbar`, `settings`) use.
"Stable" means in `wayland-protocols/stable` or `staging` (no `unstable` in the name). "Replacement" is judged against the
vendored wlroots 0.20 (`src/resources/wlroots`). The advertised list is checked by `tests/protocols`.

## Compositor (server side)

| Protocol (wl global) | Stable? | Used where | Stable/staging replacement in vendored wlroots | Done? |
|---|---|---|---|---|
| xdg-shell (`xdg_wm_base`) | stable | every window | - | yes |
| wl_compositor, subcompositor, shm, data_device, seat, output | core | everything | - | yes |
| viewporter, presentation-time | stable | scaling, frame timing | - | yes |
| tablet-v2 (`zwp_tablet_manager_v2`) | stable (promoted; the `zwp_` name is only history) | pens | wlroots already uses `stable/tablet` | yes |
| linux-dmabuf-v1 | stable | GPU buffers (not on the pixman test renderer) | - | yes |
| ext-session-lock-v1 | staging | lock screens | - | yes (was never the unstable form) |
| ext-foreign-toplevel-list-v1 | staging | window lists | - | yes |
| ext-idle-notify-v1 | staging | idle daemons | - | yes |
| ext-data-control-v1 | staging | clipboard managers | replaces wlr-data-control | yes, wlr twin kept behind `legacy_protocols` |
| ext-image-copy-capture-v1, ext-image-capture-source-v1 | staging | screenshots, screen share | replaces wlr-screencopy and wlr-export-dmabuf | yes, wlr twins kept behind `legacy_protocols` |
| cursor-shape-v1, fractional-scale-v1, xdg-activation-v1, tearing-control-v1, alpha-modifier-v1, single-pixel-buffer-v1, linux-drm-syncobj-v1, xwayland-shell-v1 | staging | as named | - | yes |
| wlr-screencopy-unstable-v1 | no | `grim` and others before ext-image-copy-capture | ext-image-copy-capture-v1 (already advertised) | kept as fallback, `legacy_protocols = 0` removes it |
| wlr-export-dmabuf-unstable-v1 | no | old screen recorders | ext-image-copy-capture-v1 | same |
| wlr-data-control-unstable-v1 | no | older wl-clipboard / cliphist | ext-data-control-v1 | same |
| KDE server-decoration | legacy | old Qt/GTK clients asking for decorations | xdg-decoration (below) | same |
| xdg-decoration-unstable-v1 | no | server-side decorations | none: still unstable upstream | kept |
| xdg-output-unstable-v1 | no | output names/positions for clients | none in wlroots 0.20 (core `wl_output` v4 only has name/description, not the layout position) | kept |
| zwp-idle-inhibit-unstable-v1 | no | video players | none in wayland-protocols | kept |
| pointer-constraints-unstable-v1, relative-pointer-unstable-v1 | no | games | none | kept |
| primary-selection-unstable-v1 | no | middle-click paste | none | kept |
| text-input-unstable-v3 | no | on-screen keyboards, IME | none | kept |
| input-method-unstable-v2 | no | IME | none | kept (honest: no stable equivalent) |
| virtual-keyboard-unstable-v1, wlr-virtual-pointer-unstable-v1 | no | wtype, remote input | none | kept |
| wlr-layer-shell-unstable-v1 | no | bars, menus, the settings panel | none (ext-layer-shell does not exist yet) | kept (honest: no stable equivalent) |
| wlr-output-management-unstable-v1 | no | kanshi, wlr-randr | none | kept |
| wlr-output-power-management-unstable-v1, wlr-gamma-control-unstable-v1 | no | DPMS, night light | none | kept |

## Clients (menu, dmenu, lemonbar, settings panel)

| Protocol | Stable? | Used where | Replacement | Done? |
|---|---|---|---|---|
| xdg-shell | stable | listed in `Makefile` CLIENT_PROTO, bindings generated | - | yes |
| wlr-layer-shell-unstable-v1 | no | all four surfaces are layer-shell overlays/bars | none | kept |
| wl_shm, wl_seat, wl_pointer, wl_keyboard | core | drawing and input | - | yes |

## What changed in this pass

Almost everything the compositor offers was already the stable/staging form (the session lock, the foreign toplevel list, idle notify,
data control, image copy capture, cursor shape, fractional scale and activation were created from the ext/staging objects).
The one real change: the wlr-* twins that have a stable successor are now optional.

    legacy_protocols = 0     # settings.conf, or the panel; needs a compositor restart

turns off wlr-screencopy, wlr-export-dmabuf, wlr-data-control and KDE server-decoration. Default is on, because grim,
wl-clipboard and friends in many distros still speak only the old forms; flip it once your tools use the ext ones.
`tests/protocols` starts the compositor both ways and checks the lists.

Not changed, deliberately: nothing that is still the only option (layer-shell, input-method v2, output management, output power,
gamma, virtual keyboard/pointer, pointer constraints, relative pointer, text-input, idle-inhibit, primary selection, xdg-output,
xdg-decoration). Replacing them would remove features, not modernise them.
