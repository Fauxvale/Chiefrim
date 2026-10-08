/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Lighting.h"
#include "Link.h"
#include "Settings.h"

namespace chiefrim::Lighting
{
	namespace
	{
		struct Config
		{
			float scale = 1.0f;       // [Lighting] fBrightness: all of it
			float pointScale = 1.0f;  // [Lighting] fPointLights: torches, fires, spells
			bool  enabled = true;     // [Lighting] bEnabled
			bool  flashlight = true;  // [Flashlight] bEnabled
			bool  flashShadowed = true;    // [Flashlight] bShadowed: a shadowed spot light, else a point light where the beam lands
			float flashBrightness = 1.5f;  // [Flashlight] fBrightness
			float flashReach = 1.0f;       // [Flashlight] fReach: of Halo's
		} config;

		// Chief's flashlight. Skyrim's renderer has spot lights only with
		// shadows: by default one, made as Skyrim makes its own (a light form
		// flagged "spot shadow", TESObjectLIGH::GenDynamic) on a node of ours
		// at the eye, turned with the view. Else (bShadowed=0) a point light
		// moved each frame to just short of where the beam lands along the
		// view, as wide as the beam's cone is there.
		struct Flashlight
		{
			cr_msg_flashlight         beam{};  // Halo's, as it shines now
			RE::NiPointer<RE::NiLight> light;  // in the scene while on
			RE::NiPointer<RE::NiNode>  node;   // the spot's: at the eye, turned with the view
			RE::TESObjectLIGH*        form = nullptr;  // the spot's form (made at run time, never saved)
			bool                      added = false;   // in the scene
			ULONGLONG                 addedAt = 0;
			int                       readded = 0;
			bool                      logged = false;
			bool                      checked = false;  // the shadow light's own numbers logged
			float                     fovPerDegree = 0.0f;  // a light form's fov, per degree, as Skyrim holds it
		} flash;

		ULONGLONG next = 0;
		float     reportedLevel = -1.0f;  // the brightness last logged
		ULONGLONG nextReport = 0;

		cr_vec3 Vec(const RE::NiPoint3& a_point) { return { a_point.x, a_point.y, a_point.z }; }
		cr_vec3 Color(const RE::NiColor& a_color, float a_scale) { return { a_color.red * a_scale, a_color.green * a_scale, a_color.blue * a_scale }; }

		struct Point
		{
			float          distance;
			cr_light_point light;
		};

		// The point lights the renderer has on now, nearest the player first.
		void NearestPoints(RE::ShadowSceneNode::RUNTIME_DATA& a_scene, const RE::NiPoint3& a_at, cr_msg_lighting& a_message)
		{
			std::vector<Point> points;
			const auto take = [&](RE::BSLight* a_light) {
				// (Chief's flashlight is Halo's own: Halo lights its objects with it already)
				if (!a_light || !a_light->pointLight || a_light == a_scene.sunLight || !a_light->light || a_light->light.get() == flash.light.get()) {
					return;
				}
				const auto& data = a_light->light->GetLightRuntimeData();
				const float radius = data.radius.x;
				const float distance = a_light->worldTranslate.GetDistance(a_at);
				const float strength = (data.diffuse.red + data.diffuse.green + data.diffuse.blue) * data.fade;
				if (radius <= 1.0f || distance >= radius + 256.0f || strength <= 0.01f) {
					return;
				}
				points.push_back({ distance, { Vec(a_light->worldTranslate), radius, Color(data.diffuse, data.fade * config.pointScale * config.scale) } });
			};
			for (auto& light : a_scene.activeLights) {
				take(light.get());
			}
			for (auto& light : a_scene.activeShadowLights) {
				take(light.get());
			}
			std::ranges::sort(points, {}, &Point::distance);
			a_message.point_count = static_cast<std::uint32_t>(std::min<std::size_t>(points.size(), CR_LIGHTING_POINTS));
			for (std::uint32_t i = 0; i < a_message.point_count; ++i) {
				a_message.points[i] = points[i].light;
			}
		}
	}

