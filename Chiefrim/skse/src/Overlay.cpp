/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Overlay.h"
#include "Handoff.h"
#include "Input.h"
#include "Link.h"
#include "Settings.h"

#include <d3d11.h>
#include <d3dcompiler.h>

namespace chiefrim::Overlay
{
	namespace
	{
		// Halo stopped publishing this long ago: hide its last frame.
		constexpr ULONGLONG kStaleMs = 500;

		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		PresentFn       originalPresent = nullptr;
		IDXGISwapChain* swapChain = nullptr;

		// Under Skyrim's menus ([Overlay] bUnderSkyrimMenus): Halo's layers
		// are drawn as the first of Skyrim's menus draws (IMenu::PostDisplay,
		// lowest first), so the HUD's prompts, subtitles and compass, and
		// any menu over them, are on top. Present draws them when no menu
		// drew in a frame.
		bool composited = false;  // this frame, under the menus

		// One of Halo's pictures, as a texture.
		struct Layer
		{
			ID3D11Texture2D*          texture = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			UINT                      width = 0, height = 0;
		};

		struct alignas(16) Params
		{
			float depth[4];       // Skyrim's near, far, 1 if its depth is reversed, Skyrim units per Halo world unit
			float now[4][4];      // Skyrim's camera now: forward, up, right (xyz), and its tangents (x, y)
			float halo[4][4];     // the camera Halo's frame was drawn through, and Halo's tangents
			// Skyrim's grade of its own picture (its image space), for Halo's
			float grade[4];       // saturation, brightness, contrast, strength (0: none)
			float tint[4];        // colour, amount
			float fade[4];        // colour, amount
			float fogNear[4];     // Skyrim's fog: the near colour, power
			float fogFar[4];      // the far colour, the most it covers
			float fogPlanes[4];   // where it starts and is full (Skyrim units), 1 if on
			float look[4];        // Chief's arms and weapon: brightness, saturation, highlights' knee, 1 if on
			float lookScene[4];   // matched to Skyrim's picture: shadow lift, exposure's least and most, 1 if metered
			float lookTint[4];    // how far towards the picture's mean hue
		};

		// A camera Skyrim published, for mapping Halo's frame onto the camera
		// of the moment (Halo's frames are a frame or two behind).
		struct PublishedCamera
		{
			std::uint32_t frame = 0;
			RE::NiPoint3  forward, up, right;
			float         tangentX = 0.0f, tangentY = 0.0f;
		};
		constexpr std::uint32_t kCameraRing = 64;

		struct
		{
			ID3D11Device*             device = nullptr;
			ID3D11DeviceContext*      context = nullptr;
			Layer                     screen, world, worldDepth, weapon;  // weapon: the screen layer's weapon share
			bool                      weaponShown = false;                // in the textures
			ID3D11VertexShader*       vs = nullptr;
			ID3D11PixelShader*        screenPs = nullptr;
			ID3D11PixelShader*        worldPs = nullptr;
			ID3D11BlendState*         blend = nullptr;
			ID3D11SamplerState*       linear = nullptr;
			ID3D11SamplerState*       point = nullptr;
			ID3D11RasterizerState*    raster = nullptr;
			ID3D11DepthStencilState*  depth = nullptr;
			ID3D11Buffer*             params = nullptr;
			bool                      initFailed = false;

			// The meter of Skyrim's picture (before Halo's layers): its copy,
			// mipped, and two texels kept from frame to frame, blended
			// towards each frame's (an eye adapting): its key and brightest,
			// and its shadows' colour
			ID3D11PixelShader*        meterPs = nullptr;
			ID3D11BlendState*         meterBlend = nullptr;
			ID3D11Texture2D*          sceneCopy = nullptr;
			ID3D11ShaderResourceView* sceneSrv = nullptr;
			ID3D11Texture2D*          meter = nullptr;
			ID3D11RenderTargetView*   meterRtv = nullptr;
			ID3D11ShaderResourceView* meterSrv = nullptr;
			ID3D11Texture2D*          meterStaging = nullptr;  // read back: the lighting's surroundings, and the log
			bool                      meterFailed = false;
			bool                      metered = false;         // this frame
			bool                      stagingPending = false;
			double                    meterAt = 0.0;           // ms, the last meter
			ULONGLONG                 nextReadback = 0, nextMeterLog = 0;
			float                     readKey = 0.0f;          // as last read back
			RE::NiColor               readMean;
			ULONGLONG                 readAt = 0;

			// Skyrim's depth, copied as its world rendering finishes
			ID3D11Texture2D*          depthCopy = nullptr;
			ID3D11ShaderResourceView* depthCopySrv = nullptr;
			bool                      depthCopied = false;  // this frame
			float                     nearPlane = 15.0f, farPlane = 353840.0f;
			int                       reversed = -1;         // unknown until a look at the depth
			std::uint32_t             depthCopies = 0;

			PublishedCamera cameras[kCameraRing];
			PublishedCamera current;           // this frame's
			std::uint32_t   haloCamera = 0;    // the camera of Halo's frame in the textures
			float           haloTangentX = 0.0f, haloTangentY = 0.0f;
			std::uint64_t   reprojected = 0, unknownCamera = 0;
			float           lastLoggedTangent = 0.0f;

			std::uint32_t displayFrame = 0;
			std::uint32_t cameraFrame = 0;    // the last camera published to Halo
			std::uint32_t cameraPending = 0;  // published this frame: wait for Halo's frame of it
			std::uint32_t lastFrame = 0;      // Halo's frame count in the textures
			bool          haveFrame = false;
			bool          worldShown = false; // the world layer in the textures has something
			std::uint32_t lastPublished = 0;
			ULONGLONG     lastPublishedAt = 0;
			std::uint64_t uploads = 0, torn = 0, waits = 0, late = 0, matched = 0;
			double        waitMs = 0.0;
			ULONGLONG     nextReport = 0;
			bool          shown = false;
		} s;

