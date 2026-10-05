/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
WINE_PEER.C

The Skyrim side of the link test: a Windows x64 program run under Proton,
opening the mapping the way the SKSE plugin will (docs §10). Built without
the Windows SDK or a C runtime (tools/linktest/build.sh), so it declares the
few kernel32 functions it needs.

It sends 100000 TELEPORT pings, each carrying its index, and waits for each
echo; it times the round trips; it publishes an input slot and checks the
native peer's player-state slot for torn reads. The results go back to the
native peer as LOG messages, which it prints.
*/

#include "chiefrim_protocol.h"

#include <stddef.h> /* wchar_t */

typedef void *HANDLE;
typedef int BOOL;
typedef unsigned long DWORD;
typedef long long LONGLONG;

#define GENERIC_READ             0x80000000ul
#define GENERIC_WRITE            0x40000000ul
#define FILE_SHARE_READ          0x1ul
#define FILE_SHARE_WRITE         0x2ul
#define OPEN_EXISTING            3ul
#define FILE_ATTRIBUTE_NORMAL    0x80ul
#define PAGE_READWRITE           0x4ul
#define FILE_MAP_ALL_ACCESS      0xF001Ful
#define INVALID_HANDLE_VALUE     ((HANDLE)(long long)-1)

__declspec(dllimport) HANDLE __stdcall CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) HANDLE __stdcall CreateFileMappingW(HANDLE, void *, DWORD, DWORD, DWORD, const wchar_t *);
__declspec(dllimport) void *__stdcall MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, unsigned long long);
__declspec(dllimport) BOOL __stdcall UnmapViewOfFile(const void *);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessId(void);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
__declspec(dllimport) BOOL __stdcall QueryPerformanceCounter(LONGLONG *);
__declspec(dllimport) BOOL __stdcall QueryPerformanceFrequency(LONGLONG *);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);

/* clang may emit calls to these even with -ffreestanding. */
void *memset(void *dst, int c, unsigned long long n)
{
	unsigned char *d = dst;
	while (n--) *d++ = (unsigned char)c;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned long long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}

int _fltused;

/* A tiny formatter: appends text and unsigned numbers to a LOG message. */
typedef struct line
{
	cr_msg_log msg;
	unsigned length;
} line;

static void put_text(line *l, const char *text)
{
	while (*text && l->length + 1 < sizeof(l->msg.text))
		l->msg.text[l->length++] = *text++;
	l->msg.text[l->length] = 0;
}

