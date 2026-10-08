/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_COMBAT.C

Chief against Skyrim's people (Chiefrim/docs/DESIGN.md §8):

- PROXIES: for each actor Skyrim lists near the player, an unseen biped of
  the host map's (a marine's) where the actor stands, its size, on the
  Covenant's team. Halo's own code hits it: bullets, plasma, splash,
  melee, headshots. Where Skyrim sends the actor's hit shapes (its
  skeleton's capsules, protocol 18), shots, melee and explosions find it
  on those, not on its biped: a wolf is hit where a wolf is. It has a vitality nothing reaches; what it loses in a
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
  in the log at start (debug, docs §8.4): the next (the debug key), or one
  by name (the console's "chiefrim give"), answered on Skyrim's console. Each proxy carries one of a few
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
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/biped_definitions.h"
#include "units/units.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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
#define CHIEFRIM_PROXY_HITBOXES   CR_HITBOXES_PER_ACTOR
#define CHIEFRIM_HEAD_SLACK       1.25f   /* a headshot's reach, of the head shape's radius */

/* what a proxy may carry, each as likely: the weapons a proxy drops */
static char const *const proxy_weapon_names[] = { "pistol", "assault rifle", "plasma pistol", "needler" };

/* ---------- globals */

/* one of an actor's hit shapes, in Halo's world: a capsule (a sphere's ends meet) */
struct chiefrim_hitbox
{
	real_point3d a, b;
	real radius;
	uint32_t flags; /* CR_HITBOX_* */
};

struct chiefrim_proxy
{
	uint32_t form_id;   /* 0: free */
	long object_index;
	boolean seen;
	boolean blasted;       /* an explosion hurt it this frame */
	real_point3d blast;    /* its centre */
	real head;             /* its head marker above its feet at scale 1, in its pose (0: not yet) */
	boolean headshot;      /* a headshot this frame */
	boolean shaped;        /* logged its shapes */
	long hitbox_count;     /* 0: Halo hits its biped */
	struct chiefrim_hitbox hitboxes[CHIEFRIM_PROXY_HITBOXES];
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
	long proxies_shaped, shape_hits;     /* for the log */
	struct chiefrim_proxy proxies[CHIEFRIM_PROXIES];
	boolean chief_dead;                  /* told Skyrim */
	long hits, hurts;                    /* since the last summary */
} combat;

static uint32_t combat_debug; /* CR_DEBUG_*: kept across maps and worlds */

/* ---------- private code */

/* a line for Skyrim's console (answering a console command) */
static void chiefrim_console(char const *format, ...)
{
	struct cr_shared *shm = chiefrim_shared();
	cr_msg_log message;
	va_list arguments;

	memset(&message, 0, sizeof(message));
	va_start(arguments, format);
	vsnprintf(message.text, sizeof(message.text), format, arguments);
	va_end(arguments);
	if (shm && chiefrim_linked())
		cr_ring_push(&shm->to_skyrim, CR_MSG_CONSOLE, &message, sizeof(message));
}

/* a weapon's tag path's last part: what the console takes ("sniper rifle") */
static char const *chiefrim_weapon_short_name(long tag_index)
{
	char const *path = tag_get_name(tag_index), *last = strrchr(path, '\\');

	return last ? last + 1 : path;
}

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

static struct chiefrim_proxy *chiefrim_proxy_of(long object_index)
{
	long slot;

	if (object_index == NONE)
		return NULL;
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		if (combat.proxies[slot].form_id && combat.proxies[slot].object_index == object_index)
			return &combat.proxies[slot];
	}
	return NULL;
}

/* the point of segment a-b nearest p, as a fraction of the way */
static real chiefrim_segment_nearest(real_point3d const *p, real_point3d const *a, real_point3d const *b, real_point3d *nearest)
{
	real_vector3d ab, ap;
	real length, fraction = 0.f;

	vector_from_points3d(a, b, &ab);
	vector_from_points3d(a, p, &ap);
	length = dot_product3d(&ab, &ab);
	if (length > 1.0e-8f)
		fraction = PIN(dot_product3d(&ap, &ab) / length, 0.f, 1.f);
	nearest->x = a->x + ab.i * fraction;
	nearest->y = a->y + ab.j * fraction;
	nearest->z = a->z + ab.k * fraction;
	return fraction;
}

