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
- HEALING: Skyrim's restore-health potions and food (CR_MSG_CHIEF_HEAL) heal
  his body, on the scale of Skyrim's damage to him.
- WEAPONS: CR_MSG_GIVE_WEAPON gives Chief one of the map's weapons, listed
  in the log at start (debug, docs §8.4). Each proxy carries one of a few
  sidearms and a type of grenade, at random, which it drops when its actor
  dies (Skyrim lists the newly dead a moment, CR_ACTOR_DEAD): Chief's loot.
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
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

#define CHIEFRIM_PROXIES          CR_ACTORS_MAX
#define CHIEFRIM_PROXY_VITALITY   100000.f /* nothing takes it all in a frame */
#define CHIEFRIM_MAXIMUM_WEAPONS  32
#define CHIEFRIM_MARINE_VITALITY  100.f /* b30's marine's (its proxies' before d20): what a hit's fraction is of */
#define CHIEFRIM_HEAD_FRACTION    0.92f /* a Skyrim person's head centre, of its height, when Skyrim doesn't say */
#define CHIEFRIM_HEAD_RADIUS      0.07f /* world units at scale 1: a head, around its marker (Chief's helmet is ~0.13 tall) */
#define CHIEFRIM_HEADSHOT_FLAG    0x0002u /* damage.c's _damage_can_cause_headshots_bit (a pistol's, a sniper's bullet) */
#define CHIEFRIM_PROXY_GRENADES   2       /* when its biped carries none of its own */

/* what a proxy may carry, each as likely: the weapons a proxy drops */
static char const *const proxy_weapon_names[] = { "pistol", "assault rifle", "plasma pistol", "needler" };

/* ---------- globals */

struct chiefrim_proxy
{
	uint32_t form_id;   /* 0: free */
	long object_index;
	boolean seen;
	boolean blasted;       /* an explosion hurt it this frame */
	real_point3d blast;    /* its centre */
	real head;             /* its head marker above its feet at scale 1, in its pose (0: not yet) */
	boolean headshot;      /* a headshot this frame */
};

static struct
{
	boolean resolved;
	long proxy_biped;                    /* definition index */
	real proxy_vitality;                 /* its biped's own (shields and body); a marine's for Chief's own */
	boolean proxy_is_chief;              /* the map has no marines: proxies are Chief's own biped */
	real proxy_height;                   /* its standing height, world units */
	real proxy_head;                     /* its head marker's height, world units (0: not yet measured) */
	long hurt_effects[4];                /* CR_HURT_*: damage effects */
	long weapons[CHIEFRIM_MAXIMUM_WEAPONS];
	long weapon_count;
	long next_weapon;
	long proxy_weapons[NUMBEROF(proxy_weapon_names)]; /* those of them the map has */
	long proxy_weapon_count;
	long proxies_armed, drops;           /* for the log */
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
	boolean own = FALSE;

	if (combat.resolved)
		return;
	combat.resolved = TRUE;
	combat.proxy_is_chief = FALSE;

	/* the proxy: a marine, else Chief's own biped (d20, the host map, has no marines) */
	combat.proxy_biped = chiefrim_find_tag(BIPED_DEFINITION_TAG, "marine", NULL);
	if (combat.proxy_biped == NONE && chief != NONE)
	{
		combat.proxy_biped = object_get(chief)->definition_index;
		own = TRUE;
		combat.proxy_is_chief = TRUE;
		/* his 150 (shields and body) took half again the hits a marine did */
		combat.proxy_vitality = CHIEFRIM_MARINE_VITALITY;
	}
	if (combat.proxy_biped != NONE)
	{
		struct biped_definition *biped = biped_definition_get(combat.proxy_biped);
		real height = biped->biped.collision_height_standing, radius = biped->biped.collision_radius;

		/* Chief's own (no marines in the map): his height is his standing
		collision's, before Chief's scaling (149 Skyrim units, his eyes at 88% of
		it); with the radius added his proxies came out a third too short */
		chiefrim_biped_unscaled(biped, &height, &radius); /* if the scaling came first */
		combat.proxy_height = own ? height : height + radius;
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
		if (!chiefrim_weapon_carried(tag_get_name(tag_index)))
			continue;
		error(_error_silent, "chiefrim: weapon %ld: %s", combat.weapon_count, tag_get_name(tag_index));
		combat.weapons[combat.weapon_count++] = tag_index;
	}
	error(_error_silent, "chiefrim: proxies are %s, %.2f world units tall",
		combat.proxy_biped != NONE ? tag_get_name(combat.proxy_biped) : "(none: no bipeds)", combat.proxy_height);

