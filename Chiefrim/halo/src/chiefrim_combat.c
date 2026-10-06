/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_COMBAT.C

Chief against Skyrim's people (Chiefrim/docs/DESIGN.md §8):

- PROXIES: for each actor Skyrim lists near the player, an unseen biped of
  the host map's (a marine's) where the actor stands, its size, on the
  Covenant's team. Halo's own code hits it: bullets, plasma, splash,
  melee, headshots. It has a vitality nothing reaches; what it loses in a
  frame, over its biped's own vitality, goes to Skyrim (CR_MSG_HIT_ACTOR)
  and is refilled. Render.c's hook keeps it out of the picture.
- CHIEF HURT: Skyrim's damage to the player (CR_MSG_PLAYER_HURT) goes
  through Halo's own damage, with a damage effect of the host map's (a
  melee's for melee, a bullet's for arrows, plasma for magic), so shields
  take it first, recharge as ever, and the HUD shows where it came from.
- DEATH: Chief is deathless while linked (chiefrim.c); with his body gone
  Skyrim is told once (CR_MSG_PLAYER_DIED) and its player dies. A new world
  (Skyrim's reload) brings him back whole.
- WEAPONS: CR_MSG_GIVE_WEAPON gives Chief one of the map's weapons, listed
  in the log at start (debug, docs §8.4).
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cache/cache_files.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_globals.h" /* (damage_effect_definitions.h needs its material count) */
#include "items/weapon_definitions.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/biped_definitions.h"
#include "units/units.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

#define CHIEFRIM_PROXIES          CR_ACTORS_MAX
#define CHIEFRIM_PROXY_VITALITY   100000.f /* nothing takes it all in a frame */
#define CHIEFRIM_MAXIMUM_WEAPONS  32

/* ---------- globals */

struct chiefrim_proxy
{
	uint32_t form_id;   /* 0: free */
	long object_index;
	boolean seen;
};

static struct
{
	boolean resolved;
	long proxy_biped;                    /* definition index */
	real proxy_vitality;                 /* its biped's own (shields and body) */
	real proxy_height;                   /* its standing height, world units */
	long hurt_effects[4];                /* CR_HURT_*: damage effects */
	long weapons[CHIEFRIM_MAXIMUM_WEAPONS];
	long weapon_count;
	long next_weapon;
	struct chiefrim_proxy proxies[CHIEFRIM_PROXIES];
	boolean chief_dead;                  /* told Skyrim */
	long hits, hurts;                    /* since the last summary */
} combat;

/* ---------- private code */

static boolean chiefrim_name_has(char const *name, char const *part)
{
	return name && strstr(name, part) != NULL;
}

/* the first tag of a group whose name has all the parts (NULL: any) */
static long chiefrim_find_tag(unsigned long group, char const *part, char const *other)
{
	struct tag_iterator iterator;
	long tag_index;

	tag_iterator_new(&iterator, (long)group);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *name = tag_get_name(tag_index);

		if ((!part || chiefrim_name_has(name, part)) && (!other || chiefrim_name_has(name, other)))
			return tag_index;
	}
	return NONE;
}

static void chiefrim_push(uint16_t type, void const *message, uint32_t size)
{
	struct cr_shared *shm = chiefrim_shared();

	if (shm && chiefrim_linked())
		cr_ring_push(&shm->to_skyrim, type, message, size);
}

