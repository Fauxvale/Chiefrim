/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Overlay.h"
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
		PresentFn originalPresent = nullptr;

		// One of Halo's pictures, as a texture.
		struct Layer
		{
			ID3D11Texture2D*          texture = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			UINT                      width = 0, height = 0;
		};

		struct alignas(16) Params
		{
			float depth[4];  // Skyrim's near, far, 1 if its depth is reversed, Skyrim units per Halo world unit
		};

		struct
		{
			ID3D11Device*             device = nullptr;
			ID3D11DeviceContext*      context = nullptr;
			Layer                     screen, world, worldDepth;
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

			// Skyrim's depth, copied as its world rendering finishes
			ID3D11Texture2D*          depthCopy = nullptr;
			ID3D11ShaderResourceView* depthCopySrv = nullptr;
			bool                      depthCopied = false;  // this frame
			float                     nearPlane = 15.0f, farPlane = 353840.0f;
			int                       reversed = -1;         // unknown until a look at the depth
			std::uint32_t             depthCopies = 0;

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
cbuffer Params : register(b0) { float4 depthParams; };
Texture2D picture : register(t0);
Texture2D<float> haloDepth : register(t1);
Texture2D<float> skyrimDepth : register(t2);
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
float4 PSScreen(VSOut i) : SV_Target
{
	return picture.Sample(linear_clamp, i.uv);
}
// Skyrim's view distance (along the view, Skyrim units) from its depth buffer.
float SkyrimViewDepth(float d)
{
	float n = depthParams.x, f = depthParams.y;
	return depthParams.z > 0.5 ? n * f / (n + d * (f - n)) : n * f / (f - d * (f - n));
}
float4 PSWorld(VSOut i) : SV_Target
{
	float4 c = picture.Sample(linear_clamp, i.uv);
	if (all(c == 0))
		discard;
	float halo = haloDepth.SampleLevel(point_clamp, i.uv, 0) * depthParams.w;
	float skyrim = SkyrimViewDepth(skyrimDepth.SampleLevel(point_clamp, i.uv, 0));
	// a little slack: Halo's decals lie on Skyrim's own surfaces
	if (halo > skyrim * 1.003 + 4.0)
		discard;
	return c;
}
)";

		bool Enabled()
		{
			static const bool value = Settings::ReadBool(L"Overlay", L"bEnabled", true);
			return value;
		}

		// How long Present waits for Halo's frame of this frame's camera.
		double WaitLimitMs()
		{
			static const double value = std::clamp(Settings::ReadFloat(L"Overlay", L"fWaitMs", 12.0f), 0.0f, 50.0f);
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

			ID3DBlob *vsBlob = nullptr, *screenBlob = nullptr, *worldBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSScreen", "ps_5_0", &screenBlob) ||
				!Compile("PSWorld", "ps_5_0", &worldBlob)) {
				SafeRelease(vsBlob);
				SafeRelease(screenBlob);
				s.initFailed = true;
				return false;
			}
			s.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s.vs);
			s.device->CreatePixelShader(screenBlob->GetBufferPointer(), screenBlob->GetBufferSize(), nullptr, &s.screenPs);
			s.device->CreatePixelShader(worldBlob->GetBufferPointer(), worldBlob->GetBufferSize(), nullptr, &s.worldPs);
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
			CR_FENCE_ACQ();
			if (CR_LOAD_ACQ(&header.seq) != seq) {
				++s.torn;
				return;
			}
			s.lastFrame = frame;
			s.haveFrame = true;
			s.worldShown = world;
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
			if (!pending) {
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

		void SetParams()
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(s.context->Map(s.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* p = static_cast<Params*>(mapped.pData);
				p->depth[0] = s.nearPlane;
				p->depth[1] = s.farPlane;
				p->depth[2] = s.reversed == 1 ? 1.0f : 0.0f;
				p->depth[3] = CR_SKY_UNITS_PER_WU;
				s.context->Unmap(s.params, 0);
			}
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
			SafeRelease(backBuffer);
			if (FAILED(hr)) {
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
			ID3D11ShaderResourceView* oldSrvs[3]{};
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
			context->PSGetShaderResources(0, 3, oldSrvs);
			context->PSGetSamplers(0, 2, oldSamplers);
			context->PSGetConstantBuffers(0, 1, &oldCb);

			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(bbDesc.Width), float(bbDesc.Height), 0.0f, 1.0f };
			const float          factor[4]{};
			ID3D11SamplerState*  samplers[2]{ s.linear, s.point };
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->OMSetBlendState(s.blend, factor, 0xFFFFFFFF);
			context->OMSetDepthStencilState(s.depth, 0);
			context->RSSetState(s.raster);
			context->RSSetViewports(1, &viewport);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(s.vs, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->PSSetSamplers(0, 2, samplers);
			context->PSSetConstantBuffers(0, 1, &s.params);

			// The world layer, where Skyrim's own picture isn't nearer; then
			// the screen layer over everything.
			if (s.worldShown && s.depthCopied && s.reversed >= 0) {
				ID3D11ShaderResourceView* srvs[3]{ s.world.srv, s.worldDepth.srv, s.depthCopySrv };
				SetParams();
				context->PSSetShader(s.worldPs, nullptr, 0);
				context->PSSetShaderResources(0, 3, srvs);
				context->Draw(3, 0);
			}
			{
				ID3D11ShaderResourceView* srvs[3]{ s.screen.srv, nullptr, nullptr };
				context->PSSetShader(s.screenPs, nullptr, 0);
				context->PSSetShaderResources(0, 3, srvs);
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
			context->PSSetShaderResources(0, 3, oldSrvs);
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
								 " waited for {} frames, {:.1f} ms on average, {} too late; Skyrim's depth {}",
						s.uploads, s.torn, s.matched, s.waits, s.waits ? s.waitMs / double(s.waits) : 0.0, s.late,
						s.reversed < 0 ? "not read yet" : s.reversed ? "reversed" : "standard");
				}
				s.uploads = s.torn = s.waits = s.late = s.matched = 0;
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
				Composite(a_swapChain);
			} catch (...) {
			}
			return originalPresent(a_swapChain, a_sync, a_flags);
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

	void Install()
	{
		auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
		auto* swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
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
	}
}