/* the ray p + t v (t from 0 to 1) into a sphere: the first t it meets the
surface going in; FALSE if it doesn't (or starts inside) */
static boolean chiefrim_ray_sphere(real_point3d const *p, real_vector3d const *v, real_point3d const *centre, real radius, real *t)
{
	real_vector3d off;
	real a = dot_product3d(v, v), b, c, h;

	vector_from_points3d(centre, p, &off);
	b = dot_product3d(&off, v);
	c = dot_product3d(&off, &off) - radius * radius;
	if (a <= 0.f || c <= 0.f || b >= 0.f)
		return FALSE;
	h = b * b - a * c;
	if (h < 0.f)
		return FALSE;
	*t = (-b - square_root(h)) / a;
	return *t >= 0.f && *t <= 1.f;
}

/* the ray into a capsule: its round side, then its ends' spheres; the
nearest t, and the surface's outward normal there */
static boolean chiefrim_ray_capsule(real_point3d const *p, real_vector3d const *v, struct chiefrim_hitbox const *box, real *t, real_vector3d *normal)
{
	real_vector3d ba, oa;
	real baba, best = 2.f, end_t;
	real_point3d at, axis;

	vector_from_points3d(&box->a, &box->b, &ba);
	vector_from_points3d(&box->a, p, &oa);
	baba = dot_product3d(&ba, &ba);
	if (baba > 1.0e-8f)
	{
		real bard = dot_product3d(&ba, v), baoa = dot_product3d(&ba, &oa);
		real a = baba * dot_product3d(v, v) - bard * bard;
		real b = baba * dot_product3d(v, &oa) - baoa * bard;
		real c = baba * dot_product3d(&oa, &oa) - baoa * baoa - box->radius * box->radius * baba;
		real h = b * b - a * c;

		if (a > 1.0e-10f && c > 0.f && h >= 0.f)
		{
			real side = (-b - square_root(h)) / a, y = baoa + side * bard;

			if (side >= 0.f && side <= 1.f && y > 0.f && y < baba)
				best = side;
		}
	}
	if (chiefrim_ray_sphere(p, v, &box->a, box->radius, &end_t) && end_t < best)
		best = end_t;
	if (baba > 1.0e-8f && chiefrim_ray_sphere(p, v, &box->b, box->radius, &end_t) && end_t < best)
		best = end_t;
	if (best > 1.f)
		return FALSE;
	*t = best;
	at.x = p->x + v->i * best;
	at.y = p->y + v->j * best;
	at.z = p->z + v->k * best;
	chiefrim_segment_nearest(&at, &box->a, &box->b, &axis);
	vector_from_points3d(&axis, &at, normal);
	if (normalize3d(normal) == 0.f)
	{
		*normal = *v;
		normal->i = -normal->i;
		normal->j = -normal->j;
		normal->k = -normal->k;
		normalize3d(normal);
	}
	return TRUE;
}

