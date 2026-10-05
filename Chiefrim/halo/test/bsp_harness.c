/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
BSP_HARNESS.C

Runs chiefrim_bsp_build alone, outside the game, on synthetic Skyrim-like
collision (Chiefrim/tools/test_bsp.sh builds it with the game's flags):
bumpy ground with a plane per triangle, rotated boxes like rocks, a ramp,
a cliff and a wall. Prints the build's size, depth and time.
*/

#include "cseries.h"
#include "chiefrim/chiefrim_bsp.h"
#include "physics/bsp3d.h"
#include "physics/collision_bsp.h"
#include "physics/collisions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#undef fopen /* the port's translates Xbox paths and isn't linked here */

#undef malloc
#undef free

static double now_ms(void)
{
	return (double)clock() * 1000.0 / CLOCKS_PER_SEC; /* CPU time: the game's flags hide POSIX clocks */
}

static float height(float x, float y)
{
	float h = 12 * sinf(x * 0.013f) * cosf(y * 0.011f) + 6 * sinf((x + y) * 0.031f);
	if (y > 256)
		h += fminf(y - 256, 1244) * tanf(25 * 3.14159265f / 180);
	if (x > 600)
		h += (x - 600) * tanf(60 * 3.14159265f / 180);
	return h;
}

static unsigned long rng = 7;
static float uniform(float lo, float hi)
{
	rng = rng * 1103515245u + 12345u;
	return lo + (hi - lo) * (float)((rng >> 8) & 0xFFFF) / 65535.f;
}

static void put(char const *text) { fputs(text, stdout); }
static void put_long(long value)
{
	char digits[24];
	int n = 0;
	unsigned long v = value < 0 ? (unsigned long)-value : (unsigned long)value;
	if (value < 0)
		put("-");
	do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n--)
		fputc(digits[n], stdout);
}

/* Does leaf L hold surface s (or its twin) in a reference to plane P? */
static int leaf_has(struct chiefrim_bsp *bsp, long leaf, long plane, long surface)
{
	struct chiefrim_bsp_leaf const *l = &bsp->leaves[leaf];
	long r;

	for (r = l->first_bsp2d_reference_index; r < l->first_bsp2d_reference_index + l->bsp2d_reference_count; r++)
	{
		static long stack[1 << 16]; long depth = 0;

		if ((bsp->references[r].plane_designator & LONG_MAX) != plane)
			continue;
		stack[depth++] = bsp->references[r].root_index;
		while (depth)
		{
			long n = stack[--depth];

			if (n & LONG_MIN)
			{
				long found = n & LONG_MAX;
				if (found == surface || found == surface + bsp->triangle_count || found + bsp->triangle_count == surface)
					return 1;
				continue;
			}
			stack[depth++] = bsp->nodes2d[n].child_indices[0];
			stack[depth++] = bsp->nodes2d[n].child_indices[1];
		}
	}
	return 0;
}

/* Halo's ray traversal again (collision_bsp.c), printing each leaf visit and
the plane crossed into it, and whether that leaf refers to the surface. */
static void trace(struct chiefrim_bsp *bsp, long node, real_point3d const *point, real_vector3d const *vector,
	real t0, real t1, long *last_plane, long surface)
{
	if (!(node & LONG_MIN))
	{
		struct bsp3d_node const *n = &bsp->nodes[node];
		real_plane3d const *plane = &bsp->planes[n->plane_designator];
		real distance = plane->n.i * point->x + plane->n.j * point->y + plane->n.k * point->z - plane->d;
		real dot = vector->i * plane->n.i + vector->j * plane->n.j + vector->k * plane->n.k;
		real d0 = dot * t0 + distance, d1 = dot * t1 + distance;
		int back = d0 < 0.f || d1 < 0.f, front = d0 >= 0.f || d1 >= 0.f;

		if (back && front)
		{
			int f = dot > 0.f;
			real t = -(distance / dot);

			trace(bsp, n->children[!f], point, vector, t0, t, last_plane, surface);
			*last_plane = n->plane_designator;
			trace(bsp, n->children[f], point, vector, t, t1, last_plane, surface);
		}
		else
			trace(bsp, n->children[front], point, vector, t0, t1, last_plane, surface);
		return;
	}
	/* no printf here: the game's flags make variadic calls into glibc unsafe */
	{
		long own = bsp->surfaces[surface].plane_designator & LONG_MAX;
		put("    leaf "); put_long(node & LONG_MAX);
		put(" t "); put_long((long)(t0 * 10000.f)); put(".."); put_long((long)(t1 * 10000.f));
		put(" (1/10000) across plane "); put_long(*last_plane);
		if (*last_plane == own)
			put(" (the surface's)");
		put("; has it there: "); put_long(*last_plane >= 0 ? leaf_has(bsp, node & LONG_MAX, *last_plane, surface) : 0);
		put("; on its own plane "); put_long(own); put(": "); put_long(leaf_has(bsp, node & LONG_MAX, own, surface));
		put("\n");
	}
}

