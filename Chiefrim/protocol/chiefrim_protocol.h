/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_PROTOCOL.H

The shared-memory layout between the Skyrim SKSE plugin (Windows x64 under
Proton) and the Halo engine (native Linux i386). See docs/DESIGN.md, §10.

One file, /dev/shm/chiefrim_v1, mapped by both sides:
- Halo creates it (shm_open + mmap MAP_SHARED).
- Skyrim opens Z:\dev\shm\chiefrim_v1 (CreateFileW + CreateFileMappingW +
  MapViewOfFile). Wine backs that with a shared mmap of the same file.

Rules, because one side is 32-bit and the other 64-bit:
- Fixed-width types only. No pointers, no size_t, no enums in structs, no
  bool.
- Every struct is padded by hand to a multiple of 8 bytes. The
  static_asserts at the bottom pin each size, and both builds compile them.
- Shared atomics are 32-bit (i386 has no cheap 64-bit atomic load).
- Little-endian, as both sides are x86.

There are no named events across the Proton boundary, so both sides poll:
- latest-value SLOTS under a seqlock, for per-frame state;
- two single-producer single-consumer RINGS for events.

This header is C (the Halo engine is C) and C++ (the SKSE plugin).
*/

#ifndef CHIEFRIM_PROTOCOL_H
#define CHIEFRIM_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants ---------------------------------------------------------- */

#define CR_MAGIC            0x46454843u /* "CHEF" */
#define CR_PROTOCOL_VERSION 27u

#define CR_SHM_NAME         "chiefrim_v1"                    /* shm_open name */
#define CR_SHM_LINUX_PATH   "/dev/shm/chiefrim_v1"
#define CR_SHM_WINE_PATH    L"Z:\\dev\\shm\\chiefrim_v1"

#define CR_RING_BYTES       (4u * 1024u * 1024u)             /* each direction: collision comes in bursts */

/* 1 Halo world unit = 10 ft = 3.048 m; Skyrim = 70 units/m (docs §4). */
#define CR_SKY_UNITS_PER_WU 213.36f

/* A side counts as gone when its heartbeat has not moved for this long. */
#define CR_HEARTBEAT_TIMEOUT_MS 2000u

/* cr_header.*_state */
#define CR_SIDE_ABSENT      0u
#define CR_SIDE_STARTING    1u
#define CR_SIDE_READY       2u /* linked and running */
#define CR_SIDE_CLOSING     3u

/* ---- shared atomics ----------------------------------------------------- */

/* clang (both targets) and GCC provide these builtins. */
#define CR_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define CR_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define CR_FENCE_ACQ()      __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define CR_FENCE_REL()      __atomic_thread_fence(__ATOMIC_RELEASE)

/* ---- vectors ------------------------------------------------------------ */

typedef struct cr_vec3
{
	float x, y, z;
} cr_vec3;

/* ---- slots (latest value, seqlock) ------------------------------------- */

/* Skyrim -> Halo. Where the world is (docs §4, §5.1 stage A). */
typedef struct cr_world_context
{
	uint32_t world_id;      /* worldspace or interior-cell FormID */
	uint32_t is_interior;   /* 0 or 1 */
	cr_vec3  origin;        /* Skyrim units; Halo (0,0,0) is here */
	float    floor_z;       /* Phase 0: Skyrim Z of the temporary flat floor */
	uint32_t generation;    /* bumps when the origin moves (load door etc.) */
	float    field_of_view; /* Chief's unzoomed view, degrees, horizontal for 4:3 as
	                           Skyrim measures it; zoom narrows it as in Halo. 0: Halo's own */
	float    chief_height;  /* Skyrim units: Chief's standing height (collision and eyes
	                           scale with it). 0: Halo's own */
	float    chief_radius;  /* Skyrim units: Chief's collision radius. 0: Halo's own */
	uint32_t collision_radius; /* regions (CR_REGION_UNITS) around Chief's that Halo builds its
	                              collision from, so shots hit that far; 0: 1. Skyrim sends one
	                              more around */
	uint32_t reserved;
} cr_world_context;

/* Skyrim -> Halo. Chief's controls (docs §7). Skyrim's ControlMap has already
turned keys, mouse and gamepad into user events, so these are actions, not
keys: rebinding a Skyrim action rebinds Chief's with it. */
#define CR_ACTION_JUMP           0u
#define CR_ACTION_CROUCH         1u
#define CR_ACTION_FIRE           2u
#define CR_ACTION_ZOOM           3u
#define CR_ACTION_RELOAD         4u
#define CR_ACTION_GRENADE        5u /* throw */
#define CR_ACTION_MELEE          6u
#define CR_ACTION_ACTION         7u /* pick up / swap weapon */
#define CR_ACTION_SWITCH_WEAPON  8u
#define CR_ACTION_SWITCH_GRENADE 9u
#define CR_ACTION_FLASHLIGHT     10u
#define CR_ACTION_MARK           11u /* not Halo's: "Chief is stuck here", for the logs */
#define CR_ACTION_COUNT          12u
#define CR_ACTION_SLOTS          16u /* room to grow */

typedef struct cr_input
{
	uint32_t frame;               /* Skyrim frame counter */
	uint32_t routing;             /* CR_ROUTE_* */
	uint32_t held;                /* bit per CR_ACTION_* held now */
	uint32_t session;             /* changes when the totals below restart */
	uint8_t  presses[CR_ACTION_SLOTS]; /* per action, +1 per press (wraps):
	                                      a tap between two Halo frames still counts */
	float    forward;             /* -1..1, +forward */
	float    strafe;              /* -1..1, +right */
	double   yaw_total;           /* radians turned since the session began, +right (clockwise) */
	double   pitch_total;         /* radians, +up */
} cr_input;

#define CR_ROUTE_HALO   0u /* gameplay: Chief gets the actions */
#define CR_ROUTE_SKYRIM 1u /* a Skyrim menu or scene owns input; Chief gets none */

