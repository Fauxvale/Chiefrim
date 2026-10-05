/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
CHIEFRIM_BSP.H

Builds Halo collision BSPs from Skyrim's collision triangles at runtime
(Chiefrim/docs/DESIGN.md §5.2). Thread-safe: the build runs on a worker
thread and touches nothing of the game's; the result is swapped in on the
main thread (chiefrim.c).
*/

#ifndef __CHIEFRIM_BSP_H
#define __CHIEFRIM_BSP_H
#pragma once

#include "cseries.h"
#include "math/real_math.h"
#include "physics/collision_bsp_definitions.h"
#include "structures/structure_bsp_definitions.h"

/* A triangle in world units (Halo's), wound counter-clockwise around its
front. id is stable across builds (region and index), so a biped standing
on it keeps standing on it after a swap. */
struct chiefrim_triangle
{
	real_point3d v[3];
	unsigned long id;
	short material;
	short pad;
};

struct chiefrim_bsp_leaf
{
	word flags;
	short bsp2d_reference_count;
	long first_bsp2d_reference_index;
};

struct chiefrim_bsp_reference
{
	long plane_designator;
	long root_index;
};

/* A built BSP: owns every array. */
struct chiefrim_bsp
{
	struct collision_bsp bsp;            /* the blocks point into the arrays below */
	struct structure_bsp structure;      /* the map's, with leaves for these leaves */

	real_plane3d *planes;
	long plane_count;
	struct bsp3d_node *nodes;
	long node_count;
	struct chiefrim_bsp_leaf *leaves;
	long leaf_count;
	struct chiefrim_bsp_reference *references;
	long reference_count;
	struct bsp2d_node *nodes2d;
	long node2d_count;
	struct collision_surface *surfaces;
	long surface_count;
	struct collision_edge *edges;
	long edge_count;
	struct collision_vertex *vertices;
	long vertex_count;
	struct structure_leaf *structure_leaves;
	long structure_leaf_count;
	struct structure_cluster *clusters;  /* the map's, without fog planes */
	long cluster_count;

	unsigned long *surface_ids;          /* the triangle's id; | 0x80000000 for its back */
	long triangle_count;                 /* kept after dropping degenerate ones: surfaces
	                                        0..n-1 are their fronts, n..2n-1 their twins */
	real min_z;                          /* the lowest point of any triangle */
	long max_depth;
	long dropped_overlaps;               /* polygons overlapping on a plane, in extra references */
	long duplicates;                     /* triangles dropped as repeats of earlier ones */
	long shared_fragments;               /* polygons placed on both sides of a split they all but touch */
};

/* Builds a BSP from triangles. map_planes (the map's collision planes) are
copied in first: the map's cluster portals index them. map_structure's
leaf 0 gives the cluster every leaf belongs to. NULL on failure, with the
reason in error. */
struct chiefrim_bsp *chiefrim_bsp_build(
	struct chiefrim_triangle const *triangles,
	long triangle_count,
	real_plane3d const *map_planes,
	long map_plane_count,
	struct structure_bsp const *map_structure,
	short default_material,
	char *error,
	long error_size);

void chiefrim_bsp_free(struct chiefrim_bsp *bsp);

/* The surface built from a triangle id (front), or NONE. */
long chiefrim_bsp_find_surface(struct chiefrim_bsp const *bsp, unsigned long id);

#endif /* __CHIEFRIM_BSP_H */
