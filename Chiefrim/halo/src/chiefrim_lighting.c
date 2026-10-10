/* SPDX-License-Identifier: GPL-3.0-or-later */
/* chiefrim_lighting.c: Skyrim's light on Halo's objects (docs §9).

Halo lights an object from its map's lightmap under it (object_lights.c):
an ambient colour, a key light, a fill, a reflection tint and a shadow. In
Skyrim's world there's no lightmap of Halo's (the collision BSP has none), so
every object had the host map's default lighting: Chief's arms and weapon as
bright in a cave at night as at noon. Skyrim sends the light around the
player instead (CR_MSG_LIGHTING): its directional ambient, its key light (the
sun, the moon, an interior's directional light) and the point lights nearest
the player (torches, fires, spells). Each object takes them at its own
position: the nearest strong point light is its fill, the others brighten
its ambient. object_lights.c's hook calls this in place of the lightmap.

Shadows (protocol 21): outside, the sun or moon reaches an object only if
nothing of Skyrim's is in its way. Chief, and what he holds (his arms and
weapon take his lighting), use what Skyrim found at his eye (its own
physics, as far as its loaded cells); every other object casts a ray to it
through Chiefrim's collision BSP, Skyrim's shapes around Chief. Halo's
blend of each object's lighting softens the change. */

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "render/render.h"

#include <math.h>
#include <string.h>

static struct
{
	boolean valid;
	cr_msg_lighting light;
} chiefrim_lighting;

void chiefrim_lighting_message(cr_msg_lighting const *message)
{
	chiefrim_lighting.light = *message;
	if (chiefrim_lighting.light.point_count > CR_LIGHTING_POINTS)
		chiefrim_lighting.light.point_count = CR_LIGHTING_POINTS;
	if (!chiefrim_lighting.valid)
		error(_error_silent, "chiefrim: Skyrim's light on Halo's objects (ambient %.2f %.2f %.2f, key %.2f %.2f %.2f)",
			message->ambient.x, message->ambient.y, message->ambient.z,
			message->key_color.x, message->key_color.y, message->key_color.z);
	chiefrim_lighting.valid = TRUE;
}

void chiefrim_lighting_forget(void)
{
	chiefrim_lighting.valid = FALSE;
}

/* how far towards the sun an object's ray looks for what shades it: past
the collision BSP's reach (iRadius 2 rings of 1024 units, ~12 world units) */
#define CHIEFRIM_SHADOW_REACH 40.f

/* under a roof (no sky over it), the share of the sky's light an object
still gets: what comes in sideways, and off the ground */
#define CHIEFRIM_ROOFED_AMBIENT 0.4f
/* how high a roof over an object is looked for (~2100 Skyrim units) */
#define CHIEFRIM_ROOF_REACH 10.f

/* Chief, or what he holds (his arms and weapon take his lighting): Skyrim
tested his eye's way to the sun and the sky */
static boolean chiefrim_is_chiefs(long object_index, struct object_datum const *object)
{
	long root = object_index, chief = chiefrim_local_unit();
	struct object_datum const *parent = object;

	while (parent->object.parent_object_index != NONE)
	{
		root = parent->object.parent_object_index;
		parent = object_get(root);
	}
	return root == chief && chief != NONE;
}

/* How much of the sky is open over the object (1 inside: the cell's
ambient). Others than Chief's: one ray straight up through the collision BSP */
static real chiefrim_sky_visible(long object_index, struct object_datum const *object, boolean chiefs)
{
	cr_msg_lighting const *light = &chiefrim_lighting.light;
	struct collision_result collision;
	real_vector3d up = { 0.f, 0.f, CHIEFRIM_ROOF_REACH };

	if (!light->key_shadowed)
		return 1.f;
	if (chiefs)
		return PIN(light->sky_visible, 0.f, 1.f);
	return collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_back_facing_surfaces_bit), &object->object.bounding_sphere_center, &up, object_index, &collision) ? 0.f : 1.f;
}