/* Halo -> Skyrim. The player and camera (docs §6). Skyrim units/radians. */
typedef struct cr_player_state
{
	uint32_t tick;          /* Halo game tick */
	uint32_t pose;          /* CR_POSE_* */
	cr_vec3  position;      /* Skyrim world units, feet */
	float    yaw;           /* Skyrim heading (rotZ), radians */
	float    pitch;         /* Skyrim rotX, radians */
	uint32_t on_ground;     /* 0 or 1 */
	cr_vec3  eye;           /* camera position, Skyrim units */
	cr_vec3  forward;       /* camera basis, Skyrim axes */
	cr_vec3  up;
	float    vertical_fov;  /* radians */
	float    body_fraction;   /* 0..1 */
	float    shield_fraction; /* 0..1 */
	uint32_t time_us;       /* Halo's clock when this state was taken (microseconds, wraps):
	                           Skyrim draws the player a moment behind, between states */
	uint32_t reserved;
} cr_player_state;

#define CR_POSE_STANDING  0u
#define CR_POSE_CROUCHING 1u
#define CR_POSE_AIRBORNE  2u
#define CR_POSE_DEAD      3u

/* A seqlock slot: seq is odd while the writer is inside. */
#define CR_DECLARE_SLOT(name, type) \
	typedef struct name             \
	{                               \
		uint32_t seq;               \
		uint32_t pad;               \
		type value;                 \
	} name

/* Skyrim -> Halo, when Skyrim moves the player (docs §7): where the player
is and looks. Halo's Chief follows: placed there each frame, aimed along
the camera. Skyrim units/radians. */
typedef struct cr_skyrim_player
{
	uint32_t frame;         /* bumps each Skyrim frame */
	uint32_t flags;         /* CR_SKYRIM_* */
	cr_vec3  position;      /* feet */
	float    yaw;           /* Skyrim heading (rotZ) */
	float    pitch;         /* Skyrim rotX */
	cr_vec3  eye;           /* the camera's position */
	cr_vec3  forward;       /* the camera's forward axis */
	cr_vec3  velocity;      /* units per second */
	uint32_t reserved[2];
} cr_skyrim_player;

#define CR_SKYRIM_DRIVES    0x0001u /* Skyrim moves the player; Chief follows (else Halo moves Chief) */
#define CR_SKYRIM_ON_GROUND 0x0002u
#define CR_SKYRIM_SNEAKING  0x0004u

/* Skyrim -> Halo. Skyrim's picture (docs §9): Halo draws Chief's layers at
its size and shape. */
typedef struct cr_display
{
	uint32_t width;         /* Skyrim's back buffer, pixels */
	uint32_t height;
	uint32_t flags;         /* CR_DISPLAY_* */
	uint32_t frame;         /* bumps each Skyrim frame (Present) */
} cr_display;

#define CR_DISPLAY_OVERLAY 0x0001u /* Skyrim composites Halo's frames: draw them */

/* Skyrim -> Halo. The camera Skyrim is rendering this frame with (docs §9),
published as its world rendering starts. Halo draws its next frame through
it, so its world layer sits exactly on Skyrim's picture, and names it in
the frame (cr_frame_header.camera_frame); Skyrim waits a moment for that
frame before compositing. Skyrim units. */
typedef struct cr_camera
{
	uint32_t frame;         /* bumps per published camera; 0: none yet */
	uint32_t flags;         /* reserved */
	cr_vec3  eye;
	cr_vec3  forward;
	cr_vec3  up;
	float    vertical_fov;  /* radians, as Skyrim renders */
	float    near_plane;    /* Skyrim's, for the logs */
	float    far_plane;
	uint32_t reserved[2];
} cr_camera;

CR_DECLARE_SLOT(cr_slot_world_context, cr_world_context);
CR_DECLARE_SLOT(cr_slot_input, cr_input);
CR_DECLARE_SLOT(cr_slot_player_state, cr_player_state);
CR_DECLARE_SLOT(cr_slot_skyrim_player, cr_skyrim_player);
/* Skyrim -> Halo. The actors near the player (docs §8.1): Halo keeps an
unseen, hittable stand-in (a proxy biped) for each. Latest value. */
#define CR_ACTORS_MAX   48u
#define CR_HITBOXES_MAX 1024u /* all the actors' together */
#define CR_HITBOXES_PER_ACTOR 48u /* a dragon's skeleton has ~30 bodies */

/* One of an actor's hit shapes (protocol 18): Skyrim's own, the capsules and
spheres of its skeleton's bodies that its arrows hit, as they stand now. A
sphere is a capsule whose ends meet; a box or a hull comes as the capsule
along its longest side. Skyrim world units. */
typedef struct cr_hitbox
{
	cr_vec3  a;
	cr_vec3  b;
	float    radius;
	uint32_t flags;         /* CR_HITBOX_* */
} cr_hitbox;

#define CR_HITBOX_HEAD   0x0001u /* a person's head (NPC Head [Head]): headshots kill */
#define CR_HITBOX_BOUNDS 0x0002u /* not its skeleton's: made from its bounds (no bodies found) */

typedef struct cr_actor
{
	uint32_t form_id;
	uint32_t flags;         /* CR_ACTOR_* */
	cr_vec3  position;      /* feet, Skyrim units */
	float    heading;       /* Skyrim heading (rotZ) */
	float    height;        /* Skyrim units */
	float    head;          /* Skyrim units: the head's centre above the feet, as it stands now
	                           (protocol 16: the proxy's head is put there); 0: unknown */
	uint16_t hitbox_first;  /* its hit shapes: cr_actors.hitboxes[first, first + count) */
	uint16_t hitbox_count;  /* 0: none (Halo hits its proxy's own biped, scaled) */
	uint32_t reserved;
} cr_actor;

#define CR_ACTOR_HOSTILE   0x0001u /* hostile to the player */
#define CR_ACTOR_DEAD      0x0002u
#define CR_ACTOR_ESSENTIAL 0x0004u
#define CR_ACTOR_ATTACKING 0x0008u /* swinging, drawing a bow, casting: the motion tracker shows it (protocol 20) */
#define CR_ACTOR_SNEAKING  0x0010u /* as a unit crouching: only Halo's own speed shows it on the tracker (protocol 20) */
#define CR_ACTOR_PERSON    0x0020u /* its race is a person's (ActorTypeNPC): its proxy carries a gun; an animal's,
                                      a draugr's or another creature's only 1 or 2 grenades (protocol 26) */