/* the actor's hit shapes, in Halo's world, for its proxy */
static void chiefrim_proxy_take_hitboxes(struct chiefrim_proxy *proxy, cr_actors const *actors, cr_actor const *actor, cr_vec3 origin)
{
	uint32_t first = actor->hitbox_first, count = actor->hitbox_count, index;
	uint32_t total = MIN(actors->hitbox_count, CR_HITBOXES_MAX);
	boolean head = FALSE;

	proxy->hitbox_count = 0;
	if (first >= total || count == 0)
		return;
	count = MIN(MIN(count, total - first), CHIEFRIM_PROXY_HITBOXES);
	for (index = 0; index < count; index++)
	{
		cr_hitbox const *in = &actors->hitboxes[first + index];
		struct chiefrim_hitbox *out = &proxy->hitboxes[proxy->hitbox_count];
		cr_vec3 a = cr_sky_to_halo(in->a, origin), b = cr_sky_to_halo(in->b, origin);
		real radius = in->radius / CR_SKY_UNITS_PER_WU;
		real sum = a.x + a.y + a.z + b.x + b.y + b.z;

		/* (a NaN fails both) */
		if (!(radius > 0.f && radius < 20.f) || !(sum > -1.0e6f && sum < 1.0e6f))
			continue;
		out->a.x = a.x;
		out->a.y = a.y;
		out->a.z = a.z;
		out->b.x = b.x;
		out->b.y = b.y;
		out->b.z = b.z;
		out->radius = radius;
		out->flags = in->flags;
		head |= (in->flags & CR_HITBOX_HEAD) != 0;
		proxy->hitbox_count++;
	}
	if (proxy->hitbox_count && !proxy->shaped)
	{
		proxy->shaped = TRUE;
		if (combat.proxies_shaped++ < 8)
			error(_error_silent, "chiefrim: %08X is hit on its own shapes: %ld of them%s%s", proxy->form_id, proxy->hitbox_count,
				head ? ", a person's head among them" : "", (proxy->hitboxes[0].flags & CR_HITBOX_BOUNDS) ? " (from its bounds)" : "");
	}
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
	{
		/* what's stuck in it (a plasma grenade, needles) lets go and goes
		off as it would: deleting the proxy would delete it too */
		long child = object_get(proxy->object_index)->object.first_child_object_index;

		while (child != NONE)
		{
			long next = object_get(child)->object.next_object_index;

			if (object_get(child)->object.type == _object_type_projectile)
				object_detach(child);
			child = next;
		}
		object_delete(proxy->object_index);
	}
	proxy->form_id = 0;
	proxy->object_index = NONE;
	proxy->hitbox_count = 0;
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
			free_proxy->shaped = FALSE;
			free_proxy->hitbox_count = 0;
			proxy = free_proxy;
		}
		proxy->seen = TRUE;
		/* where it's hit now, before it's moved (its bounds hold them) */
		chiefrim_proxy_take_hitboxes(proxy, &actors, actor, origin);

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

/* the console's "chiefrim weapons": a few to a line */
static void chiefrim_list_weapons(void)
{
	char line[120];
	long index;

	line[0] = 0;
	chiefrim_console("Chiefrim: Chief may have %ld weapons here (chiefrim give <name>):", combat.weapon_count);
	for (index = 0; index < combat.weapon_count; index++)
	{
		char const *name = chiefrim_weapon_short_name(combat.weapons[index]);

		if (line[0] && strlen(line) + strlen(name) + 3 >= sizeof(line))
		{
			chiefrim_console("  %s", line);
			line[0] = 0;
		}
		if (line[0])
			strcat(line, ", ");
		strcat(line, name);
	}
	if (line[0])
		chiefrim_console("  %s", line);
}

