# Div2Gamepad: Build & Install

Gamepad mod for **Divinity II: Developer's Cut** (Steam). Our
`Div2Patch.dll` is a drop-in replacement of the base patch: it forwards
`InitPatch` to `Div2PatchBase.dll` (the renamed Div2Patch v0.97 +
Enhancer payload) and adds its own gamepad/vibration hooks.

Русская версия: [BUILD-AND-INSTALL.ru.md](BUILD-AND-INSTALL.ru.md)

Third-party components (Div2Patch v0.97, Div2Patch Enhancer) are
**not** part of this repository — they are separate builds under their
authors' licenses:
- Div2Patch v0.97 — GitHub, by BlanknameES.
- Div2Patch Enhancer v1.0.0 (payload) — Nexus Mods, by Gametism.

---

## Part 1. Installing from binaries (for players)

### Prerequisites

1. Steam version of **Divinity II: Developer's Cut**, installed and
   launched at least once.
2. The base **Div2Patch v0.97** already installed (+ Enhancer payload
   if you want the large font). Our mod is an add-on: without the base
   it will not load. Installing the base: put the v0.97 `Div2Patch.dll`
   into `<game>\bin\` and, optionally, deploy the Enhancer payload.

### Steps

1. Close the game and the Anthology launcher.
2. Go to `<game>\bin\` (e.g.
   `E:\SteamLibrary\steamapps\common\divinity2_dev_cut\bin\`).
3. Make a backup: copy the current `Div2Patch.dll` to
   `Div2Patch.dll.backup` (or to a separate folder).
4. **Rename the base patch** (once):
   - `Div2Patch.dll` → `Div2PatchBase.dll`
   - `Div2Patch.ini` → `Div2PatchBase.ini`
   The base reads `<base name>.ini`, i.e. `Div2PatchBase.ini` — do not
   skip this rename.
5. Copy from the release folder
   (`release/Div2PatchGamepad-vibdamage-load/`) into `bin\`:
   - `Div2Patch.dll`
   - `Div2PatchGamepad.ini`
6. Launch: Steam → **Divinity Anthology** → the button
   **"Play Divinity II — normal mode"** (the first of the two "Play
   Divinity II" buttons in the launcher).
   ⚠️ **NOT "developer mode"** — that launches `Divinity2-debug.exe`,
   a different binary the mod is not built for (VEH crash on startup).

### Verifying the install

- The menu shows the Enhancer/Div2Patch watermark and the large font
  (if the base + payload are installed).
- `bin\div2gamepad.log` contains mod lines: `DllMain attach`,
  `xinput: pad CONNECTED`, `vibd hook installed`; on a load —
  `vibload: start pulse (...)`.
- Gamepad: left stick moves the hero, right stick is the cursor, RT is
  attack/click, Start is ESC. Vibration: taking a hit triggers a pulse.

### Current build

| File | sha256 |
|---|---|
| `Div2Patch.dll` | `8f9e41818d125478f88ed4c872b2fb76dc71ea5618d12727832f2acb2343fdd8` |
| `Div2PatchGamepad.ini` | `d2706f99e4f8818ec37c9bcf55ae9154981c8a2bb4cdf4aee1f0a132feed02c3` |

The repository ships the current verified build only:
`release/Div2PatchGamepad-vibdamage-load/`.

### Rollback

1. Close the game.
2. Restore `Div2Patch.dll` / `Div2PatchGamepad.ini` from the backup.
3. `Div2PatchBase.dll` / `Div2PatchBase.ini` may stay as they are —
   the base is self-contained; or rename them back to `Div2Patch.*`.

### Configuration (`Div2PatchGamepad.ini`)

| Key | Meaning |
|---|---|
| `Vibration=1`, `VibStrength=60` | vibration master switch, strength in % |
| `VibOnDamage=1`, `VibDamageStrength=0` | vibrate on damage (0 = scale from VibStrength) |
| `VibLoad=1`, `VibLoadStart=0` | vibrate at load/transition start |
| `Diag=0` | 1 = periodic diagnostic dumps (STATE/ACT/wd/…); keep 0 for play |
| `Trace=0` | 1 = per-event input trace (very expensive) |
| `PadLoadFix=1` | required: PC loading screen (otherwise black screen) |
| `PadKeepOnLoad=0`, `PadForceFlag=0`, `PadBootMode=0` | keep off (black screens / crashes) |
| `RStickCam=1`, `CamSpeed`, `CamWorldSpeed` | right-stick cursor and speeds |
| `NativeMove=1`, `StickVirt=1` | native stick movement |

The game reads the ini at startup — restart after editing.

### Troubleshooting

- **No vibration**: check `vibd hook installed` in the log,
  `Vibration=1`, `VibOnDamage=1`; the in-game "Rumble" slider also
  scales the strength.
- **No gamepad**: the log must show `xinput: pad CONNECTED`; the pad
  must be in slot 0 (`UserIndex=0`). Virtual pads (ViGEm) occupy the
  slot — close them.
- **Black screen after loading a save**: make sure `PadLoadFix=1`,
  `PadKeepOnLoad=0`, `PadForceFlag=0`.
- **Crash on startup**: the game was launched in "developer mode" —
  use normal mode.
- Log: `bin\div2gamepad.log`. Start with `Diag=0` (events only); for
  deep debugging set `Diag=1` and `Trace=1`.

---

## Part 2. Building from source (for developers)

This is a 32-bit DLL cross-build. Tested path: Windows + WSL2 (Ubuntu).
It also works on Linux directly (without the WSL wrapper).

### Dependencies (once)

```bash
sudo apt update
sudo apt install -y g++-mingw-w64-i686
```

Check: `i686-w64-mingw32-g++ --version` must respond.

### Build

From the `gamepad_mod/` folder:

```
build.cmd
```

The script converts the path via `wslpath` and prints the resulting
sha256. Manual equivalent (Linux/WSL, from `gamepad_mod/`):

```bash
i686-w64-mingw32-g++ -shared -O2 -static -static-libgcc \
    -o Div2Patch.dll div2gamepad.c div2gamepad.def -lwinmm
