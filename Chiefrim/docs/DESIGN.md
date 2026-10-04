# Chiefrim — Design Doc

> Play Skyrim as Master Chief: Halo: Combat Evolved's movement, shields, weapons, grenades, HUD
> and first-person view, running in the real Skyrim world and fighting Skyrim's NPCs.

Status: draft v0.1 · 2026-10-04

The approach follows [SkyCraft](https://github.com/chasmlol/SkyCraft) (Skyrim + Minecraft). Its
`docs/DESIGN.md` is the reference for anything this doc does not change.

---

## 1. Core principle

**Neither game is rewritten.** Halo runs its own game logic: biped physics, shields and health,
weapons, projectiles, grenades, damage, the HUD, and the first-person weapon. Skyrim runs its
own world: terrain, buildings, NPCs, AI, quests, dialogue and saves.

The two halves only **translate** between them:

- Skyrim tells Halo *what the world is shaped like* and *where the NPCs are*.
- Halo tells Skyrim *where the player is*, *what the player hit*, and *what to draw on top*.

If we find ourselves re-implementing a Halo mechanic in the SKSE plugin, or a Skyrim mechanic in
the Halo engine, the design has gone wrong.

**What differs from SkyCraft.** On the Halo side we have the engine's **source**: the
[halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal) decompilation of Xbox
build 2342. SkyCraft had to reach into Minecraft through Mixins. We change the engine directly,
in a small and clearly marked set of hooks (§14).

**No game data in the repo.** The repo holds code only. Halo's data comes from the user's own Xbox
disc image, which the Halo port extracts on the user's machine. Skyrim's data stays in the user's
install. Nothing derived from either game's assets is ever committed. `HaloProjects/` and
`References/` are git-ignored.

### Scope

| In scope (v1) | Stretch goal, revisit later |
|---|---|
| Chief as the player: movement, jumping, crouching, falling and fall damage | **Covenant enemies** replacing some Skyrim enemies (AI ownership, rendering and spawning all undecided) |
| Shields, health, melee, weapons, grenades, flashlight, zoom | Vehicles |
| Halo HUD and first-person weapon | Skyrim skill progression from Halo play |
| Fighting Skyrim NPCs, who fight back | Multiplayer |

Nothing in v1 may rule out the stretch goals. In particular, the protocol is versioned and has
spare message types (§10).

## 2. Target environment

| Thing | Value | Notes |
|---|---|---|
| Host OS | **Linux** (CachyOS) | Skyrim runs under Proton. Halo runs natively. |
| Skyrim | **SE/AE 1.6.1170**, Steam, Proton Experimental | SKSE64 1.6.1170 and Address Library are installed. SkyCraft used 1.7.104; CommonLibSSE-NG covers both. |
| Mod manager | Vortex | A heavy mod list (~250 files in `SKSE/Plugins`). No shader or ENB replacer is installed; Community Shaders has been removed. **SSE Display Tweaks** hooks the swap chain (§9, §15) |
| Display | 1920x1080 borderless, VSync on | Set by SSE Display Tweaks (`FramerateLimit = 300`) |
| SKSE plugin | C++23, CommonLibSSE-NG, CMake + vcpkg | **Cross-compiled on Linux** to a Windows x64 DLL: clang-cl + lld-link + an xwin MSVC SDK. This is proved in Phase 0. |
| Halo | halo-ce-universal, **Linux 32-bit (i386) build**, OpenGL 4.5, SDL3 | `python configure.py && ninja linux`. 32-bit because the tag and cache data contain 32-bit pointers. |
| Halo data | `maps/` extracted from the user's Xbox ISO | The game loads one `.map` (cache file) at a time (§5.3) |

## 3. Components

```
┌──── SkyrimSE.exe (Proton, Win x64) ────────────────┐        ┌──── halo (native Linux, i386, hidden window) ────────────────┐
│  chiefrim.dll  (SKSE plugin, CommonLibSSE-NG)       │        │  Chiefrim engine hooks (in the decomp fork)                  │
│                                                     │        │                                                              │
│  WorldExporter   ─ Skyrim collision near player ────┼──────▶ │  CollisionField  → added to Halo collision queries           │
│  ActorMirror     ─ nearby NPCs (pos, box, state) ───┼──────▶ │  proxy bipeds (invisible, hittable)                          │
│  InputBridge     ─ raw keyboard/mouse ──────────────┼──────▶ │  replaces SDL input for player 1                             │
│  HitBridge       ─ "NPC hit player for X" ──────────┼──────▶ │  object_cause_damage() on Chief (shields first)              │
│                                                     │        │                                                              │
│  PlayerPuppet    ◀─ position / facing / pose ───────┼─────── │  Chief biped physics (real Halo code)                        │
│  CameraDriver    ◀─ eye, forward, up, FOV ──────────┼─────── │  first-person camera                                         │
│  DamageApplier   ◀─ "you hit NPC 0x1A2B3 for X" ────┼─────── │  damage hook on proxy bipeds                                 │
│  Compositor      ◀─ color + depth layers (CPU) ─────┼─────── │  offscreen render: world / first-person / HUD layers         │
└─────────────────────────────────────────────────────┘        └──────────────────────────────────────────────────────────────┘
                     file-backed shared memory in /dev/shm (§10), polled seqlocks + SPSC rings
```

Plus one shared piece, **`protocol/chiefrim_protocol.h`**. Both halves are C or C++, so a single C
header is the whole schema. SkyCraft needed a C++ header and a Java mirror kept in step.

## 4. Coordinate mapping

Both games are **Z-up and right-handed**. That makes the mapping a scale and an offset, with no
axis swap.

- **Scale:** 1 Halo world unit (wu) = 10 ft = 3.048 m. With Skyrim's 70 units/m (SkyCraft's
  figure), **1 wu = 213.36 Skyrim units**.