	srand((unsigned)system_milliseconds()); /* proxies' kits differ from one run to the next */
	combat.proxy_weapon_count = 0;
	for (index = 0; index < (long)NUMBEROF(proxy_weapon_names); index++)
	{
		long weapon = chiefrim_weapon_tag(proxy_weapon_names[index]);

		if (weapon != NONE)
			combat.proxy_weapons[combat.proxy_weapon_count++] = weapon;
		error(_error_silent, "chiefrim: proxies may carry %s: %s", proxy_weapon_names[index],
			weapon != NONE ? tag_get_name(weapon) : "(not in this map)");
	}
}

/* Chief carries it: not a vehicle's gun, not one only the AI can use (Halo
refuses them to him), and not the flamethrower (an Xbox leftover: buggy in
his hands, and no HUD) */
boolean chiefrim_weapon_carried(char const *name)
{
	static char const *const refused[] = { "\\fuel rod", "\\hunter fuel rod", "\\energy sword", "\\flamethrower" };
	size_t length = strlen(name), index;

	if (!strncmp(name, "vehicles\\", 9) || !strncmp(name, "characters\\", 11))
		return FALSE;
	for (index = 0; index < NUMBEROF(refused); index++)
	{
		size_t part = strlen(refused[index]);

		if (length >= part && !strcmp(name + length - part, refused[index]))
			return FALSE;
	}
	return TRUE;
}

