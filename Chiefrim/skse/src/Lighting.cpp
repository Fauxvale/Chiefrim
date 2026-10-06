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
		} config;

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
				if (!a_light || !a_light->pointLight || a_light == a_scene.sunLight || !a_light->light) {
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
