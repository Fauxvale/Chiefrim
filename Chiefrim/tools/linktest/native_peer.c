/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
NATIVE_PEER.C

The Halo side of the link test (docs §10, §15): a native Linux i386 program,
like the Halo build. It creates /dev/shm/chiefrim_v1, echoes every TELEPORT
"ping" from the Wine peer back as a TELEPORT, publishes a player-state slot
whose fields are all derived from one counter (so a torn read shows), and
checks the Wine peer's input slot the same way. It prints the Wine peer's
final LOG report and its own counts.

Usage: native_peer [seconds]   (default 60; exits when the Wine peer closes)
*/

#define _GNU_SOURCE
#include "chiefrim_protocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 60;
	int fd = shm_open(CR_SHM_NAME, O_RDWR | O_CREAT | O_TRUNC, 0600);
	cr_shared *shm;
	uint32_t start, tick = 0, echoes = 0, input_reads = 0, input_torn = 0, last_frame = 0;
	int skyrim_seen = 0;
	unsigned char buffer[256];

	if (fd < 0 || ftruncate(fd, sizeof(cr_shared)) != 0)
	{
		perror("shm");
		return 1;
	}
	shm = mmap(NULL, sizeof(cr_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}
	memset(shm, 0, sizeof(*shm));
	shm->version = CR_PROTOCOL_VERSION;
	shm->total_size = sizeof(cr_shared);
	shm->halo_pid = (uint32_t)getpid();
	CR_STORE_REL(&shm->halo_state, CR_SIDE_READY);
	CR_STORE_REL(&shm->magic, CR_MAGIC); /* last: the mapping is valid */
	printf("native: %s ready (%u bytes, %u-bit), waiting for the Wine peer\n",
		CR_SHM_LINUX_PATH, (unsigned)sizeof(cr_shared), (unsigned)(sizeof(void *) * 8));
	fflush(stdout);

	start = now_ms();
	while (now_ms() - start < (uint32_t)seconds * 1000u)
	{
		int type;
		cr_player_state state;
		cr_input input;

		CR_STORE_REL(&shm->halo_heartbeat, now_ms());

		/* Player-state slot: every field derived from tick. */
		memset(&state, 0, sizeof(state));
		state.tick = ++tick;
		state.position.x = (float)(tick % 100000u);
		state.position.y = -(float)(tick % 100000u);
		state.reserved[0] = tick * 2654435761u;
		state.reserved[1] = ~tick;
		CR_SLOT_WRITE(&shm->player_state, state);

		/* Input slot: presses[i] = frame * 31 + i, held = ~frame. */
		if (CR_SLOT_READ(&shm->input, &input))
		{
			int i, ok = 1;
			for (i = 0; i < (int)CR_ACTION_SLOTS; i++)
				ok &= input.presses[i] == (uint8_t)(input.frame * 31u + (uint32_t)i);
			ok &= input.held == ~input.frame;
			ok &= input.yaw_total == (double)input.frame * 0.5;
			input_reads++;
			if (!ok || input.frame < last_frame)
				input_torn++;
			last_frame = input.frame;
		}

		while ((type = cr_ring_pop(&shm->to_halo, buffer, sizeof(buffer))) >= 0)
		{
			if (!skyrim_seen)
			{
				printf("native: Wine peer connected (pid %u)\n", CR_LOAD_ACQ(&shm->skyrim_pid));
				fflush(stdout);
				skyrim_seen = 1;
			}
			if (type == CR_MSG_TELEPORT)
			{
				while (!cr_ring_push(&shm->to_skyrim, CR_MSG_TELEPORT, buffer, sizeof(cr_msg_teleport)))
					;
				echoes++;
			}
			else if (type == CR_MSG_LOG)
			{
				printf("wine:   %s\n", ((cr_msg_log *)buffer)->text);
				fflush(stdout);
			}
		}

		if (skyrim_seen && CR_LOAD_ACQ(&shm->skyrim_state) == CR_SIDE_CLOSING)
			break;
	}

	printf("native: echoed %u pings; read input slot %u times, %u torn or out of order\n",
		echoes, input_reads, input_torn);
	CR_STORE_REL(&shm->halo_state, CR_SIDE_CLOSING);
	munmap(shm, sizeof(cr_shared));
	shm_unlink(CR_SHM_NAME);
	return skyrim_seen && input_torn == 0 ? 0 : 1;
}
