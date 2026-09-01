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
#include <dxgi1_5.h>
#include <dwmapi.h>
#include <thread>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <atomic>

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
        // 2 = V3 isolated presenter (kept for A/B),
        // 3 = V4 fullscreen compositor: FreeGen/Magpie style zero-copy output surface,
        // 4 = V5 native deep queue: LSFG-style game swapchain expansion + FIFO/vblank queueing.
        // V5 is default in this test branch so create_swapchain is armed before the game creates
        // its first DXGI swapchain. All older modes remain selectable afterwards.
        int output_mode = 4;
        bool presenter_hide_unfocused = true;
        bool presenter_self_pacing = true;
        bool presenter_allow_tearing = true;
        bool presenter_waitable_swapchain = true;
        bool presenter_cap_to_refresh = false; // never silently force x2 back to 60 Hz
        bool presenter_force_topmost = true;
        bool deep_queue_force_vsync = true;
        bool deep_queue_raise_fullscreen_refresh = true;
        bool deep_queue_force_flip_model = false; // Special-K-style compatibility lever; restart required
        int deep_queue_extra_buffers = 2; // one delayed real frame + one x2 intermediate
        bool reuse_legacy_postprocess = false; // AA/temporal reconstruction stay legacy-only unless explicitly requested
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
    // Independent additive presenter - V3 stability architecture
    //
    // IMPORTANT: the game D3D11 immediate context is NEVER touched from the presenter thread.
    // The game thread renders/copies into shared keyed-mutex textures. The presenter thread owns
    // a completely separate D3D11 device + immediate context + DXGI swapchain + HWND, opens those
    // shared textures on the same adapter, and performs all output CopyResource/Present calls.
    //
    // This matters for ReShade/Home-menu transitions: hiding the overlay is the first moment the
    // presenter becomes visible and begins presenting. V2 shared the game's immediate context on
    // this worker, which violates D3D11/DXGI threading guidance and could deadlock exactly then.
    // --------------------------------------------------------------------------------------
    struct PresenterSlot
    {
        // Game-device side. The interpolation shader writes here on the normal game/ReShade thread.
        ID3D11Texture2D *generated = nullptr;
        ID3D11Texture2D *real = nullptr;
        IDXGIKeyedMutex *generated_game_mutex = nullptr;
        IDXGIKeyedMutex *real_game_mutex = nullptr;
        HANDLE generated_shared = nullptr; // legacy DXGI shared handle; do not CloseHandle
        HANDLE real_shared = nullptr;

        // Presenter-device side. Opened from the shared handles and touched only by worker thread.
        ID3D11Texture2D *generated_present = nullptr;
        ID3D11Texture2D *real_present = nullptr;
        IDXGIKeyedMutex *generated_present_mutex = nullptr;
        IDXGIKeyedMutex *real_present_mutex = nullptr;

        bool ready = false;
        bool in_use = false;
        bool warmup_only = false;
        bool generated_ready = false;
        bool real_ready = false;
        unsigned long long serial = 0;
    };

    HINSTANCE g_module_instance = nullptr;
    HWND g_presenter_game_hwnd = nullptr;
    std::atomic<HWND> g_presenter_hwnd{nullptr};

    // The game device is borrowed only as an identity for rebuild detection and creating the
    // shared producer textures. It is never used by the worker.
    ID3D11Device *g_presenter_device = nullptr;
    IDXGIAdapter *g_presenter_adapter = nullptr; // AddRef'd until worker stops

    // Owned exclusively by presenter thread after creation.
    ID3D11Device *g_present_device = nullptr;
    ID3D11DeviceContext *g_present_ctx = nullptr;
    IDXGISwapChain1 *g_presenter_swap = nullptr;
    IDXGISwapChain2 *g_presenter_swap2 = nullptr;
    IDXGISwapChain3 *g_presenter_swap3 = nullptr;
    HANDLE g_presenter_frame_latency = nullptr; // owned by swapchain; never CloseHandle

    reshade::api::effect_runtime *g_presenter_effect_runtime = nullptr; // borrowed secondary runtime
    thread_local bool g_inside_presenter_present = false;
    std::thread g_presenter_thread;
    std::mutex g_presenter_mutex;
    std::condition_variable g_presenter_cv;
    PresenterSlot g_presenter_slots[3] = {};
    std::deque<int> g_presenter_ready;
    bool g_presenter_stop = false;
    bool g_presenter_running = false;
    std::atomic<bool> g_presenter_worker_ready{false};
    std::atomic<bool> g_presenter_worker_failed{false};
    std::atomic<bool> g_reshade_overlay_open{false};
    std::atomic<bool> g_presenter_force_hide{true};
    UINT g_presenter_width = 0, g_presenter_height = 0;
    DXGI_FORMAT g_presenter_format = DXGI_FORMAT_UNKNOWN;
    std::atomic<unsigned long long> g_presenter_real_presents{0};
    std::atomic<unsigned long long> g_presenter_generated_presents{0};
    std::atomic<unsigned long long> g_presenter_dropped_packets{0};
    std::atomic<unsigned long long> g_presenter_sync_misses{0};
    std::atomic<float> g_presenter_output_fps{0.0f};
    std::atomic<float> g_presenter_game_fps{0.0f};
    std::atomic<float> g_presenter_target_fps{0.0f};
    std::atomic<double> g_presenter_native_interval{1.0 / 60.0};
    double g_presenter_last_present_time = 0.0; // presenter thread only
    double g_presenter_next_slot = 0.0;         // presenter thread only
    std::atomic<float> g_presenter_refresh_hz{0.0f};
    std::atomic<bool> g_presenter_tearing_supported{false};
    std::atomic<bool> g_presenter_self_pacing_runtime{true};
    std::atomic<bool> g_presenter_hide_unfocused_runtime{true};
    std::atomic<bool> g_presenter_swap_tearing_enabled{false};
    std::atomic<bool> g_presenter_waitable_active{false};
    std::atomic<unsigned long long> g_presenter_wait_timeouts{0};
    std::atomic<long> g_presenter_last_hr{S_OK};
    std::atomic<const char *> g_presenter_status{"off"};

    // V5 native/deep-queue telemetry. ReShade's create_swapchain callback runs before DXGI creates
    // the game swapchain, which lets us transplant the key lsfg-vk trick: add images to the GAME'S
    // OWN queue, then let FIFO/vblank pacing consume generated + real frames instead of sleeping
    // inside the game's render callback.
    std::atomic<bool> g_deep_queue_patched{false};
    std::atomic<unsigned> g_deep_queue_original_buffers{0};
    std::atomic<unsigned> g_deep_queue_created_buffers{0};
    std::atomic<unsigned> g_deep_queue_forced_sync{0xffffffffu};
    std::atomic<unsigned> g_deep_queue_original_present_mode{0xffffffffu};
    std::atomic<unsigned> g_deep_queue_created_present_mode{0xffffffffu};
    std::atomic<bool> g_deep_queue_flip_promoted{false};
    std::atomic<HWND> g_deep_queue_hwnd{nullptr};
    std::atomic<unsigned long long> g_deep_queue_primary_area{0};
    std::atomic<bool> g_deep_queue_latency_applied{false};
    std::atomic<long> g_deep_queue_latency_hr{S_OK};
    // ReShade 6.7.3 / add-on API 18 has no low-level finish_present event. Track the
    // native-output contract with the counters the original add-on already owns instead:
    // real callback frames + successful injected Presents. This is deliberately labelled
    // "submitted output" in the UI rather than pretending it is scan-out telemetry.
    std::atomic<unsigned long long> g_deep_queue_present_count{0};
    std::atomic<float> g_deep_queue_output_fps{0.0f};
    std::atomic<unsigned long long> g_deep_queue_counter_origin{0};
    std::atomic<unsigned long long> g_deep_queue_last_sample_total{0};
    std::atomic<double> g_deep_queue_last_present_ts{0.0};

    LRESULT CALLBACK presenter_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
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

    void presenter_release_slot_game(PresenterSlot &s)
    {
        presenter_forget_cached_rtv(s.generated);
        presenter_forget_cached_rtv(s.real);
        release(s.generated_game_mutex);
        release(s.real_game_mutex);
        release(s.generated);
        release(s.real);
        s.generated_shared = nullptr;
        s.real_shared = nullptr;
        s.ready = s.in_use = s.warmup_only = false;
        s.generated_ready = s.real_ready = false;
        s.serial = 0;
    }

    void presenter_release_slot_worker(PresenterSlot &s)
    {
        release(s.generated_present_mutex);
        release(s.real_present_mutex);
        release(s.generated_present);
        release(s.real_present);
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

    float presenter_query_refresh_hz(HWND hwnd)
    {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi))
            return 0.0f;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1)
            return 0.0f;
        return static_cast<float>(dm.dmDisplayFrequency);
    }

    const char *dxgi_present_mode_name(uint32_t mode)
    {
        switch (mode) {
        case DXGI_SWAP_EFFECT_DISCARD: return "BLT discard";
        case DXGI_SWAP_EFFECT_SEQUENTIAL: return "BLT sequential";
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "flip sequential";
        case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "flip discard";
        default: return "unknown/custom";
        }
    }

    bool dxgi_present_mode_is_flip(uint32_t mode)
    {
        return mode == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || mode == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    }

    // Optional Special-K-style promotion lever. A full Special K implementation uses a proxy
    // backbuffer to cover compatibility edge cases; ReShade's create_swapchain event cannot
    // replace the COM swapchain pointer, so this deliberately stays opt-in. It is most useful
    // for windowed/borderless DX11 games that still request old BLT swap effects.
    bool maybe_promote_to_flip(reshade::api::swapchain_desc &desc)
    {
        if (!g_settings.deep_queue_force_flip_model || desc.fullscreen_state || desc.back_buffer.texture.samples != 1)
            return false;

        if (desc.present_mode == DXGI_SWAP_EFFECT_DISCARD) {
            desc.present_mode = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.back_buffer_count = std::max(desc.back_buffer_count, 2u);
            return true;
        }
        if (desc.present_mode == DXGI_SWAP_EFFECT_SEQUENTIAL) {
            desc.present_mode = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.back_buffer_count = std::max(desc.back_buffer_count, 2u);
            return true;
        }
        return false;
    }

    // LSFG-style native queue surgery. ReShade preserves a sync_interval supplied by a
    // create_swapchain add-on and applies it to later DXGI Presents. This gives us both parts of
    // the mechanism lsfg-vk relies on: enough native backbuffers for generated+real images and a
    // FIFO/vblank consumer. The interpolation shader is still ours.
    bool on_create_swapchain(reshade::api::device_api api, reshade::api::swapchain_desc &desc, void *hwnd_ptr)
    {
        if (g_settings.output_mode != 4 || api != reshade::api::device_api::d3d11 || hwnd_ptr == nullptr)
            return false;

        HWND hwnd = reinterpret_cast<HWND>(hwnd_ptr);
        HWND presenter_hwnd = g_presenter_hwnd.load(std::memory_order_acquire);
        if (presenter_hwnd && hwnd == presenter_hwnd)
            return false;

        const uint32_t requested_original_count = desc.back_buffer_count;
        const uint32_t requested_present_mode = desc.present_mode;
        bool modified = maybe_promote_to_flip(desc);
        const bool promoted_flip = desc.present_mode != requested_present_mode;

        const uint32_t original = std::max(1u, requested_original_count);
        const uint32_t extra = static_cast<uint32_t>(std::clamp(g_settings.deep_queue_extra_buffers, 1, 4));
        const uint32_t desired = std::min(8u, std::max(4u, original + extra));

        if (desc.back_buffer_count < desired) {
            desc.back_buffer_count = desired;
            modified = true;
        }

        if (g_settings.deep_queue_force_vsync && desc.sync_interval != 1u) {
            desc.sync_interval = 1u;
            desc.present_flags &= ~static_cast<uint32_t>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
            modified = true;
        }

        if (g_settings.deep_queue_raise_fullscreen_refresh && desc.fullscreen_state) {
            const float desktop_hz = presenter_query_refresh_hz(hwnd);
            if (desktop_hz > 1.0f &&
                (desc.fullscreen_refresh_rate <= 1.0f || desktop_hz > desc.fullscreen_refresh_rate + 0.5f)) {
                desc.fullscreen_refresh_rate = desktop_hz;
                modified = true;
            }
        }

        // Patch all D3D11 presentation swapchains, but use the largest HWND as telemetry/FG target
        // so a tiny video/launcher swapchain created later does not steal the counters.
        uint64_t w = desc.back_buffer.texture.width;
        uint64_t h = desc.back_buffer.texture.height;
        if (w == 0 || h == 0) {
            RECT r{};
            if (GetClientRect(hwnd, &r)) { w = static_cast<uint64_t>(std::max(0L, r.right - r.left)); h = static_cast<uint64_t>(std::max(0L, r.bottom - r.top)); }
        }
        const uint64_t area = w * h;
        uint64_t previous_area = g_deep_queue_primary_area.load(std::memory_order_relaxed);
        if (!g_deep_queue_hwnd.load(std::memory_order_relaxed) || area >= previous_area) {
            g_deep_queue_primary_area.store(area, std::memory_order_relaxed);
            g_deep_queue_patched.store(true, std::memory_order_release);
            g_deep_queue_original_buffers.store(requested_original_count, std::memory_order_relaxed);
            g_deep_queue_created_buffers.store(desc.back_buffer_count, std::memory_order_relaxed);
            g_deep_queue_forced_sync.store(desc.sync_interval, std::memory_order_relaxed);
            g_deep_queue_original_present_mode.store(requested_present_mode, std::memory_order_relaxed);
            g_deep_queue_created_present_mode.store(desc.present_mode, std::memory_order_relaxed);
            g_deep_queue_flip_promoted.store(promoted_flip, std::memory_order_relaxed);
            g_deep_queue_hwnd.store(hwnd, std::memory_order_release);
            const unsigned long long total_now =
                fg::g_real_frames.load(std::memory_order_relaxed) +
                fg::g_extra_presents.load(std::memory_order_relaxed);
            g_deep_queue_present_count.store(0, std::memory_order_relaxed);
            g_deep_queue_output_fps.store(0.0f, std::memory_order_relaxed);
            g_deep_queue_counter_origin.store(total_now, std::memory_order_relaxed);
            g_deep_queue_last_sample_total.store(total_now, std::memory_order_relaxed);
            g_deep_queue_last_present_ts.store(0.0, std::memory_order_relaxed);
        }
        return modified;
    }

    void on_init_swapchain(reshade::api::swapchain *swapchain, bool)
    {
        if (!swapchain || g_settings.output_mode != 4)
            return;
        HWND hwnd = reinterpret_cast<HWND>(swapchain->get_hwnd());
        if (hwnd && hwnd == g_deep_queue_hwnd.load(std::memory_order_acquire)) {
            const uint32_t actual = swapchain->get_back_buffer_count();
            g_deep_queue_created_buffers.store(actual, std::memory_order_relaxed);

            IDXGISwapChain *native_swap = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
            DXGI_SWAP_CHAIN_DESC native_desc{};
            if (native_swap && SUCCEEDED(native_swap->GetDesc(&native_desc))) {
                g_deep_queue_created_present_mode.store(static_cast<uint32_t>(native_desc.SwapEffect), std::memory_order_relaxed);
            }
            g_deep_queue_patched.store(actual >= 4, std::memory_order_release);
        }
    }

    void update_deep_queue_telemetry(reshade::api::effect_runtime *runtime)
    {
        if (!runtime || g_settings.output_mode != 4 || !g_deep_queue_patched.load(std::memory_order_acquire))
            return;

        const HWND hwnd = reinterpret_cast<HWND>(runtime->get_hwnd());
        const HWND wanted = g_deep_queue_hwnd.load(std::memory_order_acquire);
        if (!wanted || hwnd != wanted)
            return;

        const unsigned long long total =
            fg::g_real_frames.load(std::memory_order_relaxed) +
            fg::g_extra_presents.load(std::memory_order_relaxed);
        const unsigned long long origin = g_deep_queue_counter_origin.load(std::memory_order_relaxed);
        g_deep_queue_present_count.store(total >= origin ? total - origin : 0, std::memory_order_relaxed);

        const double now = fg::now_seconds();
        double last_ts = g_deep_queue_last_present_ts.load(std::memory_order_relaxed);
        if (last_ts <= 0.0) {
            g_deep_queue_last_present_ts.store(now, std::memory_order_relaxed);
            g_deep_queue_last_sample_total.store(total, std::memory_order_relaxed);
            return;
        }

        const double elapsed = now - last_ts;
        if (elapsed < 0.25)
            return;

        const unsigned long long previous = g_deep_queue_last_sample_total.exchange(total, std::memory_order_relaxed);
        g_deep_queue_last_present_ts.store(now, std::memory_order_relaxed);
        if (total < previous || elapsed <= 0.0)
            return;

        const float instant = static_cast<float>(static_cast<double>(total - previous) / elapsed);
        const float old_fps = g_deep_queue_output_fps.load(std::memory_order_relaxed);
        g_deep_queue_output_fps.store(old_fps > 1.0f ? old_fps * 0.75f + instant * 0.25f : instant, std::memory_order_relaxed);
    }

    bool presenter_make_shared_texture(UINT w, UINT h, DXGI_FORMAT fmt,
                                       ID3D11Texture2D **out_tex,
                                       IDXGIKeyedMutex **out_mutex,
                                       HANDLE *out_handle)
    {
        if (!fg::g_dev || !out_tex || !out_mutex || !out_handle)
            return false;
        *out_tex = nullptr;
        *out_mutex = nullptr;
        *out_handle = nullptr;

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT hr = fg::g_dev->CreateTexture2D(&d, nullptr, out_tex);
        if (FAILED(hr) || !*out_tex) {
            fg::g_last_hr = hr;
            return false;
        }
        hr = (*out_tex)->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(out_mutex));
        IDXGIResource *res = nullptr;
        if (SUCCEEDED(hr))
            hr = (*out_tex)->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&res));
        if (SUCCEEDED(hr) && res)
            hr = res->GetSharedHandle(out_handle);
        release(res);
        if (FAILED(hr) || !*out_mutex || !*out_handle) {
            fg::g_last_hr = hr;
            release(*out_mutex);
            release(*out_tex);
            *out_handle = nullptr;
            return false;
        }
        return true;
    }

    void presenter_release_game_resources()
    {
        for (auto &s : g_presenter_slots)
            presenter_release_slot_game(s);
        g_presenter_ready.clear();
        release(g_presenter_adapter);
        g_presenter_device = nullptr;
        g_presenter_game_hwnd = nullptr;
        g_presenter_width = g_presenter_height = 0;
        g_presenter_format = DXGI_FORMAT_UNKNOWN;
        g_presenter_output_fps.store(0.0f, std::memory_order_relaxed);
        g_presenter_target_fps.store(0.0f, std::memory_order_relaxed);
        g_presenter_refresh_hz.store(0.0f, std::memory_order_relaxed);
        g_presenter_tearing_supported.store(false, std::memory_order_relaxed);
        g_presenter_swap_tearing_enabled.store(false, std::memory_order_relaxed);
        g_presenter_waitable_active.store(false, std::memory_order_relaxed);
        g_presenter_wait_timeouts.store(0, std::memory_order_relaxed);
        g_presenter_last_hr.store(S_OK, std::memory_order_relaxed);
        g_presenter_last_present_time = 0.0;
        g_presenter_next_slot = 0.0;
    }

    bool presenter_prepare_game_resources(HWND game_hwnd, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (!fg::g_dev || !game_hwnd || !presenter_supported_format(fmt)) {
            g_presenter_status.store("unsupported game backbuffer", std::memory_order_relaxed);
            return false;
        }

        IDXGIDevice *dxgi_dev = nullptr;
        IDXGIAdapter *adapter = nullptr;
        HRESULT hr = fg::g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev));
        if (SUCCEEDED(hr) && dxgi_dev)
            hr = dxgi_dev->GetAdapter(&adapter);
        release(dxgi_dev);
        if (FAILED(hr) || !adapter) {
            release(adapter);
            g_presenter_status.store("game DXGI adapter unavailable", std::memory_order_relaxed);
            return false;
        }

        g_presenter_adapter = adapter; // ownership transferred
        for (auto &slot : g_presenter_slots) {
            if (!presenter_make_shared_texture(w, h, fmt, &slot.generated, &slot.generated_game_mutex, &slot.generated_shared) ||
                !presenter_make_shared_texture(w, h, fmt, &slot.real, &slot.real_game_mutex, &slot.real_shared)) {
                g_presenter_status.store("shared keyed texture creation failed", std::memory_order_relaxed);
                presenter_release_game_resources();
                return false;
            }
        }

        g_presenter_device = fg::g_dev;
        g_presenter_game_hwnd = game_hwnd;
        g_presenter_width = w;
        g_presenter_height = h;
        g_presenter_format = fmt;
        g_presenter_refresh_hz.store(presenter_query_refresh_hz(game_hwnd), std::memory_order_relaxed);
        g_presenter_status.store("shared resources ready; starting presenter thread", std::memory_order_relaxed);
        return true;
    }

    bool presenter_worker_create_window()
    {
        if (!g_presenter_game_hwnd)
            return false;

        static const wchar_t *klass = L"ReShadeFrameGenCompositorV4";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = presenter_wndproc;
        wc.hInstance = g_module_instance ? g_module_instance : GetModuleHandleW(nullptr);
        wc.lpszClassName = klass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        // V4 deliberately uses a TOP-LEVEL, unowned surface. V3 used an owned popup and therefore
        // still competed with the game's flip/composition path. Lossless-Scaling/Magpie-style
        // output works by making the compositor surface the visible final image while the game
        // continues rendering behind it. TOOLWINDOW keeps it out of Alt-Tab; TRANSPARENT and
        // NOACTIVATE keep all input/focus on the game.
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        if (g_settings.presenter_force_topmost)
            ex |= WS_EX_TOPMOST;
        HWND hwnd = CreateWindowExW(
            ex, klass, L"ReShade FrameGen Compositor", WS_POPUP,
            0, 0, 16, 16, nullptr, nullptr,
            g_module_instance ? g_module_instance : GetModuleHandleW(nullptr), nullptr);
        if (!hwnd)
            return false;

        // Exclude this utility surface from Peek/Flip3D and never activate it.
        BOOL yes = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &yes, sizeof(yes));
        g_presenter_hwnd.store(hwnd, std::memory_order_release);
        return true;
    }

    void presenter_worker_pump_messages()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    bool presenter_worker_sync_window()
    {
        HWND hwnd = g_presenter_hwnd.load(std::memory_order_acquire);
        HWND game = g_presenter_game_hwnd;
        if (!hwnd || !game)
            return false;

        bool focused = true;
        if (g_presenter_hide_unfocused_runtime.load(std::memory_order_relaxed)) {
            HWND fgwin = GetForegroundWindow();
            HWND root = fgwin ? GetAncestor(fgwin, GA_ROOT) : nullptr;
            focused = (fgwin == game || root == game || fgwin == hwnd);
        }
        const bool hidden = g_reshade_overlay_open.load(std::memory_order_relaxed) ||
                            g_presenter_force_hide.load(std::memory_order_relaxed) ||
                            IsIconic(game) || !IsWindowVisible(game) || !focused;
        if (hidden) {
            ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        RECT client{};
        POINT p{0, 0};
        if (!GetClientRect(game, &client) || !ClientToScreen(game, &p)) {
            ShowWindow(hwnd, SW_HIDE);
            return false;
        }
        RECT target{p.x, p.y, p.x + (client.right - client.left), p.y + (client.bottom - client.top)};

        // If the game already covers (almost) an entire monitor, cover that monitor exactly. This
        // mirrors standalone scaler/compositor tools and avoids a one-pixel border that can let
        // Independent Flip bypass our surface. Windowed games still use their client rectangle.
        HMONITOR mon = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT &m = mi.rcMonitor;
            if (abs(target.left - m.left) <= 8 && abs(target.top - m.top) <= 8 &&
                abs(target.right - m.right) <= 8 && abs(target.bottom - m.bottom) <= 8)
                target = m;
        }

        const int w = std::max(1L, target.right - target.left);
        const int h = std::max(1L, target.bottom - target.top);
        HWND z = g_settings.presenter_force_topmost ? HWND_TOPMOST : HWND_TOP;
        SetWindowPos(hwnd, z, target.left, target.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    bool presenter_worker_open_shared(PresenterSlot &slot)
    {
        if (!g_present_device || !slot.generated_shared || !slot.real_shared)
            return false;
        HRESULT hr = g_present_device->OpenSharedResource(slot.generated_shared, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&slot.generated_present));
        if (SUCCEEDED(hr) && slot.generated_present)
            hr = slot.generated_present->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&slot.generated_present_mutex));
        if (SUCCEEDED(hr))
            hr = g_present_device->OpenSharedResource(slot.real_shared, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&slot.real_present));
        if (SUCCEEDED(hr) && slot.real_present)
            hr = slot.real_present->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&slot.real_present_mutex));
        return SUCCEEDED(hr) && slot.generated_present_mutex && slot.real_present_mutex;
    }

    bool presenter_worker_init()
    {
        if (!g_presenter_adapter || !presenter_worker_create_window()) {
            g_presenter_status.store("presenter worker window/adapter init failed", std::memory_order_relaxed);
            return false;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        HRESULT hr = D3D11CreateDevice(g_presenter_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                       levels, static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                       &g_present_device, &fl, &g_present_ctx);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(g_presenter_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   levels + 1, static_cast<UINT>((sizeof(levels) / sizeof(levels[0])) - 1), D3D11_SDK_VERSION,
                                   &g_present_device, &fl, &g_present_ctx);
        }
        if (FAILED(hr) || !g_present_device || !g_present_ctx) {
            g_presenter_status.store("separate presenter D3D11 device creation failed", std::memory_order_relaxed);
            return false;
        }

        IDXGIDevice *dxgi_dev = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIFactory2 *factory = nullptr;
        hr = g_present_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev));
        if (SUCCEEDED(hr) && dxgi_dev)
            hr = dxgi_dev->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory));
        release(dxgi_dev);
        release(adapter);
        if (FAILED(hr) || !factory) {
            release(factory);
            g_presenter_status.store("presenter DXGI factory unavailable", std::memory_order_relaxed);
            return false;
        }

        BOOL allow_tearing = FALSE;
        IDXGIFactory5 *factory5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory5))) && factory5) {
            if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing))))
                allow_tearing = FALSE;
            factory5->Release();
        }
        g_presenter_tearing_supported.store(allow_tearing == TRUE, std::memory_order_relaxed);
        const bool swap_tearing = g_settings.presenter_allow_tearing && (allow_tearing == TRUE);
        g_presenter_swap_tearing_enabled.store(swap_tearing, std::memory_order_relaxed);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = g_presenter_width;
        sd.Height = g_presenter_height;
        sd.Format = g_presenter_format;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 3;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = 0u;
        if (g_presenter_swap_tearing_enabled.load(std::memory_order_relaxed))
            sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        // Microsoft low-latency pattern: throttle BEFORE render/present through the waitable
        // object instead of letting Present() become the hidden blocking point.
        if (g_settings.output_mode == 3 && g_settings.presenter_waitable_swapchain)
            sd.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        HWND hwnd = g_presenter_hwnd.load(std::memory_order_acquire);
        hr = factory->CreateSwapChainForHwnd(g_present_device, hwnd, &sd, nullptr, nullptr, &g_presenter_swap);
        if (SUCCEEDED(hr))
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        release(factory);
        if (FAILED(hr) || !g_presenter_swap) {
            g_presenter_status.store("separate presenter swapchain creation failed", std::memory_order_relaxed);
            return false;
        }

        if (g_settings.output_mode == 3 && g_settings.presenter_waitable_swapchain &&
            SUCCEEDED(g_presenter_swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&g_presenter_swap2))) && g_presenter_swap2) {
            // 1 keeps the compositor queue shallow. The wait happens before each output frame, so
            // the game producer remains completely independent.
            if (SUCCEEDED(g_presenter_swap2->SetMaximumFrameLatency(1))) {
                g_presenter_frame_latency = g_presenter_swap2->GetFrameLatencyWaitableObject();
                g_presenter_waitable_active.store(g_presenter_frame_latency != nullptr && g_presenter_frame_latency != INVALID_HANDLE_VALUE, std::memory_order_relaxed);
            }
        }

        if (SUCCEEDED(g_presenter_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&g_presenter_swap3))) && g_presenter_swap3) {
            if (g_presenter_format == DXGI_FORMAT_R10G10B10A2_UNORM)
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            else if (g_presenter_format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            else
                g_presenter_swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }

        for (auto &slot : g_presenter_slots) {
            if (!presenter_worker_open_shared(slot)) {
                g_presenter_status.store("presenter failed to open shared textures", std::memory_order_relaxed);
                return false;
            }
        }

        g_presenter_status.store(g_settings.output_mode == 3 ? "ready - V4 zero-copy compositor" : "ready - V3 separate device/thread", std::memory_order_relaxed);
        return true;
    }

    void presenter_worker_cleanup()
    {
        for (auto &slot : g_presenter_slots)
            presenter_release_slot_worker(slot);
        g_presenter_frame_latency = nullptr;
        g_presenter_waitable_active.store(false, std::memory_order_relaxed);
        release(g_presenter_swap3);
        release(g_presenter_swap2);
        release(g_presenter_swap);
        release(g_present_ctx);
        release(g_present_device);

        HWND hwnd = g_presenter_hwnd.exchange(nullptr, std::memory_order_acq_rel);
        if (hwnd)
            DestroyWindow(hwnd); // worker owns the HWND and destroys it on the same thread
    }

    double presenter_output_interval()
    {
        double native = g_presenter_native_interval.load(std::memory_order_relaxed);
        if (!(native > 0.0001 && native < 0.5))
            native = 1.0 / 60.0;
        double target_fps = 2.0 / native;
        const float refresh_hz = g_presenter_refresh_hz.load(std::memory_order_relaxed);
        // V3 silently clamped x2 to the refresh value returned by EnumDisplaySettings. On systems
        // where Windows/VRR reports 60 here, that line alone turns a 60->120 target back into 60.
        // V4 reports refresh as telemetry but does NOT clamp unless the user explicitly requests it.
        if (g_settings.presenter_cap_to_refresh && refresh_hz > 1.0f)
            target_fps = std::min(target_fps, static_cast<double>(refresh_hz));
        target_fps = std::max(1.0, target_fps);
        g_presenter_target_fps.store(static_cast<float>(target_fps), std::memory_order_relaxed);
        return 1.0 / target_fps;
    }

    void presenter_wait_slot(double interval)
    {
        if (!g_presenter_self_pacing_runtime.load(std::memory_order_relaxed))
            return;
        const double now = fg::now_seconds();
        if (g_presenter_next_slot <= 0.0 ||
            g_presenter_next_slot < now - interval * 2.0 ||
            g_presenter_next_slot > now + interval * 4.0)
            g_presenter_next_slot = now;
        fg::wait_until(g_presenter_next_slot, std::max(0.002, interval * 2.0));
        g_presenter_next_slot += interval;
    }

    bool presenter_worker_acquire(IDXGIKeyedMutex *mutex)
    {
        if (!mutex)
            return false;
        HRESULT hr = mutex->AcquireSync(1u, 100u); // worker may wait; game thread never does
        if (FAILED(hr)) {
            g_presenter_sync_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void presenter_worker_return(IDXGIKeyedMutex *mutex)
    {
        if (mutex)
            mutex->ReleaseSync(0u);
    }

    bool presenter_wait_for_output_slot()
    {
        if (g_settings.output_mode != 3 || !g_presenter_waitable_active.load(std::memory_order_relaxed) ||
            !g_presenter_frame_latency || g_presenter_frame_latency == INVALID_HANDLE_VALUE)
            return true;
        // WAIT_IO_COMPLETION does NOT mean the swapchain slot is signaled. Keep waiting instead of
        // accidentally overfilling the queue after an APC completion.
        for (;;) {
            DWORD wr = WaitForSingleObjectEx(g_presenter_frame_latency, 100u, TRUE);
            if (wr == WAIT_OBJECT_0)
                return true;
            if (wr == WAIT_IO_COMPLETION)
                continue;
            g_presenter_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
            return wr != WAIT_FAILED;
        }
    }

    bool presenter_copy_and_present(ID3D11Texture2D *src, IDXGIKeyedMutex *mutex, bool generated)
    {
        if (!src || !mutex || !g_presenter_swap || !g_present_ctx)
            return false;
        if (!presenter_wait_for_output_slot())
            return false;
        if (!presenter_worker_acquire(mutex))
            return false;

        ID3D11Texture2D *bb = nullptr;
        const UINT back_index = g_presenter_swap3 ? g_presenter_swap3->GetCurrentBackBufferIndex() : 0u;
        HRESULT hr = g_presenter_swap->GetBuffer(back_index, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb));
        if (SUCCEEDED(hr) && bb) {
            g_present_ctx->CopyResource(bb, src);
            bb->Release();
        }

        // Release key 0 after the present-device copy has been queued. The keyed mutex orders the
        // cross-device GPU work, so the game cannot overwrite this texture before our copy is done.
        presenter_worker_return(mutex);
        if (FAILED(hr))
            return false;

        UINT present_flags = g_presenter_swap_tearing_enabled.load(std::memory_order_relaxed) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        g_inside_presenter_present = true;
        hr = g_presenter_swap->Present(0u, present_flags);
        // Some composed/window states reject ALLOW_TEARING even though the adapter reports support.
        // Retry without the flag rather than killing the output stream.
        if (FAILED(hr) && present_flags != 0u)
            hr = g_presenter_swap->Present(0u, 0u);
        g_inside_presenter_present = false;
        g_presenter_last_hr.store(hr, std::memory_order_relaxed);
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
            if (generated) {
                g_presenter_generated_presents.fetch_add(1, std::memory_order_relaxed);
                fg::g_extra_presents.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_presenter_real_presents.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        return false;
    }

    void presenter_worker_discard(PresenterSlot &slot)
    {
        // Overlay open/unfocused: return shared ownership without ever touching the game's context.
        if (slot.generated_ready && presenter_worker_acquire(slot.generated_present_mutex))
            presenter_worker_return(slot.generated_present_mutex);
        if (slot.real_ready && presenter_worker_acquire(slot.real_present_mutex))
            presenter_worker_return(slot.real_present_mutex);
    }

    void presenter_thread_main()
    {
        const bool initialized = presenter_worker_init();
        g_presenter_worker_ready.store(initialized, std::memory_order_release);
        g_presenter_worker_failed.store(!initialized, std::memory_order_release);
        if (!initialized) {
            presenter_worker_cleanup();
            return;
        }

        for (;;) {
            presenter_worker_pump_messages();
            const bool visible = presenter_worker_sync_window();
            int idx = -1;
            {
                std::unique_lock<std::mutex> lock(g_presenter_mutex);
                g_presenter_cv.wait_for(lock, std::chrono::milliseconds(2), [] {
                    return g_presenter_stop || !g_presenter_ready.empty();
                });
                if (g_presenter_stop)
                    break;
                if (!g_presenter_ready.empty()) {
                    idx = g_presenter_ready.front();
                    g_presenter_ready.pop_front();
                    g_presenter_slots[idx].ready = false;
                    g_presenter_slots[idx].in_use = true;
                }
            }
            if (idx < 0)
                continue;

            PresenterSlot &slot = g_presenter_slots[idx];
            if (!visible) {
                presenter_worker_discard(slot);
            } else {
                const double out_interval = presenter_output_interval();
                if (slot.warmup_only || !slot.generated_ready) {
                    // A failed generated draw may still have transferred keyed-mutex ownership to
                    // key 1. Drain/return it even though we are not displaying it.
                    if (slot.generated_ready && presenter_worker_acquire(slot.generated_present_mutex))
                        presenter_worker_return(slot.generated_present_mutex);
                    if (slot.real_ready)
                        presenter_copy_and_present(slot.real_present, slot.real_present_mutex, false);
                    g_presenter_next_slot = fg::now_seconds() + std::max(out_interval, g_presenter_native_interval.load(std::memory_order_relaxed));
                } else {
                    presenter_wait_slot(out_interval);
                    presenter_copy_and_present(slot.generated_present, slot.generated_present_mutex, true);
                    presenter_wait_slot(out_interval);
                    if (slot.real_ready)
                        presenter_copy_and_present(slot.real_present, slot.real_present_mutex, false);
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_presenter_mutex);
                slot.in_use = false;
                slot.generated_ready = false;
                slot.real_ready = false;
            }
        }

        // Return ownership for packets that were queued when shutdown began, then tear down all
        // presenter-side objects on the same thread that created them.
        for (;;) {
            int idx = -1;
            {
                std::lock_guard<std::mutex> lock(g_presenter_mutex);
                if (g_presenter_ready.empty())
                    break;
                idx = g_presenter_ready.front();
                g_presenter_ready.pop_front();
                g_presenter_slots[idx].ready = false;
                g_presenter_slots[idx].in_use = true;
            }
            presenter_worker_discard(g_presenter_slots[idx]);
            std::lock_guard<std::mutex> lock(g_presenter_mutex);
            g_presenter_slots[idx].in_use = false;
            g_presenter_slots[idx].generated_ready = false;
            g_presenter_slots[idx].real_ready = false;
        }
        presenter_worker_cleanup();
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
        g_presenter_worker_ready.store(false, std::memory_order_relaxed);
        g_presenter_worker_failed.store(false, std::memory_order_relaxed);
        g_presenter_force_hide.store(true, std::memory_order_relaxed);
        g_presenter_effect_runtime = nullptr;

        std::lock_guard<std::mutex> lock(g_presenter_mutex);
        presenter_release_game_resources();
        g_presenter_status.store("off", std::memory_order_relaxed);
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

        bool rebuild = !g_presenter_running || g_presenter_device != fg::g_dev ||
                       g_presenter_game_hwnd != hwnd ||
                       g_presenter_width != d.Width || g_presenter_height != d.Height ||
                       g_presenter_format != d.Format;
        if (rebuild) {
            if (g_presenter_running)
                presenter_stop();
            if (!presenter_prepare_game_resources(hwnd, d.Width, d.Height, d.Format))
                return false;
            g_presenter_stop = false;
            g_presenter_force_hide.store(false, std::memory_order_relaxed);
            g_presenter_thread = std::thread(presenter_thread_main);
            g_presenter_running = true;
        }
        return !g_presenter_worker_failed.load(std::memory_order_acquire);
    }

    int presenter_acquire_slot()
    {
        std::lock_guard<std::mutex> lock(g_presenter_mutex);
        for (int i = 0; i < 3; ++i) {
            if (!g_presenter_slots[i].ready && !g_presenter_slots[i].in_use)
                return i;
        }
        // Stability-first policy: never steal a queued/in-flight slot. Dropping a new synthetic
        // packet is safe; reusing a resource before the worker returned key 0 is not.
        g_presenter_dropped_packets.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    void presenter_enqueue_slot(int idx, bool warmup_only, bool generated_ready, unsigned long long serial)
    {
        if (idx < 0 || idx >= 3)
            return;
        {
            std::lock_guard<std::mutex> lock(g_presenter_mutex);
            PresenterSlot &slot = g_presenter_slots[idx];
            slot.warmup_only = warmup_only;
            slot.generated_ready = generated_ready;
            slot.real_ready = true;
            slot.serial = serial;
            slot.ready = true;
            g_presenter_ready.push_back(idx);
        }
        g_presenter_cv.notify_one();
    }

    void run_independent_presenter(reshade::api::effect_runtime *runtime)
    {
        if (fg::g_inside_extra_present || g_inside_presenter_present)
            return;
        const unsigned long long frame_index = fg::g_real_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!fg::g_settings.enabled) {
            fg::g_status = "disabled";
            fg::g_have_prev = false;
            g_presenter_force_hide.store(true, std::memory_order_relaxed);
            return;
        }
        g_presenter_force_hide.store(false, std::memory_order_relaxed);
        g_presenter_self_pacing_runtime.store(g_settings.presenter_self_pacing, std::memory_order_relaxed);
        g_presenter_hide_unfocused_runtime.store(g_settings.presenter_hide_unfocused, std::memory_order_relaxed);

        reshade::api::device *api_device = runtime ? runtime->get_device() : nullptr;
        if (!api_device || api_device->get_api() != reshade::api::device_api::d3d11) {
            g_presenter_status.store("independent presenter is DX11-only", std::memory_order_relaxed);
            run_original_with_output_mode(runtime);
            return;
        }

        ID3D11Device *dev = reinterpret_cast<ID3D11Device *>(api_device->get_native());
        if (!dev || !fg::ensure_device(dev)) {
            g_presenter_status.store("D3D11 pipeline unavailable", std::memory_order_relaxed);
            return;
        }
        reshade::api::resource br = runtime->get_current_back_buffer();
        ID3D11Texture2D *bb = reinterpret_cast<ID3D11Texture2D *>(br.handle);
        if (!bb || !fg::ensure_resources(bb)) {
            g_presenter_status.store("backbuffer/resources unavailable", std::memory_order_relaxed);
            return;
        }
        if (!presenter_start_or_rebuild(runtime, bb))
            return;

        // Wait for the worker to finish its separate-device/swapchain setup without blocking the
        // game thread. Until then we simply maintain the original capture history.
        const bool worker_ready = g_presenter_worker_ready.load(std::memory_order_acquire);

        if (g_settings.reuse_legacy_postprocess && fg::g_settings.aa)
            fg::apply_aa(bb);
        fg::g_ctx->CopyResource(fg::g_curr_tex, bb);

        bool flow_ready = false;
        if (g_settings.reuse_legacy_postprocess && fg::g_settings.accumulate && fg::g_have_prev && fg::g_settings.debug_mode == 0) {
            fg::apply_accumulator(bb);
            fg::g_ctx->CopyResource(fg::g_curr_tex, bb);
            flow_ready = true;
        }

        double cb_entry = fg::now_seconds();
        if (fg::g_prev_cb_entry > 0.0) {
            double native = cb_entry - fg::g_prev_cb_entry;
            if (native > 0.0001 && native < 0.5)
                fg::g_dt_ema = (fg::g_dt_ema > 0.0) ? (fg::g_dt_ema * 0.9 + native * 0.1) : native;
        }
        fg::g_prev_cb_entry = cb_entry;
        if (fg::g_dt_ema > 0.0001 && fg::g_dt_ema < 0.5) {
            g_presenter_native_interval.store(fg::g_dt_ema, std::memory_order_relaxed);
            g_presenter_game_fps.store(static_cast<float>(1.0 / fg::g_dt_ema), std::memory_order_relaxed);
        }
        fg::g_last_wait_total = 0.0;
        fg::g_paced_ms = 0.0;

        if (worker_ready) {
            int slot_idx = presenter_acquire_slot();
            if (slot_idx >= 0) {
                PresenterSlot &slot = g_presenter_slots[slot_idx];
                bool real_locked = slot.real_game_mutex && SUCCEEDED(slot.real_game_mutex->AcquireSync(0u, 0u));
                if (real_locked) {
                    fg::g_ctx->CopyResource(slot.real, fg::g_curr_tex);
                    slot.real_game_mutex->ReleaseSync(1u);

                    const bool warmup = !fg::g_have_prev;
                    bool generated_locked = false;
                    bool generated_ok = false;
                    if (!warmup && fg::g_settings.debug_mode == 0 && slot.generated_game_mutex &&
                        SUCCEEDED(slot.generated_game_mutex->AcquireSync(0u, 0u))) {
                        generated_locked = true;
                        generated_ok = fg::draw_interpolated(slot.generated, 0.5f, !flow_ready);
                        slot.generated_game_mutex->ReleaseSync(1u);
                        if (generated_ok)
                            fg::g_gen_frames.fetch_add(1, std::memory_order_relaxed);
                    }
                    // If a generated texture was handed to key 1 but drawing failed, the worker
                    // still drains it before freeing the slot so ownership cannot get stranded.
                    presenter_enqueue_slot(slot_idx, warmup || !generated_ok, generated_locked, frame_index);
                } else {
                    g_presenter_sync_misses.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        std::swap(fg::g_prev_tex, fg::g_curr_tex);
        std::swap(fg::g_prev_srv, fg::g_curr_srv);
        fg::g_have_prev = true;
        fg::g_status = (g_settings.output_mode == 3) ? "zero-copy compositor V4" : "independent additive presenter V3";
        if (worker_ready)
            g_presenter_status.store(g_settings.output_mode == 3 ? "running - V4 zero-copy compositor" : "running - V3 separate device/thread", std::memory_order_relaxed);
    }

    void run_original_with_output_mode(reshade::api::effect_runtime *runtime)
    {
        // V5 keeps the existing interpolation and backbuffer-restore code, but changes the
        // scheduling contract. The game swapchain is deepened in on_create_swapchain and forced
        // to FIFO/vblank. We emit one generated Present with no software sleep and then return;
        // the game's untouched real Present becomes the next image in that same native queue.
        const bool legacy_immediate = (g_settings.output_mode == 1) && fg::g_settings.extra_present;
        const bool deep_queue_requested = (g_settings.output_mode == 4);
        const bool deep_queue_ready = deep_queue_requested && g_deep_queue_patched.load(std::memory_order_acquire);

        const bool saved_extra = fg::g_settings.extra_present;
        const bool saved_pace = fg::g_settings.pace;
        const int saved_sync = fg::g_settings.present_sync;
        const int saved_multiplier = fg::g_settings.multiplier;

        if (legacy_immediate) {
            fg::g_settings.pace = false;
            fg::g_settings.present_sync = 0;
        }
        else if (deep_queue_ready) {
            fg::g_settings.extra_present = true;
            fg::g_settings.pace = false;
            fg::g_settings.present_sync = g_settings.deep_queue_force_vsync ? 1 : 0;
            fg::g_settings.multiplier = 2;
            fg::g_slot = 0.0;
            fg::g_last_wait_total = 0.0;
            fg::g_paced_ms = 0.0;
        }
        else if (deep_queue_requested) {
            // Never fall back to the old halve/refill path if the current swapchain was not
            // created with the deep-queue hook. Keep history warm and request a recreation.
            fg::g_settings.extra_present = false;
            fg::g_settings.pace = false;
        }

        fg::run(runtime);

        if (deep_queue_requested && !deep_queue_ready)
            fg::g_status = "V5 waiting for patched game swapchain - restart or recreate swapchain";

        fg::g_settings.extra_present = saved_extra;
        fg::g_settings.pace = saved_pace;
        fg::g_settings.present_sync = saved_sync;
        fg::g_settings.multiplier = saved_multiplier;
    }

    void run(reshade::api::effect_runtime *runtime)
    {
        // Preserve the existing implementation byte-for-byte when Original is selected.
        if (!wants_external_backend()) {
            g_extension_status = "Original optical flow (unchanged)";
            if (g_settings.output_mode == 2 || g_settings.output_mode == 3)
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
            if (g_settings.output_mode == 2 || g_settings.output_mode == 3)
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
            if (g_settings.output_mode == 2 || g_settings.output_mode == 3)
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
        if (g_settings.output_mode == 2 || g_settings.output_mode == 3)
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
            "TRUE ADDITIVE V3: isolated presenter (A/B only)",
            "FRANKENSTEIN V4: zero-copy fullscreen compositor",
            "FRANKENSTEIN V5: native deep swapchain queue (recommended)"
        };
        int old_output_mode = g_settings.output_mode;
        if (ImGui::Combo("Output backend", &g_settings.output_mode, output_items, 5)) {
            const bool old_presenter = (old_output_mode == 2 || old_output_mode == 3);
            const bool new_presenter = (g_settings.output_mode == 2 || g_settings.output_mode == 3);
            if (old_presenter && old_output_mode != g_settings.output_mode)
                presenter_stop();
            if ((!old_presenter && new_presenter) || (old_presenter && old_output_mode != g_settings.output_mode))
                g_reshade_overlay_open.store(true, std::memory_order_release);
            fg::g_slot = 0.0;
            fg::g_last_wait_total = 0.0;
            fg::g_paced_ms = 0.0;
            fg::g_last_present_ts = 0.0;
            g_presenter_next_slot = 0.0;
            if (g_settings.output_mode == 4 && !g_deep_queue_patched.load(std::memory_order_acquire))
                fg::g_status = "V5 selected - recreate/restart swapchain so deep queue hook can apply";
            else if (old_output_mode == 4 && g_settings.output_mode != 4)
                fg::g_status = "Left V5 - recreate/restart swapchain to restore the game's original Present contract";
        }

        if (g_settings.output_mode == 4) {
            bool recreate_needed = false;
            if (ImGui::Checkbox("Force FIFO / VSync=1 on game swapchain", &g_settings.deep_queue_force_vsync)) recreate_needed = true;
            if (ImGui::Checkbox("Raise exclusive-fullscreen refresh to desktop refresh", &g_settings.deep_queue_raise_fullscreen_refresh)) recreate_needed = true;
            if (ImGui::Checkbox("Experimental: promote BLT swapchain to flip-model", &g_settings.deep_queue_force_flip_model)) recreate_needed = true;
            if (ImGui::SliderInt("Extra native swapchain buffers", &g_settings.deep_queue_extra_buffers, 1, 4)) recreate_needed = true;
            if (recreate_needed) {
                g_deep_queue_patched.store(false, std::memory_order_release);
                fg::g_status = "V5 swapchain settings changed - restart/resize/toggle display mode";
            }
            ImGui::TextDisabled("V5 scavenges lsfg-vk's scheduling trick: deepen the GAME swapchain, then queue generated + real frames into one FIFO/vblank stream.");
            ImGui::TextDisabled("It forces x2 + Extra Present internally and bypasses the old software Pace wait; FrameGen Preview values are restored when leaving V5.");
            ImGui::Text("Native swapchain patch: %s", g_deep_queue_patched.load(std::memory_order_acquire) ? "ACTIVE" : "NOT APPLIED - RECREATE/RESTART");
            ImGui::Text("Backbuffers original/actual: %u -> %u | forced SyncInterval: %s",
                g_deep_queue_original_buffers.load(std::memory_order_relaxed),
                g_deep_queue_created_buffers.load(std::memory_order_relaxed),
                g_deep_queue_forced_sync.load(std::memory_order_relaxed) == 1u ? "1 (FIFO/vblank)" : "application/default");
            const uint32_t original_mode = g_deep_queue_original_present_mode.load(std::memory_order_relaxed);
            const uint32_t active_mode = g_deep_queue_created_present_mode.load(std::memory_order_relaxed);
            ImGui::Text("Present model: %s -> %s%s",
                dxgi_present_mode_name(original_mode), dxgi_present_mode_name(active_mode),
                g_deep_queue_flip_promoted.load(std::memory_order_relaxed) ? " (promoted)" : "");
            if (!dxgi_present_mode_is_flip(active_mode))
                ImGui::TextDisabled("BLT-model detected. If V5 still collapses to game FPS, enable experimental flip promotion and restart.");
            ImGui::Text("DXGI max-frame-latency expansion: %s (HR 0x%08lX)",
                g_deep_queue_latency_applied.load(std::memory_order_acquire) ? "ACTIVE" : "not applied",
                static_cast<unsigned long>(g_deep_queue_latency_hr.load(std::memory_order_relaxed)));
            ImGui::Text("Submitted native frames: %llu | submitted output stream: %.1f FPS",
                g_deep_queue_present_count.load(std::memory_order_relaxed),
                g_deep_queue_output_fps.load(std::memory_order_relaxed));
            ImGui::TextDisabled("That submitted stream counts BOTH our generated Present and the game's real-frame callback cadence; use it instead of an in-game counter that only reports real frames.");
            ImGui::TextDisabled("A physical 60 Hz desktop cannot show 120 unique frames. 60 real -> 120 visible needs Windows/monitor output at 120 Hz or higher.");
        }
        else if (g_settings.output_mode == 2 || g_settings.output_mode == 3) {
            ImGui::Checkbox("Self-pace generated output", &g_settings.presenter_self_pacing);
            if (ImGui::Checkbox("Allow tearing for Present(0) when supported", &g_settings.presenter_allow_tearing))
                presenter_stop();
            if (g_settings.output_mode == 3) {
                if (ImGui::Checkbox("Use DXGI frame-latency waitable object", &g_settings.presenter_waitable_swapchain))
                    presenter_stop();
                ImGui::Checkbox("Cap x2 target to detected refresh (normally OFF)", &g_settings.presenter_cap_to_refresh);
                ImGui::Checkbox("Keep compositor topmost", &g_settings.presenter_force_topmost);
            }
            ImGui::Checkbox("Reuse legacy AA / temporal reconstruction", &g_settings.reuse_legacy_postprocess);
            ImGui::Checkbox("Hide presenter when game is unfocused", &g_settings.presenter_hide_unfocused);
            ImGui::TextDisabled("Presenter backends hard-bypass legacy Extra Present / Pace / injected-vblank controls.");
            if (g_settings.output_mode == 3)
                ImGui::TextDisabled("V4 is the FreeGen/Magpie-style fallback: a top-level zero-copy compositor owns visible output.");
            else
                ImGui::TextDisabled("V3 is retained only for regression/A-B testing; V5 is the primary path now.");
            ImGui::Text("Presenter: %s", g_presenter_status.load(std::memory_order_relaxed));
            ImGui::Text("Game real: %.1f FPS | target output: %.1f FPS | actual output: %.1f FPS",
                g_presenter_game_fps.load(std::memory_order_relaxed),
                g_presenter_target_fps.load(std::memory_order_relaxed),
                g_presenter_output_fps.load(std::memory_order_relaxed));
            ImGui::Text("Display refresh: %.1f Hz | tearing: %s",
                g_presenter_refresh_hz.load(std::memory_order_relaxed),
                g_presenter_tearing_supported.load(std::memory_order_relaxed) ? "supported" : "unavailable");
            ImGui::Text("Waitable swapchain: %s | waits timed out: %llu | last Present HR: 0x%08lX",
                g_presenter_waitable_active.load(std::memory_order_relaxed) ? "ACTIVE" : "off",
                g_presenter_wait_timeouts.load(std::memory_order_relaxed),
                static_cast<unsigned long>(g_presenter_last_hr.load(std::memory_order_relaxed)));
            ImGui::Text("Presenter real/gen: %llu / %llu | dropped packets: %llu | sync misses: %llu",
                g_presenter_real_presents.load(), g_presenter_generated_presents.load(), g_presenter_dropped_packets.load(), g_presenter_sync_misses.load());
        }
        else if (g_settings.output_mode == 1) {
            ImGui::TextDisabled("Old experimental no-wait mode: preserves game thread but generated/real Presents can land back-to-back.");
        }
        else {
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
            static const char *out_names[] = { "Legacy paced", "Legacy immediate", "V3 isolated presenter", "V4 zero-copy compositor", "V5 native deep queue" };
            ImGui::Text("Output mode: %s", out_names[std::clamp(g_settings.output_mode, 0, 4)]);
            ImGui::TextDisabled("Uses the community UV-space texMotionVectors contract (qUINT / ReShadeMotionEstimation / Feeder ecosystem).");
            ImGui::TextDisabled("No NVIDIA DLLs are bundled. DLSS/NR effects may still run normally before reshade_present; this addon then interpolates the resulting real frames.");
            ImGui::TextDisabled("DX12 keeps the existing D3D11On12 backend and falls back to Original motion for now.");
        }
    }

    void shutdown()
    {
        presenter_stop();
        g_presenter_effect_runtime = nullptr;
        release_external_view(true);
        release_extension_pipeline();
    }

    bool on_reshade_open_overlay(reshade::api::effect_runtime *, bool open, reshade::api::input_source)
    {
        // Do not call ShowWindow/SetWindowPos from ReShade's game/UI thread. The presenter HWND is
        // owned by its worker; merely publish visibility state and wake that thread.
        g_reshade_overlay_open.store(open, std::memory_order_release);
        g_presenter_cv.notify_all();
        return false;
    }

    void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        if (!runtime)
            return;
        const HWND hwnd = reinterpret_cast<HWND>(runtime->get_hwnd());

        if (g_settings.output_mode == 4 && hwnd && hwnd == g_deep_queue_hwnd.load(std::memory_order_acquire)) {
            reshade::api::device *api_device = runtime->get_device();
            if (api_device && api_device->get_api() == reshade::api::device_api::d3d11) {
                ID3D11Device *d3d11 = reinterpret_cast<ID3D11Device *>(api_device->get_native());
                IDXGIDevice1 *dxgi1 = nullptr;
                HRESULT hr = d3d11 ? d3d11->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void **>(&dxgi1)) : E_NOINTERFACE;
                if (SUCCEEDED(hr) && dxgi1) {
                    // DXGI defaults can throttle the producer before our deeper BufferCount matters.
                    // Let at least the native queue depth exist; this intentionally trades one
                    // frame of latency for additive output, just like the universal FG donors.
                    const UINT latency = std::clamp<UINT>(g_deep_queue_created_buffers.load(std::memory_order_relaxed), 3u, 8u);
                    hr = dxgi1->SetMaximumFrameLatency(latency);
                    dxgi1->Release();
                }
                g_deep_queue_latency_hr.store(hr, std::memory_order_relaxed);
                g_deep_queue_latency_applied.store(SUCCEEDED(hr), std::memory_order_release);
            }
        }

        HWND presenter_hwnd = g_presenter_hwnd.load(std::memory_order_acquire);
        if (presenter_hwnd && hwnd == presenter_hwnd) {
            // The presenter's own swapchain may be seen by ReShade as a second runtime. Never run
            // the user's effects (especially RenoDX/DLSS NR) on it a second time. The queued real
            // and generated textures already contain the desired game-side effect result.
            g_presenter_effect_runtime = runtime;
            runtime->set_effects_state(false);
        }
    }

    void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
    {
        if (runtime == g_presenter_effect_runtime) {
            g_presenter_effect_runtime = nullptr;
            return;
        }
        // Tear the worker down while the primary game runtime is still alive.
        if (!runtime || !g_presenter_game_hwnd || reinterpret_cast<HWND>(runtime->get_hwnd()) == g_presenter_game_hwnd)
            presenter_stop();
    }

    void on_present(reshade::api::effect_runtime *runtime)
    {
        if (!runtime || g_inside_presenter_present)
            return;
        const HWND hwnd = reinterpret_cast<HWND>(runtime->get_hwnd());
        HWND presenter_hwnd = g_presenter_hwnd.load(std::memory_order_acquire);
        if (presenter_hwnd && hwnd == presenter_hwnd)
            return;
        if (g_settings.output_mode == 4) {
            const HWND primary = g_deep_queue_hwnd.load(std::memory_order_acquire);
            if (primary && hwnd != primary)
                return;
        }
        run(runtime);
        update_deep_queue_telemetry(runtime);
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
        reshade::register_event<reshade::addon_event::create_swapchain>(&fgx::on_create_swapchain);
        reshade::register_event<reshade::addon_event::init_swapchain>(&fgx::on_init_swapchain);
        reshade::register_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(&fgx::on_reshade_open_overlay);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&fgx::on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&fgx::on_destroy_effect_runtime);
        reshade::register_overlay("Hybrid Motion + Output", &fgx::draw_overlay);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::create_swapchain>(&fgx::on_create_swapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(&fgx::on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(&fgx::on_reshade_open_overlay);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&fgx::on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&fgx::on_destroy_effect_runtime);
        fgx::shutdown();
        // Original cleanup owns the rest of the pipeline/events/add-on registration.
        return ReShadeFrameGen_OriginalDllMain(hinstDLL, reason, reserved);
    }
    return TRUE;
}