	void Install()
	{
		config.enabled = Settings::ReadFloat(L"Lighting", L"bEnabled", 1.0f) != 0.0f;
		config.scale = Settings::ReadFloat(L"Lighting", L"fBrightness", 1.0f);
		config.pointScale = Settings::ReadFloat(L"Lighting", L"fPointLights", 1.0f);
		config.flashlight = Settings::ReadBool(L"Flashlight", L"bEnabled", true);
		config.flashShadowed = Settings::ReadBool(L"Flashlight", L"bShadowed", true);
		config.flashBrightness = Settings::ReadFloat(L"Flashlight", L"fBrightness", 1.5f);
		config.flashReach = Settings::ReadFloat(L"Flashlight", L"fReach", 1.0f);
	}

	void Reset()
	{
		next = 0;
		reportedLevel = -1.0f;
		nextReport = 0;
	}

	void Update(RE::PlayerCharacter* a_player)
	{
		const auto now = ::GetTickCount64();
		if (!config.enabled || now < next) {
			return;
		}
		next = now + 100;
		auto& state = RE::BSShaderManager::State::GetSingleton();
		auto* scene = state.shadowSceneNode[0];
		if (!scene) {
			return;
		}
		auto& runtime = scene->GetRuntimeData();

		cr_msg_lighting message{};
		// Skyrim's directional ambient: colour = rotate * normal + translate, so
		// the average over all directions is the translate, and up adds column z.
		const auto& ambient = state.directionalAmbientTransform;
		message.ambient = { ambient.translate.x * config.scale, ambient.translate.y * config.scale, ambient.translate.z * config.scale };
		message.ambient_up = { ambient.rotate.entry[0][2] * config.scale, ambient.rotate.entry[1][2] * config.scale,
			ambient.rotate.entry[2][2] * config.scale };

		// The key light: the sun or moon outside, the cell's directional light inside.
		message.key_direction = { 0.0f, 0.0f, -1.0f };
		if (auto* sun = runtime.sunLight; sun && sun->light) {
			const auto& data = sun->light->GetLightRuntimeData();
			message.key_color = Color(data.diffuse, data.fade * config.scale);
			if (auto* directional = netimmerse_cast<RE::NiDirectionalLight*>(sun->light.get())) {
				auto direction = directional->GetDirectionalLightRuntimeData().worldDir;
				if (direction.z > 0.0f) {
					direction = -direction;  // it lights from above: the way it travels is down
				}
				if (direction.Length() > 1e-3f) {
					message.key_direction = Vec(direction / direction.Length());
				}
			}
		}
		NearestPoints(runtime, a_player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 100.0f }, message);

		if (!Link::Get().PushRaw(CR_MSG_LIGHTING, &message, sizeof(message))) {
			return;
		}
		// logged at first and when it changes a lot (a door, nightfall, a torch), every 5 s at most
		const float level = (message.ambient.x + message.ambient.y + message.ambient.z + message.key_color.x + message.key_color.y + message.key_color.z) / 3.0f;
		if (now >= nextReport && (reportedLevel < 0.0f || std::fabs(level - reportedLevel) > 0.3f * std::max(reportedLevel, 0.1f))) {
			reportedLevel = level;
			nextReport = now + 5000;
			logger::info("lighting: Halo's objects lit by Skyrim's: ambient ({:.2f} {:.2f} {:.2f}), key ({:.2f} {:.2f} {:.2f}) towards ({:.2f} {:.2f} {:.2f}), {} point lights near",
				message.ambient.x, message.ambient.y, message.ambient.z, message.key_color.x, message.key_color.y, message.key_color.z,
				message.key_direction.x, message.key_direction.y, message.key_direction.z, message.point_count);
		}
	}
}

