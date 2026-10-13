# Chiefrim — Design

Play Skyrim as Master Chief. Halo: Combat Evolved runs alongside Skyrim and provides Chief's
shields, health, weapons, grenades, HUD and first-person arms. Skyrim provides the world, the
people, the quests and the saves. You fight Skyrim's NPCs with Halo's weapons.

This document describes how Chiefrim works **today**. It is not a changelog: for how a feature
got this way, see the git history. The previous version of this document, which mixed design and
history, is kept in [archive/DESIGN-v0.3.md](archive/DESIGN-v0.3.md).

Last revised 2026-10-13 · protocol version 34 · OpenCE pinned at `b1ef7f7e`.

---

## 1. Overview

### 1.1 The core rule

**Neither game is rewritten.** Each game keeps doing what it already does well, and Chiefrim only
translates between them.

| Halo owns | Skyrim owns |
|---|---|
| Chief's shields and health, and their recharge | The world: terrain, buildings, cells, load doors |
| Weapons, ammo, reloading, zoom, melee | NPCs, their AI, their health and deaths |
| Projectiles, grenades, explosions, bullet holes | The player's movement, jumping, sneaking and swimming (by default) |
| Damage calculation against targets | Quests, dialogue, crime, menus, inventory |
| The HUD, motion tracker and first-person arms | Saves (Chiefrim adds Chief's kit to the co-save) |

If Chiefrim ever re-implements a Halo mechanic in the SKSE plugin, or a Skyrim mechanic in Halo,
the design has gone wrong.

### 1.2 Two processes, one shared file

Skyrim runs under Proton (Windows, 64-bit). Halo runs natively on Linux (OpenCE, 32-bit), with its
window hidden. They talk through one shared-memory file, `/dev/shm/chiefrim_v1`.

```
┌─ SkyrimSE.exe (Proton, x64) ───────────┐                 ┌─ halo (native Linux, i386, hidden) ───────┐
│ Chiefrim.dll (SKSE plugin)             │                 │ OpenCE + Chiefrim hooks                   │
│                                        │  player, camera │                                           │
│  Puppet   — per-frame driver ──────────┼────────────────▶│ Chief placed where Skyrim's player is     │
│  Input    — Skyrim's controls ─────────┼────────────────▶│ Chief's fire/zoom/reload/grenade/...      │
│  Collision— Havok shapes → triangles ──┼────────────────▶│ runtime collision BSP (for shots)         │
│  Combat   — nearby actors + hit shapes─┼────────────────▶│ invisible proxy bipeds                    │
│           ◀── hits on proxies ─────────┼─────────────────│ Halo's own damage on the proxies          │
│           ─── damage to the player ────┼────────────────▶│ damage to Chief, shields first            │
│  Arsenal  — equipped Halo items ───────┼────────────────▶│ weapon/grenades handed to Chief           │
│           ◀── weapons Chief drops ─────┼─────────────────│                                           │
│  Lighting — Skyrim's light ────────────┼────────────────▶│ Halo's objects lit by it                  │
│  Overlay  ◀── two RGBA layers + depth ─┼─────────────────│ draws only Chief's arms, weapon, HUD and  │
│              composited onto Skyrim    │                 │ its own projectiles/effects               │
│  CoSave   ◀── Chief's kit ─────────────┼─────────────────│                                           │
└────────────────────────────────────────┘                 └───────────────────────────────────────────┘
            /dev/shm/chiefrim_v1: seqlocked slots + two ring buffers, both sides poll (§5)
```

A supervisor script, `tools/launch_halo.sh`, starts Halo with Skyrim and restarts it when it
crashes or hangs (§14).

### 1.3 What a frame looks like

1. Skyrim updates the player (its own character controller moves him) and the plugin's
   `PlayerCharacter::Update` hook runs Chiefrim's per-frame work (`skse/src/Puppet.cpp`): it
   publishes the player's position, view and flags, the input actions, the nearby actors and
   their hit shapes, streams collision, and reads Halo's messages.
2. Halo's tick places Chief where Skyrim's player stands, aims him along Skyrim's camera, and
   runs its own game logic: firing, projectiles, damage, shields.
3. As Skyrim starts rendering its world, the plugin publishes the camera. Halo draws its frame
   through that camera, into two transparent layers, and publishes them.
4. At Present (or under Skyrim's first menu draw), the plugin composites Halo's layers onto
   Skyrim's picture.

## 2. Status

All the planned v1 phases are built and have been played in game: link, movement, overlay,
combat, interiors and load doors, persistence, and loot.

| Area | State |
|---|---|
| Link, crash recovery, on/off and restart | Working (§5, §14) |
| Movement | Skyrim moves the player; Chief follows (§7). A Halo-driven mode exists but is off |
| Controls | Skyrim's own bindings drive Chief's actions; prompts show Skyrim's keys (§8) |
| Collision for shots and grenades | Built at runtime from Skyrim's Havok shapes (§9) |
| Combat both ways | Working, including headshots, explosions, burning, destructibles, traps (§10) |
| Halo weapons and grenades as Skyrim items | Working: equip to hand to Chief, dropped weapons come back as items (§11) |
| Loot | Chests, boss chests, merchants and enemy drops (§11.2) |
| Saves | Chief's kit is in the SKSE co-save (§11.3) |
| Overlay | Two layers, depth-tested and reprojected onto Skyrim's camera; under Skyrim's HUD (§12) |
| Lighting and grade | Skyrim's light, shadows, fog and grade applied to Halo's pixels (§12.3, §12.4) |
| Hand-off to Skyrim | Furniture, beds, mounts, swimming, beast forms, scenes, death (§13) |
| The Arsenal (Mjolnir armor and weapon items) | Built from the user's own files as a separate mod (§15) |
| Covenant enemies, vehicles | Not started (stretch goals) |

Known problems and limits are in §18.

## 3. Environment and building

| Thing | Value |
|---|---|
| Host OS | Linux (developed on CachyOS) |
| Skyrim | SE/AE **1.6.1170**, Steam, Proton Experimental, SKSE64 and Address Library |
| SKSE plugin | C++23, CommonLibSSE-NG (submodule), CMake. **Cross-compiled on Linux** to a Windows x64 DLL with clang-cl and lld-link, against the MSVC CRT and Windows SDK fetched by xwin |
| Halo | OpenCE (the Halo CE Xbox decompilation, formerly halo-ce-universal), **Linux i386 build**, OpenGL, SDL3. 32-bit because the map data contains 32-bit pointers |
| Halo data | The user's own `maps/`, extracted from their Xbox disc image. The host map is `d20` (§6.1) |
| Tested with | SSE Display Tweaks (it hooks the swap chain; Chiefrim chains with it). No ENB or shader replacer |

**No game data in the repo.** The repo holds code only. `HaloProjects/`, `References/` and
`Chiefrim/build/` are git-ignored. Anything built from game assets (the Arsenal) stays in
`build/` and must not be redistributed.

Build commands:

| What | Command | Result |
|---|---|---|
| Halo with Chiefrim's hooks | `tools/setup_halo.py` | `halo/.work/build/linux/halo` |
| SKSE plugin | `tools/setup_skse.sh` (first run: `--accept-license` for the MSVC CRT/SDK) | `build/skse/Chiefrim.dll` |
| Plugin as a mod archive | `tools/package_skse.sh` | `build/dist/ChiefrimPlugin.zip` |
| The Arsenal: Mjolnir armor and weapon items | `tools/arsenal/build.sh HALO_MAP [SKYRIM_DATA_DIR]` | `build/dist/ChiefrimArsenal.zip` |

The project path must not contain `[` or `]` (CMake's `file(GLOB)` reads them as a pattern).

The SKSE plugin and `ChiefrimArsenal.esp` are both needed: the plugin finds Halo's weapons as
items in that esp (§11). `Chiefrim.ini` goes next to the DLL in `Data/SKSE/Plugins`.

## 4. Coordinates

Both games are Z-up and right-handed, so the mapping is a scale and an offset, with no axis swap.

- **Scale:** 1 Halo world unit = 10 ft = 3.048 m. At Skyrim's 70 units per metre, **1 wu =
  213.36 Skyrim units** (`CR_SKY_UNITS_PER_WU`).
- **Origin:** Halo's (0, 0, 0) is placed where Skyrim's player stands when a "world" starts. A new
  world starts on linking, when the worldspace or interior cell changes, when a loading screen
  closes, and when the player moves more than 1024 units horizontally in one step (fast travel,
  `coc`, a script). Each new world gets a new `generation`.
- **Heading:** Skyrim's heading is clockwise from +Y (north); Halo's yaw is counter-clockwise
  from +X. `halo_yaw = π/2 − skyrim_heading`.
- **Directions** need no conversion, only points.

The conversions live in `protocol/chiefrim_protocol.h` (`cr_sky_to_halo`, `cr_halo_to_sky`).

## 5. The link

### 5.1 Transport

Windows named shared memory doesn't cross the Proton boundary, so the link is a plain file:

- Halo creates `/dev/shm/chiefrim_v1` (`shm_open` + `mmap`).
- The plugin opens the same file as `Z:\dev\shm\chiefrim_v1` (`CreateFileW` +
  `CreateFileMappingW` + `MapViewOfFile`). Wine backs it with a shared mapping of the same file.
- There are no named events. Both sides poll, once per frame or tick.

The whole layout is one C header, `protocol/chiefrim_protocol.h`, compiled by both sides. One side
is 32-bit and the other 64-bit, so the structs use fixed-width types only, with hand padding, and
`static_assert`s pin every size and offset. `tools/test_protocol.sh` checks them on both targets.
`tools/fake_skyrim.py` mirrors the layout in Python.

The header carries a magic number and `CR_PROTOCOL_VERSION`. The plugin refuses a Halo with a
different version or size (and logs both). Bump the version whenever the layout or a message's
meaning changes, and update `fake_skyrim.py`'s copy.

### 5.2 Layout

- **A header:** magic, version, size, both process IDs, both sides' state and heartbeat.
- **Slots:** latest-value structs under a seqlock (odd sequence while being written). For
  per-frame state.
- **Two rings:** single-producer, single-consumer byte rings (4 MB each), one each way. For
  events. Messages are 8-byte aligned and start with a `{type, size}` header.
- **Frame slots:** three slots of Halo's pixels, written round-robin, each under its own seqlock
  (§12.2).

| Slot | Direction | Written | Contents |
|---|---|---|---|
| `world_context` | S→H | each new world | world ID, interior flag, origin, generation, Chief's world and viewmodel FOV, height and collision radius, collision radius in regions |
| `skyrim_player` | S→H | every frame | position, heading, pitch, eye, view direction, velocity, on-ground/sneaking flags, and whether Skyrim drives |
| `input` | S→H | every frame | held actions, per-action press counters, forward/strafe, running look totals, routing |
| `actors` | S→H | every frame | up to 48 nearby actors and up to 1024 hit shapes (§10.1) |
| `display` | S→H | every Present | Skyrim's back-buffer size; whether to draw the overlay |
| `camera` | S→H | as world rendering starts | eye, forward, up, vertical FOV of the frame Skyrim is drawing |
| `player_state` | H→S | every Halo frame | Chief's position, view, pose, vertical FOV, body and shield fractions, Halo's clock |
| `frames` | H→S | every Halo frame | the screen layer, world layer, world depth and weapon mask (§12) |

| Message | Direction | When | Purpose |
|---|---|---|---|
| `HELLO` | both | on linking | protocol version, PID, build string |
| `TELEPORT` | S→H | with each new world | put Chief at the player's position and heading |
| `COLLISION_RESET`, `COLLISION_TRIS` | S→H | streamed | Skyrim's collision triangles per region (§9) |
| `PLAYER_HURT` | S→H | on damage | Skyrim damage to the player, as a fraction of Chief's vitality (§10.3) |
| `CHIEF_HEAL` | S→H | potion or food | healing for Chief's body |
| `GIVE_WEAPON` | S→H | equip, console, F7 | a weapon or grenades for Chief (§11) |
| `ARSENAL` | S→H | on linking | which weapons Skyrim has items for; enemy drop odds |
| `CHIEF_RESTORE` | S→H | on load / new Halo | Chief's saved kit, or "use the starting loadout" (§11.3) |
| `KEY_NAMES` | S→H | on linking, on change | the player's key names for Halo's prompts |
| `LIGHTING` | S→H | ~10 Hz | Skyrim's light around the player (§12.4) |
| `DEBUG` | S→H | on change | debug drawing (hit shapes) on or off |
| `HIT_ACTOR` | H→S | per hit | Chief damaged an actor's proxy (§10.2) |
| `PLAYER_DIED` | H→S | once | Chief's body reached 0 |
| `EXPLOSION` | H→S | per explosion | push Skyrim's loose objects |
| `SHOT` | H→S | per projectile tick | the path of a shot, so Skyrim can damage destructibles and set off traps |
| `FLASHLIGHT` | H→S | on change | Chief's flashlight colour, reach and cone (§12.5) |
| `WEAPON_RETURNED` | H→S | when Chief lets go | a weapon or grenades back to Skyrim as items, with their rounds |
| `CHIEF_STATE` | H→S | on change, ≤ 4 Hz | Chief's kit for the co-save |
| `LOG`, `CONSOLE` | H→S | any time | a line for `Chiefrim.log`, or for Skyrim's console |

Message types `0x80`–`0xFF` are reserved for the stretch goals. `0x14` and `0x15` are retired.

### 5.3 Liveness

Both sides write a heartbeat. The plugin writes its own from a thread every 200 ms, so menus and
loading screens (when `PlayerCharacter::Update` doesn't run) don't break the link.

- If Halo's heartbeat stops for 2 s, the plugin unlinks, gives the player back to Skyrim, and asks
  the supervisor to restart Halo.
- If Halo's process ID changes (a restart), the plugin relinks and sends everything again.
- If Skyrim's heartbeat stops, Halo keeps its world and waits; it does not exit. The supervisor
  stops Halo when Skyrim exits.

## 6. The Halo side

### 6.1 The host map

Halo loads one cache map at a time, and every biped, weapon, effect and HUD element comes from it.
Chiefrim loads one **host map**, **`d20`** (Keyes) by default (`CHIEFRIM_MAP` overrides it):

- It must be a campaign level. Multiplayer maps start no game and spawn nobody.
- d20 is the only map with every weapon a player can carry (`tools/list_map_tags.py --matrix`).
- The fuel rod gun and energy sword exist only for the AI; Halo won't let Chief hold them. The
  flamethrower is excluded on purpose: an Xbox leftover that misbehaves in Chief's hands
  (`chiefrim_weapon_carried`).
- d20 has no marines, so NPC proxies use Chief's own biped (§10.1).

### 6.2 Chiefrim mode

Halo runs Chiefrim only when started with `CHIEFRIM=1` (the launcher sets it). In that mode:

- The level's scripts, cutscenes and AI don't run (`game.c`). Once Chief exists, the level's
  actors and objects are erased.
- The level's BSP-switch trigger volumes, background sounds, acoustics and fog are off.
- Camera shake and recoil kick are off: Skyrim's camera is the view.
- Halo's telefrag rule (a player stuck inside another unit dies) is off: Chief stands inside his
  enemies' proxies in a fight.
- Chief is deathless while linked (`cheat.deathless_player`). His death is reported to Skyrim
  instead (§10.4).
- Anti-aliasing is forced off, since the overlay is read back as drawn.
- On each new world, everything Halo had lying around (dropped weapons, grenades, decals) is
  erased: it would be in the wrong place relative to the new origin. Chief keeps what he carries.

### 6.3 How the engine is changed

Chiefrim does not fork OpenCE. `tools/setup_halo.py`:

1. clones OpenCE into `halo/.work` (git-ignored) and checks out the pin in `halo/UPSTREAM`;
2. applies `halo/patches/0001-chiefrim-hooks.patch`, the small hooks in the game's own files, each
   marked `/* CHIEFRIM */`;
3. copies `halo/overrides/` over the tree (whole upstream files replaced for licensing: the disc
   reader `xiso.c`, see [LICENSING.md](LICENSING.md));
4. copies `halo/src/*.c` and the protocol header into `source/chiefrim/`, and `halo/src/port/*`
   into `port/linux/src/`;
5. checks licenses and builds.

After editing hooks in `.work`, `tools/save_halo_patch.sh` writes them back to the patch. Moving
to a newer OpenCE means changing the pin and refreshing the patch.

Chiefrim's own Halo code:

| File | Does |
|---|---|
| `chiefrim.c` | The link, the world context, placing Chief, publishing his state, the frame loop |
| `chiefrim_input.c` | Skyrim's actions into Halo's input state |
| `chiefrim_world.c` | Collision regions, background BSP builds, swapping them in |
| `chiefrim_bsp.c` | The runtime collision BSP builder |
| `chiefrim_combat.c` | Proxies, hits both ways, death, debug weapon giving, hit-shape drawing |
| `chiefrim_inventory.c` | Chief's kit, item hand-over and returns, enemy loot |
| `chiefrim_lighting.c` | Skyrim's light on Halo's objects, Chief's flashlight |
| `port/chiefrim_overlay_gl.c` | The overlay's extra render targets and readback on the port's OpenGL device |

The hooks are in: `main.c`, `game.c`, `players.c`, `player_control.c`, `input_abstraction.c`,
`hud_messaging.c`, `motion_sensor.c`, `scenario.c`/`.h`, `objects.c`, `object_lights.c`,
`damage.c`, `collisions.c`, `items.c`, `projectiles.c`, `units.c`, `decals.c`/`.h`, `render.c`,
`render_cameras.c`, `render_objects.c`, `render_particles.c`, `rasterizer_lights.c`,
`rasterizer_xbox_screen_effect.c`, `rasterizer_xbox_widgets.c`, and the port's `view_fov.c`,
`d3d8_gl.c`, `nv2a_psh.c` and `xgpu.h`.

## 7. Movement and the camera

**Skyrim moves the player; Halo's Chief follows** (`[Movement] bSkyrimMoves=1`, the default).

- Skyrim's own character controller walks, runs, sprints, jumps, sneaks, swims and collides,
  exactly as without Chiefrim. Skyrim's movement, look, jump, sprint and sneak controls stay
  Skyrim's.
- Every frame the plugin publishes the player's position, heading, eye, view direction, velocity
  and on-ground/sneaking flags (`skyrim_player` slot).
- Every Halo tick, Chief is placed at that position and aimed along that view. Halo's movement
  input is ignored and Chief never jumps by himself. He crouches while the player sneaks. His aim
  is clamped to ±85° pitch (Halo asserts past 85.5°).
- The camera is Skyrim's first-person camera. Chiefrim only changes its field of view and hides
  Skyrim's first-person arms and weapon meshes (§7.1).

Halo's collision is therefore used only for shots, grenades and Halo's loose objects, never for
Chief's own movement.

### 7.1 Field of view

Chief has two fields of view, both in `[Camera]` and both measured as Skyrim's FOV setting is: the
horizontal angle of a 4:3 view (vertical = 2·atan(0.75·tan(h/2))). Skyrim sends them in the world
context.

- **The world:** `fWorldFieldOfView` (default 105°; 0 keeps Halo's own) is Chief's unzoomed view.
  A `render_cameras.c` hook scales Halo's projection so the unzoomed view is exactly that angle,
  and zoom keeps Halo's magnification ratios (`chiefrim_field_of_view_tangent_scale`). Halo
  publishes the vertical angle it renders with (`player_state`), and each frame the plugin sets
  Skyrim's camera to it, so zooming narrows Skyrim's view as in Halo (`skse/src/Camera.cpp`).
  `bUseHaloFov=0` keeps the player's own FOV and disables zoom; Halo still draws at its own, and
  the compositor maps the world layer onto Skyrim's view (§12.2).
- **Chief's arms and weapon:** `fViewmodelFieldOfView` (default 80°). The port's first-person
  weapon projection (`port/linux/game/view_fov.c`, hooked) uses it in place of OpenCE's
  `display.viewmodel_fov`, so a wide world doesn't stretch the weapon. Zoom doesn't change it.
  0 leaves it to OpenCE: `display.viewmodel_fov` (`HALO_VIEWMODEL_FOV`) if set, otherwise the
  world's projection, zoom included.

The third-person view is not available while Chief is linked: Toggle POV and the mouse wheel are
Chief's melee and weapon switch. (Skyrim's third person still works during a hand-off, §13.)

### 7.2 Chief's size

Halo's Chief is about 150 Skyrim units tall, too tall for Skyrim's doorways. By default
(`[Chief] fHeight=-1`) his collision and eye height are scaled to the player character's height
(about 128 for a Nord). `fRadius=-1` takes the player's controller radius, but never under 28
units (thinner bipeds tunnel through Halo's surfaces).

### 7.3 The Halo-driven mode (off)

With `bSkyrimMoves=0`, Halo's biped physics moves Chief against the runtime collision BSP, and
Skyrim's player is placed where Chief is every frame (interpolated against Halo's clock to smooth
Halo's 30 Hz ticks). Chiefrim's camera driver then also takes Skyrim's camera position and
direction from Chief's eye.

This was the first design. It works, but Halo's biped catches on edges and bounces on Skyrim's
finely bumpy meshes, so it is not the default. It carries a set of safeguards that only run in
this mode: a floor guard (puts Chief back if Halo pushes him down through a floor), a land rule
(lifts him if he ends up under the terrain), a step assist for ledges Halo won't climb, returns
to the last good spot after a fall, and logs and collision dumps when he gets stuck.

## 8. Controls

**Chief's actions are driven by Skyrim's own user events**, not raw keys. Skyrim's ControlMap
turns keys, mouse and gamepad into user events (including remaps from the Controls menu), and
the plugin maps those to Chief's actions. Rebinding a Skyrim action rebinds Chief's.

Default mapping (`Chiefrim.ini [Controls]`, each a comma-separated list of Skyrim user events):

| Chief's action | Skyrim user event (default key) | Notes |
|---|---|---|
| Move, look, jump, sprint | Skyrim's own | Skyrim moves the player (§7) |
| Crouch | Skyrim's sneak state (Ctrl) | Chief crouches while sneaking; stealth works as normal |
| Fire | Left Attack/Block (LMB) | `sFire` |
| Zoom | Right Attack/Block (RMB) | `sZoom` |
| Reload | Ready Weapon (R) | `sReload` |
| Throw grenade | Shout (Z) | `sThrowGrenade` |
| Melee | Toggle POV (F) | `sMelee` |
| Switch weapon | Zoom In / Zoom Out (mouse wheel) | `sSwitchWeapon` |
| Pick up / swap weapon | Activate (E) | `sAction`; still Skyrim's Activate too |
| Switch grenade | Chiefrim hotkey, G | `iSwitchGrenadeKey` / `iSwitchGrenadeButton` |
| Flashlight | Chiefrim hotkey, V | `iFlashlightKey` / `iFlashlightButton` |

Chiefrim's other hotkeys (DirectInput scan codes in the ini):

| Key | Does |
|---|---|
| F7 `iGiveWeaponKey` | Debug: give Chief the host map's next weapon |
| F8 `iMarkStuckKey` | Log Chief's state and dump the collision around him |
| F10 `iToggleChiefrimKey` | Chiefrim off (Skyrim's own player back, Halo stopped) or on |
| F11 `iRestartHaloKey` | Kill and restart Halo |

How it works:

- An input sink on Skyrim's input device manager sees every event with its user event already
  resolved. Hotkeys are the only raw keys. The plugin warns (log and HUD) when a Chiefrim hotkey is
  also bound to a Skyrim gameplay action.
- While linked and in gameplay, Skyrim's own handlers for the actions Chief takes are switched
  off: attack/block, ready weapon, shout and toggle POV. (In the Halo-driven mode, also movement,
  look, sprint, jump, run and auto-move.) Activate and Sneak stay on. When the link closes, the
  handlers come back on and Skyrim's player controls are reset as pausing would.
- The `input` slot carries a held bit and a press counter per action (so a tap shorter than one
  Halo frame isn't lost), and the routing. Routing goes to Skyrim (Chief gets nothing) while any
  menu, the console or a loading screen is open, and during a hand-off (§13). Halo treats input
  older than 150 ms as none.
- **Prompts name Skyrim's keys.** Twice a second the plugin works out the key bound to each of
  Chief's actions for the device last used and sends the names when they change. Halo's
  `hud_messaging.c` hook writes them where it would draw an Xbox button ("Hold E to swap for").
- Halo's own keyboard still works for testing when no Skyrim is connected.

## 9. Collision

Skyrim's world has to exist inside Halo so that Halo's bullets hit walls, grenades bounce off
floors and weapons lie on the ground. Halo can't take loose triangles: its physics walks the
surfaces and edges of a real collision BSP. So **Chiefrim builds a Halo collision BSP from
Skyrim's Havok shapes at runtime** and swaps it in.

### 9.1 Export (Skyrim side, `skse/src/Collision.cpp`)

Adapted from SkyCraft's Havok harvester (MIT).

- Reads the world's static bodies (statics, animated statics, trees, terrain, invisible walls)
  and resting physics objects at least 24 units across, walking their shape trees (MOPP,
  compressed and extended meshes, lists, transforms). Boxes, capsules and convex hulls become
  outward-wound triangles. All reads are fault-guarded.
- Destructible objects not yet destroyed (spider webs) are left out, so Halo's shots pass through
  them and Skyrim damages them instead (§10.5).
- Triangles are split to edges of at most 256 units and sorted into **1024-unit cube regions**
  by their centres.
- Terrain triangles are flagged `LAND`; terrain and closed-shape faces are flagged `ONE_SIDED`.
- Regions within `[Collision] iRadius` + 1 rings around the player (and one above and below) are
  harvested, at most 3 regions and 2.5 ms per frame. Regions next to the player are re-harvested
  every second (doors open, objects settle) and re-sent only if their hash changed.

### 9.2 Build (Halo side, `chiefrim_world.c`, `chiefrim_bsp.c`)

- Halo keeps the regions it is sent. Every 250 ms, if they changed, a **worker thread** builds a
  BSP from Chief's region and `iRadius` rings around it (5 × 5 × 3 regions at the default of 2),
  capped at 600,000 triangles.
- The builder works like Halo's own map tool: polygons crossing a split plane are cut, large sets
  are halved on axis-aligned planes, cells of up to 64 polygons split on their own planes.
  Surfaces have at most 8 corners. Each triangle becomes two surfaces (itself and a reversed
  twin), except one-sided ones. There are no solid leaves: Skyrim's collision has no inside.
- The host map's structure BSP stays loaded for its clusters, portals and lights. The new BSP's
  plane list starts with a copy of the host map's planes, because the cluster portals index it.
  Every point falls in a leaf that maps to one of the host map's clusters, so nothing in Halo
  is ever "outside the map".
- Before a swap, the build passes a self-test through Halo's own ray and sphere queries (a few
  misses are tolerated). It is swapped in between ticks via `scenario_override_bsps`, and only if
  it was built for the current origin.
- At a swap, bipeds and resting items are re-attached to the new BSP's surface under them.
- Until Skyrim's first regions arrive after a new world, Chief stands on a temporary flat floor
  at the player's feet.

**Winding:** a surface's edges run counter-clockwise in its 2D projection, the winding Halo's ray
and sphere queries treat as inside.

### 9.3 Debugging collision

- Builds slower than 1 s, failing builds, and the collision around Chief when F8 is pressed are
  dumped to `build/collision-dumps/` (git-ignored: they are Skyrim's shapes). `launch_halo.sh`
  prunes old dumps (`tools/prune_dumps.py`).
- `tools/test_bsp.sh` builds the BSP builder alone and runs it on synthetic ground and on those
  dumps; `SELF_TEST=1` runs Halo's real queries on every surface. It also runs under
  AddressSanitizer.

## 10. Combat

### 10.1 NPCs inside Halo: proxies

Each frame Skyrim lists up to **48 living actors within ~5,300 units** (25 wu), with their hit
shapes. For each, Halo keeps a **proxy**: a biped (the host map's marine, or Chief's own biped on
d20) placed where the actor stands, scaled so its head is at the actor's head, on the Covenant
team (so Chief's hits count in full, aim assist works, and the motion tracker sees it). Proxies
are never drawn and never run AI.

**Hit shapes** (`skse/src/Hitbox.cpp`): proxies are hit on **the actor's own Havok hit shapes**,
not on the biped. These are the capsules and spheres on the actor's skeleton bones, the same
shapes Skyrim's arrows hit (boxes and hulls are sent as the capsule along their longest side).
Skyrim often keeps these bodies out of its physics world, so the plugin places each one from its
bone's current animated transform and the body's offset. Halo tests them in place of the biped's
model for shots and melee (`collisions.c`), sizes the proxy's bounding sphere to hold them
(`objects.c`), and measures explosion distance to the nearest shape (`damage.c`). A wolf is hit
where the wolf is; a dragon is as big as a dragon. An actor with no usable bodies gets a single
capsule from its bounds (plus a head sphere for people).

Things that stick (plasma grenades, needles) are attached to the proxy and go off as normal when
the proxy is deleted.

`chiefrim shapes` in the console draws the hit shapes as wireframes (§14.3).
`CHIEFRIM_SHOW_PROXIES=1` draws the proxy bipeds themselves.

### 10.2 Chief hits an NPC

1. Halo's own damage code hits the proxy: every weapon, splash, melee and multiplier is Halo's.
2. A proxy has 100,000 vitality so nothing kills it. Each frame, what it lost is divided by a
   marine's vitality (100) and sent as a **fraction** (`HIT_ACTOR`); the proxy is refilled.
3. Skyrim applies `fraction × the NPC's max health × fDamageMult ÷ toughness`, where toughness is
   `clamp((npcLevel / playerLevel) ^ fLevelExponent, 0.5, 3)`. So a weapon kills a same-level NPC
   in as many hits as it kills a marine in Halo, and higher-level NPCs take up to 3× as many.
4. The hit goes through **Skyrim's own hit processing** (found via the call the melee handler
   makes), so pain sounds, hit reactions, crime, quests and kill credit all work. Hits of half a
   proxy or more stagger; bullets flinch (at most once a second). Blood and the hit sound come from
   the Iron Dagger's impact set. The NPC enters combat with the player.
   `bSkyrimHitProcessing=0` uses plain damage plus Chiefrim's own stagger, flinch and hit event.
5. **Headshots:** a bullet that can headshot (pistol, sniper rifle) hitting a person's head shape
   (`NPC Head [Head]`, within 1.25× its radius) kills the actor outright, whatever its level.
   Creatures' heads are ordinary hits.

### 10.3 An NPC hits Chief

1. While linked, the player is essential and Skyrim keeps his health full: each frame the plugin
   refunds whatever Skyrim's health lost.
2. The refunded amount ÷ `fIncomingReference` (250) is sent as a fraction of Chief's total
   vitality (`PLAYER_HURT`), with the attacker's position and the kind of hit from the last hit
   event (melee, projectile or magic). Damage over time (spells, poison, falls) is sent in sums
   twice a second.
3. Halo applies it with its own damage, using a damage effect from the map (a melee's, a bullet's
   or a plasma bolt's). **Shields absorb first and recharge as in Halo**, and the HUD shows the
   direction.
4. Restore-health potions and food heal Chief's body on the same scale (`CHIEF_HEAL`).

### 10.4 Death

- When Chief's body reaches 0, Halo sends `PLAYER_DIED` once; the plugin clears the essential flag
  and kills Skyrim's player (credited to the last attacker). Skyrim's normal death and reload
  follow, with the hand-off (§13) keeping Chief's HUD out of the death camera.
- The reload is a new world: Chief is made whole, then the save's kit is restored (§11.3).
- If Halo's own player unit changes anyway (a respawn or revert), that counts as a death, the
  level is cleared again and the proxies are forgotten.

### 10.5 Explosions, destructibles and traps

- **Explosions** (grenades, rockets): an actor hit by one is knocked down away from the blast with
  Skyrim's own ragdoll (`fBlastForce`, scaled by the hit's size), then burns for `fBurnSeconds`
  for `fBurnDamage` of its health (fire resistance applies). Skyrim's loose physics objects in
  reach are thrown up and away (`fPropLaunchSpeed`, not for small splashes under
  `fPropMinRadius`).
- **Destructible objects** (spider webs, barricades): Halo reports each projectile's path every
  tick (`SHOT`). Skyrim casts it through its own world, and if the first thing on it is a
  destructible, damages it with `TESObjectREFR::DamageObject` (`fObjectDamage`; explosions up to
  `fBlastObjectDamage`). Decorative cobwebs (statics) are untouched.
- **Traps:** the first scripted object on a shot's path, and every scripted object in an
  explosion's reach, gets a hit event as from the player, with Skyrim's torch as the weapon
  (`bShotsIgnite`), plus a fire magic effect. Oil lamps fall, tripwires and rigged beams go off,
  and oil pools and gas ignite.

### 10.6 Motion tracker

Proxies appear on Halo's motion tracker as units do: while their actor moves (faster than 40
units a second, measured from Skyrim's positions) or attacks (swinging, drawing a bow, casting).
**Red if hostile** to the player, **yellow** otherwise. A sneaking actor counts as crouching and
only shows above Halo's own speed threshold.

## 11. Weapons, items and Chief's kit

### 11.1 Halo weapons as Skyrim items

`ChiefrimArsenal.esp` (built by `tools/arsenal/build.sh`, §15) defines Halo's weapons and grenades
as Skyrim weapon items, with world models converted from the user's own Halo map:

| Form (in the esp) | Item |
|---|---|
| `900`–`907` | needler, assault rifle, pistol, shotgun, sniper rifle, rocket launcher, plasma pistol, plasma rifle |
| `908`, `909` | frag grenade, plasma grenade |

They are found, carried, dropped, stored and sold like any Skyrim item, have no Skyrim damage, and
show their state in their name: **"Needler (20/60)"** (loaded / spare), **"Plasma Rifle (40%)"**
(battery). An unnamed one has a new weapon's rounds.

**Equipping one hands it to Chief** (`skse/src/Arsenal.cpp`, `chiefrim_inventory.c`):

- The item leaves the inventory and goes to Halo with its rounds. Equipped in an item menu
  (inventory, container, barter, gift, favorites), it goes as the last of them closes: taken out
  from under an open menu, the menu read the freed entry and crashed Skyrim.
- If Chief already has the same weapon, his takes the item's rounds up to its maximum, and any
  leftover comes back as an item. An energy weapon swaps with his instead.
- Otherwise Chief takes it into his hands; if he already carries two, the one selected comes back.
- Grenades: equipping one sends all of that type; Chief takes what he has room for and the rest
  comes back.
- If he can't take it (Chiefrim off, no Chief), it comes back whole. While unlinked, an equipped
  Halo item is simply unequipped.
- Each hand-over shows a notification in Skyrim's voice, as the player taking spare kit from
  the pack: "You take the Needler from your pack.", "You salvage 40 rounds and discard the empty
  Assault Rifle.", "You clip 2 Frag Grenades to your armor.", "You can't ready the Pistol now."

**Weapons Chief lets go of come back.** Every weapon Chief drops in Halo (a swap, a third weapon)
that Skyrim has an item for is sent back with its rounds and deleted from Halo's world; the plugin
places it at the player, names it and picks it up ("You stow the Needler (20/60) in your pack."). Empty weapons Halo deletes as usual. Weapons
Skyrim has no item for stay in Halo's world.

### 11.2 Loot

The esp adds leveled lists and container lists; it overrides none of Skyrim's records.
`skse/src/Loot.cpp` adds the lists to the containers as the game loads, with chances from
`[Loot]`, so other mods' changes to the same chests are kept.

| Where | Default chance | What |
|---|---|---|
| Treasure chests (36 in Skyrim.esm) | `fChestChance` 0.12 | a third each: light weapon, medium weapon, 1–2 grenades |
| Boss chests (17) | `fBossChestChance` 0.4 | a third heavy, a third medium, the rest light or grenades |
| Merchants (39 chests: blacksmiths, general stores, caravans, fences) | `fVendorWeaponChance` 0.25, `fVendorGrenadeChance` 0.35 | a light (2/3) or medium weapon; 2 or 4 grenades |

Light: needler, assault rifle, pistol, plasma pistol. Medium: shotgun, plasma rifle. Heavy:
sniper rifle, rocket launcher. A chest the save has already filled keeps its contents until it
resets.

**Enemy drops.** As a proxy is made, Halo arms it by the odds Skyrim sends with the arsenal:

- A person (race with `ActorTypeNPC`): a gun (pistol, assault rifle, plasma pistol or needler)
  with `fEnemyWeaponChance` 0.33; one grenade with `fEnemyOneGrenadeChance` 0.2, two with
  `fEnemyTwoGrenadesChance` 0.1.
- A creature: nothing (`bCreaturesDrop=1`: 1 or 2 grenades).

When the actor dies, the proxy's kit goes to Skyrim (`WEAPON_RETURNED`, reason `LOOT`) and is
placed by the corpse as Skyrim items, falling as an NPC's own weapon would.

### 11.3 The kit and saves

Chief's **kit** is his weapons (by tag path, with their rounds or battery), the one in hand, his
grenades and the selected type, his body and shield vitality, and his flashlight charge.

- Halo sends the kit when it changes (at most 4 times a second). The plugin keeps the latest and
  writes it into the SKSE co-save (`skse/src/CoSave.cpp`, record `KIT `), linked or not.
- Loading a save sends its kit to Halo after the new world has been applied. Chief drops what he
  carries and takes exactly that.
- A save without a kit (a new game, an older save) and the first Halo of a session get the
  **starting loadout** from `[Loadout]`: weapons by name (default: assault rifle and pistol, with
  the rounds a weapon found lying around has), plus frag and plasma grenade counts.
- A restarted Halo gets the latest kit.
- Each restore has a generation, and Halo's reports carry the last one applied, so a stale report
  can't overwrite a just-loaded kit.

### 11.4 Debug weapons

F7 gives Chief the host map's next weapon. The console's `chiefrim give <name>` gives one by name
(`chiefrim weapons` lists them). These bypass the item system; a weapon given this way comes back
as an item when Chief drops it, like any other.

## 12. Rendering

Skyrim renders the world. **Halo renders only its own things**, offscreen, at Skyrim's resolution
(up to 2560×1440; larger screens get a scaled-up picture), through Skyrim's camera.

### 12.1 Two layers

In overlay mode, Halo's `render_window` draws two layers onto transparent black:

| Layer | Contains | Composited |
|---|---|---|
| **World** | Projectiles, tracers, explosions, particles, grenades, weapons on the ground, bullet holes and other decals, the lens flares of their lights | Depth-tested against Skyrim's depth buffer and reprojected onto Skyrim's current camera |
| **Screen** | Chief's arms and weapon, muzzle flash and smoke attached to the weapon, its lights' lens flares (the plasma pistol's overcharge glow), the HUD, Halo's menus | Over Skyrim's picture, under Skyrim's own HUD and menus |

No sky, sun glow, level geometry or Halo fog is drawn. Each lens flare is tested for occlusion in its own layer: a world flare against Halo's world objects (Skyrim's walls hide it only in the compositor's depth test), the weapon's against the weapon, which hides its standby glows as in Halo. Proxies are never drawn, nor the lens flares of what they hold (a plasma pistol's glows).

To make the layers compositable, the port's OpenGL device gets extra render targets
(`chiefrim_overlay_gl.c`, hooks in `d3d8_gl.c` and `nv2a_psh.c`):

- **Transmittance:** the picture's own alpha is scratch space in Halo, so a second target tracks
  how much of Skyrim's picture shows through each pixel, with every blend translated to its
  effect on that. Modulating draws (Halo's bullet holes) darken Skyrim's picture correctly.
- **Depth:** a third target stores each world pixel's view distance, nearest wins. Faint added
  light (a glow's soft edge) is encoded as `CR_FRAME_DEPTH_FAINT` minus its depth, so a faint glow
  near the eye can't pull far objects in front of Skyrim's walls. Glow sprites that Halo projects
  itself are drawn at their real distance.
- **Weapon mask:** a fourth target records how much of each screen-layer pixel is Chief's arms
  and weapon (not the HUD), so only the weapon is graded (§12.3).

At the end of each frame Halo resolves the targets into premultiplied RGBA and reads them back
(`glReadPixels`, waiting for the GPU) straight into the next of three frame slots in the shared
file.

### 12.2 Compositing (Skyrim side, `skse/src/Overlay.cpp`)

- A hook on the swap chain's `Present` (via the vtable, so it chains with SSE Display Tweaks')
  publishes the screen size, uploads Halo's newest frame into textures, and draws it. With
  `[Overlay] bUnderSkyrimMenus` (default), the layers are drawn instead as the frame's first
  Skyrim menu draws (`IMenu::PostDisplay`), so Skyrim's HUD prompts, subtitles, compass,
  notifications and menus are on top.
- The overlay shows only in gameplay (not in menus, loading screens or the console) and hides
  when Halo hasn't published for 500 ms. During a hand-off only the world layer shows.
- **Lockstep:** as `Main::RenderWorld` starts, Skyrim publishes its camera. Halo waits up to
  30 ms for each new camera and draws through it, naming the camera in the frame.
- **Depth test:** Skyrim copies its depth buffer as world rendering ends (before the HUD and
  post-processing). World-layer pixels behind Skyrim's surfaces are dropped, with a little slack
  for decals.
- **Reprojection:** Halo's frame is usually a frame or two behind, and may have a different field
  of view (§7.1). Each frame names its camera and carries Halo's projection; Skyrim keeps its last
  64 cameras, and the world-layer shader marches each pixel's view ray from the current camera
  through Halo's depth to find what Halo drew there from its older eye. Bullet holes stay on
  walls while turning, walking and strafing. `[Overlay] fWaitMs` (default 0) can additionally make
  Present wait for Halo's frame of this very camera.
- **Zoom scopes:** Halo's zoom screen effect would redraw the whole screen from Halo's (empty)
  picture. Over Skyrim, only its mask is drawn (the scope's dark surround and outline), darkening
  Skyrim's picture. The scope's blur and the sniper's night vision are left out.

### 12.3 Matching Skyrim's look

Halo's pixels are drawn after Skyrim's tone mapping and post-processing, so the compositor makes
them match:

- **Fog:** the world layer gets Skyrim's current fog (weather or interior) by distance
  (`[Grade] bFog`).
- **Grade:** both layers (the weapon, not the HUD) get Skyrim's image-space grade: saturation,
  tint, brightness, contrast (only `fContrast` 0.25 of it), and fade (`[Grade] bEnabled`,
  `fStrength`).
- **Weapon look:** each frame the compositor meters Skyrim's picture before Halo's layers go on
  (its key brightness, brightest and shadow brightness, adapting over about half a second).
  Chief's arms and weapon get an exposure that follows the key (`fWeaponExposureMin` 0.5 to
  `fWeaponExposureMax` 1.1), highlights rolled off under the scene's brightest (allowed to white
  in daylight, `fWeaponDaylightKey`), saturation `fWeaponSaturation`, and their darks lifted in
  grey by `fWeaponShadowLift` of the scene's shadows. The match is brightness only: colour comes
  from Skyrim's lights (§12.4), never from what's in view. `bWeaponMatchScene=0` uses fixed
  values.

### 12.4 Skyrim's light on Halo's objects

Halo normally lights objects from its map's lightmap, which Chiefrim's collision BSP doesn't have.
About ten times a second the plugin sends Skyrim's light around the player (`LIGHTING`), and an
`object_lights.c` hook (`chiefrim_lighting.c`) lights each Halo object from it:

- Skyrim's directional ambient, and its key light (the sun or moon outside, the cell's
  directional light inside).
- The four point lights lighting the player most (torches, fires, spells); the strongest is the
  object's fill light and tints its reflection.
- **Sun shadows:** the plugin casts five rays from the eye towards the sun or moon; Chief, his
  arms and his weapon get that share of the key light. Other Halo objects cast their own ray
  through the collision BSP.
- **Roofs:** five rays upward measure how open the sky is; under a roof the sky's light drops to
  0.4.
- **Reflection cap:** a shiny weapon reflects its surroundings, so its reflection strength is
  capped by how bright Skyrim's picture is (`[Lighting] fReflectionMatch`, `fReflectionDark`,
  `fReflectionMin`).

Halo refreshes and blends each object's lighting every tick, so changes (out of a door, a torch
lit) take about a second. `[Lighting] bEnabled`, `fBrightness`, `fPointLights`, `bShadows`.

### 12.5 The flashlight

Chief's flashlight is Halo's own light on his biped, which lights only Halo's objects. Halo sends
its current colour (with Halo's fade in and out), reach and cone whenever it changes
(`FLASHLIGHT`). Skyrim has no unshadowed spotlight, so the plugin moves one of its own dynamic
point lights each frame to just short of where a ray along the view hits, sized to the beam's
width there and dimmer with distance. `[Flashlight] bEnabled`, `fBrightness`, `fReach`.

### 12.6 Skyrim's HUD

While Chief has the player, Skyrim's crosshair and health and magicka bars are hidden (Halo's HUD
replaces them). The stamina bar, compass, sneak eye, activate prompt, enemy health bar and
notifications stay. `[HUD] bHideCrosshair`, `bHideBars`, `bHideStamina` (0).

### 12.7 GPU and window

Halo's window is always hidden (`HALO_HIDDEN_WINDOW`): a Halo window opening over Skyrim on the
same GPU froze Skyrim. On a laptop with two GPUs, `CHIEFRIM_HALO_GPU` picks Halo's GPU: `auto`
(default) uses the NVIDIA GPU through PRIME offload and XWayland's GLX when its 32-bit GLX is
installed. Halo's sound plays through SDL as normal; Skyrim keeps its own.

## 13. Hand-off to Skyrim

Some things only Skyrim can animate. While any of them holds the player, **Skyrim has the
player** (`skse/src/Handoff.cpp`):

- sitting in furniture or at a crafting station, sleeping, riding a mount, swimming;
- a beast form (werewolf, vampire lord: any non-playable race);
- a kill move;
- a script holding the player (AI-driven, or movement controls disabled: the Helgen cart, scenes);
- death (`[Handoff] bDeath`): through the death camera and the reload's loading screen, plus
  `fDeathFadeInSeconds` (2) while the loaded game fades in.

During a hand-off, input routes to Skyrim and Chief gets none; Skyrim's handlers, camera, FOV,
first-person arms, third person and HUD bars are its own; the overlay shows the world layer but
not Chief's weapon and HUD. Skyrim's damage still goes to Chief (§10.3). It starts at once and
ends when nothing has held the player for 300 ms. `[Handoff] bEnabled`.

Deep water is handled this way (Halo has no swimming).

## 14. Running, recovery and debugging

### 14.1 Launching

`tools/launch_halo.sh` supervises Halo:

- As Skyrim's Steam launch options (`/full/path/launch_halo.sh --steam %command%`) it starts Halo
  with Skyrim, outside Proton's container, and stops it when Skyrim exits. It can also be run by
  hand, before or after Skyrim; the plugin links when both are up.
- It restarts Halo whenever it exits or crashes (backing off from 1 s to 30 s while it keeps
  crashing), and obeys Skyrim through `/dev/shm/chiefrim_control` (`restart`, `stop`, `start`).
- Before killing a hung Halo it sends `SIGUSR2`, and Halo writes its main thread's stack to
  `halo.out`.
- Halo's data root is `build/halo-data`, which links to the user's `maps/` (`HALO_MAPS`), so the
  user's own Halo install and saves are never touched. Logs: `build/halo-data/debug.txt` and
  `halo.out` (previous runs as `.1`).

Environment: `CHIEFRIM_MAP` (host map), `CHIEFRIM_HALO_GPU`, `CHIEFRIM_HALO_SHOW_WINDOW=1`,
`HALO_VIEWMODEL_SHIELD=false` (no shield flare on the first-person arms), `--bot` (a scripted
player).

### 14.2 Recovery

- **F11** (or a 2 s hang) kills and restarts Halo; the plugin relinks, sends the world again and
  the latest kit.
- **F10** turns Chiefrim off: the player is Skyrim's own again (controls, camera, health,
  third person if he was in it) and Halo stops. Pressing it again starts Halo and relinks.
- Halo doesn't truncate the shared file on start, so a Skyrim still mapping it doesn't fault.

### 14.3 The console

SKSE can't add console commands on SE/AE, so the plugin takes over an unused developer command
(the first free of `TestSeenData`, `TestLocalMap`, `ShowRenderPasses`, `DumpNiUpdates`) and
renames it `chiefrim`:

| Command | Does |
|---|---|
| `chiefrim restart` / `on` / `off` / `toggle` | As F11 and F10 |
| `chiefrim give [name]` | Give Chief a weapon by name, any case (`chiefrim give sniper rifle`); no name: the next one |
| `chiefrim weapons` | List the names `give` takes |
| `chiefrim shapes [on\|off]` | Draw proxies' hit shapes as wireframes: yellow, a person's head red, shapes from bounds cyan, no shapes white |

Halo answers on the console. With the console open the game is paused, so an answer may only
appear the next time it opens.

### 14.4 Logs

- `Chiefrim.log` (SKSE's log folder): the plugin, plus lines Halo sends.
- `build/halo-data/debug.txt`: Halo's own log, including every `chiefrim:` line.
- Periodic summaries: the weapon look's meter (`the weapon's look:`), Skyrim's grade (`grade:`),
  lighting, and every 30 s how evenly the player moved.

### 14.5 Offline testing

`tools/fake_skyrim.py` stands in for the plugin: it maps the shared file, sends a world, input,
actors, lighting, items and so on, and prints Halo's state. Its many options script specific
scenarios (`--overlay WxH --grab DIR` saves Halo's frames as PNGs; `--fire-at`, `--actor-shape`,
`--give-item`, `--recenter-every`, ...; see `--help`). `tools/run_phase0.sh` runs Halo in a
headless gamescope against it and prints both logs.

Other test tools: `tools/test_protocol.sh` (layout), `tools/test_bsp.sh` (BSP builder),
`tools/check_licenses.py` (§16).

## 15. The Arsenal

The Arsenal is Chief's whole kit as a Skyrim mod: his Mjolnir armor and Halo's weapons and
grenades. `tools/arsenal/build.sh HALO_MAP [SKYRIM_DATA_DIR]` turns Chief's model in the user's
own Halo map into a Skyrim armor set, and builds Halo's weapon items (§11.1), all into
`ChiefrimArsenal.esp` and its meshes and textures (in `build/arsenal/Data/`, and packed as
`build/dist/ChiefrimArsenal.zip`). Nothing it produces goes in the repo or may be
redistributed.

| Script | Does |
|---|---|
| `halo_model.py` | Reads Chief's model and bitmaps from the map; bakes his shader as the Xbox draws it (campaign colour, reflection cube maps, gold visor) |
| `fit.py` | Fits him to Skyrim's skeleton (read from the user's Skyrim): joints and segments onto Skyrim's bones, soles on the ground, helmet lowered onto the head |
| `armor.py` | Splits him into helmet, cuirass, gauntlets and boots by bone weight, as Skyrim's armor divides a body; writes skinned SE NIFs (both weight-slider ends, male and female cuirasses, first person) via `nif.py` |
| `ground.py` | Each piece's ground model, with a convex-hull collision on Steel Plate's rigid body |
| `weapons.py` | Halo's weapons' and grenades' world models as Skyrim ground models with collision |
| `textures.py` | Uncompressed DDS textures |
| `plugin.py` | `ChiefrimArsenal.esp` (via `esp.py`): armors from Skyrim.esm's Daedric set, weapons from the Iron Dagger, the loot lists and container lists |
| `bsa.py` | Archive support |
| `blender_preview.py` | Headless Blender renders of each stage, for checking |

The armor is cosmetic (Daedric stats) and isn't placed in the world or in loot; add it with the
console. The cuirass also covers the tail slot (40).

## 16. Repo layout

```
Chiefrim-Project/
  LICENSE                     GPL-3.0
  THIRD-PARTY-NOTICES.md
  Chiefrim/
    docs/                     DESIGN.md (this), LICENSING.md, archive/
    protocol/                 chiefrim_protocol.h (the shared layout), test_protocol.c
    skse/                     the SKSE plugin: CMakeLists.txt, Chiefrim.ini, src/, cmake/ (cross toolchain),
                              extern/CommonLibSSE-NG (submodule)
    halo/                     UPSTREAM (the OpenCE pin), patches/, src/ (Chiefrim's Halo code),
                              overrides/, test/ (BSP harness)
    tools/                    build, launch, test and Arsenal scripts
    art/logo/                 the logo and the Halo font
    licenses.toml             every third-party component and its license
    build/                    (git-ignored) builds, Halo's data root, dumps, dist zips
  HaloProjects/, References/  (git-ignored) the user's Halo files; SkyCraft and other references
```

**Licensing:** Chiefrim is GPL-3.0-or-later because the plugin links CommonLibSSE-NG. Every
third-party component must be listed in `licenses.toml` under a compatible license;
`tools/check_licenses.py` enforces it and runs before every build. See
[LICENSING.md](LICENSING.md).

## 17. Decisions

| # | Decision | Why |
|---|---|---|
| 1 | **Skyrim moves the player** (`bSkyrimMoves=1`) | Halo's biped caught and bounced on Skyrim's meshes; Skyrim's controller is made for them |
| 2 | **Halo owns Chief's health**; Skyrim's player stays full and essential while linked | One health bar; lowering Skyrim's would put the essential player into bleedout |
| 3 | **Level-scaled damage**: Halo's hit fraction × NPC max health ÷ toughness | Weapons feel like Halo against peers; Skyrim's difficulty curve stays |
| 4 | **Weapons are Skyrim items**, found as loot, equipped to hand to Chief | Uses Skyrim's own inventory, loot and trading; replaced placed weapon caches |
| 5 | **Host map d20** | The only map with every weapon a player can carry |
| 6 | **Swimming, furniture, mounts, beast forms, scenes and death are Skyrim's** (hand-off) | Halo has no animations for them |
| 7 | **No third-person view** while linked | Halo never had one |
| 8 | **No fork of OpenCE**: a pinned commit, one patch, our own sources | Small, reviewable, easy to update |
| 9 | **GPL-3.0-or-later** | Required by CommonLibSSE-NG |
| 10 | **Covenant enemies deferred** (stretch goal), along with vehicles | v1 is Chief in Skyrim against Skyrim's enemies |

## 18. Known issues and limits

- **Collision holes:** on real Skyrim meshes about 0.5–1.5% of surfaces miss Halo's ray query,
  clustered where many planes meet nearly at a point. Rare shots may pass through thin geometry.
- **Collision reach:** shots only hit Skyrim's world within the built regions (at least 2048
  units at `iRadius=2`). Beyond that they fly on.
- **One depth per pixel:** where a bright near light covers a far one, the far one shows with it.
- **Zoom:** the scopes' blur and the sniper's night vision aren't drawn.
- **Movement feel:** Chief moves at Skyrim's speeds and jump height, not Halo's.
- **Spells and shouts** aren't available while Chief has the player (attack and shout are his).
  Menus, the inventory and dialogue work as normal.
- **CPU readback:** each frame's layers go through shared memory (~8 MB per layer at 1080p).
  GPU sharing between DXVK and Halo's OpenGL would be faster but isn't built.
- **Skyrim's hit-processing call** is found from the melee handler; if another plugin has hooked
  it, Chiefrim follows the hook. If it can't be found, the fallback path applies damage directly.
- **Release legality:** Chiefrim distributes no game data. Whether a public release may include a
  built Halo executable (a decompilation of Microsoft's game) is undecided.

## 19. Tried and dropped

- **Halo-driven movement as the default** (§7.3): kept behind `bSkyrimMoves=0`.
- **Hooking Halo's collision queries with loose triangles:** Halo's biped physics needs real BSP
  surfaces and edges.
- **Keeping triangles whole in the BSP**, or plane chains: duplicated triangles without end, or
  made Halo's sphere query exponential.
- **Weapon caches** (Halo weapons lying beside Skyrim chests): replaced by Halo weapons as Skyrim
  items (§11). Older saves' `CACH` co-save records are skipped.
- **A shadowed spotlight for the flashlight:** Skyrim's spot lights came out as a tiny dot with
  every approach tried. The point light (§12.5) is used instead.
- **Tinting the weapon by the scene's colour:** the picture's colour is what's in view (leaves,
  sky), not the light's. The weapon look matches brightness only.
- **Running Halo on Windows inside Proton** (plan B for the link): unnecessary, as `/dev/shm`
  works across Proton.