/* its actor died: its weapon and grenades fall where it stands, at once. A
dying unit drops its grenades as it dies but its weapon partway through its
death animation, and a proxy is gone the next frame (no gun ever fell) */
static void chiefrim_proxy_drop_kit(struct chiefrim_proxy *proxy)
{
	struct unit_datum *unit;

	if (proxy->object_index == NONE || !object_try_and_get(proxy->object_index))
		return;
	unit = unit_get(proxy->object_index);
	unit->unit.weapon_drop_delay_ticks = 0;
	if (!TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		unit_died(proxy->object_index, FALSE); /* Skyrim's kill: its grenades and weapons */
	unit_drop_current_weapon(proxy->object_index, TRUE);
	if (combat.drops++ < 6)
		error(_error_silent, "chiefrim: %08X died: its proxy dropped its kit", proxy->form_id);
}

static void chiefrim_proxy_delete(struct chiefrim_proxy *proxy)
{
	if (proxy->object_index != NONE && object_try_and_get(proxy->object_index))
		object_delete(proxy->object_index);
	proxy->form_id = 0;
	proxy->object_index = NONE;
}

/* one of the proxy weapons in place of its biped's own, and its grenades
all of one type, both at random (what it drops when it dies) */
static void chiefrim_proxy_arm(long object_index)
{
	struct unit_datum *unit = unit_get(object_index);
	short grenades = 0, type;

	for (type = 0; type < NUMBER_OF_UNIT_GRENADE_TYPES; type++)
	{
		grenades += MAX(unit->unit.grenade_counts[type], 0);
		unit->unit.grenade_counts[type] = 0;
	}
	unit->unit.grenade_counts[rand() % NUMBER_OF_UNIT_GRENADE_TYPES] = (char)(grenades > 0 ? grenades : CHIEFRIM_PROXY_GRENADES);

	if (combat.proxy_weapon_count > 0)
	{
		struct object_placement_data data;
		long weapon;

		object_placement_data_new(&data, combat.proxy_weapons[rand() % combat.proxy_weapon_count], object_index);
		object_get_origin(object_index, &data.position);
		weapon = object_new(&data);
		/* replacing: its biped's own weapon goes */
		if (weapon != NONE && !unit_add_weapon_to_inventory(object_index, weapon, _unit_add_weapon_replace))
		{
			object_delete(weapon);
			weapon = NONE;
		}
		if (combat.proxies_armed++ < 6)
			error(_error_silent, "chiefrim: a proxy carries %s and %d %s grenades",
				weapon != NONE ? tag_get_name(object_get(weapon)->definition_index) : "nothing (its weapon wasn't taken)",
				unit->unit.grenade_counts[0] + unit->unit.grenade_counts[1], unit->unit.grenade_counts[0] ? "frag" : "plasma");
	}
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
	chiefrim_proxy_arm(object_index);
	(void)actor;
	return object_index;
}

/* what the proxy lost since last frame, as a fraction of its biped's own
vitality; refilled */
static real chiefrim_proxy_take_damage(long object_index, boolean *killed)
{
	struct object_datum *object = object_get(object_index);
	real lost = (1.f - object->object.body_vitality) * object->object.maximum_body_vitality;

	/* nothing takes half its vitality but a headshot, which takes it all */
	*killed = lost >= CHIEFRIM_PROXY_VITALITY * 0.5f;
	object->object.body_vitality = 1.f;
	object->object.current_body_damage = 0.f;
	object->object.recent_body_damage = 0.f;
	/* at most a whole proxy: a headshot kills a marine outright (Halo sets its
	body to nothing, which here is the 100,000) */
	return combat.proxy_vitality > 0.f && lost > 0.f ? MIN(lost / combat.proxy_vitality, 1.f) : 0.f;
}

/* a proxy's loss this frame to Skyrim, with the explosion's centre if one
did some of it */
static void chiefrim_proxy_send_hit(struct chiefrim_proxy *proxy, real fraction, boolean killed, cr_vec3 origin)
{
	if (fraction > 0.0001f)
	{
		cr_msg_hit_actor hit;

		memset(&hit, 0, sizeof(hit));
		hit.form_id = proxy->form_id;
		hit.fraction = fraction;
		if (killed)
			hit.flags |= CR_HIT_HEADSHOT;
		if (proxy->blasted)
		{
			cr_vec3 blast;

			blast.x = proxy->blast.x;
			blast.y = proxy->blast.y;
			blast.z = proxy->blast.z;
			hit.flags |= CR_HIT_EXPLOSION;
			hit.blast = cr_halo_to_sky(blast, origin);
		}
		chiefrim_push(CR_MSG_HIT_ACTOR, &hit, sizeof(hit));
		combat.hits++;
	}
	proxy->blasted = FALSE;
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

		for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		{
			if (combat.proxies[slot].form_id == actor->form_id)
				proxy = &combat.proxies[slot];
			else if (!combat.proxies[slot].form_id && !free_proxy)
				free_proxy = &combat.proxies[slot];
		}
		if (actor->flags & CR_ACTOR_DEAD)
		{
			/* Skyrim says it just died (whatever killed it): its loot falls */
			if (proxy)
			{
				chiefrim_proxy_drop_kit(proxy);
				chiefrim_proxy_delete(proxy);
			}
			continue;
		}
		position.x = halo.x;
		position.y = halo.y;
		position.z = halo.z;
		forward.i = cosine(yaw);
		forward.j = sine(yaw);
		forward.k = 0.f;

		if (proxy && proxy->object_index != NONE && object_try_and_get(proxy->object_index) &&
			TEST_FLAG(object_get(proxy->object_index)->object.damage_flags, _object_dead_bit))
		{
			/* the proxy died (a headshot kills outright, whatever its vitality):
			its hit is sent below; a fresh one stands in from next frame */
			boolean killed;
			real fraction = chiefrim_proxy_take_damage(proxy->object_index, &killed);

			chiefrim_proxy_send_hit(proxy, fraction, TRUE, origin);
			chiefrim_proxy_drop_kit(proxy);
			chiefrim_proxy_delete(proxy);
			continue;
		}
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
			free_proxy->head = 0.f;
			free_proxy->headshot = FALSE;
			proxy = free_proxy;
		}
		proxy->seen = TRUE;

		/* what Chief did to it since last frame goes to Skyrim */
		{
			boolean killed;
			real fraction = chiefrim_proxy_take_damage(proxy->object_index, &killed);

			if (proxy->headshot)
			{
				fraction = 1.f;
				killed = TRUE;
				proxy->headshot = FALSE;
			}
			chiefrim_proxy_send_hit(proxy, fraction, killed, origin);
		}

		/* where the actor stands now, its size: its head where the actor's is.
		The biped stands as it does (Chief's own, unarmed: knees bent, head low
		and forward), so its head marker is measured in that pose, from where it
		was drawn last, and followed */
		{
			struct object_datum *object = object_get(proxy->object_index);
			real height = actor->height / CR_SKY_UNITS_PER_WU;
			real head = actor->head > 1.f ? actor->head / CR_SKY_UNITS_PER_WU : height * CHIEFRIM_HEAD_FRACTION;
			real reference;
			struct object_marker marker;
			real_point3d feet;
			real_vector3d up;

			if (object->object.scale > 0.f && object_get_marker_by_name(proxy->object_index, "head", &marker, 1))
			{
				real measured;

				object_get_origin(proxy->object_index, &feet);
				measured = (marker.matrix.position.z - feet.z) / object->object.scale;
				if (measured > 0.05f)
				{
					proxy->head = proxy->head > 0.f ? proxy->head * 0.8f + measured * 0.2f : measured;
					if (combat.proxy_head <= 0.f)
					{
						combat.proxy_head = measured;
						error(_error_silent, "chiefrim: a proxy's head is %.3f world units up as it stands (%.3f tall), a hit is of %.0f vitality",
							combat.proxy_head, combat.proxy_height, combat.proxy_vitality);
					}
				}
			}
			reference = proxy->head > 0.f ? proxy->head :
				combat.proxy_head > 0.f ? combat.proxy_head : combat.proxy_height * CHIEFRIM_HEAD_FRACTION;
			up.i = 0.f;
			up.j = 0.f;
			up.k = 1.f;
			object->object.scale = reference > 0.f && head > 0.05f ? PIN(head / reference, 0.25f, 6.f) : 1.f;
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

/* Skyrim's healing goes to his body; his shields recharge as ever */
static void chiefrim_chief_heal(long chief, cr_msg_chief_heal const *heal)
{
	struct object_datum *object;
	real body, vitality, before;

	if (chief == NONE || !object_try_and_get(chief) || heal->amount <= 0.f)
		return;
	object = object_get(chief);
	body = object_get_maximum_body_vitality(chief, FALSE);
	vitality = body + object_get_maximum_shield_vitality(chief, FALSE);
	if (body <= 0.f)
		return;
	before = object->object.body_vitality;
	object->object.body_vitality = MIN(1.f, before + heal->amount * vitality / body);
	error(_error_silent, "chiefrim: Skyrim healed Chief (%08X): body %.2f to %.2f",
		heal->item, before, object->object.body_vitality);
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

/* damage.c's hook: an explosion reaches an object; if it's a proxy, its hit
this frame says so */
void chiefrim_note_area_damage(long object_index, real_point3d const *epicenter)
{
	long slot;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		struct chiefrim_proxy *proxy = &combat.proxies[slot];

		if (proxy->form_id && proxy->object_index == object_index)
		{
			proxy->blasted = TRUE;
			proxy->blast = *epicenter;
			return;
		}
	}
}

/* projectiles.c's hook: a projectile hit a proxy. Chief's own biped (d20's
proxies) has no head that headshots kill (he's spared them in the campaign),
so a headshot is Chiefrim's: a bullet that can cause one, within a head's
reach of the head marker, kills it as one kills a marine */
void chiefrim_proxy_struck(long object_index, real_point3d const *point, long damage_definition_index)
{
	struct object_marker marker;
	struct object_datum *object;
	long slot;
	real reach, dx, dy, dz;

	if (!combat.proxy_is_chief || damage_definition_index == NONE ||
		!(damage_effect_definition_get(damage_definition_index)->damage.flags & CHIEFRIM_HEADSHOT_FLAG) ||
		!object_try_and_get(object_index) || !object_get_marker_by_name(object_index, "head", &marker, 1))
	{
		return;
	}
	object = object_get(object_index);
	reach = CHIEFRIM_HEAD_RADIUS * (object->object.scale > 0.f ? object->object.scale : 1.f);
	dx = point->x - marker.matrix.position.x;
	dy = point->y - marker.matrix.position.y;
	dz = point->z - marker.matrix.position.z;
	if (dx * dx + dy * dy + dz * dz > reach * reach)
		return;
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		if (combat.proxies[slot].form_id && combat.proxies[slot].object_index == object_index)
			combat.proxies[slot].headshot = TRUE;
	}
}

/* damage.c's hook: an explosion's area damage starts (a grenade, a rocket,
a plasma bolt's splash): Skyrim's loose objects in reach fly */
void chiefrim_note_explosion(real_point3d const *epicenter, real radius, real acceleration)
{
	cr_vec3 origin, center;
	cr_msg_explosion explosion;

	if (!chiefrim_world_origin(&origin) || radius <= 0.f)
		return;
	center.x = epicenter->x;
	center.y = epicenter->y;
	center.z = epicenter->z;
	memset(&explosion, 0, sizeof(explosion));
	explosion.center = cr_halo_to_sky(center, origin);
	explosion.radius = radius * CR_SKY_UNITS_PER_WU;
	explosion.acceleration = acceleration;
	chiefrim_push(CR_MSG_EXPLOSION, &explosion, sizeof(explosion));
}

boolean chiefrim_object_is_proxy(long object_index)
{
	long slot;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		if (combat.proxies[slot].form_id && combat.proxies[slot].object_index == object_index)
			return TRUE;
	}
	return FALSE;
}

