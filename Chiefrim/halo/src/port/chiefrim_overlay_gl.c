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
#define OVERLAY_READBACKS 2

struct cr_shared *chiefrim_shared(void);

/* entry points the port's list (gl.h) doesn't have */
static PFNGLBLENDFUNCIPROC overlay_glBlendFunci;
static PFNGLBLENDEQUATIONIPROC overlay_glBlendEquationi;
static PFNGLENABLEIPROC overlay_glEnablei;
static PFNGLDISABLEIPROC overlay_glDisablei;
static PFNGLCOLORMASKIPROC overlay_glColorMaski;
static PFNGLFENCESYNCPROC overlay_glFenceSync;
static PFNGLCLIENTWAITSYNCPROC overlay_glClientWaitSync;
static PFNGLDELETESYNCPROC overlay_glDeleteSync;
static PFNGLUNMAPBUFFERPROC overlay_glUnmapBuffer;

struct overlay_framebuffer
{
	GLuint color, depth, framebuffer;
};

struct overlay_readback
{
	GLuint buffer;
	GLsync fence;
	unsigned long width, height;
	unsigned long display_frame;
	unsigned long time_us;
};

static struct
{
	int checked, enabled;
	int loaded, failed;
	/* the coverage target, the size of the back buffer */
	GLuint coverage;
	unsigned long coverage_width, coverage_height;
	struct overlay_framebuffer framebuffers[OVERLAY_FRAMEBUFFERS];
	long framebuffer_count;
	/* the picture with the coverage as alpha (the resolve pass) */
	GLuint program, vertex_array, resolve, resolve_framebuffer;
	GLint picture_uniform, coverage_uniform;
	unsigned long resolve_width, resolve_height;
	struct overlay_readback readbacks[OVERLAY_READBACKS];
	long next_readback;
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
	OVERLAY_LOAD(glFenceSync)
	OVERLAY_LOAD(glClientWaitSync)
	OVERLAY_LOAD(glDeleteSync)
	OVERLAY_LOAD(glUnmapBuffer)
#undef OVERLAY_LOAD
	overlay.loaded = !overlay.failed;
	return overlay.loaded;
}

/* Skyrim's screen in the frame slots' bounds, keeping its shape */
static int overlay_picture_size(unsigned long *width, unsigned long *height, unsigned long *frame)
{
	unsigned long w, h;
	float fit = 1.0f;

	if (!chiefrim_overlay_enabled() || !chiefrim_overlay_display(&w, &h, frame) || w < 64 || h < 64)
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
}

unsigned int chiefrim_overlay_framebuffer(unsigned int color, unsigned int depth,
	unsigned long width, unsigned long height)
{
	static const GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	struct overlay_framebuffer *entry;
	unsigned long w, h, frame;
	long index;

	if (!color || !overlay_picture_size(&w, &h, &frame) || !overlay_load())
		return 0;
	if (!overlay.coverage || overlay.coverage_width != width || overlay.coverage_height != height)
	{
		overlay_forget_framebuffers();
		if (!overlay.coverage)
			glGenTextures(1, &overlay.coverage);
		glBindTexture(GL_TEXTURE_2D, overlay.coverage);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)width, (GLsizei)height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		overlay.coverage_width = width;
		overlay.coverage_height = height;
		xgpu_gl_state_invalidate();
		platform_log("chiefrim: overlay: %lux%lu (Skyrim's screen %lux%lu)", width, height, w, h);
	}
	for (index = 0; index < overlay.framebuffer_count; index++)
	{
		entry = &overlay.framebuffers[index];
		if (entry->color == color && entry->depth == depth)
			return entry->framebuffer;
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
	if (depth)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	glDrawBuffers(2, draw_buffers);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		platform_log("chiefrim: overlay: framebuffer %u/%u/%u is incomplete", color, overlay.coverage, depth);
	xgpu_gl_state_invalidate();
	return entry->framebuffer;
}

/* What a draw's blend does to how much of a pixel is covered. The colour
target ends up as Halo's picture over black, so the composite is
picture + Skyrim * (1 - coverage). With the destination kept by a factor f,
the coverage c becomes (1 - f) + f * c; the shader's coverage output is
(1, 1, 1, alpha), so that's red = 1 * s + c * d with: */
static void overlay_coverage_factors(GLenum source, GLenum destination, GLenum *s, GLenum *d)
{
	/* kept as it was: blends that only darken or tint what is there
	(modulation, which over black leaves black), or depend on it */
	*s = GL_ZERO;
	*d = GL_ONE;
	if (source == GL_ZERO || source == GL_DST_COLOR || source == GL_ONE_MINUS_DST_COLOR ||
		source == GL_DST_ALPHA || source == GL_ONE_MINUS_DST_ALPHA)
	{
		return;
	}
	switch (destination)
	{
	case GL_ZERO: /* f = 0: replaces */
		*s = GL_ONE;
		*d = GL_ZERO;
		break;
	case GL_ONE_MINUS_SRC_ALPHA: /* f = 1 - a */
		*s = GL_SRC_ALPHA;
		*d = GL_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_SRC_ALPHA: /* f = a */
		*s = GL_ONE_MINUS_SRC_ALPHA;
		*d = GL_SRC_ALPHA;
		break;
	case GL_CONSTANT_ALPHA:
		*s = GL_ONE_MINUS_CONSTANT_ALPHA;
		*d = GL_CONSTANT_ALPHA;
		break;
	case GL_ONE_MINUS_CONSTANT_ALPHA:
		*s = GL_CONSTANT_ALPHA;
		*d = GL_ONE_MINUS_CONSTANT_ALPHA;
		break;
	default: /* ONE (additive: light, not cover) and colour factors */
		break;
	}
}