typedef struct cr_actors
{
	uint32_t frame;
	uint32_t count;
	cr_actor actors[CR_ACTORS_MAX];
	uint32_t hitbox_count;
	uint32_t reserved;
	cr_hitbox hitboxes[CR_HITBOXES_MAX];
} cr_actors;

CR_DECLARE_SLOT(cr_slot_display, cr_display);
CR_DECLARE_SLOT(cr_slot_actors, cr_actors);
CR_DECLARE_SLOT(cr_slot_camera, cr_camera);

/* ---- rings (events) ---------------------------------------------------- */

/* Byte ring. head is written only by the producer, tail only by the
consumer; both count bytes and wrap at 2^32 (CR_RING_BYTES divides it). */
typedef struct cr_ring
{
	uint32_t head;
	uint32_t pad0[15];      /* own cache line */
	uint32_t tail;
	uint32_t pad1[15];
	uint8_t  data[CR_RING_BYTES];
} cr_ring;

/* Every message starts with this. size includes the header, rounded up to
8. A size-0 header with type CR_MSG_WRAP means "skip to the ring start". */
typedef struct cr_msg_header
{
	uint16_t type;
	uint16_t size;
	uint32_t reserved;
} cr_msg_header;

/* Message types. 0x80-0xFF are reserved for stretch goals (Covenant,
vehicles); docs §10. */
#define CR_MSG_WRAP      0x00u
#define CR_MSG_HELLO     0x01u /* both ways */
#define CR_MSG_TELEPORT  0x02u /* S->H */
#define CR_MSG_LOG       0x03u /* H->S: a line for SKSE's log */
#define CR_MSG_COLLISION_RESET 0x04u /* S->H: forget all collision regions */
#define CR_MSG_COLLISION_TRIS  0x05u /* S->H: (part of) one region's triangles */
#define CR_MSG_HIT_ACTOR    0x06u /* H->S: Chief hurt an actor's proxy (docs §8.2) */
#define CR_MSG_PLAYER_HURT  0x07u /* S->H: something in Skyrim hurt the player (docs §8.3) */
#define CR_MSG_PLAYER_DIED  0x08u /* H->S: Chief is dead */
#define CR_MSG_GIVE_WEAPON  0x09u /* S->H: debug: give Chief a weapon of the host map (docs §8.4) */
#define CR_MSG_KEY_NAMES    0x0Au /* S->H: the player's keys for Chief's actions, for Halo's prompts (protocol 12) */
#define CR_MSG_LIGHTING     0x0Bu /* S->H: Skyrim's light where the player is, for Halo's objects (protocol 13) */
#define CR_MSG_CHIEF_STATE   0x0Cu /* H->S: Chief's weapons, ammo, grenades and vitality, when they change (protocol 14) */
#define CR_MSG_CHIEF_RESTORE 0x0Du /* S->H: Chief's, from a Skyrim save, or the starting loadout (protocol 14) */
#define CR_MSG_CHIEF_HEAL    0x0Eu /* S->H: the player drank or ate something that restores health (protocol 15) */
#define CR_MSG_EXPLOSION     0x0Fu /* H->S: an explosion in Halo (a grenade, a rocket): Skyrim's loose objects fly (protocol 16) */
#define CR_MSG_FLASHLIGHT    0x10u /* H->S: Chief's flashlight as it shines now, when it changes: Skyrim's world is lit (protocol 17) */
#define CR_MSG_CONSOLE       0x11u /* H->S: a line for Skyrim's console, answering a console command (cr_msg_log; protocol 19) */
#define CR_MSG_DEBUG         0x12u /* S->H: debug drawing on or off (protocol 19) */
#define CR_MSG_SHOT          0x13u /* H->S: a projectile's way this tick: Skyrim's destructible objects on it are hurt (protocol 23) */
#define CR_MSG_CACHE_PLACE   0x14u /* S->H: a weapon cache: a weapon lying in Skyrim's world (protocol 24) */
#define CR_MSG_CACHE_TAKEN   0x15u /* H->S: Chief picked a cache's weapon up (protocol 24) */

/* Collision (docs §5.2): Skyrim's Havok shapes near the player, as
triangles in Skyrim world units, wound counter-clockwise around their
front, per cube region of CR_REGION_UNITS. A region arrives in one or more
CR_MSG_COLLISION_TRIS messages (first..first+count of total, in order);
total 0 means the region is empty. */
#define CR_REGION_UNITS        1024.0f
#define CR_TRIS_PER_MESSAGE    1600u

typedef struct cr_triangle
{
	cr_vec3  v[3];
	uint16_t material;   /* reserved: Skyrim material, 0 for now */
	uint16_t flags;      /* CR_TRIANGLE_* */
} cr_triangle;

/* Solid only from the front: the land (a height field, outside up) and the
faces of closed shapes (boxes, capsules, convex hulls, wound outward). Halo
gives them no back: where the land pokes up through a road, or someone dips
into a slab, a back would push them down through it. */
#define CR_TRIANGLE_ONE_SIDED 0x0001u
/* The land itself (Skyrim's height field): Halo keeps its heights and puts
Chief back on top if he ends up under it. */
#define CR_TRIANGLE_LAND      0x0002u

/* Chief hurt an actor: how much, as a fraction of the proxy's own vitality
(its biped's, shields and body, unscaled), so a weapon takes as many hits
as it would on that biped in Halo. Skyrim scales it (docs §13). */
typedef struct cr_msg_hit_actor
{
	cr_msg_header header;
	uint32_t form_id;
	uint32_t flags;      /* CR_HIT_* */
	float    fraction;   /* 1 = what kills the proxy's biped */
	float    reserved;
	cr_vec3  blast;      /* CR_HIT_EXPLOSION: the explosion's centre, Skyrim units */
	uint32_t reserved2;
} cr_msg_hit_actor;

