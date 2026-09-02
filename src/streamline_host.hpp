#pragma once

#include <windows.h>
#include <unknwn.h>
#include <cstdint>
#include <string>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>

#include <reshade.hpp>

namespace fg::slhost
{
struct Snapshot
{
    // Loader/bootstrap
    bool bootstrap_attempted = false;
    bool interposer_found = false;
    bool interposer_loaded = false;
    bool core_exports_ready = false;
    bool streamline_initialized = false;
    bool initialized_by_us = false;
    bool dlssg_requested = false;
    bool reflex_requested = false;
    bool pcl_requested = false;

    // What NVIDIA says this DLSS-G plugin supports in this process.
    bool requirements_queried = false;
    bool requirement_d3d11 = false;
    bool requirement_d3d12 = false;
    bool requirement_vulkan = false;
    bool requirement_hags = false;
    bool requirement_vsync_off = false;

    // Private same-adapter D3D12 endpoint used for D3D11 games.
    bool game_d3d11_seen = false;
    bool endpoint_device_created = false;
    bool endpoint_device_submitted = false;
    bool endpoint_feature_supported = false;
    bool endpoint_feature_load_attempted = false;
    bool feature_loaded = false;
    bool feature_functions_ready = false;
    bool reflex_functions_ready = false;
    bool pcl_functions_ready = false;
    bool proxy_device_ready = false;
    bool proxy_queue_ready = false;
    bool native_queue_resolved = false;
    bool proxy_factory_ready = false;
    bool endpoint_window_ready = false;
    bool proxy_swapchain_ready = false;
    bool native_swapchain_resolved = false;

    // Feeder -> D3D12 bridge and per-frame contract.
    bool feeder_mv_acquired = false;
    bool feeder_depth_acquired = false;
    bool feeder_bridge_ready = false;
    bool frame_token_ready = false;
    bool tags_submitted = false;
    bool constants_submitted = false;
    bool reflex_enabled = false;
    bool reflex_sleep_called = false;
    bool pcl_markers_submitted = false;
    bool endpoint_present_attempted = false;
    bool endpoint_present_succeeded = false;
    bool controller_ready = false;
    bool state_queried = false;

    uint32_t endpoint_width = 0;
    uint32_t endpoint_height = 0;
    uint32_t endpoint_format = 0;

    DWORD load_library_error = ERROR_SUCCESS;
    HRESULT endpoint_hr = S_OK;
    HRESULT last_present_hr = S_OK;

    sl::Result last_init = sl::Result::eErrorNotInitialized;
    sl::Result last_requirements = sl::Result::eErrorNotInitialized;
    sl::Result last_supported = sl::Result::eErrorNotInitialized;
    sl::Result last_set_device = sl::Result::eErrorNotInitialized;
    sl::Result last_set_feature_loaded = sl::Result::eErrorNotInitialized;
    sl::Result last_is_loaded = sl::Result::eErrorNotInitialized;
    sl::Result last_get_function = sl::Result::eErrorNotInitialized;
    sl::Result last_get_frame_token = sl::Result::eErrorNotInitialized;
    sl::Result last_set_tags = sl::Result::eErrorNotInitialized;
    sl::Result last_set_constants = sl::Result::eErrorNotInitialized;
    sl::Result last_reflex_options = sl::Result::eErrorNotInitialized;
    sl::Result last_reflex_sleep = sl::Result::eErrorNotInitialized;
    sl::Result last_pcl_marker = sl::Result::eErrorNotInitialized;
    sl::Result last_set_options = sl::Result::eErrorNotInitialized;
    sl::Result last_get_state = sl::Result::eErrorNotInitialized;
    sl::Result last_upgrade_device = sl::Result::eErrorNotInitialized;
    sl::Result last_upgrade_factory = sl::Result::eErrorNotInitialized;
    sl::Result last_get_native_queue = sl::Result::eErrorNotInitialized;
    sl::Result last_get_native_swapchain = sl::Result::eErrorNotInitialized;

    sl::DLSSGStatus status = static_cast<sl::DLSSGStatus>(0);
    uint32_t max_generated_frames = 0;
    uint32_t frames_presented_last_poll = 0;
    double output_fps = 0.0;
    double app_present_fps = 0.0;
    bool dynamic_mfg_supported = false;

    std::wstring interposer_path;
    std::wstring plugin_directory;
    std::string bootstrap_note;
};

// Earliest ReShade callback we use to initialize Streamline. For a D3D11 game v0.3 tells
// Streamline that our FG endpoint is D3D12; the game's own D3D11 device remains untouched.
bool on_create_device(reshade::api::device_api api, uint32_t &api_version);
void on_init_device(reshade::api::device *device);
void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize);
void on_primary_runtime(reshade::api::effect_runtime *runtime);
void on_primary_runtime_destroyed(reshade::api::effect_runtime *runtime);

void set_game_api(reshade::api::device_api api);
void request_enabled(bool enabled);
void request_multiplier(int multiplier);
void request_depth_inverted(bool inverted);
bool requested_enabled();
int requested_multiplier();
bool requested_depth_inverted();

// Called after ReShade effects for the primary game surface. In v0.3 this is the real attempt:
// copy the final D3D11 frame + Feeder guides to the private D3D12 endpoint, tag them, and Present
// through an SL proxy swapchain so NVIDIA DLSS-G owns interpolation/presentation there.
void present_tick(reshade::api::effect_runtime *runtime);
void force_reprobe();
void retry_bootstrap_now();
Snapshot snapshot();
const char *result_name(sl::Result result);
}
