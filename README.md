# d2dc-gamepad-mod

Gamepad mod for **Divinity II: Developer's Cut** (Steam): pad control on
top of the base Div2Patch v0.97 — stick movement, right-stick cursor,
trigger clicks, vibration on damage and on load/area-transition start.

Русская версия: [README.ru.md](README.ru.md)

## Install & build

Full guide — [docs/BUILD-AND-INSTALL.md](docs/BUILD-AND-INSTALL.md)
(English; Russian version:
[docs/BUILD-AND-INSTALL.ru.md](docs/BUILD-AND-INSTALL.ru.md)).

- **Prebuilt**: `release/Div2PatchGamepad-vibdamage-load/` —
  `Div2Patch.dll` + `Div2PatchGamepad.ini` + README. Copy into
  `<game>\bin\` and launch the game in normal mode from the Anthology
  launcher.
- **From source**: `gamepad_mod/build.cmd` (WSL + mingw):

        i686-w64-mingw32-g++ -shared -O2 -static -static-libgcc \
            -o Div2Patch.dll div2gamepad.c div2gamepad.def -lwinmm

- **Requirement**: the base Div2Patch v0.97 must already be installed
  and renamed to `Div2PatchBase.dll` / `Div2PatchBase.ini` — our
  `Div2Patch.dll` forwards `InitPatch` to it and adds its own hooks.

## Features

- Left stick moves; right stick is the cursor; RT/LT are LMB/RMB
  (attack/context and menu selection); Start is ESC; the D-pad switches
  quickbar slots (Quickmap 5-8).
- Vibration (our own; the engine's PC rumble controller is dead): on
  taking damage and at world-load / area-transition start.
- Fixes: `PadLoadFix` (black screen on save load), `StickyFix` (hero
  stickiness), `CamHold` (camera recenters after pad presses).
- Quiet log: `Diag=0` — events only.

## Layout

| Path | Purpose |
|---|---|
| `gamepad_mod/` | Mod source `div2gamepad.c`, `div2gamepad.def`, `build.cmd`, default `Div2PatchGamepad.ini` |
| `docs/` | Build & install guide |
| `release/Div2PatchGamepad-vibdamage-load/` | Verified ready-to-install build + README |

## How it works

`Div2Patch.dll` is a drop-in replacement of the base patch: it
chainloads the real payload as `Div2PatchBase.dll` and adds its own
hooks. Engine anchors: `NiInputDI8GamePad` (DirectInput backend), input
event pump @ `0x8EBB34`, action dispatcher @ `0x8E7710`; the exe loads
rebased, hooks compute the delta at runtime. Vibration uses
`XInputSetState`.
