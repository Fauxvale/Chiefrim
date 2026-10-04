/*
CHIEFRIM.C

The Halo side of Chiefrim, Phase 0 (Chiefrim/docs/DESIGN.md §12):

- LINK: creates /dev/shm/chiefrim_v1 (chiefrim_protocol.h), keeps a
  heartbeat, reads the Skyrim side's world context and events, and publishes
  the player state every frame.
- WORLD: replaces the map's collision BSP with one built here. Halo's biped
  physics keeps indices of the surfaces and edges it stands on, so Skyrim's
  shape has to reach it as a real collision BSP, not as loose collision
  features. Phase 0 builds the simplest one: a single flat square at the
  Skyrim player's ground height. Every point above it is in leaf 0 (the map's
  first cluster), so nothing is ever "outside the map" (docs §5.1).
- PLAYER: when the link starts, and on each Teleport, moves Chief to the
  Skyrim player's position and heading.
- LEVEL: the host map is a campaign level (multiplayer maps start no game,
  so nobody spawns). In Chiefrim mode its scripts and AI don't run (hooks in
  game_tick), and its actors are erased once Chief exists.

Halo is authoritative for the player (docs §6); Skyrim follows PlayerState.
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "ai/ai.h"
#include "camera/observer.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/players.h"
#include "objects/objects.h"
#include "physics/collision_features.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "units/bipeds.h"
#include "units/units.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---------- constants */

/* Half the side of the Phase 0 floor, in world units (1 wu = 3.048 m). */
#define FLOOR_HALF_SIZE 2000.0f

/* ---------- structures (the collision BSP's own, from collision_bsp.c) */

struct chiefrim_collision_leaf
{
	word flags;
	short bsp2d_reference_count;
	long first_bsp2d_reference_index;
};

struct chiefrim_bsp2d_reference
{
	long plane_designator;
	long root_index;
};

typedef char chiefrim_collision_leaf_size_assert[
	sizeof(struct chiefrim_collision_leaf) == 0x08 ? 1 : -1];
typedef char chiefrim_bsp2d_reference_size_assert[
	sizeof(struct chiefrim_bsp2d_reference) == 0x08 ? 1 : -1];

/* ---------- globals */

static struct
{
	boolean active;
	boolean linked;               /* the Skyrim side has said hello */
	cr_shared *shm;
	uint32_t world_generation;    /* last world context applied */
	boolean world_valid;
	cr_world_context world;
	boolean placement_pending;    /* move Chief once a unit exists */
	boolean level_cleared;        /* the level's actors are gone */
	cr_vec3 placement_position;   /* Skyrim units */
	float placement_heading;
	uint32_t ticks;
	uint32_t last_skyrim_heartbeat;
	uint32_t last_skyrim_heartbeat_change;
} chiefrim;

/* The replacement collision BSP: one node, one leaf, one surface with four
edges, and one plane of its own. The plane list starts with a copy of the
map's planes, because the structure BSP's cluster portals index it
(structure_clusters_in_sphere); the floor's plane comes after them. */
static struct
{
	struct collision_bsp bsp;
	real_plane3d *planes;         /* the map's planes, then the floor's */
	long plane_capacity;
	long floor_plane_index;
	struct bsp3d_node nodes[1];
	struct chiefrim_collision_leaf leaves[1];
	struct chiefrim_bsp2d_reference references[1];
	struct collision_surface surfaces[1];
	struct collision_edge edges[4];
	struct collision_vertex vertices[4];
} chiefrim_floor;

/* ---------- private code */

static uint32_t chiefrim_now_ms(void)
{
	return (uint32_t)system_milliseconds();
}

static void chiefrim_tag_block(struct tag_block *block, void *address, long count)
{
	block->count = count;
	block->address = address;
	block->definition = NULL;
}

