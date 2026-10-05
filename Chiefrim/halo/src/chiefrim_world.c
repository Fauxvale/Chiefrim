/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_WORLD.C

Skyrim's shape as Halo's collision (Chiefrim/docs/DESIGN.md §5):

- REGIONS: the SKSE plugin streams Skyrim's Havok collision near the player
  as triangles, per cube region (chiefrim_protocol.h, CR_MSG_COLLISION_*).
  They are kept here, in Skyrim units, until the world changes.
- BUILD: when regions change, a worker thread builds a Halo collision BSP
  from the regions around Chief (chiefrim_bsp.c).
- SWAP: on the main thread, between frames, the new BSP passes a self-test
  through Halo's own queries and replaces the old one: the collision BSP and
  the structure BSP's leaves (scenario_override_bsps). Bipeds standing on a
  triangle keep standing on it (surface ids are stable).
- FLOOR: until the first regions arrive, a flat square at the Skyrim
  player's feet (Phase 0's floor, now built the same way).
*/

#include "cseries.h"
#include "chiefrim/chiefrim.h"
#include "chiefrim/chiefrim_bsp.h"
#include "chiefrim/chiefrim_protocol.h"

#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "objects/objects.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "units/bipeds.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#undef open /* the port's translates Xbox paths; dumps go to a host path */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1 /* Linux's; the build's -D__STRICT_ANSI__ hides it */
#endif
#endif

/* The game's malloc is a debug allocator for the main thread; region data
and builds are shared with the worker, so they use the C library's. */
#undef malloc
#undef free
#undef realloc
#undef calloc
#include <stdlib.h>

/* ---------- constants */

#define REGION_SLOTS         4096     /* power of two */
#define BUILD_RADIUS_XY      1        /* regions around Chief's, in a build: at least 1024 units ahead of him */
#define BUILD_RADIUS_Z       1
#define EVICT_RADIUS         5        /* regions further away are dropped */
#define BUILD_INTERVAL_MS    250
#define SUPPORT_PROBE_ABOVE  0.1f /* world units: a biped's ground, from just above its feet... */
#define SUPPORT_PROBE_BELOW  0.2f /* ...to this far under them */
#define FLOOR_GUARD_RAISE    0.05f /* world units: the floor guard's ray starts this far above the feet */
#define SETTLE_REACH         0.5f /* world units (~107 Skyrim units) */
#define DUMP_SLOWER_THAN_MS  1000 /* builds this slow (or failing) are dumped */
#define FLOOR_HALF_SIZE      2000.0f  /* world units */
#define SELF_TEST_SAMPLES    48

/* ---------- structures */

struct region
{
	boolean used;
	boolean complete;
	long rx, ry, rz;
	unsigned long epoch;
	unsigned long total;
	unsigned long received;
	cr_triangle *triangles;
};

struct build_job
{
	struct chiefrim_triangle *triangles;
	long triangle_count;
	unsigned long map_generation;
	unsigned long origin_generation; /* the origin its triangles are relative to */
	boolean floor;
};

/* ---------- globals */

static struct
{
	boolean initialized;
	struct region regions[REGION_SLOTS];
	unsigned long epoch;
	boolean dirty;              /* regions changed since the last build started */
	boolean have_regions;       /* any non-empty region: the floor can go */
	unsigned long last_build_ms;
	long center[3];             /* Chief's region at the last build */

	cr_vec3 origin;             /* Skyrim units: Halo (0,0,0) */
	unsigned long origin_generation; /* counts origin changes */
	unsigned long world_generation;     /* Skyrim's world context in force */
	unsigned long collision_generation; /* the one Skyrim's last collision reset was for */
	real floor_z;               /* world units */
	boolean world_valid;

	struct structure_bsp *map_structure;  /* the loaded map's own */
	struct collision_bsp *map_collision;
	unsigned long map_generation;

