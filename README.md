[README.md](https://github.com/user-attachments/files/33414160/README.md)
# ROTK Clean

Internal H1Z1/Z1BR cheat - ESP, Aimbot, NoRecoil, Friend List, multiple configs.

The "clean" build: same core as the main ROTK build, with the Exploits tab (SpeedHack,
NoClip, Super Jump, Parachute Fastland, Magic Bullet, Teleport, Teleport to Vehicle) and
Chams removed entirely, not just disabled.

- **Type:** internal DLL (`rotk_clean.dll`), loaded into `H1Z1.exe`
- **Method:** hooks the game's own D3D11 `Present` call - ESP and the ImGui menu render
  directly into the game's pipeline, no second overlay window
- **Menu:** `INSERT` to open/close

## Features

### Combat
- **Aimbot** - predictive (leads moving targets off their velocity), real-physics bullet drop
  past 100 m, stance-aware head height (crouch/prone)
  - Hold-to-activate key (default `MOUSE2`) and a separate toggle key (default `F1`), both rebindable
  - FOV slider (5-150 px) with optional on-screen FOV circle
  - Smoothness slider (1-15)
  - Per-weapon bullet speed, auto-detected from the held weapon
  - **Weapon Profiles** - per-weapon FOV/Smoothness (AR-15, AK-47, 308 Hunting Rifle,
    Riot Shotgun, M9, 1911, Magnum). Tick "Weapon Profiles" to enable, "Configure Weapons"
    to edit. While enabled, the bottom-left `PROFILE:` text shows which profile is applied.
- **NoRecoil** - reversible 5-byte patch on the aim-punch call, own toggle key (default `F2`)

### ESP
- **Players** - boxes (custom color), names, health bars, distance, head dot, skeleton
- **Loot** - Kevlars, lootbags, HE grenades, ground weapons - each toggled and colored separately
- **Vehicles** - OffRoader / Truck / Police Car / ATV, each with its own color, plus distance

### Friends
- Aim at a player and press the Add Friend key (default `F6`) to exclude them from Aimbot by
  name. Remove with the X next to their name. Friends are not hidden from ESP.

### Settings
- **SafeMode** - turns NoRecoil off and keeps it locked
- **Configs** - save the active config, "Save As New Config" under a name, click a name in the
  list to load it, X to delete. The last-used config loads automatically on next inject.
- **Unload** - unhooks everything and frees the DLL without restarting the game

## Files it writes

All under `Documents\Stan\`:

| File | Contents |
|---|---|
| `Configs\<name>.ini` | one file per saved config |
| `lastconfig.txt` | name of the config to load on next inject |
| `friends.txt` | friend list, one name per line (shared across all configs) |

An old single `stan.ini` from before named configs is copied to `Configs\default.ini` once,
the first time this build runs. No diagnostic log is written - `Log()` is a no-op in this build.

## Building

- Visual Studio 2022 (v143 toolset) and the June 2010 DirectX SDK
- Open `rotk_clean.sln`, build **Release | x64**
- Output: `x64\Release\rotk_clean.dll`

If the link step fails with `LNK1104: cannot open file ...rotk_clean.dll`, the DLL is still
loaded in a running `H1Z1.exe` - close the game and build again.

## Offsets

All offsets live in `Offs.h`. The game manager is reached through the 2026-10-08 offset dump's
chain: `*(*(base + 0x4742DB8) + 0x382A0)`, falling back to the legacy slot if either hop is
null. The held-weapon chain (`entity + 0x688 -> + 0xB8 -> + 0x20`, a C string) was confirmed
against live captures and drives both bullet speed and Weapon Profiles. If a game update moves
`cgame`, ESP and Aimbot go empty first - update `RVA_CGAME_DUMP` in `Offs.h`.
