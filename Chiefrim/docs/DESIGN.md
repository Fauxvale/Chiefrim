# Chiefrim — Design Doc

> Play Skyrim as Master Chief: Halo: Combat Evolved's movement, shields, weapons, grenades, HUD
> and first-person view, running in the real Skyrim world and fighting Skyrim's NPCs.

Status: draft v0.3 · 2026-10-05 (Phase 2's compositor folded in)

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
[OpenCE](https://github.com/OpenCommunityEdition/OpenCE) (formerly halo-ce-universal) decompilation of Xbox
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
| Mod manager | Amethyst Mod Manager (Linux) | A heavy mod list (~250 files in `SKSE/Plugins`). No shader or ENB replacer is installed; Community Shaders has been removed. **SSE Display Tweaks** hooks the swap chain (§9, §15) |
| Display | 1920x1080 borderless, VSync on | Set by SSE Display Tweaks (`FramerateLimit = 300`) |
| SKSE plugin | C++23, CommonLibSSE-NG (git submodule), CMake | **Cross-compiled on Linux** to a Windows x64 DLL: clang-cl + lld-link against the MSVC CRT and Windows SDK fetched by xwin (`tools/setup_skse.sh`). Proved in Phase 0. CommonLib's dependencies come through CMake FetchContent instead of vcpkg (spdlog, rapidcsv, and DirectXTK's SimpleMath only, which avoids its shader compiler). The project path must not contain `[ ]` (CMake's `file(GLOB)` reads them as a pattern), which is why the folder is `Chiefrim-Project`. |
| Halo | OpenCE (formerly halo-ce-universal), **Linux 32-bit (i386) build**, OpenGL 4.5, SDL3 | `python configure.py && ninja linux`. 32-bit because the tag and cache data contain 32-bit pointers. |
| Halo data | `maps/` extracted from the user's Xbox ISO | The game loads one `.map` (cache file) at a time (§5.3) |

## 3. Components

```
┌──── SkyrimSE.exe (Proton, Win x64) ────────────────┐        ┌──── halo (native Linux, i386, hidden window) ────────────────┐
│  chiefrim.dll  (SKSE plugin, CommonLibSSE-NG)       │        │  Chiefrim engine hooks (in the decomp fork)                  │
│                                                     │        │                                                              │
│  WorldExporter   ─ Skyrim collision near player ────┼──────▶ │  CollisionField  → runtime collision BSP, swapped in         │
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

**The fix (proved in Phase 0): Chiefrim supplies the level's collision BSP.**

- The host map's structure BSP stays loaded: its clusters, sky, fog and sound environments.
- Chiefrim builds a **collision BSP in memory** and makes it the global one through
  `scenario_override_collision_bsp`. The hook is at the swap point, `scenario/scenario.c` (where a
  structure BSP becomes the global one).
- Every point above Chiefrim's surfaces falls in **leaf 0**, which maps to one of the host map's
  clusters. So nothing is ever outside the map, and nothing in Halo needs to know it isn't in its
  own level.
- The BSP's plane list **starts with a copy of the host map's planes**, because the structure
  BSP's cluster portals index it (`structure_clusters_in_sphere`). Chiefrim's own planes come after
  them. Phase 0 found this the hard way: a one-plane BSP crashed the first time a light connected
  to the map.

Halo's own **AI pathfinding is not needed in v1**, so the BSP carries no path data. This is the
main reason the Covenant stretch goal is a separate decision.

### 5.2 CollisionField: how Skyrim's shape reaches Halo physics

The first plan was to hook Halo's collision entry points (`collision_get_features_in_sphere`,
`collision_test_vector` and the rest) and add loose triangles to their results. **Phase 0 ruled
that out.** Halo's biped movement keeps the **index of the BSP surface it stands on and walks
that surface's edges** (`units/bipeds.c`: support surfaces, `biped_find_ground_surface`, the
"stick to surface" pass after `collision_move_pill`). Loose triangles have no surface or edge
indices.

**So Skyrim's shape reaches Halo as a real collision BSP**, built by Chiefrim and swapped in as in
§5.1. Halo's own code then runs unchanged against it: biped movement with step-up, sliding, slope
limits, jumping and fall damage, plus projectiles, decals and the camera, all with no hooks in the
collision code.

- **Phase 0:** a hand-built BSP of one flat square at the Skyrim player's ground height. It has one
  node, one leaf, one surface, four edges and four vertices.
- **Phase 1:** a **runtime BSP builder** (`halo/src/chiefrim_bsp.c`), on a worker thread, swapped
  in between ticks (`chiefrim_world.c`). Skyrim's collision is an open triangle soup, so:
  - no solid leaves: every leaf is "two-sided", so Halo's ray query tests every plane it crosses,
    exactly;
  - the land (Skyrim's height-field terrain) and the faces of closed shapes (boxes, capsules,
    convex hulls, wound outward), flagged `CR_TRIANGLE_ONE_SIDED` by the exporter, are one-sided:
    their twins are in no leaf. Inside a two-sided solid every face pulls inward too: Chief dipping
    into a road chunk's top was pushed down through it (the twelfth test: Halo's sphere query
    touched 18-46 surfaces inside a 48-unit chunk). Havok mesh triangles are made one-sided by the
    builder where they form a closed mesh (every edge shared by exactly two faces, running opposite
    ways; turned outward by signed volume); open meshes (sheets) stay two-sided. Where terrain pokes up through a road or a floor mesh, its
    back would push whoever stands on the road down through it (the tenth in-game test: Chief in a
    stack of 2-4 layers within 60 units, the floor guard fighting Halo);
  - an edge a surface shares only with its own twin (no neighbour there: a T-junction, a cut, a
    mesh's open border) is split into two edges, each with one surface on both sides: Halo makes
    every edge whose two surfaces' planes differ a collision feature (`collision_features.c`), and
    a surface and its twin always differ, so ~60% of real floors' edges stood up as ridges;
  - what Skyrim is sent is smoothed: Halo moves Chief 30 times a second while Skyrim draws at 60
    and more, so the player and the camera held still and then jumped ~13 units, on and on. As the
    port draws Halo's frames, the published position and eye are a tick behind, blended between
    the last two ticks by `game_time_get_tick_fraction()`; a jump over 100 units cuts; view
    directions stay the latest;
  - and on Skyrim's side: Halo stamps each state with its clock (`time_us`), the plugin keeps the
    last 16 and draws the player where Chief was 25 ms ago, between the two states about then
    (Skyrim draws unevenly against Halo's frames, 31-48 fps against 75 in the eighteenth test:
    taking the latest state, the player stalled and lurched). Small bounces (under 30 units, while
    on the ground or only just off it: Halo's biped leaves the ground for a tick on small rises)
    are smoothed out of what Skyrim is sent with a 60 ms lag; Skyrim's controller is told it's on
    the ground when Chief is (placed every frame, it counted itself in the air all along);
  - Halo never evicts every region it holds: that means Chief isn't where Skyrim's player is yet
    (at the level's spawn point before being placed), and Skyrim sends a region only once while its
    player stays near it (this was the fake-Skyrim test's startup flake, and could leave Chief
    without collision after a load);
  - each triangle is two surfaces, itself and a reversed twin (the sphere query matches surfaces to
    the side it came from); winged edges are shared between neighbours, and no edge is left open
    (Halo's edge features read both sides);
  - polygons that cross a splitting plane are **cut** into a piece on each side, as Halo's own map
    tool does, so the BSP is an ordinary tree and nothing is duplicated. Large sets are halved with
    axis-aligned planes (between the polygons' centres) down to cells of up to 64 polygons; inside
    a cell, splits are on the polygons' own planes, picked for few cuts and balance. Surfaces have
    at most 8 corners (bigger pieces are fanned). Near-identical planes are merged (normals within
    ~0.06°, distances within 0.0005 wu), and each triangle is moved onto its plane exactly, so the
    BSP and Halo agree on where a ray meets it.
    Two earlier designs failed on real Skyrim meshes: keeping triangles whole duplicated them across
    splits without end; *chains* of a cell's planes kept the depth down but made Halo's sphere query
    exponential (a 30,647-triangle build took 9.6 s, and Halo froze in play);
  - triangles overlapping on one plane, which no 2D split can separate, go in extra references to
    that plane in the same leaf (Halo tries them all);
  - each build passes a self-test through Halo's own ray and sphere queries before it is swapped in.
    `tools/test_bsp.sh` (`SELF_TEST=1`) runs the builder and Halo's real queries offline on
    synthetic Skyrim-like ground: 36,434 triangles build in ~0.9 s (depth 33); 160,519 of 160,530
    surfaces pass, the rest being coplanar overlaps where the ray finds the other surface. It also
    runs clean under AddressSanitizer;
  - a fragment lying all but in a splitting plane (within 0.0002 wu: two of Skyrim's faces almost
    on top of each other) goes to both sides, up to 4 times on a path: Halo's ray, in floats, may
    cross that plane a hair after the fragment's own and look for it on the other side. On real
    Skyrim dumps this took the misses from ~0.3% of surfaces to ~0.01%;
  - at a swap, each biped's support surface is the new BSP's surface under its feet (a short ray
    down), not found by triangle id: the pieces of a cut triangle share the id, and a wrong support
    surface made Halo pull Chief toward it at every swap (the stutter of the third in-game test);
  - the in-game self-test rejects a build only for more than 3 (or 25%) misses in its sample: the
    rare misses cluster (thin walls fail build after build), and a rejected build leaves Chief on
    stale collision; the floor guard dumps its first firing and every 200th;
  - exact repeats of a triangle (either winding: Skyrim's two-sided meshes) are dropped, and
    polygons stacked on one plane go in at most 8 references per leaf;
  - Skyrim's world context (a slot) and its collision reset (the ring) arrive separately; the reset
    names the world-context generation it goes with, and Halo builds nothing until both are in.
    (Applying the new origin first, Halo built the old cell's collision around it: after a door,
    Chief stood on the wrong world's geometry for a moment and dropped through.)
  - the exporter splits triangles to edges of at most 256 units before sorting them into regions
    by their centres: a big one (a box's face is two triangles; interiors' floors are big boxes)
    went only to the region of its centre, out of reach while the player stood on it;
  - a build is installed only for the origin it was made for: one still running when Skyrim moves
    the origin (a load, a door, `coc`) is thrown away (installing it shifted the ground under Chief
    and he fell through);
  - after the origin moves, Chief stands on the stand-in floor until a build has ground under him
    (Skyrim's collision arrives region by region, and the first builds may not have his yet;
    swapping the stand-in out for them dropped him through), or 6 s have passed; if that ground
    turns out higher, he is lifted onto it;
  - a floor guard: each frame, if Chief went down through a surface facing up (Halo can shove a
    wedged biped through one) with no floor within a step below, he is put back on top of it with
    his fall stopped (with a floor just below, it is layered geometry, Halo's to settle). It looks along the
    path of his pill's lower sphere's centre, not his feet (Halo's biped origin is the bottom of
    that sphere): rolling off an edge, the feet dip below the edge's height while still over it,
    and a guard on the feet put him back on every frame (he caught on road pieces' and floors'
    edges). Offline, on real
    Skyrim dumps, a short drop through ~95% of floors is seen; the rest have a steeper surface just
    above them or are slivers;
  - the land rule (exteriors): the exporter also flags the land's triangles `CR_TRIANGLE_LAND`,
    and Halo keeps their heights in a grid (0.5 wu cells). With his feet more than 0.15 wu (~32
    units) under the land, Chief is put on top of it. The land has no underside, so once Halo had
    squeezed him through it (a rock mesh overlapping a hillside), nothing pushed him back: he
    stood or wandered in the void under it until he fell (the seventeenth test's marked spots);
  - if Chief still falls below all of the collision loaded around him, he goes back to one of the
    last 16 spots where he stood on a floor of Skyrim's with room around him; back again within 5 s,
    that spot was no good (pushed out of an interior, he can stand on something in the void) and the
    one before is tried, down to where Skyrim put him in this world;
  - Chief is moved as Halo's `player_teleport` does (`biped_fix_position`), but where Halo finds no
    valid spot he is put there anyway: Halo's teleport would kill the player, and a campaign death
    waits for a checkpoint revert Chiefrim never makes (Chief never came back). While linked he is
    also deathless (`cheat.deathless_player`) until deaths follow Skyrim's;
  - a step assist: Skyrim's characters step up onto ledges (a road piece's lip, a boardwalk's
    edge) that stop Halo's biped, or worse: walking into a board's edge 35 units up, Halo pushed him
    down and under it. Checked every 100 ms while he is pushing a way: something in the way below
    48 units (rays every 3 units: boards are 4 thick), nothing at 48, a floor on top. A board's
    edge (open below) lifts him as he reaches it; an ordinary ledge only once he has stopped
    against it (lifting him ahead of every ledge made walking jumpy: 30 lifts in 3 minutes).
    Every 30 s Halo logs how often it moved Chief itself (step-ups, guard put-backs, returns), how
    many short airborne blips he had while walking (hitches), and how many frames took over 40 ms. A second pushing without moving is reported as stuck. If the floor guard fires 8 times
    in 2 s, Chief goes back to where he last stood well;
  - his collision radius, like his height, comes from Skyrim (`[Chief] fRadius`, default: the
    player's character controller, 0.26 Havok units = 18), but never under 0.13 wu (28 units):
    thinner than ~0.12 wu, Halo's biped tunnels through surfaces (at 18 he walked through a wall in
    the fake-Skyrim test, and in game sank into floors, the floor guard bouncing him back ~4 times
    a second);
  - a build takes Chief's region and those around it, `[Collision] iRadius` rings (default 2: 5 x 5
    x 3 regions of 1024 units; Skyrim sends a ring more, protocol 10), so the edge, and how far his
    shots hit Skyrim's world, is always at least that many regions ahead: with 3 x 3 (1024 units,
    ~15 m) shots at walls and NPCs further away passed through (the second in-game test). A build is
    at most 600,000 triangles: rings beyond the nearest two that would go over are left out, logged
    (Halo is 32-bit; in-game 3 x 3 builds were 100,000-200,000 triangles, 1-2.4 s);
  - open: on the latest Skyrim dumps 0.5-1.5% of surfaces miss Halo's ray query (sphere queries
    all hit), clustered where many planes meet nearly at a point. Not the near-plane band, closed
    meshes, one-sided faces or the exporter's subdivision (all tested). Chief falling through a
    road and the land at once, and through a gap in Riverwood's inn floorboards, suggests these are
    holes for him too;
  - still on the stand-in floor 3 s after a load, Halo logs why (regions, generations, builds);
  - when Chief is stuck (a second pushing without moving, no ledge to step onto; once a place) or
    the player presses Chiefrim's "mark stuck" hotkey (`iMarkStuckKey`, F8; the input slot's
    `CR_ACTION_MARK`), his state goes to the log and the collision around him is saved; the
    harness's `PROBE_AHEAD=x,y` casts rays that way from heights above his feet;
  - builds slower than 1 s (3 at most), failing ones, and the one Chief fell through are dumped to
    `build/collision-dumps/` (`CHIEFRIM_DUMP_DIR`, set by `tools/launch_halo.sh`; git-ignored,
    since they are Skyrim's shapes), and `tools/test_bsp.sh` replays them, probing under Chief.
    Each start keeps every dump of a fall and the newest 20 of slow builds (`tools/prune_dumps.py`,
    from `launch_halo.sh` and `run_phase0.sh`: by 2026-10-10 398 had piled up, 728 MB, 352 of them
    slow builds, many of the same place);
    `tools/fake_skyrim.py` has `--recenter-every`, `--start-below` and `--hole` for these cases;
  - the map's clusters are copied without fog planes (b30's sea made Chief "underwater").

**Winding:** a surface's edges run **counter-clockwise in its 2D projection**. That is the winding
the ray query (`collision_surface_test_point`) and the sphere query (`collision_surface_test_sphere`)
treat as inside. `collision_surface_test_point2d`, used only by AI pathfinding, has the opposite
sign. That may be a decompilation slip, and is to be reported upstream. The floor's self-test
checks the winding with the sphere query before any BSP is installed.

This is **better than SkyCraft**. Minecraft needs axis-aligned boxes, so SkyCraft voxelised
Skyrim at 1/8 block. Halo collides against triangles, so Skyrim's geometry goes in as it is: slopes
stay slopes, and there are no micro-steps.

**Where the data comes from (Skyrim side, WorldExporter, `skse/src/Collision.cpp`):** stage C
straight away, adapted from SkyCraft's harvester (MIT), which already reads AE's Havok shapes:
the world's static bodies (static, animated static, trees, props, terrain, ground, invisible
walls, stair helpers), and physics objects (clutter, large debris) at rest (asleep in Havok:
moving ones are left out until they settle; a near region is harvested again each second) and at
least 24 units across, their shape trees (MOPP, compressed and extended meshes, lists,
transforms), and boxes, capsules and convex hulls as outward-wound triangles; every read is
fault-guarded. Triangles go to Halo in Skyrim units, per 1024-unit cube region (each triangle in
the one region holding its centre), within ±2 regions around and ±1 below/above the player, at
most 3 regions and 2.5 ms a frame. Regions next to the player are harvested again every second,
and sent only when their triangles changed (a door opened); far ones are forgotten.

The table below was the original plan:

| Stage | Method | Covers |
|---|---|---|
| A: MVP | Havok ray casts on a grid around the player, turned into a triangle heightfield, with several hits per column for overhangs | Terrain and most statics |
| C: final | Walk the loaded `bhkWorld` and read the real shapes (heightfields, compressed meshes, boxes and capsules) into triangles on a worker thread | Everything, including mod-added content and opened doors |

The data streams as deltas per 16 m cell section. Halo keeps a ring of sections around the player
and evicts the far ones. Halo's material types (which drive footstep sounds, decals and impact
effects) are mapped from Skyrim's Havok material IDs: stone, dirt, snow, wood, metal, water.

### 5.3 Tag source

Halo loads **one cache `.map` at a time**, and every biped, weapon, projectile, effect and HUD
definition comes from that map. Chiefrim loads one **host map**.

- **It must be a campaign level.** Loaded with `map_name`, a multiplayer map starts no game and
  spawns nobody. The port's own profile-training script skips them for the same reason.
- **Phases 0 to 4 used `b30`** (The Silent Cartographer), the decomp's own default campaign level;
  **Phase 5 moved to `d20`** (below).
- **In Chiefrim mode the level's logic is off:** `game_tick` skips `hs_update` (scripts and
  cutscenes) and `ai_update`, and Chiefrim erases the level's actors and other objects once Chief
  exists. Its BSP-switch trigger volumes are off too (`players.c`): Chief, at Skyrim's
  coordinates, walked into one of b30's, the level switched BSPs under Chiefrim's collision, and
  Halo's state went bad (the third in-game test: the overlay flashed through assets, assertions
  followed). What is
  left is Halo's engine with Chief in it.
- Chief first spawns at the level's own starting location, on the level's own collision. When
  Skyrim's world context arrives, Chiefrim installs its collision and moves him (§6).
- `tools/list_map_tags.py` lists each map's tags of a class (`--class weap`, `bipd`, `eqip`;
  `--matrix` compares maps). Run on the Xbox maps (2026-10-07): **d20** (Keyes) is the only map
  with every weapon a player can carry: assault rifle, pistol, shotgun, sniper rifle, rocket
  launcher, flamethrower, plasma pistol, plasma rifle and needler (c40 lacks the flamethrower).
  The fuel rod gun and energy sword are in some maps but only the AI's: Halo refuses them to
  Chief. The multiplayer maps add nothing (and can't host). **The flamethrower is left out**
  (`chiefrim_weapon_carried`: debug key, kits, loot): an Xbox leftover, buggy in Chief's hands in
  game and without a HUD. d20 has no marine, so proxies are Chief's own biped. Unarmed it
  stands bent-kneed, head low and forward, so a proxy is scaled to put **its head marker (measured
  in that pose) where the actor's head node is** (`cr_actor.head`, protocol 16; 0.92 of the height
  for creatures without one). A hit counts against a marine's vitality (100, b30's), not Chief's
  150 (shields and body), which took half again the hits. Chief's biped has no head that headshots
  kill (he's spared them), so Chiefrim makes them: a bullet that can cause one (pistol, sniper)
  within 0.07 world units of the head marker kills the proxy, and Skyrim kills the actor
  whatever its level (`CR_HIT_HEADSHOT`). Hits on proxies bleed: Halo's impact takes the human
  material (`projectiles.c` hook), not the cyborg's sparks. `CHIEFRIM_SHOW_PROXIES=1` draws them.
  Halo's camera shake and kick (explosions, firing) are off in Chiefrim mode (`main.c` hook): the
  view is Skyrim's camera, and the shaken world layer slid decals and bullet holes over Skyrim's.
  A proxy carries a weapon and grenades at random, each as likely: a pistol, an MA5B, a plasma
  pistol or a needler, and its biped's grenade count of frags or plasmas, and drops them when its
  actor dies, however: Skyrim lists the newly dead for 3 s (`CR_ACTOR_DEAD`), and the proxy dies
  Halo's way and lets go of its gun at once (a dying unit drops it partway through its death
  animation, and the proxy was deleted the next frame: the first in-game test saw grenades fall,
  never a gun). Offline (`fake_skyrim.py --actor-dies-at`): the pistol and grenades on the ground.
  **Only people carry guns (Phase 5, 2026-10-09, protocol 26):** every wolf, rabbit and draugr
  dropping a loaded gun was too generous. Skyrim flags an actor whose race has `ActorTypeNPC`
  (00013794: the playable races, their vampires and children, dremora; `CR_ACTOR_PERSON`), and
  only its proxy is armed as above. Any other's (in `Skyrim.esm`: animals, draugr, falmer,
  skeletons, trolls, spriggans, giants, dragons, atronachs, Dwemer automatons) carries no gun and
  1 or 2 grenades of a type at random (`CHIEFRIM_CREATURE_GRENADES`). Offline (`--actor-shape
  wolf --actor-dies-at`): a creature's proxy with no gun and 1 frag grenade; a person's with a
  plasma pistol and 4 plasma grenades. Explosions (`CR_MSG_EXPLOSION`, a `damage.c` hook; only those that push objects in Halo) throw
  Skyrim's loose dynamic bodies up and away, up to `fPropLaunchSpeed` m/s at the centre. In Chiefrim mode the level's background loops and acoustics are
  off (`scenario.c` hook): Skyrim has its own ambience. `tools/run_phase0.sh` takes `CHIEFRIM_MAP`. Merging tags from several maps is later
  work.

## 6. The player

**Halo is authoritative for the player's position and physics.**

1. Each Halo frame, Chiefrim sends `PlayerState`: the interpolated position and facing (the
   port's `render_interpolation.c` blends the 30 Hz ticks), pose (standing, crouching, airborne),
   on-ground, and the camera (eye, forward, up, vertical FOV).
2. **PlayerPuppet** in Skyrim disables the player's own movement and moves the `PlayerCharacter`
   and its Havok capsule to that position every frame. The player stays a real Skyrim actor, so
   NPC targeting, detection and stealth, trigger volumes, quest location checks and projectile
   hits all keep working.
3. **CameraDriver** forces Skyrim into first person and overwrites the camera with Halo's:
   - the first-person camera state's translation is Chief's eye (crouch and jump included).
     Chief's size comes from Skyrim (`[Chief] fHeight`, sent in the world context): Halo's Chief
     is 0.7 wu (~150 Skyrim units), too tall for the doorways and ledges Skyrim's people pass
     under, so by default his biped definition's collision and camera heights scale to the
     player's own height (~128 for a Nord), and with them his eye;
   - after every `PlayerCamera::Update` (all its call sites are hooked), the camera root takes
     Chief's view direction, once a 3-second check has confirmed which columns of Skyrim's
     camera-root matrix are forward, up and right (until then Skyrim turns it, from the player's
     angles, which already follow Chief);
   - FOV: Halo's observer FOV is horizontal for 4:3 and projected with 0.85 of its tangent (Halo's
     own view is 61.5° as Skyrim measures FOV). `[Camera] fFieldOfView` (default 85) replaces that
     0.85 with tan(85°/2) / tan(base/2), base being Chief's unzoomed camera FOV from his biped tag,
     so his unzoomed view is 85° and zoom keeps Halo's magnification. Halo publishes the vertical
     angle it renders with, and Skyrim's setting follows it. `bUseHaloFov=0` keeps the player's own
     Skyrim FOV;
   - Skyrim's first-person arm and weapon meshes are hidden (only meshes, never nodes), and
     re-checked about once a second for newly equipped ones.

**Deep water — decided 2026-10-06 (§13): Skyrim's swimming.** Halo has no swimming. While
Skyrim's player swims (deep enough that Skyrim swims him: its actor state's swimming flag), the
player is handed off to Skyrim (§11): Skyrim's own controls, camera and arms swim him, Chief
follows where he is and gets no input, and Chief's weapon and HUD are put away until he stands
again.

### 6.1 Chief's armor in Skyrim (Mjolnir port)

Chief's Mjolnir armor is also a Skyrim armor set, for any humanoid race, built by
`tools/mjolnir/build.sh HALO_MAP` from the user's own Halo map and Skyrim (nothing of either
is in the repo): `build/mjolnir/Data/`, and the same packed for a mod manager as
`build/dist/ChiefrimMjolnir-local.zip` (for the user's own game only). Its stages:

- `halo_model.py` reads Chief's model and bitmaps from the map and bakes his shader as the
  Xbox draws it (his campaign colour exactly: change colour C, 87, 103, 37), and his
  reflection cube maps (his visor's is gold);
- `fit.py` fits him to Skyrim's skeleton: joints onto Skyrim's, segments turned and stretched
  onto its bones, his shoulders and torso where Halo has them (bound to Skyrim's joints), his
  boots squashed so his soles meet the ground;
- `armor.py` splits him by the bone each triangle is weighted most to, as Skyrim's armor
  divides a body: helmet (head, visor), gauntlets (forearms, hands), boots (calves, feet),
  cuirass (the rest, with the body's own forearms and calves, as Skyrim's cuirasses carry
  them, so nothing is missing without gauntlets or boots); and writes skinned SE NIFs
  (`nif.py`, which writes vanilla NIFs back byte for byte) for both ends of the weight slider,
  male and female cuirasses, and first person;
- `textures.py` writes them as uncompressed DDS (the colours exactly the bake's);
- `plugin.py` writes `ChiefrimMjolnir.esp` (`esp.py`): an armor and addon per piece, made from
  the user's Skyrim.esm's Daedric ones (races, keywords, stats). The cuirass also covers the
  tail slot (40).

`blender_preview.py` renders each stage headless (`--nif` reads the written NIFs back).

## 7. Input

**Movement (2026-10-05): Skyrim moves, Halo follows.** After two days of making Halo's biped
physics walk Skyrim's meshes (a runtime BSP builder, then floor guards, step assists, land and fall
rules), Chief still caught on edges, bounced up small rises (Halo counts a biped supported only on
ticks its move touches a walkable surface, so on Skyrim's finely bumpy meshes it left the ground
about once a second) and now and then went through. Skyrim's own character controller is made for
those meshes, so by default (`[Movement] bSkyrimMoves=1`) it moves the player: Skyrim's movement,
look, jump, sprint and sneak handlers stay Skyrim's, the plugin sends Halo the player's position,
facing, camera, velocity and ground/sneak flags each frame (`cr_skyrim_player`, protocol 5), and
Halo places Chief there each frame, aimed along the camera, with no movement or look input and no
jump (crouch still follows sneak). The camera is Skyrim's, with Chief's field of view (zoom) and
his arms hidden. Halo's collision is then for shots, grenades and physics objects. The Halo-driven
mode (everything below about Chief's movement and the collision safeguards) stays behind
`bSkyrimMoves=0`. Halo's movement feel (speed, jump height) is to be approximated in Skyrim's
values later.

**Skyrim's own controls drive Chief** (decided 2026-10-04, replacing SkyCraft's raw-key
passthrough). The Skyrim window keeps focus, and Skyrim's input system does what it always does:
it maps keys, mouse and gamepad to **user events** through its ControlMap, including the user's
remaps from the in-game Controls menu and `ControlMap_Custom.txt`. Chiefrim turns those user events
into Halo actions.

**Rule: Chiefrim never reads a raw key for anything Skyrim has a binding for.** It reads only the
user event (`ButtonEvent::QUserEvent()`), so rebinding a Skyrim action rebinds the matching Chief
action with it. For example, with movement on the arrow keys, Chief moves with the arrow keys. The
only raw keys are Chiefrim's own two hotkeys.

### Skyrim side (InputBridge, in the plugin)

- An input event sink on `BSInputDeviceManager` sees every event with its user event already
  resolved for the gameplay context. Movement comes from `forward`/`back`/`strafeLeft`/
  `strafeRight` and the `move` stick. Look comes from the `look` events (mouse motion, right stick),
  scaled by Skyrim's own `fMouseHeadingSensitivity` and inverted when Skyrim's invert-Y is on.
- **Crouch follows Skyrim's sneak state.** Skyrim's Sneak handler stays on, so Skyrim's stealth
  works, and however Sneak is configured (toggle or hold), Chief crouches exactly while the player
  sneaks.
- While linked and in gameplay, Skyrim's own `PlayerControls` handlers for the actions Chief
  takes are switched off: movement, look, sprint, ready weapon, auto-move, toggle run, run, jump,
  shout, attack/block and toggle POV. They're switched back on when the link closes. Activate
  and Sneak stay on.
- Menus open, dialogue, loading screens, console: routing is `Skyrim`, Halo gets neutral input,
  and Skyrim gets everything as usual. The plugin publishes neutral input the moment any menu
  opens (PlayerCharacter::Update doesn't run while Skyrim is paused), and Halo treats input older
  than 150 ms as none, so Chief never keeps walking on a stale key.
- **Halo's prompts name Skyrim's keys** (protocol 12). Twice a second the plugin names the key
  each of Chief's actions is bound to (Windows' key names; mouse and gamepad buttons by their own
  names), for the device the player last used, and sends them when they change
  (`CR_MSG_KEY_NAMES`). hud_messaging.c writes the name where Halo would draw its Xbox button:
  "Hold E to swap for" (offline, with the fake Skyrim's names).

### Default mapping (`Chiefrim.ini`, `[Controls]`)

| Halo action | Skyrim user event (default key) | Notes |
|---|---|---|
| Move | Forward / Back / Strafe Left / Strafe Right, Move stick | |
| Look | Look (mouse, right stick) | Skyrim's sensitivity and invert-Y |
| Jump | Jump (Space) | |
| Crouch | *Skyrim's sneak state* (Sneak, Ctrl) | Stealth keeps working |
| Fire | Left Attack/Block (left mouse button) | |
| Zoom | Right Attack/Block (right mouse button) | |
| Reload | Ready Weapon (R) | |
| Throw grenade | Shout (Z) | |
| Melee | Toggle POV (F) | |
| Switch weapon | Zoom In / Zoom Out (mouse wheel) | |
| Pick up / swap weapon | Activate (E) | Also still Skyrim's Activate |
| **Switch grenade** | **Chiefrim hotkey** `iSwitchGrenadeKey` (default G) | A raw key, Chiefrim's own |
| **Flashlight** | **Chiefrim hotkey** `iFlashlightKey` (default V) | A raw key, Chiefrim's own |

Each Halo action's Skyrim user event can be changed in the ini, by its ControlMap name. At start-up,
and when the Controls menu closes, the plugin logs the key each action is currently bound to. It
also warns, in the log and as a corner message, when a Chiefrim hotkey is also bound to a Skyrim
gameplay action.

### Halo side

Halo's port already reduces input to an abstract per-player state (`struct game_input_state` in
`source/input/input_abstraction.c`): ticks held per game control, forward and strafe, plus direct
mouse aim in radians (`player_control.c`). In Chiefrim mode, `chiefrim_input.c` fills that same
state for local player 0 from the link's input slot. Hooks in `input_abstraction.c` cover the
buttons, movement and reload, and a hook in `player_control.c` covers aim, with no magnetism, as
for the port's mouse. Halo's own keyboard still works for testing (`tools/launch_halo.sh`).

**The link carries actions, not keys:** the protocol's `cr_input` (v2) holds a bit per held
action, a press counter per action (so a tap shorter than one Halo frame isn't lost), forward and
strafe, and running yaw and pitch totals (so look motion isn't lost or doubled between the two
games' frames).

## 8. Combat

**Built (2026-10-05), to be tried in game** (`halo/src/chiefrim_combat.c`, `skse/src/Combat.cpp`,
protocol 8):

- Skyrim lists the 48 nearest living actors within ~5300 units each frame (the `actors` slot).
  Halo keeps a proxy for each: the host map's armoured marine (`characters\marine_armored`, found
  by name; Chief's own biped if none), scaled to the actor's height, on the Covenant's team (so
  Chief's hits count in full, the motion tracker shows them, and aim assist works), never drawn
  (a hook in `render_object_list`: hiding objects Halo's way would also stop collision and splash).
- A proxy has 100,000 vitality, all body. Each frame, what it lost is divided by its biped's own
  vitality (shields and body) and sent (`CR_MSG_HIT_ACTOR`); the proxy is refilled. Every weapon,
  headshot multiplier, melee and splash is Halo's own: 3 s of MA5B fire at 400 units landed 14 hits
  of 0.10 (offline test).
- Skyrim applies it level-scaled (§13) through its own hit processing (found by the call the
  melee handler makes), and starts combat. Big hits (half a proxy or more) stagger. On the
  author's 1.6.1170 the direct call wasn't found (nor by SkyCraft): another plugin likely hooks
  that call, so it leads out of Skyrim's code. The plugin now follows the call at the melee
  handler's +0x4A8, and when it leads to another module (logged by name) it still uses the hit
  processing; bullets add the flinch below. `bSkyrimHitProcessing=0`, or a call into some other
  part of Skyrim, keeps the piece-by-piece hit:
  damage, then a stagger away from Chief (`staggerStart`) or a flinch (`recoilStart`, at most
  once a second per actor), then a `TESHitEvent` for scripts. Either way an Iron Dagger's impact
  set gives the blood and the hit sound.
- Explosions (Halo's area damage: grenades, rockets, the fuel rod) throw and burn. damage.c's hook
  marks a proxy an explosion reached, and its hit goes with `CR_HIT_EXPLOSION` and the blast's
  centre (protocol 11). Skyrim knocks the actor down away from it first (`AIProcess::
  KnockExplosion`, the ragdoll Skyrim's own explosions use), with `fBlastForce` ×0.5 to ×1.5 by the
  hit's size, so one it kills flies too, then sets them alight: `FireFXShader` (0x1B212) for
  `fBurnSeconds` and `fBurnDamage` of their health over that time (fire resistance counts). An
  explosion hit doesn't stagger or flinch. Offline: a frag grenade 3 m past the actor sent its
  hit with the blast's centre.
- The player is essential while linked; each frame the health Skyrim took is refunded and sent
  (`CR_MSG_PLAYER_HURT`, as a fraction of Chief's vitality: damage ÷ `fIncomingReference`, with
  the attacker's position and the kind from the last hit event). Halo applies it with
  `object_cause_damage` and a damage effect of the map's: the MA5B's melee for melee, its bullet
  for arrows and the rest, the plasma rifle's bolt for magic. Shields take it first and recharge
  as ever; the HUD shows where it came from. Unlike the plan, Skyrim's health stays full rather
  than following Chief's body: lowered, the essential player would kneel in bleedout.
- Chief is deathless in Halo while linked, and Halo's telefrag (a player blocked inside another
  unit for 3 s dies) is off: in a fight Chief stands inside Skyrim's people's proxies, and it
  killed him, bypassing deathless (the fourth in-game test); the campaign death then reverted the
  game to its checkpoint, before the level was cleared, and b30's trees and Covenant scenery
  floated around the player. Any loss of Chief's unit (dead, gone, another) now counts as a death
  for Skyrim, and a changed unit (a respawn or revert) clears the level again and forgets the
  proxies (their indices mean nothing after a revert). A proxy that dies (a headshot kills a marine
  outright) sends a whole proxy's hit, at most, and is replaced. With his body at 0 Skyrim is told
  (`CR_MSG_PLAYER_DIED`) and kills its player (killer: the last attacker). A new world (the
  reload) makes him whole and clears the proxies.
- Chief's aim follows Skyrim's view only to 85 degrees up or down: Halo asserts beyond 85.5 (the
  second in-game test crashed looking down at a weapon to pick it up). The view itself is Skyrim's.
- Picking up and swapping weapons is Skyrim's Activate (`sAction`); Halo's prompt still names the
  Xbox button ("X"): to show the Skyrim key, later.
- Debug (§8.4): `[Controls] iGiveWeaponKey` (F7; F9 is Skyrim's Quickload) gives Chief the map's next weapon (`CR_MSG_GIVE_WEAPON`;
  vehicle guns skipped; a dropped weapon resting on a surface past 32,767 crashed Halo, which keeps
  that index in a short: now it rests on no surface in particular, `items.c`; b30 has the MA5B, M6D, plasma rifle and pistol, rocket launcher, needler,
  fuel rod and energy sword; the list is in Halo's log), dropping the one in hand if he has two.

### 8.1 Skyrim NPCs inside Halo: proxy bipeds

For every Skyrim actor within ~25 wu (about 75 m), Chiefrim spawns a **proxy**: a biped from the
host map, **not rendered**, flagged so the AI never runs on it, and moved to the actor's position
and facing every tick.

- **Hitbox (Phase 5, protocol 18, `skse/src/Hitbox.cpp`):** a proxy is hit on **its actor's own
  hit shapes**, not on its biped. Skyrim has rigid bodies on each actor's skeleton bones (layer
  BIPED), but puts them in its physics world only now and then: in furniture and some idles, hit
  or staggered, ragdolled (`bAddBipedWhenKeyframed`, off in vanilla). The first in-game test
  (2026-10-08) found most people walking about with all their bodies out of the world (stale), so
  they fell back to their bounds, and the full shapes only while in an animation. So the plugin
  places a body **out of the world from its bone**, as the animation has the bone that frame, and
  the body's own offset from it (a `bhkRigidBodyT`'s rotation and translation); a body in the world
  is where Havok has it (what Skyrim's own hits meet), and the first of those are logged against
  where their bones put them (`hitbox: <bone> ..., in the world: N units ... from where its bone
  puts it`; over 8 units says the bone placement is off). Precision (Nexus 72347) solves the same
  thing with a scaled clone of the skeleton in Havok, driven from the bones; Chiefrim needs no
  physics for it, as the shapes only go to Halo. The plugin reads them each frame (capsules and spheres as they are; a box or hull as the capsule along its longest
  side) and sends them with the actor (`cr_actors.hitboxes`, up to 48 an actor, 1024 in all).
  Halo tests them in place of the biped's collision model: shots and melee in
  `object_test_vector` (`collisions.c` hook: a ray against capsules), the proxy's bounding sphere
  holds them (`objects.c`: rays and explosions look for it there, so a dragon is as big as a
  dragon), and an explosion's distance is to the nearest shape's middle (`damage.c`: a grenade by
  a dragon's tail hurts it). What sticks (a plasma grenade, a needle) is attached to the biped's
  root node, so it goes where the proxy goes (in the first in-game test, 2026-10-08, no node halted
  Halo: `object_has_node`), and lets go when the proxy is deleted, to go off as it would. Verified
  in game (2026-10-08): bullets and the hit shapes as expected. A wolf is hit where a wolf is: a shot over its back misses, where the
  old scaled biped stood up into it. An actor with no bodies, or with them not where it stands,
  gives way to **one capsule from its bounds**, stood up or laid along its
  heading, as tall as the actor, and a sphere for a person's head (`CR_HITBOX_BOUNDS`). With no
  shapes at all (the pool full) the proxy's biped, scaled (below), is hit as before.
  **Headshots** are a person's: the body on `NPC Head [Head]` (people, draugr, falmer) is
  `CR_HITBOX_HEAD`, and a bullet that can headshot within 1.25 of its radius kills the actor. A
  creature's head is an ordinary hit: one pistol round doesn't drop a dragon. Offline
  (`fake_skyrim.py --actor-shape wolf|person`): a wolf hit on its head, body and legs aiming
  down at it, never aiming level over it (the biped took every round there); a person's head
  shot with the pistol is a headshot; a grenade by the wolf reaches it. `CHIEFRIM_SHOW_PROXIES`
  draws the biped; the console's `chiefrim shapes` draws the shapes (§11). The plugin's log names the first actors' shapes
  (`hitbox: <name>: N shapes from M of its bodies`, or `from its bounds`). Verified in game
  (2026-10-08): people, a giant, a horse, a mammoth and a dragon.
- **Destructible objects (Phase 5, 2026-10-08, protocol 23).** Skyrim's spider webs that close
  passages are activators with destruction data (`FXspiderWebKitDestructible` and its kin in
  `Skyrim.esm`: 10 health); in Halo's collision they stopped shots like walls, unharmed. Now the
  plugin leaves a destructible object not yet destroyed out of the collision it sends
  (`TESHavokUtilities::FindCollidableRef`, the base's `BGSDestructibleObjectForm` with health),
  so Halo's bullets, plasma and grenades go through, and Halo reports each projectile's way each
  tick (`CR_MSG_SHOT`, from `projectile_collision_test_line`). Skyrim casts it through its own
  world (projectile layer) and, if the first thing on it is a destructible, damages it with
  `TESObjectREFR::DamageObject`, which Papyrus's `ObjectReference.DamageObject` ends in (its stages
  and effects; destroyed at no health). Not through Papyrus: the first in-game test dispatched the
  method for each hit on a web and nothing happened, as a web has no script and so no VM object to
  call it on:
  `fObjectDamage` (10, a web's whole health) a shot. An explosion damages those in its reach,
  `fBlastObjectDamage` (50) at its centre to nothing at its edge. Decorative cobwebs (statics) have
  no destruction data and are untouched. `[Combat] bShootThroughDestructibles`. Offline: the
  pistol's rounds reported as 2304-unit ways from the muzzle. In game (2026-10-08): shots go through
  webs and break them (through the native call).
- **Traps (Phase 5, 2026-10-08).** Skyrim's traps are scripts that react to a hit (Papyrus's
  `OnHit`, from `Scripts.zip`): a hanging oil lamp (`TrapFallingOilLamp`) falls, a tripwire, a
  rigged beam or a hinge trigger goes off, on any hit; an oil pool (`TrapOilPool`, after
  `TrapExplosiveGas`) and trapped gas burn on a hit by a flame (a weapon in `TrapGasWeapon`, with
  `MagicDamageFire`, or Skyrim's torch, `Torch01`, 0001D4EC). Now the first thing on a shot's way
  (`CR_MSG_SHOT`, above), and everything in an explosion's reach, if it has a script of its own (the
  VM's attached scripts: not every wall a bullet strikes), gets the player's hit as Skyrim raises
  its own (`TESHitEvent` through `ScriptEventSourceHolder`), with the torch as its weapon
  (`bShotsIgnite`): oil and gas catch fire from a bullet, plasma or a grenade. People aren't: they
  are hit through their proxies. `[Combat] bShotsHitObjects`, `bShotsIgnite`. In game
  (2026-10-08): lamps fell and tripwires went off; oil pools shot directly didn't light. An oil
  pool lights by its own `damageObject(5.0)` (in `gasExplode`) taking it to its burning stage; the
  plugin's own damage before the hit (as on a web) didn't register on it (health 100 -> 100: its
  first stage caps damage). Now a scripted destructible gets no damage from the plugin (it destroys
  itself), and with `bShotsIgnite` scripted objects also get a fire effect applied
  (`TESMagicEffectApplyEvent`: Papyrus's `OnMagicEffectApply`, with Firebolt's `FireDamageFFAimed`,
  00012F03, in `TrapGasOnMagicEffectApply` and with `MagicDamageFire`), as Flames lights one.
  Verified in game (2026-10-08): webs break, lamps fall, tripwires go off, oil pools light when shot.
- Because proxies are real Halo objects, **Halo's own code** handles bullets, plasma, needler
  supercombines, grenade splash, melee, headshots and knockback impulses.
- Each proxy carries the actor's FormID and hostile, essential, dead, attacking and sneaking flags.
- **Motion tracker (Phase 5, protocol 20):** a proxy is a blip as Halo makes one: red if its actor
  is hostile to the player (`IsHostileToActor`), yellow if not (friends, followers, townsfolk),
  while it moves or attacks (`CR_ACTOR_ATTACKING`: a swing, a drawn bow, a spell being cast, as a
  unit firing shows standing still). The proxy is placed, with no velocity of its own, so Halo
  measures its actor's speed from where Skyrim puts it (over 0.2 s, halving through a window
  Skyrim didn't move it in). Halo's threshold (~90 Skyrim units a second) is above a Skyrim walk
  (~80), so a proxy shows from 40 units a second; only a sneaking actor (`CR_ACTOR_SNEAKING`,
  Halo's crouch) has to beat Halo's own. The proxy's team stays the Covenant's (damage and aim
  assist as before): `motion_sensor.c`'s hooks change only whether it shows and its colour.
  Offline (`fake_skyrim.py --actor-walk`, `--actor-friendly`, `--actor-sneaks`,
  `--actor-attack-at`): a hostile walker red, a friendly one yellow, a sneaking one and one
  standing still not shown, the one standing still shown red while it attacks. Verified in game
  (2026-10-08).

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

v1 ships (a) with a debug command to spawn any weapon (the F7 key, and the console's
`chiefrim give <name>`, §11). (c) is later work.

**(b) Weapon caches (Phase 5, 2026-10-08, protocol 24, `skse/src/Caches.cpp`,
`halo/src/chiefrim_caches.c`).** Halo weapons lying in Skyrim's world: Halo objects, not Skyrim
items, so no Skyrim plugin (esp) or assets. Halo lays each one down in its world (drawn in the world
layer, lit by Skyrim's light, on Skyrim's surfaces through the collision BSP) and Chief picks it up
as in Halo.
- **Sites:** a location (or one it's in) with `LocTypeBanditCamp`, `LocTypeForswornCamp` or
  `LocTypeMilitaryCamp` is a camp, with `LocTypeMilitaryFort` a fort. The first visit decides
  whether it has any (`fSiteChance`, 0.5).
- **Which and what:** the first time each cell of a site with caches loads, its chests (a container
  whose model's path has "chest") and weapon racks are shuffled and each gets one by `fChestChance`
  (0.35), up to `iMaxPerSite` (3) in the site: a light weapon (`sLightWeapons`: pistol, plasma
  pistol, needler, assault rifle), or in a fort a medium one by `fMediumChance` (0.3; shotgun,
  plasma rifle). A fort's boss chest (location ref type `BossContainer`, 000130F8) gets a heavy one by
  `fHeavyChance` (0.6; sniper rifle, rocket launcher, shotgun, plasma rifle: Halo refuses Chief the
  fuel rod and the flamethrower, which the list first had, and a ruin's fuel rod lay nowhere; the
  shotgun and plasma rifle are medium too, and a boss chest's counts as heavy for its ammunition); a
  fort's other chests stop one short of the limit to leave it a place. **Dungeons** (2026-10-09:
  heavy weapons in forts alone were too rare): a location with `LocTypeDungeon` (Nordic and Dwemer
  ruins, caves; a camp or fort in one is that) always counts, but only its boss chest, and only for a
  heavy weapon, by `fRuinHeavyChance` (0.2) in a Nordic or Dwemer ruin (`LocTypeDraugrCrypt`,
  `LocTypeDwarvenAutomatons`) and `fDungeonHeavyChance` (0.15) in any other. Verified in game
  (2026-10-09): Hillgrund's Tomb, its boss chest chosen in its last cell. A cache whose weapon is in
  no list (one chosen before a list changed) is given another of its tier when it's next sent.
- **Scarce ammunition (protocol 25):** a cache's weapon comes with its magazine loaded and a share of
  the spare rounds one in the map has (`cr_msg_cache_place.spare`, by its tier: `fLightSpare` 0.5,
  `fMediumSpare` 0.25, `fHeavySpare` 0); an energy weapon's battery is spent by 0.6 of what it falls
  short. Offline: a needler 20/20 at 0, 20/50 at 0.5, 20/80 (Halo's own) at 1. Chance is seeded by the
  playthrough (a seed in the co-save), the site and the cell.
- **Kept:** the co-save's `CACH` record has the seed, every site seen (with or without caches), every
  cell chosen, and each cache (its chest, cell, site, weapon, taken), written as they are chosen
  and read back with form IDs resolved (a plugin gone takes its caches). So a cache never moves, a
  site never rolls again, and a taken one stays taken.
- **Laid down:** the untaken caches within `fSendRadius` (2500, about the collision's reach) go to
  Halo (`CR_MSG_CACHE_PLACE`), every 5 s, on the floor beside the chest (2026-10-09: on its lid it
  relied on Halo's collision having the lid, and in game a plasma rifle fell inside a chest, found
  only by its prompt): the plugin tries 8 points around it at its bound's reach, the player's side
  first, and takes the first with nothing but the chest between it and the chest's middle (Skyrim's
  physics, cast from outside) and floor under it within 40 units of the chest's foot (a wall rack:
  any floor below); for the world Halo is in
  (the generation; a new world erases Halo's loose objects, and the next sends lay them down
  again). Halo drops each onto what lies below once its collision has it, keeps one per cache a
  world, and reports it taken (`CR_MSG_CACHE_TAKEN`) when Chief holds it.
Offline (`fake_skyrim.py --cache NAME`, `--cache-north`, `--action-at`): a shotgun laid on the
ground 150 units ahead, once a world across two recenters; laid at Chief's feet, picked up (into
his free slot) and reported taken once. Verified in game (2026-10-09): Fort Greymoor rolled none;
Broken Tower Redoubt (a Forsworn camp) rolled caches, its interior a plasma pistol by a chest, laid
down at once, laid again after a teleport, taken by a swap, saved taken, and not laid again after a
load. The log names each site's decision, each cell's chests and racks and the caches chosen, and
each one taken. Heavy weapons (a fort's boss chest): to check. White River Watch (a bandit camp):
its cells logged 1, 0, 1 and 1 (the boss's) chests, two needlers chosen; reloading saves from before
it chose the same again (the playthrough's seed). One needler, by a chest up its watchtower, was
laid on the tower once and on the ground below twice: Halo looked 4 world units down for something
to lie on, and before the tower's collision arrived found the ground. It looks 1 (~210 Skyrim
units) now, and waits for the chest's own. The log also names each location the player enters and
whether it's a camp or fort (and its decision), for a fort that logged nothing.

## 9. Rendering

Skyrim renders the world. Halo renders **only its own things**, offscreen, at Skyrim's resolution,
using the camera Skyrim is about to use. It draws three layers:

| Layer | Contents | Composited |
|---|---|---|
| **World** | Projectiles, tracers, plasma, explosions and particles, grenades in flight, weapons lying on the ground, muzzle flashes. No structure, no sky, no fog, cleared to transparent. | During Skyrim's frame, **depth-tested against Skyrim's depth buffer** |
| **First person** | Chief's arms and weapon (`first_person_weapon_render_update`), with Halo's fire, reload and melee animations | After Skyrim's scene, before its HUD |
| **HUD** | Halo's HUD (`hud_draw_screen`): shields, health, ammo, grenades, motion tracker, crosshair, and any open Halo screen | On top of everything |

**Phase 2 (done 2026-10-05): the first-person and HUD layers, as one picture.**

- **Halo side:** while Skyrim asks for the overlay (the `display` slot, below), `render_window`
  draws only the first-person weapon (`render_objects` skips every other object), its transparent
  parts, the HUD (`interface_draw_screen`), the screen flash and Halo's UI widgets. Halo's zoom
  screen effect (`interface.c`: every weapon's zoom blur and mask, desaturation, the sniper's night
  vision) is skipped: it redraws the whole screen from Halo's picture, which here is empty, and
  covered Skyrim with black (the first in-game test, zooming the pistol). Scope masks are HUD
  bitmaps and still show. Night vision and desaturation, if wanted, belong on Skyrim's picture. No sky,
  structure, decals, particles, fog, lens flares or mirrors. The port's screen takes Skyrim's
  shape: 480 lines and as many columns as the shape gives, its targets at Skyrim's pixels (at most
  2560x1440; a bigger screen gets a smaller picture, scaled up).
- **Coverage:** the picture's alpha can't say what Halo covered (the game uses destination alpha
  as scratch). So the back buffer's framebuffer gets a second target: every pixel shader also
  writes (1, 1, 1, alpha) there, and each draw's blend becomes what that blend does to coverage
  (opaque: 1; alpha blend: mixed by alpha; additive: unchanged, as light covers nothing;
  modulating: unchanged). The picture is then premultiplied over black: Skyrim draws it with
  (ONE, INV_SRC_ALPHA). A resolve pass makes RGBA from the two (`halo/src/port/chiefrim_overlay_gl.c`,
  hooked into the port's `d3d8_gl.c` and `nv2a_psh.c`).
- **Skyrim side** (`skse/src/Overlay.cpp`): a hook on the swap chain's Present (vtable, so it
  chains with SSE Display Tweaks') sends the screen size each frame, uploads Halo's newest frame
  into a dynamic texture and draws it over the picture, only in gameplay (no menu, loading screen
  or console; the same test as input routing) and only while Halo keeps publishing (hidden after
  500 ms without a frame). `[Overlay] bEnabled` turns it off. **Under Skyrim's HUD (Phase 5,
  `[Overlay] bUnderSkyrimMenus`):** the layers are drawn as the frame's first Skyrim menu draws
  (`IMenu::PostDisplay`, vtable slot 6, patched per menu class as each opens; menus draw lowest
  first), so the HUD's prompts, subtitles, compass and notifications, mods' widgets and any menu
  are over Halo's weapon and HUD. Present draws them in a frame no menu drew in. Verified in game
  (2026-10-08): a Talk prompt and subtitles over the MA5B. The HLSL is compiled at start
  (`d3dcompiler_47`): checked under Proton Experimental with DXVK, where its blend gives the
  expected pixels for opaque, half-covered, additive and empty texels.
- **Still to come:** the zoom screen effect's tints on Skyrim's picture (night vision); checking
  the sniper's and rocket launcher's scopes in game (b30 starts Chief with the MA5B and M6D only:
  needs §8.4's weapon spawn command); the layers apart (the weapon under Skyrim's HUD, not over its compass and
  messages), the world layer with depth (Phase 3), hiding Skyrim's own crosshair and bars, and
  object lighting from Skyrim (both done in Phase 4, below).
**Phase 3 (done 2026-10-06, verified in game): the world layer, in lockstep.**

- **Two layers.** In overlay mode `render_window` draws the **world layer** first (every object
  but the first-person weapon, decals, particles, contrails, transparent geometry), keeps it
  (`chiefrim_overlay_world_done`), then the **screen layer** (first-person weapon, HUD) on a cleared
  picture. The host level's objects (scenery, vehicles, weapons: 898 on b30) are erased with its
  actors, or they'd stand around Skyrim's origin. The host map's fog is off.
- **Transmittance, not coverage.** The second target now keeps how much of Skyrim's picture shows
  through (T, sent as alpha = 1 - T): a blend `S * src + D * dst` turns T into `D * T`, plus
  `src * T` where S is the destination's colour. So modulating draws darken Skyrim's picture too:
  Halo's bullet holes are 2x-modulate decals, and showed nothing with coverage.
- **Depth.** A third target keeps each world pixel's view distance: the pixel shaders write
  `1 / gl_FragCoord.w` (the clip w, the distance along the view) where they show anything,
  min-blended. Skyrim copies its main depth buffer as `Main::RenderWorld` returns (before the HUD
  and post-processing) and, at Present, drops world pixels behind its own, with a little slack for
  decals on its surfaces (0.3% + 4 units). Its convention (standard or reversed) is read once from
  the depth itself (the median is near 1 or near 0).
- **Lockstep.** As `Main::RenderWorld` starts, Skyrim publishes the camera it renders with (the
  world root camera: eye, forward, up; the `camera` slot). Halo, which in overlay mode no longer
  swaps its own window (whose vsync would pace it), waits for each new camera (30 ms at most), draws
  through it (`chiefrim_render_camera` in place of the observer's: Skyrim's eye, forward and up,
  Halo's field of view, which Skyrim's follows), reads both layers back at once and names the
  camera in the frame. At Present Skyrim waits up to `[Overlay] fWaitMs` (12) for that frame, so the
  world layer sits on this frame's picture rather than one or two frames old.
- **Reprojection (protocol 9).** The first in-game test showed the lockstep mostly missing:
  Halo's frame of the moment was ready in time for ~20% of frames (894 of 1078 waits ran out its
  12 ms), so the world layer showed a camera a frame or two old (decals slid when turning) and the
  waits cost frame rate (~33 fps). Now each frame carries Halo's projection (`tangent_x/y`, from
  its frustum), Skyrim keeps its last 64 cameras, and the world-layer shader maps each pixel's view
  ray from the camera of the moment into the camera and projection Halo drew through (depth
  compared along the ray). Exact for turning, and for any field of view mismatch; walking leaves a
  frame's parallax. Skyrim no longer waits (`fWaitMs` 0); Checked under Proton with DXVK: a marker
  at the centre of Halo's frame lands at column 69-70 of 160 after a 10 degree turn (70.2 expected).
- **Checked offline** (fake Skyrim, `--fire-at`, `--pitch`, `--speed 0`): frames name the camera
  they were drawn through; muzzle flash, smoke, sparks and bullet holes are in the world layer; a
  camera 120 units up looking 60 degrees down puts the holes at 123-146 units (120 / sin 60 = 139).
  Under Proton with DXVK, the composite shows world pixels in front of Skyrim's surface, on it and
  against the sky, and hides those behind.
- **Halo side, later layers:** `render_window` (`source/render/render.c:302`) gets a Chiefrim mode that skips
  `render_sky`, the structure lightmap and visibility passes, and the parts of the world Skyrim
  already draws.
- **Lighting:** with no lightmaps, object lighting comes from Skyrim. `WorldContext` carries the
  sun direction and colour and the ambient light, from Skyrim's weather and time of day.
- **Transport (v1, CPU):** Halo reads color and depth back into the shared memory frame slots
  through asynchronous PBOs (Phase 2: two, a frame behind), and the plugin uploads them into D3D11
  textures. At 1080p that is about 8 MB per layer per frame (16 with depth). The slots (three) are
  written round-robin, each under its own seqlock, so neither side owns one and either can restart;
  a copy that sees its slot change is torn and dropped. GPU sharing between Skyrim's D3D11 (under
  DXVK on Vulkan) and Halo's native OpenGL is a later optimisation through Vulkan external memory,
  and only if the CPU path is too slow.
- **Which GPU Halo uses (2026-10-06):** on a two-GPU laptop Halo has drawn on the integrated GPU
  (Intel UHD 630), Skyrim on the discrete one (GTX 1050) through DXVK. `CHIEFRIM_HALO_GPU=dgpu`
  (`tools/launch_halo.sh`) puts Halo on NVIDIA's through PRIME render offload: Halo is 32-bit and
  NVIDIA's 32-bit EGL can't open a Wayland display, so it goes through XWayland's GLX. Offline at
  1080p against the fake Skyrim, the overlay frames are identical; Halo alone runs ~145 frames/s
  on the Intel GPU and ~195 on NVIDIA's. In game it shares NVIDIA's GPU with Skyrim and reads its
  layers back over PCIe instead of from shared memory, so which is better is for the in-game
  comparison. In game (2026-10-06) the GTX 1050 gave a much higher frame rate, so it is the
  default (`auto`: when NVIDIA's 32-bit GLX is installed). A Halo window opening over a running
  Skyrim on the same GPU froze Skyrim until it was minimized, so the launcher always starts Halo's
  window hidden (`HALO_HIDDEN_WINDOW`; it renders the same, ~190 frames/s offline), restarts
  included; `CHIEFRIM_HALO_SHOW_WINDOW=1` shows it. Both on one GPU is also what the GPU-shared
  path above would need.
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


**Phase 4 (started 2026-10-06): Skyrim's HUD and light.**

- **Skyrim's HUD.** Halo's HUD has the crosshair, shields and health, so while Chief is linked the
  plugin hides Skyrim's crosshair and health and magicka bars (`hudmenu.swf`'s
  `HUDMovieBaseInstance.CrosshairInstance`, `Health`, `Magica`: `_visible` false each
  frame, as Skyrim's HUD shows them again on every mode change) and puts them back when the link
  closes. The stamina bar stays (Phase 5, 2026-10-08): Skyrim's sprint, which moves the player,
  spends it. The compass, sneak eye, activate prompt, enemy health bar and notifications stay.
  `[HUD] bHideCrosshair`, `bHideBars`, `bHideStamina` (0).
- **Skyrim's light on Halo's objects** (protocol 13). Halo lit objects from its map's lightmap under
  them, which Chiefrim's collision BSP doesn't have: every object had the host map's default
  light. Now ~10 times a second the plugin sends `CR_MSG_LIGHTING`: Skyrim's directional ambient
  (`BSShaderManager::State::directionalAmbientTransform`: its translate is the average, its z
  column what faces up adds), its key light (the scene's `sunLight`: sun or moon outside, the
  cell's directional light inside, pointed down), and the 4 point lights nearest the player
  (torches, fires, spells: the scene's active lights). object_lights.c's hook lights each object
  from them at its own position (chiefrim_lighting.c): ambient, the key as distant light 0, the
  strongest point light (or the sky's light from above) as distant light 1, the others into the
  ambient; reflections and shadow as Halo derives them from a lightmap. Objects' lighting is
  refreshed every tick and blended toward it every tick (Halo did both only for moving objects:
  the first in-game test kept the gun dark a minute after leaving an interior), at Halo's own step
  (0.03 a tick): dark to daylight in about a second. Offline, standing still: brightness 11, then
  79 a second after the light changes. Offline: the MA5B in a 0.05 scene against a 1.0 scene, mean weapon brightness 52
  against 79 (its ammo counter glows by itself). `[Lighting] bEnabled`, `fBrightness`, `fPointLights`.
- **Torches, braziers and other lights (Phase 5, 2026-10-08).** In game they hardly lit Chief's
  weapon, for two reasons. The plugin placed each light by `BSLight::worldTranslate`, which is
  relative to the camera (Skyrim renders about it): outside every light seemed far away (the log:
  0 point lights near), inside one passed now and then. Each light is now placed by its node
  (`NiLight::world.translate`), and the four sent are those lighting the player most (strength by
  falloff), not the nearest. And Halo's shiny weapons (the MA5B) show mostly their reflection, which
  the hook drove by the ambient and the key alone: offline, a torch beside Chief gave 5 where the
  key gave 36. The strongest point light now adds to the reflection's strength and tints it (the
  sun's tint as it was, so daylight is unchanged). Offline (`fake_skyrim.py --torch
  dx,dy,dz,reach,r,g,b`), in the dark: the MA5B 4 alone, 17 to 28 with a torch beside it, the
  barrel orange in its light; daylight 36 and 67 as before. The log names the lights sent when
  their number changes (distance, reach, colour) and how many the scene has on.
- **The shine in the sun's colour (Phase 5, 2026-10-08).** Halo has no sun of its own (its maps
  light objects from lightmaps); Skyrim's sun or moon is already the key light, its direction and
  colour. But the reflection's tint, most of what a shiny weapon shows, was the ambient's size
  (times 2, plus 0.25: Halo's own formula, by its lightmap's colour), which Skyrim's daylight
  clamps to white: an orange afternoon sun (0.63 0.46 0.35, from the log) never coloured the shine.
  Now the tint is the hue of the light reaching the object (the ambient, the key past its shadow,
  a torch), its largest part 1; its strength stays the alpha's. From the logged light: afternoon
  (1.0 0.93 0.80), a blue dusk (0.50 0.86 1.0), a grey day near white. Offline (`fake_skyrim.py
  --sun-color`): the MA5B's barrel white under a white sun, copper under an orange one.
- **Shade dims the shine; a roof dims the sky (Phase 5, 2026-10-08, protocol 22).** In game the
  pistol's slide shone as bright under a building as in the sun, though the log had the sun 0%
  seen: the reflection's strength counted the ambient in full, and Skyrim's daylight sky alone
  (~0.5) made it the most it goes. Its strength is now the light shining on it straight (the
  key, a torch) and a third of the ambient's, as Halo's follows its lightmap's direct light; in
  the open sun it is still the most. And under a roof the sky's light is blocked too: the plugin
  casts five rays up from the eye (straight up and four 45 degrees off it, 3000 units) and sends
  how many are open (`cr_msg_lighting.sky_visible`); Chief's ambient and sky fill go down to 0.4
  of themselves with none open. Halo's other objects cast one ray up through the collision BSP.
  Offline (`fake_skyrim.py --sky-visible`), the M6D: 30 in the sun, 12 in shade, 7 under a roof.
- **Matched to Skyrim's weather (Phase 5, 2026-10-08, protocol 21).** The first in-game look found
  Chief's weapon the wrong colour (none of dusk's warmth or night's blue) and too bright at night
  and in shade. Skyrim's light was there; two things weren't:
  - **The sun's shadows.** The key light reached every object, in a building's shade too. Now,
    outside (an interior's directional light casts none), the plugin casts five rays towards the
    sun or moon from the eye (Havok, line of sight, the player's capsule skipped; the eye and four
    points 12 units around it, across the light's way) and sends how many get through
    (`cr_msg_lighting.sun_visible`, `key_shadowed`). Chief, and what he holds (the first-person
    weapon takes his lighting), get the key light by that much; every other object casts its own
    ray through Chiefrim's collision BSP (`collision_test_vector`, structure, 40 world units:
    past the BSP's reach). Halo's blend of each object's lighting softens the edge. Skyrim's
    physics shapes cast the shadows: buildings, rocks, cliffs, tree trunks; not leaves.
    `[Lighting] bShadows`. Offline (`fake_skyrim.py --sun-visible`, `--sun-visible-to`,
    `--roof`): the MA5B 66 bright in the sun, 30 in shade a second after; a proxy
    (`CHIEFRIM_SHOW_PROXIES`) 43 in the open, 26 under a roof.
  - **Skyrim's grade.** The compositor draws at Present, after Skyrim's post-processing, so Halo's
    pixels missed the image space Skyrim grades its picture with (the weather's or the cell's, with
    the effects over it). Now the compositor grades them as Skyrim's HDR shader does after its tone
    map, from `ImageSpaceManager`'s blended data: saturation, the tint (towards the tint colour
    times the luminance), brightness, contrast (about 0.5; Skyrim's own pivot is a shader constant
    not read here), then the fade. Contrast is mostly off by default (`fContrast` 0.25 of it): Skyrim's (and
    Community Shaders') is made for its HDR picture before the tone map, and in game, applied in
    full to Halo's finished colours, the weapon came out far too contrasty. The log's `grade:`
    lines give Skyrim's numbers: in the first logged session (weather and Community Shaders'
    image spaces) 1.10 inside and 1.38 to 1.50 outside, so 0.25 of it puts the strongest weather
    at about the interiors' own (1.125). The world layer gets Skyrim's fog first, by its distance: the
    weather's (`Sky`: near and far colours, planes, power, clamp) or an interior's (the cell's
    lighting, or its template's where it inherits). The screen layer is graded only where it is
    Chief's arms and weapon, not the HUD: Halo marks, in a fourth target, how much of each pixel's
    colour came from the weapon (written 1, the HUD's draws 0, blended as the colour is), read
    back after the layers (`CR_FRAME_MASK`, a byte a pixel). Offline: the mask is exactly the
    arms and MA5B, none of the HUD; the shader compiles with Proton's `D3DCompile`.
    `[Grade] bEnabled`, `fStrength`, `fContrast`, `bFog`. In game (2026-10-08): the shadows
    follow the shade; the grade's contrast was too much (now 0.25 of it); the rest to check.
  - **Chief's arms and weapon matched to Skyrim's picture (2026-10-09).** The first look found
    them too bright for Skyrim's world, and a fixed toning down (brightness 0.8, highlights rolled
    off) made the next worse: in bright snow (two screenshots, the gun's pixels against the world's)
    half the gun was near black (0.02) and its middle 0.08, its shine no more than 0.72, where the
    world's middle was 0.65 to 0.75, its darkest 1% 0.04 to 0.14 (a cool haze) and its highlights
    0.9 to 1. Halo's are lit by Skyrim's light but drawn after its tone map, without the haze and
    bounce that lift its own shadows. So the compositor meters Skyrim's picture each frame before
    Halo's layers go over it (copied, mipped, read from its mip of 64 texels across at most into two
    texels blended towards each frame's over about half a second, as an eye adapts): its key (the
    log average of its brightness), its brightest (a soft maximum) and its shadows' colour (a soft
    minimum). On the weapon's pixels alone, before the grade: the saturation to
    `fWeaponSaturation` (0.9); an exposure of the square root of the key over 0.3, from
    `fWeaponExposureMin` (0.65: the dark, interiors) to `fWeaponExposureMax` (1.1: daylight), times
    `fWeaponBrightness` (1); the highlights rolled off above `fWeaponHighlights` (0.6) of the
    picture's brightest (0.4 to 1), so no shine is above the sky's; and the darks lifted towards
    `fWeaponShadowLift` (0.35) of the shadows' colour. `bWeaponMatchScene` 0 keeps only the fixed
    numbers. Simulated on the two screenshots: the gun's darks 0.07 to 0.08 in the scene's blue, its
    middle 0.18, its shine to 0.85 to 0.88, under the sky's 0.87 to 0.9. The log's `the weapon's
    look:` lines (every 15 s) give the meter's numbers and the exposure. The shaders compile with
    Proton's `D3DCompile`; the look to check in game, and in the dark.
  - **Reflections no brighter than the surroundings (2026-10-09, protocol 27).** In game, in a
    dim cabin (the meter's key 0.100, brightest 0.91: a sunbeam on the floor; exposure 0.65), the
    MA5B's silver still came out flat at 0.78, where the room's 95th percentile was 0.27 to 0.29:
    Halo's picture was already white there, past what the compositor can tone. A shiny weapon
    shows mostly its reflection, and the reflection's strength was the direct light's (the fire's
    and the window's, times 1.5 plus 0.25: at its most from about half of Skyrim's daylight). But a
    reflection is of what surrounds it. Now the plugin reads the meter back ten times a second
    (`Overlay::Surroundings`; the meter's third texel is the picture's mean colour) and sends with
    the lighting `cr_msg_lighting.reflection_cap`, the key times `[Lighting] fReflectionMatch` (2.5)
    from `fReflectionMin` (0.15) to 1, and `surround_hue`, the mean colour, its largest part 1.
    Halo caps each object's reflection strength by it and tints the reflection by the hue: the
    cabin 0.25, warm (1 0.77 0.57); a snowy day (keys 0.49 to 0.63) uncapped, cool (0.75 0.92 1).
    Offline (`fake_skyrim.py --light 0.5 --torch ... --reflection-cap 0.25 --surround-hue
    1,0.77,0.57`): the MA5B's screen-layer brightness 44 uncapped, 29 capped, its barrel's top 1%
    from 0.74 to 0.59. The log's `lighting: reflections at most` line gives the cap and hue.
    In game (2026-10-09, the same cabin: key 0.098, mean 0.175 0.143 0.106): the MA5B's silver
    from 0.70 to 0.45 (median), shaded again, but still over the room's 95th percentile (0.30) and
    near neutral (1 0.96 0.92) in a warm room (1 0.82 0.61): what's left is its light grey paint
    under Skyrim's neutral interior light. So the exposure goes down to 0.5 in the dark
    (`fWeaponExposureMin`; the key wanted 0.57), and the look shifts the weapon towards the
    picture's mean hue by `fWeaponSceneTint` (0.35; the hue the mean over its luminance, 0.5 to
    1.5), as an eye takes a room's light for white. On the screenshot: the silver about 0.35.
    In game (an autumn forest: key 0.369, mean 0.404 0.425 0.375) the MA5B came out yellow: the
    mean is the colour of what's in view, the leaves', not of the light, and Halo's reflection
    tint multiplied it with the sun's own warmth. The meter's colour is now the light's: the
    picture's mean weighted to its unsaturated parts (exp(-8 x saturation), where saturation is
    (max - min) / max; none under 0.03 bright). Its hue on the three screenshots (64 x 36 blocks):
    the cabin 1.13 0.98 0.86 (warm, 1.25 0.96 0.69 by the mean), the snow 0.90 1.02 1.07, the
    forest 0.93 1.02 0.96 (1.00 1.02 0.85 by the mean).
    **Dropped the same day: no colour from the picture at all.** In game the greyish estimate was
    still wrong where the picture is one colour: looking up into a clear sky (key 0.578, its
    "light" 0.426 0.622 0.700, its shadows 0.397 0.606 0.692) turned Chief's arms and weapon very
    blue, though only the sun lit them; in the forest the shadows' brown washed the darks. The
    picture's colour is what's in view, never reliably the light's, and the light's colour already
    reaches Halo from Skyrim's lights (the sun's, the ambient, torches). So the meter is brightness
    only (one texel: key, brightest, the shadows' soft minimum), the darks are lifted in grey by
    `fWeaponShadowLift` of the shadows' brightness, `fWeaponSceneTint` is gone, and protocol 28
    drops `surround_hue`: the reflection cap stays, and the shadows' brightness the lift takes is at most 0.25
    (looking up, a picture of sky alone has its shadows at the sky's 0.57).
- **Chief's flashlight on Skyrim's world** (Phase 5, protocol 17). Halo's flashlight is a light on
  Chief's biped (d20's `characters\cyborg\flashlight_cyborg`: white, 6 wu = 1280 units, a 45°
  cone, full to 20°), which lights Halo's world, and Chiefrim draws none of it. Each frame Halo
  works out that light as `lights_preprocess_scene` would (`object_lights.c` hook: its colour with
  the integrated light's power, so it fades in and out as in Halo, and its radius and cone) and
  sends it when it changes (`CR_MSG_FLASHLIGHT`). Skyrim's renderer has no unshadowed spot light,
  so the plugin moves a dynamic point light of its own (`ShadowSceneNode::AddLight`) each frame
  to just short of where a ray along the view (Havok, line-of-sight layer, the player's capsule
  skipped) meets something, back by the beam's width there and reaching a little past it, dimmer
  the further the beam carries. It's left out of the light sent to Halo (Halo lights its objects
  with its own). Offline: on, off and the fades, radius and cone as above. `[Flashlight]
  bEnabled`, `fBrightness`, `fReach`. Verified in game (2026-10-08): a pool of light where the
  player looks.
  - **Tried and dropped (2026-10-08): a shadowed spot light. Too buggy and unreliable to
    implement.** A real cone with shadows would look more like Halo's beam, and Skyrim's spot
    lights are all shadowed ones (light forms flagged "spot shadow", 39 in `Skyrim.esm`, most with
    a 90° field of view). Four in-game tries, each with a spot whose cone was a dot smaller than
    Halo's crosshair:
    1. A light form made at run time (`IFormFactory`), flagged dynamic and "spot shadow", Halo's
       reach and a 90° field of view, given to `TESObjectLIGH::GenDynamic` on a node of the
       plugin's at the eye, turned as the camera. Skyrim made a shadowed frustum light
       (`BSShadowFrustumLight`), pointed the right way, but its semi-width was 0.017 (tan 1°): it
       didn't take the form's field of view. Possibly from the reference it was made for (the
       player, with no light data of its own); not confirmed.
    2. The field of view in other units: a vanilla spot's form (`SolitudeInnSpotlightDefaultShadow`,
       90 in the file) holds 90 at run time, so degrees were right. No change.
    3. The frustum light's `semiWidth` and `semiHeight` set to Halo's cone once it was in the scene:
       no change in game.
    4. Those, and the frustums of its shadow map cameras (`ShadowmapDescriptor::camera`), set every
       frame: still a dot.
    How Skyrim builds a spot's cone, and from what, is unknown without reversing `GenDynamic` and
    the frustum light's update, and each guess needs an in-game test. A shadowed spot also takes
    one of Skyrim's few shadowed lights. The code is gone (commit 49414b8); the tries are in
    commits 421552e, eddad72 and 8a0f393.

## 10. Protocol / IPC

**The platform split is the main new problem.** Skyrim runs inside Proton (a Windows process in
Steam's runtime container). Halo is a native Linux process. Windows named shared memory and named
events (SkyCraft's `Local\SkyCraft_v1`) are not visible across that boundary.

- **Transport (v1):** one file, `/dev/shm/chiefrim_v1`.
  - Halo creates it and maps it with `mmap(MAP_SHARED)`.
  - The plugin opens it as `Z:\dev\shm\chiefrim_v1` and maps it with `CreateFileMapping` +
    `MapViewOfFile`. Wine backs that with a shared mmap of the same file.
  - **Verified in Phase 0** (`tools/linktest/`): a Windows x64 peer under Proton Experimental,
    inside Steam Linux Runtime 4, against a native i386 peer. 100,000 ring round trips averaged
    0.38 µs (max 65 µs), with no torn slot reads in about 2.8 million reads either way.
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
| S→H | `Input.routing = Skyrim` (a Skyrim menu, or a hand-off: Skyrim's animation, scene or swimming owns control, §11) | Every frame |
| S→H | `BeginFrame {frameId, viewport, camera}` | Per frame |
| S→H | `ChiefRestore {kit}`: a loaded save's, or the starting loadout; a new Halo gets the latest (protocol 14) | Event |
| H→S | `PlayerState {pos, yaw, pitch, pose, onGround, camera, bodyFrac, shieldFrac}` | Per frame |
| H→S | `HitActor {...}` | Event |
| H→S | `PlayerDied` | Event |
| S→H | `display` slot `{width, height, flags, frame}` (protocol 6) | Per frame (Present) |
| S→H | `camera` slot `{frame, eye, forward, up, vertical_fov, near, far}` (protocol 7) | Per frame (world rendering starts) |
| H→S | `frames`: 3 slots, each the screen layer and the world layer (RGBA8 premultiplied), the world's depth (float) and the screen layer's weapon share (a byte, protocol 21), `{seq, width, height, frame, camera_frame, time_us, flags}` (protocol 7) | Per frame |
| H→S | `MenuState {haloScreenOpen}` | On change |
| H→S | `ChiefState {kit}`: weapons, ammo, grenades, vitality, flashlight (protocol 14) | On change, ≤ 4 Hz |

Message type IDs 0x80–0xFF are reserved for the stretch goals (Covenant, vehicles).

## 11. Other systems

- **Save and load (Phase 5, `skse/src/CoSave.cpp`, `halo/src/chiefrim_inventory.c`):** Chief's
  **kit** goes into the SKSE co-save: his weapons (by tag path, so a save outlives the host map's
  tag indices; one the map lacks is left out), their rounds and an energy weapon's battery, the
  one in hand, his grenades and the type chosen, body and shield vitality, and flashlight charge.
  There is no Halo world to snapshot, which makes this much simpler than SkyCraft's. Halo reports
  the kit when it changes (`CR_MSG_CHIEF_STATE`, 4 Hz at most) and Skyrim keeps the latest, so a
  save made while Halo is off (F10) or restarting (F11, a crash) keeps the last kit. Loading a
  save sends its kit to Halo (`CR_MSG_CHIEF_RESTORE`) once the world has gone (after Halo makes
  Chief whole for the new world), and Chief drops what he carries and takes it; a save without one
  (a new game, a save from before Phase 5) gets the starting loadout of `[Loadout]` (weapons named
  by their tag path's last part, with the rounds a weapon found lying around has; frag and plasma
  grenades; empty `sWeapons`: the host map's own). A new Halo gets the latest kit, or that loadout. Each restore has a generation, and Halo's reports carry the last one
  applied, so reports from before a restore can't overwrite the loaded kit. Test stand:
  `fake_skyrim.py --restore-at`, `--restore-default-at`.
- **Healing (Phase 5):** a restore-health potion or food Skyrim's player consumes while linked
  (its restore-health effects, magnitude times duration) heals Chief's body (`CR_MSG_CHIEF_HEAL`,
  protocol 15), on the scale of Skyrim's damage to him (`fIncomingReference`). His shields
  recharge as in Halo.
- **Load doors, fast travel and teleports:** Skyrim is authoritative for these. The plugin sends
  `Teleport` (and `WorldContext` if the origin changes). Halo moves Chief and clears the
  CollisionField. With the origin moved, Halo's (0, 0, 0) is somewhere else in Skyrim, so what
  lay in Halo's world stays behind (Phase 5, 2026-10-08): weapons dropped, grenades and other
  loose objects are erased (Chief keeps what he carries), and bullet holes and other decals
  expire at the next frame, as an old one does (`decals_expire_all`, a `decals.c` hook). In game
  they had hung where the old collision was. Offline (`fake_skyrim.py --recenter-every`): the two
  weapons Chief dropped and 44 bullet holes gone at the recenter; firing and a grenade across
  three recenters, no decal errors.
  **Two fixes (2026-10-09).** (1) The erase kept what has Chief as its ultimate parent, but a weapon
  he has put away isn't attached to him, only in his slots: a door erased it (a load gave it back,
  from the co-save). Now everything in his weapon slots stays. (2) Halo halted going into a fort's
  interior (`items.c`: "#17185 is not a valid index in [#0,#4)": an item resting by its surface's
  index in a BSP swapped out, looked up in the stand-in floor's 4). The hang's stack (supervisor,
  below) named `item_update`. An item's rested surface is now checked against the collision
  installed when it's looked up (`chiefrim_item_rest_valid`, both places in `items.c`; else it
  rests on none and settles again), and each swap moves resting items onto the new BSP's surface
  under them, as bipeds (`chiefrim_world_install`; one Chief carries rests on none). The exact
  sequence that left the stale index wasn't reproduced offline; the check covers any.
- **Skyrim's own animations (the hand-off, Phase 4, `skse/src/Handoff.cpp`):** while the player
  sits (chairs, crafting stations, any furniture: its sit/sleep state), sleeps, rides, swims (§6),
  is in a beast form (werewolf, vampire lord: a race that isn't playable), is in a kill move, or a script holds him (AI-driven, or his movement controls turned off: the
  Helgen cart, scenes), Skyrim has the player. Input routes to Skyrim (Chief gets none, as in a
  menu) and Skyrim's handlers for Chief's actions are on again; the camera, field of view and
  first-person arms are Skyrim's (furniture cameras and the third person work); its HUD bars are
  back; the overlay draws the world layer (shots in flight, bullet holes) but not the screen layer
  (Chief's weapon and HUD). Skyrim moves the player, whatever `bSkyrimMoves`, and Chief follows.
  It starts at once and ends when nothing has held the player for 300 ms (stepping out of the
  water, standing up). `[Handoff] bEnabled`. No protocol change: Halo already takes routing to
  Skyrim as no input. Skyrim's damage to the player still goes to Chief's shields and health
  (§8.3), so his vitality stays the one health. Verified in game (2026-10-06): swimming, a bed
  (AI-driven to it, then furniture), a horse (AI-driven, then the mount), each handed back to Chief;
  and (2026-10-07) a werewolf and a vampire lord, linked in that form, kept by Skyrim, and Chief back
  when the werewolf form ended. Unlinking puts back the third person if Chief found the player in it
  (a beast form has no first person, and its scripts turn the POV switch off).
  **Death (Phase 5, `[Handoff] bDeath`):** when the player dies (Chief's death in Halo kills him,
  or Skyrim kills him), Skyrim has him too: its death camera, no Chief's weapon or HUD, and no
  combat bridging for a dead player. It holds through the reload's loading screen and, once that
  closes, `fDeathFadeInSeconds` (2) more while the loaded game fades in (longer while Skyrim's
  `Fader Menu` is up, up to 10 s), then hands Chief back. A player brought back without a reload
  is Chief's again at once. Verified in game (2026-10-08).
- **The console (Phase 5, protocol 19, `skse/src/Console.cpp`):** a `chiefrim` command in
  Skyrim's console. SKSE can't add one in SE/AE, so the plugin renames one of the game's developer
  commands that players don't need (the first of `TestSeenData`, `TestLocalMap`,
  `ShowRenderPasses`, `DumpNiUpdates` still there; the log says which) and gives it its handler,
  which reads the typed line whole.
  - `chiefrim restart`, `on`, `off`, `toggle`: as the F11 and F10 keys.
  - `chiefrim give [name | number]`: a weapon by its tag path's last part or the whole path, any
    case (`chiefrim give sniper rifle`); none: the next, as F7. `chiefrim weapons` lists the
    names. Halo answers on the console (`CR_MSG_CONSOLE`): given, or no such weapon. The name
    travels in `cr_msg_give_weapon.name`.
  - `chiefrim shapes [on | off]`: Halo draws each proxy's hit shapes (§8.1) as wireframes on the
    overlay's screen layer, over everything: yellow, a person's head red, shapes from bounds cyan,
    a proxy without shapes its biped's standing pill in white (`CR_MSG_DEBUG`, sent again to a new
    Halo; a `render.c` hook, Halo's own debug lines).
  Offline (`fake_skyrim.py --list-weapons-at`, `--give-name`, `--shapes`): the list, a shotgun and
  a needler given by name (any case), an unknown name refused; the wolf's and the person's shapes
  drawn where they stand. Verified in game (2026-10-08).
- **Skyrim HUD:** keep the compass, plus quest and notification messages, and the stamina bar
  (Skyrim's sprint spends it). Hide health, magicka and the crosshair, because Halo's HUD replaces
  them.
- **Skyrim inventory, magic, shouts and perks:** not available while Halo drives the player.
- **Launching and recovery (2026-10-05):** `tools/launch_halo.sh` supervises Halo: it starts it,
  starts it again whenever it exits or crashes (after 1, 4, 9 ... up to 30 s while it keeps crashing
  within 30 s of starting), and obeys Skyrim through `/dev/shm/chiefrim_control` ("<count>
  restart|stop|start"). As Skyrim's Steam launch options (`launch_halo.sh --steam %command%`) it
  runs alongside Skyrim, outside Proton's container, and stops Halo when Skyrim exits. The plugin
  asks for a restart when Halo stops responding (a hang: a crash the supervisor sees by itself),
  and on its keys: `iRestartHaloKey` (F11) kills and restarts Halo, `iToggleChiefrimKey` (F10)
  turns Chiefrim off (unlinked: Skyrim's own player, controls, camera and health; Halo stopped)
  and on. The keys are applied in the next `Link::Update`, so the unlinking path restores Skyrim's
  controls. A new Halo in the same shared file is noticed by its process id (relinked: hello and
  the world again); Halo no longer truncates the file on start (a Skyrim still mapping it would
  fault on its pages). Checked offline: a SIGKILL, a restart, off and on, and Skyrim exiting.
  **Where a hung Halo was (2026-10-09):** before it kills a Halo Skyrim found unresponsive, the
  supervisor sends it SIGUSR2, and Halo's main thread writes its stack to `halo.out` (`halo.out.1`
  once the next one starts; `addr2line -f -e halo/.work/build/linux/halo ADDRESS` names Halo's
  frames). The game's log is kept too (`debug.txt.1`): the restarted Halo starts its own afresh.
  Added after Halo hung (2 s without a frame) as a running Halo went into a fort's interior, twice
  in one session, and a fresh one handled the same interior; the logs then didn't say where.
  Phase 0 has `tools/run_phase0.sh` for the test stand.
- **Hidden window:** the port's own hidden-window mode (`HALO_HIDDEN_WINDOW`) crashes the GL
  driver within seconds in the lens-flare occlusion query (`rasterizer_lens_flares_submit_occlusion_tests`).
  This also happens on stock b30 without Chiefrim, so it's an upstream bug. The Phase 0 test stand
  runs Halo in a headless gamescope instead. Phase 2 renders offscreen and skips Halo's lens
  flares (Skyrim draws the sky), which removes the problem for Chiefrim. The plugin connects when it sees a heartbeat. Auto-launch from inside Proton runs into
  Steam's runtime container, where Halo's 32-bit SDL3 and GL libraries may be missing, so it is
  later work.

## 12. Phased plan

Each phase ends in something you can play.

| # | Phase | "Done" when |
|---|---|---|
| 0 | **Link** | The SKSE plugin cross-compiles on Linux and loads in 1.6.1170. Both sides handshake over `/dev/shm` across the Proton boundary. The coordinate and yaw mapping is unit-tested. Halo runs on the host map with Chiefrim's collision BSP: a temporary flat floor at Skyrim ground height. Walking as Chief moves the Skyrim player. `tools/fake_skyrim.py` stands in for Skyrim. **Status: done (2026-10-04).** Verified in game on 1.6.1170: the plugin links to Halo across Proton, sends the world context and Teleport, Chief is placed and the Skyrim player follows him, and menus and loading screens keep the link (heartbeat thread). The first in-game test found three bugs, all fixed (a stale BSP surface index crash, a link timeout at connect, and Chief re-placed after Skyrim pauses). |
| 1 | **Walk Skyrim as Chief** | **Status: done (2026-10-05), verified in game.** Skyrim moves the player and Chief follows (§7, `bSkyrimMoves=1`): Skyrim's own controller walks, jumps, sprints and sneaks on Skyrim's meshes; Chief is placed there each frame and aimed along the camera. Chief's actions are Skyrim's (InputBridge, §7; rebinding carries over): fire, zoom (with the view narrowing), reload, grenade, melee, weapon and grenade switch, flashlight, all verified. The camera is Skyrim's, with Chief's field of view (85, zoom from Halo) and no third-person switch while linked. Halo's collision (§5.2, the runtime BSP builder) is built from Skyrim's Havok shapes for shots and grenades. Unlinking leaves Skyrim fully playable (controls reset as pausing does). The Halo-driven mode (`bSkyrimMoves=0`, CameraDriver §6 and the movement safeguards) stays: it walked, but caught and bounced on Skyrim's meshes. |
| 2 | **Overlay** | First-person and HUD layers composited (CPU path). Chief's arms, weapon and HUD are in Skyrim, and reloads and weapon swaps animate. Works with SSE Display Tweaks. **Status: done (2026-10-05), verified in game:** the weapon, arms and HUD show as in Halo, animate, and hide in menus; zooming works (the pistol's; other scopes to check). One picture for both layers (§9). Tested with the fake Skyrim: 1920x1080 frames at Halo's frame rate (~60), 15% of the screen covered by the weapon, arms and HUD, transparent elsewhere; the compositor's shader and blend checked under Proton with DXVK. |
| 3 | **Combat** | Proxies, HitActor, PlayerHurt, shields, death, the world layer with depth (projectiles, effects, grenades). You can clear a bandit camp with an MA5B and frag grenades. **Status: done (2026-10-06), verified in game:** the world layer (§9: decals, projectiles, effects, depth-tested against Skyrim's, reprojected onto its camera), proxies, damage both ways with level scaling, shields, death both ways, the debug weapon key (§8); Skyrim's own hit processing (pain, hit reactions, crime), explosions that throw and burn; crash recovery and the on/off and restart keys (§11); Halo's prompts naming Skyrim's keys (§7). |
| 4 | **Full world** | Interiors and load doors, the deep-water decision, furniture and scene hand-off (CollisionField stage C came with Phase 1, §5.2). Also Skyrim's HUD and light (§9). **Status: done (2026-10-06), verified in game:** Skyrim's HUD and light on Halo's objects (§9); the hand-off to Skyrim's furniture, beds, mounts, scenes and swimming (§11; beast forms verified 2026-10-07); load doors, interiors and fast travel (a new world and `Teleport` when the world changes, a loading screen closes or the player jumps over 1024 units). The one crash in testing was MaxsuCombatEscape's (combat pathing run inside a cell change; it crashes the same way without Chiefrim). |
| 5 | **Persistence and polish** | Co-save state (**done 2026-10-07, verified in game**, §11: saves, loads, F10 and F11 keep the kit; a save without one gets the starting loadout), weapon acquisition beyond the loadout (**weapon caches done 2026-10-09, verified in game**, §8.4, protocol 24: chosen, laid down, taken and kept; a fort's boss chest to check), lighting matched to Skyrim weather (**built 2026-10-08, checked offline**, §9: the sun's shadows, Skyrim's grade and fog on Halo's layers, protocol 21; in game to check), better proxy hitboxes for creatures (**done 2026-10-08, verified in game**, §8.1: each actor's own hit shapes, protocol 18; people, a giant, a horse, a mammoth, a dragon), the `chiefrim` console command (**done 2026-10-08, verified in game**, §11), launch script hardening. **No third-person view** (decided 2026-10-08): Halo never had one, so Chiefrim doesn't either. |
| ★ | **Stretch: Covenant** | Revisit later (see Scope). |

## 13. Decisions

Proposed. Each one needs the user's call before the phase that depends on it.

1. **Damage scaling** (Phase 3): **decided 2026-10-05: level-scaled.** Outgoing: a hit's fraction of
   the proxy's vitality × the NPC's max health × `fDamageMult`, divided by a toughness that grows
   with the NPC's level against the player's: `clamp((npcLevel / playerLevel)^fLevelExponent, 0.5,
   3)` (`fLevelExponent` 0.5: an NPC at four times the player's level takes twice the hits). So a
   weapon kills a peer in as many hits as it kills the proxy in Halo, and Skyrim's difficulty curve
   stays. Incoming: Skyrim damage ÷ `fIncomingReference` (250) × Chief's vitality, shields first.
   All ini values. (§8)
2. **Weapon acquisition** (Phase 3): **decided 2026-10-05: the starting loadout (b30's: MA5B, M6D,
   frag grenades) and a debug command** that gives Chief any weapon in the host map. (§8.4)
3. **Deep water** (Phase 4): **decided 2026-10-06: hand over to Skyrim's swimming** while the player
   swims. (§6, §11)
4. **Host map:** **decided 2026-10-07: `d20`** (Keyes), the one map with every weapon a player
   can carry (`tools/list_map_tags.py`). `CHIEFRIM_MAP` overrides it in the launcher and the test
   stand. Chief's starting loadout is `Chiefrim.ini`'s `[Loadout]` (weapons by name, their own
   rounds; grenades), not the map's. (§5.3)
5. **Decomp management:** settled in Phase 0: pinned upstream commit, patches and our own
   sources, no fork. (§14)
6. **Covenant enemies:** deferred, stretch goal.
7. **Third-person view:** **decided 2026-10-08: none.** Halo never had one, so Chiefrim doesn't
   either; while linked the view stays first person (Skyrim's own third person only in the hand-off,
   §11).

## 14. Repo layout

```
Chiefrim/
  docs/DESIGN.md
  protocol/chiefrim_protocol.h   shared C header + layout static_asserts
  skse/                          SKSE plugin (CMake, CommonLibSSE-NG submodule, C++23,
                                 cmake/clang-cl-xwin.cmake cross toolchain)
  halo/                          Halo-side changes (see below)
  tools/                         setup_halo.py, setup_skse.sh, package_skse.sh, launch_halo.sh,
                                 run_phase0.sh, fake_skyrim.py (--overlay, --grab: Halo's
                                 frames as PNGs), test_protocol.sh, linktest/;
                                 later: tag lister
  licenses.toml                  every third-party component and its license (LICENSING.md)
  build/                         (git-ignored) test builds, Halo test data root, screenshots
```

At the repo root: `LICENSE` (GPL-3.0) and `THIRD-PARTY-NOTICES.md`.

**Halo-side changes (settled in Phase 0):** no fork. `halo/` holds:

- `UPSTREAM`: the pinned OpenCE commit (halo-ce-universal before its rename, 2026-10).
- `patches/`: the hooks in the game's own files, each marked `/* CHIEFRIM */` (`main.c`,
  `game.c`, `scenario.c`/`.h`, `player_control.c`, `input_abstraction.c`, `render_cameras.c`,
  `render.c`, `render_objects.c`, `collisions.c`, `objects.c`, `damage.c`, `decals.c`/`.h`, and the port's `d3d8_gl.c` and `nv2a_psh.c`).
- `src/`: our own engine code (`chiefrim.c`, `chiefrim.h`, ...); `src/port/` goes to
  `port/linux/src/` instead (`chiefrim_overlay_gl.c`: code on the port's OpenGL device, built with
  the platform layer's flags).
- `overrides/`: whole upstream files Chiefrim replaces, for licensing (`port/linux/src/xiso.c`;
  see [LICENSING.md](LICENSING.md)).

`tools/setup_halo.py` clones upstream into `halo/.work` (git-ignored), checks out the pin, applies
the patches, copies `src/` and the protocol header into `source/chiefrim/` (the game's build
compiles every `.c` file under `source/` by itself), and builds. `tools/save_halo_patch.sh` writes
hook edits made in `.work` back to `patches/`. Updating upstream means moving the pin and
refreshing the patches.

**Moved to OpenCE (2026-10-08).** halo-ce-universal was renamed OpenCE and moved to
OpenCommunityEdition/OpenCE; the pin went from 193cbf59 (2026-10-04) to 73dc01d0 (2026-10-08),
254 commits: mostly online and split-screen co-op (unused here), and for Chiefrim: Halo's audio
fixed (Xbox ADPCM decoding, 3D sounds' distance, resampling, a limiter, stereo sounds in the
world), renderer fixes (vertices thrown to the screen's centre near the camera, texture
bindings, constant serials wrapping), fewer GL calls, a packed bounding-sphere copy for
collision, and Custom Edition maps (a future host map). Seven hooks conflicted: five beside
upstream's new includes; `game.c`'s no-AI line beside co-op's actor driving (Chiefrim drives
neither); `d3d8_gl.c`'s overlay framebuffer beside the new multisampling (the overlay is never
multisampled); and the hitbox bounds hook goes before the new bounds copy (`objects.c`), so the
copy has the proxies' bounds. **Anti-aliasing is forced off in Chiefrim mode**
(`display.anti_aliasing`: FXAA and SMAA filter the 3D view, SSAA resizes the targets, and the
overlay is read back as drawn). `display.per_pixel_lighting` (off by default) lights Chief's arms
and weapon for each pixel by the same lights, Skyrim's included: not tried yet. Offline, against
the fake Skyrim, the overlay frames match the old build's pixel for pixel, at the same frame rate;
hits on the hit shapes, the console's commands and the link as before. Two new third-party
folders, SMAA (MIT) and zlib (Zlib), are in `licenses.toml`.

**Pin moved to b5a4870d (2026-10-10).** 225 commits, mostly Android, touch controls, voice chat,
votes to kick and netcode (unused here); for Chiefrim: sound fixes (a stream that ran dry plays
again, retired and looping voices stopped), the decompiled code's latent bugs fixed (unset
locals given values), render targets freed when the screen's scale changes, bounded formats and
copies of map strings and tag names, and an item's resting surface kept past 32767 (to 65535:
Skyrim's collision runs past 400,000, so the `items.c` hook that skips those stays). Five hooks
conflicted, each beside an upstream line (the touch controls' aiming and movement, an include,
the resting surface's helper) and kept with it. Upstream's voice chat brings Opus
(`port/third_party/opus`, BSD-3-Clause) into `licenses.toml`. Offline, as before: combat and a
proxy's drop, the overlay's layers and brightness, a cache laid down, the scripted player.
Upstream's enhanced animations (`game.enhanced_animations`, on) are third-person bipeds'
grenade throws and riders: never seen in Chiefrim.

The decomp repo contains no game data. Its own `.gitignore` already excludes `assets/` and
extracted maps. The test data root (`build/halo-data`) holds only a link to the user's `maps/`.

## 15. Risks

| Risk | Mitigation |
|---|---|
| ~~`/dev/shm` sharing across the Proton container~~ | **Resolved in Phase 0:** works, 0.38 µs round trips (§10) |
| ~~Cross-compiling CommonLibSSE-NG with clang-cl on Linux~~ | **Resolved in Phase 0:** builds with clang-cl + xwin; the DLL loads under Proton |
| ~~CommonLibSSE-NG is GPL-3.0-or-later~~ | **Decided (2026-10-04): Chiefrim is GPL-3.0-or-later.** Everything stays GPL-3.0 compatible, checked by `tools/check_licenses.py` against `licenses.toml` ([LICENSING.md](LICENSING.md)). The port's extract-xiso-based disc reader (4-clause BSD) is replaced by a stub |
| Halo depends on the BSP in more places than §5.1 covers (decals, lighting, sound environments, PVS) | Partly resolved: keeping the host map's structure BSP and its plane list covers clusters, portals and lights. More dependencies may show once Skyrim's real geometry replaces the floor |
| The runtime BSP compiler (§5.2) is the largest new piece | Start from the hand-built floor's format and self-tests. Build on a worker thread, swap between ticks. Keep regions small (a ring of cells around the player) |
| The port's hidden-window mode crashes in the lens-flare query (upstream) | Test stand uses headless gamescope. Phase 2 renders offscreen without lens flares (§11) |
| CPU readback of three layers costs too much | Only redraw the HUD layer when it changes. Lower resolution for the world layer. GPU interop later |
| SSE Display Tweaks' swap-chain and frame-limiter hooks interfere with compositing or the lockstep | Test with it from Phase 2. If it conflicts, adjust its settings or hook at a point it doesn't touch. A shader replacer (Community Shaders, ENB) added later is a separate compatibility task |
| Halo's 30 Hz tick against Skyrim's frame rate | The port already interpolates (`render_interpolation.c`). Skyrim follows the interpolated pose |
| Proxy hitboxes are wrong for non-humanoid creatures | Phase 5: Skyrim's own hit shapes for each actor (§8.1); the scaled biped only when there are none |
| Upstream decomp moves quickly | Pinned commit (`halo/UPSTREAM`). Hooks small and marked |
| Legal | Fan project. Nothing from either game is distributed, and the user supplies both games. Code licensing is settled (GPL-3.0-or-later, [LICENSING.md](LICENSING.md)). Whether releases may include a built Halo executable, given that the decompilation is of Microsoft's game, is decided before any public release |