	struct chiefrim_bsp *current;
	struct chiefrim_bsp *previous;
	boolean installed_floor;
	struct chiefrim_triangle *installed_triangles; /* current's input, for a dump */
	long installed_triangle_count;
	boolean have_chief;
	real_point3d chief;         /* last seen, world units */
	boolean settle_pending;     /* the first build since the origin moved has landed */
	boolean awaiting_first_build;

#ifdef __linux__
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	boolean thread_started;
#endif
	struct build_job job;       /* for the worker */
	boolean job_pending;
	boolean job_running;
	struct build_job done_job;  /* the job of the result, for the self-test */
	struct chiefrim_bsp *result;
	char result_error[160];
	boolean result_ready;
	unsigned long job_started_ms;
	unsigned long builds;
} world;

/* ---------- regions */

static unsigned long region_hash(long rx, long ry, long rz)
{
	unsigned long h = (unsigned long)rx * 73856093u ^ (unsigned long)ry * 19349663u ^ (unsigned long)rz * 83492791u;
	return h & (REGION_SLOTS - 1);
}

static struct region *region_find(long rx, long ry, long rz, boolean create)
{
	unsigned long index = region_hash(rx, ry, rz);
	unsigned long probes;

	for (probes = 0; probes < REGION_SLOTS; probes++)
	{
		struct region *region = &world.regions[index];

		if (region->used && region->rx == rx && region->ry == ry && region->rz == rz)
			return region;
		if (!region->used)
		{
			if (!create)
				return NULL;
			memset(region, 0, sizeof(*region));
			region->used = TRUE;
			region->rx = rx;
			region->ry = ry;
			region->rz = rz;
			return region;
		}
		index = (index + 1) & (REGION_SLOTS - 1);
	}
	return NULL;
}

static void region_clear_all(void)
{
	long i;

	for (i = 0; i < REGION_SLOTS; i++)
	{
		free(world.regions[i].triangles);
		world.regions[i].triangles = NULL;
		world.regions[i].used = FALSE;
	}
	world.have_regions = FALSE;
}

/* Open addressing can't simply delete: rebuild the table without the far
regions. */
static void region_evict(long cx, long cy, long cz)
{
	static struct region kept[REGION_SLOTS];
	long count = 0, i;

	for (i = 0; i < REGION_SLOTS; i++)
	{
		struct region *region = &world.regions[i];

		if (!region->used)
			continue;
		if (labs(region->rx - cx) > EVICT_RADIUS || labs(region->ry - cy) > EVICT_RADIUS || labs(region->rz - cz) > EVICT_RADIUS)
		{
			free(region->triangles);
			continue;
		}
		kept[count++] = *region;
	}
	memset(world.regions, 0, sizeof(world.regions));
	for (i = 0; i < count; i++)
	{
		struct region *slot = region_find(kept[i].rx, kept[i].ry, kept[i].rz, TRUE);

		if (slot)
			*slot = kept[i];
	}
}

/* ---------- the worker */

#ifdef __linux__
static long chiefrim_world_clock_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* A build's input, to replay offline (halo/test/bsp_harness --replay), when
it was slow or failed, or Chief fell through what it built:
CHIEFRIM_DUMP_DIR/build-<pid>-<n>.bin, "CRDUMP2", the count, the triangles
as they are in memory (i386), then whether Chief's position follows, and it. These are Skyrim's shapes:
tools/launch_halo.sh puts them under build/, which git ignores. */
static void chiefrim_world_dump(struct chiefrim_triangle const *triangles, long count,
	real_point3d const *chief, char const *why)
{
	static long dump_count = 0, slow_count = 0;
	char const *directory = getenv("CHIEFRIM_DUMP_DIR");
	char path[512];
	long has_chief = chief ? 1 : 0;
	real_point3d none = { 0.f, 0.f, 0.f };
	int file;
	boolean written;

	/* slow builds come in runs: a few of them, and room kept for falls */
	if (!directory || !directory[0] || dump_count >= 20 || !triangles)
		return;
	if (!chief && slow_count++ >= 3)
		return;
	snprintf(path, sizeof(path), "%s/build-%ld-%ld.bin", directory, (long)getpid(), dump_count++);
	file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0)
		return;
	written =
		write(file, "CRDUMP2", 8) == 8 &&
		write(file, &count, sizeof(count)) == (ssize_t)sizeof(count) &&
		write(file, triangles, sizeof(*triangles) * (size_t)count) == (ssize_t)(sizeof(*triangles) * (size_t)count) &&
		write(file, &has_chief, sizeof(has_chief)) == (ssize_t)sizeof(has_chief) &&
		write(file, chief ? chief : &none, sizeof(none)) == (ssize_t)sizeof(none);
	close(file);
	if (!written)
	{
		unlink(path);
		return;
	}
	fprintf(stderr, "chiefrim: dumped a build's input (%s, %ld triangles) to %s\n", why, count, path);
}