/* How much of the key light reaches the object: 1 inside (an interior's
directional light casts no shadows) */
static real chiefrim_key_visible(long object_index, struct object_datum const *object, real_vector3d const *key, boolean chiefs)
{
	cr_msg_lighting const *light = &chiefrim_lighting.light;
	struct collision_result collision;
	real_point3d from;
	real_vector3d towards;

	if (!light->key_shadowed)
		return 1.f;
	if (chiefs)
		return PIN(light->sun_visible, 0.f, 1.f);
	/* from its centre, a little towards the sun: past its own surface */
	from = object->object.bounding_sphere_center;
	from.x -= key->i * 0.05f;
	from.y -= key->j * 0.05f;
	from.z -= key->k * 0.05f;
	towards.i = -key->i * CHIEFRIM_SHADOW_REACH;
	towards.j = -key->j * CHIEFRIM_SHADOW_REACH;
	towards.k = -key->k * CHIEFRIM_SHADOW_REACH;
	return collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_back_facing_surfaces_bit), &from, &towards, object_index, &collision) ? 0.f : 1.f;
}

static real chiefrim_luminance(real_rgb_color const *color)
{
	return 0.299f * color->red + 0.587f * color->green + 0.114f * color->blue;
}

static void chiefrim_rgb(real_rgb_color *out, cr_vec3 in, real scale)
{
	out->red = MAX(in.x * scale, 0.f);
	out->green = MAX(in.y * scale, 0.f);
	out->blue = MAX(in.z * scale, 0.f);
}