/* collision_leaf_test_vector's step for one leaf and plane, spelled out:
each reference to the plane, the surface its 2D BSP gives, and that
surface's projected corners around the projected hit point. */
static void replay_leaf(struct chiefrim_bsp *bsp, long leaf, long plane_index, real_point3d const *hit)
{
	struct chiefrim_bsp_leaf const *l = &bsp->leaves[leaf];
	real_plane3d const *plane = &bsp->planes[plane_index];
	long r;

	for (r = l->first_bsp2d_reference_index; r < l->first_bsp2d_reference_index + l->bsp2d_reference_count; r++)
	{
		struct chiefrim_bsp_reference const *ref = &bsp->references[r];
		real ai = fabsf(plane->n.i), aj = fabsf(plane->n.j), ak = fabsf(plane->n.k);
		short projection;
		boolean sign;
		real_point2d p2, corner[3];
		long surface, edge_index, v = 0;

		if ((ref->plane_designator & LONG_MAX) != plane_index)
			continue;
		projection = (ak >= aj && ak >= ai) ? _z : (aj >= ai ? _y : _x);
		sign = projection_sign_from_vector3d(&plane->n, projection) != ((ref->plane_designator & LONG_MIN) ? TRUE : FALSE);
		project_point3d(hit, projection, sign, &p2);
		surface = bsp2d_test_point(&bsp->bsp.bsp2d.nodes, &p2, ref->root_index);
		put("      reference "); put_long(r); put(ref->plane_designator & LONG_MIN ? " (back)" : " (front)");
		put(": surface "); put_long(surface);
		if (surface == NONE) { put("\n"); continue; }
		edge_index = bsp->surfaces[surface].first_edge_index;
		do
		{
			struct collision_edge const *e = &bsp->edges[edge_index];
			boolean reverse = e->surface_indices[1] == surface;
			project_point3d(&bsp->vertices[e->vertex_indices[reverse]].point, projection, sign, &corner[v]);
			edge_index = e->edge_indices[reverse];
			v++;
		} while (edge_index != bsp->surfaces[surface].first_edge_index && v < 3);
		put(" corners (x10000)");
		for (v = 0; v < 3; v++) { put(" "); put_long((long)(corner[v].x * 10000)); put(","); put_long((long)(corner[v].y * 10000)); }
		put(" point "); put_long((long)(p2.x * 10000)); put(","); put_long((long)(p2.y * 10000));
		for (v = 0; v < 3; v++)
		{
			real_point2d const *a0 = &corner[v], *a1 = &corner[(v + 1) % 3];
			real cross = (p2.x - a0->x) * (a1->y - a0->y) - (p2.y - a0->y) * (a1->x - a0->x);
			put(cross > 0.f ? " out" : " in");
		}
		put("\n");
	}
}