static void *chiefrim_world_worker(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&world.lock);
	for (;;)
	{
		struct build_job job;
		struct chiefrim_bsp *bsp;
		char error[160];
		struct collision_bsp *map_collision;
		struct structure_bsp *map_structure;
		long started, elapsed;

		while (!world.job_pending)
			pthread_cond_wait(&world.wake, &world.lock);
		job = world.job;
		world.job_pending = FALSE;
		world.job_running = TRUE;
		map_collision = world.map_collision;
		map_structure = world.map_structure;
		pthread_mutex_unlock(&world.lock);

		error[0] = 0;
		started = chiefrim_world_clock_ms();
		bsp = chiefrim_bsp_build(job.triangles, job.triangle_count,
			(real_plane3d const *)map_collision->bsp3d.planes.address, map_collision->bsp3d.planes.count,
			map_structure, map_structure->collision_materials.count > 0 ? 0 : NONE, error, sizeof(error));
		elapsed = chiefrim_world_clock_ms() - started;
		if (!bsp || elapsed > DUMP_SLOWER_THAN_MS)
			chiefrim_world_dump(job.triangles, job.triangle_count, NULL, bsp ? "slow" : "failed");

		pthread_mutex_lock(&world.lock);
		world.result = bsp;
		world.done_job = job;
		csstrncpy(world.result_error, error, sizeof(world.result_error) - 1);
		world.result_ready = TRUE;
		world.job_running = FALSE;
	}
	return NULL;
}
#endif

/* ---------- checks and swaps (main thread) */

