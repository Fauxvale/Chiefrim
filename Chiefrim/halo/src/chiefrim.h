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

boolean chiefrim_active(void);

/* main_loop, before its first frame */
void chiefrim_initialize(void);

/* main_loop, after the camera update of each frame */
void chiefrim_frame(void);

/* at exit (registered by chiefrim_initialize) */
void chiefrim_dispose(void);

/* scenario.c, right after a structure BSP becomes the global one. Replaces
the map's collision BSP with Chiefrim's (docs §5.1). */
void chiefrim_structure_bsp_loaded(void);

#endif /* __CHIEFRIM_H */
