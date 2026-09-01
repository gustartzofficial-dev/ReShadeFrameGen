// ReShadeFrameGen Hybrid Motion extension
//
// This file deliberately compiles the existing implementation unchanged, then layers an
// optional texMotionVectors-backed flow/interpolation path on top. The original source remains
// in the repository and remains the default/fallback backend.

#define DllMain ReShadeFrameGen_OriginalDllMain
#include "reshade_dx11_framegen.cpp"
#undef DllMain

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <thread>
#include <condition_variable>
#include <deque>
#include <chrono>

namespace fgx
{
    enum MotionBackend : int
    {
        original = 0,
        automatic_hybrid = 1,
        external_only = 2,
        hybrid_refine = 3,
    };

    struct Settings
    {
        int motion_backend = original;
        float motion_scale_x = 1.0f;
        float motion_scale_y = 1.0f;
        float residual_radius_px = 2.0f;
        float external_confidence = 0.95f;
        float occlusion_strength = 1.15f;
        float depth_rejection_strength = 1.0f;
        bool enhanced_disocclusion = true;
        // Output path is deliberately separate from motion quality.
        // 0 = original in-swapchain pacing, 1 = legacy immediate extra-Present,
        // 2 = independent presenter (real additive output, DX11 windowed/borderless).
        int output_mode = 0;
        bool presenter_vsync = true;
        bool presenter_hide_unfocused = true;
        bool show_status = true;
    };

    Settings g_settings;

    // b1: the original add-on owns b0 (FlowCB), so this extension never changes that contract.
    struct HybridCB
    {
        float motionScaleX;
        float motionScaleY;
        float residualRadiusPx;
        float externalConfidence;
        float occlusionStrength;
        float depthRejectionStrength;
        int motionMode;              // 1 = external only, 2 = external seed + optical residual
        int enhancedDisocclusion;
    };
    static_assert((sizeof(HybridCB) % 16) == 0, "HybridCB must be 16-byte aligned");

    ID3D11PixelShader *g_ps_flow_hybrid = nullptr;
    ID3D11PixelShader *g_ps_interp_hybrid = nullptr;
    ID3D11Buffer *g_hybrid_cb = nullptr;
    ID3D11Device *g_extension_device = nullptr; // borrowed identity only

    ID3D11ShaderResourceView *g_external_mv_srv = nullptr;
    bool g_external_mv_found = false;
    char g_external_mv_effect[128] = {};
    const char *g_extension_status = "Original backend";

    const char *kHybridShader = R"HLSL(
        cbuffer FlowCB : register(b0) {
            uint W,H,lowW,lowH;
            float invW,invH;
            int searchR,searchS;
            int patchP,ds;
            int usePyramid,smoothFlow;
            int hudProtect,fastMode;
            float strength,phase;
            int useDepth;
            float aaBlend;
            int useEdgeFlow;
            float accumFeedback;
        };
        cbuffer HybridCB : register(b1) {
            float motionScaleX;
            float motionScaleY;
            float residualRadiusPx;
            float externalConfidence;
            float occlusionStrength;
            float depthRejectionStrength;
            int motionMode;
            int enhancedDisocclusion;
        };

        Texture2D texPrev : register(t0);
        Texture2D texCurr : register(t1);
        Texture2D flowTex : register(t2);
        Texture2D depthTex : register(t3);
        Texture2D texMotionVectors : register(t4);
        SamplerState smp : register(s0);

        struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
        float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

        float sad_at(float2 cuv, float2 candPx, float spreadPx)
        {
            float2 offUv = candPx * float2(invW, invH);
            float sad = 0.0;
            [unroll] for (int py = -1; py <= 1; ++py)
            [unroll] for (int px = -1; px <= 1; ++px) {
                float2 p = cuv + float2(px, py) * float2(invW, invH) * spreadPx;
                float a = luma(texCurr.SampleLevel(smp, p, 0).rgb);
                float b = luma(texPrev.SampleLevel(smp, p + offUv, 0).rgb);
                sad += abs(a - b);
            }
            return sad;
        }

        // Community texMotionVectors (qUINT / ReShadeMotionEstimation) is UV-space motion from
        // current-frame coordinates toward the matching previous-frame coordinates. Convert it
        // to the pixel-space contract used by the existing ReShadeFrameGen flow texture.
        float2 external_seed_px(float2 uv)
        {
            float2 mv = texMotionVectors.SampleLevel(smp, uv, 0).rg;
            mv *= float2(motionScaleX, motionScaleY);
            return mv * float2((float)W, (float)H);
        }

        float4 PSFlowHybrid(VSOut i) : SV_Target
        {
            float2 seed = external_seed_px(i.uv);
            float spreadPx = max(1.0, (float)ds * 0.70);
            float bestSad = sad_at(i.uv, seed, spreadPx);
            float2 best = seed;

            // Hybrid mode keeps the external dense MV as the prediction, then searches only a
            // small residual neighborhood. This preserves the game's/provider's large motion
            // while letting the original photometric signal correct local errors.
            if (motionMode == 2 && residualRadiusPx > 0.01) {
                float stepPx = residualRadiusPx * 0.5;
                [loop] for (int oy = -2; oy <= 2; ++oy)
                [loop] for (int ox = -2; ox <= 2; ++ox) {
                    float2 cand = seed + float2(ox, oy) * stepPx;
                    float sad = sad_at(i.uv, cand, spreadPx);
                    if (sad < bestSad) { bestSad = sad; best = cand; }
                }
            }

            float matchConf = saturate(1.0 - bestSad / 1.5);
            float l0 = luma(texCurr.SampleLevel(smp, i.uv, 0).rgb);
            float lx = luma(texCurr.SampleLevel(smp, i.uv + float2(invW, 0) * ds, 0).rgb);
            float ly = luma(texCurr.SampleLevel(smp, i.uv + float2(0, invH) * ds, 0).rgb);
            float structure = saturate((abs(lx - l0) + abs(ly - l0)) * 8.0);

            // External vectors are still useful in flatter regions where block matching has an
            // aperture problem, so retain a floor of provider confidence instead of zeroing it.
            float conf = matchConf * lerp(0.35, 1.0, structure) * saturate(externalConfidence);
            return float4(best, saturate(conf), 1.0);
        }