/* Rays from just in front of, and just behind, sampled surfaces must hit
them; spheres resting on them must touch them. Through Halo's own queries,
so a BSP that passes is one Halo reads as we meant. The surfaces sampled are
the ones the builder kept (it welds and drops degenerate triangles). */
static boolean chiefrim_world_self_test(struct chiefrim_bsp *bsp)
{
	struct collision_bsp_test_sphere_result *sphere;
	long samples = MIN(bsp->triangle_count, SELF_TEST_SAMPLES);
	long passed = 0, tested = 0, i;

	sphere = (struct collision_bsp_test_sphere_result *)malloc(sizeof(*sphere));
	if (!sphere)
		return FALSE;
	for (i = 0; i < samples; i++)
	{
		long surface_index = i * bsp->triangle_count / samples;
		struct collision_surface const *surface = &bsp->surfaces[surface_index];
		real_point3d points[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		short point_count = collision_surface_polygon(&bsp->bsp, surface_index, points);
		real_plane3d plane;
		real_point3d centre, start;
		real_vector3d ray, e1, e2, n;
		struct collision_bsp_test_vector_result result;
		boolean front, back, touch;
		real area;
		short p;

		if (point_count < 3)
			continue;
		bsp3d_get_plane_from_designator(&bsp->bsp.bsp3d, surface->plane_designator, &plane);
		e1.i = points[1].x - points[0].x; e1.j = points[1].y - points[0].y; e1.k = points[1].z - points[0].z;
		e2.i = points[2].x - points[0].x; e2.j = points[2].y - points[0].y; e2.k = points[2].z - points[0].z;
		n.i = e1.j * e2.k - e1.k * e2.j;
		n.j = e1.k * e2.i - e1.i * e2.k;
		n.k = e1.i * e2.j - e1.j * e2.i;
		area = 0.5f * sqrtf(n.i * n.i + n.j * n.j + n.k * n.k);
		if (area < 0.0005f)
			continue; /* slivers say little about the BSP */
		centre.x = centre.y = centre.z = 0.f;
		for (p = 0; p < point_count; p++)
		{
			centre.x += points[p].x / point_count;
			centre.y += points[p].y / point_count;
			centre.z += points[p].z / point_count;
		}
		tested++;

		start.x = centre.x + plane.n.i * 0.1f; start.y = centre.y + plane.n.j * 0.1f; start.z = centre.z + plane.n.k * 0.1f;
		ray.i = -plane.n.i * 0.2f; ray.j = -plane.n.j * 0.2f; ray.k = -plane.n.k * 0.2f;
		front = collision_bsp_test_vector(
			FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit),
			&bsp->bsp, 0, NULL, &start, &ray, REAL_MAX, &result) && result.t <= 0.55f;

		start.x = centre.x - plane.n.i * 0.1f; start.y = centre.y - plane.n.j * 0.1f; start.z = centre.z - plane.n.k * 0.1f;
		ray.i = -ray.i; ray.j = -ray.j; ray.k = -ray.k;
		back = collision_bsp_test_vector(
			FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit),
			&bsp->bsp, 0, NULL, &start, &ray, REAL_MAX, &result) && result.t <= 0.55f;

		start.x = centre.x + plane.n.i * 0.05f; start.y = centre.y + plane.n.j * 0.05f; start.z = centre.z + plane.n.k * 0.05f;
		touch = collision_bsp_test_sphere(&bsp->bsp, 0, NULL, &start, 0.1f, sphere);

		if (front && back && touch)
			passed++;
		else if (tested - passed <= 3)
			error(_error_silent, "chiefrim: BSP self-test: surface %ld (area %.4f, normal %.2f %.2f %.2f, at %.2f %.2f %.2f): "
				"ray from front %s, from back %s, sphere %s",
				surface_index, area, plane.n.i, plane.n.j, plane.n.k, centre.x, centre.y, centre.z,
				front ? "hit" : "missed", back ? "hit" : "missed", touch ? "touched" : "missed");
	}
	free(sphere);
	if (tested == 0)
		return TRUE;
	/* a miss or two in a sample is the builder's known rare kind (about
	1 surface in 10,000 on Skyrim's meshes): rejecting the build for it would
	leave Chief on stale collision. Many misses mean a broken build. */
	if (tested - passed > MAX(2, tested / 10))
	{
		error(_error_silent, "chiefrim: BSP self-test failed: %ld of %ld surfaces", passed, tested);
		return FALSE;
	}
	return TRUE;
}

static void chiefrim_world_install(struct chiefrim_bsp *bsp)
{
	struct chiefrim_bsp *old = world.current;
	struct object_iterator iterator;

	scenario_override_bsps(&bsp->bsp, &bsp->structure);

	/* bipeds keep standing on what they stood on: the new BSP's surface
	right under their feet. (Not by the Skyrim triangle's id: the builder cuts
	triangles, and the pieces of one may be far apart. A wrong support
	surface makes Halo pull the biped onto it: a jolt at every swap.) */
	object_iterator_new(&iterator, _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct biped_datum *biped = biped_get(iterator.index);
		long support = biped->biped.support_surface_index;
		long remapped = NONE;

		if (support != NONE)
		{
			struct collision_bsp_test_vector_result result;
			real_point3d feet, start;
			real_vector3d down = { 0.f, 0.f, -(SUPPORT_PROBE_ABOVE + SUPPORT_PROBE_BELOW) };

			object_get_origin(iterator.index, &feet);
			start = feet;
			start.z += SUPPORT_PROBE_ABOVE;
			if (collision_bsp_test_vector(FLAG(_collision_test_front_facing_surfaces_bit),
				&bsp->bsp, 0, NULL, &start, &down, REAL_MAX, &result))
			{
				remapped = result.surface_index;
			}
		}
		biped_disconnect_from_structure_bsp(iterator.index);
		biped->biped.support_surface_index = remapped;
	}

	/* the one before the old one: nothing can point into it any more */
	chiefrim_bsp_free(world.previous);
	world.previous = old;
	world.current = bsp;
}

