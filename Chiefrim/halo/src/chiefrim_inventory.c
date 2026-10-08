/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_INVENTORY.C

Chief's kit in Skyrim's saves (Chiefrim/docs/DESIGN.md §11, save and load):

- STATE: his weapons (by tag path, with their rounds and an energy weapon's
  battery), the one in hand, his grenades, his body and shield vitality and
  his flashlight's battery go to Skyrim (CR_MSG_CHIEF_STATE) when they change,
  four times a second at most. Skyrim keeps the latest for its next save.
- RESTORE: loading a Skyrim save sends its kit back (CR_MSG_CHIEF_RESTORE),
  or Chiefrim.ini's starting loadout when the save has none (weapons named
  by their last part, with their own rounds), or the host map's. Chief
  gives up what he carries and takes exactly that. It waits for Chief's unit
  and for the world, and comes after the world's reset in the same frame
  (chiefrim_apply_world makes him whole), so the saved vitality stays.

Each restore has a generation, and the states carry the last one applied:
Skyrim drops states from before the restore it waits on.
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cache/cache_files.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/players.h"
#include "interface/first_person_weapons.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include <string.h>
#include <strings.h>

/* ---------- constants */

#define CHIEFRIM_STATE_INTERVAL_MS 250

/* player_control.c's, not in its header */
struct player_control *player_control_get(short local_player_index);

/* ---------- globals */

static struct
{
	uint32_t generation;       /* the last restore applied (0: none, Halo's own Chief) */
	boolean pending;           /* a restore waits for Chief and the world */
	cr_chief_state restore;
	boolean sent_any;          /* a state went out since the link */
	cr_chief_state sent;       /* the last state sent */
	uint32_t sent_ms;
} inventory;

/* ---------- private code */

static void chiefrim_inventory_push(cr_chief_state const *state)
{
	struct cr_shared *shm = chiefrim_shared();
	cr_msg_chief_state message;

	if (!shm || !chiefrim_linked())
		return;
	memset(&message, 0, sizeof(message));
	message.state = *state;
	if (cr_ring_push(&shm->to_skyrim, CR_MSG_CHIEF_STATE, &message, sizeof(message)))
	{
		inventory.sent = *state;
		inventory.sent_any = TRUE;
		inventory.sent_ms = (uint32_t)system_milliseconds();
	}
}

static short chiefrim_magazine_count(long weapon_index)
{
	struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);

	return (short)MIN(definition->weapon.magazines.count, 2);
}

static void chiefrim_inventory_capture(long chief, cr_chief_state *state)
{
	struct unit_datum *unit = unit_get(chief);
	short slot, magazine;
	long grenade;

	memset(state, 0, sizeof(*state));
	state->generation = inventory.generation;
	/* mid-switch, the one he's switching to */
	slot = unit->unit.desired_weapon_index != NONE ? unit->unit.desired_weapon_index : unit->unit.current_weapon_index;
	state->current_weapon = slot >= 0 && slot < CR_CHIEF_WEAPONS ? slot : -1;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT && slot < CR_CHIEF_WEAPONS; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];
		cr_chief_weapon *saved = &state->weapons[slot];
		struct weapon_datum *weapon;
		char const *name;

		if (weapon_index == NONE || !weapon_try_and_get(weapon_index))
			continue;
		weapon = weapon_get(weapon_index);
		name = tag_get_name(weapon->definition_index);
		if (strlen(name) >= CR_WEAPON_TAG_LENGTH)
			continue; /* no host map's weapon is named that long */
		strcpy(saved->tag, name);
		for (magazine = 0; magazine < chiefrim_magazine_count(weapon_index); magazine++)
		{
			saved->rounds_total[magazine] = weapon->weapon.magazines[magazine].rounds_total;
			saved->rounds_loaded[magazine] = weapon->weapon.magazines[magazine].rounds_loaded;
		}
		saved->age = weapon->weapon.age;
	}
	if (state->current_weapon >= 0 && !state->weapons[state->current_weapon].tag[0])
		state->current_weapon = -1;
	state->current_grenade = unit->unit.current_grenade_index;
	for (grenade = 0; grenade < NUMBER_OF_UNIT_GRENADE_TYPES && grenade < CR_CHIEF_GRENADES; grenade++)
		state->grenades[grenade] = (uint8_t)MAX(unit->unit.grenade_counts[grenade], 0);
	state->body = unit->object.body_vitality;
	state->shield = unit->object.shield_vitality;
	state->flashlight = unit->unit.integrated_light_battery;
}

