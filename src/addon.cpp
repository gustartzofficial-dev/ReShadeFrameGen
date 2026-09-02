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

#include "feeder_probe.hpp"
#include "streamline_host.hpp"

extern "C" __declspec(dllexport) const char *NAME = "ReShade FrameGen - DLSS-G Host 0.4";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "NVIDIA DLSS-G frame-generation host for D3D11 games. Uses DLSS5_Feed motion/depth, "
    "bridges the frame to a same-GPU D3D12 Streamline endpoint, and presents through DLSS-G.";

namespace
{
std::atomic<reshade::api::effect_runtime *> g_primary_runtime{nullptr};
std::atomic<uint64_t> g_primary_area{0};

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
        fg::slhost::force_reprobe();
    }

    return current == runtime;
}

const char *api_name(reshade::api::device_api api)
{
    switch (api)
    {
    case reshade::api::device_api::d3d11: return "D3D11";
    case reshade::api::device_api::d3d12: return "D3D12";
    case reshade::api::device_api::vulkan: return "Vulkan";
    case reshade::api::device_api::opengl: return "OpenGL";
    default: return "Unknown";
    }
}

void draw_status(const char *label, const ImVec4 &color)
{
    ImGui::TextUnformatted("Status:");
    ImGui::SameLine();
    ImGui::TextColored(color, "%s", label);
}

void draw_overlay(reshade::api::effect_runtime *)
{
    auto *runtime = g_primary_runtime.load(std::memory_order_acquire);
    const auto *device = runtime != nullptr ? runtime->get_device() : nullptr;
    const bool primary_is_d3d11 = device != nullptr && device->get_api() == reshade::api::device_api::d3d11;

    const fg::slhost::Snapshot s = fg::slhost::snapshot();
    bool enabled = fg::slhost::requested_enabled();
    int multiplier = fg::slhost::requested_multiplier();
    bool reverse_z = fg::slhost::requested_depth_inverted();

    const bool active = enabled && s.controller_ready && s.endpoint_present_succeeded &&
                        s.last_set_options == sl::Result::eOk;
    const bool hard_error = FAILED(s.endpoint_hr) ||
                            (s.streamline_initialized && s.requirements_queried && !s.endpoint_feature_supported);

    ImGui::TextUnformatted("NVIDIA DLSS Frame Generation");
    ImGui::Separator();

    if (!primary_is_d3d11 && device != nullptr)
        draw_status("UNSUPPORTED GAME API", ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
    else if (active)
        draw_status("ACTIVE", ImVec4(0.35f, 0.95f, 0.45f, 1.0f));
    else if (s.controller_ready)
        draw_status(enabled ? "STARTING" : "READY", ImVec4(0.35f, 0.95f, 0.45f, 1.0f));
    else if (hard_error)
        draw_status("ERROR", ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
    else
        draw_status("INITIALIZING", ImVec4(1.0f, 0.72f, 0.25f, 1.0f));

    if (device != nullptr)
        ImGui::TextDisabled("Primary game renderer: %s", api_name(device->get_api()));

    ImGui::Spacing();
    if (ImGui::Checkbox("Enable Frame Generation (F6)", &enabled))
        fg::slhost::request_enabled(enabled);

    int max_multiplier = s.max_generated_frames > 0 ? static_cast<int>(s.max_generated_frames + 1) : 6;
    max_multiplier = std::clamp(max_multiplier, 2, 6);
    multiplier = std::clamp(multiplier, 2, max_multiplier);
    if (ImGui::SliderInt("Frame multiplier", &multiplier, 2, max_multiplier))
        fg::slhost::request_multiplier(multiplier);

    if (ImGui::Checkbox("Depth is reverse-Z", &reverse_z))
        fg::slhost::request_depth_inverted(reverse_z);

    if (active)
    {
        ImGui::Separator();
        ImGui::Text("Game presents: %.1f fps", s.app_present_fps);
        ImGui::Text("DLSS-G presented: %.1f fps", s.output_fps);
        if (s.max_generated_frames > 0)
            ImGui::TextDisabled("NVIDIA max multiplier: x%u", s.max_generated_frames + 1);
    }
    else if (!s.bootstrap_note.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.bootstrap_note.c_str());
    }

    ImGui::TextDisabled("Detailed initialization/errors are written to ReShade.log.");
}

bool on_create_device(reshade::api::device_api api, uint32_t &api_version)
{
    // Do not use this callback to label the game renderer: helper/private D3D12 devices created
    // by this addon or DLSS5-Feeder also pass through it. The overlay reads the primary runtime.
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
    if (!consider_primary_runtime(runtime))
        return;

    if (runtime != nullptr && runtime->get_device() != nullptr)
        fg::slhost::set_game_api(runtime->get_device()->get_api());

    fg::slhost::on_primary_runtime(runtime);
    fg::guides::resolve(runtime);
    fg::slhost::force_reprobe();
}

void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (runtime != g_primary_runtime.load(std::memory_order_acquire))
        return;

    fg::slhost::on_primary_runtime_destroyed(runtime);
    fg::guides::clear(runtime);
    g_primary_runtime.store(nullptr, std::memory_order_release);
    g_primary_area.store(0, std::memory_order_relaxed);
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
    fg::slhost::present_tick(runtime);
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
