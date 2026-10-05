/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The few game functions chiefrim_bsp.c reaches, for bsp_harness outside the
game (no game headers: plain C library). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* global_projection3d_mappings: from the game's real_math.o, linked in */

void *csmemcpy(void *destination, void const *source, unsigned long size) { return memcpy(destination, source, size); }
void *csmemset(void *buffer, long c, unsigned long size) { return memset(buffer, (int)c, size); }

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = vprintf(format, arguments);
	va_end(arguments);
	return result;
}

/* Halo's collision query code (collision_bsp.c), linked into the harness:
the calls it makes besides the queries themselves. Callers pass arguments
the cdecl way; these ignore them. */
void collision_log_usage(int function) { (void)function; }
void collision_log_start_time(void *times) { (void)times; }
void collision_log_end_time(int function, long long start) { (void)function; (void)start; }
unsigned int _control87(unsigned int value, unsigned int mask) { (void)value; (void)mask; return 0; }

char *csprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsprintf(buffer, format, arguments);
	va_end(arguments);
	return buffer;
}

/* Tag block access, strict: any index the BSP gets wrong stops the harness. */
#include <stdlib.h>
struct harness_tag_block { long count; void *address; void *definition; };
void *tag_block_get_element_with_size(const struct harness_tag_block *block, long index, long element_size)
{
	if (!block || !block->address || index < 0 || index >= block->count)
	{
		printf("BAD TAG BLOCK INDEX %ld of %ld\n", index, block ? block->count : -1L);
		abort();
	}
	return (char *)block->address + index * element_size;
}

void display_assert(const char *expression, const char *file, long line, int fatal)
{
	printf("ASSERT %s at %s:%ld\n", expression ? expression : "", file ? file : "", line);
	(void)fatal;
}

void system_exit(int code) { exit(code ? 2 : 0); }
void *global_collision_bsp;      /* compared against, never used here */
float global_real_argb_green[4]; /* debug drawing only */