```
halo.xyz = (sky.xyz - origin) / 213.36
halo.yaw = π/2 - sky.rotZ    (Skyrim heading is clockwise from +Y/north; Halo yaw is
                              counter-clockwise from +X. The sign is pinned down by a test in Phase 0.)
halo.pitch = -sky.rotX       (sign to be confirmed by the same test)
```

- **Origin:** one per worldspace or interior cell, sent in `WorldContext`. Tamriel spans roughly
  ±250k units, about ±1200 wu, where 32-bit floats still resolve under a millimetre. So v1 needs
  no floating origin, and the origin only changes at load doors.
- **Size:** Chief stands about 0.7 wu, roughly 150 Skyrim units, against the Dragonborn's ~128.
  NPCs will look a little short next to him, which is true to the lore. The Skyrim camera uses
  Halo's eye height.

## 5. The mirror world (Halo side)

### 5.1 Halo has no void world

Minecraft let SkyCraft run an empty world. Halo cannot: every object is located through the
level's structure BSP. A point outside the BSP gets no cluster, and the object is flagged
`_object_outside_of_map_bit` (`source/objects/objects.c:1797`). It then leaves the PVS, stops
rendering, and stops colliding with other objects.

The fix comes in two steps:

| Stage | Method |
|---|---|
| **A: Phase 0 shortcut** | Load a small real map. Hook `scenario_location_from_point` (`source/scenario/scenario.c:791`) so that while Chiefrim is active, every point resolves to one valid cluster and leaf. Turn off structure collision (§5.2) so the map's own geometry is never touched. Skip structure and sky rendering (§9). |
| **B: final** | Build a **synthetic structure BSP and collision BSP in memory** at map load: one cluster and one leaf, bounds that cover the worldspace, no surfaces, no lightmaps, no pathfinding data. The tag structs are all in the source (`structures/structure_bsp_definitions.h`, `physics/collision_bsp_definitions.h`). |

Halo's own **AI pathfinding is not needed in v1**, so the synthetic BSP carries no path data.
This is the main reason the Covenant stretch goal is a separate decision.