/* Halo's own ray and sphere queries on every front surface, as
chiefrim_world.c samples them in the game; details of the first failures. */
static void self_test(struct chiefrim_bsp *bsp)
{
	static struct collision_bsp_test_sphere_result sphere;
	long tested = 0, passed = 0, shown = 0, s;
	long fail_front = 0, fail_back = 0, fail_sphere = 0;

	for (s = 0; s < bsp->triangle_count; s++)
	{
		struct collision_surface const *surface = &bsp->surfaces[s];
		real_point3d points[8], centre, start;
		short point_count = collision_surface_polygon(&bsp->bsp, s, points), p;
		real_plane3d plane;
		real_vector3d ray, e1, e2, n;
		struct collision_bsp_test_vector_result result;
		boolean front, back, touch;
		real area;

		if (point_count < 3)
			continue;
		bsp3d_get_plane_from_designator(&bsp->bsp.bsp3d, surface->plane_designator, &plane);
		e1.i = points[1].x - points[0].x; e1.j = points[1].y - points[0].y; e1.k = points[1].z - points[0].z;
		e2.i = points[2].x - points[0].x; e2.j = points[2].y - points[0].y; e2.k = points[2].z - points[0].z;
		n.i = e1.j * e2.k - e1.k * e2.j; n.j = e1.k * e2.i - e1.i * e2.k; n.k = e1.i * e2.j - e1.j * e2.i;
		area = 0.5f * sqrtf(n.i * n.i + n.j * n.j + n.k * n.k);
		if (area < 0.0005f)
			continue;
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
		front = collision_bsp_test_vector(3, &bsp->bsp, 0, NULL, &start, &ray, REAL_MAX, &result) && result.t <= 0.55f; /* plane merging may sit a triangle 0.004 wu off its plane */
		start.x = centre.x - plane.n.i * 0.1f; start.y = centre.y - plane.n.j * 0.1f; start.z = centre.z - plane.n.k * 0.1f;
		ray.i = -ray.i; ray.j = -ray.j; ray.k = -ray.k;
		back = collision_bsp_test_vector(3, &bsp->bsp, 0, NULL, &start, &ray, REAL_MAX, &result) && result.t <= 0.55f; /* plane merging may sit a triangle 0.004 wu off its plane */
		start.x = centre.x + plane.n.i * 0.05f; start.y = centre.y + plane.n.j * 0.05f; start.z = centre.z + plane.n.k * 0.05f;
		touch = collision_bsp_test_sphere(&bsp->bsp, 0, NULL, &start, 0.1f, &sphere);
		fail_front += !front;
		fail_back += !back;
		fail_sphere += !touch;
		if (front && back && touch)
			passed++;
		else if (shown++ < 5)
		{
			long leaf = bsp3d_test_point(&bsp->bsp.bsp3d, 0, &centre);
			printf("  surface %ld designator %08lx normal (%.3f %.3f %.3f) d %.4f at (%.3f %.3f %.3f): front %d back %d sphere %d, leaf %ld\n",
				s, (unsigned long)surface->plane_designator, plane.n.i, plane.n.j, plane.n.k, plane.d,
				centre.x, centre.y, centre.z, front, back, touch, leaf);
			if (shown <= 2)
			{
				long last = NONE;
				real sign = front ? -1.f : 1.f; /* trace the failing side */
				start.x = centre.x + sign * plane.n.i * 0.1f; start.y = centre.y + sign * plane.n.j * 0.1f; start.z = centre.z + sign * plane.n.k * 0.1f;
				ray.i = -sign * plane.n.i * 0.2f; ray.j = -sign * plane.n.j * 0.2f; ray.k = -sign * plane.n.k * 0.2f;
				trace(bsp, 0, &start, &ray, 0.f, 1.f, &last, s);
				{
					long hit_leaf = bsp3d_test_point(&bsp->bsp.bsp3d, 0, &centre);
					put("    replay at the surface's centre, leaf "); put_long(hit_leaf); put("\n");
					replay_leaf(bsp, hit_leaf, surface->plane_designator & LONG_MAX, &centre);
				}
			}
		}
	}
	printf("self-test: %ld of %ld surfaces pass (missed from front %ld, from back %ld, sphere %ld)\n",
		passed, tested, fail_front, fail_back, fail_sphere);
}

