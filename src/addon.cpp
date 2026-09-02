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
#include <cstdio>
#include <string>

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>
#include <reshade.hpp>

#include "dependency_probe.hpp"
#include "feeder_probe.hpp"
#include "streamline_host.hpp"

extern "C" __declspec(dllexport) const char *NAME = "ReShade FrameGen - DLSS-G Host 0.3 D3D12 Endpoint";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Experimental real NVIDIA DLSS-G path for D3D11 games. Reuses DLSS5_Feed motion/depth, bridges the final D3D11 frame to a same-adapter D3D12 endpoint, and presents through a Streamline proxy swapchain.";

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
    return static_cast<uint64_t>(std::max<LONG>(0, rc.right - rc.left)) *
           static_cast<uint64_t>(std::max<LONG>(0, rc.bottom - rc.top));
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
        fg::slhost::on_primary_runtime(runtime);
    }
    return current == runtime;
}

const char *api_name(reshade::api::device_api api)
{
    if (!g_api_known) return "Unknown";
    switch (api)
    {
    case reshade::api::device_api::d3d11: return "D3D11";
    case reshade::api::device_api::d3d12: return "D3D12";
    case reshade::api::device_api::vulkan: return "Vulkan";
    case reshade::api::device_api::opengl: return "OpenGL";
    default: return "Unknown";
    }
}

void stage_line(const char *label, bool complete, bool applicable = true)
{
    if (!applicable)
        ImGui::TextDisabled("[ .. ] %s", label);
    else if (complete)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "[ OK ] %s", label);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "[WAIT] %s", label);
}

void draw_dll(const char *name, const fg::deps::DllState &dll)
{
    ImGui::Text("%s", name);
    ImGui::SameLine(190.0f);
    if (dll.found)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "FOUND");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "MISSING");
    ImGui::SameLine(270.0f);
    if (dll.loaded)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "LOADED");
    else
        ImGui::TextDisabled("not mapped yet");
    if (dll.found && !dll.path.empty())
    {
        const std::string path = fg::deps::narrow(dll.path);
        ImGui::TextDisabled("  %s", path.c_str());
    }
}

bool has_status(sl::DLSSGStatus status, sl::DLSSGStatus bit)
{
    return (static_cast<uint32_t>(status) & static_cast<uint32_t>(bit)) != 0;
}

void draw_dlssg_failures(const fg::slhost::Snapshot &s)
{
    if (!s.state_queried)
    {
        ImGui::TextDisabled("DLSS-G runtime status: not queried yet");
        return;
    }
    if (s.last_get_state != sl::Result::eOk)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "DLSS-G GetState: %s", fg::slhost::result_name(s.last_get_state));
        return;
    }
    if (s.status == sl::DLSSGStatus::eOk)
    {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "DLSS-G runtime status: OK");
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "DLSS-G runtime status: 0x%08X", static_cast<uint32_t>(s.status));
    if (has_status(s.status, sl::DLSSGStatus::eFailResolutionTooLow)) ImGui::BulletText("Output resolution too low.");
    if (has_status(s.status, sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) ImGui::BulletText("Reflex/PCL cadence is missing or out of sync.");
    if (has_status(s.status, sl::DLSSGStatus::eFailHDRFormatNotSupported)) ImGui::BulletText("Backbuffer/HDR format is unsupported by this first endpoint.");
    if (has_status(s.status, sl::DLSSGStatus::eFailCommonConstantsInvalid)) ImGui::BulletText("The generic first-test camera constants are not sufficient for this game.");
    if (has_status(s.status, sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)) ImGui::BulletText("The Streamline proxy swapchain/backbuffer-index contract is incomplete.");
}

void draw_result(const char *label, sl::Result result)
{
    ImGui::Text("%s: %s", label, fg::slhost::result_name(result));
}