namespace chiefrim::Lighting
{
	namespace
	{
		RE::ShadowSceneNode* Scene()
		{
			return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		}

		bool FlashlightOn() { return flash.beam.color.x + flash.beam.color.y + flash.beam.color.z > 0.001f; }

		// The scene still has it (a cell change can clear the scene's lights)
		bool InScene(RE::ShadowSceneNode* a_scene)
		{
			auto& runtime = a_scene->GetRuntimeData();
			for (auto& light : runtime.activeLights) {
				if (light && light->light.get() == flash.light.get()) {
					return true;
				}
			}
			for (auto& light : runtime.activeShadowLights) {
				if (light && light->light.get() == flash.light.get()) {
					return true;
				}
			}
			return false;
		}

		// How far the view's ray goes before it hits something of Skyrim's
		// (the player's own capsule aside), up to a_reach
		float BeamLength(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_forward, float a_reach)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return a_reach;
			}
			const float scale = RE::bhkWorld::GetWorldScale();
			const auto  to = a_eye + a_forward * a_reach;
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(a_eye.x * scale, a_eye.y * scale, a_eye.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
			RE::CFilter filter{};
			a_player->GetCollisionFilterInfo(filter);  // its group: the player's own capsule isn't hit
			filter.SetCollisionLayer(RE::COL_LAYER::kLOS);
			pick.rayInput.filterInfo = filter;
			{
				RE::BSReadLockGuard lock(world->worldLock);
				world->PickObject(pick);
			}
			return pick.rayOutput.HasHit() ? std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f) * a_reach : a_reach;
		}

		void TakeDown()
		{
			if (flash.added && flash.light) {
				if (auto* scene = Scene()) {
					scene->RemoveLight(flash.light.get());
				}
			}
			if (flash.light && flash.node && flash.light->parent == flash.node.get()) {
				flash.node->DetachChild(flash.light.get());
			}
			flash.light.reset();
			flash.added = false;
		}

		float Reach() { return std::max(flash.beam.radius * config.flashReach, 64.0f); }

		// The spot's form: a dynamic light with a shadowed cone, Halo's
		// reach and cone (the form's field of view is the whole cone, in degrees)
		RE::TESObjectLIGH* SpotForm()
		{
			if (!flash.form) {
				auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESObjectLIGH>();
				flash.form = factory ? factory->Create() : nullptr;
				if (!flash.form) {
					logger::warn("flashlight: couldn't make a light form; a point light instead");
					config.flashShadowed = false;
					return nullptr;
				}
			}
			// Skyrim's light forms hold their field of view as loaded from the
			// file (degrees there): a vanilla spot (Solitude's inn, 90 in
			// Skyrim.esm) says in what units, at run time
			if (flash.fovPerDegree <= 0.0f) {
				const auto* vanilla = RE::TESForm::LookupByID<RE::TESObjectLIGH>(0x0006C056);
				const float fov = vanilla ? vanilla->data.fov : 0.0f;
				flash.fovPerDegree = fov > 0.0f && fov < 6.3f ? RE::NI_PI / 180.0f : 1.0f;
				logger::info("flashlight: a vanilla spot light's field of view is {:.4f} at run time (90 in the file): {}", fov,
					flash.fovPerDegree < 1.0f ? "radians" : "degrees");
			}
			auto& data = flash.form->data;
			data.time = -1;
			data.radius = static_cast<std::uint32_t>(Reach());
			data.color = RE::Color(255, 255, 255, 0);
			data.flags.reset(RE::TES_LIGHT_FLAGS::kType);
			data.flags.set(RE::TES_LIGHT_FLAGS::kDynamic, RE::TES_LIGHT_FLAGS::kSpotShadow);
			data.fallofExponent = 1.0f;
			data.fov = std::clamp(2.0f * flash.beam.cutoff_angle * 180.0f / RE::NI_PI, 10.0f, 170.0f) * flash.fovPerDegree;
			data.nearDistance = 8.0f;
			flash.form->fade = config.flashBrightness;
			return flash.form;
		}

		// the spot: made by Skyrim's own code, on our node
		bool AddSpot(RE::PlayerCharacter* a_player)
		{
			auto* form = SpotForm();
			if (!form) {
				return false;
			}
			if (!flash.node) {
				flash.node.reset(RE::NiNode::Create(1));
				if (!flash.node) {
					return false;
				}
				flash.node->name = "Chiefrim flashlight";
			}
			auto* light = form->GenDynamic(a_player, flash.node.get(), 1, 1, 0);
			if (!light) {
				logger::warn("flashlight: Skyrim made no spot light; a point light instead");
				config.flashShadowed = false;
				return false;
			}
			flash.light.reset(light);
			if (light->parent != flash.node.get()) {
				flash.node->AttachChild(light, true);
			}
			if (!flash.logged) {
				flash.logged = true;
				logger::info("flashlight: Skyrim made its light ({}), reach {}, field of view {:.3f}",
					light->GetRTTI() ? light->GetRTTI()->GetName() : "?", form->data.radius, form->data.fov);
			}
			return true;
		}

		bool AddPoint()
		{
			auto* scene = Scene();
			auto* light = RE::NiPointLight::Create();
			if (!scene || !light) {
				return false;
			}
			flash.light.reset(light);
			light->GetLightRuntimeData().ambient = { 0.0f, 0.0f, 0.0f };
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
			params.dynamic = true;
			params.affectLand = true;
			params.affectWater = true;
			params.neverFades = true;
			params.fov = RE::NI_PI;
			params.falloff = 1.0f;
			params.nearDistance = 5.0f;
			scene->AddLight(light, params);
			return true;
		}

		// shadowed: the spot at the eye (a little ahead, clear of the
		// player's own head), turned with the view
		void PlaceSpot(const RE::NiTransform& a_view, const RE::NiPoint3& a_forward)
		{
			flash.node->local.rotate = a_view.rotate;
			flash.node->local.translate = a_view.translate + a_forward * 16.0f;
			RE::NiUpdateData update{};
			flash.node->Update(update);
			auto& data = flash.light->GetLightRuntimeData();
			data.diffuse = { flash.beam.color.x, flash.beam.color.y, flash.beam.color.z };
			data.fade = config.flashBrightness;
		}

		// a point light just short of where the beam lands, as wide as the
		// beam's cone is there, dimmer the further it carries
		void PlacePoint(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_forward)
		{
			const float reach = Reach();
			const float length = BeamLength(a_player, a_eye, a_forward, reach);
			// the beam's width where it lands: halfway between its full and its edge
			const float angle = std::clamp(0.5f * (flash.beam.cutoff_angle + flash.beam.falloff_angle), 0.05f, 1.3f);
			const float spot = std::max(length * std::tan(angle), 24.0f);
			// back from that surface, in the air the ray crossed, far enough to
			// light the spot's width; a sphere reaching a little past its edge
			const float back = std::clamp(spot, 16.0f, std::max(length * 0.85f, 16.0f));
			const float radius = std::sqrt(back * back + 1.56f * spot * spot);
			const RE::NiPoint3 at = a_eye + a_forward * std::max(length - back, 8.0f);
			const float carry = std::sqrt(std::clamp(1.0f - length / reach, 0.05f, 1.0f));

			auto& data = flash.light->GetLightRuntimeData();
			data.diffuse = { flash.beam.color.x, flash.beam.color.y, flash.beam.color.z };
			data.radius = { radius, radius, radius };
			data.fade = config.flashBrightness * carry;
			flash.light->local.translate = at;
			flash.light->world.translate = at;
		}
	}

	void OnFlashlight(const cr_msg_flashlight& a_message)
	{
		const bool was = FlashlightOn();
		flash.beam = a_message;
		if (FlashlightOn() != was) {
			logger::info("flashlight: {} (colour {:.2f} {:.2f} {:.2f}, reach {:.0f}, cone {:.0f} degrees)", FlashlightOn() ? "on" : "off",
				a_message.color.x, a_message.color.y, a_message.color.z, a_message.radius, a_message.cutoff_angle * 180.0f / RE::NI_PI);
		}
	}

	void RemoveFlashlight()
	{
		TakeDown();
		flash.beam = {};  // a new link: Halo says again how it shines
	}

	void UpdateFlashlight(RE::PlayerCharacter* a_player)
	{
		auto* scene = Scene();
		auto* camera = RE::Main::WorldRootCamera();
		if (!config.flashlight || !FlashlightOn() || !scene || !camera) {
			TakeDown();
			return;
		}
		const auto now = ::GetTickCount64();
		if (flash.added && now - flash.addedAt > 1000 && !InScene(scene)) {
			TakeDown();  // the scene let it go (a door, a load): again
			if (flash.readded++ < 5) {
				logger::info("flashlight: the scene dropped its light; added again");
			}
		}

		// the view, as Skyrim last drew it (NiCamera: its first column is forward)
		const auto& view = camera->world;
		RE::NiPoint3 forward{ view.rotate.entry[0][0], view.rotate.entry[1][0], view.rotate.entry[2][0] };
		if (forward.Length() < 1e-3f) {
			return;
		}
		forward = forward / forward.Length();

		const bool shadowed = config.flashShadowed;
		if (!flash.added) {
			if (shadowed) {
				// placed before it's made, so it starts where it shines
				if (flash.node) {
					flash.node->local.rotate = view.rotate;
					flash.node->local.translate = view.translate + forward * 16.0f;
				}
				if (!AddSpot(a_player) && !AddPoint()) {
					return;
				}
			} else if (!AddPoint()) {
				return;
			}
			flash.added = true;
			flash.addedAt = now;
			flash.checked = false;  // a new light of Skyrim's: its cone looked at again
		}
		if (!flash.checked && now - flash.addedAt > 1000) {
			// in the scene now: what kind of light Skyrim made of it, and its cone
			flash.checked = true;
			RE::BSShadowLight* shadow = nullptr;
			for (auto& light : scene->GetRuntimeData().activeShadowLights) {
				if (light && light->light.get() == flash.light.get()) {
					shadow = light.get();
				}
			}
			if (!shadow) {
				logger::info("flashlight: its light has no shadow (a plain point light)");
			} else if (!shadow->GetIsFrustumLight()) {
				logger::info("flashlight: a shadowed light, not a spot");
			} else {
				auto& frustum = static_cast<RE::BSShadowFrustumLight*>(shadow)->GetShadowFrustumLightRuntimeData();
				logger::info("flashlight: a shadowed spot: semi-width {:.3f}, semi-height {:.3f}, falloff {:.2f}, near {:.1f}, far {:.1f}",
					frustum.semiWidth, frustum.semiHeight, frustum.falloff, frustum.nearDistance, frustum.farDistance);
				// Halo's cone is 45 degrees each side of its axis: its tangent (1) and
				// its angle (0.79) are close, whichever the semi-width is. Much
				// narrower, and the field of view went in wrong: widened to it
				const float wanted = std::tan(std::clamp(flash.beam.cutoff_angle, 0.1f, 1.3f));
				if (frustum.semiWidth < 0.5f * wanted || frustum.semiHeight < 0.5f * wanted) {
					frustum.semiWidth = frustum.semiHeight = wanted;
					logger::info("flashlight: its cone was narrower than Halo's; widened to {:.3f}", wanted);
				}
			}
		}
		if (shadowed && config.flashShadowed && flash.node) {
			PlaceSpot(view, forward);
		} else {
			PlacePoint(a_player, view.translate, forward);
		}
	}
}
