# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Chiefrim lets you play Skyrim as Master Chief. Halo: Combat Evolved (OpenCE, the Halo CE Xbox decompilation) runs as a hidden native Linux i386 process alongside Skyrim SE/AE 1.6.1170 under Proton. A cross-compiled SKSE plugin talks to it over one shared-memory file. `Chiefrim/docs/DESIGN.md` is the authoritative description of how everything works now. Read the relevant section before changing a subsystem.

## The core rule

**Neither game is rewritten; Chiefrim only translates.** Halo owns Chief's shields and health, weapons, projectiles, damage, HUD and first-person arms. Skyrim owns the world, NPCs and their AI, the player's movement, quests, menus, inventory and saves. Re-implementing a Halo mechanic in the plugin, or a Skyrim mechanic in Halo, means the design has gone wrong (DESIGN.md §1.1).

## Commands

Run from `Chiefrim/`:

| What | Command |
|---|---|
| Build Halo with the hooks (resets `halo/.work` to the pin, applies the patch) | `tools/setup_halo.py` (`--debug` for assertions) |
| Rebuild Halo from the current `halo/.work` without resetting it | `tools/setup_halo.py --keep` |
| Write hook edits in `halo/.work` back to the patch | `tools/save_halo_patch.sh` |
| Build the SKSE plugin → `build/skse/Chiefrim.dll` | `tools/setup_skse.sh` |
| Package the plugin and `Chiefrim.ini` → `build/dist/ChiefrimPlugin.zip` | `tools/package_skse.sh` |
| Protocol layout test (i386, x86-64, both Windows targets) | `tools/test_protocol.sh` |
| Collision BSP builder test (needs a built `halo/.work`) | `tools/test_bsp.sh` (`SELF_TEST=1` for Halo's own queries) |
| License check (also runs before every build) | `tools/check_licenses.py` |
| Offline run: Halo in headless gamescope against a fake Skyrim | `tools/run_phase0.sh [seconds] [--visible]` |
| Fake Skyrim alone, scripted scenarios | `tools/fake_skyrim.py --help` (e.g. `--overlay WxH --grab DIR` saves frames as PNGs) |
| Run Halo for a real session (supervised, restarts on crash) | `tools/launch_halo.sh` |

The project path must not contain `[` or `]`, because CMake's `file(GLOB)` treats them as a pattern.

## Architecture

- **The link** (`protocol/chiefrim_protocol.h`): a single C header compiled by both the 32-bit Halo and the 64-bit plugin. It defines `/dev/shm/chiefrim_v1`, which the plugin opens as `Z:\dev\shm\chiefrim_v1` from inside Proton. The file holds seqlocked latest-value slots for per-frame state, two SPSC byte rings for events, and three frame slots for Halo's pixels. Both sides poll. Use fixed-width types and hand padding only; `static_assert`s pin every offset. **Any layout or meaning change requires bumping `CR_PROTOCOL_VERSION`, updating `tools/fake_skyrim.py`'s mirror of the layout (`VERSION` and the `struct.pack` formats), and updating the version in DESIGN.md's header.** The plugin refuses a Halo with a mismatched version, so Halo and the DLL must be deployed together.
- **SKSE plugin** (`skse/src/`, C++23, CommonLibSSE-NG submodule): built on Linux with clang-cl and lld-link against an MSVC CRT and SDK fetched by xwin. `Puppet.cpp` is the per-frame driver hooked into `PlayerCharacter::Update`. Each other file owns one concern (Input, Collision, Combat, Arsenal, Loot, Lighting, Overlay compositing, Camera, Handoff, CoSave, Console). Settings are read from `skse/Chiefrim.ini`, which ships next to the DLL.
- **Halo side**: OpenCE is **not forked**. `halo/UPSTREAM` pins a commit, and `setup_halo.py` builds the git-ignored work tree `halo/.work` from it:
  - `halo/patches/0001-chiefrim-hooks.patch` holds the small hooks in the game's own files. Each one is marked `/* CHIEFRIM */` and calls into `chiefrim/chiefrim.h`.
  - `halo/src/*.c` holds Chiefrim's own code, copied into `source/chiefrim/`. `halo/src/port/*` is copied into `port/linux/src/`.
  - `halo/overrides/` holds whole upstream files replaced for licensing reasons.

  Edit Chiefrim code in `halo/src/`, not in `.work/source/chiefrim/` (setup overwrites the copies). Edit hooks in `halo/.work`, then run `save_halo_patch.sh`. Chiefrim mode only runs when `CHIEFRIM=1`; without it, every `chiefrim_*` function does nothing.
- **A frame**: Skyrim moves the player and publishes position, input, nearby actors, hit shapes and streamed collision. Halo places and aims Chief and runs its own game logic. When Skyrim starts rendering, it publishes its camera. Halo draws through that camera into two transparent layers (world, then screen: arms, weapon and HUD) and publishes them with depth. The plugin composites them onto Skyrim's picture at Present (DESIGN.md §1.3, §12).
- **Coordinates and units**: Skyrim units vs Halo world units, with the world origin re-centred per world context (DESIGN.md §4). FOVs crossing the link use Skyrim's convention: horizontal, at 4:3.
- **The Arsenal** (`tools/arsenal/`): builds Chief's Mjolnir armor and Halo's weapons and grenades as Skyrim items (an esp, meshes and textures) from the user's own Halo map. The plugin needs `ChiefrimArsenal.esp`, because Halo's weapons are items in it.

## Constraints

- **No game data in the repo.** `HaloProjects/`, `References/`, `Chiefrim/build/`, `Chiefrim/halo/.work/` and `Chiefrim/.tools/` are git-ignored. Anything built from game assets stays in `build/` and is never redistributed.
- **GPL-3.0-or-later** (required by CommonLibSSE-NG). Every third-party component must be in `Chiefrim/licenses.toml` under a compatible license. See `docs/LICENSING.md`.
- **No machine's paths in the code.** The user's game files (Halo's maps, Skyrim's Data) are found by `tools/game_paths.py` (`HALO_MAPS`, `SKYRIM_DATA`, then the usual places); a script needing one asks it. `docs/INSTALLATION-LINUX.md` is the from-nothing guide for someone else's Linux machine (a Windows guide would be its own file): update it when a build step, requirement or install step changes.
- **DESIGN.md describes the current state only.** Update it in place when behavior changes; don't add history. `docs/archive/` is frozen.
- **Git workflow**: work happens on a feature branch, which is merged into `main` with a merge commit (`Merge <branch>: <summary>`). Commit subjects take the form `Area: what changed`. Bodies explain the cause and say how the change was checked (offline against the fake Skyrim, and/or in game).