/* Some of it was an explosion's (a grenade's, a rocket's: Halo's area
damage): Skyrim throws the actor away from its centre and sets it alight
(docs §8.2). */
#define CR_HIT_EXPLOSION 0x0001u
/* The proxy was killed outright: a headshot, which kills a marine whatever
its vitality. Skyrim kills the actor, whatever its level (protocol 16). */
#define CR_HIT_HEADSHOT  0x0002u

/* Skyrim's damage to the player, which Skyrim has refunded: Halo applies it
to Chief, shields first. */
typedef struct cr_msg_player_hurt
{
	cr_msg_header header;
	float    amount;     /* of Chief's whole vitality (shields and body): Skyrim's damage over [Combat] fIncomingReference */
	uint32_t kind;       /* CR_HURT_* */
	uint32_t attacker;   /* form id, 0: unknown */
	cr_vec3  from;       /* where it came from (the attacker), Skyrim units; all 0: unknown */
	uint32_t reserved[2];
} cr_msg_player_hurt;

#define CR_HURT_OTHER      0u
#define CR_HURT_MELEE      1u
#define CR_HURT_PROJECTILE 2u /* arrows, bolts, thrown */
#define CR_HURT_MAGIC      3u /* spells, poison, over time */

typedef struct cr_msg_player_died
{
	cr_msg_header header;
	uint32_t reserved[2];
} cr_msg_player_died;

/* Skyrim's healing (a potion, food): Halo adds it to Chief's body (his
shields recharge by themselves, as in Halo), on the scale of
CR_MSG_PLAYER_HURT, so a potion heals what as much Skyrim damage would hurt. */
typedef struct cr_msg_chief_heal
{
	cr_msg_header header;
	float    amount;     /* of Chief's whole vitality (shields and body): Skyrim's health over [Combat] fIncomingReference */
	uint32_t item;       /* the potion's form id, for the logs */
} cr_msg_chief_heal;

/* An explosion's area damage started (a grenade, a rocket, a plasma bolt's
splash): Skyrim pushes its loose physics objects in reach away from the centre. */
typedef struct cr_msg_explosion
{
	cr_msg_header header;
	cr_vec3  center;       /* Skyrim units */
	float    radius;       /* Skyrim units: its damage's reach */
	float    acceleration; /* Halo's push on objects in the middle (world units per tick), as a guide to its power */
	uint32_t reserved;
} cr_msg_explosion;

/* A projectile's way this tick (protocol 23): Halo's bullets, plasma and
grenades go through Skyrim's destructible objects (spider webs, barricades:
Skyrim leaves them out of the collision it sends), so Skyrim looks along
the way itself and hurts the first thing on it, if it is one. */
typedef struct cr_msg_shot
{
	cr_msg_header header;
	cr_vec3  from;         /* Skyrim units */
	cr_vec3  to;
	uint32_t reserved[2];
} cr_msg_shot;

/* Chief's flashlight (protocol 17): his biped's light as Halo would shine
it now. Halo's light has no world of Halo's to fall on, so Skyrim lights its
own along the player's view with it. Sent when it changes (switched on or
off, fading in or out) and on linking. */
typedef struct cr_msg_flashlight
{
	cr_msg_header header;
	cr_vec3  color;          /* linear, 1 = full, its power applied; all 0: off */
	float    radius;         /* Skyrim units: its reach */
	float    cutoff_angle;   /* radians, from the beam's axis to the cone's edge */
	float    falloff_angle;  /* radians: full light inside this, fading out to the cutoff */
	uint32_t reserved[2];
} cr_msg_flashlight;

/* name (protocol 19, the console's "chiefrim give"): the weapon's tag path or
its last part ("sniper rifle"), any case; Halo answers on the console. Empty:
by index, of the host map's weapons as Halo lists them in its log at start
(wraps); -1: the next after the last given (the debug key). */
#define CR_WEAPON_NAME_LENGTH 64u
#define CR_GIVE_LIST 0x0001u /* give nothing: list the weapons Chief may have, on the console */
typedef struct cr_msg_give_weapon
{
	cr_msg_header header;
	int32_t  index;
	uint32_t flags;        /* CR_GIVE_* */
	char     name[CR_WEAPON_NAME_LENGTH];
} cr_msg_give_weapon;

/* Debug drawing in Halo's overlay (protocol 19, the console's "chiefrim
shapes"): sent when it changes and on linking */
#define CR_DEBUG_HITBOXES 0x0001u /* the proxies' hit shapes, as wireframes over everything */
typedef struct cr_msg_debug
{
	cr_msg_header header;
	uint32_t flags;        /* CR_DEBUG_* */
	uint32_t reserved;
} cr_msg_debug;

/* The names of the keys (or gamepad buttons, as the player last played) that
do each of Chief's actions in Skyrim, per CR_ACTION_*: Halo's prompts ("Press
X to swap") show them in place of its Xbox buttons. ASCII, NUL-terminated;
empty: none bound (Halo's own icon). Sent on linking and when they change. */
#define CR_KEY_NAME_LENGTH 16u
typedef struct cr_msg_key_names
{
	cr_msg_header header;
	char names[CR_ACTION_COUNT][CR_KEY_NAME_LENGTH];
} cr_msg_key_names;

/* Skyrim's light around the player, ~10 times a second (docs §9): Halo lights
Chief's arms and weapon and its objects with it in place of its map's
lightmap. Colours are linear, 1 = full; directions are the way the light
travels (the sun's points down), in Skyrim's (and Halo's) axes. */
#define CR_LIGHTING_POINTS 4u
typedef struct cr_light_point
{
	cr_vec3 position;   /* Skyrim units */
	float   radius;     /* Skyrim units: no light past it */
	cr_vec3 color;      /* at the light, its fade applied */
} cr_light_point;

