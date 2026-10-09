/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_OVERLAY_GL.C

Chiefrim's overlay on the port's OpenGL device: see chiefrim_overlay_gl.h
and Chiefrim/docs/DESIGN.md §9.
*/

#include "xgpu.h"
#include "chiefrim_overlay_gl.h"
#include "../../../source/chiefrim/chiefrim_protocol.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the port's screen (d3d8_gl.c) */
#define OVERLAY_SCREEN_HEIGHT 480
#define OVERLAY_SCREEN_MAXIMUM_WIDTH 1920
#define OVERLAY_FRAMEBUFFERS 4
/* chiefrim.h's CHIEFRIM_LAYER_* */
#define OVERLAY_LAYER_WORLD 1
/* the depth target where nothing was drawn */
#define OVERLAY_FAR 1.0e30f

struct cr_shared *chiefrim_shared(void);
long chiefrim_overlay_layer(void);
long chiefrim_overlay_hud(void);
void chiefrim_overlay_projection(float *tangent_x, float *tangent_y);

/* entry points the port's list (gl.h) doesn't have */
static PFNGLBLENDFUNCIPROC overlay_glBlendFunci;
static PFNGLBLENDEQUATIONIPROC overlay_glBlendEquationi;
static PFNGLENABLEIPROC overlay_glEnablei;
static PFNGLDISABLEIPROC overlay_glDisablei;
static PFNGLCOLORMASKIPROC overlay_glColorMaski;
static void (*overlay_glReadBuffer)(GLenum mode); /* GL 1.0: no PFN type */
static PFNGLCLEARBUFFERFVPROC overlay_glClearBufferfv;

struct overlay_framebuffer
{
	GLuint color, depth, framebuffer;
};

struct overlay_resolve
{
	GLuint texture, framebuffer;
	unsigned long width, height;
};

static struct
{
	int checked, enabled;
	int loaded, failed;
	/* the targets beside the back buffer's colour: how much of Skyrim's
	picture shows through each pixel (R8, the transmittance), and the world
	layer's view depth (R32F, min-blended), and in the screen layer how much
	of each pixel's colour is the weapon's, not the HUD's (R8: CR_FRAME_MASK) */
	GLuint coverage, depth, weapon;
	unsigned long width, height;
	struct overlay_framebuffer framebuffers[OVERLAY_FRAMEBUFFERS];
	long framebuffer_count;
	GLuint bound; /* the framebuffer the back buffer was last drawn through */
	GLuint bound_color;
	/* the picture with the coverage as alpha (the resolve pass) */
	GLuint program, vertex_array;
	GLint picture_uniform, coverage_uniform;
	struct overlay_resolve world, screen;
	long world_draws;  /* draws into the world layer this frame */
	int world_drawn;   /* the world layer of this frame has something */
	unsigned long frame;
	int logged;
} overlay;

static unsigned long overlay_now_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long)((unsigned long long)now.tv_sec * 1000000ull + (unsigned long long)now.tv_nsec / 1000ull);
}

int chiefrim_overlay_enabled(void)
{
	if (!overlay.checked)
	{
		const char *flag = getenv("CHIEFRIM");

		overlay.checked = TRUE;
		overlay.enabled = flag && !strcmp(flag, "1");
	}
	return overlay.enabled;
}

