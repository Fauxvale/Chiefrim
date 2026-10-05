/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM.C

The Halo side of Chiefrim (Chiefrim/docs/DESIGN.md):

- LINK: creates /dev/shm/chiefrim_v1 (chiefrim_protocol.h), keeps a
  heartbeat, reads the Skyrim side's world context and events, and publishes
  the player state every frame.
- WORLD: Skyrim's collision becomes Halo's, as a collision BSP built at
  runtime (chiefrim_world.c, chiefrim_bsp.c; docs §5). Until Skyrim's
  triangles arrive, a flat floor at the Skyrim player's feet.
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
#include "render/render_cameras.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "units/bipeds.h"
#include "units/biped_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

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
	boolean publishing;           /* the player state is going out */
	real base_field_of_view;      /* Chief's unit's unzoomed camera FOV (radians) */
	cr_vec3 placement_position;   /* Skyrim units */
	float placement_heading;
	boolean lost_unit;            /* Chief died (or went): a respawn follows */
	boolean have_safe;            /* where Chief last stood on ground (Skyrim units) */
	boolean have_last_feet;       /* the floor guard's last position of Chief (world units) */
	real_point3d last_feet;
	long floor_guard_count;
	cr_vec3 safe_position;
	float safe_heading;
	uint32_t ticks;
	uint32_t last_skyrim_heartbeat;
	uint32_t last_skyrim_heartbeat_change;
	uint32_t skyrim_pid;          /* from its hello */
} chiefrim;

/* ---------- private code */

static uint32_t chiefrim_now_ms(void)
{
	return (uint32_t)system_milliseconds();
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
	chiefrim.have_last_feet = FALSE;
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
	if (floor_moved || world.origin.x != chiefrim.world.origin.x || world.origin.y != chiefrim.world.origin.y)
		chiefrim_world_reset(world.origin, (world.floor_z - world.origin.z) / CR_SKY_UNITS_PER_WU);
	chiefrim.world = world;
	chiefrim.world_generation = world.generation;
	chiefrim.world_valid = TRUE;
	error(_error_silent, "chiefrim: world %08X%s, origin (%.1f, %.1f, %.1f), floor %.1f, field of view %.1f, Chief's height %.0f",
		world.world_id, world.is_interior ? " (interior)" : "",
		world.origin.x, world.origin.y, world.origin.z, world.floor_z, world.field_of_view, world.chief_height);

	/* Placing Chief is the Teleport message's job (Skyrim sends one with each
	new world), never a side effect of seeing the world again. */
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
	static unsigned long buffer[0x10000 / sizeof(unsigned long)]; /* a full collision message */
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
			chiefrim.skyrim_pid = hello->pid;
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
			chiefrim.have_safe = TRUE;
			chiefrim.safe_position = teleport->position;
			chiefrim.safe_heading = teleport->yaw;
			break;
		}
		case CR_MSG_COLLISION_RESET:
		case CR_MSG_COLLISION_TRIS:
			chiefrim_world_message(type, buffer);
			break;
		default:
			break;
		}
	}
}

