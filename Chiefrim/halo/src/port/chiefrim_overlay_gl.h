/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_OVERLAY_GL.H

Chiefrim's overlay on the port's OpenGL device (Chiefrim/docs/DESIGN.md §9).
Copied into port/linux/src by Chiefrim/tools/setup_halo.py and called from
hooks marked CHIEFRIM in d3d8_gl.c and nv2a_psh.c.

Halo draws only Chief's arms and weapon and the HUD (render.c's hooks), and
Skyrim draws them over its own picture. For that it needs how much of each
pixel Halo covered, which the picture's alpha can't say (the game uses it
as scratch). So the back buffer's framebuffer gets a second colour target,
the coverage: every pixel shader also writes (1, 1, 1, its alpha) there,
and each draw's blend is turned into what that blend does to the coverage
(an opaque draw makes it 1, an alpha blend mixes it, an additive one leaves
it). A third target keeps the world layer's view depth: each pixel
shader writes 1 / gl_FragCoord.w (the clip w: the distance along the view)
where it shows anything, blended to the nearest. The world layer (render.c
draws it first) and the screen layer each become one premultiplied RGBA
image, read back at Present into the link's frame slots
(chiefrim_protocol.h).
*/

#ifndef __CHIEFRIM_OVERLAY_GL_H
#define __CHIEFRIM_OVERLAY_GL_H

/* CHIEFRIM=1: the pixel shaders write the coverage (fixed for the run) */
int chiefrim_overlay_enabled(void);

/* the width the game draws and the targets' scale, for Skyrim's screen;
FALSE while Skyrim asks for no overlay (then the port's own) */
int chiefrim_overlay_screen_mode(long *width, float scale[2]);

/* the framebuffer for the back buffer's textures with the coverage target,
or 0 when there is no overlay this frame (then the port's own) */
unsigned int chiefrim_overlay_framebuffer(unsigned int color, unsigned int depth,
	unsigned long width, unsigned long height);

/* each draw into that framebuffer, after the port's blend state: the
coverage target's. The factors are D3D's render state values (which are
GL's); adds: the blend equation is GL_FUNC_ADD; color_mask: the port's
bits (1 red, 2 green, 4 blue, 8 alpha). */
void chiefrim_overlay_draw_state(unsigned long blend_enable, unsigned long source, unsigned long destination,
	int adds, unsigned char color_mask);

/* after a clear of that framebuffer's colour: the depth target to "nothing" */
void chiefrim_overlay_cleared(void);

/* render.c, between the layers: keeps the world layer (picture, coverage,
depth) and clears the picture for the screen layer */
void chiefrim_overlay_world_done(void);

/* at Present, before the window blit: publishes both layers, waiting for
the GPU. TRUE if published: then Halo's own window is left alone (its
vsync would hold the lockstep up) */
int chiefrim_overlay_present(unsigned int color, unsigned long width, unsigned long height);

/* Halo's side (source/chiefrim/chiefrim.c): Skyrim's screen, if it wants the
overlay and the link is up, and the Skyrim camera this frame is drawn
through (0: Halo's own) */
int chiefrim_overlay_display(unsigned long *width, unsigned long *height, unsigned long *camera_frame);

#endif
