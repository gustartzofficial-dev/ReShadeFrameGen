#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>
#include <reshade.hpp>

#include "dependency_probe.hpp"
#include "feeder_probe.hpp"
#include "streamline_host.hpp"

extern "C" __declspec(dllexport) const char *NAME = "ReShade FrameGen - DLSS-G Host 0.1";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Clean-slate DLSS Frame Generation controller for ReShade. Does not synthesize or inject frames itself; "
    "it attaches to an already initialized Streamline/RenoDX host and controls NVIDIA DLSS-G on the presenting thread.";

namespace
{
std::atomic<reshade::api::effect_runtime *> g_primary_runtime{nullptr};
std::atomic<uint64_t> g_primary_area{0};
reshade::api::device_api g_api = reshade::api::device_api::d3d11;
bool g_api_known = false;

uint64_t runtime_area(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr)
        return 0;
    const HWND hwnd = static_cast<HWND>(runtime->get_hwnd());
    if (hwnd == nullptr)
        return 0;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return 0;
    const uint64_t w = static_cast<uint64_t>(std::max<LONG>(0, rc.right - rc.left));
    const uint64_t h = static_cast<uint64_t>(std::max<LONG>(0, rc.bottom - rc.top));
    return w * h;
}

bool consider_primary_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr)
        return false;

    const uint64_t area = runtime_area(runtime);
    auto *current = g_primary_runtime.load(std::memory_order_acquire);
    const uint64_t current_area = g_primary_area.load(std::memory_order_relaxed);

    if (current == nullptr || area > current_area)
    {
        g_primary_runtime.store(runtime, std::memory_order_release);
        g_primary_area.store(area, std::memory_order_relaxed);
        current = runtime;
    }
    return current == runtime;
}

const char *api_name(reshade::api::device_api api)
{
    if (!g_api_known)
        return "Unknown";

    switch (api)
    {
    case reshade::api::device_api::d3d11: return "D3D11";
    case reshade::api::device_api::d3d12: return "D3D12";
    case reshade::api::device_api::vulkan: return "Vulkan";
    case reshade::api::device_api::opengl: return "OpenGL";
    default: return "Unknown";
    }
}

void status_bit(const char *text, bool ok)
{
    if (ok)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "[OK] %s", text);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "[--] %s", text);
}

bool has_status(sl::DLSSGStatus status, sl::DLSSGStatus bit)
{
    return (static_cast<uint32_t>(status) & static_cast<uint32_t>(bit)) != 0;
}

void draw_dlssg_failures(sl::DLSSGStatus status)
{
    if (status == sl::DLSSGStatus::eOk)
    {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "DLSS-G runtime status: OK");
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "DLSS-G runtime status: 0x%08X", static_cast<uint32_t>(status));
    if (has_status(status, sl::DLSSGStatus::eFailResolutionTooLow))
        ImGui::BulletText("Output resolution is too low for DLSS-G.");
    if (has_status(status, sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime))
        ImGui::BulletText("Streamline Reflex markers are missing/not active.");
    if (has_status(status, sl::DLSSGStatus::eFailHDRFormatNotSupported))
        ImGui::BulletText("Current HDR/backbuffer format is unsupported.");
    if (has_status(status, sl::DLSSGStatus::eFailCommonConstantsInvalid))
        ImGui::BulletText("Common constants / camera contract is invalid or missing.");
    if (has_status(status, sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled))
        ImGui::BulletText("The host did not use GetCurrentBackBufferIndex as required.");
}

