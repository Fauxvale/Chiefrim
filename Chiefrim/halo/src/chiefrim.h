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

/* chiefrim_input.c (docs §7): Chief's controls from Skyrim's actions.
input_abstraction.c (keyboard_controls_update) and player_control.c call
these for each local player's controller; they only answer for player 0. */
unsigned long chiefrim_input_keyboard_actions(short controller_index);
boolean chiefrim_input_movement(short controller_index, real *forward, real *strafe);
boolean chiefrim_input_look(short gamepad_index, real *yaw, real *pitch);
boolean chiefrim_input_driving(short gamepad_index);

/* chiefrim_world.c (docs §5): Skyrim's collision as Halo's. */
void chiefrim_world_initialize(void);
void chiefrim_world_map_loaded(void);             /* a structure BSP loaded */
void chiefrim_world_reset(cr_vec3 origin, real floor_z); /* a new world; floor in world units */
void chiefrim_world_message(int type, void const *message);
void chiefrim_world_update(real_point3d const *chief); /* each frame; Chief or NULL */
boolean chiefrim_world_below_collision(real_point3d const *point); /* below all of Skyrim's collision loaded */
boolean chiefrim_world_has_skyrim_collision(void); /* not the stand-in floor */
boolean chiefrim_world_crossed_floor(real_point3d const *from, real_point3d const *to); /* feet went down through a floor */
boolean chiefrim_world_room_for(real_point3d const *feet, real height); /* nothing where his body would be */
/* Once after the first build since the origin moved: TRUE, with the ground's
height, if Chief is in the ground there (the stand-in floor was lower). */
boolean chiefrim_world_settle(real_point3d const *chief, real *ground_z);
void chiefrim_world_dump_installed(char const *why); /* the installed build's input, to build/collision-dumps */

/* scenario.c, right after a structure BSP becomes the global one. Replaces
the map's collision BSP with Chiefrim's (docs §5.1). */
void chiefrim_structure_bsp_loaded(void);

#endif /* __CHIEFRIM_H */
