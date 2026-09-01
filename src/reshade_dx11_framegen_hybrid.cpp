// ReShadeFrameGen Hybrid Motion extension
//
// This file deliberately compiles the existing implementation unchanged, then layers an
// optional texMotionVectors-backed flow/interpolation path on top. The original source remains
// in the repository and remains the default/fallback backend.

#define DllMain ReShadeFrameGen_OriginalDllMain
#include "reshade_dx11_framegen.cpp"
#undef DllMain

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
        bool preserve_native_fps = false; // additive output: never pace/wait the game's real Present
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

    void run_original_with_output_mode(reshade::api::effect_runtime *runtime)
    {
        // The original pacer intentionally holds the game's real Present so interpolated frames
        // can occupy evenly spaced slots. That is useful for smooth pacing, but on a vsync/capped
        // title the added wait can push the real Present past its next display slot and effectively
        // cut the native cadence in half. Additive mode keeps the old implementation available but
        // temporarily bypasses those waits: generated Presents are immediate and the real Present
        // is returned to the game as soon as our rendering work is finished.
        const bool override_output = g_settings.preserve_native_fps && fg::g_settings.extra_present;
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
            run_original_with_output_mode(runtime);
            return;
        }

        // First call (or a device rebuild) lets the original implementation initialize itself.
        // We then compile the extension against that exact D3D11 device for subsequent frames.
        if (!fg::g_pipeline_ready || !fg::g_dev || !fg::g_ctx) {
            // If the original pipeline was torn down (device switch / DX12 toggle), discard any
            // extension objects tied to the old D3D11 device before rebuilding.
            release_extension_pipeline();
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
        ImGui::Checkbox("Additive FG / preserve native FPS", &g_settings.preserve_native_fps);
        if (g_settings.preserve_native_fps) {
            ImGui::TextDisabled("Does not wait before the game's real Present. Generated Presents are immediate.");
            ImGui::TextDisabled("Best for recovering output FPS when a heavy effect lowers the real-frame rate.");
            ImGui::TextDisabled("The original 'Pace frames' setting is temporarily overridden, not changed.");
            if (fg::g_settings.multiplier > 2)
                ImGui::TextDisabled("Tip: x2 is recommended in additive mode; x3/x4 inject multiple frames back-to-back.");
        } else {
            ImGui::TextDisabled("Off = original paced behaviour from the FrameGen Preview tab.");
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
            ImGui::Text("Output mode: %s", g_settings.preserve_native_fps ? "Additive / preserve native FPS" : "Original paced output");
            ImGui::TextDisabled("Uses the community UV-space texMotionVectors contract (qUINT / ReShadeMotionEstimation / Feeder ecosystem).");
            ImGui::TextDisabled("No NVIDIA DLLs are bundled. DLSS/NR effects may still run normally before reshade_present; this addon then interpolates the resulting real frames.");
            ImGui::TextDisabled("DX12 keeps the existing D3D11On12 backend and falls back to Original motion for now.");
        }
    }

    void shutdown()
    {
        release_external_view(true);
        release_extension_pipeline();
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
        // Let the original source register everything it already owns first.
        if (!ReShadeFrameGen_OriginalDllMain(hinstDLL, reason, reserved))
            return FALSE;
        // Replace only its present callback with the dispatcher above. The dispatcher calls the
        // original fg::run directly whenever Original/fallback is active.
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        reshade::register_overlay("Hybrid Motion + Output", &fgx::draw_overlay);
        break;

    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::reshade_present>(&fgx::on_present);
        fgx::shutdown();
        // Original cleanup owns the rest of the pipeline/events/add-on registration.
        return ReShadeFrameGen_OriginalDllMain(hinstDLL, reason, reserved);
    }
    return TRUE;
}