/* Fills chiefrim_floor with a square at height z (world units). Edges run
counter-clockwise seen from above (in the surface's 2D projection), the
winding the ray and sphere queries treat as inside. (The opposite of what
collision_surface_test_point2d expects: that function, used only for AI
pathfinding, has the other sign.) chiefrim_floor_self_test checks it. */
static void chiefrim_floor_build(real z)
{
	static real const corners[4][2] = {
		{ -FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE },
		{ FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE },
		{ FLOOR_HALF_SIZE, FLOOR_HALF_SIZE },
		{ -FLOOR_HALF_SIZE, FLOOR_HALF_SIZE },
	};
	struct structure_bsp *structure_bsp = global_structure_bsp_get();
	struct tag_block const *map_planes = &TAG_BLOCK_GET_ELEMENT(
		&structure_bsp->collision_bsp, 0, struct collision_bsp)->bsp3d.planes;
	long const plane = map_planes->count;
	real_plane3d *planes = chiefrim_floor.planes;
	long capacity = chiefrim_floor.plane_capacity;
	long index;

	if (capacity < plane + 1)
	{
		if (planes)
			free(planes); /* the game's debug free() rejects NULL */
		capacity = plane + 1;
		planes = (real_plane3d *)malloc(capacity * sizeof(real_plane3d));
	}
	memset(&chiefrim_floor, 0, sizeof(chiefrim_floor));
	chiefrim_floor.planes = planes;
	chiefrim_floor.plane_capacity = capacity;
	chiefrim_floor.floor_plane_index = plane;

	if (plane > 0)
		memcpy(planes, map_planes->address, plane * sizeof(real_plane3d));
	planes[plane].n.i = 0.0f;
	planes[plane].n.j = 0.0f;
	planes[plane].n.k = 1.0f;
	planes[plane].d = z;

	/* children[distance >= 0]: above the floor is leaf 0, below is solid. */
	chiefrim_floor.nodes[0].plane_designator = plane;
	chiefrim_floor.nodes[0].children[0] = NONE;
	chiefrim_floor.nodes[0].children[1] = 0 | LONG_MIN;

	chiefrim_floor.leaves[0].flags = 0;
	chiefrim_floor.leaves[0].bsp2d_reference_count = 1;
	chiefrim_floor.leaves[0].first_bsp2d_reference_index = 0;

	/* No 2D nodes: the reference's root is surface 0 itself. */
	chiefrim_floor.references[0].plane_designator = plane;
	chiefrim_floor.references[0].root_index = 0 | LONG_MIN;

	chiefrim_floor.surfaces[0].plane_designator = plane;
	chiefrim_floor.surfaces[0].first_edge_index = 0;
	chiefrim_floor.surfaces[0].flags = 0;
	chiefrim_floor.surfaces[0].breakable_surface_index = (byte)NONE;
	chiefrim_floor.surfaces[0].material_index =
		structure_bsp->collision_materials.count > 0 ? 0 : NONE;

	for (index = 0; index < 4; index++)
	{
		struct collision_vertex *vertex = &chiefrim_floor.vertices[index];
		struct collision_edge *edge = &chiefrim_floor.edges[index];

		vertex->point.x = corners[index][0];
		vertex->point.y = corners[index][1];
		vertex->point.z = z;
		vertex->first_edge_index = index;

		edge->vertex_indices[0] = index;
		edge->vertex_indices[1] = (index + 1) % 4;
		edge->edge_indices[0] = (index + 1) % 4;   /* next edge around surface 0 */
		edge->edge_indices[1] = (index + 3) % 4;   /* the open side: nothing there */
		edge->surface_indices[0] = 0;
		edge->surface_indices[1] = NONE;
	}

	chiefrim_tag_block(&chiefrim_floor.bsp.bsp3d.nodes, chiefrim_floor.nodes, 1);
	chiefrim_tag_block(&chiefrim_floor.bsp.bsp3d.planes, planes, plane + 1);
	chiefrim_tag_block(&chiefrim_floor.bsp.leaves, chiefrim_floor.leaves, 1);
	chiefrim_tag_block(&chiefrim_floor.bsp.bsp2d_references, chiefrim_floor.references, 1);
	chiefrim_tag_block(&chiefrim_floor.bsp.bsp2d.nodes, NULL, 0);
	chiefrim_tag_block(&chiefrim_floor.bsp.surfaces, chiefrim_floor.surfaces, 1);
	chiefrim_tag_block(&chiefrim_floor.bsp.edges, chiefrim_floor.edges, 4);
	chiefrim_tag_block(&chiefrim_floor.bsp.vertices, chiefrim_floor.vertices, 4);
}