### 5.2 CollisionField: how Skyrim's shape reaches Halo physics

Halo's biped and projectile physics ask for collision through a few entry points in
`source/physics/collisions.c`:

- `collision_get_features_in_sphere` (`:750`): gathers spheres, cylinders and **prisms**
  (triangles) near a body. Biped movement (`collision_move_pill` and friends) is built on it.
- `collision_test_vector` (`:291`): ray tests, used by projectiles, aim and the camera.
- `collision_test_point`, `collision_test_sphere`, `collision_test_pill`.

A Chiefrim hook in each one **adds results from the CollisionField**: a triangle soup of Skyrim
collision near the player, kept in a spatial hash in Halo units. Halo's own collision response then
runs unchanged against it: step-up, sliding, slope limits, jumping and fall damage.

This is **better than SkyCraft**. Minecraft needs axis-aligned boxes, so SkyCraft voxelised
Skyrim at 1/8 block. Halo collides against triangles, so Skyrim's geometry goes in as it is: slopes
stay slopes, and there are no micro-steps.

**Where the data comes from (Skyrim side, WorldExporter):** the same plan as SkyCraft.

| Stage | Method | Covers |
|---|---|---|
| A: MVP | Havok ray casts on a grid around the player, turned into a triangle heightfield, with several hits per column for overhangs | Terrain and most statics |
| C: final | Walk the loaded `bhkWorld` and read the real shapes (heightfields, compressed meshes, boxes and capsules) into triangles on a worker thread | Everything, including mod-added content and opened doors |

The data streams as deltas per 16 m cell section. Halo keeps a ring of sections around the player
and evicts the far ones. Halo's material types (which drive footstep sounds, decals and impact
effects) are mapped from Skyrim's Havok material IDs: stone, dirt, snow, wood, metal, water.

### 5.3 Tag source

Halo loads **one cache `.map` at a time**, and every biped, weapon, projectile, effect and HUD
definition comes from that map. Chiefrim loads one **host map**. Phase 0 includes a small tool
that lists each map's bipeds and weapons, and we pick from that list:

- A multiplayer map (for example `bloodgulch`) loads quickly and has the multiplayer weapon set
  and the multiplayer Chief biped.
- A campaign map has the campaign Chief and more effects, but fewer weapons per map.

Merging tags from several maps is later work.

## 6. The player

**Halo is authoritative for the player's position and physics.**

