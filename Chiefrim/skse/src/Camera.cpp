/* SPDX-License-Identifier: GPL-3.0-or-later */
// The camera hooks follow SkyCraft's (MIT; THIRD-PARTY-NOTICES.md): the
// first-person camera state's translation, every call site of
// PlayerCamera::Update, and a check of Skyrim's camera-root axes before
// Chiefrim turns the camera itself.
#include "Camera.h"

#include "Settings.h"

#include <vector>

namespace chiefrim::Camera
{
	namespace
	{
		constexpr float kRadToDeg = 57.29577951f;

		struct State
		{
			bool          driving{ false };
			RE::NiPoint3  eye{};
			RE::NiMatrix3 rotation{};   // columns: forward, up, right (NiCamera)
			// Skyrim's camera-root convention, checked against Chief's view
			// before Chiefrim turns the root itself: for each column, which of
			// +/- forward, up, right it carries.
			std::array<std::array<int, 6>, 3> votes{};
			int                axisSamples{ 0 };
			std::array<int, 3> axisMap{ 0, 2, 4 };
			bool               axesKnown{ false };
			bool               axesRejected{ false };
			// Skyrim's own settings while Chief drives
			bool  useHaloFov{ true };   // [Camera] bUseHaloFov
			bool  fovSaved{ false };
			float savedWorldFov{ 0.0f };
			std::vector<RE::NiPointer<RE::BSGeometry>> hiddenArms;
		} s;

		RE::NiPoint3 Column(const RE::NiMatrix3& a_m, int a_c)
		{
			return { a_m.entry[0][a_c], a_m.entry[1][a_c], a_m.entry[2][a_c] };
		}

		float AngleDeg(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b)
		{
			const float la = a_a.Length(), lb = a_b.Length();
			if (la < 1e-6f || lb < 1e-6f) {
				return 180.0f;
			}
			return std::acos(std::clamp(a_a.Dot(a_b) / (la * lb), -1.0f, 1.0f)) * kRadToDeg;
		}

		// Chief's camera basis is in Halo's axes, which are Skyrim's (docs §4:
		// a scale and an offset only), so it goes in as it is.
		RE::NiMatrix3 RotationFrom(const cr_vec3& a_forward, const cr_vec3& a_up)
		{
			RE::NiPoint3 f{ a_forward.x, a_forward.y, a_forward.z };
			RE::NiPoint3 u{ a_up.x, a_up.y, a_up.z };
			f.Unitize();
			RE::NiPoint3 r = f.Cross(u);
			r.Unitize();
			u = r.Cross(f);
			RE::NiMatrix3 m;
			const float fv[3] = { f.x, f.y, f.z }, uv[3] = { u.x, u.y, u.z }, rv[3] = { r.x, r.y, r.z };
			for (int i = 0; i < 3; ++i) {
				m.entry[i][0] = fv[i];
				m.entry[i][1] = uv[i];
				m.entry[i][2] = rv[i];
			}
			return m;
		}

		// Halo sends the vertical angle it renders with; Skyrim's setting is
		// the horizontal angle of a 4:3 view.
		float SkyrimFovDeg(float a_verticalRad)
		{
			const float half = std::clamp(a_verticalRad, 0.05f, 2.8f) * 0.5f;
			return 2.0f * std::atan(std::tan(half) * (4.0f / 3.0f)) * kRadToDeg;
		}

		// Learns which columns of Skyrim's camera-root matrix are forward, up
		// and right, while Skyrim still turns it (from the player's angles,
		// which follow Chief). Only when the answer is clear does Chiefrim
		// turn the root itself; otherwise Skyrim keeps doing it.
		void LearnAxes(const RE::NiMatrix3& a_skyrim)
		{
			if (s.axesKnown || s.axesRejected) {
				return;
			}
			const RE::NiPoint3 f = Column(s.rotation, 0), u = Column(s.rotation, 1), r = Column(s.rotation, 2);
			const std::array<RE::NiPoint3, 6> candidates{ f, f * -1.0f, u, u * -1.0f, r, r * -1.0f };
			for (int c = 0; c < 3; ++c) {
				int   best = -1;
				float bestAngle = 1e9f;
				for (int k = 0; k < 6; ++k) {
					const float angle = AngleDeg(Column(a_skyrim, c), candidates[k]);
					if (angle < bestAngle) {
						bestAngle = angle;
						best = k;
					}
				}
				if (bestAngle < 8.0f) {
					++s.votes[c][best];
				}
			}
			if (++s.axisSamples < 180) {
				return;
			}
			bool ok = true;
			std::array<bool, 3> used{};
			for (int c = 0; c < 3; ++c) {
				const auto it = std::ranges::max_element(s.votes[c]);
				const int  k = static_cast<int>(it - s.votes[c].begin());
				ok &= *it > s.axisSamples * 6 / 10 && !used[k / 2];
				used[k / 2] = true;
				s.axisMap[c] = k;
			}
			static constexpr const char* kNames[6] = { "+forward", "-forward", "+up", "-up", "+right", "-right" };
			s.axesKnown = ok;
			s.axesRejected = !ok;
			logger::info("camera root axes: {} {} {} -> {}", kNames[s.axisMap[0]], kNames[s.axisMap[1]], kNames[s.axisMap[2]],
				ok ? "Chief's view now turns the camera" : "unclear; Skyrim keeps turning it (from Chief's angles)");
		}

		void TurnRoot(RE::NiAVObject* a_root)
		{
			const RE::NiPoint3 f = Column(s.rotation, 0), u = Column(s.rotation, 1), r = Column(s.rotation, 2);
			const std::array<RE::NiPoint3, 6> candidates{ f, f * -1.0f, u, u * -1.0f, r, r * -1.0f };
			RE::NiMatrix3 m;
			for (int c = 0; c < 3; ++c) {
				const auto& v = candidates[s.axisMap[c]];
				m.entry[0][c] = v.x;
				m.entry[1][c] = v.y;
				m.entry[2][c] = v.z;
			}
			a_root->local.rotate = m;
			a_root->world.rotate = m;
		}