void draw_overlay(reshade::api::effect_runtime *)
{
    const fg::deps::Snapshot d = fg::deps::probe();
    const fg::guides::Snapshot guides = fg::guides::snapshot();
    const fg::slhost::Snapshot sls = fg::slhost::snapshot();

    ImGui::TextUnformatted("Clean-slate NVIDIA path");
    ImGui::TextDisabled("No legacy optical flow. No extra Present. No secondary swapchain.");
    ImGui::Separator();

    ImGui::Text("Game API: %s", api_name(g_api));
    ImGui::Text("Primary ReShade surface: %s", g_primary_runtime.load(std::memory_order_acquire) != nullptr ? "selected" : "waiting");
    status_bit("sl.interposer.dll loaded", d.streamline_loaded);
    status_bit("sl.dlss_g.dll loaded", d.sl_dlssg_loaded);
    status_bit("sl.reflex.dll loaded", d.sl_reflex_loaded);
    status_bit("nvngx_dlssg.dll loaded", d.ngx_dlssg_loaded);
    status_bit("DLSS-G feature loaded by Streamline", sls.feature_loaded);
    status_bit("DLSS-G SetOptions/GetState resolved", sls.feature_functions_ready);

    if (g_api_known && g_api == reshade::api::device_api::d3d11)
    {
        status_bit("ShortFuse renodx-dlss.addon64 loaded (D3D11 host)", d.renodx_dlss_loaded);
        if (!d.renodx_dlss_loaded && d.renodx_dlss_file)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "renodx-dlss.addon64 exists but is not loaded. Load it early from ReShade.ini and restart.");
    }

    if (d.renodx_dlss5_loaded && !d.renodx_dlss_loaded)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "Older renodx-dlss5.addon64 detected. That is the NR/NGX hook; it is not the D3D11 DLSS-G host this build expects.");

    if (d.feeder_loaded && d.renodx_dlss_loaded)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "dlss5-feed.addon64 is also loaded. Current Feeder documentation says 64-bit D3D11/12 should use renodx-dlss directly; disable the Feeder add-on while testing this host.");

    if (d.streamline_loaded && d.renodx_dlss_loaded && !sls.feature_loaded)
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                           "Streamline is initialized, but kFeatureDLSS_G is not loaded. This means the early host did not request DLSS-G during slInit (or the plugin failed to load). Version 0.1 will not try to initialize Streamline late; that would be unsafe. This result tells us the next rewrite step is an early slInit feature-injection/bootstrap hook.");

    ImGui::Separator();
    ImGui::TextUnformatted("Feeder-style guide contract (diagnostic only in 0.1)");
    status_bit("DLSS5_Feed.fx technique found", guides.effect_present);
    status_bit("DLSS5_MV texture found", guides.motion_vectors);
    status_bit("DLSS5_Depth texture found", guides.depth);
    if (guides.effect_present && !guides.effect_enabled)
        ImGui::TextDisabled("DLSS5_Feed.fx exists but the technique is disabled.");
    ImGui::TextDisabled("0.1 does not retag these resources itself; the early Streamline/RenoDX host owns the frame token, tags, constants and Reflex cadence.");

    ImGui::Separator();
    bool enabled = fg::slhost::requested_enabled();
    int multiplier = fg::slhost::requested_multiplier();

    ImGui::BeginDisabled(!sls.controller_ready);
    if (ImGui::Checkbox("Enable real NVIDIA DLSS Frame Generation", &enabled))
        fg::slhost::request_enabled(enabled);

    int max_multiplier = sls.max_generated_frames > 0 ? static_cast<int>(sls.max_generated_frames + 1) : 4;
    max_multiplier = std::clamp(max_multiplier, 2, 6);
    if (ImGui::SliderInt("Frame multiplier", &multiplier, 2, max_multiplier))
        fg::slhost::request_multiplier(multiplier);
    ImGui::EndDisabled();

    if (!sls.controller_ready)
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "Controller not armed: fix the red prerequisites above, then restart the game.");

    if (ImGui::Button("Re-probe Streamline"))
        fg::slhost::force_reprobe();

    ImGui::Separator();
    draw_dlssg_failures(sls.status);
    ImGui::Text("Max generated frames / real frame: %u", sls.max_generated_frames);
    ImGui::Text("App Present callbacks: %.1f fps", sls.app_present_fps);
    ImGui::Text("DLSS-G frames actually presented: %.1f fps", sls.output_fps);
    ImGui::Text("Frames in last DLSS-G poll: %u", sls.frames_presented_last_poll);
    ImGui::Text("SetOptions: %s", fg::slhost::result_name(sls.last_set_options));
    ImGui::Text("GetState: %s", fg::slhost::result_name(sls.last_get_state));
    ImGui::Text("Dynamic MFG support: %s", sls.dynamic_mfg_supported ? "yes" : "no");

    ImGui::Separator();
    ImGui::TextWrapped("What this build proves: if Streamline/RenoDX has already created the real DLSS-G presentation contract, this add-on controls NVIDIA DLSS-G itself. It never replaces game frames with home-made interpolation.");
}

void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (!consider_primary_runtime(runtime))
        return;

    if (runtime != nullptr && runtime->get_device() != nullptr)
    {
        g_api = runtime->get_device()->get_api();
        g_api_known = true;
    }
    fg::slhost::set_api_is_d3d11(g_api_known && g_api == reshade::api::device_api::d3d11);
    fg::guides::resolve(runtime);
    fg::slhost::force_reprobe();
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime != g_primary_runtime.load(std::memory_order_acquire))
        return;

    fg::guides::clear(runtime);
    g_primary_runtime.store(nullptr, std::memory_order_release);
    g_primary_area.store(0, std::memory_order_relaxed);
    g_api_known = false;
    fg::slhost::set_api_is_d3d11(false);
    fg::slhost::force_reprobe();
}

void on_reloaded_effects(reshade::api::effect_runtime *runtime)
{
    if (runtime == g_primary_runtime.load(std::memory_order_acquire))
        fg::guides::resolve(runtime);
}

void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    if (!consider_primary_runtime(runtime))
        return;

    if (runtime != nullptr && runtime->get_device() != nullptr)
    {
        g_api = runtime->get_device()->get_api();
        g_api_known = true;
        fg::slhost::set_api_is_d3d11(g_api == reshade::api::device_api::d3d11);
    }

    // Streamline DLSS-G SetOptions/GetState are not thread-safe with Present. Restrict all
    // controller calls to the selected primary ReShade runtime so secondary swapchains cannot
    // race the real presentation thread or contaminate the app-present telemetry.
    fg::slhost::present_tick();
}

}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        if (!reshade::register_addon(hinstDLL))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_overlay("DLSS-G Host", &draw_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_addon(hinstDLL);
        break;
    }
    return TRUE;
}