/* What is under Chief, where he fell through: a ray straight down, and the
triangles of the input around him. */
static void probe_below(struct chiefrim_bsp *bsp, real_point3d const *chief)
{
	struct collision_bsp_test_vector_result result;
	real_point3d start = *chief;
	real_vector3d down = { 0.f, 0.f, -20.f };
	long i;

	put("chief at (x1000 wu) ");
	put_long((long)(chief->x * 1000)); put(" "); put_long((long)(chief->y * 1000)); put(" "); put_long((long)(chief->z * 1000));
	put(", lowest collision ");
	put_long((long)(bsp->min_z * 1000));
	start.z += 2.f;
	if (collision_bsp_test_vector(3, &bsp->bsp, 0, NULL, &start, &down, REAL_MAX, &result))
	{
		put("; down from 2 wu above, a hit at t x1000 ");
		put_long((long)(result.t * 1000));
		put(", surface ");
		put_long(result.surface_index);
	}
	else
		put("; down from 2 wu above: nothing");
	put("\n");
	(void)i;
}

int main(int argc, char **argv)
{
	long capacity = 400000, n = 0, i, j;
	float half = argc > 1 ? (float)atof(argv[1]) : 3072.f;
	long rocks = argc > 2 ? atol(argv[2]) : 400;
	struct chiefrim_triangle *t = (struct chiefrim_triangle *)calloc((size_t)capacity, sizeof(*t));
	real_plane3d map_plane = { { 0.f, 0.f, 1.f }, -10000.f };
	struct structure_bsp map;
	struct structure_leaf leaf;
	struct structure_cluster cluster;
	struct chiefrim_bsp *bsp;
	char error[160];
	double start;
	const float k = 1.f / 213.36f;
	long has_chief = 0;
	real_point3d chief;

	memset(&map, 0, sizeof(map));
	memset(&leaf, 0, sizeof(leaf));
	memset(&cluster, 0, sizeof(cluster));
	map.leaves.count = 1;
	map.leaves.address = &leaf;
	map.clusters.count = 1;
	map.clusters.address = &cluster;

#define TRI(ax, ay, az, bx, by, bz, cx, cy, cz) do { \
		t[n].v[0].x = (ax) * k; t[n].v[0].y = (ay) * k; t[n].v[0].z = (az) * k; \
		t[n].v[1].x = (bx) * k; t[n].v[1].y = (by) * k; t[n].v[1].z = (bz) * k; \
		t[n].v[2].x = (cx) * k; t[n].v[2].y = (cy) * k; t[n].v[2].z = (cz) * k; \
		t[n].id = (unsigned long)n; n++; } while (0)

	/* --replay FILE: a build Halo dumped (chiefrim_world.c), with where
	Chief was if he fell through it; no strcmp or memcmp, which Halo's
	headers send to its own (unlinked) versions */
	if (argc > 2 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'r')
	{
		FILE *file = fopen(argv[2], "rb");
		char magic[8];

		if (!file || fread(magic, 1, 8, file) != 8 || magic[0] != 'C' || magic[1] != 'R' ||
			fread(&n, sizeof(n), 1, file) != 1 || n < 0 || n > capacity ||
			fread(t, sizeof(*t), (size_t)n, file) != (size_t)n)
		{
			printf("can't read the dump %s\n", argv[2]);
			return 1;
		}
		if (magic[6] == '2' && fread(&has_chief, sizeof(has_chief), 1, file) == 1 && has_chief)
			has_chief = fread(&chief, sizeof(chief), 1, file) == 1;
		else
			has_chief = 0;
		fclose(file);
		goto build;
	}

	for (i = 0; i < (long)(2 * half / 64); i++)
	{
		for (j = 0; j < (long)(2 * half / 64); j++)
		{
			float x0 = -half + i * 64.f, y0 = -half + j * 64.f, x1 = x0 + 64.f, y1 = y0 + 64.f;

			TRI(x0, y0, height(x0, y0), x1, y0, height(x1, y0), x1, y1, height(x1, y1));
			TRI(x0, y0, height(x0, y0), x1, y1, height(x1, y1), x0, y1, height(x0, y1));
		}
	}
	for (i = 0; i < rocks; i++)
	{
		float cx = uniform(-half, half), cy = uniform(-half, half);
		float hx = uniform(20, 120), hy = uniform(20, 120), hz = uniform(10, 80), a = uniform(0, 3.14159f);
		float c[8][3];
		static int const q[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
		long m;

		for (m = 0; m < 8; m++)
		{
			float lx = (m & 1) ? hx : -hx, ly = (m & 2) ? hy : -hy, lz = (m & 4) ? hz : -hz;
			c[m][0] = cx + lx * cosf(a) - ly * sinf(a);
			c[m][1] = cy + lx * sinf(a) + ly * cosf(a);
			c[m][2] = height(cx, cy) + lz;
		}
		for (m = 0; m < 6; m++)
		{
			TRI(c[q[m][0]][0], c[q[m][0]][1], c[q[m][0]][2], c[q[m][1]][0], c[q[m][1]][1], c[q[m][1]][2], c[q[m][2]][0], c[q[m][2]][1], c[q[m][2]][2]);
			TRI(c[q[m][0]][0], c[q[m][0]][1], c[q[m][0]][2], c[q[m][2]][0], c[q[m][2]][1], c[q[m][2]][2], c[q[m][3]][0], c[q[m][3]][1], c[q[m][3]][2]);
		}
	}
	/* what Skyrim's meshes have and plain ground hasn't: two-sided meshes
	(every rock again, reversed), exact repeats, and flat pieces stacked on
	one plane like floorboards and road pieces */
	{
		long rock_triangles = n - (long)(2 * half / 64) * (long)(2 * half / 64) * 2, first = n - rock_triangles, m;

		for (m = 0; m < rock_triangles && n + 2 < capacity; m++)
		{
			struct chiefrim_triangle const *r = &t[first + m];

			TRI(r->v[0].x / k, r->v[0].y / k, r->v[0].z / k, r->v[2].x / k, r->v[2].y / k, r->v[2].z / k, r->v[1].x / k, r->v[1].y / k, r->v[1].z / k);
			if (m % 7 == 0)
				TRI(r->v[0].x / k, r->v[0].y / k, r->v[0].z / k, r->v[1].x / k, r->v[1].y / k, r->v[1].z / k, r->v[2].x / k, r->v[2].y / k, r->v[2].z / k);
		}
	}
	for (i = 0; i < rocks / 2; i++)
	{
		float cx = uniform(-half / 2, half / 2), cy = uniform(-half / 2, half / 2), z = 600.f;
		float hx = uniform(30, 200), hy = uniform(10, 60), a = uniform(0, 3.14159f);
		float ca = cosf(a), sa = sinf(a);
		float x0 = cx - hx * ca + hy * sa, y0 = cy - hx * sa - hy * ca;
		float x1 = cx + hx * ca + hy * sa, y1 = cy + hx * sa - hy * ca;
		float x2 = cx + hx * ca - hy * sa, y2 = cy + hx * sa + hy * ca;
		float x3 = cx - hx * ca - hy * sa, y3 = cy - hx * sa + hy * ca;

		TRI(x0, y0, z, x1, y1, z, x2, y2, z);
		TRI(x0, y0, z, x2, y2, z, x3, y3, z);
	}
	TRI(-2000, -800, 0, 2000, -800, 0, 2000, -800, 400);
	TRI(-2000, -800, 0, 2000, -800, 400, -2000, -800, 400);

build:
	start = now_ms();
	error[0] = 0;
	bsp = chiefrim_bsp_build(t, n, &map_plane, 1, &map, 0, error, sizeof(error));
	if (!bsp)
	{
		printf("FAILED after %.0f ms: %s\n", now_ms() - start, error);
		return 1;
	}
	printf("%ld triangles -> %ld kept, %ld nodes, %ld leaves, %ld references, %ld 2D nodes, %ld edges, depth %ld, overlaps %ld, duplicates %ld, shared %ld: %.0f ms\n",
		n, bsp->triangle_count, bsp->node_count, bsp->leaf_count, bsp->reference_count, bsp->node2d_count,
		bsp->edge_count, bsp->max_depth, bsp->dropped_overlaps, bsp->duplicates, bsp->shared_fragments, now_ms() - start);
	if (has_chief)
		probe_below(bsp, &chief);
	if (getenv("SELF_TEST"))
		self_test(bsp);
	chiefrim_bsp_free(bsp);
	return 0;
}