/* Chief's size from Skyrim (docs §7): Halo's Chief is 0.7 wu (~150 Skyrim
units, 7 ft in armour), too tall for doorways and ledges Skyrim's people walk
under. His biped definition's heights scale to the height Skyrim asks for;
Halo reads them every tick (bipeds.c), so collision, crouching and his eyes,
and so Skyrim's camera, follow. The definition is the loaded map's tag data:
its own values are kept, and taken again when a map load replaces them. */
static void chiefrim_apply_chief_height(long unit_index)
{
	static struct biped_definition *scaled = NULL;
	static real original[4];  /* standing and crouching collision, then camera */
	static real written = -1.0f;
	struct biped_definition *definition;
	real target, scale;

	if (unit_get(unit_index)->object.type != _object_type_biped)
		return;
	definition = biped_definition_get(biped_get(unit_index)->definition_index);
	if (definition != scaled || definition->biped.collision_height_standing != written)
	{
		/* a definition we haven't touched (or the map reloaded it) */
		original[0] = definition->biped.collision_height_standing;
		original[1] = definition->biped.collision_height_crouching;
		original[2] = definition->biped.standing_camera_height;
		original[3] = definition->biped.crouching_camera_height;
		scaled = definition;
		written = original[0];
	}
	if (original[0] <= 0.01f)
		return;

	target = chiefrim.world.chief_height > 1.0f ? chiefrim.world.chief_height / CR_SKY_UNITS_PER_WU : original[0];
	target = PIN(target, 0.2f, 1.0f); /* a hobbit to an ogre, in world units */
	scale = target / original[0];
	if (fabsf(definition->biped.collision_height_standing - original[0] * scale) < 0.0001f)
		return;
	definition->biped.collision_height_standing = original[0] * scale;
	definition->biped.collision_height_crouching = original[1] * scale;
	definition->biped.standing_camera_height = original[2] * scale;
	definition->biped.crouching_camera_height = original[3] * scale;
	written = definition->biped.collision_height_standing;
	error(_error_silent, "chiefrim: Chief is %.0f Skyrim units tall (Halo's own: %.0f); eyes at %.0f",
		target * CR_SKY_UNITS_PER_WU, original[0] * CR_SKY_UNITS_PER_WU,
		definition->biped.standing_camera_height * CR_SKY_UNITS_PER_WU);
}