static void chiefrim_combat_resolve(long chief)
{
	static char const *const kinds[4] = { "other", "melee", "projectile", "magic" };
	struct tag_iterator iterator;
	long tag_index, index;

	if (combat.resolved)
		return;
	combat.resolved = TRUE;

	/* the proxy: a marine, else Chief's own biped */
	combat.proxy_biped = chiefrim_find_tag(BIPED_DEFINITION_TAG, "marine", NULL);
	if (combat.proxy_biped == NONE && chief != NONE)
		combat.proxy_biped = object_get(chief)->definition_index;
	if (combat.proxy_biped != NONE)
	{
		struct biped_definition *biped = biped_definition_get(combat.proxy_biped);

		combat.proxy_height = biped->biped.collision_height_standing + biped->biped.collision_radius;
		if (combat.proxy_height < 0.1f)
			combat.proxy_height = 0.6f;
	}

	combat.hurt_effects[CR_HURT_MELEE] = chiefrim_find_tag(DAMAGE_EFFECT_DEFINITION_TAG, "melee", NULL);
	combat.hurt_effects[CR_HURT_PROJECTILE] = chiefrim_find_tag(DAMAGE_EFFECT_DEFINITION_TAG, "bullet", NULL);
	combat.hurt_effects[CR_HURT_MAGIC] = chiefrim_find_tag(DAMAGE_EFFECT_DEFINITION_TAG, "plasma", "bolt");
	combat.hurt_effects[CR_HURT_OTHER] = combat.hurt_effects[CR_HURT_PROJECTILE];
	for (index = 0; index < 4; index++)
	{
		if (combat.hurt_effects[index] == NONE)
			combat.hurt_effects[index] = combat.hurt_effects[CR_HURT_PROJECTILE] != NONE ?
				combat.hurt_effects[CR_HURT_PROJECTILE] : chiefrim_find_tag(DAMAGE_EFFECT_DEFINITION_TAG, NULL, NULL);
		error(_error_silent, "chiefrim: Skyrim's %s damage hurts Chief as %s", kinds[index],
			combat.hurt_effects[index] != NONE ? tag_get_name(combat.hurt_effects[index]) : "(nothing: no damage effects)");
	}

	combat.weapon_count = 0;
	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE && combat.weapon_count < CHIEFRIM_MAXIMUM_WEAPONS)
	{
		if (!strncmp(tag_get_name(tag_index), "vehicles\\", 9))
			continue; /* a vehicle's gun: nothing to carry */
		error(_error_silent, "chiefrim: weapon %ld: %s", combat.weapon_count, tag_get_name(tag_index));
		combat.weapons[combat.weapon_count++] = tag_index;
	}
	error(_error_silent, "chiefrim: proxies are %s, %.2f world units tall",
		combat.proxy_biped != NONE ? tag_get_name(combat.proxy_biped) : "(none: no bipeds)", combat.proxy_height);
}

static void chiefrim_proxy_delete(struct chiefrim_proxy *proxy)
{
	if (proxy->object_index != NONE && object_try_and_get(proxy->object_index))
		object_delete(proxy->object_index);
	proxy->form_id = 0;
	proxy->object_index = NONE;
}

static long chiefrim_proxy_new(cr_actor const *actor, real_point3d const *position, real_vector3d const *forward)
{
	struct object_placement_data data;
	long object_index;
	struct object_datum *object;

	object_placement_data_new(&data, combat.proxy_biped, NONE);
	data.position = *position;
	data.forward = *forward;
	data.up.i = 0.f;
	data.up.j = 0.f;
	data.up.k = 1.f;
	data.owner_team_index = _game_team_covenant; /* an enemy's: Chief's hits count in full */
	object_index = object_new(&data);
	if (object_index == NONE)
		return NONE;
	object = object_get(object_index);
	if (combat.proxy_vitality <= 0.f)
		combat.proxy_vitality = object->object.maximum_body_vitality + object->object.maximum_shield_vitality;
	/* a vitality nothing reaches, all body (shields would hold some back) */
	object->object.maximum_body_vitality = CHIEFRIM_PROXY_VITALITY;
	object->object.maximum_shield_vitality = 0.f;
	object->object.body_vitality = 1.f;
	object->object.shield_vitality = 0.f;
	(void)actor;
	return object_index;
}

/* what the proxy lost since last frame, as a fraction of its biped's own
vitality; refilled */
static real chiefrim_proxy_take_damage(long object_index)
{
	struct object_datum *object = object_get(object_index);
	real lost = (1.f - object->object.body_vitality) * object->object.maximum_body_vitality;

	object->object.body_vitality = 1.f;
	object->object.current_body_damage = 0.f;
	object->object.recent_body_damage = 0.f;
	return combat.proxy_vitality > 0.f && lost > 0.f ? lost / combat.proxy_vitality : 0.f;
}

