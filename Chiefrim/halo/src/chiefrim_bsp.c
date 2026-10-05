/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_BSP.C

A runtime builder of Halo collision BSPs from Skyrim's collision triangles
(Chiefrim/docs/DESIGN.md §5.2). Halo's biped physics needs a real collision
BSP: it keeps the surface it stands on by index and walks that surface's
edges, so loose triangles won't do.

How it is built, the way Halo's own tool builds one, with changes for
Skyrim's open triangle soup:

- The BSP splits space on planes, and polygons that cross a splitting plane
  are CUT into a piece on each side (Sutherland-Hodgman), so nothing is ever
  duplicated: an ordinary tree. Large sets are halved with axis-aligned
  planes (between the polygons' centres, along the longest axis) down to
  cells of up to CHIEFRIM_BSP_CELL polygons; within a cell, splits are on the
  polygons' own planes, chosen for few cuts and balance. Every path is at
  most CHIEFRIM_BSP_AXIS_DEPTH + CHIEFRIM_BSP_CELL deep: each cell split
  takes a plane out for good.
- A polygon on a splitting plane becomes surfaces there, referenced (through
  a 2D BSP of that plane's polygons) from the leaves on each side it reaches.
  Surfaces have at most 8 corners (Halo's limit); bigger pieces are fanned.
- There are no solid leaves (Skyrim's collision has no inside): every leaf
  "contains two-sided" surfaces, so Halo's ray query tests every plane it
  crosses, exactly (collision_bsp.c, the semi-empty case).
- Every polygon is two surfaces, itself and a reversed twin, because the
  sphere query matches a leaf's surfaces to the side of the plane it came
  from. Edges are winged edges shared between neighbours on the same side;
  Halo's edge features read both surfaces of every edge, so none is left
  open: all fronts are made first, then all twins, which close their fronts'
  open edges and pair up among themselves.
- Polygons overlapping on one plane, which no 2D line separates, go in extra
  references to that plane in the same leaf (Halo tries them all).

An earlier version kept triangles whole (no cuts): on Skyrim's meshes that
either duplicated triangles without end or, with node chains instead, made
Halo's sphere query exponential. Cutting is what Halo's tool does too.

Runs on a worker thread: no game state, and the C library's allocator, not
the game's debug one (which isn't thread-safe). tools/test_bsp.sh runs it
offline, with Halo's own queries, under AddressSanitizer.
*/

#include "cseries.h"
#include "chiefrim/chiefrim_bsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#undef malloc
#undef free
#undef realloc
#undef calloc

/* ---------- constants */

#define CHIEFRIM_BSP_EPSILON       0.0005f  /* world units: 0.1 Skyrim unit */
#define CHIEFRIM_BSP_CUT_EPSILON   0.00001f /* fragments: anything more crossing is cut */
#define CHIEFRIM_BSP_PLANE_DOT     0.9999995f /* planes this alike (and this close) are one: */
#define CHIEFRIM_BSP_PLANE_D       0.0005f    /* a triangle moves onto it by a hair */
#define CHIEFRIM_BSP_PLANE_CELL    0.001f     /* about the angle PLANE_DOT allows */
#define CHIEFRIM_BSP_WELD          0.002f   /* vertices this close are one */
#define CHIEFRIM_BSP_CELL          64       /* polygons: up to this many, split on their planes */
#define CHIEFRIM_BSP_AXIS_DEPTH    40       /* axis-aligned splits stop by this depth */
#define CHIEFRIM_BSP_MAXIMUM_DEPTH 120      /* Halo's plane stacks hold 128 */
#define CHIEFRIM_BSP_CANDIDATES    12
#define CHIEFRIM_BSP_SAMPLE        256
#define CHIEFRIM_FRAGMENT_POINTS   16       /* a piece being cut */
#define CHIEFRIM_SURFACE_POINTS    8        /* MAXIMUM_VERTICES_PER_COLLISION_SURFACE */
#define CHIEFRIM_LEAF_TWO_SIDED    0x0001   /* _collision_leaf_contains_two_sided_bit */
#define CHIEFRIM_BSP_OVERLAP_ROUNDS 8  /* references to one plane in one leaf, at most */
#define CHIEFRIM_TWIN              0x40000000L /* in pending lists: the emitted polygon's twin */

/* ---------- structures */

/* A polygon on its way down the BSP. */
struct fragment
{
	real_point3d p[CHIEFRIM_FRAGMENT_POINTS];
	long count;
	long plane;       /* index; its normal is the plane's, or negated */
	boolean flipped;
	unsigned long id;
};

/* A polygon that has become surfaces (its front, and its twin). */
struct emitted
{
	real_point3d p[CHIEFRIM_SURFACE_POINTS];
	long count;
	long designator;  /* the front's: plane | (flipped ? sign : 0) */
	unsigned long id;
};

struct pending
{
	long designator;
	long *surfaces;   /* emitted index, | CHIEFRIM_TWIN for its twin */
	long count;
};

struct projected
{
	long surface;     /* as in pending */
	real_point2d p[CHIEFRIM_SURFACE_POINTS];
	long count;
};

struct hash_entry
{
	unsigned long key[3];
	long value;
	boolean used;
};

struct hash_table
{
	struct hash_entry *entries;
	long capacity;   /* power of two */
	long count;
};

struct builder
{
	struct chiefrim_bsp *out;
	long plane_capacity, node_capacity, leaf_capacity, reference_capacity;
	long node2d_capacity, surface_capacity, surface_id_capacity, edge_capacity, vertex_capacity;

	struct fragment *fragments;
	long fragment_count, fragment_capacity;
	struct emitted *emitted;
	long emitted_count, emitted_capacity;

	short material;
	struct hash_table plane_table;
	struct hash_table vertex_table;
	struct hash_table edge_table;
	struct hash_table triangle_table;
	boolean failed;
	char const *failure;

	struct projected *overflow;
	long overflow_count, overflow_capacity;
};

/* ---------- helpers */

static void fail(struct builder *b, char const *why)
{
	if (!b->failed)
		b->failure = why;
	b->failed = TRUE;
}

static void *grow(struct builder *b, void *array, long *capacity, long needed, long element_size)
{
	long capacity_new;
	void *result;

	if (needed <= *capacity)
		return array;
	capacity_new = *capacity ? *capacity : 64;
	while (capacity_new < needed)
		capacity_new *= 2;
	result = realloc(array, (size_t)capacity_new * (size_t)element_size);
	if (!result)
	{
		fail(b, "out of memory");
		return array;
	}
	*capacity = capacity_new;
	return result;
}

#define GROW(b, field, capacity, needed) \
	((b)->out->field = grow((b), (b)->out->field, &(b)->capacity, (needed), (long)sizeof(*(b)->out->field)))

static unsigned long hash_key(unsigned long const key[3])
{
	unsigned long h = key[0] * 73856093u ^ key[1] * 19349663u ^ key[2] * 83492791u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	return h ^ (h >> 15);
}

static boolean hash_init(struct hash_table *table, long expected)
{
	long capacity = 64;

	while (capacity < expected * 2)
		capacity *= 2;
	table->entries = (struct hash_entry *)calloc((size_t)capacity, sizeof(struct hash_entry));
	table->capacity = capacity;
	table->count = 0;
	return table->entries != NULL;
}

static void hash_dispose(struct hash_table *table)
{
	free(table->entries);
	table->entries = NULL;
}

/* Grows the table when it is half full (keys are re-inserted). */
static boolean hash_reserve(struct hash_table *table)
{
	struct hash_table bigger;
	long i;

	if (table->count * 2 < table->capacity)
		return TRUE;
	if (!hash_init(&bigger, table->capacity))
		return FALSE;
	for (i = 0; i < table->capacity; i++)
	{
		struct hash_entry const *entry = &table->entries[i];
		unsigned long index;

		if (!entry->used)
			continue;
		index = hash_key(entry->key) & (unsigned long)(bigger.capacity - 1);
		while (bigger.entries[index].used)
			index = (index + 1) & (unsigned long)(bigger.capacity - 1);
		bigger.entries[index] = *entry;
		bigger.count++;
	}
	free(table->entries);
	*table = bigger;
	return TRUE;
}

/* The slot of key: its entry if present, else the empty slot for it. */
static struct hash_entry *hash_slot(struct hash_table *table, unsigned long const key[3])
{
	unsigned long index = hash_key(key) & (unsigned long)(table->capacity - 1);

	for (;;)
	{
		struct hash_entry *entry = &table->entries[index];

		if (!entry->used ||
			(entry->key[0] == key[0] && entry->key[1] == key[1] && entry->key[2] == key[2]))
		{
			return entry;
		}
		index = (index + 1) & (unsigned long)(table->capacity - 1);
	}
}

static void hash_put(struct hash_entry *entry, unsigned long const key[3], long value, struct hash_table *table)
{
	if (!entry->used)
		table->count++;
	entry->used = TRUE;
	entry->key[0] = key[0];
	entry->key[1] = key[1];
	entry->key[2] = key[2];
	entry->value = value;
}

static real plane_distance(real_plane3d const *plane, real_point3d const *point)
{
	return plane->n.i * point->x + plane->n.j * point->y + plane->n.k * point->z - plane->d;
}

static unsigned long quantize(real value, real step)
{
	return (unsigned long)(long)floorf(value / step + 0.5f);
}

/* ---------- planes and vertices */

/* Planes hash by their normal and distance in cells as big as the merge
tolerances; a lookup tries the neighbouring cells too, so planes this alike
merge wherever the cell boundaries fall. */
static void plane_cells(real_plane3d const *plane, real sign, long cells[4])
{
	cells[0] = (long)floorf(plane->n.i * sign / CHIEFRIM_BSP_PLANE_CELL);
	cells[1] = (long)floorf(plane->n.j * sign / CHIEFRIM_BSP_PLANE_CELL);
	cells[2] = (long)floorf(plane->n.k * sign / CHIEFRIM_BSP_PLANE_CELL);
	cells[3] = (long)floorf(plane->d * sign / CHIEFRIM_BSP_PLANE_D);
}

static void plane_key(long const cells[4], unsigned long key[3])
{
	key[0] = (unsigned long)cells[0] * 2048u + (unsigned long)cells[1];
	key[1] = (unsigned long)cells[2];
	key[2] = (unsigned long)cells[3];
}

/* The plane's index, | LONG_MIN if it is a known plane negated; NONE if new. */
static long find_plane(struct builder *b, real_plane3d const *plane)
{
	long side, offset;

	for (side = 0; side < 2; side++)
	{
		real sign = side ? -1.f : 1.f;
		long base[4];

		plane_cells(plane, sign, base);
		for (offset = 0; offset < 81; offset++)
		{
			long cells[4], k, o = offset;
			unsigned long key[3];
			struct hash_entry *entry;

			for (k = 0; k < 4; k++, o /= 3)
				cells[k] = base[k] + (o % 3) - 1;
			plane_key(cells, key);
			entry = hash_slot(&b->plane_table, key);
			if (entry->used)
			{
				real_plane3d const *found = &b->out->planes[entry->value];

				if (found->n.i * plane->n.i * sign + found->n.j * plane->n.j * sign + found->n.k * plane->n.k * sign > CHIEFRIM_BSP_PLANE_DOT &&
					fabsf(found->d - plane->d * sign) < CHIEFRIM_BSP_PLANE_D)
				{
					return side ? (entry->value | LONG_MIN) : entry->value;
				}
			}
		}
	}
	return NONE;
}

static long add_plane(struct builder *b, real_plane3d const *plane)
{
	long found = find_plane(b, plane);
	unsigned long key[3];
	struct hash_entry *entry;
	long index;

	if (found != NONE)
		return found;
	index = b->out->plane_count;
	GROW(b, planes, plane_capacity, index + 1);
	if (b->failed || !hash_reserve(&b->plane_table))
	{
		fail(b, "out of memory");
		return NONE;
	}
	b->out->planes[index] = *plane;
	b->out->plane_count++;
	{
		long cells[4];

		plane_cells(plane, 1.f, cells);
		plane_key(cells, key);
	}
	entry = hash_slot(&b->plane_table, key);
	if (!entry->used)
		hash_put(entry, key, index, &b->plane_table);
	return index;
}

static long add_vertex(struct builder *b, real_point3d const *point)
{
	unsigned long key[3];
	struct hash_entry *entry;
	long index;

	if (!hash_reserve(&b->vertex_table))
	{
		fail(b, "out of memory");
		return NONE;
	}
	key[0] = quantize(point->x, CHIEFRIM_BSP_WELD);
	key[1] = quantize(point->y, CHIEFRIM_BSP_WELD);
	key[2] = quantize(point->z, CHIEFRIM_BSP_WELD);
	entry = hash_slot(&b->vertex_table, key);
	if (entry->used)
		return entry->value;
	index = b->out->vertex_count;
	GROW(b, vertices, vertex_capacity, index + 1);
	if (b->failed)
		return NONE;
	b->out->vertices[index].point = *point;
	b->out->vertices[index].first_edge_index = NONE;
	b->out->vertex_count++;
	hash_put(entry, key, index, &b->vertex_table);
	return index;
}

/* ---------- surfaces and winged edges */

/* The edge u -> w of surface s: the open edge w -> u of a neighbour if there
is one, else a new edge. *side is the side s is on. */
static long add_edge(struct builder *b, long u, long w, long s, short *side)
{
	unsigned long key[3];
	struct hash_entry *entry;
	long index;

	if (!hash_reserve(&b->edge_table))
	{
		fail(b, "out of memory");
		return NONE;
	}
	key[0] = (unsigned long)MIN(u, w);
	key[1] = (unsigned long)MAX(u, w);
	key[2] = 0;
	entry = hash_slot(&b->edge_table, key);
	if (entry->used)
	{
		struct collision_edge *edge = &b->out->edges[entry->value];

		if (edge->surface_indices[1] == NONE && edge->vertex_indices[0] == w && edge->vertex_indices[1] == u)
		{
			edge->surface_indices[1] = s;
			*side = 1;
			return entry->value;
		}
	}

	index = b->out->edge_count;
	GROW(b, edges, edge_capacity, index + 1);
	if (b->failed)
		return NONE;
	{
		struct collision_edge *edge = &b->out->edges[index];

		edge->vertex_indices[0] = u;
		edge->vertex_indices[1] = w;
		edge->edge_indices[0] = NONE;
		edge->edge_indices[1] = NONE;
		edge->surface_indices[0] = s;
		edge->surface_indices[1] = NONE;
	}
	b->out->edge_count++;
	hash_put(entry, key, index, &b->edge_table);
	if (b->out->vertices[u].first_edge_index == NONE)
		b->out->vertices[u].first_edge_index = index;
	if (b->out->vertices[w].first_edge_index == NONE)
		b->out->vertices[w].first_edge_index = index;
	*side = 0;
	return index;
}

static void add_surface(struct builder *b, long designator, long const *vertex, long count, boolean twin, unsigned long id)
{
	long s = b->out->surface_count;
	long edges[CHIEFRIM_SURFACE_POINTS];
	short sides[CHIEFRIM_SURFACE_POINTS];
	long k;

	GROW(b, surfaces, surface_capacity, s + 1);
	b->out->surface_ids = grow(b, b->out->surface_ids, &b->surface_id_capacity, s + 1, (long)sizeof(unsigned long));
	if (b->failed)
		return;
	b->out->surface_count++;
	b->out->surface_ids[s] = twin ? (id | 0x80000000u) : (id & 0x7FFFFFFFu);

	for (k = 0; k < count; k++)
	{
		edges[k] = add_edge(b, vertex[k], vertex[(k + 1) % count], s, &sides[k]);
		if (b->failed)
			return;
	}
	for (k = 0; k < count; k++)
		b->out->edges[edges[k]].edge_indices[sides[k]] = edges[(k + 1) % count];

	b->out->surfaces[s].plane_designator = designator;
	b->out->surfaces[s].first_edge_index = edges[0];
	b->out->surfaces[s].flags = 0;
	b->out->surfaces[s].breakable_surface_index = (byte)NONE;
	b->out->surfaces[s].material_index = b->material;
}

/* The final surface index of a pending code. */
static long surface_of(struct builder const *b, long code)
{
	return (code & CHIEFRIM_TWIN) ? b->emitted_count + (code & ~CHIEFRIM_TWIN) : code;
}

/* ---------- polygons */

static boolean polygon_plane(real_point3d const *p, long count, real_plane3d *plane)
{
	real_vector3d n = { 0.f, 0.f, 0.f };
	real length;
	long i;

	/* Newell's normal: robust for slightly bent polygons */
	for (i = 0; i < count; i++)
	{
		real_point3d const *a = &p[i], *z = &p[(i + 1) % count];

		n.i += (a->y - z->y) * (a->z + z->z);
		n.j += (a->z - z->z) * (a->x + z->x);
		n.k += (a->x - z->x) * (a->y + z->y);
	}
	length = sqrtf(n.i * n.i + n.j * n.j + n.k * n.k);
	if (!(length > 1e-7f) || !(length < 1e30f))
		return FALSE;
	plane->n.i = n.i / length;
	plane->n.j = n.j / length;
	plane->n.k = n.k / length;
	plane->d = plane->n.i * p[0].x + plane->n.j * p[0].y + plane->n.k * p[0].z;
	return TRUE;
}

static long new_fragment(struct builder *b)
{
	b->fragments = grow(b, b->fragments, &b->fragment_capacity, b->fragment_count + 1, (long)sizeof(struct fragment));
	return b->failed ? NONE : b->fragment_count++;
}

/* Cuts fragment f by the plane: the pieces in front and behind (NONE when
empty or degenerate). */
static void cut(struct builder *b, long f, real_plane3d const *plane, long *front, long *back)
{
	real_point3d in_front[CHIEFRIM_FRAGMENT_POINTS + 2], behind[CHIEFRIM_FRAGMENT_POINTS + 2];
	long front_count = 0, back_count = 0, i;
	struct fragment source = b->fragments[f]; /* the array may move */

	for (i = 0; i < source.count; i++)
	{
		real_point3d const *a = &source.p[i], *z = &source.p[(i + 1) % source.count];
		real da = plane_distance(plane, a), dz = plane_distance(plane, z);

		if (da >= 0.f)
			in_front[front_count++] = *a;
		if (da <= 0.f)
			behind[back_count++] = *a;
		if ((da > 0.f && dz < 0.f) || (da < 0.f && dz > 0.f))
		{
			real t = da / (da - dz);
			real_point3d m;

			m.x = a->x + (z->x - a->x) * t;
			m.y = a->y + (z->y - a->y) * t;
			m.z = a->z + (z->z - a->z) * t;
			in_front[front_count++] = m;
			behind[back_count++] = m;
		}
	}

	*front = *back = NONE;
	{
		real_point3d *pieces[2] = { in_front, behind };
		long counts[2] = { front_count, back_count };
		long *results[2] = { front, back };
		long side;

		for (side = 0; side < 2; side++)
		{
			long piece;
			real_plane3d check;

			/* a convex polygon cut once gains at most one corner a side */
			if (counts[side] > CHIEFRIM_FRAGMENT_POINTS)
				counts[side] = CHIEFRIM_FRAGMENT_POINTS;
			if (counts[side] < 3 || !polygon_plane(pieces[side], counts[side], &check))
				continue;
			piece = new_fragment(b);
			if (piece == NONE)
				return;
			b->fragments[piece] = source;
			memcpy(b->fragments[piece].p, pieces[side], sizeof(real_point3d) * (size_t)counts[side]);
			b->fragments[piece].count = counts[side];
			*results[side] = piece;
		}
	}
}

/* Fragment f lies on a splitting plane: it becomes surfaces, fanned into
pieces of up to CHIEFRIM_SURFACE_POINTS corners. Appends the emitted indices
to out (room for CHIEFRIM_FRAGMENT_POINTS). */
static void emit(struct builder *b, long f, long *out, long *out_count)
{
	struct fragment const source = b->fragments[f];
	long start = 1;

	while (start + 1 < source.count && !b->failed)
	{
		long take = MIN(source.count - start, CHIEFRIM_SURFACE_POINTS - 1);
		long e, k;

		b->emitted = grow(b, b->emitted, &b->emitted_capacity, b->emitted_count + 1, (long)sizeof(struct emitted));
		if (b->failed)
			return;
		e = b->emitted_count++;
		b->emitted[e].p[0] = source.p[0];
		for (k = 0; k < take; k++)
			b->emitted[e].p[k + 1] = source.p[start + k];
		b->emitted[e].count = take + 1;
		b->emitted[e].designator = source.plane | (source.flipped ? LONG_MIN : 0);
		b->emitted[e].id = source.id;
		out[(*out_count)++] = e;
		if (start + take >= source.count)
			break;
		start += take - 1; /* the next piece starts at this piece's last corner */
	}
}

/* The emitted polygon (or its twin, reversed) a pending code names. */
static long polygon_of(struct builder const *b, long code, real_point3d *points)
{
	struct emitted const *e = &b->emitted[code & ~CHIEFRIM_TWIN];
	long i;

	for (i = 0; i < e->count; i++)
		points[i] = (code & CHIEFRIM_TWIN) ? e->p[(e->count - i) % e->count] : e->p[i];
	return e->count;
}

/* The code of an emitted polygon's surface whose designator has this sign. */
static long code_for(struct builder const *b, long e, boolean negative)
{
	boolean flipped = (b->emitted[e].designator & LONG_MIN) != 0;

	return negative == flipped ? e : (e | CHIEFRIM_TWIN);
}

/* ---------- 2D BSPs (one per leaf reference) */

static int compare_projected(void const *a, void const *z)
{
	long x = ((struct projected const *)a)->surface, y = ((struct projected const *)z)->surface;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void add_overflow(struct builder *b, struct projected const *items, long count)
{
	long i;

	b->overflow = grow(b, b->overflow, &b->overflow_capacity, b->overflow_count + count, (long)sizeof(struct projected));
	if (b->failed)
		return;
	for (i = 0; i < count; i++)
		b->overflow[b->overflow_count++] = items[i];
}

static void line_extent(struct projected const *item, real nx, real ny, real_point2d const *a, real *mn, real *mx)
{
	long v;

	*mn = REAL_MAX;
	*mx = -REAL_MAX;
	for (v = 0; v < item->count; v++)
	{
		real d = nx * (item->p[v].x - a->x) + ny * (item->p[v].y - a->y);

		*mn = MIN(*mn, d);
		*mx = MAX(*mx, d);
	}
}

static long build_2d(struct builder *b, struct projected *items, long count, long depth)
{
	long best_item = NONE, best_edge = 0, best_score = LONG_MAX;
	long candidates = MIN(count, 64);
	long c;

	if (count == 1 || depth > 64)
	{
		if (count > 1)
			add_overflow(b, items + 1, count - 1);
		return items[0].surface | LONG_MIN;
	}

	/* a split line along an edge of one of the polygons, with polygons on
	both sides */
	for (c = 0; c < candidates; c++)
	{
		long item = c * count / candidates;
		long e;

		for (e = 0; e < items[item].count; e++)
		{
			real_point2d const *a = &items[item].p[e];
			real_point2d const *z = &items[item].p[(e + 1) % items[item].count];
			real nx = -(z->y - a->y), ny = z->x - a->x;
			real length = sqrtf(nx * nx + ny * ny);
			long front = 0, back = 0, span = 0, i;

			if (length < 1e-6f)
				continue;
			nx /= length;
			ny /= length;
			for (i = 0; i < count; i++)
			{
				real mn, mx;

				line_extent(&items[i], nx, ny, a, &mn, &mx);
				if (mn >= -CHIEFRIM_BSP_EPSILON)
					front++;
				else if (mx <= CHIEFRIM_BSP_EPSILON)
					back++;
				else
					span++;
			}
			if (front + span < count && back + span < count)
			{
				long score = span * 4 + labs(front - back);

				if (score < best_score)
				{
					best_score = score;
					best_item = item;
					best_edge = e;
				}
			}
		}
	}

	if (best_item == NONE)
	{
		/* overlapping on this plane: no line separates them */
		add_overflow(b, items + 1, count - 1);
		return items[0].surface | LONG_MIN;
	}

	{
		real_point2d const a = items[best_item].p[best_edge];
		real_point2d const z = items[best_item].p[(best_edge + 1) % items[best_item].count];
		real nx = -(z.y - a.y), ny = z.x - a.x;
		real length = sqrtf(nx * nx + ny * ny);
		struct projected *front = (struct projected *)malloc(sizeof(struct projected) * (size_t)count);
		struct projected *back = (struct projected *)malloc(sizeof(struct projected) * (size_t)count);
		long front_count = 0, back_count = 0, node, i;

		if (!front || !back)
		{
			free(front);
			free(back);
			fail(b, "out of memory");
			return items[0].surface | LONG_MIN;
		}
		nx /= length;
		ny /= length;
		for (i = 0; i < count; i++)
		{
			real mn, mx;

			line_extent(&items[i], nx, ny, &a, &mn, &mx);
			if (mn >= -CHIEFRIM_BSP_EPSILON)
				front[front_count++] = items[i];
			else if (mx <= CHIEFRIM_BSP_EPSILON)
				back[back_count++] = items[i];
			else
			{
				front[front_count++] = items[i];
				back[back_count++] = items[i];
			}
		}

		node = b->out->node2d_count;
		GROW(b, nodes2d, node2d_capacity, node + 1);
		if (b->failed)
		{
			free(front);
			free(back);
			return items[0].surface | LONG_MIN;
		}
		b->out->node2d_count++;
		b->out->nodes2d[node].plane.n.i = nx;
		b->out->nodes2d[node].plane.n.j = ny;
		b->out->nodes2d[node].plane.d = nx * a.x + ny * a.y;
		{
			/* children[distance >= 0]: the front, then the back; both sides
			are non-empty by the choice above */
			long child_back = build_2d(b, back, back_count, depth + 1);
			long child_front = build_2d(b, front, front_count, depth + 1);

			b->out->nodes2d[node].child_indices[0] = child_back;
			b->out->nodes2d[node].child_indices[1] = child_front;
		}
		free(front);
		free(back);
		return node;
	}
}

/* A pending entry's polygons, projected as collision_bsp.c projects for
this reference. NULL if out of memory. */
static struct projected *project_entry(struct builder *b, struct pending const *entry)
{
	real_plane3d const *plane = &b->out->planes[entry->designator & LONG_MAX];
	real absolute_i = fabsf(plane->n.i), absolute_j = fabsf(plane->n.j), absolute_k = fabsf(plane->n.k);
	short projection;
	boolean sign;
	struct projected *items;
	long i;

	if (absolute_k >= absolute_j && absolute_k >= absolute_i)
		projection = _z;
	else
		projection = absolute_j >= absolute_i ? _y : _x;
	sign = projection_sign_from_vector3d(&plane->n, projection) != (entry->designator & LONG_MIN ? TRUE : FALSE);

	items = (struct projected *)malloc(sizeof(struct projected) * (size_t)entry->count);
	if (!items)
	{
		fail(b, "out of memory");
		return NULL;
	}
	for (i = 0; i < entry->count; i++)
	{
		real_point3d points[CHIEFRIM_SURFACE_POINTS];
		long count = polygon_of(b, entry->surfaces[i], points), v;

		items[i].surface = entry->surfaces[i];
		items[i].count = count;
		for (v = 0; v < count; v++)
			project_point3d(&points[v], projection, sign, &items[i].p[v]);
	}
	return items;
}

/* ---------- the 3D BSP */

static long make_leaf(struct builder *b, struct pending const *pending, long pending_count)
{
	long leaf = b->out->leaf_count;
	long first = b->out->reference_count;
	long count = 0, p, rounds;

	GROW(b, leaves, leaf_capacity, leaf + 1);
	if (b->failed)
		return NONE;
	b->out->leaf_count++;

	for (p = 0; p < pending_count; p++)
	{
		struct projected *items;
		long item_count = pending[p].count;

		if (item_count == 0)
			continue;
		items = project_entry(b, &pending[p]);
		if (!items)
			return NONE;
		/* one reference, and another to the same plane for whatever overlaps
		on it */
		rounds = 0;
		while (item_count > 0 && !b->failed)
		{
			long root, reference;

			b->overflow_count = 0;
			root = build_2d(b, items, item_count, 0);
			reference = b->out->reference_count;
			GROW(b, references, reference_capacity, reference + 1);
			if (b->failed)
				break;
			b->out->reference_count++;
			b->out->references[reference].plane_designator = pending[p].designator;
			b->out->references[reference].root_index = root; /* codes: fixed up at the end */
			count++;
			/* a polygon spanning a 2D split can overflow on both sides:
			once each, so the next round is no bigger than this one */
			qsort(b->overflow, (size_t)b->overflow_count, sizeof(struct projected), compare_projected);
			{
				long unique = 0, i;

				for (i = 0; i < b->overflow_count; i++)
				{
					if (unique == 0 || b->overflow[i].surface != items[unique - 1].surface)
						items[unique++] = b->overflow[i];
				}
				b->out->dropped_overlaps += unique;
				item_count = unique;
			}
			if (++rounds >= CHIEFRIM_BSP_OVERLAP_ROUNDS)
				break; /* stacked deeper than this: the rest go untested */
		}
		free(items);
		if (b->failed)
			return NONE;
	}

	b->out->leaves[leaf].flags = CHIEFRIM_LEAF_TWO_SIDED;
	b->out->leaves[leaf].bsp2d_reference_count = (short)MIN(count, SHORT_MAX);
	b->out->leaves[leaf].first_bsp2d_reference_index = first;
	return leaf;
}

static void fragment_extent(struct fragment const *f, real_plane3d const *plane, real *mn, real *mx)
{
	long i;

	*mn = REAL_MAX;
	*mx = -REAL_MAX;
	for (i = 0; i < f->count; i++)
	{
		real d = plane_distance(plane, &f->p[i]);

		*mn = MIN(*mn, d);
		*mx = MAX(*mx, d);
	}
}

static int compare_reals(void const *a, void const *z)
{
	real x = *(real const *)a, y = *(real const *)z;

	return x < y ? -1 : x > y ? 1 : 0;
}

static real fragment_centre(struct fragment const *f, long axis)
{
	real c = 0.f;
	long v;

	for (v = 0; v < f->count; v++)
		c += f->p[v].n[axis];
	return c / (real)f->count;
}

/* An axis-aligned plane halving a large set: the longest axis of the
fragments' centres, halfway between the two middle centres. NONE if the
centres don't spread. */
static long choose_axis_plane(struct builder *b, long const *fragments, long count)
{
	real lo[3] = { REAL_MAX, REAL_MAX, REAL_MAX }, hi[3] = { -REAL_MAX, -REAL_MAX, -REAL_MAX };
	long sample = MIN(count, 1024);
	real *centres;
	long axis = 0, i, k;
	real_plane3d plane;
	long index;

	for (i = 0; i < count; i++)
	{
		for (k = 0; k < 3; k++)
		{
			real c = fragment_centre(&b->fragments[fragments[i]], k);

			lo[k] = MIN(lo[k], c);
			hi[k] = MAX(hi[k], c);
		}
	}
	for (k = 1; k < 3; k++)
	{
		if (hi[k] - lo[k] > hi[axis] - lo[axis])
			axis = k;
	}
	if (hi[axis] - lo[axis] < 0.01f)
		return NONE;

	centres = (real *)malloc(sizeof(real) * (size_t)sample);
	if (!centres)
	{
		fail(b, "out of memory");
		return NONE;
	}
	for (i = 0; i < sample; i++)
		centres[i] = fragment_centre(&b->fragments[fragments[i * count / sample]], axis);
	qsort(centres, (size_t)sample, sizeof(real), compare_reals);
	plane.n.i = axis == 0 ? 1.f : 0.f;
	plane.n.j = axis == 1 ? 1.f : 0.f;
	plane.n.k = axis == 2 ? 1.f : 0.f;
	plane.d = sample > 1 ? (centres[sample / 2 - 1] + centres[sample / 2]) * 0.5f : centres[0];
	free(centres);
	if (!(plane.d > lo[axis] && plane.d < hi[axis]))
		plane.d = (lo[axis] + hi[axis]) * 0.5f;

	index = add_plane(b, &plane);
	return index == NONE ? NONE : (index & LONG_MAX);
}

/* One of the fragments' own planes: the one with the fewest cuts and the
best balance on a sample. */
static long choose_fragment_plane(struct builder *b, long const *fragments, long count)
{
	long best_plane = b->fragments[fragments[0]].plane;
	long best_score = LONG_MAX;
	long candidates = MIN(count, CHIEFRIM_BSP_CANDIDATES);
	long sample = MIN(count, CHIEFRIM_BSP_SAMPLE);
	long c;

	for (c = 0; c < candidates; c++)
	{
		long plane_index = b->fragments[fragments[c * count / candidates]].plane;
		real_plane3d const *plane = &b->out->planes[plane_index];
		long front = 0, back = 0, span = 0, i;

		for (i = 0; i < sample; i++)
		{
			struct fragment const *f = &b->fragments[fragments[i * count / sample]];
			real mn, mx;

			if (f->plane == plane_index)
				continue;
			fragment_extent(f, plane, &mn, &mx);
			if (mn >= -CHIEFRIM_BSP_EPSILON)
				front++;
			else if (mx <= CHIEFRIM_BSP_EPSILON)
				back++;
			else
				span++;
		}
		{
			long score = span * 3 + labs(front - back);

			if (score < best_score)
			{
				best_score = score;
				best_plane = plane_index;
			}
		}
	}
	return best_plane;
}

/* Keeps the pending surfaces that reach the given side of plane. */
static boolean filter_pending(struct builder *b, struct pending const *in, long in_count,
	real_plane3d const *plane, boolean front, struct pending *out)
{
	long p;

	for (p = 0; p < in_count; p++)
	{
		long i;

		out[p].designator = in[p].designator;
		out[p].count = 0;
		out[p].surfaces = (long *)malloc(sizeof(long) * (size_t)MAX(in[p].count, 1));
		if (!out[p].surfaces)
		{
			fail(b, "out of memory");
			return FALSE;
		}
		for (i = 0; i < in[p].count; i++)
		{
			struct emitted const *e = &b->emitted[in[p].surfaces[i] & ~CHIEFRIM_TWIN];
			real mn = REAL_MAX, mx = -REAL_MAX;
			long v;

			for (v = 0; v < e->count; v++)
			{
				real d = plane_distance(plane, &e->p[v]);

				mn = MIN(mn, d);
				mx = MAX(mx, d);
			}
			if (front ? mx >= -CHIEFRIM_BSP_EPSILON : mn <= CHIEFRIM_BSP_EPSILON)
				out[p].surfaces[out[p].count++] = in[p].surfaces[i];
		}
	}
	return TRUE;
}

static void free_pending(struct pending *pending, long count)
{
	long p;

	if (!pending)
		return;
	for (p = 0; p < count; p++)
		free(pending[p].surfaces);
	free(pending);
}

static long build_3d(struct builder *b, long const *fragments, long count,
	struct pending const *pending, long pending_count, long depth)
{
	long plane_index = NONE, node = NONE, i;
	real_plane3d plane;
	long *front = NULL, *back = NULL, *on = NULL;
	long front_count = 0, back_count = 0, on_count = 0;
	struct pending *front_pending = NULL, *back_pending = NULL;
	long child_front, child_back;

	if (b->failed)
		return NONE;
	b->out->max_depth = MAX(b->out->max_depth, depth);
	if (count == 0)
	{
		long leaf = make_leaf(b, pending, pending_count);
		return leaf == NONE ? NONE : (leaf | LONG_MIN);
	}
	if (depth >= CHIEFRIM_BSP_MAXIMUM_DEPTH)
	{
		fail(b, "past the depth limit");
		return NONE;
	}

	if (count > CHIEFRIM_BSP_CELL && depth < CHIEFRIM_BSP_AXIS_DEPTH)
		plane_index = choose_axis_plane(b, fragments, count);
	if (b->failed)
		return NONE;
	if (plane_index == NONE)
		plane_index = choose_fragment_plane(b, fragments, count);
	plane = b->out->planes[plane_index];

	/* every fragment yields at most one piece a side, and at most
	CHIEFRIM_FRAGMENT_POINTS emitted pieces */
	front = (long *)malloc(sizeof(long) * (size_t)count);
	back = (long *)malloc(sizeof(long) * (size_t)count);
	on = (long *)malloc(sizeof(long) * (size_t)count * CHIEFRIM_FRAGMENT_POINTS);
	front_pending = (struct pending *)calloc((size_t)pending_count + 1, sizeof(struct pending));
	back_pending = (struct pending *)calloc((size_t)pending_count + 1, sizeof(struct pending));
	if (!front || !back || !on || !front_pending || !back_pending)
	{
		fail(b, "out of memory");
		goto done;
	}

	for (i = 0; i < count && !b->failed; i++)
	{
		long f = fragments[i];
		real mn, mx;

		if (b->fragments[f].plane == plane_index)
		{
			emit(b, f, on, &on_count);
			continue;
		}
		/* nearly strict: a fragment kept whole on one side must not reach
		the other, or a ray meeting it there (in the other side's leaves)
		would miss it */
		fragment_extent(&b->fragments[f], &plane, &mn, &mx);
		if (mn >= -CHIEFRIM_BSP_CUT_EPSILON)
			front[front_count++] = f;
		else if (mx <= CHIEFRIM_BSP_CUT_EPSILON)
			back[back_count++] = f;
		else
		{
			long piece_front, piece_back;

			cut(b, f, &plane, &piece_front, &piece_back);
			if (piece_front != NONE)
				front[front_count++] = piece_front;
			if (piece_back != NONE)
				back[back_count++] = piece_back;
		}
	}
	if (b->failed)
		goto done;

	/* the polygons on this plane: in front, the surfaces facing it; behind,
	the ones facing away (each polygon has one of each) */
	if (!filter_pending(b, pending, pending_count, &plane, TRUE, front_pending) ||
		!filter_pending(b, pending, pending_count, &plane, FALSE, back_pending))
	{
		goto done;
	}
	front_pending[pending_count].designator = plane_index;
	back_pending[pending_count].designator = plane_index | LONG_MIN;
	front_pending[pending_count].surfaces = (long *)malloc(sizeof(long) * (size_t)MAX(on_count, 1));
	back_pending[pending_count].surfaces = (long *)malloc(sizeof(long) * (size_t)MAX(on_count, 1));
	if (!front_pending[pending_count].surfaces || !back_pending[pending_count].surfaces)
	{
		fail(b, "out of memory");
		goto done;
	}
	for (i = 0; i < on_count; i++)
	{
		front_pending[pending_count].surfaces[i] = code_for(b, on[i], FALSE);
		back_pending[pending_count].surfaces[i] = code_for(b, on[i], TRUE);
	}
	front_pending[pending_count].count = on_count;
	back_pending[pending_count].count = on_count;

	node = b->out->node_count;
	GROW(b, nodes, node_capacity, node + 1);
	if (b->failed)
		goto done;
	b->out->node_count++;
	b->out->nodes[node].plane_designator = plane_index;

	child_front = build_3d(b, front, front_count, front_pending, pending_count + 1, depth + 1);
	child_back = build_3d(b, back, back_count, back_pending, pending_count + 1, depth + 1);
	if (!b->failed)
	{
		b->out->nodes[node].children[1] = child_front; /* distance >= 0 */
		b->out->nodes[node].children[0] = child_back;
	}

done:
	free(front);
	free(back);
	free(on);
	free_pending(front_pending, pending_count + 1);
	free_pending(back_pending, pending_count + 1);
	return b->failed ? NONE : node;
}

/* 2D BSP leaves and references hold pending codes until the surfaces exist:
now they become surface indices. */
static void fix_codes(struct builder *b)
{
	long i;

	for (i = 0; i < b->out->node2d_count; i++)
	{
		long k;

		for (k = 0; k < 2; k++)
		{
			long *child = &b->out->nodes2d[i].child_indices[k];

			if (*child & LONG_MIN)
				*child = surface_of(b, *child & LONG_MAX) | LONG_MIN;
		}
	}
	for (i = 0; i < b->out->reference_count; i++)
	{
		long *root = &b->out->references[i].root_index;

		if (*root & LONG_MIN)
			*root = surface_of(b, *root & LONG_MAX) | LONG_MIN;
	}
}

/* ---------- duplicate triangles */

static unsigned long vertex_key(real_point3d const *point)
{
	unsigned long key[3];

	key[0] = quantize(point->x, CHIEFRIM_BSP_WELD);
	key[1] = quantize(point->y, CHIEFRIM_BSP_WELD);
	key[2] = quantize(point->z, CHIEFRIM_BSP_WELD);
	return hash_key(key) ^ key[0] * 2654435761u;
}

static void sort3(unsigned long k[3])
{
	unsigned long t;

	if (k[0] > k[1]) { t = k[0]; k[0] = k[1]; k[1] = t; }
	if (k[1] > k[2]) { t = k[1]; k[1] = k[2]; k[2] = t; }
	if (k[0] > k[1]) { t = k[0]; k[0] = k[1]; k[1] = t; }
}

/* TRUE if the same three corners (in any order: either winding) came
before. Skyrim's two-sided meshes repeat triangles reversed, and every
triangle gets a reversed twin anyway; a repeat would only overlap itself. */
static boolean duplicate_triangle(struct builder *b, struct chiefrim_triangle const *in)
{
	unsigned long key[3];
	struct hash_entry *entry;

	if (!hash_reserve(&b->triangle_table))
	{
		fail(b, "out of memory");
		return FALSE;
	}
	key[0] = vertex_key(&in->v[0]);
	key[1] = vertex_key(&in->v[1]);
	key[2] = vertex_key(&in->v[2]);
	sort3(key);
	entry = hash_slot(&b->triangle_table, key);
	if (entry->used)
		return TRUE;
	hash_put(entry, key, 0, &b->triangle_table);
	return FALSE;
}

/* ---------- public code */

struct chiefrim_bsp *chiefrim_bsp_build(
	struct chiefrim_triangle const *triangles,
	long triangle_count,
	real_plane3d const *map_planes,
	long map_plane_count,
	struct structure_bsp const *map_structure,
	short default_material,
	char *error,
	long error_size)
{
	struct builder b;
	long *order = NULL;
	long i;

	memset(&b, 0, sizeof(b));
	b.out = (struct chiefrim_bsp *)calloc(1, sizeof(struct chiefrim_bsp));
	b.material = default_material;
	if (!b.out ||
		!hash_init(&b.plane_table, 1024 + map_plane_count) ||
		!hash_init(&b.vertex_table, 1024) ||
		!hash_init(&b.edge_table, 1024) ||
		!hash_init(&b.triangle_table, 1024))
	{
		snprintf(error, (size_t)error_size, "out of memory");
		goto fail;
	}

	/* the map's planes first, unchanged: its cluster portals index them */
	b.out->planes = (real_plane3d *)malloc(sizeof(real_plane3d) * (size_t)MAX(map_plane_count, 1));
	if (!b.out->planes)
	{
		snprintf(error, (size_t)error_size, "out of memory");
		goto fail;
	}
	if (map_plane_count)
		memcpy(b.out->planes, map_planes, sizeof(real_plane3d) * (size_t)map_plane_count);
	b.out->plane_count = map_plane_count;
	b.plane_capacity = MAX(map_plane_count, 1);

	/* the triangles as fragments, with their planes */
	for (i = 0; i < triangle_count && !b.failed; i++)
	{
		struct chiefrim_triangle const *in = &triangles[i];
		real_plane3d plane;
		long f, designator;

		if (!polygon_plane(in->v, 3, &plane))
			continue; /* degenerate */
		if (duplicate_triangle(&b, in))
		{
			b.out->duplicates++;
			continue;
		}
		designator = add_plane(&b, &plane);
		if (designator == NONE)
			break;
		f = new_fragment(&b);
		if (f == NONE)
			break;
		/* onto the (perhaps merged) plane exactly: Halo meets the surface
		where a ray crosses that plane, and the BSP must agree on where that
		is */
		{
			real_plane3d const *on = &b.out->planes[designator & LONG_MAX];
			real sign = (designator & LONG_MIN) ? -1.f : 1.f;
			long v;

			for (v = 0; v < 3; v++)
			{
				real d = plane_distance(on, &in->v[v]);

				b.fragments[f].p[v].x = in->v[v].x - on->n.i * d;
				b.fragments[f].p[v].y = in->v[v].y - on->n.j * d;
				b.fragments[f].p[v].z = in->v[v].z - on->n.k * d;
			}
			(void)sign;
		}
		b.fragments[f].count = 3;
		b.fragments[f].plane = designator & LONG_MAX;
		b.fragments[f].flipped = (designator & LONG_MIN) != 0;
		b.fragments[f].id = in->id;
	}
	if (b.failed)
		goto failed;
	if (b.fragment_count == 0)
	{
		snprintf(error, (size_t)error_size, "no usable triangles");
		goto fail;
	}

	/* the 3D BSP: node 0 is the root */
	{
		long count = b.fragment_count;

		order = (long *)malloc(sizeof(long) * (size_t)count);
		if (!order)
		{
			snprintf(error, (size_t)error_size, "out of memory");
			goto fail;
		}
		for (i = 0; i < count; i++)
			order[i] = i;
		build_3d(&b, order, count, NULL, 0, 0);
	}
	free(order);
	order = NULL;
	if (b.failed)
		goto failed;
	if (b.out->node_count == 0)
	{
		snprintf(error, (size_t)error_size, "no BSP nodes");
		goto fail;
	}

	/* the surfaces: every emitted polygon, then every twin (reversed), so
	neighbours share edges and twins close what is left */
	for (i = 0; i < b.emitted_count && !b.failed; i++)
	{
		long vertex[CHIEFRIM_SURFACE_POINTS], v;

		for (v = 0; v < b.emitted[i].count; v++)
			vertex[v] = add_vertex(&b, &b.emitted[i].p[v]);
		if (!b.failed)
			add_surface(&b, b.emitted[i].designator, vertex, b.emitted[i].count, FALSE, b.emitted[i].id);
	}
	for (i = 0; i < b.emitted_count && !b.failed; i++)
	{
		long vertex[CHIEFRIM_SURFACE_POINTS], v, count = b.emitted[i].count;

		for (v = 0; v < count; v++)
			vertex[v] = add_vertex(&b, &b.emitted[i].p[(count - v) % count]);
		if (!b.failed)
			add_surface(&b, b.emitted[i].designator ^ LONG_MIN, vertex, count, TRUE, b.emitted[i].id);
	}
	if (b.failed)
		goto failed;
	/* edges still open (three sides on one edge): the surface's own twin is
	their other side */
	for (i = 0; i < b.out->edge_count; i++)
	{
		struct collision_edge *edge = &b.out->edges[i];

		if (edge->surface_indices[1] == NONE)
		{
			long s = edge->surface_indices[0];

			edge->surface_indices[1] = s < b.emitted_count ? s + b.emitted_count : s - b.emitted_count;
		}
	}
	fix_codes(&b);

	/* structure leaves: every one of ours (and every one the map's render
	BSP may name) in the cluster of the map's leaf 0, with no render
	surfaces */
	{
		long map_leaf_count = map_structure->leaves.count;
		struct structure_leaf template_leaf;

		memset(&template_leaf, 0, sizeof(template_leaf));
		if (map_leaf_count > 0)
			template_leaf = *(struct structure_leaf const *)map_structure->leaves.address;
		template_leaf.surface_reference_count = 0;
		template_leaf.first_surface_reference_index = 0;
		b.out->structure_leaf_count = MAX(b.out->leaf_count, map_leaf_count);
		b.out->structure_leaves = (struct structure_leaf *)malloc(sizeof(struct structure_leaf) * (size_t)b.out->structure_leaf_count);
		if (!b.out->structure_leaves)
		{
			snprintf(error, (size_t)error_size, "out of memory");
			goto fail;
		}
		for (i = 0; i < b.out->structure_leaf_count; i++)
			b.out->structure_leaves[i] = template_leaf;
		b.out->structure = *map_structure;
		b.out->structure.leaves.count = b.out->structure_leaf_count;
		b.out->structure.leaves.address = b.out->structure_leaves;
		b.out->structure.leaves.definition = NULL;

		/* the map's clusters without their fog planes: below the host
		level's sea, Halo would think Chief is under water
		(scenario_location_underwater) */
		b.out->cluster_count = map_structure->clusters.count;
		b.out->clusters = (struct structure_cluster *)malloc(sizeof(struct structure_cluster) * (size_t)MAX(b.out->cluster_count, 1));
		if (!b.out->clusters)
		{
			snprintf(error, (size_t)error_size, "out of memory");
			goto fail;
		}
		if (b.out->cluster_count)
			memcpy(b.out->clusters, map_structure->clusters.address, sizeof(struct structure_cluster) * (size_t)b.out->cluster_count);
		for (i = 0; i < b.out->cluster_count; i++)
			b.out->clusters[i].fog_reference = NONE;
		b.out->structure.clusters.address = b.out->clusters;
		b.out->structure.clusters.definition = NULL;
	}

	/* the tag blocks */
#define CHIEFRIM_BLOCK(block, array, element_count) \
	((block).count = (element_count), (block).address = (array), (block).definition = NULL)
	CHIEFRIM_BLOCK(b.out->bsp.bsp3d.nodes, b.out->nodes, b.out->node_count);
	CHIEFRIM_BLOCK(b.out->bsp.bsp3d.planes, b.out->planes, b.out->plane_count);
	CHIEFRIM_BLOCK(b.out->bsp.leaves, b.out->leaves, b.out->leaf_count);
	CHIEFRIM_BLOCK(b.out->bsp.bsp2d_references, b.out->references, b.out->reference_count);
	CHIEFRIM_BLOCK(b.out->bsp.bsp2d.nodes, b.out->nodes2d, b.out->node2d_count);
	CHIEFRIM_BLOCK(b.out->bsp.surfaces, b.out->surfaces, b.out->surface_count);
	CHIEFRIM_BLOCK(b.out->bsp.edges, b.out->edges, b.out->edge_count);
	CHIEFRIM_BLOCK(b.out->bsp.vertices, b.out->vertices, b.out->vertex_count);
#undef CHIEFRIM_BLOCK

	b.out->triangle_count = b.emitted_count; /* the fronts: surfaces 0..n-1 */
	b.out->min_z = REAL_MAX;
	for (i = 0; i < b.out->vertex_count; i++)
		b.out->min_z = MIN(b.out->min_z, b.out->vertices[i].point.z);
	free(b.fragments);
	free(b.emitted);
	free(b.overflow);
	hash_dispose(&b.plane_table);
	hash_dispose(&b.vertex_table);
	hash_dispose(&b.edge_table);
	hash_dispose(&b.triangle_table);
	return b.out;

failed:
	snprintf(error, (size_t)error_size, "%s", b.failure ? b.failure : "failed");
fail:
	free(order);
	free(b.fragments);
	free(b.emitted);
	free(b.overflow);
	hash_dispose(&b.plane_table);
	hash_dispose(&b.vertex_table);
	hash_dispose(&b.edge_table);
	hash_dispose(&b.triangle_table);
	chiefrim_bsp_free(b.out);
	return NULL;
}

void chiefrim_bsp_free(struct chiefrim_bsp *bsp)
{
	if (!bsp)
		return;
	free(bsp->planes);
	free(bsp->nodes);
	free(bsp->leaves);
	free(bsp->references);
	free(bsp->nodes2d);
	free(bsp->surfaces);
	free(bsp->edges);
	free(bsp->vertices);
	free(bsp->structure_leaves);
	free(bsp->clusters);
	free(bsp->surface_ids);
	free(bsp);
}

long chiefrim_bsp_find_surface(struct chiefrim_bsp const *bsp, unsigned long id)
{
	long index;

	if (!bsp)
		return NONE;
	/* the fronts are the first triangle_count surfaces; ids aren't sorted, so
	search linearly (only for the few bipeds, once per swap) */
	for (index = 0; index < bsp->triangle_count; index++)
	{
		if (bsp->surface_ids[index] == (id & 0x7FFFFFFFu))
			return index;
	}
	return NONE;
}