		void PinRoot(RE::PlayerCamera* a_camera)
		{
			auto* root = a_camera->cameraRoot.get();
			const bool identityParent = !root->parent || root->parent->world.translate.Length() < 0.001f;
			if (identityParent) {
				root->local.translate = s.eye;
			}
			root->world.translate = s.eye;
			a_camera->GetRuntimeData2().pos = s.eye;
			if (auto* sky = RE::Sky::GetSingleton(); sky && sky->root) {
				sky->root->local.translate = s.eye;
				sky->root->world.translate = s.eye;
			}
		}

		// Skyrim's first-person arms and weapon would hang in Chief's view
		// (his own arrive with the Phase 2 overlay). Only meshes are hidden,
		// never nodes, and exactly those are shown again.
		void HideArms(RE::PlayerCharacter* a_player, bool a_hide)
		{
			if (!a_hide) {
				for (auto& mesh : s.hiddenArms) {
					if (mesh && mesh->GetAppCulled()) {
						mesh->SetAppCulled(false);
					}
				}
				s.hiddenArms.clear();
				return;
			}
			if (auto* root = a_player->Get3D(true)) {
				RE::BSVisit::TraverseScenegraphGeometries(root, [](RE::BSGeometry* a_mesh) {
					if (!a_mesh->GetAppCulled()) {
						a_mesh->SetAppCulled(true);
						s.hiddenArms.emplace_back(a_mesh);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}

		// After Skyrim's whole camera update: Chief's eye and view, so nothing
		// downstream uses Skyrim's own (animated, smoothed) camera.
		struct PlayerCameraUpdateHook
		{
			static void thunk(RE::PlayerCamera* a_this)
			{
				func(a_this);
				if (!s.driving || !a_this->cameraRoot || !a_this->IsInFirstPerson()) {
					return;
				}
				auto* root = a_this->cameraRoot.get();
				LearnAxes(root->world.rotate);
				if (s.axesKnown) {
					TurnRoot(root);
				}
				PinRoot(a_this);
				RE::NiUpdateData update{};
				root->UpdateDownwardPass(update, 0);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The first-person camera's position: Chief's eye (crouching and
		// jumping included).
		struct FirstPersonTranslationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiPoint3& a_out)
			{
				func(a_this, a_out);
				if (s.driving) {
					a_out = s.eye;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		// PlayerCamera::Update is called directly, not through the vtable:
		// redirect each `call PlayerCamera::Update` in the game's code.
		const auto target = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(49852, 50784) }.address();
		const auto text = REL::Module::get().segment(REL::Segment::textx);
		const auto base = text.address();
		const auto* code = reinterpret_cast<const std::uint8_t*>(base);
		std::vector<std::uintptr_t> sites;
		for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
			if (code[i] != 0xE8) {
				continue;
			}
			std::int32_t rel;
			std::memcpy(&rel, code + i + 1, 4);
			if (base + i + 5 + static_cast<std::intptr_t>(rel) == target) {
				sites.push_back(base + i);
			}
		}
		auto& trampoline = SKSE::GetTrampoline();
		for (const auto site : sites) {
			PlayerCameraUpdateHook::func = trampoline.write_call<5>(site, PlayerCameraUpdateHook::thunk);
		}

		s.useHaloFov = Settings::ReadBool(L"Camera", L"bUseHaloFov", true);

		REL::Relocation<std::uintptr_t> firstPerson{ RE::VTABLE_FirstPersonState[0] };
		FirstPersonTranslationHook::func = firstPerson.write_vfunc(0x5, FirstPersonTranslationHook::thunk);
		logger::info("camera hooks installed (PlayerCamera::Update at {} call site(s), first-person translation); FOV: {}",
			sites.size(), s.useHaloFov ? "Halo's" : "Skyrim's own");
	}

	void Drive(RE::PlayerCharacter* a_player, const cr_player_state& a_state)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}
		s.eye = { a_state.eye.x, a_state.eye.y, a_state.eye.z };
		s.rotation = RotationFrom(a_state.forward, a_state.up);
		if (!s.driving) {
			logger::info("Chief's eyes drive the camera");
		}
		s.driving = true;

		if (!camera->IsInFirstPerson()) {
			camera->ForceFirstPerson();
		}
		auto& data = camera->GetRuntimeData2();
		if (!s.fovSaved) {
			s.savedWorldFov = data.worldFOV;
			s.fovSaved = true;
			logger::info("camera FOV {:.1f} -> Halo's {:.1f} (vertical {:.1f})", s.savedWorldFov,
				SkyrimFovDeg(a_state.vertical_fov), a_state.vertical_fov * kRadToDeg);
		}
		if (s.useHaloFov && a_state.vertical_fov > 0.0f) {
			data.worldFOV = SkyrimFovDeg(a_state.vertical_fov);  // follows Halo's zoom too
		}
		// New meshes (an equipped weapon, a spell) join the arms later: look
		// again about once a second.
		static std::uint32_t frames = 0;
		if (s.hiddenArms.empty() || ++frames % 60 == 0) {
			HideArms(a_player, true);
		}
	}

	void Release(RE::PlayerCharacter* a_player)
	{
		if (!s.driving) {
			return;
		}
		s.driving = false;
		if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && s.fovSaved) {
			camera->GetRuntimeData2().worldFOV = s.savedWorldFov;
		}
		s.fovSaved = false;
		HideArms(a_player, false);
		logger::info("Skyrim's own camera is back");
	}
}
