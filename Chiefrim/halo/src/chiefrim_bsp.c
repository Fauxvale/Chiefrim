/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_BSP.C

A runtime builder of Halo collision BSPs from Skyrim's collision triangles
(Chiefrim/docs/DESIGN.md §5.2). Halo's biped physics needs a real collision
BSP: it keeps the surface it stands on by index and walks that surface's
edges, so loose triangles won't do.

Skyrim's collision is an open triangle soup, with no inside to speak of, so
the BSP has no solid leaves. Instead:

- every leaf "contains two-sided" surfaces: Halo's ray query then tests the
  surfaces on every plane it crosses, exactly, from either side
  (collision_bsp.c, the semi-empty case);
- every triangle is two surfaces: itself, and a reversed twin for its back,
  because the sphere query matches a leaf's surfaces to the side of the
  plane it came from;
- a surface's edges are winged edges shared with its neighbours on the same
  side. Halo's edge features read both surfaces of every edge, so no edge
  is open: all fronts are added first (neighbours share edges), then all
  twins, which close their fronts' open edges and pair up among themselves;
  a triangle and its twin on one edge read as a knife edge, which is what an
  open edge is.

The 3D BSP halves space with axis-aligned planes (at the median of the
triangles' centres, kd-tree style) until each cell holds a few triangles.
Halo finds a triangle only through a plane it crossed on the way to a leaf,
so a cell's triangle planes must be nodes; but splitting on triangle planes
duplicates every triangle across them, and on bumpy ground (a plane per
triangle) that multiplies without end. So a cell's planes form a CHAIN whose
nodes send both sides on to the next node, ending in one leaf that refers
to every triangle of the cell from both sides: every path through the cell
crosses every plane, nothing is duplicated. (A node graph, not a tree:
Halo's traversals only follow indices.) The price: Halo's sphere query
walks a chain once per side of each plane a sphere straddles, so chains are
short (CHIEFRIM_BSP_CHAIN). A triangle on a splitting plane is referenced
(through a 2D BSP of that plane's triangles) from the leaves it reaches. Triangles aren't
clipped: a 2D BSP's leaves are whole triangles, and Halo's own exact
point-in-polygon tests sort out the rest.

Runs on a worker thread: no game state, and the C library's allocator, not
the game's debug one (which isn't thread-safe).
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

#define CHIEFRIM_BSP_EPSILON         0.0005f  /* world units: 0.1 Skyrim unit */
#define CHIEFRIM_BSP_WELD            0.002f   /* vertices this close are one */
#define CHIEFRIM_BSP_MAXIMUM_DEPTH   120      /* Halo's plane stacks hold 128 */
#define CHIEFRIM_BSP_CELL           8        /* triangles in a cell: below this, a plane chain */
#define CHIEFRIM_BSP_CHAIN          12       /* at most this many planes in one chain */
#define CHIEFRIM_BSP_AXIS_DEPTH     60       /* axis-aligned splits stop by this depth */
#define CHIEFRIM_BSP_CANDIDATES      16
#define CHIEFRIM_BSP_SAMPLE          512
#define CHIEFRIM_SURFACE_TWO_SIDED_LEAF 0x0001 /* _collision_leaf_contains_two_sided_bit */

enum
{
	_side_front,
	_side_back,
	_side_span,
	_side_on,
};

/* ---------- structures */

struct builder_triangle
{
	real_point3d v[3];
	long vertex[3];
	long plane;
	boolean flipped;      /* its normal is its plane's, negated */
	long front_surface;   /* designator plane | (flipped ? sign : 0) */
	long back_surface;    /* the other designator, reversed winding */
	unsigned long id;
	short material;
};

struct pending
{
	long designator;
	long *surfaces;
	long count;
};

struct projected
{
	long surface;
	real_point2d p[3];
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
	struct builder_triangle *triangles;
	long triangle_count;
	long map_plane_count;
	short material;
	struct hash_table plane_table;
	struct hash_table vertex_table;
	struct hash_table edge_table;
	boolean failed;
	long dropped_deep;
	/* triangles a 2D BSP couldn't separate from the rest (overlapping on one
	plane): they go in another reference to the same plane in the leaf */
	struct projected *overflow;
	long overflow_count;
	long overflow_capacity;
};

/* ---------- helpers */

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
		b->failed = TRUE;
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

/* ---------- planes and vertices */

static unsigned long quantize(real value, real step)
{
	return (unsigned long)(long)floorf(value / step + 0.5f);
}

static long find_plane(struct builder *b, real_plane3d const *plane)
{
	long index;

	for (index = 0; index < 2; index++)
	{
		real sign = index ? -1.f : 1.f;
		unsigned long key[3];
		struct hash_entry *entry;

		key[0] = quantize(plane->n.i * sign, 1.f / 1024.f) * 31u + quantize(plane->n.j * sign, 1.f / 1024.f);
		key[1] = quantize(plane->n.k * sign, 1.f / 1024.f);
		key[2] = quantize(plane->d * sign, 0.004f);
		entry = hash_slot(&b->plane_table, key);
		if (entry->used)
		{
			real_plane3d const *found = &b->out->planes[entry->value];

			if (found->n.i * plane->n.i * sign + found->n.j * plane->n.j * sign + found->n.k * plane->n.k * sign > 0.99999f &&
				fabsf(found->d - plane->d * sign) < 0.004f)
			{
				return index ? (entry->value | LONG_MIN) : entry->value;
			}
		}
	}
	return NONE;
}

static long add_plane(struct builder *b, real_plane3d const *plane)
{
	long found = find_plane(b, plane);
	unsigned long key[3];
	long index;

	if (found != NONE)
		return found;
	index = b->out->plane_count;
	GROW(b, planes, plane_capacity, index + 1);
	if (b->failed)
		return NONE;
	b->out->planes[index] = *plane;
	b->out->plane_count++;
	key[0] = quantize(plane->n.i, 1.f / 1024.f) * 31u + quantize(plane->n.j, 1.f / 1024.f);
	key[1] = quantize(plane->n.k, 1.f / 1024.f);
	key[2] = quantize(plane->d, 0.004f);
	{
		struct hash_entry *entry = hash_slot(&b->plane_table, key);

		if (!entry->used)
			hash_put(entry, key, index, &b->plane_table);
	}
	return index;
}

static long add_vertex(struct builder *b, real_point3d const *point)
{
	unsigned long key[3];
	struct hash_entry *entry;
	long index;

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

/* The edge u -> w of surface s on sheet (0 front surfaces, 1 back twins):
the open edge w -> u of a neighbour on the same sheet if there is one,
else a new edge. Returns the edge; *side is the side s is on. */
static long add_edge(struct builder *b, long u, long w, long s, unsigned long sheet, short *side)
{
	unsigned long key[3];
	struct hash_entry *entry;
	long index;

	key[0] = (unsigned long)MIN(u, w);
	key[1] = (unsigned long)MAX(u, w);
	key[2] = sheet;
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

static long add_surface(struct builder *b, long designator, long const vertex[3], boolean twin, unsigned long id)
{
	long s = b->out->surface_count;
	long edges[3];
	short sides[3];
	long k;

	GROW(b, surfaces, surface_capacity, s + 1);
	b->out->surface_ids = grow(b, b->out->surface_ids, &b->surface_id_capacity, s + 1, (long)sizeof(unsigned long));
	if (b->failed)
		return NONE;
	b->out->surface_count++;
	b->out->surface_ids[s] = twin ? (id | 0x80000000u) : (id & 0x7FFFFFFFu);

	for (k = 0; k < 3; k++)
	{
		edges[k] = add_edge(b, vertex[k], vertex[(k + 1) % 3], s, 0, &sides[k]);
		if (b->failed)
			return NONE;
	}
	for (k = 0; k < 3; k++)
		b->out->edges[edges[k]].edge_indices[sides[k]] = edges[(k + 1) % 3];

	b->out->surfaces[s].plane_designator = designator;
	b->out->surfaces[s].first_edge_index = edges[0];
	b->out->surfaces[s].flags = 0;
	b->out->surfaces[s].breakable_surface_index = (byte)NONE;
	b->out->surfaces[s].material_index = b->material;
	return s;
}

/* The surface of triangle t whose designator has this sign. */
static long surface_for(struct builder_triangle const *t, boolean negative)
{
	return negative == t->flipped ? t->front_surface : t->back_surface;
}

/* ---------- 2D BSPs (one per leaf reference) */

static void overflow(struct builder *b, struct projected const *items, long count)
{
	long i;

	if (b->overflow_count + count > b->overflow_capacity)
	{
		long capacity = MAX(64, (b->overflow_count + count) * 2);
		struct projected *grown = (struct projected *)realloc(b->overflow, sizeof(struct projected) * (size_t)capacity);

		if (!grown)
		{
			b->failed = TRUE;
			return;
		}
		b->overflow = grown;
		b->overflow_capacity = capacity;
	}
	for (i = 0; i < count; i++)
		b->overflow[b->overflow_count++] = items[i];
}

static long build_2d(struct builder *b, struct projected *items, long count, long depth)
{
	long best_item = NONE, best_edge = 0, best_score = LONG_MAX;
	long candidates = MIN(count, 64); /* every triangle's edges, in practice */
	long c;

	if (count == 1 || depth > 64)
	{
		if (count > 1)
			overflow(b, items + 1, count - 1);
		return items[0].surface | LONG_MIN;
	}

	/* a split line along an edge of one of the triangles, with triangles on
	both sides */
	for (c = 0; c < candidates; c++)
	{
		long item = c * count / candidates;
		long e;

		for (e = 0; e < 3; e++)
		{
			real_point2d const *a = &items[item].p[e];
			real_point2d const *z = &items[item].p[(e + 1) % 3];
			real nx = -(z->y - a->y), ny = z->x - a->x;
			real length = sqrtf(nx * nx + ny * ny);
			long front = 0, back = 0, span = 0, i;

			if (length < 1e-6f)
				continue;
			nx /= length;
			ny /= length;
			for (i = 0; i < count; i++)
			{
				real mn = REAL_MAX, mx = -REAL_MAX;
				long v;

				for (v = 0; v < 3; v++)
				{
					real d = nx * (items[i].p[v].x - a->x) + ny * (items[i].p[v].y - a->y);
					mn = MIN(mn, d);
					mx = MAX(mx, d);
				}
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
		overflow(b, items + 1, count - 1);
		return items[0].surface | LONG_MIN;
	}

	{
		real_point2d const a = items[best_item].p[best_edge];
		real_point2d const z = items[best_item].p[(best_edge + 1) % 3];
		real nx = -(z.y - a.y), ny = z.x - a.x;
		real length = sqrtf(nx * nx + ny * ny);
		struct projected *front = (struct projected *)malloc(sizeof(struct projected) * (size_t)count);
		struct projected *back = (struct projected *)malloc(sizeof(struct projected) * (size_t)count);
		long front_count = 0, back_count = 0, node, i;

		if (!front || !back)
		{
			free(front);
			free(back);
			b->failed = TRUE;
			return items[0].surface | LONG_MIN;
		}
		nx /= length;
		ny /= length;
		for (i = 0; i < count; i++)
		{
			real mn = REAL_MAX, mx = -REAL_MAX;
			long v;

			for (v = 0; v < 3; v++)
			{
				real d = nx * (items[i].p[v].x - a.x) + ny * (items[i].p[v].y - a.y);
				mn = MIN(mn, d);
				mx = MAX(mx, d);
			}
			/* as counted above: in front, behind, or across (both) */
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
			/* children[distance >= 0]: the front, then the back */
			long child_back = back_count ? build_2d(b, back, back_count, depth + 1) : front[0].surface | LONG_MIN;
			long child_front = front_count ? build_2d(b, front, front_count, depth + 1) : back[0].surface | LONG_MIN;

			b->out->nodes2d[node].child_indices[0] = child_back;
			b->out->nodes2d[node].child_indices[1] = child_front;
		}
		free(front);
		free(back);
		return node;
	}
}

/* The triangles of a pending entry, projected as collision_bsp.c projects
for this reference. NULL if out of memory. */
static struct projected *project_entry(struct builder *b, struct pending const *entry)
{
	real_plane3d const *plane = &b->out->planes[entry->designator & LONG_MAX];
	real absolute_i = fabsf(plane->n.i), absolute_j = fabsf(plane->n.j), absolute_k = fabsf(plane->n.k);
	short projection;
	boolean sign;
	struct projected *items;
	long i;

	/* the projection collision_bsp.c uses for this reference */
	if (absolute_k >= absolute_j && absolute_k >= absolute_i)
		projection = _z;
	else
		projection = absolute_j >= absolute_i ? _y : _x;
	sign = projection_sign_from_vector3d(&plane->n, projection) != (entry->designator & LONG_MIN ? TRUE : FALSE);

	items = (struct projected *)malloc(sizeof(struct projected) * (size_t)entry->count);
	if (!items)
	{
		b->failed = TRUE;
		return NULL;
	}
	for (i = 0; i < entry->count; i++)
	{
		struct collision_surface const *surface = &b->out->surfaces[entry->surfaces[i]];
		long edge_index = surface->first_edge_index;
		long v;

		items[i].surface = entry->surfaces[i];
		for (v = 0; v < 3; v++)
		{
			struct collision_edge const *edge = &b->out->edges[edge_index];
			boolean reverse = edge->surface_indices[1] == entry->surfaces[i];

			project_point3d(&b->out->vertices[edge->vertex_indices[reverse]].point, projection, sign, &items[i].p[v]);
			edge_index = edge->edge_indices[reverse];
		}
	}
	return items;
}

/* ---------- the 3D BSP */

static long make_leaf(struct builder *b, struct pending const *pending, long pending_count)
{
	long leaf = b->out->leaf_count;
	long first = b->out->reference_count;
	long count = 0, p;

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
		on it (Halo tries every reference of a leaf) */
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
			b->out->references[reference].root_index = root;
			count++;
			b->out->dropped_overlaps += b->overflow_count; /* now: in extra references */
			if (b->overflow_count) /* the game's memcpy rejects NULL, even for 0 bytes */
				memcpy(items, b->overflow, sizeof(struct projected) * (size_t)b->overflow_count);
			item_count = b->overflow_count;
		}
		free(items);
		if (b->failed)
			return NONE;
	}

	b->out->leaves[leaf].flags = CHIEFRIM_SURFACE_TWO_SIDED_LEAF;
	b->out->leaves[leaf].bsp2d_reference_count = (short)MIN(count, SHORT_MAX);
	b->out->leaves[leaf].first_bsp2d_reference_index = first;
	return leaf;
}

static short classify(struct builder const *b, struct builder_triangle const *t, long plane_index, real_plane3d const *plane)
{
	real mn = REAL_MAX, mx = -REAL_MAX;
	long v;

	if (t->plane == plane_index)
		return _side_on;
	for (v = 0; v < 3; v++)
	{
		real d = plane_distance(plane, &t->v[v]);
		mn = MIN(mn, d);
		mx = MAX(mx, d);
	}
	if (mn >= -CHIEFRIM_BSP_EPSILON && mx > CHIEFRIM_BSP_EPSILON)
		return _side_front;
	if (mx <= CHIEFRIM_BSP_EPSILON && mn < -CHIEFRIM_BSP_EPSILON)
		return _side_back;
	return _side_span; /* across it, or on it without being of it: both sides */
}

static long choose_plane(struct builder *b, long const *triangles, long count)
{
	long best_plane = b->triangles[triangles[0]].plane;
	long best_score = LONG_MAX;
	long candidates = MIN(count, CHIEFRIM_BSP_CANDIDATES);
	long sample = MIN(count, CHIEFRIM_BSP_SAMPLE);
	long c;

	for (c = 0; c < candidates; c++)
	{
		long plane_index = b->triangles[triangles[c * count / candidates]].plane;
		real_plane3d const *plane = &b->out->planes[plane_index];
		long front = 0, back = 0, span = 0, on = 0, i;
		long score;

		for (i = 0; i < sample; i++)
		{
			switch (classify(b, &b->triangles[triangles[i * count / sample]], plane_index, plane))
			{
			case _side_front: front++; break;
			case _side_back: back++; break;
			case _side_span: span++; break;
			default: on++; break;
			}
		}
		score = span * 4 + labs(front - back) - on;
		if (score < best_score)
		{
			best_score = score;
			best_plane = plane_index;
		}
	}
	return best_plane;
}

/* Keeps the surfaces of pending that reach the given side of plane. */
static boolean filter_pending(struct builder *b, struct pending const *in, long in_count,
	real_plane3d const *plane, boolean front, struct pending *out)
{
	long p;

	for (p = 0; p < in_count; p++)
	{
		long i;

		out[p].designator = in[p].designator;
		out[p].count = 0;
		out[p].surfaces = in[p].count ? (long *)malloc(sizeof(long) * (size_t)in[p].count) : NULL;
		if (in[p].count && !out[p].surfaces)
		{
			b->failed = TRUE;
			return FALSE;
		}
		for (i = 0; i < in[p].count; i++)
		{
			long surface = in[p].surfaces[i];
			struct builder_triangle const *t = &b->triangles[surface % b->triangle_count]; /* fronts, then twins */
			real mn = REAL_MAX, mx = -REAL_MAX;
			long v;

			for (v = 0; v < 3; v++)
			{
				real d = plane_distance(plane, &t->v[v]);
				mn = MIN(mn, d);
				mx = MAX(mx, d);
			}
			if (front ? mx >= -CHIEFRIM_BSP_EPSILON : mn <= CHIEFRIM_BSP_EPSILON)
				out[p].surfaces[out[p].count++] = surface;
		}
	}
	return TRUE;
}

static void free_pending(struct pending *pending, long count)
{
	long p;

	for (p = 0; p < count; p++)
		free(pending[p].surfaces);
	free(pending);
}

static int compare_reals(void const *a, void const *z)
{
	real x = *(real const *)a, y = *(real const *)z;

	return x < y ? -1 : x > y ? 1 : 0;
}

/* An axis-aligned plane halving a large set: the longest axis of the
triangles' centres, at their median. NONE if it can't make both halves
smaller (everything straddles it). */
static long choose_axis_plane(struct builder *b, long const *triangles, long count)
{
	real lo[3] = { REAL_MAX, REAL_MAX, REAL_MAX }, hi[3] = { -REAL_MAX, -REAL_MAX, -REAL_MAX };
	long sample = MIN(count, 1024);
	real *centres;
	long axis = 0, i, front = 0, back = 0;
	real_plane3d plane;
	long plane_index;

	for (i = 0; i < count; i++)
	{
		struct builder_triangle const *t = &b->triangles[triangles[i]];
		long k;

		for (k = 0; k < 3; k++)
		{
			real c = (t->v[0].n[k] + t->v[1].n[k] + t->v[2].n[k]) / 3.f;

			lo[k] = MIN(lo[k], c);
			hi[k] = MAX(hi[k], c);
		}
	}
	for (i = 1; i < 3; i++)
	{
		if (hi[i] - lo[i] > hi[axis] - lo[axis])
			axis = i;
	}
	if (hi[axis] - lo[axis] < 0.01f)
		return NONE;

	centres = (real *)malloc(sizeof(real) * (size_t)sample);
	if (!centres)
	{
		b->failed = TRUE;
		return NONE;
	}
	for (i = 0; i < sample; i++)
	{
		struct builder_triangle const *t = &b->triangles[triangles[i * count / sample]];

		centres[i] = (t->v[0].n[axis] + t->v[1].n[axis] + t->v[2].n[axis]) / 3.f;
	}
	qsort(centres, (size_t)sample, sizeof(real), compare_reals);
	plane.n.i = axis == 0 ? 1.f : 0.f;
	plane.n.j = axis == 1 ? 1.f : 0.f;
	plane.n.k = axis == 2 ? 1.f : 0.f;
	/* halfway between two centres, never through one: a split plane through
	a triangle crosses Halo's rays where that triangle's plane does, and the
	ray query takes the crossing for the split's, missing the triangle */
	plane.d = sample > 1 ? (centres[sample / 2 - 1] + centres[sample / 2]) * 0.5f : centres[0];
	free(centres);

	/* both halves must shrink */
	for (i = 0; i < count; i++)
	{
		struct builder_triangle const *t = &b->triangles[triangles[i]];
		real mn = MIN(MIN(t->v[0].n[axis], t->v[1].n[axis]), t->v[2].n[axis]) - plane.d;
		real mx = MAX(MAX(t->v[0].n[axis], t->v[1].n[axis]), t->v[2].n[axis]) - plane.d;

		if (mx > -CHIEFRIM_BSP_EPSILON)
			front++;
		if (mn < CHIEFRIM_BSP_EPSILON)
			back++;
	}
	if (front >= count || back >= count)
		return NONE;

	plane_index = add_plane(b, &plane);
	return plane_index == NONE ? NONE : (plane_index & LONG_MAX);
}

/* A cell's planes as a chain (see the top): nodes in order, both children
the next one, then one leaf with every triangle of the cell referenced from
both sides of its plane, besides what the cell's ancestors pass down. NONE
if the cell has more planes than a chain may hold. */
static long build_chain(struct builder *b, long const *triangles, long count,
	struct pending const *pending, long pending_count, long depth)
{
	long planes[CHIEFRIM_BSP_CHAIN];
	long plane_count = 0, first_node, leaf, i, p;
	struct pending *all;
	long all_count;

	for (i = 0; i < count; i++)
	{
		long plane = b->triangles[triangles[i]].plane;

		for (p = 0; p < plane_count && planes[p] != plane; p++)
			;
		if (p < plane_count)
			continue;
		if (plane_count == CHIEFRIM_BSP_CHAIN)
			return NONE;
		planes[plane_count++] = plane;
	}
	if (depth + plane_count >= CHIEFRIM_BSP_MAXIMUM_DEPTH)
		return NONE;

	/* the leaf's references: the ancestors', and each chain plane's
	triangles from both sides */
	all_count = pending_count + plane_count * 2;
	all = (struct pending *)calloc((size_t)MAX(all_count, 1), sizeof(struct pending));
	if (!all)
	{
		b->failed = TRUE;
		return NONE;
	}
	for (i = 0; i < pending_count; i++)
		all[i] = pending[i]; /* shared, not freed here */
	for (p = 0; p < plane_count; p++)
	{
		struct pending *front = &all[pending_count + p * 2];
		struct pending *back = &all[pending_count + p * 2 + 1];

		front->designator = planes[p];
		back->designator = planes[p] | LONG_MIN;
		front->surfaces = (long *)malloc(sizeof(long) * (size_t)count);
		back->surfaces = (long *)malloc(sizeof(long) * (size_t)count);
		if (!front->surfaces || !back->surfaces)
		{
			b->failed = TRUE;
			break;
		}
		for (i = 0; i < count; i++)
		{
			struct builder_triangle const *t = &b->triangles[triangles[i]];

			if (t->plane != planes[p])
				continue;
			front->surfaces[front->count++] = surface_for(t, FALSE);
			back->surfaces[back->count++] = surface_for(t, TRUE);
		}
	}

	first_node = b->out->node_count;
	if (!b->failed)
		GROW(b, nodes, node_capacity, first_node + plane_count);
	leaf = b->failed ? NONE : make_leaf(b, all, all_count);
	if (!b->failed && leaf != NONE)
	{
		b->out->node_count += plane_count;
		for (p = 0; p < plane_count; p++)
		{
			long next = p + 1 < plane_count ? first_node + p + 1 : (leaf | LONG_MIN);

			b->out->nodes[first_node + p].plane_designator = planes[p];
			b->out->nodes[first_node + p].children[0] = next;
			b->out->nodes[first_node + p].children[1] = next;
		}
		b->out->max_depth = MAX(b->out->max_depth, depth + plane_count);
	}
	for (p = pending_count; p < all_count; p++)
		free(all[p].surfaces);
	free(all);
	if (b->failed || leaf == NONE)
		return NONE;
	return plane_count ? first_node : (leaf | LONG_MIN);
}

static long build_3d(struct builder *b, long const *triangles, long count,
	struct pending const *pending, long pending_count, long depth)
{
	long plane_index = NONE, node, i;
	real_plane3d plane;
	long *front = NULL, *back = NULL, *on = NULL;
	long front_count = 0, back_count = 0, on_count = 0;
	struct pending *front_pending = NULL, *back_pending = NULL;
	long child_front, child_back;

	if (b->failed)
		return NONE;
	b->out->max_depth = MAX(b->out->max_depth, depth);
	if (count == 0 || depth >= CHIEFRIM_BSP_MAXIMUM_DEPTH)
	{
		long leaf;

		b->dropped_deep += count;
		leaf = make_leaf(b, pending, pending_count);
		return leaf == NONE ? NONE : (leaf | LONG_MIN);
	}

	if (count > CHIEFRIM_BSP_CELL && depth < CHIEFRIM_BSP_AXIS_DEPTH)
		plane_index = choose_axis_plane(b, triangles, count);
	if (b->failed)
		return NONE;
	if (plane_index == NONE)
	{
		long chain = build_chain(b, triangles, count, pending, pending_count, depth);

		if (chain != NONE || b->failed)
			return chain;
		/* too many planes for one chain, and no axis plane divides them:
		split on a triangle plane (duplicating the ones across it) */
		plane_index = choose_plane(b, triangles, count);
	}
	plane = b->out->planes[plane_index];

	front = (long *)malloc(sizeof(long) * (size_t)count);
	back = (long *)malloc(sizeof(long) * (size_t)count);
	on = (long *)malloc(sizeof(long) * (size_t)count);
	front_pending = (struct pending *)calloc((size_t)pending_count + 1, sizeof(struct pending));
	back_pending = (struct pending *)calloc((size_t)pending_count + 1, sizeof(struct pending));
	if (!front || !back || !on || !front_pending || !back_pending)
	{
		b->failed = TRUE;
		goto done;
	}

	for (i = 0; i < count; i++)
	{
		switch (classify(b, &b->triangles[triangles[i]], plane_index, &plane))
		{
		case _side_front: front[front_count++] = triangles[i]; break;
		case _side_back: back[back_count++] = triangles[i]; break;
		case _side_span:
			front[front_count++] = triangles[i];
			back[back_count++] = triangles[i];
			break;
		default: on[on_count++] = triangles[i]; break;
		}
	}

	/* the triangles on this plane: in front, the surfaces facing it; behind,
	the ones facing away (each triangle has one of each) */
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
		b->failed = TRUE;
		goto done;
	}
	for (i = 0; i < on_count; i++)
	{
		struct builder_triangle const *t = &b->triangles[on[i]];

		front_pending[pending_count].surfaces[i] = surface_for(t, FALSE);
		back_pending[pending_count].surfaces[i] = surface_for(t, TRUE);
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
	if (front_pending)
		free_pending(front_pending, pending_count + 1);
	if (back_pending)
		free_pending(back_pending, pending_count + 1);
	return b->failed ? NONE : node;
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
	long reversed[3];
	long i;

	memset(&b, 0, sizeof(b));
	b.out = (struct chiefrim_bsp *)calloc(1, sizeof(struct chiefrim_bsp));
	b.material = default_material;
	if (!b.out ||
		!hash_init(&b.plane_table, triangle_count + map_plane_count) ||
		!hash_init(&b.vertex_table, triangle_count * 3) ||
		!hash_init(&b.edge_table, triangle_count * 6))
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
	b.map_plane_count = map_plane_count;

	/* triangles: planes, welded vertices, two surfaces each (front, back
	twin) on their own sheets of winged edges */
	b.triangles = (struct builder_triangle *)malloc(sizeof(struct builder_triangle) * (size_t)MAX(triangle_count, 1));
	if (!b.triangles)
	{
		snprintf(error, (size_t)error_size, "out of memory");
		goto fail;
	}
	for (i = 0; i < triangle_count && !b.failed; i++)
	{
		struct chiefrim_triangle const *in = &triangles[i];
		struct builder_triangle *t = &b.triangles[b.triangle_count];
		real_vector3d e1, e2, n;
		real length;
		real_plane3d plane;
		long plane_designator;
		long vertex[3];

		e1.i = in->v[1].x - in->v[0].x; e1.j = in->v[1].y - in->v[0].y; e1.k = in->v[1].z - in->v[0].z;
		e2.i = in->v[2].x - in->v[0].x; e2.j = in->v[2].y - in->v[0].y; e2.k = in->v[2].z - in->v[0].z;
		n.i = e1.j * e2.k - e1.k * e2.j;
		n.j = e1.k * e2.i - e1.i * e2.k;
		n.k = e1.i * e2.j - e1.j * e2.i;
		length = sqrtf(n.i * n.i + n.j * n.j + n.k * n.k);
		if (!(length > 1e-7f) || !(length < 1e30f)) /* also NaN and infinity */
			continue; /* degenerate */
		n.i /= length; n.j /= length; n.k /= length;
		plane.n = n;
		plane.d = n.i * in->v[0].x + n.j * in->v[0].y + n.k * in->v[0].z;

		vertex[0] = add_vertex(&b, &in->v[0]);
		vertex[1] = add_vertex(&b, &in->v[1]);
		vertex[2] = add_vertex(&b, &in->v[2]);
		if (b.failed)
			break;
		if (vertex[0] == vertex[1] || vertex[1] == vertex[2] || vertex[2] == vertex[0])
			continue; /* welded away */

		plane_designator = add_plane(&b, &plane);
		if (plane_designator == NONE)
			break;
		t->v[0] = in->v[0];
		t->v[1] = in->v[1];
		t->v[2] = in->v[2];
		t->vertex[0] = vertex[0];
		t->vertex[1] = vertex[1];
		t->vertex[2] = vertex[2];
		t->plane = plane_designator & LONG_MAX;
		t->flipped = (plane_designator & LONG_MIN) != 0;
		t->id = in->id;
		t->material = in->material;

		/* surface t (front) and triangle_count + t (twin): filter_pending relies on it */
		t->front_surface = add_surface(&b, plane_designator, vertex, FALSE, in->id);
		b.triangle_count++;
	}
	for (i = 0; i < b.triangle_count && !b.failed; i++)
	{
		struct builder_triangle *t = &b.triangles[i];

		reversed[0] = t->vertex[0];
		reversed[1] = t->vertex[2];
		reversed[2] = t->vertex[1];
		t->back_surface = add_surface(&b, b.out->surfaces[t->front_surface].plane_designator ^ LONG_MIN,
			reversed, TRUE, t->id);
	}
	/* edges still open (non-manifold leftovers): the triangle's own twin
	is their other side */
	for (i = 0; i < b.out->edge_count && !b.failed; i++)
	{
		struct collision_edge *edge = &b.out->edges[i];

		if (edge->surface_indices[1] == NONE)
		{
			long s = edge->surface_indices[0];

			edge->surface_indices[1] = s < b.triangle_count ? s + b.triangle_count : s - b.triangle_count;
		}
	}
	if (b.failed)
	{
		snprintf(error, (size_t)error_size, "out of memory reading the triangles");
		goto fail;
	}
	if (b.triangle_count == 0)
	{
		snprintf(error, (size_t)error_size, "no usable triangles");
		goto fail;
	}

	/* the 3D BSP: node 0 is the root */
	order = (long *)malloc(sizeof(long) * (size_t)b.triangle_count);
	if (!order)
	{
		snprintf(error, (size_t)error_size, "out of memory");
		goto fail;
	}
	for (i = 0; i < b.triangle_count; i++)
		order[i] = i;
	build_3d(&b, order, b.triangle_count, NULL, 0, 0);
	free(order);
	order = NULL;
	if (b.failed || b.out->node_count == 0)
	{
		snprintf(error, (size_t)error_size, b.failed ? "out of memory building the BSP" : "no BSP nodes");
		goto fail;
	}
	if (b.dropped_deep)
	{
		snprintf(error, (size_t)error_size, "%ld triangles past the depth limit", b.dropped_deep);
		goto fail;
	}

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

	b.out->triangle_count = b.triangle_count;
	free(b.triangles);
	free(b.overflow);
	hash_dispose(&b.plane_table);
	hash_dispose(&b.vertex_table);
	hash_dispose(&b.edge_table);
	return b.out;

fail:
	free(order);
	free(b.triangles);
	free(b.overflow);
	hash_dispose(&b.plane_table);
	hash_dispose(&b.vertex_table);
	hash_dispose(&b.edge_table);
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
	/* front surfaces are the first triangle_count, in triangle order; ids
	aren't sorted, so search linearly (only for the few bipeds, per swap) */
	for (index = 0; index < bsp->triangle_count; index++)
	{
		if (bsp->surface_ids[index] == (id & 0x7FFFFFFFu))
			return index;
	}
	return NONE;
}
