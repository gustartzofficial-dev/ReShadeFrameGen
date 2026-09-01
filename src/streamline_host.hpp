#pragma once

#include <windows.h>
#include <cstdint>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>

namespace fg::slhost
{
struct Snapshot
{
    bool interposer_loaded = false;
    bool core_exports_ready = false;
    bool feature_loaded = false;
    bool feature_functions_ready = false;
    bool controller_ready = false;

    sl::Result last_is_loaded = sl::Result::eErrorNotInitialized;
    sl::Result last_get_function = sl::Result::eErrorNotInitialized;
    sl::Result last_set_options = sl::Result::eErrorNotInitialized;
    sl::Result last_get_state = sl::Result::eErrorNotInitialized;

    sl::DLSSGStatus status = sl::DLSSGStatus::eOk;
    uint32_t max_generated_frames = 0;
    uint32_t frames_presented_last_poll = 0;
    double output_fps = 0.0;
    double app_present_fps = 0.0;
    bool dynamic_mfg_supported = false;
};

void set_api_is_d3d11(bool value);
void mark_app_present();
void request_enabled(bool enabled);
void request_multiplier(int multiplier);
bool requested_enabled();
int requested_multiplier();
void present_tick();
void force_reprobe();
Snapshot snapshot();
const char *result_name(sl::Result result);
}