boolean chiefrim_object_lighting(long object_index, struct render_lighting *lighting)
{
	cr_msg_lighting const *light = &chiefrim_lighting.light;
	struct object_datum *object;
	cr_vec3 origin, center;
	real best = 0.f, brightness;
	long point, best_point = NONE;
	real_vector3d key;
	real_rgb_color torch;
	real sky;
	boolean chiefs;

	if (!chiefrim_active() || !chiefrim_lighting.valid || !chiefrim_world_origin(&origin))
		return FALSE;
	object = object_get(object_index);
	/* where the object is, in Skyrim's units */
	center.x = object->object.bounding_sphere_center.x;
	center.y = object->object.bounding_sphere_center.y;
	center.z = object->object.bounding_sphere_center.z;
	center = cr_halo_to_sky(center, origin);

	memset(lighting, 0, sizeof(*lighting));
	lighting->distant_light_count = 2;
	chiefs = chiefrim_is_chiefs(object_index, object);
	/* the sky's light, less under a roof */
	sky = CHIEFRIM_ROOFED_AMBIENT + (1.f - CHIEFRIM_ROOFED_AMBIENT) * chiefrim_sky_visible(object_index, object, chiefs);
	chiefrim_rgb(&lighting->ambient_color, light->ambient, sky);

	key.i = light->key_direction.x;
	key.j = light->key_direction.y;
	key.k = light->key_direction.z;
	if (normalize3d(&key) == 0.f)
	{
		key.i = 0.f;
		key.j = 0.f;
		key.k = -1.f;
	}
	chiefrim_rgb(&lighting->distant_lights[0].color, light->key_color, chiefrim_key_visible(object_index, object, &key, chiefs));
	lighting->distant_lights[0].direction = key;

	/* the fill: the sky's light from above, unless a point light is stronger */
	chiefrim_rgb(&lighting->distant_lights[1].color, light->ambient_up, sky);
	lighting->distant_lights[1].direction.i = 0.f;
	lighting->distant_lights[1].direction.j = 0.f;
	lighting->distant_lights[1].direction.k = -1.f;
	best = chiefrim_luminance(&lighting->distant_lights[1].color);

	for (point = 0; point < (long)light->point_count; point++)
	{
		cr_light_point const *p = &light->points[point];
		real dx = center.x - p->position.x, dy = center.y - p->position.y, dz = center.z - p->position.z;
		real distance = sqrtf(dx * dx + dy * dy + dz * dz);
		real falloff, strength;
		real_rgb_color color;

		if (p->radius <= 1.f || distance >= p->radius)
			continue;
		/* Skyrim's falloff, near enough: 1 - (d/r)^2 */
		falloff = 1.f - (distance / p->radius) * (distance / p->radius);
		chiefrim_rgb(&color, p->color, falloff);
		strength = chiefrim_luminance(&color);
		if (strength > best && distance > 1.f)
		{
			/* the old fill's share goes to the ambient */
			if (best_point != NONE || best > 0.f)
			{
				lighting->ambient_color.red += 0.5f * lighting->distant_lights[1].color.red;
				lighting->ambient_color.green += 0.5f * lighting->distant_lights[1].color.green;
				lighting->ambient_color.blue += 0.5f * lighting->distant_lights[1].color.blue;
			}
			best = strength;
			best_point = point;
			lighting->distant_lights[1].color = color;
			lighting->distant_lights[1].direction.i = dx / distance;
			lighting->distant_lights[1].direction.j = dy / distance;
			lighting->distant_lights[1].direction.k = dz / distance;
		}
		else
		{
			lighting->ambient_color.red += 0.5f * color.red;
			lighting->ambient_color.green += 0.5f * color.green;
			lighting->ambient_color.blue += 0.5f * color.blue;
		}
	}

	/* reflections and the shadow, as build_distant_lights makes them from a
	lightmap: by the light that reaches the object, a torch's (the fill, when
	it is a point light, also tinting it) as well as the sun's. Shiny weapons (the MA5B) show
	mostly their reflection: with the key's alone a torch beside Chief added
	a little diffuse light and no shine (offline, 5 against the key's 36) */
	memset(&torch, 0, sizeof(torch));
	if (best_point != NONE)
		torch = lighting->distant_lights[1].color;
	/* its strength by the light shining on it straight, the sun's and a
	torch's, as Halo's by its lightmap's: with the ambient's in full,
	Skyrim's daylight sky alone (~0.5) made it the most it goes, and the
	pistol's slide shone as bright in a building's shade as in the sun (the
	first in-game look) */
	brightness = chiefrim_luminance(&lighting->distant_lights[0].color) + chiefrim_luminance(&torch) +
		0.35f * chiefrim_luminance(&lighting->ambient_color);
	lighting->reflection_tint_color.alpha = PIN(brightness * 1.5f + 0.25f, 0.f, 1.f);
	/* but no more than its surroundings, as Skyrim's picture shows them
	(protocol 27), give: a reflection is of them. In a dim cabin the MA5B
	shone near white by a fire's light (the room's brightest 0.3) */
	if (light->reflection_cap > 0.f)
		lighting->reflection_tint_color.alpha = MIN(lighting->reflection_tint_color.alpha, PIN(light->reflection_cap, 0.f, 1.f));
	/* tinted by the colour of the light that reaches it, the sun's or moon's
	(past what shades it), the sky's and a torch's: their hue, its largest
	part 1 (how bright is the alpha's). Halo tints by its lightmap's colour
	too, but by its size: Skyrim's daylight (ambient 0.44 0.54 0.51, an
	orange afternoon sun 0.63 0.46 0.35) came out white, the shine never the
	sun's colour (the first in-game look) */
	{
		real red = lighting->ambient_color.red + lighting->distant_lights[0].color.red + torch.red;
		real green = lighting->ambient_color.green + lighting->distant_lights[0].color.green + torch.green;
		real blue = lighting->ambient_color.blue + lighting->distant_lights[0].color.blue + torch.blue;
		real largest = MAX(MAX(red, green), MAX(blue, 0.001f));

		lighting->reflection_tint_color.red = PIN(red / largest, 0.f, 1.f);
		lighting->reflection_tint_color.green = PIN(green / largest, 0.f, 1.f);
		lighting->reflection_tint_color.blue = PIN(blue / largest, 0.f, 1.f);
	}
	lighting->shadow_vector = key;
	if (lighting->shadow_vector.k > -0.5f)
	{
		lighting->shadow_vector.k = -0.5f;
		normalize3d(&lighting->shadow_vector);
	}
	lighting->shadow_color.red = PIN(1.f - lighting->distant_lights[0].color.red * 1.3f + 0.25f, 0.03f, 1.f);
	lighting->shadow_color.green = PIN(1.f - lighting->distant_lights[0].color.green * 1.3f + 0.25f, 0.03f, 1.f);
	lighting->shadow_color.blue = PIN(1.f - lighting->distant_lights[0].color.blue * 1.3f + 0.25f, 0.03f, 1.f);
	return TRUE;
}

