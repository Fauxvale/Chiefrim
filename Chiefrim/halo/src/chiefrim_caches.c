/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_CACHES.C

Weapon caches (Chiefrim/docs/DESIGN.md §8.4): weapons lying in Skyrim's
bandit camps and forts. Skyrim chooses them, keeps them in its co-save and
sends each one here (CR_MSG_CACHE_PLACE) for the world Halo is in now; a new
world erases Halo's loose objects, these with them, and Skyrim sends them
again. Halo lays the weapon down on what lies below its position, once its
collision has that (regions arrive over a few frames), and tells Skyrim
when Chief picks it up (CR_MSG_CACHE_TAKEN), so it stays taken.
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "physics/collisions.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

#define CHIEFRIM_CACHES 64
/* how far below its position a cache looks for something to lie on (world
units: ~210 Skyrim units: its chest's lid, the floor before its rack). Not
further: before the chest's own collision arrived, one on a watchtower
fell through to the ground below it (in game) */
#define CHIEFRIM_CACHE_DROP 1.f
/* a cache that finds nothing to lie on in this long is given up (until
Skyrim sends it again) */
#define CHIEFRIM_CACHE_PATIENCE_MS 60000u

/* ---------- globals */

static struct
{
	struct
	{
		boolean used;
		boolean placed;           /* its weapon is in the world */
		boolean taken;            /* Chief has it: a message still on its way doesn't lay it down again */
		uint32_t id;
		uint32_t generation;
		uint32_t since_ms;        /* when it came */
		cr_vec3 position;         /* Skyrim's */
		float yaw;
		float spare;
		long tag_index;
		long object_index;
		char weapon[CR_WEAPON_TAG_LENGTH];
	} caches[CHIEFRIM_CACHES];
	long logged;
} chiefrim_caches;

/* ---------- private code */

static void chiefrim_cache_taken(uint32_t id)
{
	struct cr_shared *shm = chiefrim_shared();
	cr_msg_cache_taken message;

	if (!shm || !chiefrim_linked())
		return;
	memset(&message, 0, sizeof(message));
	message.id = id;
	cr_ring_push(&shm->to_skyrim, CR_MSG_CACHE_TAKEN, &message, sizeof(message));
}

/* Scarce: its magazines loaded, and a share of the spare rounds one in the
map has; an energy weapon (no magazines) part spent */
static void chiefrim_cache_ammunition(long weapon_index, real spare)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);
	short magazine, count = (short)MIN(definition->weapon.magazines.count, NUMBEROF(weapon->weapon.magazines));

	for (magazine = 0; magazine < count; magazine++)
	{
		struct weapon_magazine_definition *limits =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, magazine, struct weapon_magazine_definition);
		struct weapon_magazine *rounds = &weapon->weapon.magazines[magazine];
		short loaded = MIN(limits->rounds_loaded_maximum, limits->rounds_total_initial);
		short extra = (short)MAX(limits->rounds_total_initial - loaded, 0);

		rounds->rounds_loaded = loaded;
		rounds->rounds_total = (short)(loaded + (short)(extra * spare + 0.5f));
	}
	if (count == 0)
		weapon->weapon.age = PIN(0.6f * (1.f - spare), 0.f, 1.f);
}

/* ---------- public code */

void chiefrim_caches_forget(void)
{
	memset(chiefrim_caches.caches, 0, sizeof(chiefrim_caches.caches));
}

