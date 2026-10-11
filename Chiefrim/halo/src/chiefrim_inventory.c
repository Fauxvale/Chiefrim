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

Halo's weapons as Skyrim items (docs §8.4, protocol 29):

- GIVE: the player equipped one (CR_MSG_GIVE_WEAPON, CR_GIVE_ITEM). Once
  Halo's world is in place, Chief takes it with its rounds and has it in
  hand; or his own of the same takes its rounds, up to its maximum, and the
  rest goes back; or (an energy weapon) it takes his one's place. What he
  can't take goes back whole.
- RETURN: each weapon he lets go of (units.c: a swap, a third weapon, one
  of the same taking its place) that Skyrim has an item for (its arsenal,
  CR_MSG_ARSENAL: the rest stay in Halo's world, as before) goes back to Skyrim with its rounds
  (CR_MSG_WEAPON_RETURNED) and is deleted from Halo's world the frame after:
  none of his lies about in Halo's world. One that can't fire any more Halo
  deletes as it drops it, as ever, and doesn't go back. Not as he dies:
  Skyrim loads a save then.
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cache/cache_files.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/players.h"
#include "items/items.h"
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
#define CHIEFRIM_GIVES_MAX 8        /* waiting for the world */
#define CHIEFRIM_RELEASED_MAX 8     /* let go of this frame, deleted the next */

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
	cr_msg_give_weapon gives[CHIEFRIM_GIVES_MAX]; /* from Skyrim's items, waiting for the world */
	long give_count;
	long released[CHIEFRIM_RELEASED_MAX]; /* his weapons let go of, gone to Skyrim: to delete */
	long released_count;
	cr_msg_arsenal arsenal;    /* the weapons Skyrim has items for */
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

/* A weapon and its rounds; its tag left empty if its name is too long (no
host map's is) */
static void chiefrim_weapon_capture(long weapon_index, cr_chief_weapon *saved)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	char const *name = tag_get_name(weapon->definition_index);
	short magazine;

	memset(saved, 0, sizeof(*saved));
	if (strlen(name) >= CR_WEAPON_TAG_LENGTH)
		return;
	strcpy(saved->tag, name);
	for (magazine = 0; magazine < chiefrim_magazine_count(weapon_index); magazine++)
	{
		saved->rounds_total[magazine] = weapon->weapon.magazines[magazine].rounds_total;
		saved->rounds_loaded[magazine] = weapon->weapon.magazines[magazine].rounds_loaded;
	}
	saved->age = weapon->weapon.age;
}

static void chiefrim_inventory_capture(long chief, cr_chief_state *state)
{
	struct unit_datum *unit = unit_get(chief);
	short slot;
	long grenade;

	memset(state, 0, sizeof(*state));
	state->generation = inventory.generation;
	/* mid-switch, the one he's switching to */
	slot = unit->unit.desired_weapon_index != NONE ? unit->unit.desired_weapon_index : unit->unit.current_weapon_index;
	state->current_weapon = slot >= 0 && slot < CR_CHIEF_WEAPONS ? slot : -1;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT && slot < CR_CHIEF_WEAPONS; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index != NONE && weapon_try_and_get(weapon_index))
			chiefrim_weapon_capture(weapon_index, &state->weapons[slot]);
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
long chiefrim_weapon_tag(char const *name)
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

		/* negative: the weapon's own, as object_new made it (a new one's).
		Its total is its spare rounds, beside those loaded (a reload moves
		them across, firing takes from those loaded) */
		if (saved->rounds_total[magazine] >= 0)
			rounds->rounds_total = (short)PIN(saved->rounds_total[magazine], 0, limits->rounds_total_maximum);
		if (saved->rounds_loaded[magazine] >= 0)
			rounds->rounds_loaded = (short)PIN(saved->rounds_loaded[magazine], 0, limits->rounds_loaded_maximum);
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

/* ---------- Halo's weapons as Skyrim items */

static void chiefrim_weapon_return(uint32_t reason, uint32_t item, long taken, cr_chief_weapon const *weapon)
{
	struct cr_shared *shm = chiefrim_shared();
	cr_msg_weapon_returned message;

	if (!shm || !chiefrim_linked())
		return;
	memset(&message, 0, sizeof(message));
	message.reason = reason;
	message.item = item;
	message.taken = (int32_t)taken;
	message.weapon = *weapon;
	if (!cr_ring_push(&shm->to_skyrim, CR_MSG_WEAPON_RETURNED, &message, sizeof(message)))
		error(_error_silent, "chiefrim: the ring to Skyrim is full: %s isn't returned", weapon->tag);
}

/* A magazine's rounds as a new weapon of this definition has them
(weapons.c's weapon_new): loaded as many as fit, the rest spare */
static void chiefrim_rounds_own(struct weapon_magazine_definition const *limits, short *total, short *loaded)
{
	*loaded = (short)MIN(limits->rounds_total_initial, limits->rounds_loaded_maximum);
	*total = (short)(limits->rounds_total_initial - *loaded);
}