        float4 fetchFlow(float2 uv)
        {
            float2 lsz = float2((float)lowW, (float)lowH);
            float2 p = uv * lsz - 0.5;
            float2 base = floor(p);
            float2 fracp = frac(p);
            float2 centerColorUV = uv;
            float centerL = luma(texCurr.SampleLevel(smp, centerColorUV, 0).rgb);
            float4 accum = 0.0;
            float wsum = 0.0;

            [unroll] for (int yy = 0; yy <= 1; ++yy)
            [unroll] for (int xx = 0; xx <= 1; ++xx) {
                float2 q = base + float2(xx, yy);
                float2 quv = (q + 0.5) / lsz;
                float bil = (xx == 0 ? (1.0 - fracp.x) : fracp.x) *
                            (yy == 0 ? (1.0 - fracp.y) : fracp.y);
                float edgeW = 1.0;
                if (useEdgeFlow != 0) {
                    float nl = luma(texCurr.SampleLevel(smp, quv, 0).rgb);
                    edgeW = exp(-abs(nl - centerL) * 24.0);
                }
                float w = bil * edgeW + 1e-5;
                accum += flowTex.SampleLevel(smp, quv, 0) * w;
                wsum += w;
            }
            return accum / max(wsum, 1e-5);
        }

        float4 PSInterpHybrid(VSOut i) : SV_Target
        {
            float t = saturate(phase);
            float4 f = fetchFlow(i.uv);
            float2 ouv = f.xy * float2(invW, invH);
            float conf = saturate(f.z) * saturate(strength);

            float2 uvA = saturate(i.uv + t * ouv);
            float2 uvB = saturate(i.uv - (1.0 - t) * ouv);
            float4 a = texPrev.SampleLevel(smp, uvA, 0);
            float4 b = texCurr.SampleLevel(smp, uvB, 0);
            float4 pc = texPrev.SampleLevel(smp, i.uv, 0);
            float4 cc = texCurr.SampleLevel(smp, i.uv, 0);
            float4 plain = lerp(pc, cc, t);
            float4 warped = lerp(a, b, t);
            float4 nearest = (t < 0.5) ? pc : cc;

            float disagree = abs(luma(a.rgb) - luma(b.rgb));
            float consist = saturate(1.0 - disagree * 4.0);
            float occl = saturate(disagree * 6.0 * max(occlusionStrength, 0.0));

            if (enhancedDisocclusion != 0) {
                // Motion-field self-consistency: if nearby samples along the warp path disagree
                // strongly, we are likely crossing an occlusion/disocclusion boundary.
                float2 fa = fetchFlow(uvA).xy;
                float2 fb = fetchFlow(uvB).xy;
                float fieldMismatch = length(fa - fb) / max(2.0, length(f.xy) + 2.0);
                float fieldGuard = saturate(fieldMismatch * 2.25 * max(occlusionStrength, 0.0));
                occl = max(occl, fieldGuard);
                consist *= (1.0 - saturate(fieldMismatch * 1.5));
            }

            float depthGuard = 0.0;
            if (useDepth != 0) {
                float dc = depthTex.SampleLevel(smp, i.uv, 0).r;
                float dxv = abs(depthTex.SampleLevel(smp, i.uv + float2(invW, 0), 0).r - dc);
                float dyv = abs(depthTex.SampleLevel(smp, i.uv + float2(0, invH), 0).r - dc);
                float dPath = abs(depthTex.SampleLevel(smp, uvB, 0).r - dc);
                depthGuard = saturate(((dxv + dyv) * 40.0 + dPath * 18.0) * max(depthRejectionStrength, 0.0));
                occl = max(occl, depthGuard);
            }

            // Prefer a real frame at uncertain geometry instead of blending two contradictory
            // warps. This is deliberately conservative: it trades a little interpolation for
            // less silhouette smearing/ghosting.
            warped = lerp(warped, nearest, occl);
            float w = conf * consist * (1.0 - depthGuard * 0.65);
            float4 outc = lerp(plain, warped, saturate(w));

            if (hudProtect != 0) {
                float3 d3 = abs(pc.rgb - cc.rgb);
                float chg = max(d3.r, max(d3.g, d3.b));
                float staticMask = saturate(1.0 - chg * 50.0);
                outc.rgb = lerp(outc.rgb, cc.rgb, staticMask);
            }
            return outc;
        }
    )HLSL";

    template <typename T>
    void release(T *&p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    void release_external_view(bool clear_status = false)
    {
        release(g_external_mv_srv);
        if (clear_status) {
            g_external_mv_found = false;
            g_external_mv_effect[0] = '\0';
        }
    }

    void release_extension_pipeline()
    {
        release(g_ps_flow_hybrid);
        release(g_ps_interp_hybrid);
        release(g_hybrid_cb);
        g_extension_device = nullptr;
    }

    bool compile_ps(const char *entry, ID3D11PixelShader **out)
    {
        ID3DBlob *blob = nullptr;
        ID3DBlob *errors = nullptr;
        HRESULT hr = D3DCompile(kHybridShader, std::strlen(kHybridShader), "ReShadeFrameGen HybridMV",
                                nullptr, nullptr, entry, "ps_5_0", 0, 0, &blob, &errors);
        if (FAILED(hr) || !blob) {
            if (errors)
                fg::log_debug("hybrid compile %s failed hr=0x%08lX %s", entry, hr,
                              static_cast<const char *>(errors->GetBufferPointer()));
            release(errors);
            release(blob);
            return false;
        }
        hr = fg::g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
        release(errors);
        release(blob);
        if (FAILED(hr)) {
            fg::log_debug("hybrid CreatePixelShader %s failed hr=0x%08lX", entry, hr);
            return false;
        }
        return true;
    }

    bool ensure_extension_pipeline()
    {
        if (!fg::g_pipeline_ready || !fg::g_dev)
            return false;
        if (g_extension_device == fg::g_dev && g_ps_flow_hybrid && g_ps_interp_hybrid && g_hybrid_cb)
            return true;

        release_extension_pipeline();
        g_extension_device = fg::g_dev;

        if (!compile_ps("PSFlowHybrid", &g_ps_flow_hybrid) ||
            !compile_ps("PSInterpHybrid", &g_ps_interp_hybrid)) {
            release_extension_pipeline();
            g_extension_status = "Hybrid shader compile failed - Original fallback";
            return false;
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(HybridCB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hr = fg::g_dev->CreateBuffer(&bd, nullptr, &g_hybrid_cb);
        if (FAILED(hr) || !g_hybrid_cb) {
            release_extension_pipeline();
            g_extension_status = "Hybrid constant buffer failed - Original fallback";
            return false;
        }
        return true;
    }

    bool refresh_external_motion(reshade::api::effect_runtime *runtime)
    {
        release_external_view();
        g_external_mv_found = false;
        g_external_mv_effect[0] = '\0';

        // The extension is D3D11-first. The original add-on's D3D11On12 route remains unchanged
        // and is used as the fallback on DX12 rather than trying to reinterpret a D3D12 descriptor.
        reshade::api::device *api_dev = runtime ? runtime->get_device() : nullptr;
        if (!api_dev || api_dev->get_api() != reshade::api::device_api::d3d11)
            return false;

        reshade::api::effect_texture_variable var = runtime->find_texture_variable(nullptr, "texMotionVectors");
        if (var.handle == 0)
            return false;

        reshade::api::resource_view srv = {};
        reshade::api::resource_view srv_srgb = {};
        runtime->get_texture_binding(var, &srv, &srv_srgb);
        if (srv.handle == 0)
            return false;

        ID3D11ShaderResourceView *native = reinterpret_cast<ID3D11ShaderResourceView *>(static_cast<uintptr_t>(srv.handle));
        if (!native)
            return false;

        native->AddRef();
        g_external_mv_srv = native;
        g_external_mv_found = true;
        runtime->get_texture_variable_effect_name(var, g_external_mv_effect);
        return true;
    }

    bool wants_external_backend()
    {
        return g_settings.motion_backend != original;
    }

    int resolved_motion_mode()
    {
        if (!g_external_mv_found)
            return 0;
        if (g_settings.motion_backend == external_only)
            return 1;
        if (g_settings.motion_backend == automatic_hybrid || g_settings.motion_backend == hybrid_refine)
            return 2;
        return 0;
    }

    void update_extension_cb(int motion_mode)
    {
        HybridCB cb{};
        cb.motionScaleX = g_settings.motion_scale_x;
        cb.motionScaleY = g_settings.motion_scale_y;
        cb.residualRadiusPx = std::clamp(g_settings.residual_radius_px, 0.0f, 8.0f);
        cb.externalConfidence = std::clamp(g_settings.external_confidence, 0.0f, 1.0f);
        cb.occlusionStrength = std::clamp(g_settings.occlusion_strength, 0.0f, 3.0f);
        cb.depthRejectionStrength = std::clamp(g_settings.depth_rejection_strength, 0.0f, 3.0f);
        cb.motionMode = motion_mode;
        cb.enhancedDisocclusion = g_settings.enhanced_disocclusion ? 1 : 0;
        fg::g_ctx->UpdateSubresource(g_hybrid_cb, 0, nullptr, &cb, 0, 0);
    }

    void run_original_with_output_mode(reshade::api::effect_runtime *runtime);

    // --------------------------------------------------------------------------------------
    // Independent additive presenter
    //
    // Why this exists: Present() calls injected into the game's own swapchain necessarily share
    // the game's backbuffer cadence. Spacing those presents stalls the game thread; not spacing
    // them makes generated + real frames land back-to-back. DLSS-G/FSR solve this with a proxy
    // presentation path. ReShade add-ons cannot replace ReShade's DXGI proxy after the game has
    // already created it, so this experimental DX11 path uses a second, click-through owned
    // presentation window/swapchain. The game keeps its own Present cadence untouched underneath;
    // this presenter displays captured real + generated frames at the monitor cadence above it.
    //
    // It is intentionally x2-only for the first test build. This keeps the queue bounded and makes
    // the expected behavior unambiguous: 60 real -> ~120 presented on a >=120 Hz display, 30 -> ~60.
    // --------------------------------------------------------------------------------------
    struct PresenterSlot
    {
        ID3D11Texture2D *generated = nullptr;
        ID3D11Texture2D *real = nullptr;
        bool ready = false;
        bool in_use = false;
        bool warmup_only = false;
        unsigned long long serial = 0;
    };

    HINSTANCE g_module_instance = nullptr;
    HWND g_presenter_game_hwnd = nullptr;
    HWND g_presenter_hwnd = nullptr;
    IDXGISwapChain1 *g_presenter_swap = nullptr;
    IDXGISwapChain3 *g_presenter_swap3 = nullptr;
    ID3D11Device *g_presenter_device = nullptr; // borrowed identity; detects device recreation
    ID3D11Multithread *g_presenter_mt = nullptr;
    BOOL g_presenter_prev_mt = FALSE;
    HANDLE g_presenter_latency_waitable = nullptr; // owned by DXGI; do not CloseHandle
    std::thread g_presenter_thread;
    std::mutex g_presenter_mutex;
    std::condition_variable g_presenter_cv;
    PresenterSlot g_presenter_slots[3] = {};
    std::deque<int> g_presenter_ready;
    bool g_presenter_stop = false;
    bool g_presenter_running = false;
    bool g_reshade_overlay_open = false;
    UINT g_presenter_width = 0, g_presenter_height = 0;
    DXGI_FORMAT g_presenter_format = DXGI_FORMAT_UNKNOWN;
    std::atomic<unsigned long long> g_presenter_real_presents{0};
    std::atomic<unsigned long long> g_presenter_generated_presents{0};
    std::atomic<unsigned long long> g_presenter_dropped_packets{0};
    std::atomic<float> g_presenter_output_fps{0.0f};
    double g_presenter_last_present_time = 0.0; // presenter thread only
    const char *g_presenter_status = "off";

    LRESULT CALLBACK presenter_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; // never steal mouse input from the game
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void presenter_forget_cached_rtv(ID3D11Texture2D *tex)
    {
        if (!tex) return;
        for (int i = 0; i < fg::g_bbrtv_count; ++i) {
            if (fg::g_bbrtv_cache[i].tex != tex)
                continue;
            fg::safe_release(fg::g_bbrtv_cache[i].rtv);
            for (int j = i + 1; j < fg::g_bbrtv_count; ++j)
                fg::g_bbrtv_cache[j - 1] = fg::g_bbrtv_cache[j];
            --fg::g_bbrtv_count;
            fg::g_bbrtv_cache[fg::g_bbrtv_count] = {};
            --i;
        }
    }

    void presenter_release_slot(PresenterSlot &s)
    {
        presenter_forget_cached_rtv(s.generated);
        presenter_forget_cached_rtv(s.real);
        release(s.generated);
        release(s.real);
        s.ready = s.in_use = s.warmup_only = false;
        s.serial = 0;
    }

    void presenter_release_resources()
    {
        for (auto &s : g_presenter_slots)
            presenter_release_slot(s);
        g_presenter_ready.clear();
        release(g_presenter_swap3);
        release(g_presenter_swap);
        g_presenter_device = nullptr;
        g_presenter_latency_waitable = nullptr;
        g_presenter_width = g_presenter_height = 0;
        g_presenter_format = DXGI_FORMAT_UNKNOWN;
        g_presenter_last_present_time = 0.0;
        g_presenter_output_fps.store(0.0f, std::memory_order_relaxed);
    }

    void presenter_hide_window()
    {
        if (g_presenter_hwnd)
            ShowWindow(g_presenter_hwnd, SW_HIDE);
    }

    void presenter_destroy_window()
    {
        if (g_presenter_hwnd) {
            DestroyWindow(g_presenter_hwnd);
            g_presenter_hwnd = nullptr;
        }
        g_presenter_game_hwnd = nullptr;
    }

    bool presenter_supported_format(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
        }
    }

    bool presenter_create_window(HWND game_hwnd)
    {
        if (!game_hwnd)
            return false;
        if (g_presenter_hwnd && g_presenter_game_hwnd == game_hwnd)
            return true;

        presenter_destroy_window();

        static const wchar_t *klass = L"ReShadeFrameGenIndependentPresenter";
        static bool class_registered = false;
        if (!class_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = presenter_wndproc;
            wc.hInstance = g_module_instance ? g_module_instance : GetModuleHandleW(nullptr);
            wc.lpszClassName = klass;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            class_registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        if (!class_registered)
            return false;

        // WS_POPUP with game_hwnd as owner keeps this surface above the game without making it
        // system-topmost. NOACTIVATE + HTTRANSPARENT keep keyboard/mouse focus on the game.
        g_presenter_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            klass, L"ReShade FrameGen Presenter", WS_POPUP,
            0, 0, 16, 16, game_hwnd, nullptr,
            g_module_instance ? g_module_instance : GetModuleHandleW(nullptr), nullptr);
        if (!g_presenter_hwnd)
            return false;
        g_presenter_game_hwnd = game_hwnd;
        return true;
    }

    bool presenter_sync_window()
    {
        if (!g_presenter_hwnd || !g_presenter_game_hwnd)
            return false;

        bool focused = true;
        if (g_settings.presenter_hide_unfocused) {
            HWND fgwin = GetForegroundWindow();
            focused = fgwin == g_presenter_game_hwnd || fgwin == g_presenter_hwnd ||
                      IsChild(g_presenter_game_hwnd, fgwin) || IsChild(fgwin, g_presenter_game_hwnd);
        }
        if (g_reshade_overlay_open || IsIconic(g_presenter_game_hwnd) || !IsWindowVisible(g_presenter_game_hwnd) || !focused) {
            presenter_hide_window();
            return false;
        }

        RECT r{};
        if (!GetClientRect(g_presenter_game_hwnd, &r))
            return false;
        POINT p{0, 0};
        if (!ClientToScreen(g_presenter_game_hwnd, &p))
            return false;
        int w = std::max(1L, r.right - r.left);
        int h = std::max(1L, r.bottom - r.top);
        SetWindowPos(g_presenter_hwnd, HWND_TOP, p.x, p.y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        return true;
    }

    bool presenter_make_texture(UINT w, UINT h, DXGI_FORMAT fmt, ID3D11Texture2D **out)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = fg::g_dev->CreateTexture2D(&d, nullptr, out);
        if (FAILED(hr)) {
            fg::g_last_hr = hr;
            return false;
        }
        return true;
    }

    bool presenter_create_swapchain_and_slots(HWND game_hwnd, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (!fg::g_dev || !presenter_supported_format(fmt)) {
            g_presenter_status = "unsupported backbuffer format";
            return false;
        }
        if (!presenter_create_window(game_hwnd)) {
            g_presenter_status = "presenter window creation failed";
            return false;
        }

        presenter_release_resources();

        IDXGIDevice *dxgi_dev = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIFactory2 *factory = nullptr;
        HRESULT hr = fg::g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev));
        if (SUCCEEDED(hr) && dxgi_dev)
            hr = dxgi_dev->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
        if (FAILED(hr) || !factory) {
            release(factory); release(adapter); release(dxgi_dev);
            g_presenter_status = "DXGI factory unavailable";
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w; sd.Height = h;
        sd.Format = fmt;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        hr = factory->CreateSwapChainForHwnd(fg::g_dev, g_presenter_hwnd, &sd, nullptr, nullptr, &g_presenter_swap);
        factory->MakeWindowAssociation(g_presenter_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        release(factory); release(adapter); release(dxgi_dev);
        if (FAILED(hr) || !g_presenter_swap) {
            fg::g_last_hr = hr;
            g_presenter_status = "presenter swapchain creation failed";
            presenter_release_resources();
            return false;
        }

        IDXGISwapChain2 *swap2 = nullptr;
        if (SUCCEEDED(g_presenter_swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&swap2))) && swap2) {
            swap2->SetMaximumFrameLatency(1);
            g_presenter_latency_waitable = swap2->GetFrameLatencyWaitableObject();
            swap2->Release();
        }
        if (SUCCEEDED(g_presenter_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&g_presenter_swap3))) && g_presenter_swap3) {
            // Mirror the obvious color-space choice for common SDR/HDR backbuffer formats.
            // If the driver rejects it, presentation still continues using the swapchain default.
            if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            else
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }

        for (auto &slot : g_presenter_slots) {
            if (!presenter_make_texture(w, h, fmt, &slot.generated) ||
                !presenter_make_texture(w, h, fmt, &slot.real)) {
                g_presenter_status = "presenter queue texture creation failed";
                presenter_release_resources();
                return false;
            }
        }

        // The game and presenter share the D3D11 immediate context. Ask the runtime to serialize
        // context calls across both threads. This costs some CPU overhead but avoids racing D3D11
        // state while the presenter copies to its own swapchain.
        if (!g_presenter_mt && fg::g_ctx && SUCCEEDED(fg::g_ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&g_presenter_mt))) && g_presenter_mt) {
            g_presenter_prev_mt = g_presenter_mt->GetMultithreadProtected();
            g_presenter_mt->SetMultithreadProtected(TRUE);
        }

        g_presenter_device = fg::g_dev;
        g_presenter_width = w;
        g_presenter_height = h;
        g_presenter_format = fmt;
        g_presenter_status = "ready";
        presenter_sync_window();
        return true;
    }

    bool presenter_copy_and_present(ID3D11Texture2D *src, bool generated)
    {
        if (!src || !g_presenter_swap || !fg::g_ctx)
            return false;
        ID3D11Texture2D *bb = nullptr;
        const UINT back_index = g_presenter_swap3 ? g_presenter_swap3->GetCurrentBackBufferIndex() : 0u;
        HRESULT hr = g_presenter_swap->GetBuffer(back_index, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb));
        if (FAILED(hr) || !bb)
            return false;

        fg::g_ctx->CopyResource(bb, src);
        // Present submits the copy; avoid an explicit Flush here so the presenter thread does not
        // inject unnecessary command-stream flushes into the game's shared D3D11 device.
        bb->Release();

        // VSync here blocks only the independent presenter thread. That is the crucial difference
        // from the legacy path: the game is free to render its next real frame while this thread
        // waits for the generated/real output slots.
        hr = g_presenter_swap->Present(g_settings.presenter_vsync ? 1u : 0u, 0);
        if (SUCCEEDED(hr)) {
            const double t = fg::now_seconds();
            if (g_presenter_last_present_time > 0.0) {
                const double dt = t - g_presenter_last_present_time;
                if (dt > 0.0001 && dt < 0.5) {
                    const float instant = static_cast<float>(1.0 / dt);
                    const float old = g_presenter_output_fps.load(std::memory_order_relaxed);
                    g_presenter_output_fps.store(old > 1.0f ? (old * 0.90f + instant * 0.10f) : instant, std::memory_order_relaxed);
                }
            }
            g_presenter_last_present_time = t;
            fg::record_present();
            if (generated) {
                g_presenter_generated_presents.fetch_add(1, std::memory_order_relaxed);
                fg::g_extra_presents.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_presenter_real_presents.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        fg::g_last_hr = hr;
        return false;
    }

    void presenter_thread_main()
    {
        for (;;) {
            int idx = -1;
            {
                std::unique_lock<std::mutex> lock(g_presenter_mutex);
                g_presenter_cv.wait(lock, [] { return g_presenter_stop || !g_presenter_ready.empty(); });
                if (g_presenter_stop)
                    break;
                // Low-latency policy: if producer outran us, keep the newest packet and discard
                // older ready packets rather than building a long FG queue.
                while (g_presenter_ready.size() > 1) {
                    int old = g_presenter_ready.front();
                    g_presenter_ready.pop_front();
                    g_presenter_slots[old].ready = false;
                    g_presenter_dropped_packets.fetch_add(1, std::memory_order_relaxed);
                }
                idx = g_presenter_ready.front();
                g_presenter_ready.pop_front();
                g_presenter_slots[idx].ready = false;
                g_presenter_slots[idx].in_use = true;
            }

            PresenterSlot &slot = g_presenter_slots[idx];
            // Window ownership/positioning stays on the game's callback thread. The presenter
            // worker only touches DXGI and the queued textures, reducing cross-thread HWND work.
            if (!g_reshade_overlay_open && g_presenter_hwnd && IsWindowVisible(g_presenter_hwnd)) {
                if (!slot.warmup_only)
                    presenter_copy_and_present(slot.generated, true);
                presenter_copy_and_present(slot.real, false);
            }

            {
                std::lock_guard<std::mutex> lock(g_presenter_mutex);
                slot.in_use = false;
            }
        }
    }

    void presenter_stop()
    {
        {
            std::lock_guard<std::mutex> lock(g_presenter_mutex);
            g_presenter_stop = true;
        }
        g_presenter_cv.notify_all();
        if (g_presenter_thread.joinable())
            g_presenter_thread.join();
        g_presenter_running = false;
        g_presenter_stop = false;

        std::lock_guard<std::mutex> lock(g_presenter_mutex);
        presenter_release_resources();
        if (g_presenter_mt) {
            g_presenter_mt->SetMultithreadProtected(g_presenter_prev_mt);
            release(g_presenter_mt);
        }
        presenter_destroy_window();
        g_presenter_status = "off";
    }

    bool presenter_start_or_rebuild(reshade::api::effect_runtime *runtime, ID3D11Texture2D *game_bb)
    {
        if (!runtime || !game_bb || !fg::g_dev)
            return false;
        D3D11_TEXTURE2D_DESC d{};
        game_bb->GetDesc(&d);
        HWND hwnd = reinterpret_cast<HWND>(runtime->get_hwnd());
        if (!hwnd)
            return false;

        bool rebuild = !g_presenter_swap || g_presenter_device != fg::g_dev ||
                       g_presenter_game_hwnd != hwnd ||
                       g_presenter_width != d.Width || g_presenter_height != d.Height ||
                       g_presenter_format != d.Format;
        if (rebuild) {
            // No queue item is allowed to reference resources while they are recreated.
            if (g_presenter_running)
                presenter_stop();
            if (!presenter_create_swapchain_and_slots(hwnd, d.Width, d.Height, d.Format))
                return false;
        }
        if (!g_presenter_running) {
            g_presenter_stop = false;
            g_presenter_thread = std::thread(presenter_thread_main);
            g_presenter_running = true;
        }
        return true;
    }

    int presenter_acquire_slot()
    {
        std::lock_guard<std::mutex> lock(g_presenter_mutex);
        for (int i = 0; i < 3; ++i)
            if (!g_presenter_slots[i].ready && !g_presenter_slots[i].in_use)
                return i;

        // All free slots may be queued. Drop the oldest queued packet, never the one currently
        // being presented, to keep latency bounded.
        if (!g_presenter_ready.empty()) {
            int i = g_presenter_ready.front();
            g_presenter_ready.pop_front();
            g_presenter_slots[i].ready = false;
            g_presenter_dropped_packets.fetch_add(1, std::memory_order_relaxed);
            return i;
        }
        return -1;
    }

    void presenter_enqueue_slot(int idx, bool warmup_only, unsigned long long serial)
    {
        if (idx < 0 || idx >= 3)
            return;
        {
            std::lock_guard<std::mutex> lock(g_presenter_mutex);
            PresenterSlot &slot = g_presenter_slots[idx];
            slot.warmup_only = warmup_only;
            slot.serial = serial;
            slot.ready = true;
            g_presenter_ready.push_back(idx);
        }
        g_presenter_cv.notify_one();
    }

    void run_independent_presenter(reshade::api::effect_runtime *runtime)
    {
        // This path purposely does not call the original extra-Present logic. It reuses all of the
        // original capture/flow/interpolation resources, but queues output into the independent
        // presenter instead of consuming the game's swapchain slots.
        if (fg::g_inside_extra_present)
            return;
        const unsigned long long frame_index = fg::g_real_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!fg::g_settings.enabled) {
            fg::g_status = "disabled";
            fg::g_have_prev = false;
            presenter_hide_window();
            return;
        }

        reshade::api::device *api_device = runtime ? runtime->get_device() : nullptr;
        if (!api_device || api_device->get_api() != reshade::api::device_api::d3d11) {
            g_presenter_status = "independent presenter is DX11-only";
            // Preserve the old implementation for DX12/Vulkan/etc. rather than breaking them.
            run_original_with_output_mode(runtime);
            return;
        }

        ID3D11Device *dev = reinterpret_cast<ID3D11Device *>(api_device->get_native());
        if (!dev || !fg::ensure_device(dev)) {
            g_presenter_status = "D3D11 pipeline unavailable";
            return;
        }
        reshade::api::resource br = runtime->get_current_back_buffer();
        ID3D11Texture2D *bb = reinterpret_cast<ID3D11Texture2D *>(br.handle);
        if (!bb || !fg::ensure_resources(bb)) {
            g_presenter_status = "backbuffer/resources unavailable";
            return;
        }
        if (!presenter_start_or_rebuild(runtime, bb))
            return;
        presenter_sync_window();

        if (fg::g_settings.aa)
            fg::apply_aa(bb);
        fg::g_ctx->CopyResource(fg::g_curr_tex, bb);

        bool flow_ready = false;
        if (fg::g_settings.accumulate && fg::g_have_prev && fg::g_settings.debug_mode == 0) {
            fg::apply_accumulator(bb);
            fg::g_ctx->CopyResource(fg::g_curr_tex, bb);
            flow_ready = true;
        }

        double cb_entry = fg::now_seconds();
        if (fg::g_prev_cb_entry > 0.0) {
            double native = cb_entry - fg::g_prev_cb_entry; // no presenter wait occurs on this thread
            if (native > 0.0001 && native < 0.5)
                fg::g_dt_ema = (fg::g_dt_ema > 0.0) ? (fg::g_dt_ema * 0.9 + native * 0.1) : native;
        }
        fg::g_prev_cb_entry = cb_entry;
        fg::g_last_wait_total = 0.0;
        fg::g_paced_ms = 0.0;

        int slot_idx = presenter_acquire_slot();
        if (slot_idx >= 0) {
            PresenterSlot &slot = g_presenter_slots[slot_idx];
            // Always queue the actual real frame; on the warm-up frame there is no previous real
            // frame yet, so it is the only thing the output presenter displays.
            fg::g_ctx->CopyResource(slot.real, fg::g_curr_tex);
            bool warmup = !fg::g_have_prev;
            bool generated_ok = false;
            if (!warmup && fg::g_settings.debug_mode == 0) {
                generated_ok = fg::draw_interpolated(slot.generated, 0.5f, !flow_ready);
                if (generated_ok)
                    fg::g_gen_frames.fetch_add(1, std::memory_order_relaxed);
            }
            presenter_enqueue_slot(slot_idx, warmup || !generated_ok, frame_index);
        }

        std::swap(fg::g_prev_tex, fg::g_curr_tex);
        std::swap(fg::g_prev_srv, fg::g_curr_srv);
        fg::g_have_prev = true;
        fg::g_status = "independent additive presenter";
        g_presenter_status = "running";
    }

    void run_original_with_output_mode(reshade::api::effect_runtime *runtime)
    {
        // The original pacer intentionally holds the game's real Present so interpolated frames
        // can occupy evenly spaced slots. That is useful for smooth pacing, but on a vsync/capped
        // title the added wait can push the real Present past its next display slot and effectively
        // cut the native cadence in half. Additive mode keeps the old implementation available but
        // temporarily bypasses those waits: generated Presents are immediate and the real Present
        // is returned to the game as soon as our rendering work is finished.
        const bool override_output = (g_settings.output_mode == 1) && fg::g_settings.extra_present;
        const bool saved_pace = fg::g_settings.pace;
        const int saved_sync = fg::g_settings.present_sync;

        if (override_output) {
            fg::g_settings.pace = false;
            fg::g_settings.present_sync = 0;
        }

        fg::run(runtime);

        // Preserve the user's settings in the original FrameGen Preview tab. Additive mode is a
        // runtime override only, so turning it back off restores the exact old paced behaviour.
        if (override_output) {
            fg::g_settings.pace = saved_pace;
            fg::g_settings.present_sync = saved_sync;
        }
    }

    void run(reshade::api::effect_runtime *runtime)
    {
        // Preserve the existing implementation byte-for-byte when Original is selected.
        if (!wants_external_backend()) {
            g_extension_status = "Original optical flow (unchanged)";
            if (g_settings.output_mode == 2)
                run_independent_presenter(runtime);
            else
                run_original_with_output_mode(runtime);
            return;
        }

        // First call (or a device rebuild) lets the original implementation initialize itself.
        // We then compile the extension against that exact D3D11 device for subsequent frames.
        if (!fg::g_pipeline_ready || !fg::g_dev || !fg::g_ctx) {
            // If the original pipeline was torn down (device switch / DX12 toggle), discard any
            // extension objects tied to the old D3D11 device before rebuilding.
            release_extension_pipeline();
            if (g_settings.output_mode == 2)
                run_independent_presenter(runtime);
            else
                run_original_with_output_mode(runtime);
            if (fg::g_pipeline_ready && fg::g_dev)
                ensure_extension_pipeline();
            return;
        }

        refresh_external_motion(runtime);
        const int motion_mode = resolved_motion_mode();
        if (motion_mode == 0 || !ensure_extension_pipeline()) {
            g_extension_status = g_external_mv_found ?
                "Hybrid unavailable - Original fallback" :
                "texMotionVectors not found - Original fallback";
            if (g_settings.output_mode == 2)
                run_independent_presenter(runtime);
            else
                run_original_with_output_mode(runtime);
            return;
        }

        update_extension_cb(motion_mode);

        // Save exactly the two slots this extension adds. The original StateBlock intentionally
        // owns only t0..t3 and b0, so t4/b1 survive its individual full-screen passes.
        ID3D11ShaderResourceView *old_t4 = nullptr;
        ID3D11Buffer *old_b1 = nullptr;
        fg::g_ctx->PSGetShaderResources(4, 1, &old_t4);
        fg::g_ctx->PSGetConstantBuffers(1, 1, &old_b1);
        fg::g_ctx->PSSetShaderResources(4, 1, &g_external_mv_srv);
        fg::g_ctx->PSSetConstantBuffers(1, 1, &g_hybrid_cb);

        // Swap only the two shader objects. Every resource, pacing path, accumulator, extra-Present
        // path, debug view and fallback remains the original implementation.
        ID3D11PixelShader *original_flow = fg::g_ps_flow;
        ID3D11PixelShader *original_interp = fg::g_ps_interp;
        fg::g_ps_flow = g_ps_flow_hybrid;
        fg::g_ps_interp = g_ps_interp_hybrid;

        g_extension_status = (motion_mode == 1) ?
            "External texMotionVectors" :
            "Hybrid texMotionVectors + optical residual";
        if (g_settings.output_mode == 2)
            run_independent_presenter(runtime);
        else
            run_original_with_output_mode(runtime);

        fg::g_ps_flow = original_flow;
        fg::g_ps_interp = original_interp;

        // Avoid retaining ReShade effect resources in D3D11 state after our callback.
        ID3D11ShaderResourceView *null_t4 = nullptr;
        fg::g_ctx->PSSetShaderResources(4, 1, &null_t4);
        fg::g_ctx->PSSetShaderResources(4, 1, &old_t4);
        fg::g_ctx->PSSetConstantBuffers(1, 1, &old_b1);
        release(old_t4);
        release(old_b1);
        release_external_view();
    }

    void draw_overlay(reshade::api::effect_runtime *)
    {
        if (!ImGui::CollapsingHeader("Hybrid motion / Feeder interop", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        const char *items[] = {
            "Original optical flow (unchanged)",
            "Auto: Hybrid when texMotionVectors exists",
            "External texMotionVectors only",
            "Hybrid: external MV + optical residual"
        };
        ImGui::Combo("Motion backend", &g_settings.motion_backend, items, 4);

        ImGui::Separator();
        ImGui::Text("Frame output / pacing");
        const char *output_items[] = {
            "Legacy: original paced in-swapchain FG",
            "Legacy: immediate extra Present (no wait)",
            "TRUE ADDITIVE: independent presenter (DX11, x2)"
        };
        int old_output_mode = g_settings.output_mode;
        if (ImGui::Combo("Output backend", &g_settings.output_mode, output_items, 3)) {
            if (old_output_mode == 2 && g_settings.output_mode != 2)
                presenter_stop();
        }
        if (g_settings.output_mode == 2) {
            ImGui::Checkbox("Presenter VSync (recommended)", &g_settings.presenter_vsync);
            ImGui::Checkbox("Hide presenter when game is unfocused", &g_settings.presenter_hide_unfocused);
            ImGui::TextDisabled("Game Present is left untouched. Generated + real frames are shown on a separate owned swapchain.");
            ImGui::TextDisabled("First test implementation: DX11 + windowed/borderless + x2 only. Exclusive fullscreen is unsupported; HDR is experimental.");
            ImGui::TextDisabled("FrameGen Preview's Extra Present / Pace options are ignored by this backend.");
            ImGui::Text("Presenter: %s", g_presenter_status);
            ImGui::Text("Presenter output: %.1f FPS", g_presenter_output_fps.load(std::memory_order_relaxed));
            ImGui::Text("Presenter real/gen: %llu / %llu | dropped packets: %llu",
                g_presenter_real_presents.load(), g_presenter_generated_presents.load(), g_presenter_dropped_packets.load());
            if (fg::g_settings.multiplier != 2)
                ImGui::TextDisabled("Multiplier is forced conceptually to x2 in this presenter test build.");
        } else if (g_settings.output_mode == 1) {
            ImGui::TextDisabled("Old experimental no-wait mode: preserves game thread but generated/real Presents can land back-to-back.");
        } else {
            ImGui::TextDisabled("Original behavior: divides the game's frame interval into generated + real slots.");
        }

        if (g_settings.motion_backend != original) {
            ImGui::SliderFloat("External MV scale X", &g_settings.motion_scale_x, -4.0f, 4.0f, "%.3f");
            ImGui::SliderFloat("External MV scale Y", &g_settings.motion_scale_y, -4.0f, 4.0f, "%.3f");
            if (g_settings.motion_backend != external_only)
                ImGui::SliderFloat("Hybrid residual radius (px)", &g_settings.residual_radius_px, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("External confidence", &g_settings.external_confidence, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Enhanced disocclusion rejection", &g_settings.enhanced_disocclusion);
            ImGui::SliderFloat("Occlusion rejection", &g_settings.occlusion_strength, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Depth silhouette rejection", &g_settings.depth_rejection_strength, 0.0f, 3.0f, "%.2f");

            ImGui::Separator();
            ImGui::Text("Motion texture: %s", g_external_mv_found ? "FOUND" : "searched each frame");
            if (g_external_mv_effect[0] != '\0')
                ImGui::Text("Provider effect: %s", g_external_mv_effect);
            ImGui::Text("Backend: %s", g_extension_status);
            static const char *out_names[] = { "Legacy paced", "Legacy immediate", "Independent additive presenter" };
            ImGui::Text("Output mode: %s", out_names[std::clamp(g_settings.output_mode, 0, 2)]);
            ImGui::TextDisabled("Uses the community UV-space texMotionVectors contract (qUINT / ReShadeMotionEstimation / Feeder ecosystem).");
            ImGui::TextDisabled("No NVIDIA DLLs are bundled. DLSS/NR effects may still run normally before reshade_present; this addon then interpolates the resulting real frames.");
            ImGui::TextDisabled("DX12 keeps the existing D3D11On12 backend and falls back to Original motion for now.");
        }
    }

    void shutdown()
    {
        presenter_stop();
        release_external_view(true);
        release_extension_pipeline();
    }

    bool on_reshade_open_overlay(reshade::api::effect_runtime *, bool open, reshade::api::input_source)
    {
        g_reshade_overlay_open = open;
        if (open)
            presenter_hide_window();
        return false;
    }

    void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        // Tear the worker down while ReShade/runtime code is still fully alive rather than waiting
        // for DLL_PROCESS_DETACH. This also handles games recreating their effect runtime/swapchain.
        if (!runtime || !g_presenter_game_hwnd || reinterpret_cast<HWND>(runtime->get_hwnd()) == g_presenter_game_hwnd)
            presenter_stop();
    }

    void on_present(reshade::api::effect_runtime *runtime)
    {
        run(runtime);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        fgx::g_module_instance = hinstDLL;
        // Let the original source register everything it already owns first.
        if (!ReShadeFrameGen_OriginalDllMain(hinstDLL, reason, reserved))
            return FALSE;
        // Replace only its present callback with the dispatcher above. The dispatcher calls the
        // original fg::run directly whenever Original/fallback is active.
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(&fgx::on_reshade_open_overlay);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&fgx::on_destroy_effect_runtime);
        reshade::register_overlay("Hybrid Motion + Output", &fgx::draw_overlay);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(&fgx::on_reshade_open_overlay);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&fgx::on_destroy_effect_runtime);
        fgx::shutdown();
        // Original cleanup owns the rest of the pipeline/events/add-on registration.
        return ReShadeFrameGen_OriginalDllMain(hinstDLL, reason, reserved);
    }
    return TRUE;
}