/* The map's weapon by its tag path, or by the path's last part ("shotgun"),
in any case */
static long chiefrim_weapon_tag(char const *name)
{
	struct tag_iterator iterator;
	long tag_index, found = NONE;
	size_t length = strlen(name);

	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *path = tag_get_name(tag_index);
		size_t path_length = strlen(path);

		if (!chiefrim_weapon_carried(path))
			continue;
		if (!strcasecmp(path, name))
			return tag_index;
		if (found == NONE && path_length > length && path[path_length - length - 1] == '\\' &&
			!strcasecmp(path + path_length - length, name))
		{
			found = tag_index;
		}
	}
	return found;
}

/* Everything he carries, gone; the one in hand first leaves his hands */
static void chiefrim_inventory_clear(long chief)
{
	struct unit_datum *unit = unit_get(chief);
	short slot;

	if (unit->unit.current_weapon_index != NONE)
		first_person_weapon_message_from_unit(chief, _first_person_weapon_message_drop);
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		unit->unit.weapon_object_indices[slot] = NONE;
		if (weapon_index != NONE && object_try_and_get(weapon_index))
			object_delete(weapon_index);
	}
	unit->unit.current_weapon_index = NONE;
	unit->unit.desired_weapon_index = NONE;
	for (slot = 0; slot < NUMBER_OF_UNIT_GRENADE_TYPES; slot++)
		unit->unit.grenade_counts[slot] = 0;
}

/* One saved weapon into his inventory; its slot there, or NONE */
static short chiefrim_inventory_add(long chief, cr_chief_weapon const *saved)
{
	struct unit_datum *unit = unit_get(chief);
	struct object_placement_data data;
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	long tag_index, weapon_index;
	short slot, magazine;

	tag_index = chiefrim_weapon_tag(saved->tag);
	if (tag_index == NONE)
	{
		error(_error_silent, "chiefrim: %s isn't a weapon Chief can have in the host map; left out", saved->tag);
		return NONE;
	}
	object_placement_data_new(&data, tag_index, chief);
	object_get_origin(chief, &data.position);
	weapon_index = object_new(&data);
	if (weapon_index == NONE)
		return NONE;
	if (!unit_add_weapon_to_inventory(chief, weapon_index, _unit_add_weapon_normal))
	{
		object_delete(weapon_index);
		error(_error_silent, "chiefrim: couldn't give Chief %s (the AI's only?)", saved->tag);
		return NONE;
	}
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT && unit->unit.weapon_object_indices[slot] != weapon_index; slot++)
		;
	weapon = weapon_get(weapon_index);
	definition = weapon_definition_get(weapon->definition_index);
	for (magazine = 0; magazine < chiefrim_magazine_count(weapon_index); magazine++)
	{
		struct weapon_magazine_definition *limits =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, magazine, struct weapon_magazine_definition);
		struct weapon_magazine *rounds = &weapon->weapon.magazines[magazine];

		/* negative: the weapon's own, as one lying in the map has them */
		rounds->rounds_total = saved->rounds_total[magazine] < 0 ? limits->rounds_total_initial :
			(short)PIN(saved->rounds_total[magazine], 0, limits->rounds_total_maximum);
		rounds->rounds_loaded = saved->rounds_loaded[magazine] < 0 ? MIN(limits->rounds_loaded_maximum, rounds->rounds_total) :
			(short)PIN(saved->rounds_loaded[magazine], 0, MIN(limits->rounds_loaded_maximum, rounds->rounds_total));
	}
	weapon->weapon.age = PIN(saved->age, 0.f, 1.f);
	return slot < MAXIMUM_WEAPONS_PER_UNIT ? slot : NONE;
}

