#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>
#include <reshade.hpp>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern "C" __declspec(dllexport) const char *NAME = "DX11 FrameGen Preview";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Experimental DX11 pyramid optical-flow frame generation preview for ReShade.";

namespace fg
{
    struct Settings
    {
        bool enabled = false;
        bool pyramid = true;
        bool smooth = false;
        bool hud_protect = true;
        bool debug_overlay = true;
        bool fast_mode = true;
        bool extra_present = false;
        bool active_preview = true;
        bool visible_test = false;
        int present_interval = 1;
        int preview_divisor = 1;
        int flow_downscale = 24;
        bool fast_search = true;
        float strength = 0.75f;
    };

    Settings g_settings;
    std::atomic<unsigned long long> g_real_frames{0};
    std::atomic<unsigned long long> g_gen_frames{0};
    std::atomic<unsigned long long> g_extra_presents{0};
    std::atomic<unsigned long long> g_draw_attempts{0};
    std::atomic<unsigned long long> g_draw_success{0};
    bool g_inside_extra_present = false;
    const char *g_status = "idle";
    HRESULT g_last_hr = S_OK;

    ID3D11Device *g_dev = nullptr;
    ID3D11DeviceContext *g_ctx = nullptr;
    ID3D11VertexShader *g_vs = nullptr;
    ID3D11PixelShader *g_ps_flow = nullptr;
    ID3D11PixelShader *g_ps_smooth = nullptr;
    ID3D11PixelShader *g_ps_interp = nullptr;
    ID3D11SamplerState *g_smp = nullptr;
    ID3D11BlendState *g_blend = nullptr;
    ID3D11Buffer *g_cb = nullptr;

    ID3D11Texture2D *g_prev_tex = nullptr;
    ID3D11ShaderResourceView *g_prev_srv = nullptr;
    ID3D11Texture2D *g_curr_tex = nullptr;
    ID3D11ShaderResourceView *g_curr_srv = nullptr;
    ID3D11Texture2D *g_flow1 = nullptr;
    ID3D11RenderTargetView *g_flow1_rtv = nullptr;
    ID3D11ShaderResourceView *g_flow1_srv = nullptr;
    ID3D11Texture2D *g_flow2 = nullptr;
    ID3D11RenderTargetView *g_flow2_rtv = nullptr;
    ID3D11ShaderResourceView *g_flow2_srv = nullptr;

    bool g_pipeline_ready = false;
    bool g_have_prev = false;
    unsigned g_w = 0, g_h = 0, g_lw = 0, g_lh = 0;
    DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;

    constexpr int kMinDS = 8;
    constexpr int kMaxDS = 32;

    struct FlowCB
    {
        unsigned W, H, lowW, lowH;
        float invW, invH;
        int searchR, searchS;
        int patchP, ds;
        int usePyramid, smoothFlow;
        int hudProtect, fastMode;
        float strength;
        float pad[5]; // pad FlowCB to 80 bytes (multiple of 16) so CreateBuffer succeeds
    };

    const char *kShader = R"HLSL(
        cbuffer FlowCB : register(b0) {
            uint W,H,lowW,lowH;
            float invW,invH;
            int searchR,searchS;
            int patchP,ds;
            int usePyramid,smoothFlow;
            int hudProtect,fastMode;
            float strength,pad1,pad2,pad3;
        };
        Texture2D texPrev : register(t0);
        Texture2D texCurr : register(t1);
        Texture2D flowTex : register(t2);
        SamplerState smp : register(s0);

        struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
        VSOut VSMain(uint id : SV_VertexID) {
            VSOut o;
            o.uv = float2((id << 1) & 2, id & 2);
            o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return o;
        }
        float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
        float sad_at(float2 cuv, float2 candPx, float spreadPx) {
            float2 offUv = candPx * float2(invW, invH);
            float sad = 0;
            [unroll] for (int py = -1; py <= 1; py++)
            [unroll] for (int px = -1; px <= 1; px++) {
                float2 p = cuv + float2(px, py) * float2(invW, invH) * spreadPx;
                sad += abs(luma(texCurr.SampleLevel(smp, p, 0).rgb) -
                           luma(texPrev.SampleLevel(smp, p + offUv, 0).rgb));
            }
            return sad;
        }
        float4 PSFlow(VSOut i) : SV_Target {
            float2 cuv = i.uv;
            float2 flowPx = float2(0, 0);
            float bestSad = 1e20;

            if (usePyramid != 0) {
                int levels = (fastMode != 0) ? 2 : 3;
                [loop] for (int lvl = 0; lvl < levels; lvl++) {
                    float stepPx = (lvl == 0) ? ((fastMode != 0) ? 6.0 : 8.0) : ((lvl == 1) ? ((fastMode != 0) ? 2.0 : 3.0) : 1.0);
                    float spreadPx = (lvl == 0) ? (ds * ((fastMode != 0) ? 1.25 : 2.0)) : ((lvl == 1) ? (float)ds : (ds * 0.5));
                    int rng = (fastMode != 0) ? 1 : ((lvl == 0) ? 2 : 1);
                    float bSad = 1e20;
                    float2 bO = flowPx;
                    [loop] for (int oy = -rng; oy <= rng; oy++)
                    [loop] for (int ox = -rng; ox <= rng; ox++) {
                        float2 candPx = flowPx + float2(ox, oy) * stepPx;
                        float sad = sad_at(cuv, candPx, spreadPx);
                        if (sad < bSad) { bSad = sad; bO = candPx; }
                    }
                    flowPx = bO;
                    bestSad = bSad;
                }
            } else {
                [loop] for (int oy = -8; oy <= 8; oy += 4)
                [loop] for (int ox = -8; ox <= 8; ox += 4) {
                    float2 candPx = float2(ox, oy);
                    float sad = sad_at(cuv, candPx, ds);
                    if (sad < bestSad) { bestSad = sad; flowPx = candPx; }
                }
            }
            float conf = saturate(1.0 - bestSad / 1.5);
            return float4(flowPx, conf, 1);
        }
        float4 PSFlowSmooth(VSOut i) : SV_Target {
            float2 ts = float2(1.0 / lowW, 1.0 / lowH);
            float4 acc = 0;
            [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                acc += flowTex.SampleLevel(smp, i.uv + float2(x, y) * ts, 0);
            return acc / 9.0;
        }
        float4 PSMain(VSOut i) : SV_Target {
            float4 f = flowTex.SampleLevel(smp, i.uv, 0);
            float2 ouv = f.xy * float2(invW, invH);
            float conf = saturate(f.z) * saturate(strength);
            float4 a = texPrev.SampleLevel(smp, i.uv + 0.5 * ouv, 0);
            float4 b = texCurr.SampleLevel(smp, i.uv - 0.5 * ouv, 0);
            float consist = saturate(1.0 - abs(luma(a.rgb) - luma(b.rgb)) * 4.0);
            float w = conf * consist;
            float4 pc = texPrev.SampleLevel(smp, i.uv, 0);
            float4 cc = texCurr.SampleLevel(smp, i.uv, 0);
            float4 plain = lerp(pc, cc, 0.5);
            float4 warped = lerp(a, b, 0.5);
            float4 outc = lerp(plain, warped, w);
            if (hudProtect != 0) {
                float3 d3 = abs(pc.rgb - cc.rgb);
                float chg = max(d3.r, max(d3.g, d3.b));
                float staticMask = saturate(1.0 - chg * 50.0);
                outc.rgb = lerp(outc.rgb, cc.rgb, staticMask);
            }
            return outc;
        }
    )HLSL";

