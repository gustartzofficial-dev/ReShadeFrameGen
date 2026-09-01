#include "streamline_host.hpp"
#include "dependency_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace fg::slhost
{
namespace
{
using InitFn = sl::Result(const sl::Preferences &, uint64_t);
using ShutdownFn = sl::Result();
using SetD3DDeviceFn = sl::Result(void *);
using IsFeatureLoadedFn = PFun_slIsFeatureLoaded;
using GetFeatureFunctionFn = PFun_slGetFeatureFunction;

HMODULE g_interposer = nullptr;
InitFn *g_init = nullptr;
ShutdownFn *g_shutdown = nullptr;
SetD3DDeviceFn *g_set_d3d_device = nullptr;
IsFeatureLoadedFn *g_is_feature_loaded = nullptr;
GetFeatureFunctionFn *g_get_feature_function = nullptr;
PFun_slDLSSGSetOptions *g_set_options = nullptr;
PFun_slDLSSGGetState *g_get_state = nullptr;

std::atomic_bool g_requested_enabled{false};
std::atomic_int g_requested_multiplier{2};
std::atomic_bool g_dirty{false};
std::atomic_bool g_force_probe{true};
std::atomic_bool g_slinit_attempted{false};
std::atomic<reshade::api::device_api> g_game_api{reshade::api::device_api::d3d11};

std::mutex g_state_mutex;
Snapshot g_state{};

ULONGLONG g_last_probe_ms = 0;
ULONGLONG g_last_poll_ms = 0;
uint64_t g_app_presents_since_poll = 0;

std::wstring g_plugin_dir;
std::array<const wchar_t *, 1> g_plugin_paths{};
constexpr std::array<sl::Feature, 3> k_requested_features = {
    sl::kFeaturePCL,
    sl::kFeatureReflex,
    sl::kFeatureDLSS_G,
};

// A stable project identifier for this open-source host experiment. Streamline accepts a projectId
// in place of an NVIDIA applicationId; this is not pretending to be another game's application ID.
constexpr const char *k_project_id = "68c3c204-a7b9-43e0-a319-37b62eef12f7";
constexpr const char *k_engine_version = "ReShadeFrameGen-DLSSGHost-0.2.1";

void set_note(Snapshot &s, const char *note)
{
    s.bootstrap_note = note != nullptr ? note : "";
}

void resolve_core_exports(HMODULE module, Snapshot &s)
{
    if (module == nullptr)
        return;

    g_interposer = module;
    g_init = reinterpret_cast<InitFn *>(GetProcAddress(module, "slInit"));
    g_shutdown = reinterpret_cast<ShutdownFn *>(GetProcAddress(module, "slShutdown"));
    g_set_d3d_device = reinterpret_cast<SetD3DDeviceFn *>(GetProcAddress(module, "slSetD3DDevice"));
    g_is_feature_loaded = reinterpret_cast<IsFeatureLoadedFn *>(GetProcAddress(module, "slIsFeatureLoaded"));
    g_get_feature_function = reinterpret_cast<GetFeatureFunctionFn *>(GetProcAddress(module, "slGetFeatureFunction"));

    s.core_exports_ready = g_init != nullptr && g_is_feature_loaded != nullptr &&
                           g_get_feature_function != nullptr && g_set_d3d_device != nullptr;
}

void resolve_feature_functions(Snapshot &s)
{
    if (g_is_feature_loaded == nullptr || g_get_feature_function == nullptr)
    {
        s.feature_loaded = false;
        s.feature_functions_ready = false;
        return;
    }

    bool loaded = false;
    s.last_is_loaded = (*g_is_feature_loaded)(sl::kFeatureDLSS_G, loaded);
    s.streamline_initialized = s.streamline_initialized || s.last_is_loaded != sl::Result::eErrorNotInitialized;
    s.feature_loaded = s.last_is_loaded == sl::Result::eOk && loaded;

    if (s.feature_loaded && (g_set_options == nullptr || g_get_state == nullptr))
    {
        void *set_ptr = nullptr;
        void *get_ptr = nullptr;
        const sl::Result r_set = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_ptr);
        const sl::Result r_get = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGGetState", get_ptr);
        s.last_get_function = r_set != sl::Result::eOk ? r_set : r_get;
        if (r_set == sl::Result::eOk && r_get == sl::Result::eOk && set_ptr != nullptr && get_ptr != nullptr)
        {
            g_set_options = reinterpret_cast<PFun_slDLSSGSetOptions *>(set_ptr);
            g_get_state = reinterpret_cast<PFun_slDLSSGGetState *>(get_ptr);
        }
    }

    s.feature_functions_ready = g_set_options != nullptr && g_get_state != nullptr;
}

bool perform_bootstrap(bool early_device_creation)
{
    Snapshot s;
    {
        std::lock_guard lock(g_state_mutex);
        s = g_state;
    }

    s.bootstrap_attempted = true;
    const deps::Snapshot deps = deps::probe();
    s.interposer_found = deps.interposer.found;
    s.interposer_loaded = deps.interposer.loaded;
    s.interposer_path = deps.interposer.path;
    s.plugin_directory = deps::preferred_streamline_directory();

    if (!deps.interposer.found && !deps.interposer.loaded)
    {
        set_note(s, "sl.interposer.dll was not found beside the game EXE (or fallback ./streamline folder).");
        std::lock_guard lock(g_state_mutex);
        g_state = s;
        return false;
    }

    HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    if (module == nullptr)
    {
        DWORD error = ERROR_SUCCESS;
        module = deps::load_from_detected_path(deps.interposer, &error);
        s.load_library_error = error;
        s.interposer_loaded = module != nullptr;
        if (module == nullptr)
        {
            set_note(s, "Found sl.interposer.dll, but Windows failed to load it. See LoadLibrary error code.");
            std::lock_guard lock(g_state_mutex);
            g_state = s;
            return false;
        }
    }

    resolve_core_exports(module, s);
    if (!s.core_exports_ready)
    {
        set_note(s, "sl.interposer.dll loaded, but required Streamline exports were not found.");
        std::lock_guard lock(g_state_mutex);
        g_state = s;
        return false;
    }

    // Do not query feature-loaded state before a D3D device exists: Streamline documents
    // slIsFeatureLoaded as a post-device API. At this pre-device point we either initialize the
    // Streamline instance once, or leave an already-attempted instance alone and verify it later.
    if (early_device_creation && !g_slinit_attempted.exchange(true, std::memory_order_acq_rel))
    {
        g_plugin_dir = s.plugin_directory;
        g_plugin_paths[0] = g_plugin_dir.c_str();

        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.pathsToPlugins = g_plugin_paths.data();
        pref.numPathsToPlugins = static_cast<uint32_t>(g_plugin_paths.size());
        pref.pathToLogsAndData = g_plugin_dir.c_str();
        pref.featuresToLoad = k_requested_features.data();
        pref.numFeaturesToLoad = static_cast<uint32_t>(k_requested_features.size());
        pref.projectId = k_project_id;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = k_engine_version;
        // Be honest about the host API. Streamline supports a D3D11 integration path and may
        // internally use DX11-on-DX12 for feature execution.
        const auto game_api = g_game_api.load(std::memory_order_relaxed);
        pref.renderAPI = game_api == reshade::api::device_api::d3d11 ? sl::RenderAPI::eD3D11 : sl::RenderAPI::eD3D12;

        s.last_init = (*g_init)(pref, sl::kSDKVersion);
        s.initialized_by_us = s.last_init == sl::Result::eOk;
        s.streamline_initialized = s.initialized_by_us;
        s.dlssg_requested = true;
        s.reflex_requested = true;
        s.pcl_requested = true;

        if (!s.initialized_by_us)
        {
            set_note(s, "slInit returned an error during the pre-device bootstrap. The DLLs were found and loaded; inspect the result/Streamline log.");
            std::lock_guard lock(g_state_mutex);
            g_state = s;
            return false;
        }
        set_note(s, "Streamline initialized before the game device. Waiting for the game device, plugin state and swapchain proxy.");
    }
    else if (!early_device_creation)
    {
        set_note(s, "DLL state refreshed. slInit is intentionally not called from the late UI button; use a cold restart for a new bootstrap attempt.");
    }
    else
    {
        set_note(s, "Streamline bootstrap was already attempted in this process; waiting for post-device state.");
    }

    s.controller_ready = s.feature_loaded && s.feature_functions_ready &&
                         s.swapchain_is_streamline_proxy && s.host_device_submitted;

    std::lock_guard lock(g_state_mutex);
    g_state = s;
    return true;
}

void refresh_resolution()
{
    Snapshot s;
    {
        std::lock_guard lock(g_state_mutex);
        s = g_state;
    }

    const deps::Snapshot d = deps::probe();
    s.interposer_found = d.interposer.found;
    s.interposer_loaded = d.interposer.loaded;
    if (!d.interposer.path.empty())
        s.interposer_path = d.interposer.path;
    if (s.plugin_directory.empty())
        s.plugin_directory = deps::preferred_streamline_directory();

    HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    if (module != nullptr)
        resolve_core_exports(module, s);
    resolve_feature_functions(s);
    s.controller_ready = s.feature_loaded && s.feature_functions_ready &&
                         s.swapchain_is_streamline_proxy && s.host_device_submitted;

    std::lock_guard lock(g_state_mutex);
    g_state = s;
}

bool is_sl_proxy(IUnknown *object)
{
    if (object == nullptr)
        return false;

    // NVIDIA-documented QueryInterface escape hatch for third-party overlays to identify SL proxies.
    static const IID k_streamline_native_iid = {
        0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}
    };

    IUnknown *native = nullptr;
    const HRESULT hr = object->QueryInterface(k_streamline_native_iid, reinterpret_cast<void **>(&native));
    if (SUCCEEDED(hr) && native != nullptr)
    {
        native->Release();
        return true;
    }
    return false;
}