/* Checks the floor with the engine's own queries; logs and returns FALSE
on a mistake. */
static boolean chiefrim_floor_self_test(real z)
{
	struct collision_bsp *bsp = &chiefrim_floor.bsp;
	real_point3d above = { 1.0f, 2.0f, z + 0.5f };
	real_point3d below = { 1.0f, 2.0f, z - 0.5f };
	real_point3d resting = { 1.0f, 2.0f, z + 0.1f };
	real_point3d beyond = { FLOOR_HALF_SIZE * 2.0f, 0.0f, z + 0.1f };
	real_vector3d down = { 0.0f, 0.0f, -1.0f };
	struct collision_bsp_test_vector_result result;
	struct collision_bsp_test_sphere_result *sphere =
		(struct collision_bsp_test_sphere_result *)malloc(sizeof(*sphere));
	boolean ok = TRUE;

	if (bsp3d_test_point(&bsp->bsp3d, 0, &above) != 0)
	{
		error(_error_silent, "chiefrim: floor self-test: a point above is not in leaf 0");
		ok = FALSE;
	}
	if (bsp3d_test_point(&bsp->bsp3d, 0, &below) != NONE)
	{
		error(_error_silent, "chiefrim: floor self-test: a point below is not solid");
		ok = FALSE;
	}
	/* The query biped movement depends on (collision_get_features_in_sphere). */
	if (!collision_bsp_test_sphere(bsp, 0, NULL, &resting, 0.25f, sphere) ||
		sphere->surface_count != 1 || sphere->surface_indices[0] != 0)
	{
		error(_error_silent, "chiefrim: floor self-test: a sphere resting on the floor does not touch it (winding)");
		ok = FALSE;
	}
	if (collision_bsp_test_sphere(bsp, 0, NULL, &beyond, 0.25f, sphere))
	{
		error(_error_silent, "chiefrim: floor self-test: a sphere beyond the edge touches the floor");
		ok = FALSE;
	}
	free(sphere);
	if (!collision_bsp_test_vector(
			FLAG(_collision_test_front_facing_surfaces_bit),
			bsp, 0, NULL, &above, &down, REAL_MAX, &result) ||
		fabsf(result.t - 0.5f) > 0.001f || result.surface_index != 0)
	{
		error(_error_silent, "chiefrim: floor self-test: a ray down does not hit the floor at t=0.5");
		ok = FALSE;
	}

	return ok;
}

/* The floor's height in world units: the world context's floor_z, relative
to its origin. */
static real chiefrim_floor_height(void)
{
	if (!chiefrim.world_valid)
		return 0.0f;
	return (chiefrim.world.floor_z - chiefrim.world.origin.z) / CR_SKY_UNITS_PER_WU;
}

static void chiefrim_install_floor(void)
{
	real z = chiefrim_floor_height();

	chiefrim_floor_build(z);
	if (!chiefrim_floor_self_test(z))
	{
		error(_error_silent, "chiefrim: keeping the map's own collision");
		return;
	}
	scenario_override_collision_bsp(&chiefrim_floor.bsp);
	error(_error_silent, "chiefrim: collision is a flat floor at z=%.3f wu", z);
}

static long chiefrim_local_unit(void)
{
	long player_index = local_player_get_player_index(0);

	if (player_index == NONE)
		return NONE;
	return player_get(player_index)->unit_index;
}

static void chiefrim_place_player(void)
{
	long player_index = local_player_get_player_index(0);
	long unit_index = chiefrim_local_unit();
	cr_vec3 halo;
	real_point3d position;
	real_vector3d forward;
	float yaw;

	if (unit_index == NONE || !chiefrim.world_valid)
		return;

	halo = cr_sky_to_halo(chiefrim.placement_position, chiefrim.world.origin);
	position.x = halo.x;
	position.y = halo.y;
	position.z = halo.z + 0.05f; /* just above the floor */
	yaw = cr_sky_heading_to_halo_yaw(chiefrim.placement_heading);
	forward.i = cosf(yaw);
	forward.j = sinf(yaw);
	forward.k = 0.0f;

	player_teleport(player_index, NONE, &position);
	player_control_set_facing(0, &forward);
	chiefrim.placement_pending = FALSE;
	error(_error_silent, "chiefrim: placed Chief at (%.2f, %.2f, %.2f) wu, yaw %.3f",
		position.x, position.y, position.z, yaw);
}