    void log_debug(const char *fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        OutputDebugStringA("[reshade-dx11-fg] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }

    template <typename T> void safe_release(T *&p) { if (p) { p->Release(); p = nullptr; } }

    void release_resources()
    {
        safe_release(g_prev_srv); safe_release(g_prev_tex);
        safe_release(g_curr_srv); safe_release(g_curr_tex);
        safe_release(g_flow1_srv); safe_release(g_flow1_rtv); safe_release(g_flow1);
        safe_release(g_flow2_srv); safe_release(g_flow2_rtv); safe_release(g_flow2);
        g_have_prev = false;
        g_w = g_h = g_lw = g_lh = 0;
        g_fmt = DXGI_FORMAT_UNKNOWN;
    }

    void shutdown()
    {
        release_resources();
        safe_release(g_cb); safe_release(g_blend); safe_release(g_smp);
        safe_release(g_ps_interp); safe_release(g_ps_smooth); safe_release(g_ps_flow); safe_release(g_vs);
        safe_release(g_ctx); safe_release(g_dev);
        g_pipeline_ready = false;
    }

    bool compile_one(const char *entry, const char *target, ID3DBlob **out)
    {
        ID3DBlob *err = nullptr;
        HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, entry, target, 0, 0, out, &err);
        if (FAILED(hr)) {
            log_debug("compile %s failed hr=0x%08lX %s", entry, hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "");
            safe_release(err);
            return false;
        }
        safe_release(err);
        return true;
    }

    bool ensure_device(ID3D11Device *device)
    {
        if (g_pipeline_ready && g_dev == device)
            return true;

        shutdown();
        g_dev = device;
        g_dev->AddRef();
        g_dev->GetImmediateContext(&g_ctx);

        ID3DBlob *vs = nullptr, *pf = nullptr, *psm = nullptr, *pi = nullptr;
        if (!compile_one("VSMain", "vs_5_0", &vs)) { g_status = "VS compile failed"; safe_release(vs); return false; }
        if (!compile_one("PSFlow", "ps_5_0", &pf)) { g_status = "flow shader compile failed"; safe_release(vs); safe_release(pf); return false; }
        if (!compile_one("PSFlowSmooth", "ps_5_0", &psm)) { g_status = "smooth shader compile failed"; safe_release(vs); safe_release(pf); safe_release(psm); return false; }
        if (!compile_one("PSMain", "ps_5_0", &pi)) { g_status = "interp shader compile failed"; safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi); return false; }

        g_last_hr = g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
        if (FAILED(g_last_hr)) { g_status = "VS create failed"; safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi); return false; }
        g_last_hr = g_dev->CreatePixelShader(pf->GetBufferPointer(), pf->GetBufferSize(), nullptr, &g_ps_flow);
        if (FAILED(g_last_hr)) { g_status = "flow PS create failed"; safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi); return false; }
        g_last_hr = g_dev->CreatePixelShader(psm->GetBufferPointer(), psm->GetBufferSize(), nullptr, &g_ps_smooth);
        if (FAILED(g_last_hr)) { g_status = "smooth PS create failed"; safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi); return false; }
        g_last_hr = g_dev->CreatePixelShader(pi->GetBufferPointer(), pi->GetBufferSize(), nullptr, &g_ps_interp);
        if (FAILED(g_last_hr)) { g_status = "interp PS create failed"; safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi); return false; }
        safe_release(vs); safe_release(pf); safe_release(psm); safe_release(pi);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_last_hr = g_dev->CreateSamplerState(&sd, &g_smp);
        if (FAILED(g_last_hr)) { g_status = "sampler create failed"; return false; }

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_last_hr = g_dev->CreateBlendState(&bd, &g_blend);
        if (FAILED(g_last_hr)) { g_status = "blend create failed"; return false; }

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(FlowCB);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_last_hr = g_dev->CreateBuffer(&cbd, nullptr, &g_cb);
        if (FAILED(g_last_hr)) { g_status = "cbuffer create failed"; return false; }

        g_pipeline_ready = g_vs && g_ps_flow && g_ps_smooth && g_ps_interp && g_smp && g_blend && g_cb;
        if (g_pipeline_ready)
            log_debug("initialized DX11 pipeline");
        return g_pipeline_ready;
    }

    bool make_capture(const D3D11_TEXTURE2D_DESC &bb, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
    {
        D3D11_TEXTURE2D_DESC d = bb;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, tex);
        if (FAILED(hr)) { g_last_hr = hr; log_debug("CreateTexture2D capture failed hr=0x%08lX fmt=%u %ux%u", hr, d.Format, d.Width, d.Height); return false; }
        hr = g_dev->CreateShaderResourceView(*tex, nullptr, srv);
        if (FAILED(hr)) { g_last_hr = hr; log_debug("CreateSRV capture failed hr=0x%08lX fmt=%u", hr, d.Format); return false; }
        return true;
    }

    bool make_flow(ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv, ID3D11ShaderResourceView **srv)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = g_lw; d.Height = g_lh; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, tex);
        if (FAILED(hr)) { g_last_hr = hr; log_debug("CreateTexture2D flow failed hr=0x%08lX %ux%u", hr, d.Width, d.Height); return false; }
        hr = g_dev->CreateRenderTargetView(*tex, nullptr, rtv);
        if (FAILED(hr)) { g_last_hr = hr; log_debug("CreateRTV flow failed hr=0x%08lX", hr); return false; }
        hr = g_dev->CreateShaderResourceView(*tex, nullptr, srv);
        if (FAILED(hr)) { g_last_hr = hr; log_debug("CreateSRV flow failed hr=0x%08lX", hr); return false; }
        return true;
    }

    bool ensure_resources(ID3D11Texture2D *bb)
    {
        D3D11_TEXTURE2D_DESC d{};
        bb->GetDesc(&d);
        if (d.SampleDesc.Count != 1) { g_status = "MSAA backbuffer unsupported"; return false; }
        if (g_prev_tex && d.Width == g_w && d.Height == g_h && d.Format == g_fmt)
            return true;

        release_resources();
        g_w = d.Width; g_h = d.Height; g_fmt = d.Format;
        unsigned ds = static_cast<unsigned>(std::clamp(g_settings.flow_downscale, kMinDS, kMaxDS));
        g_lw = std::max(1u, (g_w + ds - 1) / ds);
        g_lh = std::max(1u, (g_h + ds - 1) / ds);

        if (!make_capture(d, &g_prev_tex, &g_prev_srv)) { g_status = "capture texture failed"; return false; }
        if (!make_capture(d, &g_curr_tex, &g_curr_srv)) { g_status = "capture texture failed"; return false; }
        if (!make_flow(&g_flow1, &g_flow1_rtv, &g_flow1_srv)) { g_status = "flow texture failed"; return false; }
        if (!make_flow(&g_flow2, &g_flow2_rtv, &g_flow2_srv)) { g_status = "flow texture failed"; return false; }

        log_debug("resources %ux%u flow=%ux%u ds=%u", g_w, g_h, g_lw, g_lh, ds);
        return true;
    }

    struct StateBlock
    {
        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11DepthStencilView *dsv = nullptr;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT vpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        ID3D11InputLayout *il = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11ShaderResourceView *srv[4] = {};
        ID3D11SamplerState *smp = nullptr;
        ID3D11BlendState *blend = nullptr;
        FLOAT blendFactor[4] = {};
        UINT sampleMask = 0;
        ID3D11Buffer *cb = nullptr;
    };

    void save(StateBlock &s)
    {
        g_ctx->OMGetRenderTargets(1, &s.rtv, &s.dsv);
        g_ctx->RSGetViewports(&s.vpN, s.vp);
        g_ctx->VSGetShader(&s.vs, nullptr, nullptr);
        g_ctx->PSGetShader(&s.ps, nullptr, nullptr);
        g_ctx->IAGetInputLayout(&s.il);
        g_ctx->IAGetPrimitiveTopology(&s.topo);
        g_ctx->PSGetShaderResources(0, 4, s.srv);
        g_ctx->PSGetSamplers(0, 1, &s.smp);
        g_ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
        g_ctx->PSGetConstantBuffers(0, 1, &s.cb);
    }

    void restore(StateBlock &s)
    {
        g_ctx->OMSetRenderTargets(1, &s.rtv, s.dsv);
        if (s.vpN) g_ctx->RSSetViewports(s.vpN, s.vp);
        g_ctx->VSSetShader(s.vs, nullptr, 0);
        g_ctx->PSSetShader(s.ps, nullptr, 0);
        g_ctx->IASetInputLayout(s.il);
        g_ctx->IASetPrimitiveTopology(s.topo);
        g_ctx->PSSetShaderResources(0, 4, s.srv);
        g_ctx->PSSetSamplers(0, 1, &s.smp);
        g_ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
        g_ctx->PSSetConstantBuffers(0, 1, &s.cb);
        safe_release(s.rtv); safe_release(s.dsv); safe_release(s.vs); safe_release(s.ps); safe_release(s.il);
        for (auto &v : s.srv) safe_release(v);
        safe_release(s.smp); safe_release(s.blend); safe_release(s.cb);
    }

    void pass(ID3D11RenderTargetView *rtv, float w, float h, ID3D11PixelShader *ps,
              ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b, ID3D11ShaderResourceView *c)
    {
        ID3D11RenderTargetView *nullrtv = nullptr;
        g_ctx->OMSetRenderTargets(1, &nullrtv, nullptr);
        ID3D11ShaderResourceView *srvs[3] = { a, b, c };
        g_ctx->PSSetShaderResources(0, 3, srvs);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width = w; vp.Height = h; vp.MaxDepth = 1.0f;
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->PSSetShader(ps, nullptr, 0);
        g_ctx->Draw(3, 0);
    }

    bool draw_interpolated(ID3D11Texture2D *backbuffer)
    {
        g_draw_attempts.fetch_add(1, std::memory_order_relaxed);
        ID3D11RenderTargetView *bbrtv = nullptr;
        HRESULT rt_hr = g_dev->CreateRenderTargetView(backbuffer, nullptr, &bbrtv);
        if (FAILED(rt_hr) || !bbrtv) { g_last_hr = rt_hr; g_status = "backbuffer RTV failed"; log_debug("CreateRTV backbuffer failed hr=0x%08lX", rt_hr); return false; }

        StateBlock s;
        save(s);
        const FLOAT blendFactor[4] = {0, 0, 0, 0};
        g_ctx->OMSetBlendState(g_blend, blendFactor, 0xffffffff);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetSamplers(0, 1, &g_smp);

        FlowCB cb{};
        cb.W = g_w; cb.H = g_h; cb.lowW = g_lw; cb.lowH = g_lh;
        cb.invW = 1.0f / static_cast<float>(g_w);
        cb.invH = 1.0f / static_cast<float>(g_h);
        cb.searchR = g_settings.fast_search ? 8 : 12; cb.searchS = g_settings.fast_search ? 4 : 2; cb.patchP = 1; cb.ds = std::clamp(g_settings.flow_downscale, kMinDS, kMaxDS);
        cb.usePyramid = g_settings.pyramid ? 1 : 0;
        cb.smoothFlow = g_settings.smooth ? 1 : 0;
        cb.hudProtect = g_settings.hud_protect ? 1 : 0;
        cb.fastMode = g_settings.fast_mode ? 1 : 0;
        cb.strength = g_settings.strength;
        g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);

        ID3D11ShaderResourceView *nullsrv = nullptr;
        if (g_settings.active_preview) {
            pass(g_flow1_rtv, static_cast<float>(g_lw), static_cast<float>(g_lh), g_ps_flow, g_prev_srv, g_curr_srv, nullsrv);
            if (g_settings.smooth)
                pass(g_flow2_rtv, static_cast<float>(g_lw), static_cast<float>(g_lh), g_ps_smooth, nullsrv, nullsrv, g_flow1_srv);
            pass(bbrtv, static_cast<float>(g_w), static_cast<float>(g_h), g_ps_interp,
                 g_prev_srv, g_curr_srv, g_settings.smooth ? g_flow2_srv : g_flow1_srv);
        } else {
            pass(bbrtv, static_cast<float>(g_w), static_cast<float>(g_h), g_ps_interp,
                 g_prev_srv, g_curr_srv, g_flow1_srv);
        }

        ID3D11ShaderResourceView *nulls[4] = { nullptr, nullptr, nullptr, nullptr };
        g_ctx->PSSetShaderResources(0, 4, nulls);
        restore(s);
        bbrtv->Release();
        g_draw_success.fetch_add(1, std::memory_order_relaxed);
        g_status = g_settings.extra_present ? "generated present submitted" : "preview drawn";
        return true;
    }

    bool present_generated_frame(reshade::api::effect_runtime *runtime, ID3D11Texture2D *backbuffer)
    {
        if (g_inside_extra_present)
            return false;

        IDXGISwapChain *swap = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
        if (!swap)
            return false;

        // Draw the generated frame into the current backbuffer and present it immediately.
        // Then restore the real current frame into the next backbuffer so ReShade/the game can
        // continue with the normal Present. This is intentionally experimental and disabled by default.
        if (!draw_interpolated(backbuffer))
            return false;

        g_inside_extra_present = true;
        HRESULT phr = swap->Present(0, 0);
        g_inside_extra_present = false;
        if (FAILED(phr)) {
            log_debug("extra Present failed hr=0x%08lX", phr);
            return false;
        }

        ID3D11Texture2D *restore_bb = nullptr;
        IDXGISwapChain3 *swap3 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&swap3))) && swap3) {
            UINT idx = swap3->GetCurrentBackBufferIndex();
            swap3->GetBuffer(idx, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&restore_bb));
            swap3->Release();
        }
        if (!restore_bb) {
            // Fallback for older swapchains: buffer zero is often the current backbuffer in blt-model.
            swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&restore_bb));
        }
        if (restore_bb) {
            g_ctx->CopyResource(restore_bb, g_curr_tex);
            restore_bb->Release();
        }
        g_extra_presents.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void run(reshade::api::effect_runtime *runtime)
    {
        if (g_inside_extra_present)
            return;
        const unsigned long long frame_index = g_real_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!g_settings.enabled) {
            g_status = "disabled";
            g_have_prev = false;
            return;
        }

        reshade::api::device *api_device = runtime->get_device();
        if (api_device == nullptr || api_device->get_api() != reshade::api::device_api::d3d11) { g_status = "not d3d11"; return; }

        ID3D11Device *dev = reinterpret_cast<ID3D11Device *>(api_device->get_native());
        if (!dev) { g_status = "no d3d11 device"; return; }
        if (!ensure_device(dev)) { if (!g_status || std::strcmp(g_status, "idle") == 0) g_status = "pipeline failed"; return; }

        reshade::api::resource backbuffer_resource = runtime->get_current_back_buffer();
        ID3D11Texture2D *bb = reinterpret_cast<ID3D11Texture2D *>(backbuffer_resource.handle);
        if (!bb || !ensure_resources(bb)) { if (!bb) g_status = "no backbuffer"; return; }

        g_ctx->CopyResource(g_curr_tex, bb);
        if (g_have_prev) {
            if (g_settings.extra_present) {
                int div = std::max(1, g_settings.present_interval);
                if ((frame_index % static_cast<unsigned long long>(div)) == 0) {
                    if (present_generated_frame(runtime, bb))
                        g_gen_frames.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                int div = std::max(1, g_settings.preview_divisor);
                if ((frame_index % static_cast<unsigned long long>(div)) == 0) {
                    if (draw_interpolated(bb))
                        g_gen_frames.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        else { g_status = "history warming up"; }
        std::swap(g_prev_tex, g_curr_tex);
        std::swap(g_prev_srv, g_curr_srv);
        g_have_prev = true;
    }
}

static void draw_settings_overlay(reshade::api::effect_runtime *)
{
    ImGui::Checkbox("Enable framegen", &fg::g_settings.enabled);
    ImGui::Checkbox("Active preview render", &fg::g_settings.active_preview);
    ImGui::Checkbox("Experimental extra Present", &fg::g_settings.extra_present);
    ImGui::SliderInt("Extra Present interval", &fg::g_settings.present_interval, 1, 4);
    ImGui::Checkbox("Pyramid optical flow", &fg::g_settings.pyramid);
    ImGui::Checkbox("Fast mode", &fg::g_settings.fast_mode);
    ImGui::Checkbox("Fast search", &fg::g_settings.fast_search);
    ImGui::Checkbox("Smooth flow", &fg::g_settings.smooth);
    ImGui::Checkbox("HUD/static protection", &fg::g_settings.hud_protect);
    bool changed_ds = ImGui::SliderInt("Flow downscale", &fg::g_settings.flow_downscale, 8, 32);
    ImGui::SliderInt("Preview every N frames", &fg::g_settings.preview_divisor, 1, 4);
    ImGui::SliderFloat("Strength", &fg::g_settings.strength, 0.0f, 1.0f);
    if (changed_ds) fg::release_resources();
    ImGui::Checkbox("Debug overlay", &fg::g_settings.debug_overlay);
    ImGui::Text("Status: %s", fg::g_status);
    ImGui::Text("Last HR: 0x%08lX", fg::g_last_hr);
    ImGui::Text("Real frames: %llu", fg::g_real_frames.load());
    ImGui::Text("Interpolated frames: %llu", fg::g_gen_frames.load());
    ImGui::Text("Extra presents: %llu", fg::g_extra_presents.load());
    ImGui::Text("Draw attempts/success: %llu / %llu", fg::g_draw_attempts.load(), fg::g_draw_success.load());
    ImGui::Text("Flow grid: %ux%u", fg::g_lw, fg::g_lh);
    ImGui::TextDisabled("Preview mode writes into the current backbuffer. Extra Present attempts real generated-frame presentation and is experimental.");
}

static void draw_osd_overlay(reshade::api::effect_runtime *)
{
    if (!fg::g_settings.debug_overlay)
        return;
    ImGui::Text("DX11 FG: %s %s", fg::g_settings.enabled ? "ON" : "OFF", fg::g_settings.extra_present ? "GEN" : "PREVIEW");
    ImGui::Text("Real %llu / Interp %llu / Presents %llu", fg::g_real_frames.load(), fg::g_gen_frames.load(), fg::g_extra_presents.load());
    ImGui::Text("Status: %s", fg::g_status);
}

static void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    fg::run(runtime);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        if (!reshade::register_addon(hinstDLL))
            return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_overlay(nullptr, &draw_settings_overlay);
        reshade::register_overlay("OSD", &draw_osd_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        fg::shutdown();
        reshade::unregister_addon(hinstDLL);
        break;
    }
    return TRUE;
}