void apply_requested_options()
{
    if (!g_dirty.exchange(false, std::memory_order_acq_rel))
        return;

    Snapshot local = snapshot();
    if (!local.controller_ready || g_set_options == nullptr)
    {
        g_dirty.store(true, std::memory_order_release);
        return;
    }

    const bool enable = g_requested_enabled.load(std::memory_order_relaxed);
    int multiplier = std::clamp(g_requested_multiplier.load(std::memory_order_relaxed), 2, 6);
    if (local.max_generated_frames > 0)
        multiplier = std::min(multiplier, static_cast<int>(local.max_generated_frames + 1));

    sl::DLSSGOptions options{};
    options.mode = enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = static_cast<uint32_t>(std::max(1, multiplier - 1));

    const sl::ViewportHandle viewport{0};
    const sl::Result result = (*g_set_options)(viewport, options);

    std::lock_guard lock(g_state_mutex);
    g_state.last_set_options = result;
}

void poll_state()
{
    if (g_get_state == nullptr)
        return;

    const ULONGLONG now = GetTickCount64();
    if (g_last_poll_ms != 0 && now - g_last_poll_ms < 500)
        return;

    const double seconds = g_last_poll_ms == 0 ? 0.0 : static_cast<double>(now - g_last_poll_ms) / 1000.0;
    g_last_poll_ms = now;

    sl::DLSSGState state{};
    const sl::ViewportHandle viewport{0};
    const sl::Result result = (*g_get_state)(viewport, state, nullptr);

    std::lock_guard lock(g_state_mutex);
    g_state.last_get_state = result;
    g_state.state_queried = true;
    if (result == sl::Result::eOk)
    {
        g_state.status = state.status;
        g_state.max_generated_frames = state.numFramesToGenerateMax;
        g_state.frames_presented_last_poll = state.numFramesActuallyPresented;
        g_state.dynamic_mfg_supported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        if (seconds > 0.0)
        {
            g_state.output_fps = static_cast<double>(state.numFramesActuallyPresented) / seconds;
            g_state.app_present_fps = static_cast<double>(g_app_presents_since_poll) / seconds;
        }
    }
    g_app_presents_since_poll = 0;
}
}