```

Why these flags:
- `-static-libgcc` — **mandatory**: without it the DLL depends on
  `libgcc_s_dw2-1.dll` and the game silently fails to load the mod.
- `-lwinmm` — timers/`timeSetEvent` used by the mod.
- `-static` — no extra CRT DLL dependencies.
- The build hash may differ from the table above: PE timestamps
  (linker non-determinism, ~7.5 KB difference) — this is normal, the
  code is identical. Trust "BUILD OK" and actual behavior, not the
  sha256 match.

### Deploying your build for testing

1. Close the game.
2. Copy `Div2Patch.dll` and `Div2PatchGamepad.ini` into `<game>\bin\`
   (see Part 1, steps 3–6; `Div2PatchBase.*` must already be there).
3. Launch — normal mode in the Anthology launcher.

### Repository map

| Path | Purpose |
|---|---|
| `gamepad_mod/div2gamepad.c` | the whole mod (~10.6k lines) |
| `gamepad_mod/div2gamepad.def` | `InitPatch` export |
| `gamepad_mod/Div2PatchGamepad.ini` | default key preset |
| `gamepad_mod/build.cmd` | one-command build |
| `release/` | ready-to-use verified build (see Part 1) |

### Debugging

- `Diag=1` in the ini — periodic dumps (`STATE`, `ACT`, `wd`, `d95`, …).
- `Trace=1` — per-event input trace (expensive; debugging only).
- `DbgCmd=1` — file console: drop `NNN.cmd` into `<game>\bin\dbgcmd\`
  (verbs: `ping`, `state`, `vibtest`, `hold/release/pulse`, `rumble`…),
  the reply is `NNN.rsp`.
- Log: `bin\div2gamepad.log`; clearing it before a session is advised.

### Testing rules

- Clear the log before a smoke session.
- Do not leave ViGEm daemons running (a virtual pad takes XInput slot 0
  and breaks controls).
- Synthetic menu clicks: the game reads the cursor as raw physical
  coordinates; with Windows scaling ≠100% a click based on a screenshot
  needs coordinate recalculation.