void chiefrim_overlay_draw_state(unsigned long blend_enable, unsigned long source, unsigned long destination,
	int adds, unsigned char color_mask)
{
	GLenum s, d;

	/* the port's (unindexed) state set both targets; this sets the second */
	overlay_glColorMaski(1, (color_mask & 7) != 0, GL_FALSE, GL_FALSE, GL_FALSE);
	if (!blend_enable)
	{
		overlay_glDisablei(GL_BLEND, 1);
		return;
	}
	if (adds)
		overlay_coverage_factors((GLenum)source, (GLenum)destination, &s, &d);
	else
		s = GL_ZERO, d = GL_ONE;
	overlay_glEnablei(GL_BLEND, 1);
	overlay_glBlendEquationi(1, GL_FUNC_ADD);
	overlay_glBlendFunci(1, s, d);
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

static int overlay_resolve_ready(unsigned long width, unsigned long height)
{
	if (!overlay.program)
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
			"uniform sampler2D coverage;\n"
			"out vec4 result;\n"
			"void main()\n"
			"{\n"
			"	ivec2 pixel = ivec2(gl_FragCoord.xy);\n"
			"	result = vec4(texelFetch(picture, pixel, 0).rgb, texelFetch(coverage, pixel, 0).r);\n"
			"}\n";
		GLuint vs = overlay_compile(GL_VERTEX_SHADER, vertex);
		GLuint fs = overlay_compile(GL_FRAGMENT_SHADER, fragment);
		GLint ok = 0;

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
			return FALSE;
		}
		overlay.picture_uniform = glGetUniformLocation(overlay.program, "picture");
		overlay.coverage_uniform = glGetUniformLocation(overlay.program, "coverage");
		glGenVertexArrays(1, &overlay.vertex_array);
	}
	if (!overlay.resolve || overlay.resolve_width != width || overlay.resolve_height != height)
	{
		if (!overlay.resolve)
		{
			glGenTextures(1, &overlay.resolve);
			glGenFramebuffers(1, &overlay.resolve_framebuffer);
		}
		glBindTexture(GL_TEXTURE_2D, overlay.resolve);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindFramebuffer(GL_FRAMEBUFFER, overlay.resolve_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, overlay.resolve, 0);
		overlay.resolve_width = width;
		overlay.resolve_height = height;
	}
	return TRUE;
}

/* the readback a frame ago, into the next frame slot */
static void overlay_publish(struct overlay_readback *readback)
{
	struct cr_shared *shm = chiefrim_shared();
	unsigned long bytes = readback->width * readback->height * 4;
	const void *pixels;
	uint32_t slot;
	cr_frame_header *header;

	if (!readback->fence)
		return;
	overlay_glClientWaitSync(readback->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 50000000ull);
	overlay_glDeleteSync(readback->fence);
	readback->fence = 0;
	if (!shm || readback->width > CR_FRAME_MAX_WIDTH || readback->height > CR_FRAME_MAX_HEIGHT)
		return;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->buffer);
	pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	if (pixels)
	{
		uint32_t latest = CR_LOAD_ACQ(&shm->frames.latest);

		slot = latest ? latest % CR_FRAME_SLOTS : 0; /* the one after the latest */
		header = &shm->frames.slots[slot];
		cr_slot_write_begin(&header->seq);
		memcpy(shm->frames.pixels[slot], pixels, bytes);
		header->width = (uint32_t)readback->width;
		header->height = (uint32_t)readback->height;
		header->frame = (uint32_t)++overlay.frame;
		header->display_frame = (uint32_t)readback->display_frame;
		header->time_us = (uint32_t)readback->time_us;
		header->flags = CR_FRAME_VISIBLE;
		cr_slot_write_end(&header->seq);
		CR_STORE_REL(&shm->frames.latest, slot + 1);
		CR_STORE_REL(&shm->frames.published, shm->frames.published + 1);
		overlay_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void chiefrim_overlay_present(unsigned int color, unsigned long width, unsigned long height)
{
	struct overlay_readback *readback;
	unsigned long w, h, frame;
	GLint vertex_array = 0;

	if (!overlay_picture_size(&w, &h, &frame) || !overlay_load() || !overlay.coverage ||
		overlay.coverage_width != width || overlay.coverage_height != height ||
		width > CR_FRAME_MAX_WIDTH || height > CR_FRAME_MAX_HEIGHT || !overlay_resolve_ready(width, height))
	{
		/* (the targets follow Skyrim's screen a frame later: halo_screen_commit) */
		return;
	}
	if (!overlay.logged)
	{
		overlay.logged = TRUE;
		platform_log("chiefrim: overlay: publishing %lux%lu frames", width, height);
	}

	/* the picture with its coverage as alpha */
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
	glBindFramebuffer(GL_FRAMEBUFFER, overlay.resolve_framebuffer);
	glViewport(0, 0, (GLsizei)width, (GLsizei)height);
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

	/* read it back without waiting, and publish last frame's */
	readback = &overlay.readbacks[overlay.next_readback];
	overlay.next_readback = (overlay.next_readback + 1) % OVERLAY_READBACKS;
	overlay_publish(readback); /* (only if still pending: normally done a frame ago) */
	if (!readback->buffer)
		glGenBuffers(1, &readback->buffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->buffer);
	if (readback->width != width || readback->height != height)
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(width * height * 4), NULL, GL_STREAM_READ);
	readback->width = width;
	readback->height = height;
	readback->display_frame = frame;
	readback->time_us = overlay_now_us();
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	readback->fence = overlay_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	overlay_publish(&overlay.readbacks[overlay.next_readback]); /* last frame's */

	glBindVertexArray((GLuint)vertex_array); /* the port binds its own once, at start */
	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_state_invalidate();
}