bool on_create_device(reshade::api::device_api api, uint32_t &)
{
    g_game_api.store(api, std::memory_order_relaxed);
    if (api == reshade::api::device_api::d3d11 || api == reshade::api::device_api::d3d12)
        perform_bootstrap(true);
    return false;
}

void on_init_device(reshade::api::device *device)
{
    if (device == nullptr)
        return;

    Snapshot s = snapshot();
    if (!s.streamline_initialized || g_set_d3d_device == nullptr)
        return;

    if (device->get_api() == reshade::api::device_api::d3d11 ||
        device->get_api() == reshade::api::device_api::d3d12)
    {
        s.host_device_seen = true;
        s.host_device_api = device->get_api();
        void *native = reinterpret_cast<void *>(static_cast<uintptr_t>(device->get_native()));
        s.last_set_device = (*g_set_d3d_device)(native);
        s.host_device_submitted = s.last_set_device == sl::Result::eOk;
        if (s.host_device_submitted)
            set_note(s, device->get_api() == reshade::api::device_api::d3d11
                ? "D3D11 game device submitted to Streamline; waiting for the DLSS-G swapchain proxy/state."
                : "D3D12 game device submitted to Streamline; waiting for the DLSS-G swapchain proxy/state.");

        resolve_feature_functions(s);
        s.controller_ready = s.feature_loaded && s.feature_functions_ready &&
                             s.swapchain_is_streamline_proxy && s.host_device_submitted;
        std::lock_guard lock(g_state_mutex);
        g_state = s;
    }
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    if (swapchain == nullptr || resize)
        return;

    Snapshot s = snapshot();
    s.swapchain_seen = true;
    IUnknown *native = reinterpret_cast<IUnknown *>(static_cast<uintptr_t>(swapchain->get_native()));
    s.swapchain_is_streamline_proxy = is_sl_proxy(native);
    if (s.swapchain_is_streamline_proxy)
        set_note(s, "The game's swapchain is a Streamline proxy. DLSS-G presentation plumbing is attached.");
    else if (s.streamline_initialized)
        set_note(s, "Streamline initialized, but this game swapchain is NOT an SL proxy. DLSS-G cannot present generated frames through it yet.");

    s.controller_ready = s.feature_loaded && s.feature_functions_ready &&
                         s.swapchain_is_streamline_proxy && s.host_device_submitted;
    std::lock_guard lock(g_state_mutex);
    g_state = s;
}