static void chiefrim_world_drop_builds(void)
{
	chiefrim_bsp_free(world.current);
	chiefrim_bsp_free(world.previous);
	world.current = NULL;
	world.previous = NULL;
}

static boolean chiefrim_world_install_floor(void)
{
	struct chiefrim_triangle floor[2];
	static real const corners[4][2] = {
		{ -FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE },
		{ FLOOR_HALF_SIZE, -FLOOR_HALF_SIZE },
		{ FLOOR_HALF_SIZE, FLOOR_HALF_SIZE },
		{ -FLOOR_HALF_SIZE, FLOOR_HALF_SIZE },
	};
	static int const order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
	struct chiefrim_bsp *bsp;
	char message[160];
	long t, v;

	if (!world.map_structure || !world.map_collision)
		return FALSE;
	memset(floor, 0, sizeof(floor));
	for (t = 0; t < 2; t++)
	{
		for (v = 0; v < 3; v++)
		{
			floor[t].v[v].x = corners[order[t][v]][0];
			floor[t].v[v].y = corners[order[t][v]][1];
			floor[t].v[v].z = world.floor_z;
		}
		floor[t].id = 0x7FFF0000u + (unsigned long)t;
	}
	message[0] = 0;
	bsp = chiefrim_bsp_build(floor, 2,
		(real_plane3d const *)world.map_collision->bsp3d.planes.address, world.map_collision->bsp3d.planes.count,
		world.map_structure, world.map_structure->collision_materials.count > 0 ? 0 : NONE, message, sizeof(message));
	if (!bsp || !chiefrim_world_self_test(bsp))
	{
		error(_error_silent, "chiefrim: no floor (%s)", bsp ? "self-test" : message);
		chiefrim_bsp_free(bsp);
		return FALSE;
	}
	chiefrim_world_install(bsp);
	world.installed_floor = TRUE;
	error(_error_silent, "chiefrim: collision is a flat floor at z=%.3f wu (until Skyrim's arrives)", world.floor_z);
	return TRUE;
}