static void chiefrim_inventory_apply(long chief, cr_chief_state const *state)
{
	struct unit_datum *unit = unit_get(chief);
	short in_hand = NONE;
	long index;

	chiefrim_inventory_clear(chief);
	if (state->flags & CR_CHIEF_STARTING_LOADOUT)
	{
		player_add_equipment(chief, 0, FALSE);
		if (unit->unit.weapon_object_indices[0] != NONE)
			in_hand = 0;
		unit->object.body_vitality = 1.f;
		unit->object.shield_vitality = 1.f;
		unit->unit.integrated_light_battery = 1.f;
		error(_error_silent, "chiefrim: the host map's starting loadout");
	}
	else
	{
		long weapons = 0;

		for (index = 0; index < CR_CHIEF_WEAPONS; index++)
		{
			cr_chief_weapon weapon = state->weapons[index];
			short slot;

			weapon.tag[CR_WEAPON_TAG_LENGTH - 1] = 0;
			if (!weapon.tag[0])
				continue;
			slot = chiefrim_inventory_add(chief, &weapon);
			if (slot == NONE)
				continue;
			weapons++;
			if (index == state->current_weapon || in_hand == NONE)
				in_hand = slot;
		}
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES && index < CR_CHIEF_GRENADES; index++)
			unit->unit.grenade_counts[index] = (char)MIN(state->grenades[index], 127);
		if (state->current_grenade >= 0 && state->current_grenade < NUMBER_OF_UNIT_GRENADE_TYPES)
		{
			long local = unit->unit.player_index != NONE ? player_get(unit->unit.player_index)->local_player_index : NONE;

			unit->unit.current_grenade_index = unit->unit.desired_grenade_index = (char)state->current_grenade;
			if (local != NONE) /* the player's choice, which the unit's follows */
				player_control_get((short)local)->desired_grenade_index = (short)state->current_grenade;
		}
		/* a save made dead would kill him again: a sliver at least */
		unit->object.body_vitality = PIN(state->body, 0.05f, 1.f);
		unit->object.shield_vitality = PIN(state->shield, 0.f, 3.f);
		unit->unit.integrated_light_battery = PIN(state->flashlight, 0.f, 1.f);
		error(_error_silent, "chiefrim: Chief's kit from Skyrim: %ld weapons, grenades %d/%d, body %.2f, shields %.2f",
			weapons, state->grenades[0], state->grenades[1], state->body, state->shield);
	}
	unit->object.current_body_damage = 0.f;
	unit->object.current_shield_damage = 0.f;
	if (in_hand != NONE)
	{
		unit->unit.desired_weapon_index = in_hand;
		player_control_set_desired_weapon(chief, in_hand);
	}
}

/* ---------- public code */

void chiefrim_inventory_linked(void)
{
	/* a new Skyrim side: it waits on no restore of ours, and hears the kit again */
	inventory.generation = 0;
	inventory.pending = FALSE;
	inventory.sent_any = FALSE;
}

void chiefrim_inventory_message(cr_msg_chief_state const *message)
{
	inventory.restore = message->state;
	inventory.pending = TRUE;
}

void chiefrim_inventory_update(long chief, boolean world_valid, boolean dead)
{
	cr_chief_state state;

	if (chief == NONE || !unit_try_and_get(chief))
		return;
	if (inventory.pending && world_valid)
	{
		chiefrim_inventory_apply(chief, &inventory.restore);
		inventory.generation = inventory.restore.generation;
		inventory.pending = FALSE;
		inventory.sent_any = FALSE; /* tell Skyrim at once */
	}
	if (inventory.pending || dead)
		return; /* a dead Chief's kit is no one's to save */
	chiefrim_inventory_capture(chief, &state);
	if (inventory.sent_any &&
		((uint32_t)system_milliseconds() - inventory.sent_ms < CHIEFRIM_STATE_INTERVAL_MS ||
		!memcmp(&state, &inventory.sent, sizeof(state))))
	{
		return;
	}
	chiefrim_inventory_push(&state);
}