static void chiefrim_proxies_update(cr_vec3 origin)
{
	struct cr_shared *shm = chiefrim_shared();
	static cr_actors actors;
	uint32_t index, slot;

	if (!shm || combat.proxy_biped == NONE || !CR_SLOT_READ(&shm->actors, &actors))
		return;
	if (actors.count > CR_ACTORS_MAX)
		actors.count = CR_ACTORS_MAX;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		combat.proxies[slot].seen = FALSE;

	for (index = 0; index < actors.count; index++)
	{
		cr_actor const *actor = &actors.actors[index];
		struct chiefrim_proxy *proxy = NULL, *free_proxy = NULL;
		cr_vec3 halo = cr_sky_to_halo(actor->position, origin);
		real_point3d position;
		real_vector3d forward;
		real yaw = cr_sky_heading_to_halo_yaw(actor->heading);

		if (actor->flags & CR_ACTOR_DEAD)
			continue;
		for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		{
			if (combat.proxies[slot].form_id == actor->form_id)
				proxy = &combat.proxies[slot];
			else if (!combat.proxies[slot].form_id && !free_proxy)
				free_proxy = &combat.proxies[slot];
		}
		position.x = halo.x;
		position.y = halo.y;
		position.z = halo.z;
		forward.i = cosine(yaw);
		forward.j = sine(yaw);
		forward.k = 0.f;

		if (proxy && (proxy->object_index == NONE || !object_try_and_get(proxy->object_index)))
		{
			proxy->form_id = 0; /* gone (Halo cleaned it up): a new one */
			free_proxy = free_proxy ? free_proxy : proxy;
			proxy = NULL;
		}
		if (!proxy)
		{
			if (!free_proxy)
				continue;
			free_proxy->object_index = chiefrim_proxy_new(actor, &position, &forward);
			if (free_proxy->object_index == NONE)
				continue;
			free_proxy->form_id = actor->form_id;
			proxy = free_proxy;
		}
		proxy->seen = TRUE;

		/* what Chief did to it since last frame goes to Skyrim */
		{
			real fraction = chiefrim_proxy_take_damage(proxy->object_index);

			if (fraction > 0.0001f)
			{
				cr_msg_hit_actor hit;

				memset(&hit, 0, sizeof(hit));
				hit.form_id = actor->form_id;
				hit.fraction = fraction;
				chiefrim_push(CR_MSG_HIT_ACTOR, &hit, sizeof(hit));
				combat.hits++;
			}
		}

		/* where the actor stands now, its size */
		{
			struct object_datum *object = object_get(proxy->object_index);
			real height = actor->height / CR_SKY_UNITS_PER_WU;
			real_vector3d up;

			up.i = 0.f;
			up.j = 0.f;
			up.k = 1.f;
			object->object.scale = combat.proxy_height > 0.f && height > 0.05f ?
				PIN(height / combat.proxy_height, 0.25f, 6.f) : 1.f;
			object_set_position(proxy->object_index, &position, &forward, &up);
			object->object.translational_velocity.i = 0.f;
			object->object.translational_velocity.j = 0.f;
			object->object.translational_velocity.k = 0.f;
		}
	}

	/* actors no longer listed (out of range, dead, unloaded) */
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		if (combat.proxies[slot].form_id && !combat.proxies[slot].seen)
			chiefrim_proxy_delete(&combat.proxies[slot]);
	}
}

static void chiefrim_chief_hurt(long chief, cr_msg_player_hurt const *hurt, cr_vec3 origin)
{
	long effect = combat.hurt_effects[hurt->kind < 4 ? hurt->kind : CR_HURT_OTHER];
	struct damage_effect_definition *definition;
	struct damage_data damage;
	real vitality, points, typical;
	real_point3d chief_position;

	if (effect == NONE || chief == NONE || !(hurt->amount > 0.f))
		return;
	definition = damage_effect_definition_get(effect);
	vitality = object_get_maximum_body_vitality(chief, FALSE) + object_get_maximum_shield_vitality(chief, FALSE);
	points = hurt->amount * vitality;
	object_get_origin(chief, &chief_position);

	damage_data_new(&damage, effect);
	/* scale 0: the effect's minimum damage, times the multiplier */
	typical = definition->damage.damage_minimum;
	damage.scale = 0.f;
	if (!(typical > 0.f))
	{
		typical = 0.5f * (definition->damage.damage_lower_bound + definition->damage.damage_upper_bound);
		damage.scale = 1.f;
	}
	if (!(typical > 0.f))
		return;
	damage.multiplier = points / typical;
	damage.owner_team_index = NONE;
	damage.owner_object_index = NONE;
	damage.owner_player_index = NONE;
	damage.epicenter = chief_position;
	damage.origin = chief_position;
	if (hurt->from.x != 0.f || hurt->from.y != 0.f || hurt->from.z != 0.f)
	{
		cr_vec3 from = cr_sky_to_halo(hurt->from, origin);

		damage.origin.x = from.x;
		damage.origin.y = from.y;
		damage.origin.z = from.z;
	}
	damage.direction.i = chief_position.x - damage.origin.x;
	damage.direction.j = chief_position.y - damage.origin.y;
	damage.direction.k = chief_position.z - damage.origin.z;
	if (normalize3d(&damage.direction) == 0.f)
	{
		damage.direction.i = 0.f;
		damage.direction.j = 0.f;
		damage.direction.k = -1.f;
	}
	scenario_location_from_point(&damage.location, &damage.origin);
	object_cause_damage(&damage, chief, NONE, NONE, NONE, NULL);
	combat.hurts++;
}