1. Each Halo frame, Chiefrim sends `PlayerState`: the interpolated position and facing (the
   port's `render_interpolation.c` blends the 30 Hz ticks), pose (standing, crouching, airborne),
   on-ground, and the camera (eye, forward, up, vertical FOV).
2. **PlayerPuppet** in Skyrim disables the player's own movement and moves the `PlayerCharacter`
   and its Havok capsule to that position every frame. The player stays a real Skyrim actor, so
   NPC targeting, detection and stealth, trigger volumes, quest location checks and projectile
   hits all keep working.
3. **CameraDriver** forces Skyrim into first person, overwrites the camera with Halo's, and
   converts Halo's vertical FOV to Skyrim's horizontal one. The Skyrim body and arms are hidden.

**Deep water — open decision (§13).** Halo has no swimming. Options:

- Chief walks on the bottom of lakes and rivers.
- Control hands over to Skyrim's swimming while the player's head is underwater.

## 7. Input

Same model as SkyCraft. The Skyrim window has OS focus. **InputBridge** reads raw input through
Skyrim's input device manager, swallows it from Skyrim's controls, and writes it to the shared
input slot. In Halo, the SDL keyboard and mouse path in `port/linux/src/sdl_platform.c`
(`platform_pump_events`) takes its input from that slot instead. The Halo window stays hidden, and
it is told it has focus so the mouse stays captured.

| Key | Goes to | Does |
|---|---|---|
| **G** | Skyrim | Activate: doors, NPCs (talk), containers, levers, furniture |
| **Esc** | Skyrim | Skyrim journal / system menu (Halo's pause menu is suppressed) |
| **J** / **M** / **T** | Skyrim | Journal / map / wait |
| **F9**, **~** | Skyrim | Quickload, console |
| **O** | Halo | Halo settings screen (controls, mouse, audio), drawn in the HUD layer |
| everything else | Halo | Halo's own bindings: WASD, mouse, fire, grenade, melee (F), reload (R), action (E: pick up or swap weapons), flashlight (Q), zoom (Z) and so on |

**Routing modes** (from SkyCraft):

- **Gameplay:** input goes to Halo, with the allow-list above going to Skyrim.
- **Halo screen open:** input goes to Halo.
- **Skyrim menu open** (dialogue, barter, lockpicking, map, loading screen): input goes to
  Skyrim, and the Halo game clock is frozen (`game_time` stops).

## 8. Combat

### 8.1 Skyrim NPCs inside Halo: proxy bipeds

For every Skyrim actor within ~25 wu (about 75 m), Chiefrim spawns a **proxy**: a biped from the
host map, **not rendered**, flagged so the AI never runs on it, and moved to the actor's position
and facing every tick.

- **Hitbox:** v1 reuses the proxy biped's collision model, scaled to the actor's height. That is
  close for humanoids and rough for wolves, dragons and giants. A later version builds a
  collision model in memory from the actor's bounds (one capsule, plus one sphere for the head).
- Because proxies are real Halo objects, **Halo's own code** handles bullets, plasma, needler
  supercombines, grenade splash, melee, headshots and knockback impulses.
- Each proxy carries the actor's FormID and hostile, essential and dead flags.

### 8.2 Chief hits an NPC

1. Halo computes the damage on the proxy through its normal path: `object_cause_damage`
   (`source/objects/damage.c:1353`) and the area-of-effect path.
2. The hook reads the result, keeps the proxy alive (it isn't the real NPC), and sends
   `HitActor {formId, damage fraction, isHeadshot, damageType, impulse, sourceWeapon}`.
3. **DamageApplier** in Skyrim applies it through the game's own hit pipeline, so the NPC reacts
   properly: stagger, blood, sound, aggro, and a crime alarm against citizens. SkyCraft's
   `skse/src/Combat.cpp` is the starting point. The fallback is `DamageActorValue(Health)`, then an
   assault alarm, then a stagger event.
4. **Damage scaling (proposed, §13):** Halo damage is measured against the proxy's own vitality.
   Skyrim damage = that fraction × the NPC's max health × `fDamageMult` (default 1.0). A weapon
   then takes as many shots to kill a Skyrim bandit as it takes to kill the proxy biped in Halo,
   at any NPC level.

### 8.3 An NPC hits Chief

1. Skyrim's hit on the player puppet (melee, arrow, spell) is caught in a hook and **cancelled on
   the Skyrim side**.
2. The plugin sends `PlayerHurt {amount, type, sourceFormId, direction}`.
3. Halo applies it with `object_cause_damage` on Chief, using damage types made for Chiefrim
   (melee, arrow, magic). **Shields absorb first and recharge on Halo's normal delay.** The HUD
   shield bar, the damage indicators and the shield-down sound all work as they do in Halo.
4. Scaling (proposed): Halo damage = Skyrim damage ÷ `fIncomingReference` (default 250) × Chief's
   total vitality.
5. **Halo health is authoritative.** Skyrim's player health mirrors Chief's body (not his shields)
   as a fraction, so NPC behaviour still reads sensibly. Chief dying kills the Skyrim player and
   runs Skyrim's normal death and reload flow. Halo's respawn is suppressed.

### 8.4 Weapon acquisition — open decision (§13)

Without Covenant enemies there are no weapon drops. Options:

- **a)** A starting loadout from the ini, for example the MA5B and the M6D. Weapons dropped or
  swapped stay in the Halo world.
- **b)** Weapon caches placed at Skyrim locations: ammo crates in forts, weapons on racks.
- **c)** A loot bridge, where certain Skyrim items in containers become Halo weapons or ammo.