/* Collects the complete regions around Chief and hands them to the worker. */
static void chiefrim_world_start_build(long cx, long cy, long cz)
{
#ifdef __linux__
	long count = 0, i, n;
	struct chiefrim_triangle *triangles;

	for (i = 0; i < REGION_SLOTS; i++)
	{
		struct region const *region = &world.regions[i];

		if (region->used && region->complete && region->epoch == world.epoch &&
			labs(region->rx - cx) <= BUILD_RADIUS_XY && labs(region->ry - cy) <= BUILD_RADIUS_XY &&
			labs(region->rz - cz) <= BUILD_RADIUS_Z)
		{
			count += (long)region->total;
		}
	}
	if (count == 0)
		return;
	triangles = (struct chiefrim_triangle *)malloc(sizeof(struct chiefrim_triangle) * (size_t)count);
	if (!triangles)
		return;
	n = 0;
	for (i = 0; i < REGION_SLOTS; i++)
	{
		struct region const *region = &world.regions[i];
		unsigned long t;

		if (!(region->used && region->complete && region->epoch == world.epoch &&
			labs(region->rx - cx) <= BUILD_RADIUS_XY && labs(region->ry - cy) <= BUILD_RADIUS_XY &&
			labs(region->rz - cz) <= BUILD_RADIUS_Z))
		{
			continue;
		}
		for (t = 0; t < region->total; t++)
		{
			struct chiefrim_triangle *out = &triangles[n++];
			long v;

			for (v = 0; v < 3; v++)
			{
				cr_vec3 halo = cr_sky_to_halo(region->triangles[t].v[v], world.origin);

				out->v[v].x = halo.x;
				out->v[v].y = halo.y;
				out->v[v].z = halo.z;
			}
			/* stable across builds: the region, and the index in it */
			out->id = (((unsigned long)region->rx & 0x7Fu) << 24 | ((unsigned long)region->ry & 0x7Fu) << 17 |
				((unsigned long)region->rz & 0x1u) << 16 | (t & 0xFFFFu)) & 0x7FFFFFFFu;
			out->material = 0;
			out->pad = 0;
		}
	}

	pthread_mutex_lock(&world.lock);
	world.job.triangles = triangles;
	world.job.triangle_count = n;
	world.job.map_generation = world.map_generation;
	world.job.origin_generation = world.origin_generation;
	world.job.floor = FALSE;
	world.job_pending = TRUE;
	world.dirty = FALSE;
	world.job_started_ms = system_milliseconds();
	pthread_cond_signal(&world.wake);
	pthread_mutex_unlock(&world.lock);
	world.last_build_ms = system_milliseconds();
#else
	(void)cx; (void)cy; (void)cz;
#endif
}

static void chiefrim_world_collect_result(void)
{
#ifdef __linux__
	struct chiefrim_bsp *bsp = NULL;
	struct build_job job;
	char message[160];
	boolean ready;

	pthread_mutex_lock(&world.lock);
	ready = world.result_ready;
	if (ready)
	{
		bsp = world.result;
		job = world.done_job;
		csstrncpy(message, world.result_error, sizeof(message) - 1);
		message[sizeof(message) - 1] = 0;
		world.result = NULL;
		world.result_ready = FALSE;
	}
	pthread_mutex_unlock(&world.lock);
	if (!ready)
		return;

	if (!bsp)
		error(_error_silent, "chiefrim: collision build failed: %s", message);
	else if (job.map_generation != world.map_generation || job.origin_generation != world.origin_generation ||
		!world.world_valid)
	{
		chiefrim_bsp_free(bsp); /* built for a map, world or origin that is gone */
	}
	else if (!chiefrim_world_self_test(bsp))
		chiefrim_bsp_free(bsp);
	else
	{
		world.builds++;
		if (world.builds <= 3 || world.builds % 20 == 0)
		{
			error(_error_silent, "chiefrim: collision #%lu: %ld triangles -> %ld nodes, %ld leaves, %ld surfaces, depth %ld%s, %lu ms",
				world.builds, bsp->triangle_count, bsp->node_count, bsp->leaf_count, bsp->surface_count,
				bsp->max_depth, bsp->dropped_overlaps ? " (some coplanar overlaps)" : "",
				system_milliseconds() - world.job_started_ms);
		}
		chiefrim_world_install(bsp);
		world.installed_floor = FALSE;
		if (world.awaiting_first_build)
		{
			world.awaiting_first_build = FALSE;
			world.settle_pending = TRUE;
		}
		free(world.installed_triangles);
		world.installed_triangles = job.triangles;
		world.installed_triangle_count = job.triangle_count;
		job.triangles = NULL;
	}
	free(job.triangles);
#endif
}

static boolean chiefrim_world_worker_busy(void)
{
#ifdef __linux__
	boolean busy;

	if (!world.thread_started)
		return TRUE;
	pthread_mutex_lock(&world.lock);
	busy = world.job_pending || world.job_running || world.result_ready;
	pthread_mutex_unlock(&world.lock);
	return busy;
#else
	return TRUE;
#endif
}

/* ---------- public code */