/* Places Chief where he last stood on ground (over a respawn or a fall). */
static void chiefrim_return_to_safe(void)
{
	chiefrim.placement_position = chiefrim.safe_position;
	chiefrim.placement_position.z += 16.0f; /* settle onto it, not into it */
	chiefrim.placement_heading = chiefrim.safe_heading;
	chiefrim.placement_pending = TRUE;
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
	{
		if (chiefrim.publishing)
		{
			error(_error_silent, "chiefrim: stopped publishing the player (%s)",
				unit_index == NONE ? "no unit" : "no world");
			chiefrim.publishing = FALSE;
			chiefrim.lost_unit = unit_index == NONE;
		}
		return;
	}
	if (!chiefrim.publishing)
	{
		error(_error_silent, "chiefrim: publishing the player");
		chiefrim.publishing = TRUE;
		if (chiefrim.lost_unit && chiefrim.have_safe && !chiefrim.placement_pending)
		{
			/* Halo respawned Chief at the level's own spawn point, which the
			Skyrim player would follow: back to the ground he last stood on */
			error(_error_silent, "chiefrim: Chief is back; returning him to where he last stood");
			chiefrim.lost_unit = FALSE;
			chiefrim_return_to_safe();
			return;
		}
		chiefrim.lost_unit = FALSE;
	}

	chiefrim_apply_chief_height(unit_index);

	memset(&state, 0, sizeof(state));
	state.tick = chiefrim.ticks;

	object_get_origin(unit_index, &origin);
	halo.x = origin.x;
	halo.y = origin.y;
	halo.z = origin.z;
	state.position = cr_halo_to_sky(halo, chiefrim.world.origin);

	{
		struct unit_definition const *definition = unit_definition_get(unit_get(unit_index)->definition_index);

		chiefrim.base_field_of_view = definition->unit.camera_field_of_view;
	}

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
		if (state.on_ground && chiefrim_world_has_skyrim_collision() &&
			chiefrim_world_room_for(&origin, biped_definition_get(biped->definition_index)->biped.collision_height_standing))
		{
			/* on Skyrim's ground, not the stand-in floor of a new origin,
			and clear of everything (not wedged into something) */
			chiefrim.have_safe = TRUE;
			chiefrim.safe_position = state.position;
			chiefrim.safe_heading = state.yaw;
		}
		else if (chiefrim.have_safe && !chiefrim.placement_pending && chiefrim_world_below_collision(&origin))
		{
			/* below every triangle loaded around him: he fell through a hole
			in the collision. Catch him before Halo kills him. */
			error(_error_silent, "chiefrim: Chief fell through the collision (%.0f units below where he last stood); returning him",
				chiefrim.safe_position.z - state.position.z);
			chiefrim_world_dump_installed("fell through");
			chiefrim_return_to_safe();
		}
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
		/* The observer's field of view is horizontal, for a 4:3 view, and
		Halo projects with 0.85 of its tangent (main.c, render_cameras.c):
		send the vertical angle it really renders with. */
		state.vertical_fov = 2.0f * atan2f(
			0.75f * render_camera_get_adjusted_field_of_view_tangent(camera->field_of_view), 1.0f);
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
		/* a Skyrim that stalled and came back (its heartbeat moves again,
		same process): linked again, no new hello needed */
		if (!chiefrim.linked && chiefrim.skyrim_pid && CR_LOAD_ACQ(&chiefrim.shm->skyrim_pid) == chiefrim.skyrim_pid &&
			CR_LOAD_ACQ(&chiefrim.shm->skyrim_state) == CR_SIDE_READY)
		{
			chiefrim.linked = TRUE;
			error(_error_silent, "chiefrim: Skyrim is responding again");
		}
	}
	else if (chiefrim.linked && now - chiefrim.last_skyrim_heartbeat_change > CR_HEARTBEAT_TIMEOUT_MS)
	{
		/* Keep the world and Chief where they are: Skyrim says hello again
		and sends a new world and Teleport when it comes back. */
		error(_error_silent, "chiefrim: Skyrim stopped responding; waiting for it to come back");
		chiefrim.linked = FALSE;
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

real chiefrim_field_of_view_tangent_scale(void)
{
	real base = chiefrim.base_field_of_view > 0.01f ? chiefrim.base_field_of_view : DEGREES_TO_RADIANS(70.f);

	if (!chiefrim.active || !chiefrim.world_valid ||
		chiefrim.world.field_of_view < 10.0f || chiefrim.world.field_of_view > 170.0f)
	{
		return 0.0f;
	}
	/* Skyrim's angle is horizontal for 4:3, as Halo's tangent is: the
	unzoomed view gets exactly it, and zoom keeps Halo's magnification. */
	return tangent(DEGREES_TO_RADIANS(chiefrim.world.field_of_view) * 0.5f) / tangent(base * 0.5f);
}

struct cr_shared *chiefrim_shared(void)
{
	return chiefrim.active ? chiefrim.shm : NULL;
}

boolean chiefrim_linked(void)
{
	return chiefrim.active && chiefrim.linked;
}

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
	chiefrim_world_initialize();
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

	{
		long unit_index = chiefrim_local_unit();
		real_point3d chief;

		if (unit_index != NONE)
			object_get_origin(unit_index, &chief);
		chiefrim_world_update(unit_index != NONE ? &chief : NULL);
		if (unit_index != NONE && chiefrim.have_last_feet && !chiefrim.placement_pending &&
			chiefrim_world_crossed_floor(&chiefrim.last_feet, &chief))
		{
			/* Halo pushed him down through a floor (wedged against
			something, mostly): back on top of it, his fall stopped */
			struct unit_datum *unit = unit_get(unit_index);
			real_point3d back = chiefrim.last_feet;

			back.z += 0.01f;
			player_teleport(local_player_get_player_index(0), NONE, &back);
			if (unit->object.translational_velocity.k < 0.f)
				unit->object.translational_velocity.k = 0.f;
			if (chiefrim.floor_guard_count++ % 50 == 0)
			{
				error(_error_silent, "chiefrim: Halo pushed Chief down through a floor at (%.2f, %.2f, %.2f) wu; put him back (%ld times)",
					chief.x, chief.y, chief.z, chiefrim.floor_guard_count);
			}
			chief = back;
		}
		chiefrim.have_last_feet = unit_index != NONE;
		if (unit_index != NONE)
			chiefrim.last_feet = chief;
		if (unit_index != NONE)
		{
			real ground_z;

			if (chiefrim_world_settle(&chief, &ground_z))
			{
				real lift = ground_z + 0.05f - chief.z;

				chief.z += lift;
				player_teleport(local_player_get_player_index(0), NONE, &chief);
				chiefrim.last_feet = chief;
				error(_error_silent, "chiefrim: Skyrim's ground arrived above Chief's feet; lifted him %.2f wu onto it", lift);
			}
		}
	}

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
	it, so the level can spawn Chief at its own starting location; then
	Chiefrim's collision goes over it and Chief moves. */
	chiefrim_world_map_loaded();
	if (chiefrim.world_valid)
		chiefrim.placement_pending = TRUE;
}