static void put_u64(line *l, unsigned long long v)
{
	char digits[24];
	int n = 0;
	do
	{
		digits[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n--)
	{
		char one[2] = { digits[n], 0 };
		put_text(l, one);
	}
}

static void send_line(cr_shared *shm, line *l)
{
	while (!cr_ring_push(&shm->to_halo, CR_MSG_LOG, &l->msg, sizeof(l->msg)))
		;
	memset(l, 0, sizeof(*l));
}

#define PINGS 100000u

void mainCRTStartup(void)
{
	HANDLE file, mapping;
	cr_shared *shm;
	LONGLONG frequency, t0, t1, total_start, total_end;
	unsigned long long min_ns = ~0ull, max_ns = 0, sum_ns = 0;
	unsigned buckets[4] = { 0, 0, 0, 0 }; /* <10us, <100us, <1ms, >=1ms */
	unsigned i, state_reads = 0, state_torn = 0, last_tick = 0, bad_echo = 0;
	unsigned char buffer[256];
	line l;

	memset(&l, 0, sizeof(l));
	file = CreateFileW(CR_SHM_WINE_PATH, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE)
		ExitProcess(2);
	mapping = CreateFileMappingW(file, 0, PAGE_READWRITE, 0, sizeof(cr_shared), 0);
	if (!mapping)
		ExitProcess(3);
	shm = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cr_shared));
	if (!shm)
		ExitProcess(4);
	if (CR_LOAD_ACQ(&shm->magic) != CR_MAGIC || shm->version != CR_PROTOCOL_VERSION ||
		shm->total_size != sizeof(cr_shared))
		ExitProcess(5);

	shm->skyrim_pid = GetCurrentProcessId();
	CR_STORE_REL(&shm->skyrim_state, CR_SIDE_READY);
	QueryPerformanceFrequency(&frequency);

	put_text(&l, "mapped Z:\\dev\\shm\\chiefrim_v1, native pid ");
	put_u64(&l, shm->halo_pid);
	put_text(&l, ", native state ");
	put_u64(&l, CR_LOAD_ACQ(&shm->halo_state));
	send_line(shm, &l);

	QueryPerformanceCounter(&total_start);
	for (i = 1; i <= PINGS; i++)
	{
		cr_msg_teleport ping;
		cr_input input;
		cr_player_state state;
		unsigned long long ns;
		int k, type;

		/* Input slot, checked by the native side. */
		memset(&input, 0, sizeof(input));
		input.frame = i;
		for (k = 0; k < (int)CR_ACTION_SLOTS; k++)
			input.presses[k] = (uint8_t)(i * 31u + (unsigned)k);
		input.held = ~i;
		input.yaw_total = (double)i * 0.5;
		CR_SLOT_WRITE(&shm->input, input);
		CR_STORE_REL(&shm->skyrim_heartbeat, (uint32_t)GetTickCount64());

		memset(&ping, 0, sizeof(ping));
		ping.position.x = (float)i;
		ping.header.reserved = i;
		QueryPerformanceCounter(&t0);
		while (!cr_ring_push(&shm->to_halo, CR_MSG_TELEPORT, &ping, sizeof(ping)))
			;
		for (;;)
		{
			type = cr_ring_pop(&shm->to_skyrim, buffer, sizeof(buffer));
			if (type >= 0)
				break;
			QueryPerformanceCounter(&t1);
			if ((t1 - t0) > frequency * 5) /* 5 s: the native side is gone */
			{
				put_text(&l, "timed out waiting for echo ");
				put_u64(&l, i);
				send_line(shm, &l);
				total_end = t1;
				goto report;
			}
		}
		QueryPerformanceCounter(&t1);
		if (type != CR_MSG_TELEPORT || ((cr_msg_teleport *)buffer)->header.reserved != i)
			bad_echo++;

		ns = (unsigned long long)(t1 - t0) * 1000000000ull / (unsigned long long)frequency;
		sum_ns += ns;
		if (ns < min_ns) min_ns = ns;
		if (ns > max_ns) max_ns = ns;
		buckets[ns < 10000 ? 0 : ns < 100000 ? 1 : ns < 1000000 ? 2 : 3]++;

		/* Player-state slot, every field derived from tick. */
		if (CR_SLOT_READ(&shm->player_state, &state))
		{
			unsigned t = state.tick;
			int ok = state.position.x == (float)(t % 100000u) &&
				state.position.y == -(float)(t % 100000u) &&
				state.reserved[0] == t * 2654435761u &&
				state.reserved[1] == ~t;
			state_reads++;
			if (!ok || t < last_tick)
				state_torn++;
			last_tick = t;
		}
	}
	QueryPerformanceCounter(&total_end);

report:
	put_text(&l, "pings ");
	put_u64(&l, i - 1);
	put_text(&l, " in ");
	put_u64(&l, (unsigned long long)(total_end - total_start) * 1000ull / (unsigned long long)frequency);
	put_text(&l, " ms, bad echoes ");
	put_u64(&l, bad_echo);
	send_line(shm, &l);

	put_text(&l, "round trip ns: min ");
	put_u64(&l, min_ns);
	put_text(&l, " avg ");
	put_u64(&l, i > 1 ? sum_ns / (i - 1) : 0);
	put_text(&l, " max ");
	put_u64(&l, max_ns);
	send_line(shm, &l);

	put_text(&l, "round trips <10us ");
	put_u64(&l, buckets[0]);
	put_text(&l, ", <100us ");
	put_u64(&l, buckets[1]);
	put_text(&l, ", <1ms ");
	put_u64(&l, buckets[2]);
	put_text(&l, ", >=1ms ");
	put_u64(&l, buckets[3]);
	send_line(shm, &l);

	put_text(&l, "player-state slot reads ");
	put_u64(&l, state_reads);
	put_text(&l, ", torn or out of order ");
	put_u64(&l, state_torn);
	send_line(shm, &l);

	Sleep(200); /* let the native side drain the ring */
	CR_STORE_REL(&shm->skyrim_state, CR_SIDE_CLOSING);
	UnmapViewOfFile(shm);
	CloseHandle(mapping);
	CloseHandle(file);
	ExitProcess(bad_echo == 0 && state_torn == 0 && i > PINGS ? 0 : 1);
}