void draw_overlay(reshade::api::effect_runtime *)
{
    const fg::deps::Snapshot d = fg::deps::probe();
    const fg::guides::Snapshot guides = fg::guides::snapshot();
    const fg::slhost::Snapshot s = fg::slhost::snapshot();

    ImGui::TextUnformatted("DLSS-G Host 0.3 - D3D11 -> D3D12 endpoint");
    ImGui::TextDisabled("First build that actually consumes DLSS5_Feed MV/depth and attempts a real NVIDIA DLSS-G proxy Present.");
    ImGui::Separator();

    ImGui::Text("Game API: %s", api_name(g_api));
    ImGui::Text("Primary ReShade surface: %s", g_primary_runtime.load(std::memory_order_acquire) ? "selected" : "waiting");
    if (g_api_known && g_api != reshade::api::device_api::d3d11)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "v0.3 endpoint test currently targets D3D11 games only.");

    ImGui::Separator();
    ImGui::TextUnformatted("1. Streamline bootstrap");
    stage_line("sl.interposer.dll found", s.interposer_found);
    stage_line("sl.interposer.dll loaded", s.interposer_loaded);
    stage_line("manual-hooking / frame-tagging exports resolved", s.core_exports_ready);
    stage_line("slInit(D3D12 + DLSS-G + Reflex + PCL)", s.streamline_initialized);
    if (!s.plugin_directory.empty())
    {
        const std::string plugin_dir = fg::deps::narrow(s.plugin_directory);
        ImGui::TextDisabled("Plugin path: %s", plugin_dir.c_str());
    }
    draw_result("slInit", s.last_init);

    ImGui::Separator();
    ImGui::TextUnformatted("2. Private same-GPU D3D12 DLSS-G endpoint");
    stage_line("D3D11 game device detected; primary surface selected", s.game_d3d11_seen && g_primary_runtime.load(std::memory_order_acquire) != nullptr);
    stage_line("same-adapter native D3D12 device created", s.endpoint_device_created);
    stage_line("D3D12 device submitted to Streamline", s.endpoint_device_submitted);
    stage_line("DLSS-G supported on this adapter", s.endpoint_feature_supported);
    stage_line("DLSS-G feature/plugin loaded", s.feature_loaded);
    stage_line("DLSS-G SetOptions/GetState resolved", s.feature_functions_ready);
    stage_line("Reflex function resolved", s.reflex_functions_ready);
    stage_line("PCL marker function resolved", s.pcl_functions_ready);
    stage_line("Streamline proxy D3D12 device", s.proxy_device_ready);
    stage_line("Streamline proxy presenting queue", s.proxy_queue_ready);
    stage_line("native command queue resolved behind proxy", s.native_queue_resolved);
    stage_line("Streamline proxy DXGI factory", s.proxy_factory_ready);
    stage_line("endpoint compositor window", s.endpoint_window_ready);
    stage_line("Streamline proxy swapchain", s.proxy_swapchain_ready);
    stage_line("native swapchain resolved behind proxy", s.native_swapchain_resolved);

    if (s.requirements_queried)
    {
        ImGui::Text("DLSS-G API flags: D3D11 %s | D3D12 %s | Vulkan %s",
                    s.requirement_d3d11 ? "YES" : "no",
                    s.requirement_d3d12 ? "YES" : "no",
                    s.requirement_vulkan ? "YES" : "no");
        ImGui::Text("Special requirements: HAGS %s | VSync off %s",
                    s.requirement_hags ? "required" : "not flagged",
                    s.requirement_vsync_off ? "required" : "not flagged");
    }
    draw_result("slSetD3DDevice", s.last_set_device);
    draw_result("slIsFeatureSupported(DLSS-G)", s.last_supported);
    draw_result("slSetFeatureLoaded(DLSS-G)", s.last_set_feature_loaded);
    draw_result("slIsFeatureLoaded(DLSS-G)", s.last_is_loaded);

    ImGui::Separator();
    ImGui::TextUnformatted("3. DLSS5 Feeder input provider");
    stage_line("DLSS5_Feed.fx technique found", guides.effect_present);
    stage_line("DLSS5_Feed.fx technique ENABLED / updating", guides.effect_enabled);
    stage_line("DLSS5_MV texture declared", guides.motion_vectors);
    stage_line("DLSS5_Depth texture declared", guides.depth);
    stage_line("native D3D11 MV texture acquired", s.feeder_mv_acquired);
    stage_line("native D3D11 depth texture acquired", s.feeder_depth_acquired);
    stage_line("D3D11 <-> D3D12 shared color/MV/depth bridge", s.feeder_bridge_ready);
    if (d.renodx_dlss5_loaded)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "[ OK ] your renodx-dlss5.addon64 is loaded");
    ImGui::TextDisabled("Feeder is used for the guides. It is not responsible for the FG presentation endpoint.");

    ImGui::Separator();
    ImGui::TextUnformatted("4. Per-frame DLSS-G contract");
    stage_line("controller ready for first NVIDIA Present", s.controller_ready);
    stage_line("frame token created", s.frame_token_ready, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("MV/depth/HUD-less-color tags submitted", s.tags_submitted, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("common constants submitted", s.constants_submitted, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("Reflex low-latency enabled", s.reflex_enabled, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("slReflexSleep called for frame token", s.reflex_sleep_called, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("PCL markers submitted", s.pcl_markers_submitted, s.endpoint_present_attempted || fg::slhost::requested_enabled());
    stage_line("proxy Present attempted", s.endpoint_present_attempted, fg::slhost::requested_enabled());
    stage_line("proxy Present succeeded", s.endpoint_present_succeeded, s.endpoint_present_attempted);

    if (!s.bootstrap_note.empty())
        ImGui::TextWrapped("Current stage: %s", s.bootstrap_note.c_str());
    if (FAILED(s.endpoint_hr))
        ImGui::Text("Endpoint HRESULT: 0x%08X", static_cast<unsigned int>(s.endpoint_hr));
    if (s.endpoint_present_attempted)
        ImGui::Text("Present HRESULT: 0x%08X", static_cast<unsigned int>(s.last_present_hr));

    ImGui::Separator();
    ImGui::TextUnformatted("REAL NVIDIA DLSS Frame Generation test");
    bool enabled = fg::slhost::requested_enabled();
    int multiplier = fg::slhost::requested_multiplier();
    bool reverse_z = fg::slhost::requested_depth_inverted();

    ImGui::BeginDisabled(!s.controller_ready && !enabled);
    if (ImGui::Checkbox("Enable REAL DLSS-G endpoint (F6)", &enabled))
        fg::slhost::request_enabled(enabled);
    ImGui::EndDisabled();

    int max_multiplier = s.max_generated_frames > 0 ? static_cast<int>(s.max_generated_frames + 1) : 2;
    max_multiplier = std::clamp(max_multiplier, 2, 6);
    if (ImGui::SliderInt("Frame multiplier", &multiplier, 2, max_multiplier))
        fg::slhost::request_multiplier(multiplier);
    if (ImGui::Checkbox("Depth is reverse-Z", &reverse_z))
        fg::slhost::request_depth_inverted(reverse_z);

    if (!s.controller_ready)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Not READY yet: the first WAIT above is the blocker.");
    else if (!enabled)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "READY. Press F6 for the first x2 attempt.");
    else
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "FG requested. The transparent endpoint window should now cover the game while NVIDIA owns its Present path.");

    ImGui::TextWrapped("First-test warning: v0.3 uses generic identity camera matrices and treats final game color as HUD-less color. This is for proving real DLSS-G execution first; image quality/UI separation comes after the endpoint is alive.");

    if (ImGui::Button("Re-probe state")) fg::slhost::force_reprobe();
    ImGui::SameLine();
    if (ImGui::Button("Refresh DLL state")) fg::slhost::retry_bootstrap_now();
    ImGui::TextDisabled("A cold restart is required to retry slInit. The refresh button never calls slInit twice.");

    ImGui::Separator();
    ImGui::TextUnformatted("NVIDIA state / output proof");
    draw_dlssg_failures(s);
    ImGui::Text("Max generated frames per real frame: %u", s.max_generated_frames);
    ImGui::Text("Real game Present callbacks: %.1f fps", s.app_present_fps);
    ImGui::Text("DLSS-G frames actually presented: %.1f fps", s.output_fps);
    draw_result("slGetNewFrameToken", s.last_get_frame_token);
    draw_result("slSetTagForFrame", s.last_set_tags);
    draw_result("slSetConstants", s.last_set_constants);
    draw_result("slReflexSetOptions", s.last_reflex_options);
    draw_result("slReflexSleep", s.last_reflex_sleep);
    draw_result("slPCLSetMarker", s.last_pcl_marker);
    draw_result("slDLSSGSetOptions", s.last_set_options);
    draw_result("slDLSSGGetState", s.last_get_state);

    ImGui::Separator();
    ImGui::TextUnformatted("Installed DLL visibility");
    ImGui::TextDisabled("FOUND = exists beside game EXE (./streamline is fallback). LOADED = mapped in this process.");
    draw_dll("sl.interposer.dll", d.interposer);
    draw_dll("sl.dlss_g.dll", d.sl_dlssg);
    draw_dll("sl.reflex.dll", d.sl_reflex);
    draw_dll("sl.pcl.dll", d.sl_pcl);
    draw_dll("sl.common.dll", d.sl_common);
    draw_dll("nvngx_dlssg.dll", d.ngx_dlssg);
}

bool on_create_device(reshade::api::device_api api, uint32_t &api_version)
{
    g_api = api;
    g_api_known = true;
    fg::slhost::set_game_api(api);
    return fg::slhost::on_create_device(api, api_version);
}

void on_init_device(reshade::api::device *device)
{
    fg::slhost::on_init_device(device);
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    fg::slhost::on_init_swapchain(swapchain, resize);
}

void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (!consider_primary_runtime(runtime)) return;
    if (runtime && runtime->get_device())
    {
        g_api = runtime->get_device()->get_api();
        g_api_known = true;
        fg::slhost::set_game_api(g_api);
    }
    fg::slhost::on_primary_runtime(runtime);
    fg::guides::resolve(runtime);
    fg::slhost::force_reprobe();
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime != g_primary_runtime.load(std::memory_order_acquire)) return;
    fg::slhost::on_primary_runtime_destroyed(runtime);
    fg::guides::clear(runtime);
    g_primary_runtime.store(nullptr, std::memory_order_release);
    g_primary_area.store(0, std::memory_order_relaxed);
    g_api_known = false;
    fg::slhost::force_reprobe();
}

void on_reloaded_effects(reshade::api::effect_runtime *runtime)
{
    if (runtime == g_primary_runtime.load(std::memory_order_acquire)) fg::guides::resolve(runtime);
}

void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    if (!consider_primary_runtime(runtime)) return;
    fg::slhost::present_tick(runtime);
}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        if (!reshade::register_addon(hinstDLL)) return FALSE;
        reshade::register_event<reshade::addon_event::create_device>(&on_create_device);
        reshade::register_event<reshade::addon_event::init_device>(&on_init_device);
        reshade::register_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::register_overlay("DLSS-G Host", &draw_overlay);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<reshade::addon_event::create_device>(&on_create_device);
        reshade::unregister_event<reshade::addon_event::init_device>(&on_init_device);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(&on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
        reshade::unregister_addon(hinstDLL);
        break;
    }
    return TRUE;
}
