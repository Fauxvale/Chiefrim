# Installing Chiefrim on Linux

This guide takes you from nothing to playing Skyrim as Master Chief. It assumes no knowledge of
how Chiefrim works. For that, see [DESIGN.md](DESIGN.md).

Chiefrim is built from source on your own computer, and that's the only way to get it: it needs
your own copies of both games, and nothing made from them may be shared.

## What you're setting up

Chiefrim is three pieces that you build, plus two common Skyrim mods:

| Piece | What it is | Built by |
|---|---|---|
| **Halo** | Halo: Combat Evolved (OpenCE, the Xbox decompilation, plus Chiefrim's hooks), a native Linux program that runs hidden beside Skyrim | `tools/setup_halo.py` |
| **The plugin** | `Chiefrim.dll`, an SKSE plugin that links Skyrim to Halo | `tools/setup_skse.sh` |
| **The Arsenal** | `ChiefrimArsenal.esp` with its meshes and textures: Chief's armor and Halo's weapons as Skyrim items, converted from your own copies of the games | `tools/arsenal/build.sh` |
| SKSE64 and Address Library | The Skyrim Script Extender and its address database, which every SKSE plugin needs | Downloaded |

While you play, Halo and Skyrim run side by side and talk through a shared-memory file. Neither
needs to know where the other is installed.

## Requirements

> **Tested setup.** Chiefrim was developed and tested exclusively on Skyrim **1.6.1170** with
> **SKSE 2.2.6**, on a vanilla-adjacent load order of about 300 mods, under Proton Experimental
> on CachyOS. No other versions or setups have been tested. They may work, but there are no
> guarantees.

**Computer**

- Linux on x86-64. Chiefrim has been built and played on CachyOS (Arch-based). Other
  distributions should work but haven't been tried; the package names below are a guide.
- A GPU and driver that run Skyrim under Proton, with 32-bit OpenGL 4.5 for Halo (Mesa, or
  NVIDIA with its 32-bit libraries).

**Games**

- **The Elder Scrolls V: Skyrim Special Edition or Anniversary Edition on Steam**, version
  **1.6.1170** (the current Steam version), run with **Proton**. It's the only version tested,
  with Proton Experimental.
- **An Xbox disc image of Halo: Combat Evolved** (`.iso` or `.xiso`), any region or version,
  made from a disc you own. Halo's maps are extracted from it with the upstream OpenCE release
  (step 4).

**Path**

- The folder you put Chiefrim in must not have `[`, `]`, `*` or `?` anywhere in its path. CMake
  can't build there.

## 1. Install the build tools

Chiefrim builds three ways: Halo as a 32-bit Linux program, the plugin as a 64-bit Windows DLL
(cross-compiled with LLVM, no Windows needed), and the Arsenal with Python.

**Arch Linux, CachyOS, Manjaro** (with the `multilib` repository enabled):

```
sudo pacman -S --needed git curl python python-numpy ninja clang lld llvm \
    lib32-glibc lib32-sdl3 lib32-mesa lib32-libx11 lib32-libxext lib32-pipewire
```

**Debian and Ubuntu** (untested; needs a release with a 32-bit SDL 3, such as Debian 13 or Ubuntu 25.04):

```
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install git curl python3 python3-venv python3-numpy ninja-build clang lld llvm \
    gcc-multilib libc6-dev-i386 libsdl3-dev:i386
```

Halo also needs the 32-bit OpenGL, X11 and PipeWire (or PulseAudio) client libraries to run.
A system that plays 32-bit Steam games usually has them already.

Then check what you have:

- `python3 --version` must be 3.11 or later.
- `clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc` and `llvm-mt` must all run. Some distributions
  only install them with a version on the end (for example `clang-cl-19`). If so, link each to
  its plain name somewhere on your `PATH`, such as `~/.local/bin`.

The plugin's build also downloads two things itself, into `.tools/` inside the project: CMake
(into a private Python environment) and Microsoft's C runtime and Windows SDK (with
[xwin](https://github.com/Jake-Shadle/xwin)).

## 2. Get the code

```
git clone https://github.com/Fauxvale/Chiefrim.git Chiefrim-Project
cd Chiefrim-Project/Chiefrim
```

Every command in the rest of this guide runs from this `Chiefrim` folder, the one with `tools/`
in it.

## 3. Build Halo

```
tools/setup_halo.py
```

This downloads OpenCE at the version Chiefrim is pinned to, into `halo/.work`, adds Chiefrim's
hooks and builds it. The result is `halo/.work/build/linux/halo`. It takes a few minutes the
first time.

## 4. Extract Halo's maps

Halo needs its maps, which come from your disc image. Chiefrim's build of Halo can't read a disc
image: the code that does it isn't under a license Chiefrim can include (see
[LICENSING.md](LICENSING.md)). The upstream OpenCE release can, so you use it once, just to
extract the maps.

1. Download `halo-linux-release.zip` from the latest release on OpenCE's releases page:
   <https://github.com/OpenCommunityEdition/OpenCE/releases/latest>. Or, with the GitHub CLI:

   ```
   gh release download -R OpenCommunityEdition/OpenCE -p halo-linux-release.zip -D ~/Downloads
   ```

2. Unzip it into a folder of its own, outside `Chiefrim-Project`, and make it runnable. The zip
   doesn't keep the file's run permission:

   ```
   mkdir -p ~/Games/OpenCE
   unzip ~/Downloads/halo-linux-release.zip -d ~/Games/OpenCE
   chmod +x ~/Games/OpenCE/halo
   ```

3. Start it:

   ```
   ~/Games/OpenCE/halo
   ```

   It finds no maps and asks to extract them from an Xbox disc image. Choose **Yes**, pick your
   Halo disc image in the file picker, and wait while it copies the maps (about 2 GB) to
   `~/Games/OpenCE/maps`. The game then starts. Close it.

4. Move the maps to where Chiefrim looks for them, back in the `Chiefrim` folder:

   ```
   mv ~/Games/OpenCE/maps halo/.work/build/linux/maps
   ```

Chiefrim finds the maps there by itself, and rebuilding Halo later keeps them. You can delete
`~/Games/OpenCE` afterwards. To keep playing upstream OpenCE as well, copy the maps with `cp -r`
instead of moving them.

Already have Halo's `maps/` folder extracted somewhere else? Skip this step, and set `HALO_MAPS`
to that folder each time you run a Chiefrim command:

```
export HALO_MAPS=/path/to/your/maps
```

To check where Chiefrim will look, run `tools/game_paths.py maps`.

## 5. Build the plugin

The plugin is compiled against Microsoft's C runtime and Windows SDK, which the build downloads.
Downloading them means accepting
Microsoft's license terms for them, so the first build only does it when you say so:

```
tools/setup_skse.sh --accept-license
tools/package_skse.sh
```

Later builds don't need `--accept-license`. The result is `build/dist/ChiefrimPlugin.zip`: the
plugin, its settings file `Chiefrim.ini`, and its license.

## 6. Build the Arsenal

The Arsenal converts Master Chief's armor and Halo's weapons from your Halo maps into Skyrim
items, using your Skyrim's own files as templates. It needs both games found:

```
tools/game_paths.py map
tools/game_paths.py skyrim
```

The first should print a `d20.map` that exists (Halo's Keyes level, the one Chiefrim uses). The
second should print your Skyrim's `Data` folder. Chiefrim looks for Skyrim in all your Steam
libraries, including a Flatpak Steam's. If it's somewhere else, set `SKYRIM_DATA`:

```
export SKYRIM_DATA="/path/to/Skyrim Special Edition/Data"
```

Then build:

```
tools/arsenal/build.sh
```

The result is `build/dist/ChiefrimArsenal.zip`. It contains converted game assets, so it's for
your own game only: don't share it.

## 7. Install SKSE and Address Library

If you already play Skyrim with SKSE plugins, you have these; check the versions and skip ahead.

1. **SKSE64**: from [skse.silverlock.org](https://skse.silverlock.org), **SKSE 2.2.6**, the
   build for game version 1.6.1170 and the only one tested. Copy `skse64_loader.exe` and the `skse64_1_6_1170.dll` into
   Skyrim's folder (the one with `SkyrimSE.exe`), and the archive's `Data` folder into Skyrim's.
2. **Make Steam start Skyrim through SKSE.** Under Proton, the usual way is to stand the loader
   in for Skyrim's launcher. In Skyrim's folder, rename `SkyrimSELauncher.exe` to
   `SkyrimSELauncher.exe.bak`, and copy `skse64_loader.exe` to `SkyrimSELauncher.exe`.
3. **Address Library for SKSE Plugins**: from
   [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/32444), the
   "All in one (Anniversary Edition)" file. Its `SKSE/Plugins` files go into Skyrim's
   `Data/SKSE/Plugins`.

## 8. Install Chiefrim's files

Install both zips from `build/dist/`, `ChiefrimPlugin.zip` and `ChiefrimArsenal.zip`, as mods.

**With a mod manager** (Mod Organizer 2, Vortex or similar): install each zip as a mod and
enable both, including the plugin `ChiefrimArsenal.esp`.

**By hand**:

1. Unzip both into Skyrim's `Data` folder. You should then have
   `Data/SKSE/Plugins/Chiefrim.dll`, `Data/SKSE/Plugins/Chiefrim.ini` and
   `Data/ChiefrimArsenal.esp`, with `meshes` and `textures` folders beside it.
2. Enable `ChiefrimArsenal.esp`. Without a mod manager, add this line to Skyrim's
   `Plugins.txt`:

   ```
   *ChiefrimArsenal.esp
   ```

   Proton keeps that file in Skyrim's Wine prefix, inside the Steam library that holds
   Skyrim:
   `steamapps/compatdata/489830/pfx/drive_c/users/steamuser/AppData/Local/Skyrim Special Edition/Plugins.txt`.
   Create it if it isn't there.

The plugin and the esp are both needed: the plugin finds Halo's weapons as items in the esp.

## 9. Start Halo with Skyrim

`tools/launch_halo.sh` runs Halo while you play. It keeps Halo hidden, restarts it if it crashes,
and lets Skyrim's hotkeys stop and restart it.

**Let Steam start it** (recommended). In Steam, open Skyrim's **Properties**, then **General**,
and set **Launch Options** to the script's full path followed by `--steam %command%`. For example,
if you cloned to `/home/you/Games/Chiefrim-Project`:

```
"/home/you/Games/Chiefrim-Project/Chiefrim/tools/launch_halo.sh" --steam %command%
```

Halo then starts with Skyrim and stops when Skyrim exits. Also under **Properties**, in
**Compatibility**, force **Proton Experimental**.

This is tested with Steam installed from your distribution. With the Flatpak Steam, the launch
options run inside Flatpak's sandbox, which may not have Halo's 32-bit libraries; if Halo doesn't
start that way, start it yourself instead.

**Or start it yourself**, before or after Skyrim (for example when a mod manager launches the
game):

```
tools/launch_halo.sh
```

Leave it running while you play; Ctrl+C stops it. Skyrim links to Halo whenever both are up.

## 10. Play

Start Skyrim from Steam (or your mod manager) and load a save. Once linked, you're Master Chief:
you see his arms, his weapon and Halo's HUD in first person, and Skyrim's NPCs take Halo's
weapons' damage.

| Key | Does |
|---|---|
| Your usual Skyrim keys | Move, look, jump, sprint, sneak (Chief crouches) |
| Left mouse | Fire |
| Right mouse | Zoom |
| R (Ready Weapon) | Reload |
| Z (Shout) | Throw a grenade |
| F (Toggle POV) | Melee |
| Mouse wheel | Switch weapon |
| E (Activate) | Pick up or swap a weapon; still Skyrim's Activate too |
| G | Switch grenade type |
| V | Flashlight |
| F10 | Chiefrim off and on: off gives you Skyrim's own character back |
| F11 | Restart Halo, if it's stuck |

Chief's actions follow Skyrim's own controls, so rebinding an action in Skyrim's Controls menu
rebinds Chief's too. Everything else, including the hotkeys, the field of view and how Halo's
picture is matched to Skyrim's, is in `Chiefrim.ini` next to the DLL; each setting is explained
there.

Halo's weapons and grenades turn up as Skyrim items: in chests, at blacksmiths and general
stores, and dropped by enemies you kill. Equip one to take it into Chief's hands. Chief's armor
isn't placed in the world; add it with Skyrim's console (`help "MJOLNIR" 4` lists the pieces'
IDs, then `player.additem <ID> 1`).

## If something goes wrong

**Nothing of Halo shows in game.**
- Is Halo running? With Steam's launch options, its output is in `build/halo-data/halo.out`.
  Started by hand, its terminal shows it.
- Did the plugin load? SKSE writes `Chiefrim.log` in Skyrim's Wine prefix, in
  `drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/`. If it isn't there,
  SKSE isn't starting (step 7) or the DLL isn't in `Data/SKSE/Plugins`.
- After updating, rebuild and install **both** Halo and the plugin. The plugin refuses a Halo
  built from a different version of Chiefrim.

**A build says it can't find Halo's maps or Skyrim.** Run `tools/game_paths.py maps` and
`tools/game_paths.py skyrim` to see where it looked, then extract the maps (step 4) or set
`HALO_MAPS` or `SKYRIM_DATA`.

**Halo is slow, or picks the wrong GPU, on a laptop with two.** Set `CHIEFRIM_HALO_GPU` to
`igpu` or `dgpu` when you run `launch_halo.sh`. `halo.out`'s `OpenGL` line names the GPU it got.

**Halo crashes or freezes.** Press F11, or let `launch_halo.sh` restart it. Halo's own log is
`build/halo-data/debug.txt`.

Known problems and limits are listed in [DESIGN.md §18](DESIGN.md#18-known-issues-and-limits).

## Updating

```
git pull
tools/setup_halo.py
tools/setup_skse.sh
tools/package_skse.sh
tools/arsenal/build.sh
```

Then reinstall `ChiefrimPlugin.zip` and `ChiefrimArsenal.zip` (step 8). Your Halo maps and the
downloaded tools are kept.

## Uninstalling

- Remove Chiefrim's files from `Data` (or the two mods from your mod manager), and the
  `ChiefrimArsenal.esp` line from `Plugins.txt`.
- Clear Skyrim's launch options in Steam.
- Delete the `Chiefrim-Project` folder. That also removes Halo's maps, the build and the
  downloaded tools.
- A save made with Chiefrim keeps Chief's kit in SKSE's co-save. Without the plugin, Skyrim
  ignores it.

## Developer tools (optional)

These aren't needed to play:

| Command | Does |
|---|---|
| `tools/run_phase0.sh` | Runs Halo against a fake Skyrim, without the game (needs `gamescope` unless `--visible`) |
| `tools/test_protocol.sh` | Checks the shared file's layout for both programs |
| `tools/test_bsp.sh` | Tests the collision builder |
| `tools/list_map_tags.py --matrix MAP.map...` | Lists which weapons and other tags each Halo map holds |

DESIGN.md §3 and §14 describe the build and the tools in full.
