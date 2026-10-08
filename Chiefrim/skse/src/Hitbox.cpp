/* SPDX-License-Identifier: GPL-3.0-or-later */
// An actor's hit shapes for Halo (docs §8.1): the rigid bodies on its
// skeleton's bones, which Skyrim keeps in the world, moved with the
// animation, for its own arrows and spells to hit. The Havok shape layouts
// are those Collision.cpp reads.
#include "Hitbox.h"

#include <vector>

namespace chiefrim::Hitbox
{
	namespace
	{
		constexpr const char* kPersonHead = "NPC Head [Head]";  // humanoid skeletons' (people, draugr, falmer)
		constexpr int         kLoggedActors = 12;

		struct Capsule
		{
			float         a[3], b[3], r;  // Havok units, world
			std::uint32_t flags;
		};

		struct Body
		{
			RE::NiPointer<RE::bhkNiCollisionObject> object;
			bool                                    head;
		};

		struct Entry
		{
			RE::NiPointer<RE::NiAVObject> root;  // the 3D its bodies were found in
			std::vector<Body>             bodies;
			bool                          seen{ false };
			bool                          logged{ false };
		};

		struct
		{
			std::unordered_map<RE::FormID, Entry> actors;
			std::unordered_map<int, bool>         loggedTypes;
			int                                   loggedActors = 0;
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

		// hkTransform: rotation columns, then translation
		void XfPoint(const float* a_xf, const float* a_p, float* a_out)
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_xf[i] * a_p[0] + a_xf[4 + i] * a_p[1] + a_xf[8 + i] * a_p[2] + a_xf[12 + i];
			}
		}

		void XfCompose(const float* a_parent, const float* a_child, float* a_out)
		{
			for (int c = 0; c < 3; ++c) {
				for (int i = 0; i < 3; ++i) {
					a_out[c * 4 + i] = a_parent[i] * a_child[c * 4] + a_parent[4 + i] * a_child[c * 4 + 1] + a_parent[8 + i] * a_child[c * 4 + 2];
				}
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
				if (std::fabs(col[0] * col[0] + col[1] * col[1] + col[2] * col[2] - 1.0f) > 0.05f) {
					return false;
				}
			}
			return true;
		}

		struct Job
		{
			std::vector<Capsule>* out;
			std::uint32_t         flags;
		};

		void Push(Job& a_job, const float* a_xf, const float* a_a, const float* a_b, float a_r)
		{
			Capsule cap{};
			XfPoint(a_xf, a_a, cap.a);
			XfPoint(a_xf, a_b, cap.b);
			cap.r = a_r;
			cap.flags = a_job.flags;
			if (Finite(cap.a, 3) && Finite(cap.b, 3) && std::isfinite(cap.r) && cap.r > 0.0f && cap.r < 100.0f) {
				a_job.out->push_back(cap);
			}
		}

		// Any convex shape: the capsule along the longest side of its own box
		void PushBox(const RE::hkpShape* a_shape, const float* a_xf, Job& a_job)
		{
			alignas(16) static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			RE::hkAabb box;
			a_shape->GetAabbImpl(*reinterpret_cast<const RE::hkTransform*>(identity), 0.0f, box);
			alignas(16) float lo[4], hi[4];
			_mm_store_ps(lo, box.min.quad);
			_mm_store_ps(hi, box.max.quad);
			if (!Finite(lo, 3) || !Finite(hi, 3)) {
				return;
			}
			float half[3], centre[3];
			int   longest = 0;
			for (int i = 0; i < 3; ++i) {
				half[i] = std::max(0.0f, (hi[i] - lo[i]) * 0.5f);
				centre[i] = (hi[i] + lo[i]) * 0.5f;
				if (half[i] > half[longest]) {
					longest = i;
				}
			}
			const float r = 0.5f * (half[(longest + 1) % 3] + half[(longest + 2) % 3]);
			float       a[3] = { centre[0], centre[1], centre[2] }, b[3] = { centre[0], centre[1], centre[2] };
			a[longest] -= std::max(0.0f, half[longest] - r);
			b[longest] += std::max(0.0f, half[longest] - r);
			Push(a_job, a_xf, a, b, r);
		}

