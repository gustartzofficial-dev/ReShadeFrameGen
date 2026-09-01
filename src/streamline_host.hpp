#pragma once

#include <windows.h>
#include <unknwn.h> // IUnknown / COM QueryInterface used for Streamline proxy detection
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
    bool bootstrap_attempted = false;
    bool interposer_found = false;
    bool interposer_loaded = false;
    bool core_exports_ready = false;
    bool streamline_initialized = false;
    bool initialized_by_us = false;
    bool dlssg_requested = false;
    bool reflex_requested = false;
    bool pcl_requested = false;

    bool host_device_seen = false;
    bool host_device_submitted = false;
    reshade::api::device_api host_device_api = reshade::api::device_api::d3d11;
    bool swapchain_seen = false;
    bool swapchain_is_streamline_proxy = false;

    bool feature_loaded = false;
    bool feature_functions_ready = false;
    bool controller_ready = false;
    bool state_queried = false;

    DWORD load_library_error = ERROR_SUCCESS;
    sl::Result last_init = sl::Result::eErrorNotInitialized;
    sl::Result last_set_device = sl::Result::eErrorNotInitialized;
    sl::Result last_is_loaded = sl::Result::eErrorNotInitialized;
    sl::Result last_get_function = sl::Result::eErrorNotInitialized;
    sl::Result last_set_options = sl::Result::eErrorNotInitialized;
    sl::Result last_get_state = sl::Result::eErrorNotInitialized;

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

// Earliest safe bootstrap point ReShade exposes. Returns false so it never changes the game's API version.
bool on_create_device(reshade::api::device_api api, uint32_t &api_version);
void on_init_device(reshade::api::device *device);
void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize);

void set_game_api(reshade::api::device_api api);
void mark_app_present();
void request_enabled(bool enabled);
void request_multiplier(int multiplier);
bool requested_enabled();
int requested_multiplier();
void present_tick();
void force_reprobe();
void retry_bootstrap_now();
Snapshot snapshot();
const char *result_name(sl::Result result);
}
