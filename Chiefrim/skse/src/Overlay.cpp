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

		struct
		{
			ID3D11Device*             device = nullptr;
			ID3D11DeviceContext*      context = nullptr;
			ID3D11Texture2D*          texture = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
			UINT                      textureWidth = 0, textureHeight = 0;
			ID3D11VertexShader*       vs = nullptr;
			ID3D11PixelShader*        ps = nullptr;
			ID3D11BlendState*         blend = nullptr;
			ID3D11SamplerState*       sampler = nullptr;
			ID3D11RasterizerState*    raster = nullptr;
			ID3D11DepthStencilState*  depth = nullptr;
			bool                      initFailed = false;

			std::uint32_t displayFrame = 0;
			std::uint32_t lastFrame = 0;      // Halo's frame count in the texture
			bool          haveFrame = false;
			std::uint32_t lastPublished = 0;
			ULONGLONG     lastPublishedAt = 0;
			std::uint64_t uploads = 0, torn = 0;
			ULONGLONG     nextReport = 0;
			bool          shown = false;
		} s;

		// A full-screen triangle; the picture is premultiplied, scaled to the
		// back buffer if Halo's is smaller (screens over CR_FRAME_MAX_*).
		constexpr char kShader[] = R"(
Texture2D picture : register(t0);
SamplerState linear_clamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
float4 PSMain(VSOut i) : SV_Target
{
	return picture.Sample(linear_clamp, i.uv);
}
)";

		bool Enabled()
		{
			static const bool value = Settings::ReadBool(L"Overlay", L"bEnabled", true);
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

		bool InitResources(IDXGISwapChain* a_swapChain)
		{
			if (s.device) {
				return true;
			}
			if (s.initFailed || FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s.device)))) {
				s.initFailed = true;
				return false;
			}
			s.device->GetImmediateContext(&s.context);

			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSMain", "ps_5_0", &psBlob)) {
				SafeRelease(vsBlob);
				s.initFailed = true;
				return false;
			}
			s.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &s.vs);
			s.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &s.ps);
			SafeRelease(vsBlob);
			SafeRelease(psBlob);

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
			s.device->CreateSamplerState(&sd, &s.sampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			s.device->CreateRasterizerState(&rd, &s.raster);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.StencilEnable = FALSE;
			s.device->CreateDepthStencilState(&dd, &s.depth);

			const bool ok = s.vs && s.ps && s.blend && s.sampler && s.raster && s.depth;
			logger::info("overlay: compositor {}", ok ? "ready" : "failed to initialize");
			s.initFailed = !ok;
			return ok;
		}

		bool EnsureTexture(UINT a_width, UINT a_height)
		{
			if (s.texture && s.textureWidth == a_width && s.textureHeight == a_height) {
				return true;
			}
			SafeRelease(s.srv);
			SafeRelease(s.texture);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = a_width;
			td.Height = a_height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(s.device->CreateTexture2D(&td, nullptr, &s.texture)) ||
				FAILED(s.device->CreateShaderResourceView(s.texture, nullptr, &s.srv))) {
				logger::error("overlay: a {}x{} texture can't be made", a_width, a_height);
				SafeRelease(s.texture);
				return false;
			}
			s.textureWidth = a_width;
			s.textureHeight = a_height;
			logger::info("overlay: Halo's frames are {}x{}", a_width, a_height);
			return true;
		}

		// Halo's newest frame into the texture, if there's a new one. Halo
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
			if ((seq & 1) || (s.haveFrame && frame == s.lastFrame) || width == 0 || height == 0 ||
				width > CR_FRAME_MAX_WIDTH || height > CR_FRAME_MAX_HEIGHT || !EnsureTexture(width, height)) {
				return;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(s.context->Map(s.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return;
			}
			const auto* source = a_frames->pixels[latest - 1];
			const auto  rowBytes = std::size_t(width) * 4;
			auto*       destination = static_cast<std::uint8_t*>(mapped.pData);
			if (mapped.RowPitch == rowBytes) {
				std::memcpy(destination, source, rowBytes * height);
			} else {
				for (UINT y = 0; y < height; ++y) {
					std::memcpy(destination + std::size_t(y) * mapped.RowPitch, source + std::size_t(y) * rowBytes, rowBytes);
				}
			}
			s.context->Unmap(s.texture, 0);
			CR_FENCE_ACQ();
			if (CR_LOAD_ACQ(&header.seq) != seq) {
				++s.torn;
				return;
			}
			s.lastFrame = frame;
			s.haveFrame = true;
			++s.uploads;
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
			ID3D11ShaderResourceView* oldSrv = nullptr;
			ID3D11SamplerState*       oldSampler = nullptr;
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
			context->PSGetShaderResources(0, 1, &oldSrv);
			context->PSGetSamplers(0, 1, &oldSampler);

			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(bbDesc.Width), float(bbDesc.Height), 0.0f, 1.0f };
			const float          factor[4]{};
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
			context->PSSetShader(s.ps, nullptr, 0);
			context->PSSetShaderResources(0, 1, &s.srv);
			context->PSSetSamplers(0, 1, &s.sampler);
			context->Draw(3, 0);

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
			context->PSSetShaderResources(0, 1, &oldSrv);
			context->PSSetSamplers(0, 1, &oldSampler);
			for (auto*& old : oldRtv) {
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
			SafeRelease(oldSrv);
			SafeRelease(oldSampler);
			SafeRelease(rtv);
		}

		void Composite(IDXGISwapChain* a_swapChain)
		{
			auto& link = Link::Get();
			const auto* frames = link.Frames();
			if (!frames) {
				s.haveFrame = false;
				s.lastPublished = 0;
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
			if (!Enabled() || !InitResources(a_swapChain)) {
				return;
			}

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
					logger::info("overlay: {} frames from Halo in the last 30 s ({} torn copies)", s.uploads, s.torn);
				}
				s.uploads = s.torn = 0;
				s.nextReport = now + 30000;
			}
			if (show) {
				Draw(a_swapChain);
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
	}
}