/* The item's rounds into Chief's weapon of the same, up to its maximum:
its spare rounds, then those loaded. Returns the rounds taken; the item is
left with the rest */
static long chiefrim_merge_rounds(long held_index, cr_chief_weapon *item)
{
	struct weapon_datum *held = weapon_get(held_index);
	struct weapon_definition *definition = weapon_definition_get(held->definition_index);
	short magazine;
	long taken = 0;

	for (magazine = 0; magazine < chiefrim_magazine_count(held_index); magazine++)
	{
		struct weapon_magazine_definition *limits =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, magazine, struct weapon_magazine_definition);
		struct weapon_magazine *rounds = &held->weapon.magazines[magazine];
		short total = item->rounds_total[magazine], loaded = item->rounds_loaded[magazine];
		short room = (short)MAX(limits->rounds_total_maximum - rounds->rounds_total, 0), take;

		if (total < 0 || loaded < 0)
		{
			short own_total, own_loaded;

			chiefrim_rounds_own(limits, &own_total, &own_loaded);
			total = total < 0 ? own_total : total;
			loaded = loaded < 0 ? own_loaded : loaded;
		}
		take = MIN(room, total);
		total -= take;
		room -= take;
		rounds->rounds_total += take;
		taken += take;
		take = MIN(room, loaded);
		loaded -= take;
		rounds->rounds_total += take;
		taken += take;
		item->rounds_total[magazine] = total;
		item->rounds_loaded[magazine] = loaded;
	}
	return taken;
}

/* Its rounds can be taken from another of the same: a magazine holds spare
rounds (an energy weapon's has none: Halo's pickup never merges them) */
static boolean chiefrim_weapon_takes_rounds(long weapon_index)
{
	struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
	short magazine;

	for (magazine = 0; magazine < chiefrim_magazine_count(weapon_index); magazine++)
	{
		if (TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, magazine, struct weapon_magazine_definition)->rounds_total_maximum > 0)
			return TRUE;
	}
	return FALSE;
}

static boolean chiefrim_weapon_rounds_left(cr_chief_weapon const *item, short magazines)
{
	short magazine;

	for (magazine = 0; magazine < magazines; magazine++)
	{
		if (item->rounds_total[magazine] > 0 || item->rounds_loaded[magazine] > 0)
			return TRUE;
	}
	return FALSE;
}

/* Chief's weapon of this definition, or NONE */
static long chiefrim_held_weapon(long chief, long definition_index, short *slot_out)
{
	struct unit_datum *unit = unit_get(chief);
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index != NONE && weapon_try_and_get(weapon_index) &&
			weapon_get(weapon_index)->definition_index == definition_index)
		{
			*slot_out = slot;
			return weapon_index;
		}
	}
	return NONE;
}

static void chiefrim_give_item(long chief, cr_msg_give_weapon const *give)
{
	cr_chief_weapon item = give->weapon;
	struct unit_datum *unit;
	long tag_index, held_index;
	short slot;

	item.tag[CR_WEAPON_TAG_LENGTH - 1] = 0;
	tag_index = chiefrim_weapon_tag(item.tag);
	if (chief == NONE || !unit_try_and_get(chief) || chiefrim_combat_chief_dead() || tag_index == NONE)
	{
		error(_error_silent, "chiefrim: Chief can't take %s (%s); back to Skyrim", item.tag,
			tag_index == NONE ? "not a weapon of the host map he may have" : "no Chief");
		chiefrim_weapon_return(CR_RETURNED_REFUSED, give->item, 0, &item);
		return;
	}
	unit = unit_get(chief);
	held_index = chiefrim_held_weapon(chief, tag_index, &slot);
	if (held_index != NONE && chiefrim_weapon_takes_rounds(held_index))
	{
		short magazines = chiefrim_magazine_count(held_index);
		long taken = chiefrim_merge_rounds(held_index, &item);

		error(_error_silent, "chiefrim: Chief's %s took %ld rounds of another%s", item.tag, taken,
			chiefrim_weapon_rounds_left(&item, magazines) ? "; the rest back to Skyrim" : "");
		if (!chiefrim_weapon_rounds_left(&item, magazines))
			item.tag[0] = 0; /* all taken: nothing comes back */
		chiefrim_weapon_return(CR_RETURNED_LEFTOVER, give->item, taken, &item);
		return;
	}
	if (held_index != NONE)
	{
		/* an energy weapon: this one takes his one's place, which goes back
		(chiefrim_weapon_released) */
		unit->unit.desired_weapon_index = slot;
		unit_drop_selected_weapon(chief);
	}
	else if (unit_get_weapon_count(chief) >= 2)
	{
		unit_drop_selected_weapon(chief);
	}
	slot = chiefrim_inventory_add(chief, &item);
	if (slot == NONE)
	{
		chiefrim_weapon_return(CR_RETURNED_REFUSED, give->item, 0, &item);
		return;
	}
	unit->unit.desired_weapon_index = slot;
	player_control_set_desired_weapon(chief, slot);
	error(_error_silent, "chiefrim: Chief took %s from Skyrim (%d/%d rounds, battery %.2f)", item.tag,
		item.rounds_loaded[0], item.rounds_total[0], 1.f - item.age);
}