static void chiefrim_apply_world(void)
{
	cr_world_context world;
	boolean floor_moved;

	if (!CR_SLOT_READ(&chiefrim.shm->world_context, &world))
		return;
	if (chiefrim.world_valid && world.generation == chiefrim.world_generation)
		return;

	floor_moved = !chiefrim.world_valid ||
		world.floor_z - world.origin.z != chiefrim.world.floor_z - chiefrim.world.origin.z;
	chiefrim.world = world;
	chiefrim.world_generation = world.generation;
	chiefrim.world_valid = TRUE;
	error(_error_silent, "chiefrim: world %08X%s, origin (%.1f, %.1f, %.1f), floor %.1f",
		world.world_id, world.is_interior ? " (interior)" : "",
		world.origin.x, world.origin.y, world.origin.z, world.floor_z);

	if (floor_moved && global_scenario_try_and_get())
		chiefrim_install_floor();
	chiefrim.placement_pending = TRUE;
}

static void chiefrim_log_to_skyrim(char const *text)
{
	cr_msg_log message;

	memset(&message, 0, sizeof(message));
	csstrncpy(message.text, text, sizeof(message.text) - 1);
	cr_ring_push(&chiefrim.shm->to_skyrim, CR_MSG_LOG, &message, sizeof(message));
}

static void chiefrim_say_hello(void)
{
	cr_msg_hello hello;

	memset(&hello, 0, sizeof(hello));
	hello.protocol_version = CR_PROTOCOL_VERSION;
#ifdef __linux__
	hello.pid = (uint32_t)getpid();
#endif
	csstrncpy(hello.build, "halo-ce-universal + chiefrim phase 0", sizeof(hello.build) - 1);
	cr_ring_push(&chiefrim.shm->to_skyrim, CR_MSG_HELLO, &hello, sizeof(hello));
}

static void chiefrim_pump_events(void)
{
	unsigned char buffer[256];
	int type;

	while ((type = cr_ring_pop(&chiefrim.shm->to_halo, buffer, sizeof(buffer))) >= 0)
	{
		switch (type)
		{
		case CR_MSG_HELLO:
		{
			cr_msg_hello const *hello = (cr_msg_hello const *)buffer;

			if (hello->protocol_version != CR_PROTOCOL_VERSION)
			{
				error(_error_silent, "chiefrim: Skyrim side speaks protocol %u, this is %u",
					hello->protocol_version, CR_PROTOCOL_VERSION);
				break;
			}
			chiefrim.linked = TRUE;
			error(_error_silent, "chiefrim: linked to Skyrim (pid %u, %s)", hello->pid, hello->build);
			chiefrim_say_hello();
			break;
		}
		case CR_MSG_TELEPORT:
		{
			cr_msg_teleport const *teleport = (cr_msg_teleport const *)buffer;

			chiefrim.placement_position = teleport->position;
			chiefrim.placement_heading = teleport->yaw;
			chiefrim.placement_pending = TRUE;
			break;
		}
		default:
			break;
		}
	}
}