static void chiefrim_give_weapon(long chief, int32_t requested)
{
	struct object_placement_data data;
	long weapon, index;

	if (chief == NONE || combat.weapon_count == 0)
		return;
	index = requested >= 0 ? requested % combat.weapon_count : combat.next_weapon % combat.weapon_count;
	combat.next_weapon = index + 1;
	object_placement_data_new(&data, combat.weapons[index], chief);
	object_get_origin(chief, &data.position);
	weapon = object_new(&data);
	if (weapon == NONE)
		return;
	if (unit_get_weapon_count(chief) >= 2)
		unit_drop_selected_weapon(chief);
	if (!unit_add_weapon_to_inventory(chief, weapon, TRUE))
	{
		object_delete(weapon);
		error(_error_silent, "chiefrim: couldn't give Chief %s", tag_get_name(combat.weapons[index]));
		return;
	}
	error(_error_silent, "chiefrim: gave Chief weapon %ld, %s", index, tag_get_name(combat.weapons[index]));
}

/* ---------- public code */

boolean chiefrim_object_unseen(long object_index)
{
	long slot;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		if (combat.proxies[slot].form_id && combat.proxies[slot].object_index == object_index)
			return TRUE;
	}
	return FALSE;
}

void chiefrim_combat_map_loaded(void)
{
	long slot;

	memset(&combat, 0, sizeof(combat));
	combat.proxy_biped = NONE;
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		combat.proxies[slot].object_index = NONE;
}

void chiefrim_combat_reset(long chief)
{
	long slot;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		chiefrim_proxy_delete(&combat.proxies[slot]);
	if (chief != NONE && object_try_and_get(chief))
	{
		struct object_datum *object = object_get(chief);

		object->object.body_vitality = 1.f;
		object->object.shield_vitality = 1.f;
		object->object.current_body_damage = 0.f;
		object->object.current_shield_damage = 0.f;
		object->object.shield_stun_ticks = 0;
	}
	if (combat.chief_dead)
		error(_error_silent, "chiefrim: a new world: Chief is whole again");
	combat.chief_dead = FALSE;
}

void chiefrim_combat_message(long chief, int type, void const *message, cr_vec3 origin)
{
	switch (type)
	{
	case CR_MSG_PLAYER_HURT:
		if (!combat.chief_dead)
			chiefrim_chief_hurt(chief, (cr_msg_player_hurt const *)message, origin);
		break;
	case CR_MSG_GIVE_WEAPON:
		chiefrim_combat_resolve(chief);
		chiefrim_give_weapon(chief, ((cr_msg_give_weapon const *)message)->index);
		break;
	default:
		break;
	}
}

void chiefrim_combat_update(long chief, cr_vec3 origin)
{
	static uint32_t summary_ms;
	uint32_t now = (uint32_t)system_milliseconds();

	if (chief == NONE)
		return;
	chiefrim_combat_resolve(chief);
	chiefrim_proxies_update(origin);

	/* Chief's body gone (he's deathless while linked): Skyrim's player dies */
	if (!combat.chief_dead && object_get(chief)->object.body_vitality <= 0.001f)
	{
		cr_msg_player_died died;

		memset(&died, 0, sizeof(died));
		chiefrim_push(CR_MSG_PLAYER_DIED, &died, sizeof(died));
		combat.chief_dead = TRUE;
		error(_error_silent, "chiefrim: Chief is dead; Skyrim's player dies");
	}

	if (now - summary_ms >= 30000)
	{
		long slot, count = 0;

		for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
			count += combat.proxies[slot].form_id != 0;
		if (summary_ms && (combat.hits || combat.hurts || count))
			error(_error_silent, "chiefrim: last 30 s: %ld proxies now, %ld hits on them, %ld on Chief", count, combat.hits, combat.hurts);
		combat.hits = combat.hurts = 0;
		summary_ms = now;
	}
}