void set_game_api(reshade::api::device_api api)
{
    g_game_api.store(api, std::memory_order_relaxed);
}

void mark_app_present()
{
    ++g_app_presents_since_poll;
}

void request_enabled(bool enabled)
{
    g_requested_enabled.store(enabled, std::memory_order_relaxed);
    g_dirty.store(true, std::memory_order_release);
}

void request_multiplier(int multiplier)
{
    g_requested_multiplier.store(std::clamp(multiplier, 2, 6), std::memory_order_relaxed);
    g_dirty.store(true, std::memory_order_release);
}

bool requested_enabled() { return g_requested_enabled.load(std::memory_order_relaxed); }
int requested_multiplier() { return g_requested_multiplier.load(std::memory_order_relaxed); }

void present_tick()
{
    mark_app_present();
    const ULONGLONG now = GetTickCount64();
    if (g_force_probe.exchange(false, std::memory_order_acq_rel) || g_last_probe_ms == 0 || now - g_last_probe_ms >= 1000)
    {
        g_last_probe_ms = now;
        refresh_resolution();
    }
    apply_requested_options();
    poll_state();
}

void force_reprobe() { g_force_probe.store(true, std::memory_order_release); }
void retry_bootstrap_now() { perform_bootstrap(false); force_reprobe(); }

Snapshot snapshot()
{
    std::lock_guard lock(g_state_mutex);
    return g_state;
}

const char *result_name(sl::Result r)
{
    switch (r)
    {
    case sl::Result::eOk: return "OK";
    case sl::Result::eErrorNotInitialized: return "not initialized";
    case sl::Result::eErrorInvalidParameter: return "invalid parameter";
    case sl::Result::eErrorFeatureMissing: return "feature missing";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
    case sl::Result::eErrorMissingOrInvalidAPI: return "missing/invalid API";
    case sl::Result::eErrorOSDisabledHWS: return "HAGS disabled";
    case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
    default: return "see Streamline log";
    }
}
}
