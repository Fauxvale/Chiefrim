/* SPDX-License-Identifier: GPL-3.0-or-later */
// The Havok harvesting here (gathering the world's static bodies, walking
// their shape trees, primitives as triangles, fault-guarded reads) is
// adapted from SkyCraft's Collision.cpp (MIT; THIRD-PARTY-NOTICES.md). Chiefrim
// sends triangles in Skyrim units where SkyCraft voxelizes for Minecraft.
#include "Collision.h"

#include "Link.h"
#include "Settings.h"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace chiefrim::Collision
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// regions around the player's: what Halo builds from, and a ring more
		// (so they're there when he walks on): Settings::CollisionRadius
		int RadiusXY() { return int(Settings::CollisionRadius()) + 1; }
		constexpr int  kBelow = 1;
		constexpr int  kAbove = 1;
		constexpr auto kFrameBudget = std::chrono::microseconds(2500);
		constexpr int  kMaxRegionsPerFrame = 3;
		constexpr auto kRefreshNear = std::chrono::milliseconds(1000);  // doors and the like move
		constexpr std::uint32_t kMaxKeys = 16384;
		constexpr std::size_t   kMaxTriangles = 200000;  // per region, a sanity bound

		struct Tri
		{
			float v[9];          // Skyrim world units
			bool  solid{ false };  // a face of a closed shape (box, capsule, convex hull), wound outward
		};

		struct Body
		{
			const RE::hkpShape* shape;
			const float*        xf;      // hkTransform: rotation columns, then translation
			float               lo[3], hi[3];  // world AABB, Skyrim units
			bool                terrain;       // the land: a height field, outside up
		};

		struct Pending
		{
			int                       rx, ry, rz;
			std::vector<cr_triangle>  tris;
			std::size_t               sent{ 0 };
			bool                      started{ false };
		};

		struct State
		{
			std::uint32_t epoch{ 0 };
			std::unordered_map<std::uint64_t, Clock::time_point> harvested;
			std::unordered_map<std::uint64_t, std::uint64_t> sentHash;  // what Halo has of each region
			std::vector<std::array<int, 3>> offsets;
			std::vector<Body>   bodies;
			std::vector<Pending> outbox;   // regions waiting for room in the ring
			std::unordered_map<int, bool> loggedTypes;
			std::uint64_t regionsSent{ 0 }, trianglesSent{ 0 };
			Clock::time_point lastLog{};
		} s;

		float SkyrimPerHavok() { return RE::bhkWorld::GetWorldScaleInverse(); }

		bool Finite(const float* a_v, int a_n)
		{
			for (int i = 0; i < a_n; ++i) {
				if (!std::isfinite(a_v[i]) || std::fabs(a_v[i]) > 1.0e7f) {
					return false;
				}
			}
			return true;
		}

		void XfPoint(const float* a_xf, const float* a_p, float* a_out)
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_xf[i] * a_p[0] + a_xf[4 + i] * a_p[1] + a_xf[8 + i] * a_p[2] + a_xf[12 + i];
			}
		}

		void XfDir(const float* a_xf, const float* a_d, float* a_out)
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_xf[i] * a_d[0] + a_xf[4 + i] * a_d[1] + a_xf[8 + i] * a_d[2];
			}
		}

		void XfCompose(const float* a_parent, const float* a_child, float* a_out)
		{
			for (int c = 0; c < 3; ++c) {
				XfDir(a_parent, a_child + c * 4, a_out + c * 4);
				a_out[c * 4 + 3] = 0.0f;
			}
			XfPoint(a_parent, a_child + 12, a_out + 12);
			a_out[15] = 1.0f;
		}

		bool XfLooksValid(const float* a_xf)
		{
			if (!Finite(a_xf, 16)) {
				return false;
			}
			for (int c = 0; c < 3; ++c) {
				const float* col = a_xf + c * 4;
				const float  len = col[0] * col[0] + col[1] * col[1] + col[2] * col[2];
				if (std::fabs(len - 1.0f) > 0.05f) {
					return false;
				}
			}
			return true;
		}

		void HkAabbToSky(const RE::hkAabb& a_box, float a_k, float* a_lo, float* a_hi)
		{
			alignas(16) float mn[4], mx[4];
			_mm_store_ps(mn, a_box.min.quad);
			_mm_store_ps(mx, a_box.max.quad);
			for (int i = 0; i < 3; ++i) {
				a_lo[i] = mn[i] * a_k;
				a_hi[i] = mx[i] * a_k;
			}
		}

		bool Overlaps(const float* a_lo, const float* a_hi, const float* b_lo, const float* b_hi)
		{
			return a_lo[0] <= b_hi[0] && a_hi[0] >= b_lo[0] && a_lo[1] <= b_hi[1] && a_hi[1] >= b_lo[1] && a_lo[2] <= b_hi[2] && a_hi[2] >= b_lo[2];
		}

		const float* Vec(const void* a_base, std::size_t a_offset)
		{
			return reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(a_base) + a_offset);
		}

		template <class T>
		T Field(const void* a_base, std::size_t a_offset)
		{
			T value;
			std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(a_base) + a_offset, sizeof(T));
			return value;
		}

		inline void Sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
		inline void Cross(const float* a, const float* b, float* o)
		{
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		}
		inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		// What Chief collides with: the world, not actors, projectiles or clutter.
		bool Included(RE::COL_LAYER a_layer)
		{
			switch (a_layer) {
			case RE::COL_LAYER::kStatic:
			case RE::COL_LAYER::kAnimStatic:
			case RE::COL_LAYER::kTransparent:
			case RE::COL_LAYER::kTrees:
			case RE::COL_LAYER::kProps:
			case RE::COL_LAYER::kTerrain:
			case RE::COL_LAYER::kGround:
			case RE::COL_LAYER::kInvisibleWall:
			case RE::COL_LAYER::kStairHelper:
				return true;
			default:
				return false;
			}
		}

		// ---- shapes -> triangles (and solid primitives) -------------------------

		struct Job
		{
			std::vector<Tri> tris;
			struct Box { float c[3], axis[3][3], half[3]; };
			struct Capsule { float a[3], b[3], r; };
			struct Convex { std::vector<std::array<float, 4>> planes; float lo[3], hi[3]; };
			std::vector<Box>     boxes;
			std::vector<Capsule> capsules;
			std::vector<Convex>  convexes;
			bool terrain{ false };
		};

		void EmitAabbFallback(const RE::hkpShape* a_shape, const float* a_xf, Job& a_job)
		{
			RE::hkAabb box;
			a_shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(a_xf), 0.0f, box);
			float lo[3], hi[3];
			HkAabbToSky(box, SkyrimPerHavok(), lo, hi);
			if (!Finite(lo, 3) || !Finite(hi, 3) || hi[0] - lo[0] > 4096 || hi[1] - lo[1] > 4096 || hi[2] - lo[2] > 4096) {
				return;
			}
			Job::Box b{};
			for (int i = 0; i < 3; ++i) {
				b.c[i] = (lo[i] + hi[i]) * 0.5f;
				b.half[i] = (hi[i] - lo[i]) * 0.5f;
				b.axis[i][i] = 1.0f;
			}
			a_job.boxes.push_back(b);
		}

		void Collect(const RE::hkpShape* a_shape, const float* a_xf, const float a_lo[3], const float a_hi[3], Job& a_job, int a_depth)
		{
			if (!a_shape || a_depth > 8 || a_job.tris.size() > kMaxTriangles) {
				return;
			}
			const float k = SkyrimPerHavok();
			using T = RE::hkpShapeType;
			const auto type = a_shape->type;

			switch (type) {
			case T::kMOPP:
			case T::kBVTree:
				{
					auto* bv = static_cast<const RE::hkpBvTreeShape*>(a_shape);
					// Query box -> Havok world -> shape-local (inverse transform of the 8 corners).
					float hlo[3], hhi[3];
					for (int i = 0; i < 3; ++i) {
						hlo[i] = a_lo[i] / k;
						hhi[i] = a_hi[i] / k;
					}
					float llo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, lhi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
					for (int c = 0; c < 8; ++c) {
						const float p[3] = { (c & 1) ? hhi[0] : hlo[0], (c & 2) ? hhi[1] : hlo[1], (c & 4) ? hhi[2] : hlo[2] };
						const float d[3] = { p[0] - a_xf[12], p[1] - a_xf[13], p[2] - a_xf[14] };
						for (int i = 0; i < 3; ++i) {
							const float v = a_xf[i * 4] * d[0] + a_xf[i * 4 + 1] * d[1] + a_xf[i * 4 + 2] * d[2];  // R^T d
							llo[i] = std::min(llo[i], v);
							lhi[i] = std::max(lhi[i], v);
						}
					}
					RE::hkAabb local;
					local.min = RE::hkVector4(llo[0], llo[1], llo[2], 0.0f);
					local.max = RE::hkVector4(lhi[0], lhi[1], lhi[2], 0.0f);
					static thread_local std::vector<RE::hkpShapeKey> keys(kMaxKeys);
					const auto found = std::min<std::uint32_t>(bv->QueryAabbImpl(local, keys.data(), kMaxKeys), kMaxKeys);
					const auto* container = bv->GetContainer();
					if (!container) {
						return;
					}
					for (std::uint32_t i = 0; i < found; ++i) {
						RE::hkpShapeBuffer buffer;
						Collect(container->GetChildShape(keys[i], buffer), a_xf, a_lo, a_hi, a_job, a_depth + 1);
					}
					return;
				}
			case T::kList:
			case T::kCollection:
			case T::kCompressedMesh:
			case T::kExtendedMesh:
			case T::kTriangleCollection:
			case T::kConvexList:
				{
					const auto* container = a_shape->GetContainer();
					if (!container) {
						EmitAabbFallback(a_shape, a_xf, a_job);
						return;
					}
					int guard = 0;
					for (auto key = container->GetFirstKey(); key != RE::HK_INVALID_SHAPE_KEY && guard < 200000; key = container->GetNextKey(key), ++guard) {
						RE::hkpShapeBuffer buffer;
						const auto*        child = container->GetChildShape(key, buffer);
						if (!child) {
							continue;
						}
						RE::hkAabb box;
						child->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(a_xf), 0.0f, box);
						float lo[3], hi[3];
						HkAabbToSky(box, k, lo, hi);
						if (Overlaps(lo, hi, a_lo, a_hi)) {
							Collect(child, a_xf, a_lo, a_hi, a_job, a_depth + 1);
						}
					}
					return;
				}
			case T::kTriangle:
				{
					Tri tri{};
					for (int v = 0; v < 3; ++v) {
						float w[3];
						XfPoint(a_xf, Vec(a_shape, 0x30 + v * 0x10), w);
						for (int i = 0; i < 3; ++i) {
							tri.v[v * 3 + i] = w[i] * k;
						}
					}
					if (Finite(tri.v, 9)) {
						if (a_job.terrain) {
							// The land is a height field: its outside is up, whatever the winding says.
							float e1[3], e2[3], n[3];
							Sub(tri.v + 3, tri.v, e1);
							Sub(tri.v + 6, tri.v, e2);
							Cross(e1, e2, n);
							if (n[2] < 0.0f) {
								std::swap_ranges(tri.v + 3, tri.v + 6, tri.v + 6);
							}
						}
						a_job.tris.push_back(tri);
					}
					return;
				}
			case T::kBox:
				{
					const float* half = Vec(a_shape, 0x30);
					const float  radius = Field<float>(a_shape, 0x20);
					Job::Box b{};
					const float zero[3] = { 0, 0, 0 };
					float centre[3];
					XfPoint(a_xf, zero, centre);
					for (int i = 0; i < 3; ++i) {
						b.c[i] = centre[i] * k;
						XfDir(a_xf, std::array<float, 3>{ i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f }.data(), b.axis[i]);
						b.half[i] = (half[i] + radius) * k;
					}
					if (Finite(b.c, 3) && Finite(b.half, 3)) {
						a_job.boxes.push_back(b);
					}
					return;
				}
			case T::kCapsule:
			case T::kSphere:
				{
					const float radius = Field<float>(a_shape, 0x20);
					Job::Capsule cap{};
					const float zero[3] = { 0, 0, 0 };
					float w[3];
					XfPoint(a_xf, type == T::kCapsule ? Vec(a_shape, 0x30) : zero, w);
					for (int i = 0; i < 3; ++i) {
						cap.a[i] = w[i] * k;
					}
					XfPoint(a_xf, type == T::kCapsule ? Vec(a_shape, 0x40) : zero, w);
					for (int i = 0; i < 3; ++i) {
						cap.b[i] = w[i] * k;
					}
					cap.r = radius * k;
					if (Finite(cap.a, 3) && Finite(cap.b, 3) && std::isfinite(cap.r) && cap.r < 4096.0f) {
						a_job.capsules.push_back(cap);
					}
					return;
				}
			case T::kConvexVertices:
				{
					const auto& planes = *reinterpret_cast<const RE::hkArray<RE::hkVector4>*>(reinterpret_cast<const std::uint8_t*>(a_shape) + 0x78);
					const float radius = Field<float>(a_shape, 0x20);
					if (planes.size() <= 0 || planes.size() > 512) {
						EmitAabbFallback(a_shape, a_xf, a_job);
						return;
					}
					Job::Convex cvx{};
					RE::hkAabb box;
					a_shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(a_xf), 0.0f, box);
					HkAabbToSky(box, k, cvx.lo, cvx.hi);
					for (std::int32_t i = 0; i < planes.size(); ++i) {
						alignas(16) float p[4];
						_mm_store_ps(p, planes.data()[i].quad);
						float nw[3];
						XfDir(a_xf, p, nw);
						const float dw = p[3] - (nw[0] * a_xf[12] + nw[1] * a_xf[13] + nw[2] * a_xf[14]) - radius;
						cvx.planes.push_back({ nw[0], nw[1], nw[2], dw * k });
					}
					if (Finite(cvx.lo, 3) && Finite(cvx.hi, 3)) {
						a_job.convexes.push_back(std::move(cvx));
					}
					return;
				}
			case T::kConvexTransform:
			case T::kConvexTranslate:
				{
					const auto* child = Field<const RE::hkpShape*>(a_shape, 0x30);
					alignas(16) float local[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
					if (type == T::kConvexTransform) {
						std::memcpy(local, Vec(a_shape, 0x40), sizeof(local));
					} else {
						std::memcpy(local + 12, Vec(a_shape, 0x40), sizeof(float) * 3);
					}
					if (!child || !XfLooksValid(local)) {
						EmitAabbFallback(a_shape, a_xf, a_job);
						return;
					}
					alignas(16) float composed[16];
					XfCompose(a_xf, local, composed);
					Collect(child, composed, a_lo, a_hi, a_job, a_depth + 1);
					return;
				}
			case T::kTransform:
				{
					const auto* child = Field<const RE::hkpShape*>(a_shape, 0x28);
					alignas(16) float local[16];
					std::memcpy(local, Vec(a_shape, 0x50), sizeof(local));
					if (!child || !XfLooksValid(local)) {
						EmitAabbFallback(a_shape, a_xf, a_job);
						return;
					}
					alignas(16) float composed[16];
					XfCompose(a_xf, local, composed);
					Collect(child, composed, a_lo, a_hi, a_job, a_depth + 1);
					return;
				}
			default:
				if (!s.loggedTypes[static_cast<int>(type)]) {
					s.loggedTypes[static_cast<int>(type)] = true;
					logger::info("collision: Havok shape type {} handled as its bounding box (convex: {})", static_cast<int>(type), a_shape->IsConvex());
				}
				if (a_shape->IsConvex()) {
					EmitAabbFallback(a_shape, a_xf, a_job);
				}
				return;
			}
		}

		// Havok shape layouts are partly reverse-engineered: never let a bad read take down the game.
		using CollectFn = void (*)(const RE::hkpShape*, const float*, const float*, const float*, Job*);
		bool GuardedCollect(CollectFn a_fn, const RE::hkpShape* a_shape, const float* a_xf, const float* a_lo, const float* a_hi, Job* a_job)
		{
			__try {
				a_fn(a_shape, a_xf, a_lo, a_hi, a_job);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool GuardedAabb(const RE::hkpShape* a_shape, const float* a_xf, RE::hkAabb& a_out)
		{
			__try {
				a_shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(a_xf), 0.0f, a_out);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Solid primitives as triangles wound outward.
		void Triangulate(const Job& a_job, std::vector<Tri>& a_out)
		{
			a_out.insert(a_out.end(), a_job.tris.begin(), a_job.tris.end());
			const float* centre = nullptr;
			const float* outward = nullptr;
			auto emit = [&](const float* a, const float* b, const float* c) {
				Tri t{ { a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2] }, true };
				float e1[3], e2[3], n[3];
				Sub(b, a, e1);
				Sub(c, a, e2);
				Cross(e1, e2, n);
				float dir[3] = { 0, 0, 0 };
				if (outward) {
					std::memcpy(dir, outward, sizeof(dir));
				} else if (centre) {
					for (int i = 0; i < 3; ++i) {
						dir[i] = (a[i] + b[i] + c[i]) / 3.0f - centre[i];
					}
				}
				if (Dot(n, dir) < 0.0f) {
					std::swap_ranges(t.v + 3, t.v + 6, t.v + 6);
				}
				a_out.push_back(t);
			};
			auto quad = [&](const float* a, const float* b, const float* c, const float* d) {
				emit(a, b, c);
				emit(a, c, d);
			};
			auto box = [&](const float* c, const float (*axis)[3], const float* half) {
				float corner[8][3];
				for (int i = 0; i < 8; ++i) {
					const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
					for (int k = 0; k < 3; ++k) {
						corner[i][k] = c[k] + axis[0][k] * half[0] * sx + axis[1][k] * half[1] * sy + axis[2][k] * half[2] * sz;
					}
				}
				quad(corner[0], corner[1], corner[3], corner[2]);
				quad(corner[4], corner[5], corner[7], corner[6]);
				quad(corner[0], corner[1], corner[5], corner[4]);
				quad(corner[2], corner[3], corner[7], corner[6]);
				quad(corner[0], corner[2], corner[6], corner[4]);
				quad(corner[1], corner[3], corner[7], corner[5]);
			};
			for (const auto& b : a_job.boxes) {
				centre = b.c;
				box(b.c, b.axis, b.half);
			}
			for (const auto& cap : a_job.capsules) {
				// Capsules as boxes around them: Chief's pill slides off a
				// box as well, and trunks and posts are what these are.
				float ab[3];
				Sub(cap.b, cap.a, ab);
				const float len = std::sqrt(Dot(ab, ab));
				float axis[3][3]{};
				if (len > 1e-4f) {
					for (int k = 0; k < 3; ++k) {
						axis[2][k] = ab[k] / len;
					}
				} else {
					axis[2][2] = 1.0f;
				}
				const float ref[3] = { std::fabs(axis[2][2]) < 0.9f ? 0.0f : 1.0f, 0.0f, std::fabs(axis[2][2]) < 0.9f ? 1.0f : 0.0f };
				Cross(ref, axis[2], axis[0]);
				const float l0 = std::sqrt(Dot(axis[0], axis[0]));
				for (int k = 0; k < 3; ++k) {
					axis[0][k] /= l0;
				}
				Cross(axis[2], axis[0], axis[1]);
				const float c[3] = { (cap.a[0] + cap.b[0]) * 0.5f, (cap.a[1] + cap.b[1]) * 0.5f, (cap.a[2] + cap.b[2]) * 0.5f };
				const float half[3] = { cap.r, cap.r, len * 0.5f + cap.r };
				centre = c;
				box(c, axis, half);
			}
			// Convex hull from planes: clip a big square on each plane by all the other planes.
			centre = nullptr;
			for (const auto& cvx : a_job.convexes) {
				const float ex = cvx.hi[0] - cvx.lo[0], ey = cvx.hi[1] - cvx.lo[1], ez = cvx.hi[2] - cvx.lo[2];
				const float diag = std::sqrt(ex * ex + ey * ey + ez * ez) + 1.0f;
				const float mid[3] = { (cvx.lo[0] + cvx.hi[0]) * 0.5f, (cvx.lo[1] + cvx.hi[1]) * 0.5f, (cvx.lo[2] + cvx.hi[2]) * 0.5f };
				for (std::size_t i = 0; i < cvx.planes.size(); ++i) {
					const auto& pl = cvx.planes[i];
					const float n[3] = { pl[0], pl[1], pl[2] };
					const float nl = std::sqrt(Dot(n, n));
					if (nl < 1e-6f) {
						continue;
					}
					const float dist = (Dot(n, mid) + pl[3]) / (nl * nl);
					const float o[3] = { mid[0] - n[0] * dist, mid[1] - n[1] * dist, mid[2] - n[2] * dist };
					const float ref[3] = { std::fabs(n[2]) < 0.9f * nl ? 0.0f : 1.0f, 0.0f, std::fabs(n[2]) < 0.9f * nl ? 1.0f : 0.0f };
					float t1[3], t2[3];
					Cross(ref, n, t1);
					const float lt = std::sqrt(Dot(t1, t1));
					for (int k = 0; k < 3; ++k) {
						t1[k] /= lt;
					}
					Cross(n, t1, t2);
					const float l2 = std::sqrt(Dot(t2, t2));
					for (int k = 0; k < 3; ++k) {
						t2[k] /= l2;
					}
					std::vector<std::array<float, 3>> poly;
					const float sgn1[4] = { -1, 1, 1, -1 }, sgn2[4] = { -1, -1, 1, 1 };
					for (int q = 0; q < 4; ++q) {
						const float s1 = sgn1[q] * diag, s2 = sgn2[q] * diag;
						poly.push_back({ o[0] + t1[0] * s1 + t2[0] * s2, o[1] + t1[1] * s1 + t2[1] * s2, o[2] + t1[2] * s1 + t2[2] * s2 });
					}
					for (std::size_t j = 0; j < cvx.planes.size() && poly.size() >= 3; ++j) {
						if (j == i) {
							continue;
						}
						const auto& cp = cvx.planes[j];
						std::vector<std::array<float, 3>> out;
						for (std::size_t v = 0; v < poly.size(); ++v) {
							const auto& A = poly[v];
							const auto& B = poly[(v + 1) % poly.size()];
							const float da = cp[0] * A[0] + cp[1] * A[1] + cp[2] * A[2] + cp[3];
							const float db = cp[0] * B[0] + cp[1] * B[1] + cp[2] * B[2] + cp[3];
							if (da <= 0.0f) {
								out.push_back(A);
							}
							if ((da <= 0.0f) != (db <= 0.0f)) {
								const float t = da / (da - db);
								out.push_back({ A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, A[2] + (B[2] - A[2]) * t });
							}
						}
						poly.swap(out);
					}
					outward = n;
					for (std::size_t v = 1; v + 1 < poly.size(); ++v) {
						emit(poly[0].data(), poly[v].data(), poly[v + 1].data());
					}
					outward = nullptr;
				}
			}
		}

		void GatherBodies(RE::hkpWorld* a_world)
		{
			s.bodies.clear();
			const float k = SkyrimPerHavok();
			auto addIsland = [&](RE::hkpSimulationIsland* a_island) {
				if (!a_island) {
					return;
				}
				auto& entities = a_island->entities;
				for (std::int32_t i = 0; i < entities.size(); ++i) {
					auto* entity = entities.data()[i];
					if (!entity) {
						continue;
					}
					const auto& collidable = entity->collidable;
					if (!Included(collidable.GetCollisionLayer())) {
						continue;
					}
					const auto* shape = collidable.shape;
					const auto* xf = static_cast<const float*>(collidable.motion);
					if (!shape || !xf || !Finite(xf, 16)) {
						continue;
					}
					RE::hkAabb box;
					if (!GuardedAabb(shape, xf, box)) {
						continue;
					}
					const auto layer = collidable.GetCollisionLayer();
					Body body{ shape, xf, {}, {}, layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround };
					HkAabbToSky(box, k, body.lo, body.hi);
					if (Finite(body.lo, 3) && Finite(body.hi, 3)) {
						s.bodies.push_back(body);
					}
				}
			};
			addIsland(a_world->fixedIsland);
			for (std::int32_t i = 0; i < a_world->activeSimulationIslands.size(); ++i) {
				addIsland(a_world->activeSimulationIslands.data()[i]);
			}
			for (std::int32_t i = 0; i < a_world->inactiveSimulationIslands.size(); ++i) {
				addIsland(a_world->inactiveSimulationIslands.data()[i]);
			}
		}

		std::uint64_t RegionKey(int a_x, int a_y, int a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		}

		// Splits triangles longer than kMaxEdge (at the middle of the longest
		// edge) until none is: a big one (a box's face is two triangles, and
		// interiors' floors are big boxes) would go only to the region of its
		// centre, which may be out of reach while the player stands on it.
		void Subdivide(std::vector<Tri>& a_tris)
		{
			static constexpr float kMaxEdge = 256.0f;
			for (std::size_t i = 0; i < a_tris.size(); ++i) {
				for (;;) {
					Tri& t = a_tris[i];
					float best = 0.0f;
					int edge = 0;
					for (int e = 0; e < 3; ++e) {
						const float* a = &t.v[e * 3];
						const float* b = &t.v[((e + 1) % 3) * 3];
						const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
						const float length = Dot(d, d);
						if (length > best) {
							best = length;
							edge = e;
						}
					}
					if (!(best > kMaxEdge * kMaxEdge) || a_tris.size() > 4'000'000) {
						break;
					}
					// edge a->b, opposite corner c: (a, m, c) stays, (m, b, c) is added
					const int a = edge, b = (edge + 1) % 3;
					float m[3];
					for (int k = 0; k < 3; ++k) {
						m[k] = (t.v[a * 3 + k] + t.v[b * 3 + k]) * 0.5f;
					}
					Tri other = t;
					for (int k = 0; k < 3; ++k) {
						t.v[b * 3 + k] = m[k];
						other.v[a * 3 + k] = m[k];
					}
					a_tris.push_back(other);  // may move a_tris: t is not used after this
				}
			}
		}

		// One region's triangles: every triangle whose centre is in it (so a
		// triangle is in exactly one region). Big ones are split first.
		Pending Harvest(int a_rx, int a_ry, int a_rz)
		{
			const float lo[3] = { a_rx * CR_REGION_UNITS, a_ry * CR_REGION_UNITS, a_rz * CR_REGION_UNITS };
			const float hi[3] = { lo[0] + CR_REGION_UNITS, lo[1] + CR_REGION_UNITS, lo[2] + CR_REGION_UNITS };
			Pending out{ a_rx, a_ry, a_rz, {}, 0, false };
			static constexpr CollectFn collect = [](const RE::hkpShape* a_shape, const float* a_xf, const float* a_lo, const float* a_hi, Job* a_job) {
				Collect(a_shape, a_xf, a_lo, a_hi, *a_job, 0);
			};
			for (const auto& body : s.bodies) {
				if (!Overlaps(body.lo, body.hi, lo, hi)) {
					continue;
				}
				Job job;
				job.terrain = body.terrain;
				if (!GuardedCollect(collect, body.shape, body.xf, lo, hi, &job)) {
					if (!s.loggedTypes[-1]) {
						s.loggedTypes[-1] = true;
						logger::warn("collision: faulted reading a Havok shape (type {}); skipping it", static_cast<int>(body.shape->type));
					}
					continue;
				}
				std::vector<Tri> tris;
				Triangulate(job, tris);
				Subdivide(tris);
				for (const auto& t : tris) {
					const float c[3] = { (t.v[0] + t.v[3] + t.v[6]) / 3.0f, (t.v[1] + t.v[4] + t.v[7]) / 3.0f, (t.v[2] + t.v[5] + t.v[8]) / 3.0f };
					if (c[0] < lo[0] || c[0] >= hi[0] || c[1] < lo[1] || c[1] >= hi[1] || c[2] < lo[2] || c[2] >= hi[2]) {
						continue;
					}
					cr_triangle tri{};
					for (int v = 0; v < 3; ++v) {
						tri.v[v] = { t.v[v * 3], t.v[v * 3 + 1], t.v[v * 3 + 2] };
					}
					// The land, and closed shapes' faces, are solid only from outside:
					// a back would pull whoever dips into them further in.
					tri.flags = (job.terrain || t.solid) ? CR_TRIANGLE_ONE_SIDED : 0;
					if (job.terrain) {
						tri.flags |= CR_TRIANGLE_LAND;
					}
					out.tris.push_back(tri);
				}
			}
			return out;
		}

		// FNV-1a over a region's triangles: a region is sent again only when it changed.
		std::uint64_t HashOf(const std::vector<cr_triangle>& a_tris)
		{
			std::uint64_t h = 1469598103934665603ull;
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_tris.data());
			for (std::size_t i = 0; i < a_tris.size() * sizeof(cr_triangle); ++i) {
				h = (h ^ bytes[i]) * 1099511628211ull;
			}
			return h ^ a_tris.size();
		}

		// Sends as much of the outbox as the ring has room for.
		void Flush()
		{
			auto& link = Link::Get();
			static cr_msg_collision_tris message;
			while (!s.outbox.empty()) {
				auto& region = s.outbox.front();
				const auto total = static_cast<std::uint32_t>(region.tris.size());
				do {
					const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(CR_TRIS_PER_MESSAGE, total - region.sent));
					message.epoch = s.epoch;
					message.rx = region.rx;
					message.ry = region.ry;
					message.rz = region.rz;
					message.total = total;
					message.first = static_cast<std::uint32_t>(region.sent);
					message.count = count;
					if (count) {
						std::memcpy(message.tris, region.tris.data() + region.sent, sizeof(cr_triangle) * count);
					}
					if (!link.PushRaw(CR_MSG_COLLISION_TRIS, &message, CR_COLLISION_TRIS_SIZE(count))) {
						return;  // the ring is full: the rest next frame
					}
					region.sent += count;
					region.started = true;
				} while (region.sent < total);
				++s.regionsSent;
				s.trianglesSent += total;
				s.outbox.erase(s.outbox.begin());
			}
		}
	}

	void Reset(std::uint32_t a_worldGeneration)
	{
		++s.epoch;
		s.harvested.clear();
		s.sentHash.clear();
		s.outbox.clear();
		cr_msg_collision_reset reset{};
		reset.epoch = s.epoch;
		reset.world_generation = a_worldGeneration;
		Link::Get().PushRaw(CR_MSG_COLLISION_RESET, &reset, sizeof(reset));
		logger::info("collision: reset (epoch {})", s.epoch);
	}

	void Update(RE::PlayerCharacter* a_player)
	{
		if (s.offsets.empty()) {
			for (int dx = -RadiusXY(); dx <= RadiusXY(); ++dx) {
				for (int dy = -RadiusXY(); dy <= RadiusXY(); ++dy) {
					for (int dz = -kBelow; dz <= kAbove; ++dz) {
						s.offsets.push_back({ dx, dy, dz });
					}
				}
			}
			std::ranges::sort(s.offsets, {}, [](const auto& o) { return o[0] * o[0] + o[1] * o[1] + o[2] * o[2] * 2; });
		}

		Flush();
		if (!s.outbox.empty()) {
			return;  // Halo is still taking the last ones
		}

		auto* cell = a_player->GetParentCell();
		auto* bhk = cell ? cell->GetbhkWorld() : nullptr;
		auto* world = bhk ? bhk->GetWorld1() : nullptr;
		if (!world) {
			return;
		}

		const auto pos = a_player->GetPosition();
		const int prx = static_cast<int>(std::floor(pos.x / CR_REGION_UNITS));
		const int pry = static_cast<int>(std::floor(pos.y / CR_REGION_UNITS));
		const int prz = static_cast<int>(std::floor(pos.z / CR_REGION_UNITS));
		const auto now = Clock::now();
		const auto start = now;
		int  done = 0;
		bool gathered = false;

		for (const auto& o : s.offsets) {
			const int rx = prx + o[0], ry = pry + o[1], rz = prz + o[2];
			const auto key = RegionKey(rx, ry, rz);
			const auto it = s.harvested.find(key);
			const bool isNear = std::abs(o[0]) <= 1 && std::abs(o[1]) <= 1 && o[2] >= -1 && o[2] <= 0;
			if (it != s.harvested.end() && !(isNear && now - it->second > kRefreshNear)) {
				continue;
			}
			if (!gathered) {
				RE::BSReadLockGuard lock(bhk->worldLock);
				GatherBodies(world);
				gathered = true;
			}
			Pending region;
			{
				RE::BSReadLockGuard lock(bhk->worldLock);
				region = Harvest(rx, ry, rz);
			}
			s.harvested[key] = now;
			const auto hash = HashOf(region.tris);
			const auto sent = s.sentHash.find(key);
			if (sent == s.sentHash.end() || sent->second != hash) {
				s.sentHash[key] = hash;  // new, or it changed (a door opened)
				s.outbox.push_back(std::move(region));
			}
			if (++done >= kMaxRegionsPerFrame || Clock::now() - start > kFrameBudget) {
				break;
			}
		}
		Flush();

		// Forget far regions (Halo drops them too): they are harvested again
		// when the player comes back. Never wholesale: that re-sent everything.
		if (s.harvested.size() > s.offsets.size() * 2) {
			const auto isFar = [&](std::uint64_t a_key) {
				const auto unpack = [](std::uint64_t v) { return static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v & 0x1FFFFF) << 11) >> 11); };
				return std::abs(unpack(a_key >> 42) - prx) > RadiusXY() + 2 || std::abs(unpack(a_key >> 21) - pry) > RadiusXY() + 2 ||
				       std::abs(unpack(a_key) - prz) > kBelow + 2;
			};
			std::erase_if(s.harvested, [&](const auto& a_entry) { return isFar(a_entry.first); });
			std::erase_if(s.sentHash, [&](const auto& a_entry) { return isFar(a_entry.first); });
		}
		if (now - s.lastLog > std::chrono::seconds(10) && s.regionsSent) {
			logger::info("collision: {} regions, {} triangles sent so far", s.regionsSent, s.trianglesSent);
			s.lastLog = now;
		}
	}
}
