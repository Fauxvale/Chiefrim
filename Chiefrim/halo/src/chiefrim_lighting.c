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
its ambient. object_lights.c's hook calls this in place of the lightmap. */

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cseries/errors.h"
#include "objects/objects.h"
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
	chiefrim_rgb(&lighting->ambient_color, light->ambient, 1.f);

	key.i = light->key_direction.x;
	key.j = light->key_direction.y;
	key.k = light->key_direction.z;
	if (normalize3d(&key) == 0.f)
	{
		key.i = 0.f;
		key.j = 0.f;
		key.k = -1.f;
	}
	chiefrim_rgb(&lighting->distant_lights[0].color, light->key_color, 1.f);
	lighting->distant_lights[0].direction = key;

	/* the fill: the sky's light from above, unless a point light is stronger */
	chiefrim_rgb(&lighting->distant_lights[1].color, light->ambient_up, 1.f);
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

	/* reflections and the shadow, as build_distant_lights makes them from a lightmap */
	brightness = chiefrim_luminance(&lighting->ambient_color) + 0.5f * chiefrim_luminance(&lighting->distant_lights[0].color);
	lighting->reflection_tint_color.alpha = PIN(brightness * 1.5f + 0.25f, 0.f, 1.f);
	lighting->reflection_tint_color.red = PIN(lighting->ambient_color.red * 2.f + 0.25f, 0.f, 1.f);
	lighting->reflection_tint_color.green = PIN(lighting->ambient_color.green * 2.f + 0.25f, 0.f, 1.f);
	lighting->reflection_tint_color.blue = PIN(lighting->ambient_color.blue * 2.f + 0.25f, 0.f, 1.f);
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