/* proxies aren't drawn, but with CHIEFRIM_SHOW_PROXIES=1 (to see where their
hitboxes are) */
boolean chiefrim_object_unseen(long object_index)
{
	static int show = -1;

	if (show < 0)
		show = getenv("CHIEFRIM_SHOW_PROXIES") && atoi(getenv("CHIEFRIM_SHOW_PROXIES")) != 0;
	return !show && chiefrim_object_is_proxy(object_index);
}

void chiefrim_combat_map_loaded(void)
{
	long slot;

	memset(&combat, 0, sizeof(combat));
	combat.proxy_biped = NONE;
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
		combat.proxies[slot].object_index = NONE;
}

/* Halo's game state went back (a checkpoint revert): the proxies' object
indices mean nothing now; they're forgotten, not deleted */
void chiefrim_combat_forget(void)
{
	long slot;

	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		combat.proxies[slot].form_id = 0;
		combat.proxies[slot].object_index = NONE;
	}
}

/* Chief died in a way that isn't his body running out (deathless covers that
one): his unit dead or gone */
void chiefrim_combat_chief_lost(void)
{
	if (!combat.chief_dead)
	{
		cr_msg_player_died died;

		memset(&died, 0, sizeof(died));
		chiefrim_push(CR_MSG_PLAYER_DIED, &died, sizeof(died));
		combat.chief_dead = TRUE;
		error(_error_silent, "chiefrim: Chief is gone (his unit died or went); Skyrim's player dies");
	}
}

boolean chiefrim_combat_chief_dead(void)
{
	return combat.chief_dead;
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
	case CR_MSG_CHIEF_HEAL:
		if (!combat.chief_dead)
			chiefrim_chief_heal(chief, (cr_msg_chief_heal const *)message);
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

	/* Chief's body gone (he's deathless while linked) or his unit dead:
	Skyrim's player dies */
	if (!combat.chief_dead && TEST_FLAG(object_get(chief)->object.damage_flags, _object_dead_bit))
		chiefrim_combat_chief_lost();
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