static int overlay_load(void)
{
	if (overlay.loaded || overlay.failed)
		return overlay.loaded;
#define OVERLAY_LOAD(name) \
	overlay_##name = (__typeof__(overlay_##name))SDL_GL_GetProcAddress(#name); \
	if (!overlay_##name) \
	{ \
		platform_log("chiefrim: overlay: OpenGL function %s is unavailable", #name); \
		overlay.failed = TRUE; \
	}
	OVERLAY_LOAD(glBlendFunci)
	OVERLAY_LOAD(glBlendEquationi)
	OVERLAY_LOAD(glEnablei)
	OVERLAY_LOAD(glDisablei)
	OVERLAY_LOAD(glColorMaski)
	OVERLAY_LOAD(glReadBuffer)
	OVERLAY_LOAD(glClearBufferfv)
#undef OVERLAY_LOAD
	overlay.loaded = !overlay.failed;
	return overlay.loaded;
}

/* Skyrim's screen in the frame slots' bounds, keeping its shape */
static int overlay_picture_size(unsigned long *width, unsigned long *height, unsigned long *camera_frame)
{
	unsigned long w, h;
	float fit = 1.0f;

	if (!chiefrim_overlay_enabled() || !chiefrim_overlay_display(&w, &h, camera_frame) || w < 64 || h < 64)
		return FALSE;
	if ((float)w * fit > (float)CR_FRAME_MAX_WIDTH)
		fit = (float)CR_FRAME_MAX_WIDTH / (float)w;
	if ((float)h * fit > (float)CR_FRAME_MAX_HEIGHT)
		fit = (float)CR_FRAME_MAX_HEIGHT / (float)h;
	*width = (unsigned long)((float)w * fit + 0.5f);
	*height = (unsigned long)((float)h * fit + 0.5f);
	if (*width > CR_FRAME_MAX_WIDTH)
		*width = CR_FRAME_MAX_WIDTH;
	if (*height > CR_FRAME_MAX_HEIGHT)
		*height = CR_FRAME_MAX_HEIGHT;
	return TRUE;
}

int chiefrim_overlay_screen_mode(long *width, float scale[2])
{
	unsigned long w, h, frame;
	long columns;

	if (!overlay_picture_size(&w, &h, &frame))
		return FALSE;
	/* the game's 480 lines, as many columns as Skyrim's shape gives; the
	render targets get the picture's pixels */
	columns = (long)((OVERLAY_SCREEN_HEIGHT * w + h / 2) / h) & ~1L;
	if (columns < 640)
		columns = 640;
	if (columns > OVERLAY_SCREEN_MAXIMUM_WIDTH)
		columns = OVERLAY_SCREEN_MAXIMUM_WIDTH;
	*width = columns;
	scale[0] = (float)w / (float)columns;
	scale[1] = (float)h / (float)OVERLAY_SCREEN_HEIGHT;
	return TRUE;
}

static void overlay_forget_framebuffers(void)
{
	long index;

	for (index = 0; index < overlay.framebuffer_count; index++)
		glDeleteFramebuffers(1, &overlay.framebuffers[index].framebuffer);
	overlay.framebuffer_count = 0;
	overlay.bound = 0;
}

static void overlay_target(GLuint *texture, GLint format, GLenum layout, GLenum type, unsigned long width, unsigned long height)
{
	if (!*texture)
		glGenTextures(1, texture);
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, format, (GLsizei)width, (GLsizei)height, 0, layout, type, NULL);
}

unsigned int chiefrim_overlay_framebuffer(unsigned int color, unsigned int depth,
	unsigned long width, unsigned long height)
{
	static const GLenum draw_buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	struct overlay_framebuffer *entry;
	unsigned long w, h, frame;
	long index;

	if (!color || !overlay_picture_size(&w, &h, &frame) || !overlay_load())
		return 0;
	if (!overlay.coverage || overlay.width != width || overlay.height != height)
	{
		overlay_forget_framebuffers();
		overlay_target(&overlay.coverage, GL_R8, GL_RED, GL_UNSIGNED_BYTE, width, height);
		overlay_target(&overlay.depth, GL_R32F, GL_RED, GL_FLOAT, width, height);
		overlay_target(&overlay.weapon, GL_R8, GL_RED, GL_UNSIGNED_BYTE, width, height);
		overlay.width = width;
		overlay.height = height;
		xgpu_gl_state_invalidate();
		platform_log("chiefrim: overlay: %lux%lu (Skyrim's screen %lux%lu)", width, height, w, h);
	}
	for (index = 0; index < overlay.framebuffer_count; index++)
	{
		entry = &overlay.framebuffers[index];
		if (entry->color == color && entry->depth == depth)
		{
			overlay.bound = entry->framebuffer;
			overlay.bound_color = color;
			return entry->framebuffer;
		}
	}
	if (overlay.framebuffer_count == OVERLAY_FRAMEBUFFERS)
		overlay_forget_framebuffers();
	entry = &overlay.framebuffers[overlay.framebuffer_count++];
	entry->color = color;
	entry->depth = depth;
	glGenFramebuffers(1, &entry->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, entry->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, overlay.coverage, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, overlay.depth, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, overlay.weapon, 0);
	if (depth)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	glDrawBuffers(4, draw_buffers);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		platform_log("chiefrim: overlay: framebuffer %u/%u/%u/%u/%u is incomplete", color, overlay.coverage, overlay.depth,
			overlay.weapon, depth);
	xgpu_gl_state_invalidate();
	overlay.bound = entry->framebuffer;
	overlay.bound_color = color;
	return entry->framebuffer;
}

/* What a draw's blend does to how much of Skyrim's picture shows through
a pixel: its transmittance T. The colour target ends up as Halo's picture
over black, C, so the composite is C + Skyrim * T, sent as alpha = 1 - T
(premultiplied). A blend out = S * src + D * dst turns T into D * T, plus
src * T where S is the destination's colour (a modulation multiplies what
is under it, Skyrim's picture included: bullet holes). The shader writes
the transmittance target (luminance(src), 0, 0, alpha(src)), so its blend
is (S == DST_COLOR ? DST_COLOR : ZERO, D); factors of the game's
destination alpha, which the target hasn't, keep T. */
static void overlay_transmittance_factors(int blend_enable, GLenum source, GLenum destination, GLenum *s, GLenum *d)
{
	if (!blend_enable)
	{
		*s = GL_ZERO; /* replaces: Skyrim's picture is hidden */
		*d = GL_ZERO;
		return;
	}
	*s = source == GL_DST_COLOR ? GL_DST_COLOR : GL_ZERO;
	switch (destination)
	{
	case GL_ZERO:
	case GL_ONE:
	case GL_SRC_COLOR:
	case GL_ONE_MINUS_SRC_COLOR:
	case GL_SRC_ALPHA:
	case GL_ONE_MINUS_SRC_ALPHA:
	case GL_CONSTANT_COLOR:
	case GL_ONE_MINUS_CONSTANT_COLOR:
	case GL_CONSTANT_ALPHA:
	case GL_ONE_MINUS_CONSTANT_ALPHA:
		*d = destination;
		break;
	default: /* the destination's alpha (the game's scratch), saturate */
		*s = GL_ZERO;
		*d = GL_ONE;
		break;
	}
}

void chiefrim_overlay_draw_state(unsigned long blend_enable, unsigned long source, unsigned long destination,
	int adds, unsigned char color_mask)
{
	GLenum s, d;
	int world = chiefrim_overlay_layer() == OVERLAY_LAYER_WORLD;

	/* the port's (unindexed) state set every target; this sets the others */
	overlay_glColorMaski(1, (color_mask & 7) != 0, GL_FALSE, GL_FALSE, GL_FALSE);
	/* the world layer's depth: the nearest of what each draw shows */
	overlay_glColorMaski(2, world && (color_mask & 7) != 0, GL_FALSE, GL_FALSE, GL_FALSE);
	if (world && (color_mask & 7))
	{
		overlay.world_draws++;
		overlay_glEnablei(GL_BLEND, 2);
		overlay_glBlendEquationi(2, GL_MIN);
		overlay_glBlendFunci(2, GL_ONE, GL_ONE);
	}
	if (adds || !blend_enable)
		overlay_transmittance_factors(blend_enable != 0, (GLenum)source, (GLenum)destination, &s, &d);
	else
		s = GL_ZERO, d = GL_ONE; /* subtract, min, max: kept */
	overlay_glEnablei(GL_BLEND, 1);
	overlay_glBlendEquationi(1, GL_FUNC_ADD);
	overlay_glBlendFunci(1, s, d);

	/* the weapon's share of the screen layer's colour: the shader writes
	(1, 0, 0, alpha), and this target blends as the colour does, its source
	counted only for the weapon's draws (the HUD's: zero), so where the HUD
	covers the weapon its share goes; a draw that subtracts or keeps a
	min or max keeps it. Its blend is always on: a draw that replaces is
	the weapon's (1) or the HUD's (0). */
	overlay_glColorMaski(3, !world && (color_mask & 7) != 0, GL_FALSE, GL_FALSE, GL_FALSE);
	if (!blend_enable)
		s = chiefrim_overlay_hud() ? GL_ZERO : GL_ONE, d = GL_ZERO;
	else if (!adds)
		s = GL_ZERO, d = GL_ONE;
	else
		s = chiefrim_overlay_hud() ? GL_ZERO : (GLenum)source, d = (GLenum)destination;
	overlay_glEnablei(GL_BLEND, 3);
	overlay_glBlendEquationi(3, GL_FUNC_ADD);
	overlay_glBlendFunci(3, s, d);
}

void chiefrim_overlay_cleared(void)
{
	static const GLfloat far[4] = { OVERLAY_FAR, OVERLAY_FAR, OVERLAY_FAR, OVERLAY_FAR };
	static const GLfloat through[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	if (!overlay.loaded)
		return;
	overlay_glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glClearBufferfv(GL_COLOR, 1, through); /* all of Skyrim shows through */
	overlay_glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glClearBufferfv(GL_COLOR, 2, far);
	overlay_glColorMaski(3, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glClearBufferfv(GL_COLOR, 3, none);
	xgpu_gl_state_invalidate();
}

static GLuint overlay_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("chiefrim: overlay: shader: %s", log);
	}
	return shader;
}

static int overlay_program_ready(void)
{
	static const char vertex[] =
		"#version 330 core\n"
		"void main()\n"
		"{\n"
		"	vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char fragment[] =
		"#version 330 core\n"
		"uniform sampler2D picture;\n"
		"uniform sampler2D transmittance;\n"
		"out vec4 result;\n"
		"void main()\n"
		"{\n"
		"	ivec2 pixel = ivec2(gl_FragCoord.xy);\n"
		"	result = vec4(texelFetch(picture, pixel, 0).rgb, 1.0 - texelFetch(transmittance, pixel, 0).r);\n"
		"}\n";
	GLuint vs, fs;
	GLint ok = 0;

	if (overlay.program)
		return TRUE;
	vs = overlay_compile(GL_VERTEX_SHADER, vertex);
	fs = overlay_compile(GL_FRAGMENT_SHADER, fragment);
	overlay.program = glCreateProgram();
	glAttachShader(overlay.program, vs);
	glAttachShader(overlay.program, fs);
	glLinkProgram(overlay.program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(overlay.program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		platform_log("chiefrim: overlay: the resolve program doesn't link");
		overlay.failed = TRUE;
		overlay.loaded = FALSE;
		return FALSE;
	}
	overlay.picture_uniform = glGetUniformLocation(overlay.program, "picture");
	overlay.coverage_uniform = glGetUniformLocation(overlay.program, "transmittance");
	glGenVertexArrays(1, &overlay.vertex_array);
	return TRUE;
}

/* the back buffer's picture with its coverage as alpha, into a resolve */
static void overlay_resolve(struct overlay_resolve *resolve, GLuint color)
{
	GLint vertex_array = 0;

	if (!resolve->texture || resolve->width != overlay.width || resolve->height != overlay.height)
	{
		overlay_target(&resolve->texture, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, overlay.width, overlay.height);
		if (!resolve->framebuffer)
			glGenFramebuffers(1, &resolve->framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, resolve->framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve->texture, 0);
		resolve->width = overlay.width;
		resolve->height = overlay.height;
	}
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
	glBindFramebuffer(GL_FRAMEBUFFER, resolve->framebuffer);
	glViewport(0, 0, (GLsizei)overlay.width, (GLsizei)overlay.height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(overlay.program);
	glBindVertexArray(overlay.vertex_array);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, color);
	glBindSampler(0, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, overlay.coverage);
	glBindSampler(1, 0);
	glUniform1i(overlay.picture_uniform, 0);
	glUniform1i(overlay.coverage_uniform, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray((GLuint)vertex_array); /* the port binds its own once, at start */
	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_state_invalidate();
}

void chiefrim_overlay_world_done(void)
{
	static const GLfloat clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const GLfloat through[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	overlay.world_drawn = FALSE;
	if (!overlay.loaded || !overlay.bound || !overlay_program_ready())
		return;
	overlay.world_drawn = overlay.world_draws > 0;
	overlay.world_draws = 0;
	if (overlay.world_drawn)
		overlay_resolve(&overlay.world, overlay.bound_color);
	/* the screen layer starts on nothing (the depth target keeps the
	world's, for Present); Halo's depth too, as at a frame's start */
	glBindFramebuffer(GL_FRAMEBUFFER, overlay.bound);
	glDisable(GL_SCISSOR_TEST);
	overlay_glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glClearBufferfv(GL_COLOR, 0, clear);
	overlay_glClearBufferfv(GL_COLOR, 1, through); /* all of Skyrim shows through */
	overlay_glColorMaski(3, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	overlay_glClearBufferfv(GL_COLOR, 3, clear); /* none of it the weapon's yet */
	glDepthMask(GL_TRUE);
	glStencilMask(0xff);
	glClearDepth(1.0);
	glClearStencil(0);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	xgpu_gl_state_invalidate();
}

static void overlay_read(GLuint framebuffer, GLenum attachment, GLenum layout, GLenum type, void *pixels)
{
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	overlay_glReadBuffer(attachment);
	glReadPixels(0, 0, (GLsizei)overlay.width, (GLsizei)overlay.height, layout, type, pixels);
}

int chiefrim_overlay_present(unsigned int color, unsigned long width, unsigned long height)
{
	struct cr_shared *shm = chiefrim_shared();
	unsigned long w, h, camera_frame;
	uint32_t latest, slot;
	cr_frame_header *header;
	uint8_t *pixels;
	int world = overlay.world_drawn;

	overlay.world_drawn = FALSE;
	overlay.world_draws = 0;
	if (!shm || !overlay_picture_size(&w, &h, &camera_frame) || !overlay.loaded || !overlay.coverage ||
		overlay.width != width || overlay.height != height ||
		width > CR_FRAME_MAX_WIDTH || height > CR_FRAME_MAX_HEIGHT || !overlay_program_ready())
	{
		/* (the targets follow Skyrim's screen a frame later: halo_screen_commit) */
		return FALSE;
	}
	if (!overlay.logged)
	{
		overlay.logged = TRUE;
		platform_log("chiefrim: overlay: publishing %lux%lu frames", width, height);
	}
	overlay_resolve(&overlay.screen, color);

	/* Read straight into the next slot, waiting for the GPU: Skyrim waits
	for this frame (lockstep), so there's no use for it a frame later. */
	latest = CR_LOAD_ACQ(&shm->frames.latest);
	slot = latest ? latest % CR_FRAME_SLOTS : 0; /* the one after the latest */
	header = &shm->frames.slots[slot];
	pixels = shm->frames.pixels[slot];
	cr_slot_write_begin(&header->seq);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	overlay_read(overlay.screen.framebuffer, GL_COLOR_ATTACHMENT0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	if (world && overlay.bound)
	{
		overlay_read(overlay.world.framebuffer, GL_COLOR_ATTACHMENT0, GL_RGBA, GL_UNSIGNED_BYTE,
			pixels + CR_FRAME_LAYER_BYTES);
		overlay_read(overlay.bound, GL_COLOR_ATTACHMENT2, GL_RED, GL_FLOAT, pixels + 2 * CR_FRAME_LAYER_BYTES);
	}
	if (overlay.bound)
	{
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		overlay_read(overlay.bound, GL_COLOR_ATTACHMENT3, GL_RED, GL_UNSIGNED_BYTE, pixels + 3 * CR_FRAME_LAYER_BYTES);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
	}
	header->width = (uint32_t)width;
	header->height = (uint32_t)height;
	header->frame = (uint32_t)++overlay.frame;
	header->camera_frame = (uint32_t)camera_frame;
	header->time_us = (uint32_t)overlay_now_us();
	header->flags = CR_FRAME_VISIBLE | (world ? CR_FRAME_WORLD : 0u) | (overlay.bound ? CR_FRAME_MASK : 0u);
	chiefrim_overlay_projection(&header->tangent_x, &header->tangent_y);
	cr_slot_write_end(&header->seq);
	CR_STORE_REL(&shm->frames.latest, slot + 1);
	CR_STORE_REL(&shm->frames.published, shm->frames.published + 1);
	overlay_glReadBuffer(GL_COLOR_ATTACHMENT0);
	xgpu_gl_state_invalidate();
	return TRUE;
}
