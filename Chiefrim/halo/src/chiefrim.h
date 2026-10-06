/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM.H

The Halo side of Chiefrim (Chiefrim/docs/DESIGN.md). Copied into the
decompilation's source/chiefrim/ by Chiefrim/tools/setup_halo.py, and called
from a few hooks marked CHIEFRIM in the game's own files.

Chiefrim mode is on when the environment variable CHIEFRIM is set to 1.
Without it, every function here does nothing and the game is unchanged.
*/

#ifndef __CHIEFRIM_H
#define __CHIEFRIM_H
#pragma once

#include "cseries.h"
#include "math/real_math.h"
#include "chiefrim/chiefrim_protocol.h"

boolean chiefrim_active(void);

/* main_loop, before its first frame */
void chiefrim_initialize(void);

/* main_loop, after the camera update of each frame */
void chiefrim_frame(void);

/* at exit (registered by chiefrim_initialize) */
void chiefrim_dispose(void);

/* render_cameras.c: the projection's tangent scale for Chief's field of view
from Skyrim (docs §6), instead of Halo's 0.85; 0 when Halo's own applies. */
real chiefrim_field_of_view_tangent_scale(void);

/* The link, for the other Chiefrim files: NULL when Chiefrim mode is off. */
struct cr_shared *chiefrim_shared(void);
/* Skyrim has said hello and keeps its heartbeat. */
boolean chiefrim_linked(void);
/* Skyrim moves the player and Chief follows (docs §7), rather than Halo moving Chief. */
boolean chiefrim_skyrim_drives(void);

/* Skyrim draws Halo's layers over its picture (docs §9): Halo draws only
Chief's arms and weapon and the HUD (render.c, render_objects.c), and
port/linux/src/chiefrim_overlay_gl.c sends them. */
boolean chiefrim_overlay_wanted(void);
int chiefrim_overlay_display(unsigned long *width, unsigned long *height, unsigned long *camera_frame);
struct observer_result;
/* main.c: the camera of local player 0's window, Skyrim's (lockstep) */
struct observer_result const *chiefrim_render_camera(short local_player_index, struct observer_result const *observer);
/* render.c: the layer being drawn (render_objects.c and the port follow it) */
#define CHIEFRIM_LAYER_ALL    0 /* not in overlay mode: Halo's whole view */
#define CHIEFRIM_LAYER_WORLD  1 /* projectiles, effects, decals, objects */
#define CHIEFRIM_LAYER_SCREEN 2 /* the first-person weapon, the HUD */
long chiefrim_overlay_layer(void);
void chiefrim_note_projection(real x0, real x1, real y0, real y1); /* render.c: the view's tangents */
void chiefrim_overlay_projection(float *tangent_x, float *tangent_y);
void chiefrim_set_render_layer(long layer);
void chiefrim_overlay_world_done(void); /* port: the world layer is drawn; on to the screen's */

/* chiefrim_input.c (docs §7): Chief's controls from Skyrim's actions.
input_abstraction.c (keyboard_controls_update) and player_control.c call
these for each local player's controller; they only answer for player 0. */
unsigned long chiefrim_input_keyboard_actions(short controller_index);
boolean chiefrim_input_movement(short controller_index, real *forward, real *strafe);
boolean chiefrim_input_look(short gamepad_index, real *yaw, real *pitch);
boolean chiefrim_input_driving(short gamepad_index);
boolean chiefrim_input_mark(void); /* the "mark stuck" hotkey, once per press */

/* chiefrim_combat.c (docs §8): proxies for Skyrim's people, damage both ways */
void chiefrim_combat_map_loaded(void);
void chiefrim_combat_reset(long chief);                /* a new world */
void chiefrim_combat_message(long chief, int type, void const *message, cr_vec3 origin);
void chiefrim_combat_update(long chief, cr_vec3 origin); /* each frame while linked */
boolean chiefrim_object_unseen(long object_index);     /* render_objects.c: a proxy */
void chiefrim_combat_forget(void);                     /* Halo's game state went back */
void chiefrim_combat_chief_lost(void);                 /* Chief's unit died or went */

/* chiefrim_world.c (docs §5): Skyrim's collision as Halo's. */
void chiefrim_world_initialize(void);
void chiefrim_world_map_loaded(void);             /* a structure BSP loaded */
void chiefrim_world_reset(cr_vec3 origin, real floor_z); /* a new world; floor in world units */
void chiefrim_world_generation(unsigned long generation); /* the world context in force */
void chiefrim_world_build_radius(unsigned long radius); /* regions around Chief's in a build */
void chiefrim_world_message(int type, void const *message);
void chiefrim_world_update(real_point3d const *chief); /* each frame; Chief or NULL */
boolean chiefrim_world_below_collision(real_point3d const *point); /* below all of Skyrim's collision loaded */
boolean chiefrim_world_has_skyrim_collision(void); /* not the stand-in floor */
boolean chiefrim_world_crossed_floor(real_point3d const *from, real_point3d const *to, real raise); /* went down through a floor */
boolean chiefrim_world_room_for(real_point3d const *feet, real height); /* nothing where his body would be */
boolean chiefrim_world_floor_beneath(real_point3d const *feet); /* a floor right under the feet */
boolean chiefrim_world_floor_within(real_point3d const *feet, real reach); /* a floor up to reach below */
boolean chiefrim_world_land_height(real_point3d const *point, real *z); /* the land under (or over) a point */
boolean chiefrim_world_step_ahead(real_point3d const *feet, real_vector3d const *direction, real radius,
	real max_step, real *top_z, boolean *overhang); /* a ledge ahead to step up onto; overhang: open below it */
/* Once after the first build since the origin moved: TRUE, with the ground's
height, if Chief is in the ground there (the stand-in floor was lower). */
boolean chiefrim_world_settle(real_point3d const *chief, real *ground_z);
void chiefrim_world_dump_installed(char const *why); /* the installed build's input, to build/collision-dumps */

/* scenario.c, right after a structure BSP becomes the global one. Replaces
the map's collision BSP with Chiefrim's (docs §5.1). */
void chiefrim_structure_bsp_loaded(void);

#endif /* __CHIEFRIM_H */