typedef struct cr_msg_lighting
{
	cr_msg_header header;
	cr_vec3  ambient;          /* Skyrim's directional ambient, averaged */
	cr_vec3  ambient_up;       /* its colour on surfaces facing up, minus the average */
	cr_vec3  key_color;        /* the sun, the moon, or the interior's directional light */
	cr_vec3  key_direction;    /* unit */
	uint32_t point_count;      /* the point lights nearest the player (torches, fires, spells) */
	cr_light_point points[CR_LIGHTING_POINTS];
	/* protocol 21: shadows from the key light. key_shadowed: it casts them
	(the sun or moon outside; an interior's directional light doesn't), so
	Halo tests each object's way to it. sun_visible: how much of it reaches
	Chief's eye past Skyrim's world (0..1), for Chief, his arms and weapon. */
	uint32_t key_shadowed;
	float    sun_visible;
	/* protocol 22: how much of the sky is open over Chief's eye (0..1; 1
	inside, where the ambient is the cell's): under a roof the sky's light
	is mostly blocked too. Halo dims Chief's ambient by it, and tests other
	objects' way up itself. */
	float    sky_visible;
	/* protocol 27: what surrounds Chief, as Skyrim's picture shows it (the
	compositor's meter of it). A shiny weapon shows mostly its reflection,
	which is of its surroundings: a dim cabin's, not the light's. reflection_cap:
	the most a reflection's strength goes (0..1; 0: not metered, no cap).
	surround_hue: the colour of the picture's light (its mean, weighted to
	its unsaturated parts: autumn leaves aren't the light's colour), its
	largest part 1, which tints reflections (0 0 0: none). */
	float    reflection_cap;
	cr_vec3  surround_hue;
} cr_msg_lighting;

/* Chief's kit (docs §11, save and load): what the Skyrim co-save keeps.
Halo sends it (CR_MSG_CHIEF_STATE) when it changes, a few times a second at
most; Skyrim keeps the latest and writes it into each save. Loading a save
sends it back (CR_MSG_CHIEF_RESTORE), and Halo gives Chief exactly that; a
save without one (older, or a new game) asks for the starting loadout. A new
Halo (a restart) gets the latest too.

generation: each restore's is new, and Halo's states carry the last one it
applied (0: none yet, its own Chief), so Skyrim drops states from before
the restore it is waiting on. Weapons are named by tag path, so a save
survives the host map's tags moving; one the map doesn't have is skipped.
Skyrim may name one by its last part ("shotgun"), as Chiefrim.ini's
[Loadout] does: Halo takes the map's weapon whose path ends with it. */
#define CR_CHIEF_WEAPONS     4u  /* Halo's MAXIMUM_WEAPONS_PER_UNIT */
#define CR_CHIEF_GRENADES    4u  /* Halo has 2 types; room to grow */
#define CR_WEAPON_TAG_LENGTH 64u

typedef struct cr_chief_weapon
{
	char     tag[CR_WEAPON_TAG_LENGTH]; /* the weapon's tag path, NUL-terminated; empty: no weapon */
	int16_t  rounds_total[2];   /* per magazine: all its rounds, those loaded too; negative (S->H): */
	int16_t  rounds_loaded[2];  /* the weapon's own, as one found in the map has them */
	float    age;               /* energy weapons: battery spent, 0..1 */
	uint32_t reserved;
} cr_chief_weapon;

typedef struct cr_chief_state
{
	uint32_t generation;
	uint32_t flags;             /* CR_CHIEF_* */
	int32_t  current_weapon;    /* index into weapons, -1: none */
	int32_t  current_grenade;   /* grenade type, -1: none */
	uint8_t  grenades[CR_CHIEF_GRENADES]; /* per type: frag, plasma */
	float    body;              /* 0..1 of his body's vitality */
	float    shield;            /* 0..1 (over 1: overshield) */
	float    flashlight;        /* battery, 0..1 */
	uint32_t reserved[2];
	cr_chief_weapon weapons[CR_CHIEF_WEAPONS]; /* in Halo's inventory order */
} cr_chief_state;

/* CR_MSG_CHIEF_RESTORE: no saved kit, the host map's starting loadout and
Chief whole (the rest of the state is unused) */
#define CR_CHIEF_STARTING_LOADOUT 0x0001u

typedef struct cr_msg_chief_state
{
	cr_msg_header  header;
	cr_chief_state state;
} cr_msg_chief_state;

/* Weapon caches (protocol 24, docs §8.4): Skyrim chooses where weapons lie
in its bandit camps and forts, keeps them in its co-save, and has Halo lay
each down in the world it is in now (its world_context generation; a
message for another world is dropped, as a new world erases Halo's loose
objects and Skyrim sends its caches again). Halo drops it onto what lies
below the position (once its collision has it) and reports the cache taken
when Chief picks the weapon up. */
typedef struct cr_msg_cache_place
{
	cr_msg_header header;
	uint32_t id;           /* Skyrim's, for the taken report */
	uint32_t generation;   /* the world_context it is for */
	cr_vec3  position;     /* Skyrim units: above where it lies */
	float    yaw;          /* radians from +x, counterclockwise: which way it points */
	char     weapon[CR_WEAPON_TAG_LENGTH]; /* a tag path or its last part, NUL-terminated */
	/* protocol 25: its ammunition, scarce: its magazine loaded, and this
	share (0..1) of the spare rounds one in the map has; an energy weapon's
	battery is spent by as much as it falls short */
	float    spare;
	uint32_t reserved;
} cr_msg_cache_place;

typedef struct cr_msg_cache_taken
{
	cr_msg_header header;
	uint32_t id;
	uint32_t reserved[3];
} cr_msg_cache_taken;

typedef struct cr_msg_hello
{
	cr_msg_header header;
	uint32_t protocol_version;
	uint32_t pid;
	char     build[48];  /* free text, NUL-terminated */
} cr_msg_hello;

typedef struct cr_msg_teleport
{
	cr_msg_header header;
	cr_vec3  position;   /* Skyrim units */
	float    yaw;        /* Skyrim heading */
} cr_msg_teleport;

typedef struct cr_msg_log
{
	cr_msg_header header;
	char     text[120];
} cr_msg_log;

