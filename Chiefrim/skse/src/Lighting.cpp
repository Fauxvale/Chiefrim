/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Lighting.h"
#include "Link.h"
#include "Overlay.h"
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
			float flashBrightness = 1.5f;  // [Flashlight] fBrightness
			float flashReach = 1.0f;       // [Flashlight] fReach: of Halo's
			bool  shadows = true;          // [Lighting] bShadows: the sun's, on Halo's objects
			float shadowReach = 16384.0f;  // how far towards the sun a shadow's caster is looked for
			float shadowSpread = 12.0f;    // the rays around the eye, apart
			float skyReach = 3000.0f;      // how far up a roof over the eye is looked for
			float reflectionMatch = 2.5f;  // [Lighting] fReflectionMatch: a reflection's most, of the picture's key (0: no cap)
			float reflectionMin = 0.15f;   // [Lighting] fReflectionMin: the least that cap goes
		} config;

		// Chief's flashlight: a point light of Skyrim's (its renderer has no
		// unshadowed spot), moved each frame to just short of where the beam
		// lands along the view, as wide as the beam's cone is there.
		struct Flashlight
		{
			cr_msg_flashlight              beam{};  // Halo's, as it shines now
			RE::NiPointer<RE::NiPointLight> light;
			bool                           added = false;  // in the scene
			ULONGLONG                      addedAt = 0;
			int                            readded = 0;
		} flash;

		ULONGLONG next = 0;
		float     reportedLevel = -1.0f;  // the brightness last logged
		bool      reportedShade = false;  // and whether Chief was in shade
		bool      reportedRoof = false;   // and under a roof
		std::uint32_t reportedPoints = 0; // and how many point lights reached him
		ULONGLONG nextReport = 0;

		cr_vec3 Vec(const RE::NiPoint3& a_point) { return { a_point.x, a_point.y, a_point.z }; }

		// How far along from -> to a ray goes before it hits something of
		// Skyrim's (line of sight: the player's own capsule aside), 0..1
		float RayFraction(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return 1.0f;
			}
			const float scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
			RE::CFilter filter{};
			a_player->GetCollisionFilterInfo(filter);  // its group: the player's own capsule isn't hit
			filter.SetCollisionLayer(RE::COL_LAYER::kLOS);
			pick.rayInput.filterInfo = filter;
			{
				RE::BSReadLockGuard lock(world->worldLock);
				world->PickObject(pick);
			}
			return pick.rayOutput.HasHit() ? std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f) : 1.0f;
		}

		// How much of the sun (or moon) reaches the eye: rays towards it from
		// the eye and four points around it, across the light's way, so a
		// shadow's edge comes in by steps (Halo blends the rest)
		float SunVisible(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_travel)
		{
			const RE::NiPoint3 towards = a_travel * -1.0f;
			RE::NiPoint3       side = towards.Cross({ 0.0f, 0.0f, 1.0f });
			if (side.Length() < 1e-3f) {
				side = { 1.0f, 0.0f, 0.0f };
			}
			side = side / side.Length();
			RE::NiPoint3 up = side.Cross(towards);
			up = up / std::max(up.Length(), 1e-3f);
			const std::array offsets{ RE::NiPoint3{}, side * config.shadowSpread, side * -config.shadowSpread, up * config.shadowSpread,
				up * -config.shadowSpread };
			int lit = 0;
			for (const auto& offset : offsets) {
				const auto from = a_eye + offset;
				lit += RayFraction(a_player, from, from + towards * config.shadowReach) >= 1.0f;
			}
			return static_cast<float>(lit) / static_cast<float>(offsets.size());
		}

		// How much of the sky is open over the eye: rays straight up and four
		// 45 degrees from it, around (a roof, an overhang, a cliff's lip)
		float SkyVisible(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_eye)
		{
			constexpr float slant = 0.7071f;
			const std::array ways{ RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, RE::NiPoint3{ slant, 0.0f, slant }, RE::NiPoint3{ -slant, 0.0f, slant },
				RE::NiPoint3{ 0.0f, slant, slant }, RE::NiPoint3{ 0.0f, -slant, slant } };
			int open = 0;
			for (const auto& way : ways) {
				open += RayFraction(a_player, a_eye, a_eye + way * config.skyReach) >= 1.0f;
			}
			return static_cast<float>(open) / static_cast<float>(ways.size());
		}
		cr_vec3 Color(const RE::NiColor& a_color, float a_scale) { return { a_color.red * a_scale, a_color.green * a_scale, a_color.blue * a_scale }; }

		struct Point
		{
			float          share;  // how much it lights the player: its strength by its falloff there
			cr_light_point light;
		};
		std::uint32_t activeCount = 0;  // the scene's point lights, the last look

		// The point lights the renderer has on now that reach the player, the
		// ones that light him most first. A light's place is its node's: the
		// BSLight's own translate is relative to the camera (Skyrim renders
		// about it), which put every light outside far from the player.
		void NearestPoints(RE::ShadowSceneNode::RUNTIME_DATA& a_scene, const RE::NiPoint3& a_at, cr_msg_lighting& a_message)
		{
			std::vector<Point> points;
			activeCount = 0;
			const auto take = [&](RE::BSLight* a_light) {
				// (Chief's flashlight is Halo's own: Halo lights its objects with it already)
				if (!a_light || !a_light->pointLight || a_light == a_scene.sunLight || !a_light->light || a_light->light.get() == flash.light.get()) {
					return;
				}
				++activeCount;
				const auto& data = a_light->light->GetLightRuntimeData();
				const auto  position = a_light->light->world.translate;
				const float radius = data.radius.x;
				const float distance = position.GetDistance(a_at);
				const float strength = (data.diffuse.red + data.diffuse.green + data.diffuse.blue) * data.fade;
				// past its reach a little: it still lights Chief's weapon, held out
				if (radius <= 1.0f || distance >= radius + 64.0f || strength <= 0.01f) {
					return;
				}
				const float along = std::min(distance / radius, 1.0f);
				const float share = strength * std::max(1.0f - along * along, 0.02f);
				points.push_back({ share, { Vec(position), radius, Color(data.diffuse, data.fade * config.pointScale * config.scale) } });
			};
			for (auto& light : a_scene.activeLights) {
				take(light.get());
			}
			for (auto& light : a_scene.activeShadowLights) {
				take(light.get());
			}
			std::ranges::sort(points, std::greater{}, &Point::share);
			a_message.point_count = static_cast<std::uint32_t>(std::min<std::size_t>(points.size(), CR_LIGHTING_POINTS));
			for (std::uint32_t i = 0; i < a_message.point_count; ++i) {
				a_message.points[i] = points[i].light;
			}
		}
	}

	namespace
	{
		// the lights sent, for the log: how far, how far they reach, colour
		std::string Describe(const cr_msg_lighting& a_message, const RE::NiPoint3& a_at)
		{
			std::string text;
			for (std::uint32_t i = 0; i < a_message.point_count; ++i) {
				const auto& p = a_message.points[i];
				text += std::format("{} {:.0f} away (reach {:.0f}, {:.2f} {:.2f} {:.2f})", i ? ";" : ":", RE::NiPoint3{ p.position.x, p.position.y, p.position.z }.GetDistance(a_at),
					p.radius, p.color.x, p.color.y, p.color.z);
			}
			return text;
		}
	}

	void Install()
	{
		config.enabled = Settings::ReadFloat(L"Lighting", L"bEnabled", 1.0f) != 0.0f;
		config.scale = Settings::ReadFloat(L"Lighting", L"fBrightness", 1.0f);
		config.pointScale = Settings::ReadFloat(L"Lighting", L"fPointLights", 1.0f);
		config.shadows = Settings::ReadBool(L"Lighting", L"bShadows", true);
		config.flashlight = Settings::ReadBool(L"Flashlight", L"bEnabled", true);
		config.flashBrightness = Settings::ReadFloat(L"Flashlight", L"fBrightness", 1.5f);
		config.flashReach = Settings::ReadFloat(L"Flashlight", L"fReach", 1.0f);
		config.reflectionMatch = std::max(Settings::ReadFloat(L"Lighting", L"fReflectionMatch", 2.5f), 0.0f);
		config.reflectionMin = std::clamp(Settings::ReadFloat(L"Lighting", L"fReflectionMin", 0.15f), 0.0f, 1.0f);
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
		message.sun_visible = 1.0f;
		message.sky_visible = 1.0f;
		if (auto* cell = a_player->GetParentCell(); config.shadows && cell && !cell->IsInteriorCell()) {
			if (auto* camera = RE::Main::WorldRootCamera()) {
				message.sky_visible = SkyVisible(a_player, camera->world.translate);
			}
		}
		if (auto* sun = runtime.sunLight; sun && sun->light) {
			const auto& data = sun->light->GetLightRuntimeData();
			message.key_color = Color(data.diffuse, data.fade * config.scale);
			if (auto* directional = netimmerse_cast<RE::NiDirectionalLight*>(sun->light.get())) {
				auto direction = directional->GetDirectionalLightRuntimeData().worldDir;
				if (direction.z > 0.0f) {
					direction = -direction;  // it lights from above: the way it travels is down
				}
				if (direction.Length() > 1e-3f) {
					direction = direction / direction.Length();
					message.key_direction = Vec(direction);
					// outside, the sun and moon cast shadows; an interior's
					// directional light lights everything
					auto* cell = a_player->GetParentCell();
					auto* camera = RE::Main::WorldRootCamera();
					if (config.shadows && cell && !cell->IsInteriorCell() && camera) {
						message.key_shadowed = 1;
						message.sun_visible = SunVisible(a_player, camera->world.translate, direction);
					}
				}
			}
		}
		NearestPoints(runtime, a_player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 100.0f }, message);

		// What a shiny weapon reflects is its surroundings, as Skyrim's picture
		// shows them: in a dim cabin the MA5B shone near white by the fire's
		// light (the reflection's strength is the light's, at its most from
		// about half of Skyrim's daylight), where the room's brightest were 0.3
		float key = 0.0f;
		if (config.reflectionMatch > 0.0f && Overlay::Surroundings(key)) {
			message.reflection_cap = std::clamp(key * config.reflectionMatch, std::max(config.reflectionMin, 0.01f), 1.0f);
		}

		if (!Link::Get().PushRaw(CR_MSG_LIGHTING, &message, sizeof(message))) {
			return;
		}
		// logged at first and when it changes a lot (a door, nightfall, a torch), every 5 s at most
		const bool  shade = message.key_shadowed && message.sun_visible < 0.5f;
		const bool  roofed = message.key_shadowed && message.sky_visible < 0.5f;
		const float level = (message.ambient.x + message.ambient.y + message.ambient.z + message.key_color.x + message.key_color.y + message.key_color.z) / 3.0f;
		if (now >= nextReport && (reportedLevel < 0.0f || std::fabs(level - reportedLevel) > 0.3f * std::max(reportedLevel, 0.1f) || shade != reportedShade || roofed != reportedRoof ||
							   message.point_count != reportedPoints)) {
			reportedLevel = level;
			reportedPoints = message.point_count;
			reportedShade = shade;
			reportedRoof = roofed;
			nextReport = now + 5000;
			logger::info("lighting: Halo's objects lit by Skyrim's: ambient ({:.2f} {:.2f} {:.2f}), key ({:.2f} {:.2f} {:.2f}) towards ({:.2f} {:.2f} {:.2f}), {} point lights near (of {} on){}, {}",
				message.ambient.x, message.ambient.y, message.ambient.z, message.key_color.x, message.key_color.y, message.key_color.z,
				message.key_direction.x, message.key_direction.y, message.key_direction.z, message.point_count, activeCount, Describe(message, a_player->GetPosition()),
				!message.key_shadowed ? "no shadows (inside)" : std::format("the sun {:.0f}% seen, the sky {:.0f}% open", message.sun_visible * 100.0f, message.sky_visible * 100.0f));
			logger::info("lighting: reflections at most {:.2f} (0: no cap), by how bright Skyrim's picture around Chief is",
				message.reflection_cap);
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
			return false;
		}

		// How far the view's ray goes before it hits something of Skyrim's
		// (the player's own capsule aside), up to a_reach
		float BeamLength(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_forward, float a_reach)
		{
			return RayFraction(a_player, a_eye, a_eye + a_forward * a_reach) * a_reach;
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

	namespace
	{
		void TakeDown()
		{
			if (flash.added && flash.light) {
				if (auto* scene = Scene()) {
					scene->RemoveLight(flash.light.get());
				}
			}
			flash.added = false;
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
		if (!flash.light) {
			flash.light.reset(RE::NiPointLight::Create());
			if (!flash.light) {
				return;
			}
			auto& data = flash.light->GetLightRuntimeData();
			data.ambient = { 0.0f, 0.0f, 0.0f };
		}
		const auto now = ::GetTickCount64();
		if (flash.added && now - flash.addedAt > 1000 && !InScene(scene)) {
			flash.added = false;  // the scene let it go (a door, a load): again
			if (flash.readded++ < 5) {
				logger::info("flashlight: the scene dropped its light; added again");
			}
		}
		if (!flash.added) {
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
			params.dynamic = true;
			params.shadowLight = false;
			params.portalStrict = false;
			params.affectLand = true;
			params.affectWater = true;
			params.neverFades = true;
			params.fov = RE::NI_PI;
			params.falloff = 1.0f;
			params.nearDistance = 5.0f;
			scene->AddLight(flash.light.get(), params);
			flash.added = true;
			flash.addedAt = now;
		}

		// the view, as Skyrim last drew it (NiCamera: its first column is forward)
		const auto& view = camera->world;
		const RE::NiPoint3 eye = view.translate;
		RE::NiPoint3 forward{ view.rotate.entry[0][0], view.rotate.entry[1][0], view.rotate.entry[2][0] };
		if (forward.Length() < 1e-3f) {
			return;
		}
		forward = forward / forward.Length();

		const float reach = std::max(flash.beam.radius * config.flashReach, 64.0f);
		const float length = BeamLength(a_player, eye, forward, reach);
		// the beam's width where it lands: halfway between its full and its edge
		const float angle = std::clamp(0.5f * (flash.beam.cutoff_angle + flash.beam.falloff_angle), 0.05f, 1.3f);
		const float spot = std::max(length * std::tan(angle), 24.0f);
		// back from that surface, in the air the ray crossed, far enough to
		// light the spot's width; a sphere reaching a little past its edge
		const float back = std::clamp(spot, 16.0f, std::max(length * 0.85f, 16.0f));
		const float radius = std::sqrt(back * back + 1.56f * spot * spot);
		const RE::NiPoint3 at = eye + forward * std::max(length - back, 8.0f);
		// dimmer the further it carries
		const float carry = std::sqrt(std::clamp(1.0f - length / reach, 0.05f, 1.0f));

		auto& data = flash.light->GetLightRuntimeData();
		data.diffuse = { flash.beam.color.x, flash.beam.color.y, flash.beam.color.z };
		data.radius = { radius, radius, radius };
		data.fade = config.flashBrightness * carry;
		flash.light->local.translate = at;
		flash.light->world.translate = at;
	}
}