static void chiefrim_publish_player(void)
{
	long unit_index = chiefrim_local_unit();
	struct observer_result const *camera;
	cr_player_state state;
	real_point3d origin;
	real_vector3d aim;
	cr_vec3 halo;

	if (unit_index == NONE || !chiefrim.world_valid)
		return;

	memset(&state, 0, sizeof(state));
	state.tick = chiefrim.ticks;

	object_get_origin(unit_index, &origin);
	halo.x = origin.x;
	halo.y = origin.y;
	halo.z = origin.z;
	state.position = cr_halo_to_sky(halo, chiefrim.world.origin);

	unit_get_aiming_vector(unit_index, &aim);
	state.yaw = cr_halo_yaw_to_sky_heading(atan2f(aim.j, aim.i));
	state.pitch = -asinf(aim.k < -1.0f ? -1.0f : aim.k > 1.0f ? 1.0f : aim.k);

	{
		struct unit_datum *unit = unit_get(unit_index);

		state.body_fraction = unit->object.body_vitality;
		state.shield_fraction = unit->object.shield_vitality;
		if (unit->object.body_vitality <= 0.0f)
			state.pose = CR_POSE_DEAD;
	}
	if (unit_get(unit_index)->object.type == _object_type_biped && state.pose != CR_POSE_DEAD)
	{
		struct biped_datum *biped = biped_get(unit_index);

		state.on_ground = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
		if (!state.on_ground)
			state.pose = CR_POSE_AIRBORNE;
		else if (biped->biped.crouch > 0.5f)
			state.pose = CR_POSE_CROUCHING;
		else
			state.pose = CR_POSE_STANDING;
	}

	camera = observer_get_camera(0);
	if (camera)
	{
		halo.x = camera->position.x;
		halo.y = camera->position.y;
		halo.z = camera->position.z;
		state.eye = cr_halo_to_sky(halo, chiefrim.world.origin);
		state.forward.x = camera->forward.i;
		state.forward.y = camera->forward.j;
		state.forward.z = camera->forward.k;
		state.up.x = camera->up.i;
		state.up.y = camera->up.j;
		state.up.z = camera->up.k;
		state.vertical_fov = camera->field_of_view;
	}

	CR_SLOT_WRITE(&chiefrim.shm->player_state, state);
}

static void chiefrim_watch_skyrim(void)
{
	uint32_t now = chiefrim_now_ms();
	uint32_t heartbeat = CR_LOAD_ACQ(&chiefrim.shm->skyrim_heartbeat);

	if (heartbeat != chiefrim.last_skyrim_heartbeat)
	{
		chiefrim.last_skyrim_heartbeat = heartbeat;
		chiefrim.last_skyrim_heartbeat_change = now;
	}
	else if (chiefrim.linked && now - chiefrim.last_skyrim_heartbeat_change > CR_HEARTBEAT_TIMEOUT_MS)
	{
		error(_error_silent, "chiefrim: Skyrim stopped responding; waiting for it to come back");
		chiefrim.linked = FALSE;
		chiefrim.world_valid = FALSE;
	}
}


/* CHIEFRIM_DEBUG=1: logs Chief's pill and the collision queries around him
for the frames after each placement. */
static void chiefrim_debug_collision(void)
{
	static int frames_left = -1;
	long unit_index = chiefrim_local_unit();
	real_point3d base, top;
	real height, width;
	struct collision_feature_list *features;
	struct collision_bsp_test_sphere_result *sphere;
	struct collision_result ray;
	real_vector3d down = { 0.0f, 0.0f, -2.0f };
	boolean sphere_hit, features_hit, ray_hit;

	if (frames_left < 0)
	{
		char const *flag = getenv("CHIEFRIM_DEBUG");
		frames_left = flag && !strcmp(flag, "1") ? 0 : -2;
	}
	if (frames_left == -2 || unit_index == NONE || unit_get(unit_index)->object.type != _object_type_biped)
		return;
	if (chiefrim.placement_pending)
		frames_left = 40;
	if (frames_left <= 0)
		return;
	frames_left--;

	features = (struct collision_feature_list *)malloc(sizeof(*features));
	sphere = (struct collision_bsp_test_sphere_result *)malloc(sizeof(*sphere));
	biped_get_physics_pill(unit_index, &base, &height, &width);
	sphere_hit = collision_bsp_test_sphere(global_collision_bsp_get(), 0, NULL, &base, width + 0.05f, sphere);
	features_hit = collision_get_features_in_sphere(
		FLAG(_collision_test_structure_bit), &base, width + height * 0.5f + 0.1f, height, width, unit_index, features);
	top = base;
	top.z += 1.0f;
	ray_hit = collision_test_vector(FLAG(_collision_test_structure_bit), &top, &down, NONE, &ray);
	error(_error_silent,
		"chiefrim debug: base z %.3f h %.3f w %.3f flags %08lX | sphere %d (s%ld e%ld v%ld l%ld) | features %d (%d/%d/%d) | ray %d t %.3f type %d",
		base.z, height, width, biped_get(unit_index)->biped.flags,
		sphere_hit, sphere->surface_count, sphere->edge_count, sphere->vertex_count, sphere->leaf_count,
		features_hit, features->count[0], features->count[1], features->count[2],
		ray_hit, ray.t, ray.type);
	free(features);
	free(sphere);
}