static void chiefrim_give_weapon(long chief, cr_msg_give_weapon const *give)
{
	struct object_placement_data data;
	long weapon, index;
	char name[CR_WEAPON_NAME_LENGTH + 1];

	memcpy(name, give->name, CR_WEAPON_NAME_LENGTH);
	name[CR_WEAPON_NAME_LENGTH] = 0;
	if (give->flags & CR_GIVE_LIST)
	{
		chiefrim_list_weapons();
		return;
	}
	if (chief == NONE || combat.weapon_count == 0)
	{
		chiefrim_console("Chiefrim: no weapons to give (%s)", chief == NONE ? "no Chief yet" : "none in this map");
		return;
	}
	if (name[0])
	{
		long tag = chiefrim_weapon_tag(name);

		for (index = 0; index < combat.weapon_count && combat.weapons[index] != tag; index++)
			;
		if (tag == NONE || index == combat.weapon_count)
		{
			chiefrim_console("Chiefrim: no \"%s\" that Chief can carry here; chiefrim weapons lists them", name);
			return;
		}
	}
	else
	{
		index = give->index >= 0 ? give->index % combat.weapon_count : combat.next_weapon % combat.weapon_count;
	}
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
		chiefrim_console("Chiefrim: couldn't give Chief the %s", chiefrim_weapon_short_name(combat.weapons[index]));
		return;
	}
	error(_error_silent, "chiefrim: gave Chief weapon %ld, %s", index, tag_get_name(combat.weapons[index]));
	chiefrim_console("Chiefrim: gave Chief the %s", chiefrim_weapon_short_name(combat.weapons[index]));
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
so a headshot is Chiefrim's: a bullet that can cause one, on a person's head
shape (or, a proxy without shapes, within a head's reach of its head
marker), kills it as one kills a marine. A creature's head is no headshot:
one bullet doesn't drop a dragon */
void chiefrim_proxy_struck(long object_index, real_point3d const *point, long damage_definition_index)
{
	struct chiefrim_proxy *proxy = chiefrim_proxy_of(object_index);
	struct object_marker marker;
	struct object_datum *object;
	long slot;
	real reach, dx, dy, dz;
	boolean can_headshot = damage_definition_index != NONE &&
		(damage_effect_definition_get(damage_definition_index)->damage.flags & CHIEFRIM_HEADSHOT_FLAG);

	if (proxy && proxy->hitbox_count > 0)
	{
		long index, nearest = 0;
		real nearest_distance = REAL_MAX;

		for (index = 0; index < proxy->hitbox_count; index++)
		{
			struct chiefrim_hitbox const *box = &proxy->hitboxes[index];
			real_point3d axis;
			real distance;

			chiefrim_segment_nearest(point, &box->a, &box->b, &axis);
			distance = distance3d(point, &axis) - box->radius;
			if (distance < nearest_distance)
			{
				nearest_distance = distance;
				nearest = index;
			}
			if (can_headshot && (box->flags & CR_HITBOX_HEAD) &&
				distance3d(point, &axis) <= box->radius * CHIEFRIM_HEAD_SLACK)
			{
				proxy->headshot = TRUE;
			}
		}
		if (combat.shape_hits++ < 12)
			error(_error_silent, "chiefrim: a shot hit %08X on shape %ld of %ld%s", proxy->form_id, nearest, proxy->hitbox_count,
				proxy->headshot ? ": a headshot" : (proxy->hitboxes[nearest].flags & CR_HITBOX_HEAD) ? ": its head" : "");
		return;
	}
	if (!combat.proxy_is_chief || !can_headshot ||
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
	return chiefrim_proxy_of(object_index) != NULL;
}

/* collisions.c's hook (shots, melee, explosions' line of sight): a proxy
with its actor's shapes is hit on them, nearer than what the ray has met
so far. NONE: not such a proxy (its biped's model, as for any object) */
long chiefrim_proxy_test_vector(long object_index, real_point3d const *point, real_vector3d const *vector, struct collision_result *collision)
{
	struct chiefrim_proxy *proxy = chiefrim_proxy_of(object_index);
	long index;
	boolean hit = FALSE;

	if (!proxy || proxy->hitbox_count <= 0)
		return NONE;
	for (index = 0; index < proxy->hitbox_count; index++)
	{
		real t;
		real_vector3d normal;

		if (chiefrim_ray_capsule(point, vector, &proxy->hitboxes[index], &t, &normal) && t < collision->t)
		{
			collision->type = _collision_result_object;
			collision->t = t;
			collision->plane.n = normal;
			collision->plane.d = normal.i * (point->x + vector->i * t) + normal.j * (point->y + vector->j * t) + normal.k * (point->z + vector->k * t);
			collision->material_type = _material_human;
			collision->object_index = object_index;
			collision->region_index = NONE;
			/* its biped's root: what sticks (a plasma grenade, a needle) is
			attached there and goes where the proxy goes (NONE halted Halo:
			object_has_node) */
			collision->node_index = 0;
			collision->bsp_index = NONE;
			collision->surface_index = NONE;
			collision->plane_designator = NONE;
			collision->flags = 0;
			collision->breakable_surface_index = 0;
			collision->material_index = NONE;
			hit = TRUE;
		}
	}
	return hit;
}

/* objects.c's hook, where an object's bounding sphere is set: a proxy's
holds its actor's shapes (a dragon is bigger than any biped), so rays and
explosions look for it there */
void chiefrim_proxy_bounds(long object_index, real_point3d *center, real *radius)
{
	struct chiefrim_proxy *proxy = chiefrim_proxy_of(object_index);
	real_point3d low, high;
	long index;
	real reach = 0.f;

	if (!proxy || proxy->hitbox_count <= 0)
		return;
	low = high = proxy->hitboxes[0].a;
	for (index = 0; index < proxy->hitbox_count; index++)
	{
		struct chiefrim_hitbox const *box = &proxy->hitboxes[index];

		low.x = MIN(low.x, MIN(box->a.x, box->b.x) - box->radius);
		low.y = MIN(low.y, MIN(box->a.y, box->b.y) - box->radius);
		low.z = MIN(low.z, MIN(box->a.z, box->b.z) - box->radius);
		high.x = MAX(high.x, MAX(box->a.x, box->b.x) + box->radius);
		high.y = MAX(high.y, MAX(box->a.y, box->b.y) + box->radius);
		high.z = MAX(high.z, MAX(box->a.z, box->b.z) + box->radius);
	}
	center->x = 0.5f * (low.x + high.x);
	center->y = 0.5f * (low.y + high.y);
	center->z = 0.5f * (low.z + high.z);
	for (index = 0; index < proxy->hitbox_count; index++)
	{
		struct chiefrim_hitbox const *box = &proxy->hitboxes[index];

		reach = MAX(reach, MAX(distance3d(center, &box->a), distance3d(center, &box->b)) + box->radius);
	}
	*radius = reach;
}

/* damage.c's hook: an explosion's distance to a proxy with shapes is to the
nearest of their middles (a grenade at a dragon's tail is near it), never
further than Halo's own, to its centre */
void chiefrim_proxy_area_distance(long object_index, real_point3d const *epicenter, real *distance)
{
	struct chiefrim_proxy *proxy = chiefrim_proxy_of(object_index);
	long index;

	if (!proxy)
		return;
	for (index = 0; index < proxy->hitbox_count; index++)
	{
		real_point3d axis;

		chiefrim_segment_nearest(epicenter, &proxy->hitboxes[index].a, &proxy->hitboxes[index].b, &axis);
		*distance = MIN(*distance, distance3d(epicenter, &axis));
	}
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

/* Skyrim's console turned debug drawing on or off (CR_MSG_DEBUG) */
void chiefrim_combat_debug(cr_msg_debug const *message)
{
	if (((combat_debug ^ message->flags) & CR_DEBUG_HITBOXES))
		error(_error_silent, "chiefrim: the proxies' hit shapes %s", (message->flags & CR_DEBUG_HITBOXES) ? "drawn" : "hidden");
	combat_debug = message->flags;
}

/* a ring of segments around centre, in the plane of u and v (unit, square) */
static void chiefrim_draw_arc(real_point3d const *centre, real_vector3d const *u, real_vector3d const *v, real radius,
	real from, real to, real_argb_color const *color)
{
	enum { segments = 12 };
	real_point3d previous, point;
	long index;

	for (index = 0; index <= segments; index++)
	{
		real angle = from + (to - from) * (real)index / (real)segments;
		real c = cosine(angle) * radius, s = sine(angle) * radius;

		point.x = centre->x + u->i * c + v->i * s;
		point.y = centre->y + u->j * c + v->j * s;
		point.z = centre->z + u->k * c + v->k * s;
		if (index > 0)
			rasterizer_debug_line(&previous, &point, color);
		previous = point;
	}
}

/* a capsule as a wireframe: rings at its ends, four lines between, its
caps' arcs; a sphere as three rings */
static void chiefrim_draw_capsule(real_point3d const *a, real_point3d const *b, real radius, real_argb_color const *color)
{
	real_vector3d axis, u, v;
	real length;

	vector_from_points3d(a, b, &axis);
	length = normalize3d(&axis);
	if (length < 0.001f)
	{
		axis.i = 0.f;
		axis.j = 0.f;
		axis.k = 1.f;
	}
	normalize3d(perpendicular3d(&axis, &u));
	cross_product3d(&axis, &u, &v);
	chiefrim_draw_arc(a, &u, &v, radius, 0.f, 2.f * (real)M_PI, color);
	if (length < 0.001f)
	{
		chiefrim_draw_arc(a, &u, &axis, radius, 0.f, 2.f * (real)M_PI, color);
		chiefrim_draw_arc(a, &v, &axis, radius, 0.f, 2.f * (real)M_PI, color);
		return;
	}
	chiefrim_draw_arc(b, &u, &v, radius, 0.f, 2.f * (real)M_PI, color);
	{
		real_vector3d sides[4];
		long index;

		sides[0] = u;
		sides[1] = v;
		scale_vector3d(&u, -1.f, &sides[2]);
		scale_vector3d(&v, -1.f, &sides[3]);
		for (index = 0; index < 4; index++)
		{
			real_point3d from, to;

			point_from_line3d(a, &sides[index], radius, &from);
			point_from_line3d(b, &sides[index], radius, &to);
			rasterizer_debug_line(&from, &to, color);
		}
	}
	/* the caps: half rings over each end, outward */
	chiefrim_draw_arc(b, &u, &axis, radius, 0.f, (real)M_PI, color);
	chiefrim_draw_arc(b, &v, &axis, radius, 0.f, (real)M_PI, color);
	chiefrim_draw_arc(a, &u, &axis, radius, (real)M_PI, 2.f * (real)M_PI, color);
	chiefrim_draw_arc(a, &v, &axis, radius, (real)M_PI, 2.f * (real)M_PI, color);
}

/* render.c's hook, on the overlay's screen layer: with the console's
"chiefrim shapes", each proxy's hit shapes over everything (yellow; a
person's head red; from bounds, cyan), or, without shapes, its biped's
standing pill (white) */
void chiefrim_render_hitboxes(void)
{
	static real_argb_color const body = { 1.f, 1.f, 0.85f, 0.1f }, head = { 1.f, 1.f, 0.15f, 0.1f },
		bounds = { 1.f, 0.2f, 0.9f, 1.f }, biped = { 1.f, 0.9f, 0.9f, 0.9f };
	long slot, index;

	if (!(combat_debug & CR_DEBUG_HITBOXES))
		return;
	for (slot = 0; slot < CHIEFRIM_PROXIES; slot++)
	{
		struct chiefrim_proxy const *proxy = &combat.proxies[slot];

		if (!proxy->form_id || proxy->object_index == NONE || !object_try_and_get(proxy->object_index))
			continue;
		for (index = 0; index < proxy->hitbox_count; index++)
		{
			struct chiefrim_hitbox const *box = &proxy->hitboxes[index];

			chiefrim_draw_capsule(&box->a, &box->b, box->radius,
				(box->flags & CR_HITBOX_HEAD) ? &head : (box->flags & CR_HITBOX_BOUNDS) ? &bounds : &body);
		}
		if (proxy->hitbox_count == 0)
		{
			struct object_datum *object = object_get(proxy->object_index);
			real scale = object->object.scale > 0.f ? object->object.scale : 1.f, radius = 0.1f * scale;
			real_point3d feet, top;

			object_get_origin(proxy->object_index, &feet);
			top = feet;
			feet.z += radius;
			top.z += MAX(combat.proxy_height * scale - radius, radius);
			chiefrim_draw_capsule(&feet, &top, radius, &biped);
		}
	}
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
		combat.proxies[slot].hitbox_count = 0;
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
		chiefrim_give_weapon(chief, (cr_msg_give_weapon const *)message);
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
