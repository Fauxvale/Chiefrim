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
#define CR_PROTOCOL_VERSION 4u

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
#define CR_ACTION_COUNT          11u
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
	uint32_t reserved[2];
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

CR_DECLARE_SLOT(cr_slot_world_context, cr_world_context);
CR_DECLARE_SLOT(cr_slot_input, cr_input);
CR_DECLARE_SLOT(cr_slot_player_state, cr_player_state);

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
	uint16_t flags;
} cr_triangle;

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

	/* rings */
	cr_ring to_halo;
	cr_ring to_skyrim;
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
CR_STATIC_ASSERT(sizeof(cr_world_context) == 40, "cr_world_context");
CR_STATIC_ASSERT(sizeof(cr_input) == 56, "cr_input");
CR_STATIC_ASSERT(__builtin_offsetof(cr_input, yaw_total) == 40, "cr_input.yaw_total");
CR_STATIC_ASSERT(sizeof(cr_player_state) == 88, "cr_player_state");
CR_STATIC_ASSERT(sizeof(cr_slot_world_context) == 48, "cr_slot_world_context");
CR_STATIC_ASSERT(sizeof(cr_slot_input) == 64, "cr_slot_input");
CR_STATIC_ASSERT(sizeof(cr_slot_player_state) == 96, "cr_slot_player_state");
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
CR_STATIC_ASSERT(__builtin_offsetof(cr_shared, to_halo) == 272, "cr_shared.to_halo");
CR_STATIC_ASSERT(sizeof(cr_shared) == 272 + 2 * (128 + CR_RING_BYTES), "cr_shared");

#ifdef __cplusplus
}
#endif

#endif /* CHIEFRIM_PROTOCOL_H */