typedef struct cr_msg_collision_reset
{
	cr_msg_header header;
	uint32_t epoch;      /* regions from older epochs are stale */
	uint32_t world_generation; /* the world context (generation) this collision is for;
	                              Halo builds nothing until both have arrived. 0: any */
} cr_msg_collision_reset;

typedef struct cr_msg_collision_tris
{
	cr_msg_header header;
	uint32_t epoch;
	int32_t  rx, ry, rz; /* region: floor(skyrim position / CR_REGION_UNITS) */
	uint32_t total;      /* the region's triangle count */
	uint32_t first;      /* this message's first triangle */
	uint32_t count;      /* triangles in this message */
	uint32_t reserved;
	cr_triangle tris[CR_TRIS_PER_MESSAGE]; /* only count are sent */
} cr_msg_collision_tris;

#define CR_COLLISION_TRIS_SIZE(count) \
	((uint32_t)__builtin_offsetof(cr_msg_collision_tris, tris) + (uint32_t)(count) * (uint32_t)sizeof(cr_triangle))

/* ---- frames (H->S, docs §9) -------------------------------------------- */

/* Halo's pictures of what it draws over Skyrim's, in two layers:
- SCREEN: Chief's arms and weapon, and the HUD; over everything.
- WORLD: projectiles, grenades, effects, decals, objects; with each
  pixel's view depth (along the camera's forward, Halo world units; huge
  where nothing was drawn), so Skyrim hides what is behind its own.
Colours are RGBA8, premultiplied (draw with ONE, INV_SRC_ALPHA), on
transparent black; depth is float32. Top row first, rows of width
elements. In a slot: the screen layer at 0, the world layer at
CR_FRAME_LAYER_BYTES, its depth at 2 * CR_FRAME_LAYER_BYTES; the world
layer and depth only if the header says CR_FRAME_WORLD. With CR_FRAME_MASK
(protocol 21), at 3 * CR_FRAME_LAYER_BYTES: the screen layer's weapon share,
one byte a pixel (255: all of the pixel's colour is Chief's arms and weapon,
0: the HUD's, or nothing), so Skyrim grades the weapon as its own picture
and leaves the HUD alone.

Halo writes the slots round-robin, each under its own seqlock, and then
names it the latest. Skyrim copies the latest slot out and checks its seq
again: a slot changed meanwhile is torn and dropped (Halo would have to
draw two more frames during one copy). Nobody owns a slot, so either side
can restart at any time. */
#define CR_FRAME_SLOTS      3u
#define CR_FRAME_MAX_WIDTH  2560u /* larger screens get a smaller picture, scaled up */
#define CR_FRAME_MAX_HEIGHT 1440u
#define CR_FRAME_LAYER_BYTES (CR_FRAME_MAX_WIDTH * CR_FRAME_MAX_HEIGHT * 4u)
#define CR_FRAME_MASK_BYTES  (CR_FRAME_MAX_WIDTH * CR_FRAME_MAX_HEIGHT)
#define CR_FRAME_BYTES      (3u * CR_FRAME_LAYER_BYTES + CR_FRAME_MASK_BYTES)

typedef struct cr_frame_header
{
	uint32_t seq;           /* odd while Halo writes the slot */
	uint32_t width;         /* pixels */
	uint32_t height;
	uint32_t frame;         /* Halo's frame count */
	uint32_t camera_frame;  /* the cr_camera.frame drawn through; 0: Halo's own camera */
	uint32_t time_us;       /* Halo's clock when drawn (wraps) */
	uint32_t flags;         /* CR_FRAME_* */
	float    tangent_x;     /* Halo's projection: the view's half-width at distance 1 */
	float    tangent_y;     /* and its half-height (0: unknown). With the camera of
	                           camera_frame, Skyrim maps the world layer onto its own
	                           camera of the moment (it moved since) */
	uint32_t reserved[3];
} cr_frame_header;

#define CR_FRAME_VISIBLE 0x0001u /* reserved: always set */
#define CR_FRAME_WORLD   0x0002u /* the world layer has something (else it is all transparent) */
#define CR_FRAME_MASK    0x0004u /* the screen layer's weapon share follows (protocol 21) */

typedef struct cr_frames
{
	uint32_t latest;        /* the newest complete slot + 1; 0: none yet */
	uint32_t published;     /* frames published (wraps) */
	uint32_t reserved[6];
	cr_frame_header slots[CR_FRAME_SLOTS];
	uint32_t pad[4];        /* pixels start on a 64-byte line */
	uint8_t  pixels[CR_FRAME_SLOTS][CR_FRAME_BYTES];
} cr_frames;

/* ---- the whole mapping ------------------------------------------------- */

typedef struct cr_shared
{
	/* header */
	uint32_t magic;
	uint32_t version;
	uint32_t total_size;
	uint32_t reserved0;

	uint32_t skyrim_pid;
	uint32_t halo_pid;
	uint32_t skyrim_state;      /* CR_SIDE_* */
	uint32_t halo_state;
	uint32_t skyrim_heartbeat;  /* milliseconds, the writer's own clock */
	uint32_t halo_heartbeat;
	uint32_t reserved1[6];

	/* slots */
	cr_slot_world_context world_context;  /* S->H */
	cr_slot_input         input;          /* S->H */
	cr_slot_player_state  player_state;   /* H->S */
	cr_slot_skyrim_player skyrim_player;  /* S->H */

	/* rings */
	cr_ring to_halo;
	cr_ring to_skyrim;

	/* protocol 6: after the rings, so the offsets above stay */
	cr_slot_display display;              /* S->H */
	cr_slot_camera  camera;               /* S->H, protocol 7 */
	cr_slot_actors  actors;               /* S->H, protocol 8 */
	uint32_t reserved2[8];                /* the frames start on a 64-byte line */
	cr_frames frames;                     /* H->S */
} cr_shared;

/* ---- seqlock helpers ---------------------------------------------------- */

static inline void cr_slot_write_begin(uint32_t *seq)
{
	CR_STORE_REL(seq, *seq + 1u);
	CR_FENCE_REL();
}

static inline void cr_slot_write_end(uint32_t *seq)
{
	CR_STORE_REL(seq, *seq + 1u);
}