v1 ships (a) with a debug command to spawn any weapon. (b) and (c) are later work.

## 9. Rendering

Skyrim renders the world. Halo renders **only its own things**, offscreen, at Skyrim's resolution,
using the camera Skyrim is about to use. It draws three layers:

| Layer | Contents | Composited |
|---|---|---|
| **World** | Projectiles, tracers, plasma, explosions and particles, grenades in flight, weapons lying on the ground, muzzle flashes. No structure, no sky, no fog, cleared to transparent. | During Skyrim's frame, **depth-tested against Skyrim's depth buffer** |
| **First person** | Chief's arms and weapon (`first_person_weapon_render_update`), with Halo's fire, reload and melee animations | After Skyrim's scene, before its HUD |
| **HUD** | Halo's HUD (`hud_draw_screen`): shields, health, ammo, grenades, motion tracker, crosshair, and any open Halo screen | On top of everything |

- **Halo side:** `render_window` (`source/render/render.c:302`) gets a Chiefrim mode that skips
  `render_sky`, the structure lightmap and visibility passes, and the parts of the world Skyrim
  already draws.
- **Lighting:** with no lightmaps, object lighting comes from Skyrim. `WorldContext` carries the
  sun direction and colour and the ambient light, from Skyrim's weather and time of day.
- **Transport (v1, CPU):** Halo reads color and depth back into the shared memory frame slots
  through asynchronous PBOs (triple-buffered), and the plugin uploads them into D3D11 textures.
  At 1080p that is about 16 MB per layer per frame. GPU sharing between Skyrim's D3D11 (under
  DXVK on Vulkan) and Halo's native OpenGL is a later optimisation through Vulkan external memory,
  and only if the CPU path is too slow.
- **Frame lockstep:** as in SkyCraft. Skyrim signals "begin frame N" with the camera. Halo renders
  frame N. Skyrim waits, with a timeout, for "frame N ready" before compositing, and reuses frame
  N−1 if Halo misses the deadline.
- **Hook points in Skyrim:** start from SkyCraft's `Overlay.cpp` and `WorldRender.cpp`, then check
  them with RenderDoc. No shader replacer is installed. **SSE Display Tweaks** hooks the swap
  chain and limits the frame rate, so the compositor and the frame lockstep are tested with it
  from Phase 2.
- **Sound:** Halo plays its own sound through SDL (gunfire, reloads, shield alarms, Chief's
  grunts), positioned at the shared camera. Skyrim keeps its own sound. Unlike SkyCraft, Halo is
  **not** muted.

## 10. Protocol / IPC