void chiefrim_caches_message(cr_msg_cache_place const *message)
{
	long index, free_index = NONE;

	for (index = 0; index < CHIEFRIM_CACHES; index++)
	{
		if (chiefrim_caches.caches[index].used && chiefrim_caches.caches[index].id == message->id &&
			chiefrim_caches.caches[index].generation == message->generation)
		{
			return; /* already here */
		}
		if (!chiefrim_caches.caches[index].used && free_index == NONE)
			free_index = index;
	}
	if (free_index == NONE)
		return;
	memset(&chiefrim_caches.caches[free_index], 0, sizeof(chiefrim_caches.caches[free_index]));
	chiefrim_caches.caches[free_index].used = TRUE;
	chiefrim_caches.caches[free_index].id = message->id;
	chiefrim_caches.caches[free_index].generation = message->generation;
	chiefrim_caches.caches[free_index].since_ms = (uint32_t)system_milliseconds();
	chiefrim_caches.caches[free_index].position = message->position;
	chiefrim_caches.caches[free_index].yaw = message->yaw;
	chiefrim_caches.caches[free_index].spare = PIN(message->spare, 0.f, 1.f);
	chiefrim_caches.caches[free_index].object_index = NONE;
	memcpy(chiefrim_caches.caches[free_index].weapon, message->weapon, CR_WEAPON_TAG_LENGTH);
	chiefrim_caches.caches[free_index].weapon[CR_WEAPON_TAG_LENGTH - 1] = 0;
	chiefrim_caches.caches[free_index].tag_index = chiefrim_weapon_tag(chiefrim_caches.caches[free_index].weapon);
	if (chiefrim_caches.caches[free_index].tag_index == NONE)
	{
		error(_error_silent, "chiefrim: cache %08X: %s isn't a weapon Chief can have in the host map", message->id,
			chiefrim_caches.caches[free_index].weapon);
		chiefrim_caches.caches[free_index].used = FALSE;
	}
}

void chiefrim_caches_update(long chief, uint32_t generation, cr_vec3 origin)
{
	uint32_t now = (uint32_t)system_milliseconds();
	long index;

	for (index = 0; index < CHIEFRIM_CACHES; index++)
	{
		struct object_placement_data data;
		struct collision_result collision;
		real_vector3d down = { 0.f, 0.f, -CHIEFRIM_CACHE_DROP };
		real_point3d from;
		cr_vec3 at;
		boolean hit;

		if (!chiefrim_caches.caches[index].used)
			continue;
		if (chiefrim_caches.caches[index].generation != generation)
		{
			/* another world's: Skyrim sends this one's */
			chiefrim_caches.caches[index].used = FALSE;
			continue;
		}
		if (chiefrim_caches.caches[index].taken)
			continue;
		if (chiefrim_caches.caches[index].placed)
		{
			long weapon = chiefrim_caches.caches[index].object_index;

			if (!object_try_and_get(weapon))
			{
				/* gone some other way: Skyrim lays it down again in the next world */
				chiefrim_caches.caches[index].used = FALSE;
			}
			else if (chief != NONE && object_get_ultimate_parent(weapon) == chief)
			{
				error(_error_silent, "chiefrim: cache %08X taken (%s)", chiefrim_caches.caches[index].id,
					chiefrim_caches.caches[index].weapon);
				chiefrim_cache_taken(chiefrim_caches.caches[index].id);
				chiefrim_caches.caches[index].taken = TRUE; /* kept for this world */
			}
			continue;
		}
		/* down onto what lies below, once the collision has it */
		at = cr_sky_to_halo(chiefrim_caches.caches[index].position, origin);
		from.x = at.x;
		from.y = at.y;
		from.z = at.z;
		hit = collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
			&from, &down, NONE, &collision);
		if (!hit)
		{
			if (now - chiefrim_caches.caches[index].since_ms > CHIEFRIM_CACHE_PATIENCE_MS)
			{
				error(_error_silent, "chiefrim: cache %08X found nothing to lie on; given up", chiefrim_caches.caches[index].id);
				chiefrim_caches.caches[index].used = FALSE;
			}
			continue;
		}
		object_placement_data_new(&data, chiefrim_caches.caches[index].tag_index, NONE);
		data.position = collision.point;
		data.position.z += 0.05f;
		data.forward.i = cosf(chiefrim_caches.caches[index].yaw);
		data.forward.j = sinf(chiefrim_caches.caches[index].yaw);
		data.forward.k = 0.f;
		data.up.i = 0.f;
		data.up.j = 0.f;
		data.up.k = 1.f;
		chiefrim_caches.caches[index].object_index = object_new(&data);
		if (chiefrim_caches.caches[index].object_index == NONE)
		{
			chiefrim_caches.caches[index].used = FALSE;
			continue;
		}
		chiefrim_cache_ammunition(chiefrim_caches.caches[index].object_index, chiefrim_caches.caches[index].spare);
		chiefrim_caches.caches[index].placed = TRUE;
		if (chiefrim_caches.logged++ < 20)
		{
			error(_error_silent, "chiefrim: cache %08X: %s laid down at (%.1f, %.1f, %.1f)", chiefrim_caches.caches[index].id,
				chiefrim_caches.caches[index].weapon, data.position.x, data.position.y, data.position.z);
		}
	}
}