void chiefrim_world_initialize(void)
{
	if (world.initialized)
		return;
	memset(&world, 0, sizeof(world));
#ifdef __linux__
	pthread_mutex_init(&world.lock, NULL);
	pthread_cond_init(&world.wake, NULL);
	if (pthread_create(&world.thread, NULL, chiefrim_world_worker, NULL) == 0)
	{
		pthread_detach(world.thread);
		world.thread_started = TRUE;
	}
	else
	{
		error(_error_silent, "chiefrim: no collision worker thread; the floor stays");
	}
#endif
	world.initialized = TRUE;
}

void chiefrim_world_map_loaded(void)
{
	/* the map's own BSPs, before anything of ours replaces them */
	world.map_structure = global_structure_bsp_get();
	world.map_collision = global_collision_bsp_get();
	world.map_generation++;
	chiefrim_world_drop_builds();
	world.dirty = world.have_regions;
	if (world.world_valid)
		chiefrim_world_install_floor();
}

void chiefrim_world_reset(cr_vec3 origin, real floor_z)
{
	world.origin = origin;
	world.origin_generation++; /* builds in flight were for the old origin */
	world.awaiting_first_build = TRUE;
	world.settle_pending = FALSE;
	world.floor_z = floor_z;
	world.world_valid = TRUE;
	/* The regions are in Skyrim units, whatever the origin: only Skyrim's
	collision reset (with each new world) drops them. Until a build for the
	new origin lands, the floor. */
	world.dirty = world.have_regions;
	if (world.map_structure)
		chiefrim_world_install_floor();
}

void chiefrim_world_message(int type, void const *message)
{
	if (type == CR_MSG_COLLISION_RESET)
	{
		cr_msg_collision_reset const *reset = (cr_msg_collision_reset const *)message;

		world.epoch = reset->epoch;
		world.collision_generation = reset->world_generation;
		region_clear_all();
		world.dirty = FALSE;
	}
	else if (type == CR_MSG_COLLISION_TRIS)
	{
		cr_msg_collision_tris const *tris = (cr_msg_collision_tris const *)message;
		struct region *region;

		if (tris->epoch != world.epoch)
			return;
		region = region_find(tris->rx, tris->ry, tris->rz, TRUE);
		if (!region)
			return;
		if (tris->first == 0)
		{
			free(region->triangles);
			region->triangles = tris->total ? (cr_triangle *)malloc(sizeof(cr_triangle) * tris->total) : NULL;
			region->total = region->triangles || !tris->total ? tris->total : 0;
			region->received = 0;
			region->complete = FALSE;
			region->epoch = tris->epoch;
		}
		if (tris->first != region->received || tris->first + tris->count > region->total)
			return; /* out of order: wait for the region to be sent again */
		if (tris->count)
			memcpy(&region->triangles[tris->first], tris->tris, sizeof(cr_triangle) * tris->count);
		region->received += tris->count;
		if (region->received == region->total)
		{
			region->complete = TRUE;
			world.dirty = TRUE;
			if (region->total)
				world.have_regions = TRUE;
		}
	}
}

void chiefrim_world_update(real_point3d const *chief)
{
	long cx, cy, cz;

	if (!world.initialized || !world.world_valid || !world.map_structure)
		return;
	chiefrim_world_collect_result();
	world.have_chief = chief != NULL;
	if (chief)
		world.chief = *chief;
	if (!chief || !world.have_regions)
		return;

	{
		cr_vec3 halo = { chief->x, chief->y, chief->z };
		cr_vec3 sky = cr_halo_to_sky(halo, world.origin);

		cx = (long)floorf(sky.x / CR_REGION_UNITS);
		cy = (long)floorf(sky.y / CR_REGION_UNITS);
		cz = (long)floorf(sky.z / CR_REGION_UNITS);
	}
	if (cx != world.center[0] || cy != world.center[1] || cz != world.center[2])
	{
		/* Chief is in another region: drop the far ones, build anew */
		region_evict(cx, cy, cz);
		world.center[0] = cx;
		world.center[1] = cy;
		world.center[2] = cz;
		world.dirty = TRUE;
	}
	/* Skyrim's world context and its collision reset come by different
	ways (a slot, the ring): until both are in, the regions held are the
	old world's, and built around the new origin they'd be in the wrong
	place. */
	if (world.collision_generation && world.collision_generation != world.world_generation)
		return;
	if (world.dirty && !chiefrim_world_worker_busy() &&
		system_milliseconds() - world.last_build_ms >= BUILD_INTERVAL_MS)
	{
		chiefrim_world_start_build(cx, cy, cz);
	}
}