/* Copies a slot's value; returns its sequence (even), or 0 if the writer
kept it busy or nothing has been written yet. */
static inline uint32_t cr_slot_read(const uint32_t *seq, void *dst, const void *src, uint32_t size)
{
	int attempt;

	for (attempt = 0; attempt < 64; attempt++)
	{
		uint32_t before = CR_LOAD_ACQ(seq);
		uint32_t i;

		if (before & 1u)
			continue;
		for (i = 0; i < size; i++)
			((volatile uint8_t *)dst)[i] = ((const volatile uint8_t *)src)[i];
		CR_FENCE_ACQ();
		if (CR_LOAD_ACQ(seq) == before)
			return before;
	}
	return 0;
}

#define CR_SLOT_WRITE(slot, val)                  \
	do                                            \
	{                                             \
		cr_slot_write_begin(&(slot)->seq);        \
		(slot)->value = (val);                    \
		cr_slot_write_end(&(slot)->seq);          \
	} while (0)

#define CR_SLOT_READ(slot, out) \
	cr_slot_read(&(slot)->seq, (out), &(slot)->value, (uint32_t)sizeof((slot)->value))

/* ---- ring helpers ------------------------------------------------------- */

static inline uint32_t cr_align8(uint32_t n)
{
	return (n + 7u) & ~7u;
}

/* Producer. Returns 1 if written, 0 if the ring is full. */
static inline int cr_ring_push(cr_ring *ring, uint16_t type, const void *msg, uint32_t size)
{
	uint32_t total = cr_align8(size);
	uint32_t head = ring->head; /* only we write it */
	uint32_t tail = CR_LOAD_ACQ(&ring->tail);
	uint32_t offset = head % CR_RING_BYTES;
	uint32_t room_to_end = CR_RING_BYTES - offset;
	uint32_t needed = total;
	uint32_t i;
	cr_msg_header *header;

	if (size < sizeof(cr_msg_header) || total > 0xFFF8u)
		return 0;
	if (room_to_end < total)
		needed += room_to_end; /* skip the tail end of the buffer */
	if (CR_RING_BYTES - (head - tail) < needed)
		return 0;

	if (room_to_end < total)
	{
		if (room_to_end >= sizeof(cr_msg_header))
		{
			cr_msg_header *wrap = (cr_msg_header *)&ring->data[offset];
			wrap->type = CR_MSG_WRAP;
			wrap->size = 0;
			wrap->reserved = 0;
		}
		head += room_to_end;
		offset = 0;
	}

	for (i = 0; i < size; i++)
		ring->data[offset + i] = ((const uint8_t *)msg)[i];
	header = (cr_msg_header *)&ring->data[offset];
	header->type = type;
	header->size = (uint16_t)total;
	CR_STORE_REL(&ring->head, head + total);
	return 1;
}

/* Consumer. Copies the next message (up to capacity bytes) and returns its
type, or -1 if the ring is empty. */
static inline int cr_ring_pop(cr_ring *ring, void *out, uint32_t capacity)
{
	for (;;)
	{
		uint32_t tail = ring->tail; /* only we write it */
		uint32_t head = CR_LOAD_ACQ(&ring->head);
		uint32_t offset = tail % CR_RING_BYTES;
		uint32_t room_to_end = CR_RING_BYTES - offset;
		const cr_msg_header *header;
		uint32_t size, i;

		if (head == tail)
			return -1;
		if (room_to_end < sizeof(cr_msg_header))
		{
			CR_STORE_REL(&ring->tail, tail + room_to_end);
			continue;
		}
		header = (const cr_msg_header *)&ring->data[offset];
		if (header->type == CR_MSG_WRAP && header->size == 0)
		{
			CR_STORE_REL(&ring->tail, tail + room_to_end);
			continue;
		}
		size = header->size;
		for (i = 0; i < size && i < capacity; i++)
			((uint8_t *)out)[i] = ring->data[offset + i];
		{
			int type = header->type;
			CR_STORE_REL(&ring->tail, tail + size);
			return type;
		}
	}
}

/* ---- coordinates (docs §4) --------------------------------------------- */

static inline cr_vec3 cr_sky_to_halo(cr_vec3 sky, cr_vec3 origin)
{
	cr_vec3 r;
	r.x = (sky.x - origin.x) / CR_SKY_UNITS_PER_WU;
	r.y = (sky.y - origin.y) / CR_SKY_UNITS_PER_WU;
	r.z = (sky.z - origin.z) / CR_SKY_UNITS_PER_WU;
	return r;
}

static inline cr_vec3 cr_halo_to_sky(cr_vec3 halo, cr_vec3 origin)
{
	cr_vec3 r;
	r.x = halo.x * CR_SKY_UNITS_PER_WU + origin.x;
	r.y = halo.y * CR_SKY_UNITS_PER_WU + origin.y;
	r.z = halo.z * CR_SKY_UNITS_PER_WU + origin.z;
	return r;
}

/* Skyrim heading: 0 = +Y (north), clockwise. Halo yaw: 0 = +X,
counter-clockwise. Both radians, Halo's in [0, 2pi). */
#define CR_PI 3.14159265358979323846f

static inline float cr_wrap_2pi(float a)
{
	const float two_pi = 2.0f * CR_PI;
	while (a < 0.0f) a += two_pi;
	while (a >= two_pi) a -= two_pi;
	return a;
}

static inline float cr_sky_heading_to_halo_yaw(float heading)
{
	return cr_wrap_2pi(CR_PI * 0.5f - heading);
}

static inline float cr_halo_yaw_to_sky_heading(float yaw)
{
	return cr_wrap_2pi(CR_PI * 0.5f - yaw);
}

/* Directions are scale-free, so only points need the conversion above. */

/* ---- layout pins (both builds compile these) --------------------------- */

#ifdef __cplusplus
#define CR_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define CR_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