/* ---------- Chief's flashlight on Skyrim's world */

/* Halo's flashlight is a light on Chief's biped, which lights Halo's own
world; Chiefrim draws none of that, so it went nowhere. Its light as it
shines now (on, off, fading in) goes to Skyrim (CR_MSG_FLASHLIGHT), which
lights its own world along the player's view with it. */
static struct
{
	boolean sent;          /* Skyrim has one */
	boolean logged;
	uint32_t sent_ms;
	cr_msg_flashlight last;
} chiefrim_flashlight;

void chiefrim_flashlight_linked(void)
{
	chiefrim_flashlight.sent = FALSE;
}

void chiefrim_flashlight_update(long chief)
{
	struct cr_shared *shm = chiefrim_shared();
	cr_msg_flashlight message;
	real_rgb_color color = { 0 };
	real radius = 0.f, cutoff = 0.f, falloff = 0.f;
	uint32_t now = (uint32_t)system_milliseconds();
	boolean on, was_on, changed;

	if (!shm)
		return;
	memset(&message, 0, sizeof(message));
	if (chief != NONE && chiefrim_object_flashlight(chief, &color, &radius, &cutoff, &falloff))
	{
		message.color.x = PIN(color.red, 0.f, 1.f);
		message.color.y = PIN(color.green, 0.f, 1.f);
		message.color.z = PIN(color.blue, 0.f, 1.f);
		message.radius = radius * CR_SKY_UNITS_PER_WU;
		message.cutoff_angle = cutoff;
		message.falloff_angle = falloff;
		if (!chiefrim_flashlight.logged)
		{
			chiefrim_flashlight.logged = TRUE;
			error(_error_silent, "chiefrim: Chief's flashlight: radius %.2f world units, cone %.1f degrees (full to %.1f)",
				radius, cutoff * 180.f / CR_PI, falloff * 180.f / CR_PI);
		}
	}
	on = message.color.x + message.color.y + message.color.z > 0.001f;
	was_on = chiefrim_flashlight.last.color.x + chiefrim_flashlight.last.color.y + chiefrim_flashlight.last.color.z > 0.001f;
	changed = fabsf(message.color.x - chiefrim_flashlight.last.color.x) > 0.01f ||
		fabsf(message.color.y - chiefrim_flashlight.last.color.y) > 0.01f ||
		fabsf(message.color.z - chiefrim_flashlight.last.color.z) > 0.01f ||
		fabsf(message.radius - chiefrim_flashlight.last.radius) > 0.02f * MAX(chiefrim_flashlight.last.radius, 1.f);
	/* switched on or off at once; fading, ~30 times a second */
	if (chiefrim_flashlight.sent && (on == was_on) && (!changed || now - chiefrim_flashlight.sent_ms < 33))
		return;
	if (!cr_ring_push(&shm->to_skyrim, CR_MSG_FLASHLIGHT, &message, sizeof(message)))
		return;
	if (on != was_on)
		error(_error_silent, "chiefrim: Chief's flashlight %s", on ? "on" : "off");
	chiefrim_flashlight.last = message;
	chiefrim_flashlight.sent = TRUE;
	chiefrim_flashlight.sent_ms = now;
}