/* ---------- public code */

boolean chiefrim_active(void)
{
	return chiefrim.active;
}

void chiefrim_initialize(void)
{
	char const *flag = getenv("CHIEFRIM");

	memset(&chiefrim, 0, sizeof(chiefrim));
	if (!flag || strcmp(flag, "1") != 0)
		return;

#ifdef __linux__
	{
		int fd = shm_open(CR_SHM_NAME, O_RDWR | O_CREAT | O_TRUNC, 0600);
		void *address;

		if (fd < 0 || ftruncate(fd, sizeof(cr_shared)) != 0)
		{
			error(_error_silent, "chiefrim: could not create %s", CR_SHM_LINUX_PATH);
			if (fd >= 0)
				close(fd);
			return;
		}
		address = mmap(NULL, sizeof(cr_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (address == MAP_FAILED)
		{
			error(_error_silent, "chiefrim: could not map %s", CR_SHM_LINUX_PATH);
			return;
		}
		chiefrim.shm = (cr_shared *)address;
	}
#else
	error(_error_silent, "chiefrim: this platform has no link yet (docs §10)");
	return;
#endif

	memset(chiefrim.shm, 0, sizeof(cr_shared));
	chiefrim.shm->version = CR_PROTOCOL_VERSION;
	chiefrim.shm->total_size = sizeof(cr_shared);
#ifdef __linux__
	chiefrim.shm->halo_pid = (uint32_t)getpid();
#endif
	CR_STORE_REL(&chiefrim.shm->halo_state, CR_SIDE_READY);
	CR_STORE_REL(&chiefrim.shm->magic, CR_MAGIC); /* last: the mapping is valid */
	chiefrim.active = TRUE;
	atexit(chiefrim_dispose); /* the game leaves through several exits */
	error(_error_silent, "chiefrim: active, %s ready (protocol %u)", CR_SHM_LINUX_PATH, CR_PROTOCOL_VERSION);
}

void chiefrim_frame(void)
{
	if (!chiefrim.active)
		return;

	if (!chiefrim.level_cleared && chiefrim_local_unit() != NONE)
	{
		ai_erase(NONE, NONE, NONE, TRUE);
		chiefrim.level_cleared = TRUE;
		error(_error_silent, "chiefrim: erased the level's actors");
	}

	chiefrim.ticks++;
	CR_STORE_REL(&chiefrim.shm->halo_heartbeat, chiefrim_now_ms());
	chiefrim_watch_skyrim();
	chiefrim_pump_events();
	chiefrim_apply_world();

	chiefrim_debug_collision();
	if (chiefrim.placement_pending)
		chiefrim_place_player();
	chiefrim_publish_player();
}

void chiefrim_dispose(void)
{
	if (!chiefrim.active)
		return;

	CR_STORE_REL(&chiefrim.shm->halo_state, CR_SIDE_CLOSING);
#ifdef __linux__
	munmap(chiefrim.shm, sizeof(cr_shared));
	shm_unlink(CR_SHM_NAME);
#endif
	chiefrim.shm = NULL;
	chiefrim.active = FALSE;
}

void chiefrim_structure_bsp_loaded(void)
{
	if (!chiefrim.active)
		return;

	chiefrim.level_cleared = FALSE;

	/* A new BSP brings its own collision. Until Skyrim's world arrives, keep
	it, so the level can spawn Chief at its own starting location; then put
	the floor over it and move him. */
	if (chiefrim.world_valid)
	{
		chiefrim_install_floor();
		chiefrim.placement_pending = TRUE;
	}
}