**The platform split is the main new problem.** Skyrim runs inside Proton (a Windows process in
Steam's runtime container). Halo is a native Linux process. Windows named shared memory and named
events (SkyCraft's `Local\SkyCraft_v1`) are not visible across that boundary.

- **Transport (v1):** one file, `/dev/shm/chiefrim_v1`.
  - Halo creates it and maps it with `mmap(MAP_SHARED)`.
  - The plugin opens it as `Z:\dev\shm\chiefrim_v1` and maps it with `CreateFileMapping` +
    `MapViewOfFile`. Wine backs that with a shared mmap of the same file.
  - **Phase 0 verifies** that writes are coherent both ways, and that pressure-vessel shares
    `/dev/shm` with the Proton container.
- **No named events.** Both sides poll seqlock slots and ring heads, each once per frame or tick,
  with a short spin around the frame lockstep.
- **Fallback transport:** build Halo for Windows and run it inside Skyrim's Proton prefix. Then
  SkyCraft's Win32 shared memory and events work as they are. The upstream Windows build only
  builds on Windows, so this costs a cross-compile setup. It is kept as plan B.
- **Layout:** a header (magic, protocol version, both PIDs, heartbeats), latest-value slots under
  a seqlock, and two SPSC ring buffers.
  - One side is **32-bit and the other 64-bit**, so every struct uses fixed-width types with
    explicit padding and no pointers or `size_t`.
  - A layout test (`static_assert` on sizes and offsets) is compiled into both builds.
- **Crash safety:** heartbeats. If Halo dies, Skyrim restores normal player control. If Skyrim
  dies, Halo exits.

Initial message catalog:

| Dir | Message | Rate |
|---|---|---|
| S→H | `Hello / Heartbeat` | 1 Hz |
| S→H | `WorldContext {worldspace/cell, origin, sun dir/colour, ambient, timeScale}` | On change |
| S→H | `CollisionSection {sectionPos, triangles[], materials[]}` / `CollisionEvict` | Streamed |
| S→H | `ActorUpsert {formId, pos, yaw, height, radius, flags}` / `ActorRemove` | 30 Hz |
| S→H | `Input {keys, mouse dx/dy, wheel, buttons}` | Per frame |
| S→H | `PlayerHurt {...}` | Event |
| S→H | `Teleport {pos, yaw}` (load doors, fast travel, scripted moves) | Event |
| S→H | `Freeze {on}` (Skyrim menu or scene owns control) | On change |
| S→H | `BeginFrame {frameId, viewport, camera}` | Per frame |
| S→H | `SaveRequest / LoadState {blob}` | Event |
| H→S | `PlayerState {pos, yaw, pitch, pose, onGround, camera, bodyFrac, shieldFrac}` | Per frame |
| H→S | `HitActor {...}` | Event |
| H→S | `PlayerDied` | Event |
| H→S | `FrameReady {frameId, layer offsets}` | Per frame |
| H→S | `MenuState {haloScreenOpen}` | On change |
| H→S | `SaveState {blob}` | Reply |

Message type IDs 0x80–0xFF are reserved for the stretch goals (Covenant, vehicles).

## 11. Other systems

- **Save and load:** SKSE serialization stores a Halo **player-state blob** in the Skyrim co-save:
  weapons, ammo, grenades, body and shield vitality, flashlight charge. There is no Halo world to
  snapshot, which makes this much simpler than SkyCraft's. Loading a Skyrim save restores the blob
  into a freshly spawned Chief.
- **Load doors, fast travel and teleports:** Skyrim is authoritative for these. The plugin sends
  `Teleport` (and `WorldContext` if the origin changes). Halo moves Chief and clears the
  CollisionField.
- **Skyrim's own animations:** furniture, crafting stations, beds, levers, horses and scripted
  scenes hand control to Skyrim (`Freeze`) until they end, as in SkyCraft.
- **Skyrim HUD:** keep the compass, plus quest and notification messages. Hide health, magicka,
  stamina and the crosshair, because Halo's HUD replaces them.
- **Skyrim inventory, magic, shouts and perks:** not available while Halo drives the player.
- **Launching (v1):** the user starts Halo with `tools/launch_halo.sh` (hidden, Chiefrim mode, host
  map). The plugin connects when it sees a heartbeat. Auto-launch from inside Proton runs into
  Steam's runtime container, where Halo's 32-bit SDL3 and GL libraries may be missing, so it is
  later work.

## 12. Phased plan

Each phase ends in something you can play.

| # | Phase | "Done" when |
|---|---|---|
| 0 | **Link** | The SKSE plugin cross-compiles on Linux and loads in 1.6.1170. Both sides handshake over `/dev/shm` across the Proton boundary. The coordinate and yaw mapping is unit-tested. Halo runs hidden on the host map with the stage-A location hook and a temporary flat floor at Skyrim ground height. Walking as Chief moves the Skyrim player. `tools/fake_skyrim.py` and `tools/fake_halo.py` stand in for either side. |
| 1 | **Walk Skyrim as Chief** | CollisionField stage A, CameraDriver, InputBridge. You can run, jump and crouch around Whiterun with Halo movement, and slopes and walls behave. |
| 2 | **Overlay** | First-person and HUD layers composited (CPU path). Chief's arms, weapon and HUD are in Skyrim, and reloads and weapon swaps animate. Works with SSE Display Tweaks. |
| 3 | **Combat** | Proxies, HitActor, PlayerHurt, shields, death, the world layer with depth (projectiles, effects, grenades). You can clear a bandit camp with an MA5B and frag grenades. |
| 4 | **Full world** | CollisionField stage C, interiors and load doors, synthetic BSP (stage B), the deep-water decision, furniture and scene hand-off. |
| 5 | **Persistence and polish** | Co-save state, weapon acquisition beyond the loadout, lighting matched to Skyrim weather, better proxy hitboxes for creatures, launch script hardening, third-person view. |
| ★ | **Stretch: Covenant** | Revisit later (see Scope). |

## 13. Decisions

Proposed. Each one needs the user's call before the phase that depends on it.

1. **Damage scaling** (Phase 3): outgoing damage as a fraction of the proxy's vitality × NPC max
   health × `fDamageMult`. Incoming damage ÷ `fIncomingReference` × Chief's vitality. Both are ini
   values. (§8)
2. **Weapon acquisition** (Phase 3): starting loadout + a debug spawn command for v1. (§8.4)
3. **Deep water** (Phase 4): walk on the bottom, or hand over to Skyrim's swimming. (§6)
4. **Host map** (Phase 0): chosen from the tag-listing tool's output. (§5.3)
5. **Decomp management** (Phase 0): see §14.
6. **Covenant enemies:** deferred, stretch goal.

## 14. Repo layout

```
Chiefrim/
  docs/DESIGN.md
  protocol/chiefrim_protocol.h   shared C header + layout static_asserts
  skse/                          SKSE plugin (CMake, vcpkg, CommonLibSSE-NG, C++23,
                                 clang-cl cross toolchain file)
  halo/                          Halo-side changes (see below)
  tools/                         launch_halo.sh, fake_skyrim.py, fake_halo.py, tag lister,
                                 deploy-to-Vortex script
```

**Halo-side changes (proposed):** a fork of halo-ce-universal on a `chiefrim` branch, added as a
git submodule at `Chiefrim/halo/`.

- New code lives under `port/chiefrim/`: the link, CollisionField, proxies, layer rendering and
  input source.
- Upstream files only get small hook calls marked `/* CHIEFRIM */`. That keeps rebasing onto
  upstream manageable.
- The decomp repo contains no game data. Its own `.gitignore` already excludes `assets/` and
  extracted maps.

## 15. Risks

| Risk | Mitigation |
|---|---|
| `/dev/shm` sharing across the Proton container fails or isn't coherent | First test in Phase 0. Fallback: Windows Halo build inside the Proton prefix (§10) |
| Cross-compiling CommonLibSSE-NG with clang-cl on Linux | First task in Phase 0. Fallback: build in a Windows VM, or MSVC under Wine |
| Halo depends on the BSP in more places than §5.1 covers (decals, lighting, sound environments, PVS) | The stage-A hook surfaces them in Phase 0. Each one is fixed at its source or stubbed |
| CPU readback of three layers costs too much | Only redraw the HUD layer when it changes. Lower resolution for the world layer. GPU interop later |
| SSE Display Tweaks' swap-chain and frame-limiter hooks interfere with compositing or the lockstep | Test with it from Phase 2. If it conflicts, adjust its settings or hook at a point it doesn't touch. A shader replacer (Community Shaders, ENB) added later is a separate compatibility task |
| Halo's 30 Hz tick against Skyrim's frame rate | The port already interpolates (`render_interpolation.c`). Skyrim follows the interpolated pose |
| Proxy hitboxes are wrong for non-humanoid creatures | Scaled biped for v1. Built-in-memory collision model in Phase 5 |
| Upstream decomp moves quickly | Pin the submodule commit. Keep hooks small and marked |
| Legal | Fan project. Nothing from either game is distributed, and the user supplies both games. Whether releases may include a built Halo executable is decided before any public release |
