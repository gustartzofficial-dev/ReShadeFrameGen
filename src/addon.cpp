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
#include <string>

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include <imgui.h>
#include <reshade.hpp>

#include "dependency_probe.hpp"
#include "feeder_probe.hpp"
#include "streamline_host.hpp"

extern "C" __declspec(dllexport) const char *NAME = "ReShade FrameGen - DLSS-G Host 0.2 Bootstrap";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Clean-slate DLSS-G bootstrap/controller. Finds and loads the real Streamline DLLs, requests DLSS-G/Reflex/PCL early, "
    "submits the real D3D11/D3D12 game device, and only arms NVIDIA Frame Generation when Streamline presentation is actually attached.";

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
        ImGui::TextDisabled("not loaded yet");
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
    if (has_status(s.status, sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) ImGui::BulletText("Reflex cadence is missing/not detected.");
    if (has_status(s.status, sl::DLSSGStatus::eFailHDRFormatNotSupported)) ImGui::BulletText("Backbuffer/HDR format unsupported.");
    if (has_status(s.status, sl::DLSSGStatus::eFailCommonConstantsInvalid)) ImGui::BulletText("Common camera/frame constants are missing or invalid.");
    if (has_status(s.status, sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)) ImGui::BulletText("Streamline swapchain/backbuffer-index contract is incomplete.");
}

void draw_overlay(reshade::api::effect_runtime *)
{
    const fg::deps::Snapshot d = fg::deps::probe();
    const fg::guides::Snapshot guides = fg::guides::snapshot();
    const fg::slhost::Snapshot s = fg::slhost::snapshot();

    ImGui::TextUnformatted("DLSS-G Host 0.2 - active bootstrap test");
    ImGui::TextDisabled("No legacy interpolation. No fake extra Present. Uses the real NVIDIA Streamline/DLSS-G DLLs.");
    ImGui::Separator();

    ImGui::Text("Game API: %s", api_name(g_api));
    ImGui::Text("Primary ReShade surface: %s", g_primary_runtime.load(std::memory_order_acquire) ? "selected" : "waiting");

    ImGui::Separator();
    ImGui::TextUnformatted("Installed vs loaded DLLs");
    ImGui::TextDisabled("FOUND = file exists on disk. LOADED = Windows has mapped it into this game process.");
    draw_dll("sl.interposer.dll", d.interposer);
    draw_dll("sl.dlss_g.dll", d.sl_dlssg);
    draw_dll("sl.reflex.dll", d.sl_reflex);
    draw_dll("sl.pcl.dll", d.sl_pcl);
    draw_dll("sl.common.dll", d.sl_common);
    draw_dll("nvngx_dlssg.dll", d.ngx_dlssg);

    ImGui::Separator();
    ImGui::TextUnformatted("Bootstrap stages");
    stage_line("1. sl.interposer.dll found", s.interposer_found);
    stage_line("2. sl.interposer.dll mapped into process", s.interposer_loaded);
    stage_line("3. Streamline core exports resolved", s.core_exports_ready);
    stage_line("4. Streamline initialized", s.streamline_initialized);
    stage_line("5. DLSS-G feature/plugin loaded", s.feature_loaded);
    stage_line("6. DLSS-G SetOptions/GetState resolved", s.feature_functions_ready);
    stage_line("7. Game D3D device observed and submitted to SL", s.host_device_submitted);
    stage_line("8. Real game swapchain is a Streamline proxy", s.swapchain_is_streamline_proxy);
    stage_line("9. Controller armed", s.controller_ready);

    if (!s.bootstrap_note.empty())
        ImGui::TextWrapped("Current stage: %s", s.bootstrap_note.c_str());
    if (s.load_library_error != ERROR_SUCCESS)
        ImGui::Text("LoadLibrary Win32 error: %lu", static_cast<unsigned long>(s.load_library_error));
    if (!s.plugin_directory.empty())
    {
        const std::string plugin_dir = fg::deps::narrow(s.plugin_directory);
        ImGui::TextWrapped("Streamline plugin directory: %s", plugin_dir.c_str());
    }
    ImGui::Text("slInit: %s", fg::slhost::result_name(s.last_init));
    if (s.host_device_seen)
        ImGui::Text("Game device submitted as: %s", api_name(s.host_device_api));
    ImGui::Text("slSetD3DDevice: %s", fg::slhost::result_name(s.last_set_device));
    ImGui::Text("slIsFeatureLoaded(DLSS-G): %s", fg::slhost::result_name(s.last_is_loaded));

    if (d.renodx_dlss5_loaded)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "[ OK ] renodx-dlss5.addon64 loaded (RenoDX DLSS5/NR host)");
    if (d.renodx_dlss_loaded)
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "[ OK ] renodx-dlss.addon64 loaded");

    ImGui::Separator();
    ImGui::TextUnformatted("Feeder guide resources");
    stage_line("DLSS5_Feed.fx technique found", guides.effect_present);
    stage_line("DLSS5_MV texture found", guides.motion_vectors);
    stage_line("DLSS5_Depth texture found", guides.depth);
    ImGui::TextDisabled("These prove the guide data exists. 0.2 does not yet steal ownership of RenoDX's frame token/resources.");

    ImGui::Separator();
    ImGui::TextUnformatted("Real NVIDIA DLSS Frame Generation");
    bool enabled = fg::slhost::requested_enabled();
    int multiplier = fg::slhost::requested_multiplier();
    ImGui::BeginDisabled(!s.controller_ready);
    if (ImGui::Checkbox("Enable DLSS Frame Generation", &enabled)) fg::slhost::request_enabled(enabled);
    int max_multiplier = s.max_generated_frames > 0 ? static_cast<int>(s.max_generated_frames + 1) : 4;
    max_multiplier = std::clamp(max_multiplier, 2, 6);
    if (ImGui::SliderInt("Frame multiplier", &multiplier, 2, max_multiplier)) fg::slhost::request_multiplier(multiplier);
    ImGui::EndDisabled();

    if (!s.controller_ready)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Not armed yet: the first WAIT stage above is the actual blocker.");

    if (ImGui::Button("Re-probe state")) fg::slhost::force_reprobe();
    ImGui::SameLine();
    if (ImGui::Button("Retry DLL load (diagnostic)")) fg::slhost::retry_bootstrap_now();
    ImGui::TextDisabled("The retry button does not call slInit late. Restart the game to retry the pre-device bootstrap safely.");

    ImGui::Separator();
    ImGui::TextUnformatted("NVIDIA state");
    draw_dlssg_failures(s);
    ImGui::Text("Max generated frames / real frame: %u", s.max_generated_frames);
    ImGui::Text("App Present callbacks: %.1f fps", s.app_present_fps);
    ImGui::Text("DLSS-G frames actually presented: %.1f fps", s.output_fps);
    ImGui::Text("SetOptions: %s", fg::slhost::result_name(s.last_set_options));
    ImGui::Text("GetState: %s", fg::slhost::result_name(s.last_get_state));
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
    fg::guides::resolve(runtime);
    fg::slhost::force_reprobe();
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime != g_primary_runtime.load(std::memory_order_acquire)) return;
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
    fg::slhost::present_tick();
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