		// Full-screen triangles. Halo's pictures are premultiplied, scaled to
		// the back buffer if Halo's are smaller (screens over CR_FRAME_MAX_*).
		// The world layer's pixels behind Skyrim's own are dropped.
		constexpr char kShader[] = R"(
cbuffer Params : register(b0)
{
	float4 depthParams; float4 nowBasis[4]; float4 haloBasis[4];
	float4 grade; float4 tint; float4 fade; float4 fogNear; float4 fogFar; float4 fogPlanes;
	float4 look; float4 lookScene; float4 lookTint;
};
Texture2D picture : register(t0);
Texture2D<float> haloDepth : register(t1);
Texture2D<float> skyrimDepth : register(t2);
Texture2D<float> weaponShare : register(t3);
Texture2D scene : register(t4);
Texture2D meter : register(t5);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
// Skyrim's grade, as its HDR shader makes it after the tone map: saturation,
// the tint, brightness and contrast, then a fade. On Halo's premultiplied
// colour, by the amount a_share of it that is Skyrim's world (not the HUD).
float4 Grade(float4 c, float a_share)
{
	float w = a_share * grade.w;
	if (w <= 0 || c.a <= 0.002)
		return c;
	float3 u = c.rgb / c.a;
	float  luminance = dot(u, float3(0.2125, 0.7154, 0.0721));
	float3 g = lerp(luminance.xxx, u, grade.x);
	g = lerp(g, tint.rgb * luminance, tint.a);
	g = grade.z * (grade.y * g - 0.5) + 0.5;
	g = lerp(g, fade.rgb, fade.a);
	return float4(lerp(c.rgb, saturate(g) * c.a, w), c.a);
}
static const float3 kLuma = float3(0.2125, 0.7154, 0.0721);
// Skyrim's picture, before Halo's layers, metered from a small mip of it:
// texel 0 its key (the log average of its brightness) and its brightest (a
// soft maximum), texel 1 its shadows' colour (a soft minimum), texel 2 its
// mean colour
float4 PSMeter(VSOut i) : SV_Target
{
	uint level = 0, w, h, levels;
	scene.GetDimensions(0, w, h, levels);
	while (max(w, h) >> level > 64 && level + 1 < levels)
		level++;
	scene.GetDimensions(level, w, h, levels);
	w = min(w, 64u);
	h = min(h, 64u);
	float  logSum = 0, lowWeight = 0, high = 0, highWeight = 0;
	float3 low = 0, sum = 0;
	[loop] for (uint y = 0; y < h; y++)
	{
		[loop] for (uint x = 0; x < w; x++)
		{
			float3 c = saturate(scene.Load(int3(x, y, level)).rgb);
			float  l = dot(c, kLuma);
			logSum += log(l + 0.001);
			float dark = exp(-16 * l), bright = exp(16 * (l - 1));
			low += c * dark;
			sum += c;
			lowWeight += dark;
			high += l * bright;
			highWeight += bright;
		}
	}
	if (i.pos.x < 1)
		return float4(exp(logSum / max(w * h, 1u)), high / max(highWeight, 1e-20), 0, 1);
	if (i.pos.x < 2)
		return float4(low / max(lowWeight, 1e-20), 1);
	return float4(sum / max(w * h, 1u), 1);
}
// Chief's arms and weapon toned to Skyrim's world before its grade. Halo's
// are lit by Skyrim's light, but drawn after Skyrim's tone map and without
// the haze that lifts its shadows: in bright snow they were near black and
// dull, in the dark too bright. So, by the meter of Skyrim's picture: the
// saturation; an exposure that follows the picture's key (as an eye adapts);
// a shift towards the picture's mean hue (as an eye takes a room's light for
// white: a cabin's warm, a snowy day's cool); the highlights rolled off
// towards the picture's brightest (no shine above the sky's); and the
// shadows lifted towards the picture's own shadows' colour (the cool haze of
// a snowy day, nothing at night). By the amount a_share of the pixel that is
// the weapon.
float4 Look(float4 c, float a_share)
{
	if (look.w <= 0 || a_share <= 0 || c.a <= 0.002)
		return c;
	float  key = 0.3, ceiling = 1;
	float3 shadow = 0, hue = 1;
	if (lookScene.w > 0)
	{
		float3 mean = meter.Load(int3(2, 0, 0)).rgb;
		hue = lerp(1, clamp(mean / max(dot(mean, kLuma), 0.02), 0.5, 1.5), lookTint.x);
		float4 m = meter.Load(int3(0, 0, 0));
		key = m.x;
		ceiling = clamp(m.y, 0.4, 1);
		shadow = saturate(meter.Load(int3(1, 0, 0)).rgb) * lookScene.x;
	}
	float  exposure = lookScene.w > 0 ? clamp(sqrt(key / 0.3), lookScene.y, lookScene.z) : 1;
	float3 u = c.rgb / c.a;
	float3 g = max(lerp(dot(u, kLuma).xxx, u, look.y), 0) * exposure * look.x * hue;
	float  l = dot(g, kLuma);
	float  k = look.z * ceiling, room = max(ceiling - k, 1e-3);
	if (l > k)
		g *= (k + room * (1 - exp(-(l - k) / room))) / l;
	g = shadow + (1 - dot(shadow, kLuma)) * g;
	return float4(lerp(c.rgb, saturate(g) * c.a, a_share), c.a);
}
float4 PSScreen(VSOut i) : SV_Target
{
	float share = weaponShare.Sample(linear_clamp, i.uv);
	return Grade(Look(picture.Sample(linear_clamp, i.uv), share), share);
}
// Skyrim's view distance (along the view, Skyrim units) from its depth buffer.
float SkyrimViewDepth(float d)
{
	float n = depthParams.x, f = depthParams.y;
	return depthParams.z > 0.5 ? n * f / (n + d * (f - n)) : n * f / (f - d * (f - n));
}
// The world layer was drawn through an older camera (haloBasis: forward, up,
// right, tangents); this pixel's view ray, from the camera of the moment
// (nowBasis), is where in Halo's picture to look. Exact for turning; walking
// leaves a parallax of a frame's step.
float4 PSWorld(VSOut i) : SV_Target
{
	float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
	float3 ray = nowBasis[0].xyz + ndc.x * nowBasis[3].x * nowBasis[2].xyz + ndc.y * nowBasis[3].y * nowBasis[1].xyz;
	float  ahead = dot(ray, haloBasis[0].xyz);
	if (ahead <= 1e-4)
		discard;
	float2 h = float2(dot(ray, haloBasis[2].xyz), dot(ray, haloBasis[1].xyz)) / ahead / haloBasis[3].xy;
	if (any(abs(h) > 1))
		discard;
	float2 uv = float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);
	float4 c = picture.Sample(linear_clamp, uv);
	if (all(c == 0))
		discard;
	// Halo's depth is along its forward; along this camera's: the same point
	float halo = haloDepth.SampleLevel(point_clamp, uv, 0) * depthParams.w / ahead;
	float skyrim = SkyrimViewDepth(skyrimDepth.SampleLevel(point_clamp, i.uv, 0));
	// a little slack: Halo's decals lie on Skyrim's own surfaces
	if (halo > skyrim * 1.003 + 4.0)
		discard;
	// Skyrim's fog on what lies in it, as on its own world
	if (fogPlanes.z > 0.5)
	{
		float f = min(pow(saturate((halo - fogPlanes.x) / max(fogPlanes.y - fogPlanes.x, 1.0)), fogNear.w), fogFar.w);
		c.rgb = lerp(c.rgb, lerp(fogNear.rgb, fogFar.rgb, f) * c.a, f);
	}
	return Grade(c, 1.0);
}
)";

		void PutBasis(float (&a_out)[4][4], const PublishedCamera& a_camera, float a_tangentX, float a_tangentY)
		{
			const RE::NiPoint3* axes[3]{ &a_camera.forward, &a_camera.up, &a_camera.right };
			for (int i = 0; i < 3; ++i) {
				a_out[i][0] = axes[i]->x;
				a_out[i][1] = axes[i]->y;
				a_out[i][2] = axes[i]->z;
				a_out[i][3] = 0.0f;
			}
			a_out[3][0] = a_tangentX;
			a_out[3][1] = a_tangentY;
			a_out[3][2] = a_out[3][3] = 0.0f;
		}

		bool Enabled()
		{
			static const bool value = Settings::ReadBool(L"Overlay", L"bEnabled", true);
			return value;
		}

		// How long Present waits for Halo's frame of this frame's camera.
		double WaitLimitMs()
		{
			static const double value = std::clamp(Settings::ReadFloat(L"Overlay", L"fWaitMs", 0.0f), 0.0f, 50.0f);
			return value;
		}

		template <class T>
		void SafeRelease(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		double NowMs()
		{
			LARGE_INTEGER now{}, frequency{};
			::QueryPerformanceCounter(&now);
			::QueryPerformanceFrequency(&frequency);
			return double(now.QuadPart) * 1000.0 / double(frequency.QuadPart);
		}

		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob* errors = nullptr;
			const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "chiefrim_overlay", nullptr, nullptr, a_entry, a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("overlay: shader {} doesn't compile: {}", a_entry,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
			}
			SafeRelease(errors);
			return SUCCEEDED(hr);
		}

		bool InitResources(ID3D11Device* a_device)
		{
			if (s.device) {
				return true;
			}
			if (s.initFailed || !a_device) {
				s.initFailed = true;
				return false;
			}
			s.device = a_device;
			s.device->AddRef();
			s.device->GetImmediateContext(&s.context);

			ID3DBlob *vsBlob = nullptr, *screenBlob = nullptr, *worldBlob = nullptr, *meterBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSScreen", "ps_5_0", &screenBlob) ||
				!Compile("PSWorld", "ps_5_0", &worldBlob) || !Compile("PSMeter", "ps_5_0", &meterBlob)) {
				SafeRelease(vsBlob);
				SafeRelease(screenBlob);
				SafeRelease(worldBlob);
				s.initFailed = true;
				return false;
			}
			s.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s.vs);
			s.device->CreatePixelShader(screenBlob->GetBufferPointer(), screenBlob->GetBufferSize(), nullptr, &s.screenPs);
			s.device->CreatePixelShader(worldBlob->GetBufferPointer(), worldBlob->GetBufferSize(), nullptr, &s.worldPs);
			s.device->CreatePixelShader(meterBlob->GetBufferPointer(), meterBlob->GetBufferSize(), nullptr, &s.meterPs);
			SafeRelease(meterBlob);
			SafeRelease(vsBlob);
			SafeRelease(screenBlob);
			SafeRelease(worldBlob);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;  // premultiplied
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;  // the back buffer's alpha stays
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			s.device->CreateBlendState(&bd, &s.blend);
			// the meter: each frame's blended in by the blend factor
			bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
			bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
			s.device->CreateBlendState(&bd, &s.meterBlend);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			s.device->CreateSamplerState(&sd, &s.linear);
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			s.device->CreateSamplerState(&sd, &s.point);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			s.device->CreateRasterizerState(&rd, &s.raster);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.StencilEnable = FALSE;
			s.device->CreateDepthStencilState(&dd, &s.depth);

			D3D11_BUFFER_DESC cbd{};
			cbd.ByteWidth = sizeof(Params);
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			s.device->CreateBuffer(&cbd, nullptr, &s.params);

			const bool ok = s.vs && s.screenPs && s.worldPs && s.blend && s.linear && s.point && s.raster && s.depth && s.params;
			logger::info("overlay: compositor {}", ok ? "ready" : "failed to initialize");
			s.initFailed = !ok;
			return ok;
		}

		bool EnsureLayer(Layer& a_layer, UINT a_width, UINT a_height, DXGI_FORMAT a_format, const char* a_what)
		{
			if (a_layer.texture && a_layer.width == a_width && a_layer.height == a_height) {
				return true;
			}
			SafeRelease(a_layer.srv);
			SafeRelease(a_layer.texture);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = a_width;
			td.Height = a_height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = a_format;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(s.device->CreateTexture2D(&td, nullptr, &a_layer.texture)) ||
				FAILED(s.device->CreateShaderResourceView(a_layer.texture, nullptr, &a_layer.srv))) {
				logger::error("overlay: a {}x{} texture for the {} can't be made", a_width, a_height, a_what);
				SafeRelease(a_layer.texture);
				return false;
			}
			a_layer.width = a_width;
			a_layer.height = a_height;
			logger::info("overlay: Halo's {} is {}x{}", a_what, a_width, a_height);
			return true;
		}

		bool Upload(Layer& a_layer, const std::uint8_t* a_source, UINT a_height, std::size_t a_rowBytes)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(s.context->Map(a_layer.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return false;
			}
			auto* destination = static_cast<std::uint8_t*>(mapped.pData);
			if (mapped.RowPitch == a_rowBytes) {
				std::memcpy(destination, a_source, a_rowBytes * a_height);
			} else {
				for (UINT y = 0; y < a_height; ++y) {
					std::memcpy(destination + std::size_t(y) * mapped.RowPitch, a_source + std::size_t(y) * a_rowBytes, a_rowBytes);
				}
			}
			s.context->Unmap(a_layer.texture, 0);
			return true;
		}

		// Halo's newest frame into the textures, if there's a new one. Halo
		// writes the slots round-robin under seqlocks: a slot that changed
		// during the copy is torn (a frame half-old, half three newer) and
		// copied again next time.
		void UploadLatest(const cr_frames* a_frames)
		{
			const auto latest = CR_LOAD_ACQ(&a_frames->latest);
			if (latest == 0 || latest > CR_FRAME_SLOTS) {
				return;
			}
			const auto& header = a_frames->slots[latest - 1];
			const auto  seq = CR_LOAD_ACQ(&header.seq);
			const auto  frame = header.frame;
			const auto  width = header.width;
			const auto  height = header.height;
			const auto  flags = header.flags;
			const auto  cameraFrame = header.camera_frame;
			if ((seq & 1) || (s.haveFrame && frame == s.lastFrame) || width == 0 || height == 0 ||
				width > CR_FRAME_MAX_WIDTH || height > CR_FRAME_MAX_HEIGHT ||
				!EnsureLayer(s.screen, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "screen layer")) {
				return;
			}
			const auto* pixels = a_frames->pixels[latest - 1];
			if (!Upload(s.screen, pixels, height, std::size_t(width) * 4)) {
				return;
			}
			const bool world = (flags & CR_FRAME_WORLD) &&
			                   EnsureLayer(s.world, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, "world layer") &&
			                   EnsureLayer(s.worldDepth, width, height, DXGI_FORMAT_R32_FLOAT, "world layer's depth") &&
			                   Upload(s.world, pixels + CR_FRAME_LAYER_BYTES, height, std::size_t(width) * 4) &&
			                   Upload(s.worldDepth, pixels + 2 * std::size_t(CR_FRAME_LAYER_BYTES), height, std::size_t(width) * 4);
			const bool weapon = (flags & CR_FRAME_MASK) && EnsureLayer(s.weapon, width, height, DXGI_FORMAT_R8_UNORM, "weapon's share") &&
			                    Upload(s.weapon, pixels + 3 * std::size_t(CR_FRAME_LAYER_BYTES), height, width);
			CR_FENCE_ACQ();
			if (CR_LOAD_ACQ(&header.seq) != seq) {
				++s.torn;
				return;
			}
			s.weaponShown = weapon;
			s.lastFrame = frame;
			s.haveFrame = true;
			s.worldShown = world;
			s.haloCamera = cameraFrame;
			s.haloTangentX = header.tangent_x;
			s.haloTangentY = header.tangent_y;
			++s.uploads;
			if (cameraFrame && cameraFrame == s.cameraFrame) {
				++s.matched;
			}
		}

		// Lockstep (docs §9): Halo draws the frame of the camera published as
		// Skyrim's world rendering began; wait a moment for it, so Halo's world
		// layer sits on this frame's picture, not on one a frame or two old.
		void WaitForCameraFrame(const cr_frames* a_frames)
		{
			const auto pending = std::exchange(s.cameraPending, 0u);
			if (!pending || WaitLimitMs() <= 0.0) {
				return;
			}
			++s.waits;
			const double start = NowMs();
			for (;;) {
				const auto latest = CR_LOAD_ACQ(&a_frames->latest);
				if (latest >= 1 && latest <= CR_FRAME_SLOTS) {
					const auto& header = a_frames->slots[latest - 1];
					const auto  seq = CR_LOAD_ACQ(&header.seq);
					if (!(seq & 1) && static_cast<std::int32_t>(header.camera_frame - pending) >= 0) {
						break;
					}
				}
				const double waited = NowMs() - start;
				if (waited >= WaitLimitMs()) {
					++s.late;
					break;
				}
				::SwitchToThread();
			}
			s.waitMs += NowMs() - start;
		}

		struct GradeConfig
		{
			bool  enabled = true;   // [Grade] bEnabled
			float strength = 1.0f;  // [Grade] fStrength
			bool  fog = true;       // [Grade] bFog
			float contrast = 0.25f; // [Grade] fContrast: how much of Skyrim's contrast
			// Chief's arms and weapon, before the grade (the HUD isn't touched)
			float weaponBrightness = 1.0f;   // [Grade] fWeaponBrightness
			float weaponSaturation = 0.9f;   // [Grade] fWeaponSaturation
			float weaponHighlights = 0.6f;   // [Grade] fWeaponHighlights: the knee, of the picture's brightest (1: none)
			bool  matchScene = true;         // [Grade] bWeaponMatchScene: metered from Skyrim's picture
			float shadowLift = 0.35f;        // [Grade] fWeaponShadowLift: of the picture's shadows' colour
			float exposureMin = 0.5f;        // [Grade] fWeaponExposureMin: in the dark
			float exposureMax = 1.1f;        // [Grade] fWeaponExposureMax: in daylight
			float sceneTint = 0.35f;         // [Grade] fWeaponSceneTint: towards the picture's mean hue
		};

		const GradeConfig& Grading()
		{
			static const GradeConfig config{ Settings::ReadBool(L"Grade", L"bEnabled", true),
				std::clamp(Settings::ReadFloat(L"Grade", L"fStrength", 1.0f), 0.0f, 1.0f), Settings::ReadBool(L"Grade", L"bFog", true),
				std::clamp(Settings::ReadFloat(L"Grade", L"fContrast", 0.25f), 0.0f, 1.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponBrightness", 1.0f), 0.0f, 2.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponSaturation", 0.9f), 0.0f, 2.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponHighlights", 0.6f), 0.0f, 1.0f),
				Settings::ReadBool(L"Grade", L"bWeaponMatchScene", true),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponShadowLift", 0.35f), 0.0f, 1.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponExposureMin", 0.5f), 0.1f, 4.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponExposureMax", 1.1f), 0.1f, 4.0f),
				std::clamp(Settings::ReadFloat(L"Grade", L"fWeaponSceneTint", 0.35f), 0.0f, 1.0f) };
			return config;
		}

		void PutColor(float (&a_out)[4], const RE::NiColor& a_color, float a_w)
		{
			a_out[0] = a_color.red;
			a_out[1] = a_color.green;
			a_out[2] = a_color.blue;
			a_out[3] = a_w;
		}

		RE::NiColor FromColor(const RE::Color& a_color)
		{
			return { a_color.red / 255.0f, a_color.green / 255.0f, a_color.blue / 255.0f };
		}

		// Skyrim's grade, logged when it changes much (a weather, a door), every
		// 5 s at most: the numbers to tune [Grade] by
		void LogGrade(const RE::ImageSpaceBaseData::Cinematic& a_cinematic, const RE::ImageSpaceBaseData::Tint& a_tint, float a_fade)
		{
			static std::array<float, 5> last{ -1.0f };
			static ULONGLONG            next = 0;
			const std::array<float, 5>  now{ a_cinematic.saturation, a_cinematic.brightness, a_cinematic.contrast, a_tint.amount, a_fade };
			const auto                  tick = ::GetTickCount64();
			bool                        changed = false;
			for (std::size_t i = 0; i < now.size(); ++i) {
				changed |= std::fabs(now[i] - last[i]) > 0.05f;
			}
			if (!changed || tick < next) {
				return;
			}
			last = now;
			next = tick + 5000;
			logger::info("grade: Skyrim's image space: saturation {:.2f}, brightness {:.2f}, contrast {:.2f} ({:.2f} of it on Halo's), tint ({:.2f} {:.2f} {:.2f}) {:.2f}, fade {:.2f}",
				now[0], now[1], now[2], Grading().contrast, a_tint.color.red, a_tint.color.green, a_tint.color.blue, now[3], now[4]);
		}

		// Skyrim's grade of its picture now (its image space: the weather's or
		// the cell's, with the effects over it, blended) and its fog (the
		// weather's outside, the cell's or its lighting template's inside),
		// for Halo's layers to look as its own world does
		void PutGrade(Params& a_params)
		{
			const auto& config = Grading();
			a_params.grade[0] = a_params.grade[1] = a_params.grade[2] = 1.0f;
			a_params.grade[3] = 0.0f;
			a_params.look[0] = config.weaponBrightness;
			a_params.look[1] = config.weaponSaturation;
			a_params.look[2] = config.weaponHighlights;
			a_params.look[3] = config.matchScene || config.weaponBrightness != 1.0f || config.weaponSaturation != 1.0f ||
			                           config.weaponHighlights < 1.0f ?
			                       1.0f :
			                       0.0f;
			a_params.lookScene[0] = config.shadowLift;
			a_params.lookScene[1] = std::min(config.exposureMin, config.exposureMax);
			a_params.lookScene[2] = config.exposureMax;
			a_params.lookScene[3] = s.metered ? 1.0f : 0.0f;
			a_params.lookTint[0] = config.sceneTint;
			if (!config.enabled) {
				return;
			}
			if (auto* manager = RE::ImageSpaceManager::GetSingleton()) {
				const auto& data = manager->GetRuntimeData().data;
				const auto& cinematic = data.baseData.cinematic;
				// nothing set yet (no game loaded): no grade
				if (cinematic.saturation > 0.0f || cinematic.brightness > 0.0f || cinematic.contrast > 0.0f) {
					a_params.grade[0] = cinematic.saturation;
					a_params.grade[1] = cinematic.brightness;
					// Skyrim's contrast is for its HDR picture before the tone
					// map (Community Shaders' too); on Halo's finished colours,
					// in full, it crushed the weapon's shadows and highlights
					// (the first in-game test)
					a_params.grade[2] = 1.0f + (cinematic.contrast - 1.0f) * config.contrast;
					a_params.grade[3] = config.strength;
					PutColor(a_params.tint, data.baseData.tint.color, std::clamp(data.baseData.tint.amount, 0.0f, 1.0f));
					a_params.fade[0] = data.modData.data[RE::ImageSpaceModData::kFadeR];
					a_params.fade[1] = data.modData.data[RE::ImageSpaceModData::kFadeG];
					a_params.fade[2] = data.modData.data[RE::ImageSpaceModData::kFadeB];
					a_params.fade[3] = std::clamp(data.modData.data[RE::ImageSpaceModData::kFadeAmount], 0.0f, 1.0f);
					LogGrade(cinematic, data.baseData.tint, a_params.fade[3]);
				}
			}
			if (!config.fog) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return;
			}
			float       nearPlane = 0.0f, farPlane = 0.0f, power = 1.0f, most = 1.0f;
			RE::NiColor nearColor, farColor;
			if (cell->IsInteriorCell()) {
				const auto* own = cell->GetLighting();
				if (!own) {
					return;
				}
				// what the cell takes from its lighting template
				auto*       lighting = cell->GetRuntimeData().lightingTemplate;
				const auto* shared = lighting ? &lighting->data : nullptr;
				const auto  from = [&](RE::INTERIOR_DATA::Inherit a_what) {
					return shared && own->lightingTemplateInheritanceFlags.any(a_what) ? shared : own;
				};
				using Inherit = RE::INTERIOR_DATA::Inherit;
				nearColor = FromColor(from(Inherit::kFogColor)->fogColorNear);
				farColor = FromColor(from(Inherit::kFogColor)->fogColorFar);
				nearPlane = from(Inherit::kFogNear)->fogNear;
				farPlane = from(Inherit::kFogFar)->fogFar;
				power = from(Inherit::kFogPower)->fogPower;
				most = from(Inherit::kFogMax)->fogClamp;
			} else if (auto* sky = RE::Sky::GetSingleton()) {
				nearColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogNear];
				farColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogFar];
				nearPlane = sky->fogNear;
				farPlane = sky->fogFar;
				power = sky->fogPower;
				most = sky->fogClamp;
			}
			if (farPlane <= nearPlane || farPlane <= 0.0f) {
				return;  // no fog
			}
			PutColor(a_params.fogNear, nearColor, power > 0.0f ? power : 1.0f);
			PutColor(a_params.fogFar, farColor, most > 0.0f ? std::min(most, 1.0f) : 1.0f);
			a_params.fogPlanes[0] = nearPlane;
			a_params.fogPlanes[1] = farPlane;
			a_params.fogPlanes[2] = 1.0f;
		}

		void SetParams()
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(s.context->Map(s.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* p = static_cast<Params*>(mapped.pData);
				*p = {};
				PutGrade(*p);
				p->depth[0] = s.nearPlane;
				p->depth[1] = s.farPlane;
				p->depth[2] = s.reversed == 1 ? 1.0f : 0.0f;
				p->depth[3] = CR_SKY_UNITS_PER_WU;
				PutBasis(p->now, s.current, s.current.tangentX, s.current.tangentY);
				// the camera of Halo's frame; unknown (too old, or Halo's own): as now
				const auto& then = s.cameras[s.haloCamera % kCameraRing];
				const bool  known = s.haloCamera && then.frame == s.haloCamera && s.haloTangentX > 0.0f && s.haloTangentY > 0.0f;
				PutBasis(p->halo, known ? then : s.current, known ? s.haloTangentX : s.current.tangentX,
					known ? s.haloTangentY : s.current.tangentY);
				++(known ? s.reprojected : s.unknownCamera);
				s.context->Unmap(s.params, 0);
			}
		}

		// Skyrim's picture metered, before Halo's layers go over it (Look, in
		// the shader): copied, mipped, and read from its mip of 64 texels
		// across at most into the meter's two texels, blended towards this
		// frame's over about half a second. False: not metered (the look
		// falls back to its fixed numbers).
		bool Meter(ID3D11Texture2D* a_backBuffer, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (!Grading().matchScene || s.meterFailed || !s.meterPs || !s.meterBlend) {
				return false;
			}
			const auto fail = [](const char* a_what, int a_format) {
				logger::warn("overlay: the weapon's look can't meter Skyrim's picture ({}, format {}): its fixed numbers instead", a_what,
					a_format);
				s.meterFailed = true;
				return false;
			};
			if (s.sceneCopy) {
				D3D11_TEXTURE2D_DESC cd{};
				s.sceneCopy->GetDesc(&cd);
				if (cd.Width != a_desc.Width || cd.Height != a_desc.Height || cd.Format != a_desc.Format) {
					SafeRelease(s.sceneSrv);
					SafeRelease(s.sceneCopy);
				}
			}
			if (!s.sceneCopy) {
				UINT support = 0;
				if (a_desc.SampleDesc.Count != 1 || FAILED(s.device->CheckFormatSupport(a_desc.Format, &support)) ||
					!(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
					return fail("no mips for its format", static_cast<int>(a_desc.Format));
				}
				D3D11_TEXTURE2D_DESC cd{};
				cd.Width = a_desc.Width;
				cd.Height = a_desc.Height;
				cd.MipLevels = 0;  // all of them
				cd.ArraySize = 1;
				cd.Format = a_desc.Format;
				cd.SampleDesc.Count = 1;
				cd.Usage = D3D11_USAGE_DEFAULT;
				cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				cd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
				if (FAILED(s.device->CreateTexture2D(&cd, nullptr, &s.sceneCopy)) ||
					FAILED(s.device->CreateShaderResourceView(s.sceneCopy, nullptr, &s.sceneSrv))) {
					SafeRelease(s.sceneCopy);
					return fail("no copy", static_cast<int>(a_desc.Format));
				}
				logger::info("overlay: the weapon's look meters Skyrim's picture ({}x{}, format {})", a_desc.Width, a_desc.Height,
					static_cast<int>(a_desc.Format));
			}
			if (!s.meter) {
				D3D11_TEXTURE2D_DESC md{};
				md.Width = 3;
				md.Height = 1;
				md.MipLevels = 1;
				md.ArraySize = 1;
				md.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				md.SampleDesc.Count = 1;
				md.Usage = D3D11_USAGE_DEFAULT;
				md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				if (FAILED(s.device->CreateTexture2D(&md, nullptr, &s.meter)) ||
					FAILED(s.device->CreateRenderTargetView(s.meter, nullptr, &s.meterRtv)) ||
					FAILED(s.device->CreateShaderResourceView(s.meter, nullptr, &s.meterSrv))) {
					return fail("no meter", static_cast<int>(md.Format));
				}
				md.Usage = D3D11_USAGE_STAGING;
				md.BindFlags = 0;
				md.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				s.device->CreateTexture2D(&md, nullptr, &s.meterStaging);
			}

			auto* context = s.context;
			// the numbers, ten times a second (the copy asked for a frame or
			// more ago): the lighting's surroundings (Surroundings), and the log
			const auto tick = ::GetTickCount64();
			if (s.stagingPending && s.meterStaging) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(context->Map(s.meterStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
					const auto* texels = static_cast<const float*>(mapped.pData);
					s.readKey = texels[0];
					s.readMean = { texels[8], texels[9], texels[10] };
					s.readAt = tick;
					if (tick >= s.nextMeterLog) {
						s.nextMeterLog = tick + 15000;
						const auto& config = Grading();
						const float exposure = std::clamp(std::sqrt(std::max(texels[0], 0.0f) / 0.3f),
							std::min(config.exposureMin, config.exposureMax), config.exposureMax);
						logger::info("overlay: the weapon's look: Skyrim's picture's key {:.3f}, brightest {:.3f}, shadows ({:.3f} {:.3f} {:.3f}),"
									 " mean ({:.3f} {:.3f} {:.3f}); exposure {:.2f}, highlights to {:.2f}",
							texels[0], texels[1], texels[4], texels[5], texels[6], texels[8], texels[9], texels[10],
							exposure * config.weaponBrightness, std::clamp(texels[1], 0.4f, 1.0f));
					}
					context->Unmap(s.meterStaging, 0);
					s.stagingPending = false;
				}
			}

			context->CopySubresourceRegion(s.sceneCopy, 0, 0, 0, 0, a_backBuffer, 0, nullptr);
			context->GenerateMips(s.sceneSrv);
			// a gap (a menu, a loading screen): the picture now, not blended
			const double now = NowMs();
			const double gap = now - s.meterAt;
			const float  weight = s.meterAt <= 0.0 || gap <= 0.0 || gap >= 2000.0 ? 1.0f : float(1.0 - std::exp(-gap / 500.0));
			s.meterAt = now;
			const float           factor[4]{ weight, weight, weight, weight };
			const D3D11_VIEWPORT  viewport{ 0.0f, 0.0f, 3.0f, 1.0f, 0.0f, 1.0f };
			ID3D11ShaderResourceView* none[6]{};
			ID3D11ShaderResourceView* sceneSrvs[6]{ nullptr, nullptr, nullptr, nullptr, s.sceneSrv, nullptr };
			context->PSSetShaderResources(0, 6, none);  // the meter isn't bound while it's drawn to
			context->OMSetRenderTargets(1, &s.meterRtv, nullptr);
			context->OMSetBlendState(s.meterBlend, factor, 0xFFFFFFFF);
			context->RSSetViewports(1, &viewport);
			context->PSSetShader(s.meterPs, nullptr, 0);
			context->PSSetShaderResources(0, 6, sceneSrvs);
			context->Draw(3, 0);
			context->PSSetShaderResources(0, 6, none);

			if (s.meterStaging && !s.stagingPending && tick >= s.nextReadback) {
				context->CopyResource(s.meterStaging, s.meter);
				s.stagingPending = true;
				s.nextReadback = tick + 100;
			}
			return true;
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			ID3D11Texture2D* backBuffer = nullptr;
			if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
				return;
			}
			D3D11_TEXTURE2D_DESC bbDesc{};
			backBuffer->GetDesc(&bbDesc);
			ID3D11RenderTargetView* rtv = nullptr;
			const auto              hr = s.device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
			if (FAILED(hr)) {
				SafeRelease(backBuffer);
				static bool logged = false;
				if (!std::exchange(logged, true)) {
					logger::error("overlay: no render target view of the back buffer (format {})", static_cast<int>(bbDesc.Format));
				}
				return;
			}

			// Skyrim's pipeline state, which we put back afterwards.
			auto*                     context = s.context;
			ID3D11RenderTargetView*   oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   oldDsv = nullptr;
			ID3D11BlendState*         oldBlend = nullptr;
			float                     oldFactor[4]{};
			UINT                      oldMask = 0;
			ID3D11RasterizerState*    oldRaster = nullptr;
			ID3D11DepthStencilState*  oldDepth = nullptr;
			UINT                      oldStencil = 0;
			D3D11_VIEWPORT            oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_PRIMITIVE_TOPOLOGY  oldTopology{};
			ID3D11InputLayout*        oldLayout = nullptr;
			ID3D11VertexShader*       oldVs = nullptr;
			ID3D11GeometryShader*     oldGs = nullptr;
			ID3D11HullShader*         oldHs = nullptr;
			ID3D11DomainShader*       oldDs = nullptr;
			ID3D11PixelShader*        oldPs = nullptr;
			ID3D11ShaderResourceView* oldSrvs[6]{};
			ID3D11SamplerState*       oldSamplers[2]{};
			ID3D11Buffer*             oldCb = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
			context->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
			context->RSGetState(&oldRaster);
			context->OMGetDepthStencilState(&oldDepth, &oldStencil);
			context->RSGetViewports(&oldViewportCount, oldViewports);
			context->IAGetPrimitiveTopology(&oldTopology);
			context->IAGetInputLayout(&oldLayout);
			context->VSGetShader(&oldVs, nullptr, nullptr);
			context->GSGetShader(&oldGs, nullptr, nullptr);
			context->HSGetShader(&oldHs, nullptr, nullptr);
			context->DSGetShader(&oldDs, nullptr, nullptr);
			context->PSGetShader(&oldPs, nullptr, nullptr);
			context->PSGetShaderResources(0, 6, oldSrvs);
			context->PSGetSamplers(0, 2, oldSamplers);
			context->PSGetConstantBuffers(0, 1, &oldCb);

			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(bbDesc.Width), float(bbDesc.Height), 0.0f, 1.0f };
			const float          factor[4]{};
			ID3D11SamplerState*  samplers[2]{ s.linear, s.point };
			context->OMSetDepthStencilState(s.depth, 0);
			context->RSSetState(s.raster);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(s.vs, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->PSSetSamplers(0, 2, samplers);
			context->PSSetConstantBuffers(0, 1, &s.params);
			// Skyrim's picture as it is, before Halo's go over it: the
			// weapon's look is matched to it
			s.metered = !Handoff::Active() && s.meterPs && Meter(backBuffer, bbDesc);
			SafeRelease(backBuffer);
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->OMSetBlendState(s.blend, factor, 0xFFFFFFFF);
			context->RSSetViewports(1, &viewport);

			// The world layer, where Skyrim's own picture isn't nearer; then
			// the screen layer over everything. Both graded as Skyrim's picture
			// is (the screen layer's weapon, not its HUD).
			SetParams();
			if (s.worldShown && s.depthCopied && s.reversed >= 0 && s.current.frame) {
				ID3D11ShaderResourceView* srvs[4]{ s.world.srv, s.worldDepth.srv, s.depthCopySrv, nullptr };
				context->PSSetShader(s.worldPs, nullptr, 0);
				context->PSSetShaderResources(0, 4, srvs);
				context->Draw(3, 0);
			}
			if (!Handoff::Active()) {  // Skyrim has the player: no weapon or HUD of Chief's
				ID3D11ShaderResourceView* srvs[6]{ s.screen.srv, nullptr, nullptr, s.weaponShown ? s.weapon.srv : nullptr, nullptr,
					s.metered ? s.meterSrv : nullptr };
				context->PSSetShader(s.screenPs, nullptr, 0);
				context->PSSetShaderResources(0, 6, srvs);
				context->Draw(3, 0);
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
			context->OMSetBlendState(oldBlend, oldFactor, oldMask);
			context->OMSetDepthStencilState(oldDepth, oldStencil);
			context->RSSetState(oldRaster);
			context->RSSetViewports(oldViewportCount, oldViewports);
			context->IASetPrimitiveTopology(oldTopology);
			context->IASetInputLayout(oldLayout);
			context->VSSetShader(oldVs, nullptr, 0);
			context->GSSetShader(oldGs, nullptr, 0);
			context->HSSetShader(oldHs, nullptr, 0);
			context->DSSetShader(oldDs, nullptr, 0);
			context->PSSetShader(oldPs, nullptr, 0);
			context->PSSetShaderResources(0, 6, oldSrvs);
			context->PSSetSamplers(0, 2, oldSamplers);
			context->PSSetConstantBuffers(0, 1, &oldCb);
			for (auto*& old : oldRtv) {
				SafeRelease(old);
			}
			for (auto*& old : oldSrvs) {
				SafeRelease(old);
			}
			for (auto*& old : oldSamplers) {
				SafeRelease(old);
			}
			SafeRelease(oldDsv);
			SafeRelease(oldBlend);
			SafeRelease(oldRaster);
			SafeRelease(oldDepth);
			SafeRelease(oldLayout);
			SafeRelease(oldVs);
			SafeRelease(oldGs);
			SafeRelease(oldHs);
			SafeRelease(oldDs);
			SafeRelease(oldPs);
			SafeRelease(oldCb);
			SafeRelease(rtv);
		}

		void Composite(IDXGISwapChain* a_swapChain)
		{
			auto& link = Link::Get();
			const auto* frames = link.Frames();
			const bool  depthCopied = std::exchange(s.depthCopied, false);
			if (!frames) {
				s.haveFrame = false;
				s.lastPublished = 0;
				s.cameraPending = 0;
				return;
			}

			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(a_swapChain->GetDesc(&desc))) {
				return;
			}
			cr_display display{};
			display.width = desc.BufferDesc.Width;
			display.height = desc.BufferDesc.Height;
			display.flags = Enabled() ? CR_DISPLAY_OVERLAY : 0u;
			display.frame = ++s.displayFrame;
			link.SendDisplay(display);
			if (!Enabled()) {
				return;
			}
			if (!s.device) {
				ID3D11Device* device = nullptr;
				if (SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
					InitResources(device);
					device->Release();
				}
			}
			if (!s.device || s.initFailed) {
				return;
			}

			WaitForCameraFrame(frames);
			const auto now = ::GetTickCount64();
			const auto published = CR_LOAD_ACQ(&frames->published);
			if (published != s.lastPublished) {
				s.lastPublished = published;
				s.lastPublishedAt = now;
			}
			UploadLatest(frames);

			const bool show = s.haveFrame && now - s.lastPublishedAt < kStaleMs && Input::InGameplay();
			if (show != s.shown) {
				s.shown = show;
				logger::info("overlay: {}", show ? "showing Halo's layers" : "hidden");
			}
			if (now >= s.nextReport) {
				if (s.nextReport && (s.uploads || s.torn)) {
					logger::info("overlay: in the last 30 s, {} frames from Halo ({} torn copies), {} for this frame's camera;"
								 " waited for {} frames, {:.1f} ms on average, {} too late; world layer reprojected {} times"
								 " ({} from an unknown camera); Skyrim's depth {}",
						s.uploads, s.torn, s.matched, s.waits, s.waits ? s.waitMs / double(s.waits) : 0.0, s.late,
						s.reprojected, s.unknownCamera, s.reversed < 0 ? "not read yet" : s.reversed ? "reversed" : "standard");
					if (s.haloTangentY > 0.0f && std::fabs(s.haloTangentY - s.lastLoggedTangent) > 0.001f) {
						s.lastLoggedTangent = s.haloTangentY;
						logger::info("overlay: views: Skyrim's {:.4f} x {:.4f}, Halo's {:.4f} x {:.4f} (the world layer is mapped from Halo's to Skyrim's)",
							s.current.tangentX, s.current.tangentY, s.haloTangentX, s.haloTangentY);
					}
				}
				s.uploads = s.torn = s.waits = s.late = s.matched = s.reprojected = s.unknownCamera = 0;
				s.waitMs = 0.0;
				s.nextReport = now + 30000;
			}
			if (show) {
				s.depthCopied = depthCopied;
				Draw(a_swapChain);
				s.depthCopied = false;
			}
		}

		HRESULT WINAPI PresentHook(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			try {
				if (!std::exchange(composited, false)) {
					Composite(a_swapChain);
				}
			} catch (...) {
			}
			return originalPresent(a_swapChain, a_sync, a_flags);
		}

		bool UnderMenus()
		{
			static const bool value = Settings::ReadBool(L"Overlay", L"bUnderSkyrimMenus", true);
			return value;
		}

		// ---- under Skyrim's menus -----------------------------------------

		// IMenu::PostDisplay (vtable slot 6) draws a menu's movie. Each menu
		// class's slot is patched once, as one of them opens; what was there
		// (another mod's hook, or Skyrim's own) is called after.
		using PostDisplayFn = void (*)(RE::IMenu*);
		constexpr std::size_t kPostDisplaySlot = 6;

		struct MenuClass
		{
			void**        vtable = nullptr;
			PostDisplayFn original = nullptr;
		};
		std::mutex             menuClassesLock;
		std::vector<MenuClass> menuClasses;

		void PostDisplayHook(RE::IMenu* a_menu)
		{
			auto**        vtable = *reinterpret_cast<void***>(a_menu);
			PostDisplayFn original = nullptr;
			{
				std::scoped_lock lock(menuClassesLock);
				for (const auto& entry : menuClasses) {
					if (entry.vtable == vtable) {
						original = entry.original;
						break;
					}
				}
			}
			if (!composited && swapChain) {
				composited = true;  // the frame's first menu: Halo's layers under it
				try {
					Composite(swapChain);
				} catch (...) {
				}
			}
			if (original) {
				original(a_menu);
			}
		}

		void HookMenu(RE::IMenu* a_menu, std::string_view a_name)
		{
			if (!a_menu) {
				return;
			}
			auto**           vtable = *reinterpret_cast<void***>(a_menu);
			std::scoped_lock lock(menuClassesLock);
			for (const auto& entry : menuClasses) {
				if (entry.vtable == vtable) {
					return;
				}
			}
			auto* slot = &vtable[kPostDisplaySlot];
			if (*slot == reinterpret_cast<void*>(&PostDisplayHook)) {
				return;
			}
			menuClasses.push_back({ vtable, reinterpret_cast<PostDisplayFn>(*slot) });
			DWORD oldProtect = 0;
			::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
			*slot = reinterpret_cast<void*>(&PostDisplayHook);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			logger::info("overlay: Halo's layers go under {} (and any menu of its kind)", a_name);
		}

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* Get()
			{
				static MenuSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening) {
					if (auto* ui = RE::UI::GetSingleton()) {
						const std::string_view name{ a_event->menuName.c_str() };
						HookMenu(ui->GetMenu(name).get(), name);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void HookMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!UnderMenus() || !Enabled() || !ui) {
				logger::info("overlay: Halo's layers over Skyrim's menus (at Present)");
				return;
			}
			for (auto& menu : ui->menuStack) {
				HookMenu(menu.get(), "an open menu");
			}
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
		}

		// ---- inside Skyrim's frame: the camera out, the depth in ----------

		// The camera this frame is rendered with, to Halo (lockstep).
		void PublishCamera()
		{
			auto& link = Link::Get();
			auto* camera = RE::Main::WorldRootCamera();
			if (!Enabled() || !link.Frames() || !camera) {
				return;
			}
			const auto& world = camera->world;
			const auto& frustum = camera->GetRuntimeData2().viewFrustum;
			cr_camera out{};
			out.frame = ++s.cameraFrame;
			if (out.frame == 0) {
				out.frame = ++s.cameraFrame;  // 0 means "none"
			}
			out.eye = { world.translate.x, world.translate.y, world.translate.z };
			// NiCamera: its columns are forward, up, right
			out.forward = { world.rotate.entry[0][0], world.rotate.entry[1][0], world.rotate.entry[2][0] };
			out.up = { world.rotate.entry[0][1], world.rotate.entry[1][1], world.rotate.entry[2][1] };
			out.vertical_fov = std::atan(frustum.fTop) - std::atan(frustum.fBottom);
			out.near_plane = frustum.fNear;
			out.far_plane = frustum.fFar;
			link.SendCamera(out);
			s.cameraPending = out.frame;
			PublishedCamera kept;
			kept.frame = out.frame;
			kept.forward = { out.forward.x, out.forward.y, out.forward.z };
			kept.up = { out.up.x, out.up.y, out.up.z };
			kept.right = kept.forward.Cross(kept.up);  // screen right (Z up, right-handed), as Halo's view has it
			kept.tangentX = 0.5f * std::fabs(frustum.fRight - frustum.fLeft);
			kept.tangentY = 0.5f * std::fabs(frustum.fTop - frustum.fBottom);
			s.cameras[out.frame % kCameraRing] = kept;
			s.current = kept;
			s.nearPlane = frustum.fNear;
			s.farPlane = frustum.fFar;
		}

		// Skyrim's convention, from the depth itself: a perspective depth
		// buffer is mostly near 1 (standard) or near 0 (reversed). Read once.
		void LearnDepthConvention(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_depth)
		{
			D3D11_TEXTURE2D_DESC dd{};
			a_depth->GetDesc(&dd);
			if (dd.Format != DXGI_FORMAT_R24G8_TYPELESS && dd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT) {
				logger::warn("overlay: Skyrim's depth is format {}, not read; taken as standard", static_cast<int>(dd.Format));
				s.reversed = 0;
				return;
			}
			D3D11_TEXTURE2D_DESC sd = dd;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			ID3D11Texture2D* staging = nullptr;
			if (FAILED(s.device->CreateTexture2D(&sd, nullptr, &staging))) {
				return;
			}
			a_context->CopyResource(staging, a_depth);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
				std::vector<float> values;
				for (UINT y = 0; y < dd.Height; y += 16) {
					const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch);
					for (UINT x = 0; x < dd.Width; x += 16) {
						values.push_back(float(row[x] & 0xFFFFFF) / 16777215.0f);
					}
				}
				a_context->Unmap(staging, 0);
				if (!values.empty()) {
					std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
					const float median = values[values.size() / 2];
					const auto [low, high] = std::minmax_element(values.begin(), values.end());
					s.reversed = median < 0.5f ? 1 : 0;
					logger::info("overlay: Skyrim's depth: median {:.5f}, {:.5f}-{:.5f}, near {:.1f}, far {:.0f}: {}", median, *low, *high,
						s.nearPlane, s.farPlane, s.reversed ? "reversed" : "standard");
				}
			}
			SafeRelease(staging);
		}

		// Skyrim's depth as its world rendering ends (before the HUD and
		// post-processing), for the world layer's test at Present.
		void CopyDepth()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!Enabled() || !s.device || !renderer || !Link::Get().Frames()) {
				return;
			}
			const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto*       texture = reinterpret_cast<ID3D11Texture2D*>(depth.texture);
			auto*       srv = reinterpret_cast<ID3D11ShaderResourceView*>(depth.depthSRV);
			auto*       context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			if (!texture || !srv || !context) {
				return;
			}
			D3D11_TEXTURE2D_DESC dd{};
			texture->GetDesc(&dd);
			if (s.depthCopy) {
				D3D11_TEXTURE2D_DESC cd{};
				s.depthCopy->GetDesc(&cd);
				if (cd.Width != dd.Width || cd.Height != dd.Height || cd.Format != dd.Format) {
					SafeRelease(s.depthCopySrv);
					SafeRelease(s.depthCopy);
				}
			}
			if (!s.depthCopy) {
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				srv->GetDesc(&sv);
				D3D11_TEXTURE2D_DESC cd = dd;
				cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				cd.Usage = D3D11_USAGE_DEFAULT;
				cd.CPUAccessFlags = 0;
				cd.MiscFlags = 0;
				if (dd.SampleDesc.Count != 1 || FAILED(s.device->CreateTexture2D(&cd, nullptr, &s.depthCopy)) ||
					FAILED(s.device->CreateShaderResourceView(s.depthCopy, &sv, &s.depthCopySrv))) {
					static bool logged = false;
					if (!std::exchange(logged, true)) {
						logger::error("overlay: no copy of Skyrim's depth ({}x{}, format {}, {} samples): Halo's world layer won't show",
							dd.Width, dd.Height, static_cast<int>(dd.Format), dd.SampleDesc.Count);
					}
					SafeRelease(s.depthCopy);
					return;
				}
				logger::info("overlay: Skyrim's depth is {}x{}, format {}", dd.Width, dd.Height, static_cast<int>(dd.Format));
			}
			context->CopyResource(s.depthCopy, texture);
			s.depthCopied = true;
			// after a few seconds of play, when the picture has a world in it
			if (s.reversed < 0 && ++s.depthCopies >= 300 && Input::InGameplay()) {
				LearnDepthConvention(context, texture);
			}
		}

		struct RenderWorldHook
		{
			static void thunk(bool a_arg)
			{
				try {
					PublishCamera();
				} catch (...) {
				}
				func(a_arg);
				try {
					CopyDepth();
				} catch (...) {
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Main::RenderWorld's call in the frame function (AE; SkyCraft's
		// in-frame drawing hooks it too).
		void HookRenderWorld()
		{
			if (!REL::Module::IsAE()) {
				logger::warn("overlay: Skyrim's world rendering is hooked on AE only: no world layer");
				return;
			}
			const auto target = REL::ID(107142).address();
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
			if (sites.size() != 1) {
				logger::warn("overlay: {} calls to Main::RenderWorld, not one: no world layer", sites.size());
				return;
			}
			RenderWorldHook::func = SKSE::GetTrampoline().write_call<5>(sites[0], RenderWorldHook::thunk);
			logger::info("overlay: hooked Skyrim's world rendering (camera to Halo, depth for the world layer)");
		}
	}

	bool Surroundings(float& a_key, RE::NiColor& a_mean)
	{
		if (!s.readAt || ::GetTickCount64() - s.readAt > 2000) {
			return false;
		}
		a_key = s.readKey;
		a_mean = s.readMean;
		return true;
	}

	void Install()
	{
		auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
		swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
		if (!swapChain) {
			logger::error("overlay: no swap chain yet; Halo's layers won't show");
			return;
		}
		// IDXGISwapChain::Present is the vtable's ninth entry. Patching the
		// vtable chains with other Present hooks (SSE Display Tweaks).
		auto** vtable = *reinterpret_cast<void***>(swapChain);
		DWORD  oldProtect = 0;
		::VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
		originalPresent = reinterpret_cast<PresentFn>(vtable[8]);
		vtable[8] = reinterpret_cast<void*>(&PresentHook);
		::VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
		logger::info("overlay: Present hooked ({})", Enabled() ? "on" : "off: [Overlay] bEnabled=0");
		HookRenderWorld();
		HookMenus();
	}
}
