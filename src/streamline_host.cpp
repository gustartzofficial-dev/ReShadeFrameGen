#include "streamline_host.hpp"
#include "dependency_probe.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace fg::slhost
{
namespace
{
using IsFeatureLoadedFn = PFun_slIsFeatureLoaded;
using GetFeatureFunctionFn = PFun_slGetFeatureFunction;

HMODULE g_interposer = nullptr;
IsFeatureLoadedFn *g_is_feature_loaded = nullptr;
GetFeatureFunctionFn *g_get_feature_function = nullptr;
PFun_slDLSSGSetOptions *g_set_options = nullptr;
PFun_slDLSSGGetState *g_get_state = nullptr;

std::atomic_bool g_requested_enabled{false};
std::atomic_int g_requested_multiplier{2};
std::atomic_bool g_dirty{false};
std::atomic_bool g_force_probe{true};
std::atomic_bool g_is_d3d11{false};

std::mutex g_state_mutex;
Snapshot g_state{};

ULONGLONG g_last_probe_ms = 0;
ULONGLONG g_last_poll_ms = 0;
uint64_t g_app_presents_since_poll = 0;

void clear_resolved()
{
    g_interposer = nullptr;
    g_is_feature_loaded = nullptr;
    g_get_feature_function = nullptr;
    g_set_options = nullptr;
    g_get_state = nullptr;
}

void resolve_streamline()
{
    Snapshot local;
    {
        std::lock_guard lock(g_state_mutex);
        local = g_state;
    }

    HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    local.interposer_loaded = module != nullptr;
    if (module == nullptr)
    {
        clear_resolved();
        local.core_exports_ready = false;
        local.feature_loaded = false;
        local.feature_functions_ready = false;
        local.controller_ready = false;
        std::lock_guard lock(g_state_mutex);
        g_state = local;
        return;
    }

    if (module != g_interposer || g_is_feature_loaded == nullptr || g_get_feature_function == nullptr)
    {
        g_interposer = module;
        g_is_feature_loaded = reinterpret_cast<IsFeatureLoadedFn *>(GetProcAddress(module, "slIsFeatureLoaded"));
        g_get_feature_function = reinterpret_cast<GetFeatureFunctionFn *>(GetProcAddress(module, "slGetFeatureFunction"));
        g_set_options = nullptr;
        g_get_state = nullptr;
    }

    local.core_exports_ready = g_is_feature_loaded != nullptr && g_get_feature_function != nullptr;
    if (!local.core_exports_ready)
    {
        local.feature_loaded = false;
        local.feature_functions_ready = false;
        local.controller_ready = false;
        std::lock_guard lock(g_state_mutex);
        g_state = local;
        return;
    }

    bool loaded = false;
    local.last_is_loaded = (*g_is_feature_loaded)(sl::kFeatureDLSS_G, loaded);
    local.feature_loaded = local.last_is_loaded == sl::Result::eOk && loaded;

    if (local.feature_loaded && (g_set_options == nullptr || g_get_state == nullptr))
    {
        void *set_ptr = nullptr;
        void *get_ptr = nullptr;
        const sl::Result r_set = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_ptr);
        const sl::Result r_get = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGGetState", get_ptr);
        local.last_get_function = r_set != sl::Result::eOk ? r_set : r_get;
        if (r_set == sl::Result::eOk && r_get == sl::Result::eOk && set_ptr != nullptr && get_ptr != nullptr)
        {
            g_set_options = reinterpret_cast<PFun_slDLSSGSetOptions *>(set_ptr);
            g_get_state = reinterpret_cast<PFun_slDLSSGGetState *>(get_ptr);
        }
    }

    local.feature_functions_ready = g_set_options != nullptr && g_get_state != nullptr;

    // D3D11 has no official native DLSS-G integration path. For this scratch build we only
    // allow it when ShortFuse's current RenoDX DLSS host is loaded, because that is the
    // component that owns the D3D12 endpoint / Streamline presentation plumbing.
    const deps::Snapshot d = deps::probe();
    const bool host_ok = !g_is_d3d11.load(std::memory_order_relaxed) || d.renodx_dlss_loaded;
    local.controller_ready = local.feature_loaded && local.feature_functions_ready && host_ok;

    std::lock_guard lock(g_state_mutex);
    g_state = local;
}

void apply_requested_options()
{
    if (!g_dirty.exchange(false, std::memory_order_acq_rel))
        return;

    Snapshot local = snapshot();
    if (!local.controller_ready || g_set_options == nullptr)
    {
        // Keep the request pending so it is automatically applied once the early host is ready.
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

    // Keep all presentation/resource knobs at their host defaults. The current RenoDX/Streamline
    // host owns the swapchain, tags, formats, UI recomposition and Reflex contract. This controller
    // only turns NVIDIA's FG feature on/off and chooses the multiplier.
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

void set_api_is_d3d11(bool value)
{
    if (g_is_d3d11.exchange(value, std::memory_order_relaxed) != value)
        g_force_probe.store(true, std::memory_order_release);
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

bool requested_enabled()
{
    return g_requested_enabled.load(std::memory_order_relaxed);
}

int requested_multiplier()
{
    return g_requested_multiplier.load(std::memory_order_relaxed);
}

void present_tick()
{
    mark_app_present();
    const ULONGLONG now = GetTickCount64();
    if (g_force_probe.exchange(false, std::memory_order_acq_rel) || g_last_probe_ms == 0 || now - g_last_probe_ms >= 1000)
    {
        g_last_probe_ms = now;
        resolve_streamline();
    }

    // Both functions are intentionally called from ReShade's presentation callback. NVIDIA's
    // programming guide states DLSS-G SetOptions/GetState are not thread safe with Present.
    apply_requested_options();
    poll_state();
}

void force_reprobe()
{
    g_force_probe.store(true, std::memory_order_release);
}

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
    case sl::Result::eErrorDeviceNotCreated: return "device not created";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter";
    case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
    case sl::Result::eErrorOSDisabledHWS: return "HAGS disabled";
    case sl::Result::eErrorMissingProxy: return "missing Streamline proxy";
    case sl::Result::eErrorInvalidIntegration: return "invalid Streamline integration";
    case sl::Result::eErrorNotInitialized: return "not initialized";
    case sl::Result::eErrorInitNotCalled: return "slInit not called";
    case sl::Result::eErrorFeatureMissing: return "feature missing";
    case sl::Result::eErrorFeatureNotSupported: return "feature unsupported";
    case sl::Result::eErrorFeatureMissingHooks: return "feature hooks missing";
    case sl::Result::eErrorFeatureFailedToLoad: return "feature failed to load";
    case sl::Result::eErrorFeatureMissingDependency: return "feature dependency missing";
    case sl::Result::eErrorCommonConstantsMissing: return "common constants missing";
    case sl::Result::eErrorMissingConstants: return "constants missing";
    case sl::Result::eErrorInvalidState: return "invalid state";
    default: return "Streamline error";
    }
}
}