/* the weapons let go of last frame, gone to Skyrim, out of Halo's world
(unless something picked one up since) */
static void chiefrim_released_delete(void)
{
	long index;

	for (index = 0; index < inventory.released_count; index++)
	{
		long weapon_index = inventory.released[index];

		if (object_try_and_get(weapon_index) && object_get(weapon_index)->object.parent_object_index == NONE)
		{
			error(_error_silent, "chiefrim: %s out of Halo's world (it's Skyrim's now)", tag_get_name(object_get(weapon_index)->definition_index));
			object_delete(weapon_index);
		}
	}
	inventory.released_count = 0;
}

/* ---------- public code */

static boolean chiefrim_arsenal_has(char const *tag)
{
	uint32_t index;

	for (index = 0; index < inventory.arsenal.count && index < CR_ARSENAL_MAX; index++)
	{
		if (!strncasecmp(inventory.arsenal.tags[index], tag, CR_WEAPON_TAG_LENGTH))
			return TRUE;
	}
	return FALSE;
}

void chiefrim_weapon_released(long unit_index, long weapon_index)
{
	cr_chief_weapon weapon;

	if (!chiefrim_active() || !chiefrim_linked() || unit_index == NONE || unit_index != chiefrim_local_unit() ||
		chiefrim_combat_chief_dead() || !weapon_try_and_get(weapon_index) || !weapon_can_be_fired(weapon_index))
	{
		return;
	}
	chiefrim_weapon_capture(weapon_index, &weapon);
	if (!weapon.tag[0] || !chiefrim_arsenal_has(weapon.tag))
		return; /* no item for it in Skyrim: it stays in Halo's world */
	chiefrim_weapon_return(CR_RETURNED_DROPPED, 0, 0, &weapon);
	if (inventory.released_count < CHIEFRIM_RELEASED_MAX)
		inventory.released[inventory.released_count++] = weapon_index;
	error(_error_silent, "chiefrim: Chief let go of %s (%d/%d rounds, battery %.2f): back to Skyrim", weapon.tag,
		weapon.rounds_loaded[0], weapon.rounds_total[0], 1.f - weapon.age);
}

void chiefrim_inventory_arsenal(cr_msg_arsenal const *message)
{
	uint32_t index;

	inventory.arsenal = *message;
	for (index = 0; index < CR_ARSENAL_MAX; index++)
		inventory.arsenal.tags[index][CR_WEAPON_TAG_LENGTH - 1] = 0;
	error(_error_silent, "chiefrim: Skyrim has items for %u of Halo's weapons", MIN(message->count, CR_ARSENAL_MAX));
}

void chiefrim_inventory_give(cr_msg_give_weapon const *message)
{
	if (inventory.give_count >= CHIEFRIM_GIVES_MAX)
	{
		cr_chief_weapon weapon = message->weapon;

		weapon.tag[CR_WEAPON_TAG_LENGTH - 1] = 0;
		chiefrim_weapon_return(CR_RETURNED_REFUSED, message->item, 0, &weapon);
		return;
	}
	inventory.gives[inventory.give_count++] = *message;
}

void chiefrim_inventory_linked(void)
{
	/* a new Skyrim side: it waits on no restore of ours, and hears the kit again */
	inventory.generation = 0;
	inventory.pending = FALSE;
	inventory.sent_any = FALSE;
	inventory.give_count = 0; /* the Skyrim that sent them is gone */
	inventory.released_count = 0;
	inventory.arsenal.count = 0; /* the new one sends its own */
}

void chiefrim_inventory_message(cr_msg_chief_state const *message)
{
	inventory.restore = message->state;
	inventory.pending = TRUE;
}

void chiefrim_inventory_update(long chief, boolean world_valid, boolean dead)
{
	cr_chief_state state;

	chiefrim_released_delete();
	if (chief == NONE || !unit_try_and_get(chief))
		return;
	if (inventory.pending && world_valid)
	{
		chiefrim_inventory_apply(chief, &inventory.restore);
		inventory.generation = inventory.restore.generation;
		inventory.pending = FALSE;
		inventory.sent_any = FALSE; /* tell Skyrim at once */
	}
	if (!inventory.pending && world_valid && inventory.give_count > 0)
	{
		long index;

		for (index = 0; index < inventory.give_count; index++)
			chiefrim_give_item(chief, &inventory.gives[index]);
		inventory.give_count = 0;
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