		void Collect(const RE::hkpShape* a_shape, const float* a_xf, Job& a_job, int a_depth)
		{
			if (!a_shape || a_depth > 6) {
				return;
			}
			using T = RE::hkpShapeType;
			const auto type = a_shape->type;
			switch (type) {
			case T::kCapsule:
				Push(a_job, a_xf, Vec(a_shape, 0x30), Vec(a_shape, 0x40), Field<float>(a_shape, 0x20));
				return;
			case T::kSphere:
				{
					const float zero[3] = { 0, 0, 0 };
					Push(a_job, a_xf, zero, zero, Field<float>(a_shape, 0x20));
					return;
				}
			case T::kConvexTransform:
			case T::kConvexTranslate:
			case T::kTransform:
				{
					const bool        plain = type == T::kTransform;
					const auto*       child = Field<const RE::hkpShape*>(a_shape, plain ? 0x28 : 0x30);
					alignas(16) float local[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
					if (type == T::kConvexTranslate) {
						std::memcpy(local + 12, Vec(a_shape, 0x40), sizeof(float) * 3);
					} else {
						std::memcpy(local, Vec(a_shape, plain ? 0x50 : 0x40), sizeof(local));
					}
					if (!child || !XfLooksValid(local)) {
						if (a_shape->IsConvex()) {
							PushBox(a_shape, a_xf, a_job);
						}
						return;
					}
					alignas(16) float composed[16];
					XfCompose(a_xf, local, composed);
					Collect(child, composed, a_job, a_depth + 1);
					return;
				}
			case T::kList:
			case T::kConvexList:
				{
					const auto* container = a_shape->GetContainer();
					if (!container) {
						return;
					}
					int guard = 0;
					for (auto key = container->GetFirstKey(); key != RE::HK_INVALID_SHAPE_KEY && guard < 64; key = container->GetNextKey(key), ++guard) {
						RE::hkpShapeBuffer buffer;
						Collect(container->GetChildShape(key, buffer), a_xf, a_job, a_depth + 1);
					}
					return;
				}
			default:
				if (!s.loggedTypes[static_cast<int>(type)]) {
					s.loggedTypes[static_cast<int>(type)] = true;
					logger::info("hitbox: a body's Havok shape type {} taken as the capsule of its box (convex: {})", static_cast<int>(type), a_shape->IsConvex());
				}
				if (a_shape->IsConvex()) {
					PushBox(a_shape, a_xf, a_job);
				}
				return;
			}
		}

		// The layouts are partly reverse-engineered: a bad read mustn't take down the game.
		using CollectFn = void (*)(const RE::hkpShape*, const float*, Job*);
		bool Guarded(CollectFn a_fn, const RE::hkpShape* a_shape, const float* a_xf, Job* a_job)
		{
			__try {
				a_fn(a_shape, a_xf, a_job);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool BipedLayer(RE::COL_LAYER a_layer)
		{
			return a_layer == RE::COL_LAYER::kBiped || a_layer == RE::COL_LAYER::kBipedNoCC || a_layer == RE::COL_LAYER::kDeadBip;
		}

		void Find(Entry& a_entry, RE::NiAVObject* a_root)
		{
			a_entry.root.reset(a_root);
			a_entry.bodies.clear();
			RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) {
				const auto* node = a_object->sceneObject;
				a_entry.bodies.push_back({ RE::NiPointer<RE::bhkNiCollisionObject>(a_object), node && node->name == kPersonHead });
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// Its skeleton's bodies, in the world now; false if none are, or they
		// aren't where the actor is (not moved with it: a ragdoll not driven)
		bool FromBodies(const Entry& a_entry, const RE::NiPoint3& a_feet, float a_height, std::vector<Capsule>& a_out, int& a_bodies)
		{
			static constexpr CollectFn collect = [](const RE::hkpShape* a_shape, const float* a_xf, Job* a_job) { Collect(a_shape, a_xf, *a_job, 0); };
			const float k = SkyrimPerHavok();
			a_bodies = 0;
			for (const auto& body : a_entry.bodies) {
				auto* world = body.object ? body.object->body.get() : nullptr;
				auto* object = world ? static_cast<RE::hkpWorldObject*>(world->referencedObject.get()) : nullptr;
				if (!object || !object->world || !BipedLayer(object->collidable.GetCollisionLayer())) {
					continue;
				}
				const auto* shape = object->collidable.shape;
				const auto* xf = static_cast<const float*>(object->collidable.motion);
				if (!shape || !xf || !XfLooksValid(xf)) {
					continue;
				}
				Job job{ &a_out, body.head ? CR_HITBOX_HEAD : 0u };
				Guarded(collect, shape, xf, &job);
				++a_bodies;
			}
			// where the actor is: none further from its feet than its height and a margin
			const float reach = (a_height * 1.5f + 256.0f) / k;
			const float feet[3] = { a_feet.x / k, a_feet.y / k, a_feet.z / k };
			std::size_t nearby = 0;
			for (const auto& cap : a_out) {
				float d = 0.0f;
				for (int i = 0; i < 3; ++i) {
					const float m = 0.5f * (cap.a[i] + cap.b[i]) - feet[i];
					d += m * m;
				}
				nearby += std::sqrt(d) <= reach;
			}
			if (a_out.empty() || nearby * 2 < a_out.size()) {
				a_out.clear();
				return false;
			}
			return true;
		}

		// No bodies: one capsule from its bounds, stood up or laid along its
		// heading (a horse, a mudcrab), as tall as it is; and a person's head
		void FromBounds(RE::Actor* a_actor, const RE::NiPoint3& a_feet, float a_height, std::vector<Capsule>& a_out)
		{
			const float k = SkyrimPerHavok();
			const auto  lo = a_actor->GetBoundMin();
			const auto  hi = a_actor->GetBoundMax();
			// the bounds' proportions, at the actor's height (scaled or not, as they come)
			const float scale = hi.z - lo.z > 1.0f ? a_height / (hi.z - lo.z) : 1.0f;
			float       width = (hi.x - lo.x) * scale, length = (hi.y - lo.y) * scale;
			if (!(width > 1.0f) || !(length > 1.0f) || width > 4096.0f || length > 4096.0f) {
				width = length = a_height * 0.3f;
			}
			const float heading = a_actor->GetAngleZ();
			const float fx = std::sin(heading), fy = std::cos(heading);  // forward (0: north)
			const float rx = fy, ry = -fx;                                // right
			const float cx = 0.5f * (lo.x + hi.x) * scale, cy = 0.5f * (lo.y + hi.y) * scale;
			const float centre[2] = { a_feet.x + rx * cx + fx * cy, a_feet.y + ry * cx + fy * cy };
			Capsule     cap{};
			if (length > a_height) {
				cap.r = 0.5f * std::min(width, a_height);
				const float half = std::max(0.0f, 0.5f * length - cap.r);
				const float z = a_feet.z + a_height - cap.r;
				cap.a[0] = centre[0] - fx * half, cap.a[1] = centre[1] - fy * half, cap.a[2] = z;
				cap.b[0] = centre[0] + fx * half, cap.b[1] = centre[1] + fy * half, cap.b[2] = z;
			} else {
				cap.r = 0.5f * std::min(width, length);
				cap.a[0] = cap.b[0] = centre[0];
				cap.a[1] = cap.b[1] = centre[1];
				cap.a[2] = a_feet.z + cap.r;
				cap.b[2] = a_feet.z + std::max(cap.r, a_height - cap.r);
			}
			cap.flags = CR_HITBOX_BOUNDS;
			for (int i = 0; i < 3; ++i) {
				cap.a[i] /= k;
				cap.b[i] /= k;
			}
			cap.r /= k;
			a_out.push_back(cap);

			auto* root = a_actor->Get3D(false);
			auto* head = root ? root->GetObjectByName(kPersonHead) : nullptr;
			if (head) {
				Capsule ball{};
				const auto& at = head->world.translate;
				ball.a[0] = ball.b[0] = at.x / k;
				ball.a[1] = ball.b[1] = at.y / k;
				ball.a[2] = ball.b[2] = at.z / k;
				ball.r = a_height * 0.075f / k;
				ball.flags = CR_HITBOX_BOUNDS | CR_HITBOX_HEAD;
				a_out.push_back(ball);
			}
		}
	}

	std::uint32_t Collect(RE::Actor* a_actor, cr_hitbox* a_out, std::uint32_t a_max)
	{
		static std::vector<Capsule> caps;
		caps.clear();
		auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root || a_max == 0) {
			return 0;
		}
		auto& entry = s.actors[a_actor->GetFormID()];
		entry.seen = true;
		if (entry.root.get() != root) {
			Find(entry, root);
			entry.logged = false;
		}
		const auto  feet = a_actor->GetPosition();
		const float height = std::clamp(a_actor->GetHeight(), 20.0f, 2000.0f);
		int         bodies = 0;
		const bool  own = FromBodies(entry, feet, height, caps, bodies);
		if (!own) {
			FromBounds(a_actor, feet, height, caps);
		}
		// the head first: it stays if there are more than fit
		std::stable_partition(caps.begin(), caps.end(), [](const Capsule& a_cap) { return (a_cap.flags & CR_HITBOX_HEAD) != 0; });

		const float   k = SkyrimPerHavok();
		std::uint32_t count = 0;
		for (const auto& cap : caps) {
			if (count >= a_max) {
				break;
			}
			auto& out = a_out[count++];
			out.a = { cap.a[0] * k, cap.a[1] * k, cap.a[2] * k };
			out.b = { cap.b[0] * k, cap.b[1] * k, cap.b[2] * k };
			out.radius = cap.r * k;
			out.flags = cap.flags;
		}
		if (!entry.logged && s.loggedActors < kLoggedActors) {
			entry.logged = true;
			++s.loggedActors;
			const bool head = std::any_of(caps.begin(), caps.end(), [](const Capsule& a_cap) { return (a_cap.flags & CR_HITBOX_HEAD) != 0; });
			if (own) {
				logger::info("hitbox: {} ({:08X}): {} shapes from {} of its {} bodies{}{}", a_actor->GetName(), a_actor->GetFormID(), count, bodies,
					entry.bodies.size(), head ? ", a person's head" : "", caps.size() > count ? std::format(" ({} left out)", caps.size() - count) : "");
			} else {
				logger::info("hitbox: {} ({:08X}): from its bounds ({} bodies found, {} in the world where it is){}", a_actor->GetName(),
					a_actor->GetFormID(), entry.bodies.size(), bodies, head ? ", and a person's head" : "");
			}
		}
		return count;
	}

	void EndFrame()
	{
		std::erase_if(s.actors, [](auto& a_entry) {
			if (!a_entry.second.seen) {
				return true;
			}
			a_entry.second.seen = false;
			return false;
		});
	}
}
