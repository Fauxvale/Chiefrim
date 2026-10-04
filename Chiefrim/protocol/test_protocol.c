/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
TEST_PROTOCOL.C

Unit tests for chiefrim_protocol.h: coordinate mapping, the ring and the
seqlock slots. Built and run as both i386 and x86-64 by
tools/test_protocol.sh.
*/

#include "chiefrim_protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if (!(cond))                                                      \
		{                                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
			failures++;                                                   \
		}                                                                 \
	} while (0)

static int near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static float angle_diff(float a, float b)
{
	float d = cr_wrap_2pi(a - b);
	return d > CR_PI ? 2.0f * CR_PI - d : d;
}

static void test_coordinates(void)
{
	cr_vec3 origin = { 1000.0f, -2000.0f, 50.0f };
	cr_vec3 sky = { 1000.0f + 213.36f, -2000.0f - 2.0f * 213.36f, 50.0f + 0.5f * 213.36f };
	cr_vec3 halo = cr_sky_to_halo(sky, origin);
	cr_vec3 back = cr_halo_to_sky(halo, origin);

	CHECK(near(halo.x, 1.0f, 1e-5f));
	CHECK(near(halo.y, -2.0f, 1e-5f));
	CHECK(near(halo.z, 0.5f, 1e-5f));
	CHECK(near(back.x, sky.x, 1e-2f));
	CHECK(near(back.y, sky.y, 1e-2f));
	CHECK(near(back.z, sky.z, 1e-2f));

	/* Far corner of Tamriel stays sub-millimetre (docs §4). */
	{
		cr_vec3 zero = { 0, 0, 0 };
		cr_vec3 far_sky = { 250000.0f, -250000.0f, 30000.0f };
		cr_vec3 far_back = cr_halo_to_sky(cr_sky_to_halo(far_sky, zero), zero);
		CHECK(near(far_back.x, far_sky.x, 0.1f)); /* 0.1 unit = 1.4 mm */
		CHECK(near(far_back.y, far_sky.y, 0.1f));
	}

	/* Heading: Skyrim north (+Y, heading 0) is Halo yaw pi/2; Skyrim east
	(+X, heading pi/2) is Halo yaw 0. */
	CHECK(angle_diff(cr_sky_heading_to_halo_yaw(0.0f), CR_PI * 0.5f) < 1e-5f);
	CHECK(angle_diff(cr_sky_heading_to_halo_yaw(CR_PI * 0.5f), 0.0f) < 1e-5f);
	CHECK(angle_diff(cr_sky_heading_to_halo_yaw(CR_PI), CR_PI * 1.5f) < 1e-5f);
	{
		float h;
		for (h = 0.0f; h < 2.0f * CR_PI; h += 0.1f)
		{
			float yaw = cr_sky_heading_to_halo_yaw(h);
			/* The facing vectors agree: Skyrim (sin h, cos h), Halo (cos y, sin y). */
			CHECK(near(sinf(h), cosf(yaw), 1e-5f));
			CHECK(near(cosf(h), sinf(yaw), 1e-5f));
			CHECK(angle_diff(cr_halo_yaw_to_sky_heading(yaw), h) < 1e-5f);
		}
	}
}

static void test_slot(void)
{
	cr_slot_player_state slot;
	cr_player_state in, out;

	memset(&slot, 0, sizeof(slot));
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	CHECK(CR_SLOT_READ(&slot, &out) == 0); /* never written */

	in.tick = 42;
	in.position.x = 1.5f;
	CR_SLOT_WRITE(&slot, in);
	CHECK(CR_SLOT_READ(&slot, &out) == 2);
	CHECK(out.tick == 42 && out.position.x == 1.5f);

	slot.seq = 3; /* a writer is inside */
	CHECK(CR_SLOT_READ(&slot, &out) == 0);
}

static void test_ring(void)
{
	cr_ring *ring = calloc(1, sizeof(*ring));
	cr_msg_teleport teleport;
	unsigned char buffer[256];
	int pushed = 0, popped = 0, i;

	CHECK(cr_ring_pop(ring, buffer, sizeof(buffer)) == -1);

	memset(&teleport, 0, sizeof(teleport));
	teleport.position.x = 7.0f;
	CHECK(cr_ring_push(ring, CR_MSG_TELEPORT, &teleport, sizeof(teleport)));
	CHECK(cr_ring_pop(ring, buffer, sizeof(buffer)) == CR_MSG_TELEPORT);
	CHECK(((cr_msg_teleport *)buffer)->position.x == 7.0f);
	CHECK(((cr_msg_teleport *)buffer)->header.size == sizeof(teleport));

	/* Many rounds of logs (128 bytes) and teleports (24 bytes), pushed in
	bursts so the ring fills and wraps many times. */
	for (i = 0; i < 200000; i++)
	{
		int burst;
		for (burst = 0; burst < 3000; burst++)
		{
			int which = (pushed % 3) == 0;
			if (which)
			{
				cr_msg_log log;
				memset(&log, 0, sizeof(log));
				snprintf(log.text, sizeof(log.text), "%d", pushed);
				if (!cr_ring_push(ring, CR_MSG_LOG, &log, sizeof(log)))
					break;
			}
			else
			{
				teleport.position.x = (float)pushed;
				if (!cr_ring_push(ring, CR_MSG_TELEPORT, &teleport, sizeof(teleport)))
					break;
			}
			pushed++;
		}
		for (;;)
		{
			int type = cr_ring_pop(ring, buffer, sizeof(buffer));
			if (type < 0)
				break;
			if ((popped % 3) == 0)
			{
				CHECK(type == CR_MSG_LOG);
				CHECK(atoi(((cr_msg_log *)buffer)->text) == popped);
			}
			else
			{
				CHECK(type == CR_MSG_TELEPORT);
				CHECK(((cr_msg_teleport *)buffer)->position.x == (float)popped);
			}
			popped++;
			if (failures > 10)
				goto done;
		}
		if (pushed > 3000000)
			break;
	}
done:
	CHECK(pushed == popped);
	CHECK(pushed > 1000000); /* wrapped the 256 KiB ring many times */
	free(ring);
}

int main(void)
{
	test_coordinates();
	test_slot();
	test_ring();
	printf("%s: %d failure(s), %u-bit\n", failures ? "FAILED" : "ok", failures,
		(unsigned)(sizeof(void *) * 8));
	return failures != 0;
}