CR_STATIC_ASSERT(sizeof(cr_vec3) == 12, "cr_vec3");
CR_STATIC_ASSERT(sizeof(cr_world_context) == 48, "cr_world_context");
CR_STATIC_ASSERT(sizeof(cr_input) == 56, "cr_input");
CR_STATIC_ASSERT(__builtin_offsetof(cr_input, yaw_total) == 40, "cr_input.yaw_total");
CR_STATIC_ASSERT(sizeof(cr_player_state) == 88, "cr_player_state");
CR_STATIC_ASSERT(sizeof(cr_slot_world_context) == 56, "cr_slot_world_context");
CR_STATIC_ASSERT(sizeof(cr_slot_input) == 64, "cr_slot_input");
CR_STATIC_ASSERT(sizeof(cr_slot_player_state) == 96, "cr_slot_player_state");
CR_STATIC_ASSERT(sizeof(cr_skyrim_player) == 72, "cr_skyrim_player");
CR_STATIC_ASSERT(sizeof(cr_slot_skyrim_player) == 80, "cr_slot_skyrim_player");
CR_STATIC_ASSERT(sizeof(cr_msg_header) == 8, "cr_msg_header");
CR_STATIC_ASSERT(sizeof(cr_msg_hello) == 64, "cr_msg_hello");
CR_STATIC_ASSERT(sizeof(cr_msg_teleport) == 24, "cr_msg_teleport");
CR_STATIC_ASSERT(sizeof(cr_msg_log) == 128, "cr_msg_log");
CR_STATIC_ASSERT(sizeof(cr_triangle) == 40, "cr_triangle");
CR_STATIC_ASSERT(sizeof(cr_msg_collision_reset) == 16, "cr_msg_collision_reset");
CR_STATIC_ASSERT(__builtin_offsetof(cr_msg_collision_tris, tris) == 40, "cr_msg_collision_tris.tris");
CR_STATIC_ASSERT(CR_COLLISION_TRIS_SIZE(CR_TRIS_PER_MESSAGE) <= 0xFFF8u, "a full collision message fits a ring message");
CR_STATIC_ASSERT(sizeof(cr_ring) == 128 + CR_RING_BYTES, "cr_ring");
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, world_context) == 64, "cr_shared.world_context");
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, to_halo) == 360, "cr_shared.to_halo");
CR_STATIC_ASSERT(sizeof(cr_display) == 16, "cr_display");
CR_STATIC_ASSERT(sizeof(cr_slot_display) == 24, "cr_slot_display");
CR_STATIC_ASSERT(sizeof(cr_frame_header) == 48, "cr_frame_header");
CR_STATIC_ASSERT(sizeof(cr_camera) == 64, "cr_camera");
CR_STATIC_ASSERT(sizeof(cr_slot_camera) == 72, "cr_slot_camera");
CR_STATIC_ASSERT(sizeof(cr_hitbox) == 32, "cr_hitbox");
CR_STATIC_ASSERT(sizeof(cr_actor) == 40, "cr_actor");
CR_STATIC_ASSERT(sizeof(cr_slot_actors) == 16 + 40 * CR_ACTORS_MAX + 8 + 32 * CR_HITBOXES_MAX, "cr_slot_actors");
CR_STATIC_ASSERT(sizeof(cr_msg_hit_actor) == 40, "cr_msg_hit_actor");
CR_STATIC_ASSERT(sizeof(cr_msg_player_hurt) == 40, "cr_msg_player_hurt");
CR_STATIC_ASSERT(sizeof(cr_msg_player_died) == 16, "cr_msg_player_died");
CR_STATIC_ASSERT(sizeof(cr_msg_chief_heal) == 16, "cr_msg_chief_heal");
CR_STATIC_ASSERT(sizeof(cr_msg_explosion) == 32, "cr_msg_explosion");
CR_STATIC_ASSERT(sizeof(cr_msg_shot) == 40, "cr_msg_shot");
CR_STATIC_ASSERT(sizeof(cr_msg_cache_place) == 104, "cr_msg_cache_place");
CR_STATIC_ASSERT(sizeof(cr_msg_cache_taken) == 24, "cr_msg_cache_taken");
CR_STATIC_ASSERT(sizeof(cr_msg_give_weapon) == 16 + CR_WEAPON_NAME_LENGTH, "cr_msg_give_weapon");
CR_STATIC_ASSERT(sizeof(cr_msg_debug) == 16, "cr_msg_debug");
CR_STATIC_ASSERT(sizeof(cr_msg_flashlight) == 40, "cr_msg_flashlight");
CR_STATIC_ASSERT(sizeof(cr_msg_key_names) == 8 + 12 * 16, "cr_msg_key_names");
CR_STATIC_ASSERT(sizeof(cr_msg_lighting) == 8 + 4 * 12 + 4 + 4 * 28 + 12 + 16, "cr_msg_lighting");
CR_STATIC_ASSERT(sizeof(cr_chief_weapon) == 80, "cr_chief_weapon");
CR_STATIC_ASSERT(sizeof(cr_chief_state) == 40 + 4 * 80, "cr_chief_state");
CR_STATIC_ASSERT(sizeof(cr_msg_chief_state) == 8 + 360, "cr_msg_chief_state");
CR_STATIC_ASSERT(__builtin_offsetof(cr_frames, pixels) == 192, "cr_frames.pixels");
#define CR_OFFSET_DISPLAY (360u + 2u * (128u + CR_RING_BYTES))
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, display) == CR_OFFSET_DISPLAY, "cr_shared.display");
#define CR_SLOT_ACTORS_BYTES (16u + 40u * CR_ACTORS_MAX + 8u + 32u * CR_HITBOXES_MAX)
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, frames) == CR_OFFSET_DISPLAY + 96u + CR_SLOT_ACTORS_BYTES + 32u, "cr_shared.frames");
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, frames) % 64u == 0, "cr_shared.frames: on a line");
CR_STATIC_ASSERT(sizeof(cr_shared) == CR_OFFSET_DISPLAY + 96u + CR_SLOT_ACTORS_BYTES + 32u + 192u + CR_FRAME_SLOTS * CR_FRAME_BYTES, "cr_shared");

#ifdef __cplusplus
}
#endif

#endif /* CHIEFRIM_PROTOCOL_H */