boolean chiefrim_world_below_collision(real_point3d const *point)
{
	/* half a world unit (~100 Skyrim units) of slack: a body resting on the
	lowest ground is above it anyway */
	return world.current && !world.installed_floor && point->z < world.current->min_z - 0.5f;
}

void chiefrim_world_dump_installed(char const *why)
{
#ifdef __linux__
	if (!world.installed_floor)
	{
		chiefrim_world_dump(world.installed_triangles, world.installed_triangle_count,
			world.have_chief ? &world.chief : NULL, why);
	}
#else
	(void)why;
#endif
}

boolean chiefrim_world_settle(real_point3d const *chief, real *ground_z)
{
	struct collision_bsp_test_vector_result result;
	real_point3d start;
	real_vector3d down = { 0.f, 0.f, -(SETTLE_REACH + 0.1f) };

	if (!world.settle_pending || !world.current || world.installed_floor)
		return FALSE;
	world.settle_pending = FALSE;
	/* Chief stood on the stand-in floor until now: if Skyrim's ground is
	a little higher there, he is in it. From a knee's height above him (not
	so high as to find a roof), down to just under his feet. */
	start = *chief;
	start.z += SETTLE_REACH;
	if (!collision_bsp_test_vector(3, &world.current->bsp, 0, NULL, &start, &down, REAL_MAX, &result))
		return FALSE;
	*ground_z = start.z + down.k * result.t;
	return *ground_z > chief->z + 0.01f;
}

boolean chiefrim_world_has_skyrim_collision(void)
{
	return world.current && !world.installed_floor;
}

/* Did a biped going from one point to the next (feet, world units) pass
down through a floor (a surface facing up)? Only downward moves: going up
steps and slopes is Halo's to do. */
boolean chiefrim_world_crossed_floor(real_point3d const *from, real_point3d const *to)
{
	struct collision_bsp_test_vector_result result;
	real_point3d start = *from;
	real_vector3d move;

	if (!world.current || world.installed_floor || to->z >= from->z)
		return FALSE;
	start.z += FLOOR_GUARD_RAISE;
	move.i = to->x - from->x;
	move.j = to->y - from->y;
	move.k = to->z - from->z;
	if (!collision_bsp_test_vector(FLAG(_collision_test_front_facing_surfaces_bit),
		&world.current->bsp, 0, NULL, &start, &move, REAL_MAX, &result))
	{
		return FALSE;
	}
	{
		real k = result.plane ? result.plane->n.k : 0.f;

		if (result.plane_designator < 0)
			k = -k;
		return k > 0.7f; /* a floor, not a wall he slid down */
	}
}

/* Room for Chief here (feet, world units): a ball the width of him, from
his knees up, touches nothing. */
boolean chiefrim_world_room_for(real_point3d const *feet, real height)
{
	static struct collision_bsp_test_sphere_result *sphere = NULL;
	real_point3d centre = *feet;
	real radius = 0.18f;

	if (!world.current || world.installed_floor)
		return TRUE;
	if (!sphere && !(sphere = (struct collision_bsp_test_sphere_result *)malloc(sizeof(*sphere))))
		return TRUE;
	centre.z += MAX(height * 0.55f, radius + 0.1f);
	return !collision_bsp_test_sphere(&world.current->bsp, 0, NULL, &centre, radius, sphere);
}

void chiefrim_world_generation(unsigned long generation)
{
	world.world_generation = generation;
}
