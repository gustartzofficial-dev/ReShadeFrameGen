#include "streamline_host.hpp"
#include "dependency_probe.hpp"
#include "feeder_probe.hpp"

#include <unknwn.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sl_reflex.h>
#include <sl_pcl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace fg::slhost
{
namespace
{
using InitFn = PFun_slInit;
using ShutdownFn = PFun_slShutdown;
using SetD3DDeviceFn = PFun_slSetD3DDevice;
using IsFeatureLoadedFn = PFun_slIsFeatureLoaded;
using SetFeatureLoadedFn = PFun_slSetFeatureLoaded;
using IsFeatureSupportedFn = PFun_slIsFeatureSupported;
using GetFeatureRequirementsFn = PFun_slGetFeatureRequirements;
using GetFeatureFunctionFn = PFun_slGetFeatureFunction;
using UpgradeInterfaceFn = PFun_slUpgradeInterface;
using GetNativeInterfaceFn = PFun_slGetNativeInterface;
using GetNewFrameTokenFn = PFun_slGetNewFrameToken;
using SetTagForFrameFn = PFun_slSetTagForFrame;
using SetConstantsFn = PFun_slSetConstants;

HMODULE g_interposer = nullptr;
InitFn *g_init = nullptr;
ShutdownFn *g_shutdown = nullptr;
SetD3DDeviceFn *g_set_d3d_device = nullptr;
IsFeatureLoadedFn *g_is_feature_loaded = nullptr;
SetFeatureLoadedFn *g_set_feature_loaded = nullptr;
IsFeatureSupportedFn *g_is_feature_supported = nullptr;
GetFeatureRequirementsFn *g_get_feature_requirements = nullptr;
GetFeatureFunctionFn *g_get_feature_function = nullptr;
UpgradeInterfaceFn *g_upgrade_interface = nullptr;
GetNativeInterfaceFn *g_get_native_interface = nullptr;
GetNewFrameTokenFn *g_get_new_frame_token = nullptr;
SetTagForFrameFn *g_set_tag_for_frame = nullptr;
SetConstantsFn *g_set_constants = nullptr;

PFun_slDLSSGSetOptions *g_set_options = nullptr;
PFun_slDLSSGGetState *g_get_state = nullptr;
PFun_slReflexSetOptions *g_reflex_set_options = nullptr;
PFun_slReflexSleep *g_reflex_sleep = nullptr;
PFun_slPCLSetMarker *g_pcl_set_marker = nullptr;

std::atomic_bool g_requested_enabled{false};
std::atomic_int g_requested_multiplier{2};
std::atomic_bool g_depth_inverted{false};
std::atomic_bool g_force_probe{true};
std::atomic_bool g_slinit_attempted{false};
std::atomic<reshade::api::device_api> g_game_api{reshade::api::device_api::d3d11};

std::mutex g_state_mutex;
Snapshot g_state{};

ULONGLONG g_last_probe_ms = 0;
ULONGLONG g_last_poll_ms = 0;
std::atomic<uint64_t> g_app_presents_since_poll{0};
std::atomic_bool g_reset_fps_counters{true};
uint64_t g_dlssg_presents_since_poll = 0;
uint32_t g_frame_index = 0;
std::atomic_bool g_reset_next_capture{true};
bool g_dlssg_was_enabled = false;

// v0.8 never calls Streamline/DXGI Present from inside ReShade's reshade_present callback.
// The game thread only copies the completed NR frame into a free bridge slot and signals an
// input fence. A dedicated worker owns frame tokens, DLSS-G calls and the private swapchain.
std::atomic_bool g_fg_worker_stop{false};
std::atomic_bool g_fg_worker_running{false};
std::atomic_bool g_fg_worker_busy{false};
HANDLE g_fg_worker_handle = nullptr;
HANDLE g_fg_worker_wake = nullptr;
DWORD g_fg_worker_id = 0;
std::atomic<uint64_t> g_enqueued_frames{0};
std::atomic<uint64_t> g_dropped_frames{0};
// DLSS-G invokes this from the Present thread for asynchronous DXGI errors. Keep the
// callback lock-free and consume the HRESULT after Present returns.
std::atomic<long> g_last_dlssg_api_error{S_OK};

std::wstring g_plugin_dir;
std::array<const wchar_t *, 1> g_plugin_paths{};
constexpr std::array<sl::Feature, 3> k_requested_features = {
    sl::kFeaturePCL,
    sl::kFeatureReflex,
    sl::kFeatureDLSS_G,
};

constexpr const char *k_project_id = "68c3c204-a7b9-43e0-a319-37b62eef12f7";
constexpr const char *k_engine_version = "ReShadeFrameGen-DLSSGHost-0.8-SafeContract";
const sl::ViewportHandle k_viewport{0};

// The private endpoint is deliberately created with the real system DLLs rather than the game's
// proxy dxgi.dll. ReShade continues to own the game's D3D11 swapchain; Streamline owns this D3D12
// swapchain. This keeps the two presentation stacks from wrapping each other recursively.
HMODULE g_system_d3d12 = nullptr;
HMODULE g_system_dxgi = nullptr;
using D3D12CreateDeviceProc = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
using CreateDXGIFactory2Proc = HRESULT (WINAPI *)(UINT, REFIID, void **);
D3D12CreateDeviceProc g_d3d12_create_device = nullptr;
CreateDXGIFactory2Proc g_create_dxgi_factory2 = nullptr;

// ReShade D3D/DXGI proxies expose their original COM object through this private IID.
// v0.8 unwraps BOTH the private D3D12 device and the factory before giving them to Streamline.
// RenoDX does not need a ReShade wrapper around this FG-only device: its NGX detours are process
// wide and the v0.6 log already proved it sees feature 11. Keeping a single native identity here
// avoids proxy-on-proxy command queues/resources while the game's Feeder/NR device remains intact.
constexpr GUID k_reshade_unwrapped_object =
    { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };

ComPtr<ID3D11Device> g_game_device11;
ComPtr<ID3D11Device1> g_game_device11_1;
ComPtr<ID3D11Device5> g_game_device11_5;
ComPtr<ID3D11DeviceContext> g_game_context11;
ComPtr<ID3D11DeviceContext4> g_game_context11_4;
ComPtr<IDXGIAdapter1> g_game_adapter;

ComPtr<ID3D12Device> g_device12;
ComPtr<ID3D12Device> g_proxy_device12;
ComPtr<ID3D12CommandQueue> g_proxy_queue12;
ComPtr<ID3D12CommandQueue> g_native_queue12;
ComPtr<IDXGIFactory4> g_native_factory;
ComPtr<IDXGIFactory4> g_proxy_factory;
ComPtr<IDXGISwapChain3> g_proxy_swapchain;
ComPtr<IDXGISwapChain3> g_native_swapchain;

constexpr uint32_t k_frame_ring_size = 1;

struct CommandSlot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    // Separate post-Present list. It is queued only after NVIDIA has finished consuming
    // MV/depth and returns the shared guide resources to COMMON before D3D11 can reuse them.
    ComPtr<ID3D12CommandAllocator> release_allocator;
    ComPtr<ID3D12GraphicsCommandList> release_list;
    uint64_t completed_value = 0;
};
std::array<CommandSlot, k_frame_ring_size> g_command_slots{};
ComPtr<ID3D12GraphicsCommandList> g_command_list;

// Cross-API synchronization stays directional even in the single-flight baseline.
// D3D11 signals input readiness; D3D12 signals safe reuse. Keeping independent timelines avoids
// the bidirectional-fence ordering mistake from the early prototypes.
ComPtr<ID3D12Fence> g_input_fence12;
ComPtr<ID3D11Fence> g_input_fence11;
ComPtr<ID3D12Fence> g_release_fence12;
HANDLE g_input_fence_event = nullptr;
HANDLE g_release_fence_event = nullptr;
uint64_t g_input_fence_value = 0;
uint64_t g_release_fence_value = 0;

std::mutex g_note_log_mutex;
std::string g_last_logged_note;

// Shared textures must use a typed member of the backbuffer's format family.
// This mirrors the proven DLSS5-Feeder D3D11<->D3D12 transport and avoids
// CreateSharedHandle / swapchain failures on typeless and sRGB backbuffers.
DXGI_FORMAT typed_share_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

struct SharedTexture
{
    ComPtr<ID3D12Resource> d12;
    ComPtr<ID3D11Texture2D> d11;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t mip_levels = 0;
    uint16_t array_size = 0;

    bool matches(const D3D11_TEXTURE2D_DESC &desc) const
    {
        return d12 && d11 && format == typed_share_format(desc.Format) && width == desc.Width && height == desc.Height &&
               mip_levels == desc.MipLevels && array_size == desc.ArraySize && desc.SampleDesc.Count == 1;
    }

    void reset()
    {
        d11.Reset();
        d12.Reset();
        format = DXGI_FORMAT_UNKNOWN;
        width = height = 0;
        mip_levels = array_size = 0;
    }
};

enum class BridgeSlotState : uint32_t
{
    free = 0,
    capturing,
    queued,
    processing,
    wait_release,
};

struct BridgeSlot
{
    SharedTexture color;
    SharedTexture mv;
    SharedTexture depth;
    std::atomic<BridgeSlotState> state{BridgeSlotState::free};
    uint64_t input_ready_value = 0;
    uint64_t release_value = 0;
    uint32_t frame_number = 0;
    bool reset_history = false;
    D3D11_TEXTURE2D_DESC color_desc{};
    D3D11_TEXTURE2D_DESC mv_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};

    bool matches(const D3D11_TEXTURE2D_DESC &color_desc_in,
                 const D3D11_TEXTURE2D_DESC &mv_desc_in,
                 const D3D11_TEXTURE2D_DESC &depth_desc_in) const
    {
        return color.matches(color_desc_in) && mv.matches(mv_desc_in) && depth.matches(depth_desc_in);
    }

    void reset()
    {
        color.reset();
        mv.reset();
        depth.reset();
        input_ready_value = 0;
        release_value = 0;
        frame_number = 0;
        reset_history = false;
        color_desc = {};
        mv_desc = {};
        depth_desc = {};
        state.store(BridgeSlotState::free, std::memory_order_release);
    }
};

std::array<BridgeSlot, k_frame_ring_size> g_bridge_slots{};

bool all_bridge_slots_idle()
{
    if (g_fg_worker_busy.load(std::memory_order_acquire))
        return false;
    for (const auto &slot : g_bridge_slots)
        if (slot.state.load(std::memory_order_acquire) != BridgeSlotState::free)
            return false;
    return true;
}

void reclaim_completed_bridge_slots()
{
    if (!g_release_fence12)
        return;
    const uint64_t completed = g_release_fence12->GetCompletedValue();
    for (auto &slot : g_bridge_slots)
    {
        if (slot.state.load(std::memory_order_acquire) == BridgeSlotState::wait_release &&
            slot.release_value != 0 && completed >= slot.release_value)
        {
            slot.input_ready_value = 0;
            slot.release_value = 0;
            slot.state.store(BridgeSlotState::free, std::memory_order_release);
        }
    }
}

int acquire_free_bridge_slot()
{
    reclaim_completed_bridge_slots();
    for (uint32_t i = 0; i < k_frame_ring_size; ++i)
    {
        auto expected = BridgeSlotState::free;
        if (g_bridge_slots[i].state.compare_exchange_strong(expected, BridgeSlotState::capturing,
                                                             std::memory_order_acq_rel))
            return static_cast<int>(i);
    }
    return -1;
}

int acquire_queued_bridge_slot()
{
    // Slots can be recycled independently. Always choose the oldest queued real frame rather than
    // the lowest array index, otherwise a freshly-reused slot 0 could jump ahead of older frames
    // still waiting in slots 1/2 and feed DLSS-G out of temporal order.
    for (;;)
    {
        int candidate = -1;
        uint32_t oldest_frame = static_cast<uint32_t>(-1);
        for (uint32_t i = 0; i < k_frame_ring_size; ++i)
        {
            if (g_bridge_slots[i].state.load(std::memory_order_acquire) != BridgeSlotState::queued)
                continue;
            const uint32_t frame = g_bridge_slots[i].frame_number;
            if (candidate < 0 || frame < oldest_frame)
            {
                candidate = static_cast<int>(i);
                oldest_frame = frame;
            }
        }

        if (candidate < 0)
            return -1;

        auto expected = BridgeSlotState::queued;
        if (g_bridge_slots[static_cast<uint32_t>(candidate)].state.compare_exchange_strong(
                expected, BridgeSlotState::processing, std::memory_order_acq_rel))
            return candidate;
        // A disable/drop may have raced the scan. Rescan rather than processing a stale slot.
    }
}

void drop_queued_bridge_slots()
{
    for (auto &slot : g_bridge_slots)
    {
        auto expected = BridgeSlotState::queued;
        slot.state.compare_exchange_strong(expected, BridgeSlotState::free, std::memory_order_acq_rel);
    }
}

HWND g_game_hwnd = nullptr;
HWND g_endpoint_hwnd = nullptr;
ATOM g_window_class = 0;
uint32_t g_endpoint_width = 0;
uint32_t g_endpoint_height = 0;
DXGI_FORMAT g_endpoint_format = DXGI_FORMAT_UNKNOWN;
uint32_t g_endpoint_backbuffer_count = 0;
bool g_allow_tearing = false;

void on_dlssg_api_error(const sl::APIError &error)
{
    g_last_dlssg_api_error.store(static_cast<long>(error.hres), std::memory_order_release);
}

void set_note(Snapshot &s, const char *note)
{
    const std::string next = note != nullptr ? note : "";
    s.bootstrap_note = next;

    // Keep the overlay functional and compact. Detailed stage changes go to ReShade.log,
    // and are de-duplicated so a successful per-frame path does not spam the file.
    if (!next.empty())
    {
        std::lock_guard log_lock(g_note_log_mutex);
        if (next != g_last_logged_note)
        {
            const std::string line = "[ReShadeFrameGen] " + next;
            reshade::log::message(reshade::log::level::info, line.c_str());
            g_last_logged_note = next;
        }
    }
}

void set_hr(Snapshot &s, HRESULT hr, const char *note)
{
    s.endpoint_hr = hr;
    if (FAILED(hr))
        set_note(s, note);
}

bool load_system_graphics_exports(Snapshot &s)
{
    if (g_d3d12_create_device != nullptr && g_create_dxgi_factory2 != nullptr)
        return true;

    if (g_system_d3d12 == nullptr)
        g_system_d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_system_dxgi == nullptr)
        g_system_dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (g_system_d3d12 != nullptr)
        g_d3d12_create_device = reinterpret_cast<D3D12CreateDeviceProc>(GetProcAddress(g_system_d3d12, "D3D12CreateDevice"));
    if (g_system_dxgi != nullptr)
        g_create_dxgi_factory2 = reinterpret_cast<CreateDXGIFactory2Proc>(GetProcAddress(g_system_dxgi, "CreateDXGIFactory2"));

    if (g_d3d12_create_device == nullptr || g_create_dxgi_factory2 == nullptr)
    {
        set_note(s, "Could not resolve the real system D3D12/DXGI exports for the private endpoint.");
        s.endpoint_hr = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    return true;
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
    g_set_feature_loaded = reinterpret_cast<SetFeatureLoadedFn *>(GetProcAddress(module, "slSetFeatureLoaded"));
    g_is_feature_supported = reinterpret_cast<IsFeatureSupportedFn *>(GetProcAddress(module, "slIsFeatureSupported"));
    g_get_feature_requirements = reinterpret_cast<GetFeatureRequirementsFn *>(GetProcAddress(module, "slGetFeatureRequirements"));
    g_get_feature_function = reinterpret_cast<GetFeatureFunctionFn *>(GetProcAddress(module, "slGetFeatureFunction"));
    g_upgrade_interface = reinterpret_cast<UpgradeInterfaceFn *>(GetProcAddress(module, "slUpgradeInterface"));
    g_get_native_interface = reinterpret_cast<GetNativeInterfaceFn *>(GetProcAddress(module, "slGetNativeInterface"));
    g_get_new_frame_token = reinterpret_cast<GetNewFrameTokenFn *>(GetProcAddress(module, "slGetNewFrameToken"));
    g_set_tag_for_frame = reinterpret_cast<SetTagForFrameFn *>(GetProcAddress(module, "slSetTagForFrame"));
    g_set_constants = reinterpret_cast<SetConstantsFn *>(GetProcAddress(module, "slSetConstants"));

    s.core_exports_ready = g_init && g_set_d3d_device && g_is_feature_loaded && g_set_feature_loaded &&
                           g_is_feature_supported && g_get_feature_requirements && g_get_feature_function &&
                           g_upgrade_interface && g_get_native_interface && g_get_new_frame_token &&
                           g_set_tag_for_frame && g_set_constants;
}

void query_requirements(Snapshot &s)
{
    if (g_get_feature_requirements == nullptr || !s.streamline_initialized)
        return;

    sl::FeatureRequirements req{};
    s.last_requirements = (*g_get_feature_requirements)(sl::kFeatureDLSS_G, req);
    s.requirements_queried = s.last_requirements == sl::Result::eOk;
    if (!s.requirements_queried)
        return;

    const uint32_t flags = static_cast<uint32_t>(req.flags);
    const auto has = [flags](sl::FeatureRequirementFlags bit) {
        return (flags & static_cast<uint32_t>(bit)) != 0;
    };
    s.requirement_d3d11 = has(sl::FeatureRequirementFlags::eD3D11Supported);
    s.requirement_d3d12 = has(sl::FeatureRequirementFlags::eD3D12Supported);
    s.requirement_vulkan = has(sl::FeatureRequirementFlags::eVulkanSupported);
    s.requirement_hags = has(sl::FeatureRequirementFlags::eHardwareSchedulingRequired);
    s.requirement_vsync_off = has(sl::FeatureRequirementFlags::eVSyncOffRequired);
}

void resolve_feature_functions(Snapshot &s)
{
    if (g_is_feature_loaded == nullptr || g_get_feature_function == nullptr || !s.endpoint_device_submitted)
        return;

    bool loaded = false;
    s.last_is_loaded = (*g_is_feature_loaded)(sl::kFeatureDLSS_G, loaded);
    s.feature_loaded = s.last_is_loaded == sl::Result::eOk && loaded;

    if (s.feature_loaded && (g_set_options == nullptr || g_get_state == nullptr))
    {
        void *set_ptr = nullptr;
        void *get_ptr = nullptr;
        const sl::Result r_set = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_ptr);
        const sl::Result r_get = (*g_get_feature_function)(sl::kFeatureDLSS_G, "slDLSSGGetState", get_ptr);
        s.last_get_function = r_set != sl::Result::eOk ? r_set : r_get;
        if (r_set == sl::Result::eOk && r_get == sl::Result::eOk && set_ptr && get_ptr)
        {
            g_set_options = reinterpret_cast<PFun_slDLSSGSetOptions *>(set_ptr);
            g_get_state = reinterpret_cast<PFun_slDLSSGGetState *>(get_ptr);
        }
    }
    s.feature_functions_ready = g_set_options && g_get_state;

    if (g_reflex_set_options == nullptr)
    {
        void *ptr = nullptr;
        if ((*g_get_feature_function)(sl::kFeatureReflex, "slReflexSetOptions", ptr) == sl::Result::eOk && ptr)
            g_reflex_set_options = reinterpret_cast<PFun_slReflexSetOptions *>(ptr);
    }
    if (g_reflex_sleep == nullptr)
    {
        void *ptr = nullptr;
        if ((*g_get_feature_function)(sl::kFeatureReflex, "slReflexSleep", ptr) == sl::Result::eOk && ptr)
            g_reflex_sleep = reinterpret_cast<PFun_slReflexSleep *>(ptr);
    }
    s.reflex_functions_ready = g_reflex_set_options != nullptr;

    if (g_pcl_set_marker == nullptr)
    {
        void *ptr = nullptr;
        if ((*g_get_feature_function)(sl::kFeaturePCL, "slPCLSetMarker", ptr) == sl::Result::eOk && ptr)
            g_pcl_set_marker = reinterpret_cast<PFun_slPCLSetMarker *>(ptr);
    }
    s.pcl_functions_ready = g_pcl_set_marker != nullptr;
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
        set_note(s, "sl.interposer.dll was not found beside the game EXE (./streamline is fallback only).");
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
    else
    {
        s.interposer_loaded = true;
    }

    resolve_core_exports(module, s);
    if (!s.core_exports_ready)
    {
        set_note(s, "sl.interposer.dll loaded, but v0.8 could not resolve the manual-hooking/frame-tagging exports it needs.");
        std::lock_guard lock(g_state_mutex);
        g_state = s;
        return false;
    }

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

        // D3D11 games feed a PRIVATE SAME-ADAPTER D3D12 endpoint. The game's
        // D3D11 device remains native and is never handed to DLSS-G.
        pref.renderAPI = sl::RenderAPI::eD3D12;
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                     sl::PreferenceFlags::eUseManualHooking |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;

        s.last_init = (*g_init)(pref, sl::kSDKVersion);
        s.initialized_by_us = s.last_init == sl::Result::eOk;
        s.streamline_initialized = s.initialized_by_us;
        s.dlssg_requested = true;
        s.reflex_requested = true;
        s.pcl_requested = true;

        if (!s.initialized_by_us)
        {
            set_note(s, "slInit failed while preparing the private D3D12 DLSS-G endpoint. Check the Streamline log beside the game EXE.");
            std::lock_guard lock(g_state_mutex);
            g_state = s;
            return false;
        }

        query_requirements(s);
        set_note(s, "Streamline initialized as D3D12. Waiting for the D3D11 game device so v0.8 can create the same-adapter D3D12 endpoint.");
    }
    else if (!early_device_creation)
    {
        set_note(s, "DLL state refreshed. A cold restart is required to retry slInit; v0.8 never calls slInit a second time late.");
    }

    std::lock_guard lock(g_state_mutex);
    g_state = s;
    return true;
}

LRESULT CALLBACK endpoint_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    default: return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

bool ensure_endpoint_window(Snapshot &s)
{
    if (g_endpoint_hwnd != nullptr && IsWindow(g_endpoint_hwnd))
    {
        s.endpoint_window_ready = true;
        return true;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (g_window_class == 0)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = endpoint_wndproc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ReShadeFrameGenDLSSGEndpoint";
        g_window_class = RegisterClassExW(&wc);
        if (g_window_class == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Could not register the DLSS-G endpoint window class.");
            return false;
        }
    }

    g_endpoint_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"ReShadeFrameGenDLSSGEndpoint", L"ReShadeFrameGen DLSS-G Endpoint",
        WS_POPUP,
        0, 0, 16, 16,
        nullptr, nullptr, instance, nullptr);
    if (g_endpoint_hwnd == nullptr)
    {
        set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Could not create the DLSS-G endpoint window.");
        return false;
    }

    ShowWindow(g_endpoint_hwnd, SW_HIDE);
    s.endpoint_window_ready = true;
    return true;
}

void pump_endpoint_messages()
{
    MSG msg{};
    while (g_endpoint_hwnd != nullptr && PeekMessageW(&msg, g_endpoint_hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool align_endpoint_window(bool show)
{
    if (g_endpoint_hwnd == nullptr || g_game_hwnd == nullptr || !IsWindow(g_game_hwnd))
        return false;

    if (!show || IsIconic(g_game_hwnd))
    {
        ShowWindow(g_endpoint_hwnd, SW_HIDE);
        return false;
    }

    // Avoid leaving a top-most FG surface over other applications when the game loses focus.
    const HWND foreground = GetForegroundWindow();
    if (foreground != g_game_hwnd && foreground != g_endpoint_hwnd)
    {
        ShowWindow(g_endpoint_hwnd, SW_HIDE);
        return false;
    }

    RECT rc{};
    POINT origin{0, 0};
    if (!GetClientRect(g_game_hwnd, &rc) || !ClientToScreen(g_game_hwnd, &origin))
        return false;
    const int width = std::max<LONG>(1, rc.right - rc.left);
    const int height = std::max<LONG>(1, rc.bottom - rc.top);
    SetWindowPos(g_endpoint_hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    return true;
}

bool wait_for_release_value(uint64_t value)
{
    if (value == 0 || !g_release_fence12)
        return true;
    if (g_release_fence12->GetCompletedValue() >= value)
        return true;
    if (g_release_fence_event == nullptr)
        return false;
    if (FAILED(g_release_fence12->SetEventOnCompletion(value, g_release_fence_event)))
        return false;
    return WaitForSingleObject(g_release_fence_event, 2000) == WAIT_OBJECT_0;
}

void destroy_swapchain()
{
    if (g_endpoint_hwnd)
        ShowWindow(g_endpoint_hwnd, SW_HIDE);
    g_native_swapchain.Reset();
    g_proxy_swapchain.Reset();
    g_endpoint_width = g_endpoint_height = 0;
    g_endpoint_format = DXGI_FORMAT_UNKNOWN;
    g_endpoint_backbuffer_count = 0;
}

void destroy_bridge_resources()
{
    // Resource destruction is rare (resize/shutdown), so it is fine to drain here. Normal frame
    // reuse is handled by a single-flight bridge and never waits on the immediately previous frame.
    for (auto &slot : g_bridge_slots)
    {
        if (slot.release_value != 0)
            wait_for_release_value(slot.release_value);
        slot.reset();
    }
    for (auto &slot : g_command_slots)
        slot.completed_value = 0;
    g_reset_next_capture.store(true, std::memory_order_release);
}

bool create_shared_texture(const D3D11_TEXTURE2D_DESC &src, SharedTexture &out, Snapshot &s)
{
    if (!g_device12 || !g_game_device11_1 || src.Width == 0 || src.Height == 0 || src.SampleDesc.Count != 1)
        return false;

    const DXGI_FORMAT share_format = typed_share_format(src.Format);

    // First try the natural D3D12 -> D3D11 direction. This works on many drivers.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = src.Width;
    desc.Height = src.Height;
    desc.DepthOrArraySize = static_cast<UINT16>(std::max<UINT>(1, src.ArraySize));
    desc.MipLevels = static_cast<UINT16>(std::max<UINT>(1, src.MipLevels));
    desc.Format = share_format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    ComPtr<ID3D12Resource> d12;
    ComPtr<ID3D11Texture2D> d11;
    HANDLE handle = nullptr;

    HRESULT first_hr = g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&d12));
    if (SUCCEEDED(first_hr))
        first_hr = g_device12->CreateSharedHandle(d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(first_hr) && handle != nullptr)
        first_hr = g_game_device11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&d11));
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }

    if (FAILED(first_hr) || !d12 || !d11)
    {
        // Important: some D3D11 drivers reject resources created in the D3D12 -> D3D11
        // direction. DLSS5-Feeder already handles this in production by reversing ownership:
        // create the NT-handle texture on D3D11, then open that same allocation on D3D12.
        d11.Reset();
        d12.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = src.Width;
        td.Height = src.Height;
        td.MipLevels = std::max<UINT>(1, src.MipLevels);
        td.ArraySize = std::max<UINT>(1, src.ArraySize);
        td.Format = share_format;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = g_game_device11_1->CreateTexture2D(&td, nullptr, &d11);
        ComPtr<IDXGIResource1> dxgi_resource;
        if (SUCCEEDED(hr))
            hr = d11.As(&dxgi_resource);
        if (SUCCEEDED(hr))
            hr = dxgi_resource->CreateSharedHandle(nullptr,
                                                   DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   nullptr, &handle);
        if (SUCCEEDED(hr) && handle != nullptr)
            hr = g_device12->OpenSharedHandle(handle, IID_PPV_ARGS(&d12));
        if (handle != nullptr)
            CloseHandle(handle);

        if (FAILED(hr) || !d11 || !d12)
        {
            set_hr(s, FAILED(hr) ? hr : first_hr,
                   "Both D3D12->D3D11 and D3D11->D3D12 shared-texture creation failed.");
            return false;
        }
    }

    out.reset();
    out.d12 = d12;
    out.d11 = d11;
    out.format = share_format;
    out.width = src.Width;
    out.height = src.Height;
    out.mip_levels = static_cast<uint16_t>(std::max<UINT>(1, src.MipLevels));
    out.array_size = static_cast<uint16_t>(std::max<UINT>(1, src.ArraySize));
    return true;
}

bool create_shared_fences(Snapshot &s)
{
    if (g_input_fence12 && g_input_fence11 && g_release_fence12 && g_game_context11_4)
        return true;
    if (!g_device12 || !g_game_device11_5 || !g_game_context11_4)
    {
        set_note(s, "D3D11/D3D12 shared-fence interop is unavailable (ID3D11Device5 / ID3D11DeviceContext4 required).");
        return false;
    }

    g_input_fence11.Reset();
    g_input_fence12.Reset();
    g_release_fence12.Reset();
    g_input_fence_value = 0;
    g_release_fence_value = 0;

    // Only the input fence crosses APIs: D3D11 signals after copying Color/MV/Depth and the FG
    // worker waits on the D3D12 side. The release fence is native D3D12 only; the game thread
    // merely polls GetCompletedValue() to decide whether a ring slot is reusable.
    HRESULT hr = g_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_input_fence12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to create the shared D3D11->D3D12 input fence.");
        return false;
    }

    HANDLE handle = nullptr;
    hr = g_device12->CreateSharedHandle(g_input_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr) || handle == nullptr)
    {
        set_hr(s, FAILED(hr) ? hr : E_FAIL, "Failed to export the D3D11->D3D12 input fence.");
        return false;
    }

    hr = g_game_device11_5->OpenSharedFence(handle, IID_PPV_ARGS(&g_input_fence11));
    CloseHandle(handle);
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D11 failed to open the D3D11->D3D12 input fence.");
        return false;
    }

    hr = g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_release_fence12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to create the native D3D12 ring-release fence.");
        return false;
    }

    if (g_input_fence_event == nullptr)
        g_input_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_release_fence_event == nullptr)
        g_release_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_input_fence_event == nullptr || g_release_fence_event == nullptr)
    {
        set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Failed to create the endpoint fence events.");
        return false;
    }
    return true;
}

bool create_command_objects(Snapshot &s)
{
    if (g_command_list && g_command_slots[0].allocator &&
        g_command_slots[0].release_allocator && g_command_slots[0].release_list)
        return true;
    if (!g_device12)
        return false;

    for (auto &slot : g_command_slots)
    {
        HRESULT hr = g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(hr))
        {
            set_hr(s, hr, "Failed to create a D3D12 command allocator for the endpoint.");
            return false;
        }

        hr = g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.release_allocator));
        if (FAILED(hr))
        {
            set_hr(s, hr, "Failed to create the D3D12 guide-release allocator.");
            return false;
        }

        hr = g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           slot.release_allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&slot.release_list));
        if (FAILED(hr))
        {
            set_hr(s, hr, "Failed to create the D3D12 guide-release command list.");
            return false;
        }
        slot.release_list->Close();
    }

    HRESULT hr = g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                g_command_slots[0].allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&g_command_list));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to create the D3D12 endpoint command list.");
        return false;
    }
    g_command_list->Close();
    return true;
}

bool create_private_d3d12_endpoint(ID3D11Device *game_device, Snapshot &s)
{
    if (game_device == nullptr || !s.streamline_initialized || !s.core_exports_ready)
        return false;
    if (!load_system_graphics_exports(s))
        return false;

    // This function is intentionally idempotent. Earlier builds returned as soon as g_device12
    // existed, which permanently stranded a partially-built endpoint after any one-time failure.
    g_game_device11 = game_device;
    if (!g_game_device11_1)
        game_device->QueryInterface(IID_PPV_ARGS(&g_game_device11_1));
    if (!g_game_device11_5)
        game_device->QueryInterface(IID_PPV_ARGS(&g_game_device11_5));
    if (!g_game_context11)
        game_device->GetImmediateContext(&g_game_context11);
    if (g_game_context11 && !g_game_context11_4)
        g_game_context11.As(&g_game_context11_4);

    if (!g_game_device11_1 || !g_game_device11_5 || !g_game_context11_4)
    {
        set_note(s, "The D3D11 device does not expose the interop interfaces required by the D3D12 frame-generation endpoint.");
        return false;
    }
    s.game_d3d11_seen = true;

    if (!g_game_adapter)
    {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = game_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (SUCCEEDED(hr))
            hr = dxgi_device->GetAdapter(&adapter);
        if (FAILED(hr) || !adapter)
        {
            set_hr(s, hr, "Could not resolve the D3D11 game's DXGI adapter for the same-adapter D3D12 endpoint.");
            return false;
        }
        adapter.As(&g_game_adapter);
    }

    if (!g_device12)
    {
        // ReShade detours D3D12CreateDevice process-wide, even when the export was resolved from
        // System32. Therefore first capture whatever interface comes back, then explicitly unwrap
        // ReShade's proxy before Streamline ever sees it. All endpoint allocators/resources/fences
        // are built from this same native ID3D12Device identity.
        ComPtr<ID3D12Device> created_device;
        HRESULT hr = g_d3d12_create_device(g_game_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created_device));
        if (FAILED(hr) || !created_device)
        {
            set_hr(s, hr, "D3D12CreateDevice failed on the D3D11 game's adapter. DLSS-G endpoint cannot start.");
            return false;
        }

        IUnknown *unwrapped = nullptr;
        const HRESULT unwrap_hr = created_device->QueryInterface(k_reshade_unwrapped_object,
                                                                  reinterpret_cast<void **>(&unwrapped));
        if (SUCCEEDED(unwrap_hr) && unwrapped != nullptr)
        {
            hr = unwrapped->QueryInterface(IID_PPV_ARGS(&g_device12));
            unwrapped->Release();
            if (FAILED(hr) || !g_device12)
            {
                set_hr(s, hr, "ReShade exposed its native D3D12 device, but ID3D12Device recovery failed.");
                return false;
            }
            set_note(s, "Native D3D12 endpoint recovered behind ReShade; Streamline will use one device identity.");
        }
        else
        {
            // If ReShade did not wrap this call, the returned device is already native.
            g_device12 = created_device;
        }
    }
    s.endpoint_device_created = g_device12 != nullptr;

    if (!s.endpoint_device_submitted)
    {
        s.last_set_device = (*g_set_d3d_device)(g_device12.Get());
        s.endpoint_device_submitted = s.last_set_device == sl::Result::eOk;
    }
    if (!s.endpoint_device_submitted)
    {
        set_note(s, "Private D3D12 device exists, but slSetD3DDevice rejected it.");
        return false;
    }

    query_requirements(s);

    if (g_game_adapter && g_is_feature_supported && !s.endpoint_feature_supported)
    {
        DXGI_ADAPTER_DESC1 ad{};
        if (SUCCEEDED(g_game_adapter->GetDesc1(&ad)))
        {
            sl::AdapterInfo info{};
            info.deviceLUID = reinterpret_cast<uint8_t *>(&ad.AdapterLuid);
            info.deviceLUIDSizeInBytes = sizeof(ad.AdapterLuid);
            s.last_supported = (*g_is_feature_supported)(sl::kFeatureDLSS_G, info);
            s.endpoint_feature_supported = s.last_supported == sl::Result::eOk;
        }
    }
    if (!s.endpoint_feature_supported)
    {
        set_note(s, "NVIDIA Streamline reports DLSS-G unsupported on the selected game adapter.");
        return false;
    }

    bool loaded = false;
    s.last_is_loaded = (*g_is_feature_loaded)(sl::kFeatureDLSS_G, loaded);
    if (s.last_is_loaded != sl::Result::eOk || !loaded)
    {
        s.endpoint_feature_load_attempted = true;
        s.last_set_feature_loaded = (*g_set_feature_loaded)(sl::kFeatureDLSS_G, true);
        loaded = false;
        s.last_is_loaded = (*g_is_feature_loaded)(sl::kFeatureDLSS_G, loaded);
    }
    s.feature_loaded = s.last_is_loaded == sl::Result::eOk && loaded;
    if (!s.feature_loaded)
    {
        set_note(s, "The D3D12 endpoint device is active, but Streamline could not load DLSS-G.");
        return false;
    }

    resolve_feature_functions(s);
    if (!s.feature_functions_ready)
    {
        set_note(s, "DLSS-G loaded, but its SetOptions/GetState entry points are not available yet.");
        return false;
    }

    if (!g_proxy_device12)
    {
        ID3D12Device *proxy_device = g_device12.Get();
        s.last_upgrade_device = (*g_upgrade_interface)(reinterpret_cast<void **>(&proxy_device));
        if (s.last_upgrade_device != sl::Result::eOk || proxy_device == nullptr)
        {
            set_note(s, "slUpgradeInterface failed for the private D3D12 device.");
            return false;
        }
        // slUpgradeInterface creates a proxy with its own initial COM reference in manual-hooking
        // mode. Adopt that reference instead of assigning through ComPtr (which would AddRef again
        // and leak the proxy). If SL ever returns the original native pointer, take our own ref.
        if (proxy_device == g_device12.Get())
            g_proxy_device12 = proxy_device;
        else
            g_proxy_device12.Attach(proxy_device);
    }
    s.proxy_device_ready = g_proxy_device12 != nullptr;

    if (!g_proxy_queue12)
    {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qdesc.NodeMask = 0;
        const HRESULT hr = g_proxy_device12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g_proxy_queue12));
        if (FAILED(hr))
        {
            set_hr(s, hr, "Streamline proxy device failed to create the presenting command queue.");
            return false;
        }
    }
    s.proxy_queue_ready = g_proxy_queue12 != nullptr;

    if (!g_native_queue12)
    {
        void *native_queue = nullptr;
        s.last_get_native_queue = (*g_get_native_interface)(g_proxy_queue12.Get(), &native_queue);
        if (s.last_get_native_queue != sl::Result::eOk || native_queue == nullptr)
        {
            set_note(s, "Streamline proxy queue exists, but slGetNativeInterface could not recover its native queue.");
            return false;
        }
        g_native_queue12.Attach(static_cast<ID3D12CommandQueue *>(native_queue));
    }
    s.native_queue_resolved = g_native_queue12 != nullptr;

    if (!g_native_factory)
    {
        // CreateDXGIFactory2 is normally intercepted by ReShade, so the first object returned here
        // is a ReShade DXGI proxy. Creating our swapchain through that proxy makes ReShade create
        // another effect_runtime. DLSS5-Feeder stores only one runtime globally, so that second
        // runtime steals Feeder away from the game's D3D11 runtime and RenoDX never sees its DLSS
        // create/evaluate calls. The D3D12 device was already unwrapped above; unwrap the factory
        // as well so neither half of the endpoint creates a second ReShade runtime.
        ComPtr<IDXGIFactory4> created_factory;
        HRESULT hr = g_create_dxgi_factory2(0, IID_PPV_ARGS(&created_factory));
        if (FAILED(hr) || !created_factory)
        {
            set_hr(s, hr, "Could not create the DXGI factory for the DLSS-G endpoint.");
            return false;
        }

        IUnknown *unwrapped = nullptr;
        const HRESULT unwrap_hr = created_factory->QueryInterface(k_reshade_unwrapped_object,
                                                                   reinterpret_cast<void **>(&unwrapped));
        if (SUCCEEDED(unwrap_hr) && unwrapped != nullptr)
        {
            hr = unwrapped->QueryInterface(IID_PPV_ARGS(&g_native_factory));
            unwrapped->Release();
            if (FAILED(hr) || !g_native_factory)
            {
                set_hr(s, hr, "ReShade exposed its unwrapped DXGI factory, but IDXGIFactory4 recovery failed.");
                return false;
            }
            set_note(s, "Native DXGI factory recovered behind ReShade; the FG swapchain will not create a second ReShade runtime.");
        }
        else
        {
            // If this call already bypassed ReShade there is nothing to unwrap. Use it directly.
            g_native_factory = created_factory;
        }
    }

    if (!g_proxy_factory)
    {
        IDXGIFactory4 *proxy_factory = g_native_factory.Get();
        s.last_upgrade_factory = (*g_upgrade_interface)(reinterpret_cast<void **>(&proxy_factory));
        if (s.last_upgrade_factory != sl::Result::eOk || proxy_factory == nullptr)
        {
            set_note(s, "slUpgradeInterface failed for the endpoint DXGI factory.");
            return false;
        }
        // Same ownership rule as the upgraded device above: adopt a newly-created SL proxy,
        // but AddRef if SL returned the original native factory unchanged.
        if (proxy_factory == g_native_factory.Get())
            g_proxy_factory = proxy_factory;
        else
            g_proxy_factory.Attach(proxy_factory);
    }
    s.proxy_factory_ready = g_proxy_factory != nullptr;

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(g_native_factory.As(&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            g_allow_tearing = allow == TRUE;
    }

    // These used to be one-shot setup operations. Keep retrying until both exist so a transient
    // interop failure cannot leave the addon displaying READY while the async worker can never run.
    if (!create_command_objects(s) || !create_shared_fences(s))
        return false;

    s.endpoint_hr = S_OK;
    set_note(s, "DLSS-G endpoint is initialized. Building the shared frame bridge.");
    return true;
}

bool create_proxy_swapchain(uint32_t width, uint32_t height, DXGI_FORMAT format, Snapshot &s)
{
    if (!g_proxy_factory || !g_proxy_queue12 || !ensure_endpoint_window(s))
        return false;

    if (g_proxy_swapchain && g_endpoint_width == width && g_endpoint_height == height && g_endpoint_format == format)
    {
        s.proxy_swapchain_ready = true;
        s.native_swapchain_resolved = g_native_swapchain != nullptr;
        return true;
    }

    destroy_swapchain();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = g_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swap1;
    const HRESULT hr = g_proxy_factory->CreateSwapChainForHwnd(g_proxy_queue12.Get(), g_endpoint_hwnd,
                                                                &desc, nullptr, nullptr, &swap1);
    if (FAILED(hr))
    {
        set_hr(s, hr, "Streamline proxy factory failed to create the D3D12 presentation swapchain.");
        return false;
    }

    if (FAILED(swap1.As(&g_proxy_swapchain)))
    {
        set_hr(s, E_NOINTERFACE, "Endpoint swapchain does not expose IDXGISwapChain3.");
        return false;
    }

    void *native = nullptr;
    s.last_get_native_swapchain = (*g_get_native_interface)(g_proxy_swapchain.Get(), &native);
    if (s.last_get_native_swapchain == sl::Result::eOk && native)
        g_native_swapchain.Attach(static_cast<IDXGISwapChain3 *>(native));
    s.native_swapchain_resolved = g_native_swapchain != nullptr;

    g_endpoint_width = width;
    g_endpoint_height = height;
    g_endpoint_format = format;
    g_endpoint_backbuffer_count = desc.BufferCount;
    DXGI_SWAP_CHAIN_DESC1 actual_desc{};
    IDXGISwapChain1 *desc_swapchain = g_native_swapchain ? static_cast<IDXGISwapChain1 *>(g_native_swapchain.Get())
                                                        : static_cast<IDXGISwapChain1 *>(g_proxy_swapchain.Get());
    if (desc_swapchain != nullptr && SUCCEEDED(desc_swapchain->GetDesc1(&actual_desc)) && actual_desc.BufferCount != 0)
        g_endpoint_backbuffer_count = actual_desc.BufferCount;
    s.endpoint_width = width;
    s.endpoint_height = height;
    s.endpoint_format = static_cast<uint32_t>(format);
    s.proxy_swapchain_ready = true;
    g_reset_next_capture.store(true, std::memory_order_release);

    // Disable the normal Alt+Enter behavior on our utility window.
    g_native_factory->MakeWindowAssociation(g_endpoint_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    return true;
}

bool ensure_frame_bridge(ID3D11Texture2D *game_color, ID3D11Texture2D *mv, ID3D11Texture2D *depth, Snapshot &s)
{
    if (!game_color || !mv || !depth)
        return false;

    D3D11_TEXTURE2D_DESC cdesc{}, mdesc{}, ddesc{};
    game_color->GetDesc(&cdesc);
    mv->GetDesc(&mdesc);
    depth->GetDesc(&ddesc);

    if (cdesc.SampleDesc.Count != 1 || mdesc.SampleDesc.Count != 1 || ddesc.SampleDesc.Count != 1)
    {
        set_note(s, "v0.8 requires single-sample color/MV/depth textures.");
        return false;
    }

    reclaim_completed_bridge_slots();

    bool rebuild = false;
    for (const auto &slot : g_bridge_slots)
        rebuild |= !slot.matches(cdesc, mdesc, ddesc);

    const DXGI_FORMAT wanted_format = typed_share_format(cdesc.Format);
    const bool swapchain_rebuild = !g_proxy_swapchain || g_endpoint_width != cdesc.Width ||
                                   g_endpoint_height != cdesc.Height || g_endpoint_format != wanted_format;

    // Resize/reformat is rare. Never destroy resources underneath the worker: simply skip FG
    // submissions until every old slot has retired, then rebuild on a later game Present.
    if ((rebuild || swapchain_rebuild) && !all_bridge_slots_idle())
    {
        set_note(s, "FG endpoint resize/reformat is waiting for the async queue to drain; the game is not blocked.");
        return false;
    }

    if (rebuild)
    {
        destroy_bridge_resources();
        for (auto &slot : g_bridge_slots)
        {
            if (!create_shared_texture(cdesc, slot.color, s) ||
                !create_shared_texture(mdesc, slot.mv, s) ||
                !create_shared_texture(ddesc, slot.depth, s))
            {
                destroy_bridge_resources();
                return false;
            }
        }
    }

    if (!create_proxy_swapchain(cdesc.Width, cdesc.Height, wanted_format, s))
        return false;

    const bool became_ready = !s.feeder_bridge_ready;
    s.feeder_bridge_ready = true;
    s.endpoint_hr = S_OK;
    if (became_ready)
        set_note(s, "Single-flight bridge ready. DLSS-G can now be enabled.");
    return true;
}

void set_identity(sl::float4x4 &m)
{
    m.row[0] = sl::float4(1.f, 0.f, 0.f, 0.f);
    m.row[1] = sl::float4(0.f, 1.f, 0.f, 0.f);
    m.row[2] = sl::float4(0.f, 0.f, 1.f, 0.f);
    m.row[3] = sl::float4(0.f, 0.f, 0.f, 1.f);
}

sl::Constants make_test_constants(uint32_t mv_width, uint32_t mv_height, uint32_t color_width, uint32_t color_height, bool reset_history)
{
    sl::Constants c{};
    set_identity(c.cameraViewToClip);
    set_identity(c.clipToCameraView);
    set_identity(c.clipToPrevClip);
    set_identity(c.prevClipToClip);
    c.jitterOffset = sl::float2(0.f, 0.f);
    // Although documented as optional, Streamline's common validator warns if this is left
    // at INVALID_FLOAT. The Feeder path does not use a pinhole offset.
    c.cameraPinholeOffset = sl::float2(0.f, 0.f);
    c.mvecScale = sl::float2(mv_width ? 1.0f / static_cast<float>(mv_width) : 1.0f,
                             mv_height ? 1.0f / static_cast<float>(mv_height) : 1.0f);
    c.cameraPos = sl::float3(0.f, 0.f, 0.f);
    c.cameraUp = sl::float3(0.f, 1.f, 0.f);
    c.cameraRight = sl::float3(1.f, 0.f, 0.f);
    c.cameraFwd = sl::float3(0.f, 0.f, 1.f);
    c.cameraNear = 0.1f;
    c.cameraFar = 1000.0f;
    c.cameraFOV = 1.0471975512f;
    c.cameraAspectRatio = color_height ? static_cast<float>(color_width) / static_cast<float>(color_height) : 1.0f;
    c.depthInverted = g_depth_inverted.load(std::memory_order_relaxed) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset_history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    return c;
}

bool emit_marker(sl::PCLMarker marker, const sl::FrameToken &token, Snapshot &s)
{
    if (!g_pcl_set_marker)
        return false;
    s.last_pcl_marker = (*g_pcl_set_marker)(marker, token);
    return s.last_pcl_marker == sl::Result::eOk;
}

bool capture_real_frame_nonblocking(reshade::api::effect_runtime *runtime, Snapshot &s)
{
    if (!runtime || !g_game_context11 || !g_game_context11_4 || !g_input_fence11)
        return false;

    guides::NativeTextures guide{};
    if (!guides::acquire_native_textures(runtime, guide))
    {
        s.feeder_mv_acquired = guide.motion_vectors != nullptr;
        s.feeder_depth_acquired = guide.depth != nullptr;
        return false;
    }
    s.feeder_mv_acquired = true;
    s.feeder_depth_acquired = true;

    const reshade::api::resource backbuffer = runtime->get_current_back_buffer();
    if (backbuffer.handle == 0)
        return false;
    auto *game_color = reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(backbuffer.handle));

    if (!ensure_frame_bridge(game_color, guide.motion_vectors, guide.depth, s))
        return false;

    const int slot_index = acquire_free_bridge_slot();
    if (slot_index < 0)
    {
        // Backpressure policy for v0.8: keep exactly one FG frame in flight until we have
        // proven the complete resource-state/lifetime contract stable. Never stall the game/NR
        // pipeline; simply skip this FG input and try again next frame.
        g_dropped_frames.fetch_add(1, std::memory_order_relaxed);
        g_reset_next_capture.store(true, std::memory_order_release);
        return false;
    }

    BridgeSlot &bridge = g_bridge_slots[static_cast<uint32_t>(slot_index)];
    game_color->GetDesc(&bridge.color_desc);
    guide.motion_vectors->GetDesc(&bridge.mv_desc);
    guide.depth->GetDesc(&bridge.depth_desc);
    bridge.frame_number = ++g_frame_index;
    bridge.reset_history = g_reset_next_capture.exchange(false, std::memory_order_acq_rel);
    bridge.release_value = 0;

    g_game_context11->CopyResource(bridge.color.d11.Get(), game_color);
    g_game_context11->CopyResource(bridge.mv.d11.Get(), guide.motion_vectors);
    g_game_context11->CopyResource(bridge.depth.d11.Get(), guide.depth);

    bridge.input_ready_value = ++g_input_fence_value;
    const HRESULT hr = g_game_context11_4->Signal(g_input_fence11.Get(), bridge.input_ready_value);
    if (FAILED(hr))
    {
        bridge.state.store(BridgeSlotState::free, std::memory_order_release);
        set_hr(s, hr, "D3D11 failed to signal the async Feeder input fence.");
        return false;
    }

    // Flush submits the D3D11 copy/fence packet but does not wait for it. The worker's native
    // D3D12 queue performs the cross-API wait, so the game's Present can continue immediately.
    g_game_context11->Flush();
    bridge.state.store(BridgeSlotState::queued, std::memory_order_release);
    g_enqueued_frames.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool retire_bridge_slot(uint32_t slot_index, Snapshot &s)
{
    BridgeSlot &bridge = g_bridge_slots[slot_index];
    CommandSlot &slot = g_command_slots[slot_index];

    // Streamline consumed the guides in NON_PIXEL_SHADER_RESOURCE. Before the shared allocation
    // can cross back to D3D11, explicitly restore it to COMMON. This mirrors DLSS5-Feeder's
    // proven D3D11<->D3D12 contract instead of assuming a third-party feature restores states.
    HRESULT hr = slot.release_allocator->Reset();
    if (SUCCEEDED(hr))
        hr = slot.release_list->Reset(slot.release_allocator.Get(), nullptr);
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to reset the D3D12 guide-release command list.");
        return false;
    }

    D3D12_RESOURCE_BARRIER release_barriers[2]{};
    release_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    release_barriers[0].Transition.pResource = bridge.mv.d12.Get();
    release_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    release_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    release_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    release_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    release_barriers[1].Transition.pResource = bridge.depth.d12.Get();
    release_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    release_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    release_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    slot.release_list->ResourceBarrier(2, release_barriers);

    hr = slot.release_list->Close();
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to close the D3D12 guide-release command list.");
        return false;
    }
    ID3D12CommandList *release_lists[] = { slot.release_list.Get() };
    g_native_queue12->ExecuteCommandLists(1, release_lists);

    const uint64_t release_value = ++g_release_fence_value;
    hr = g_native_queue12->Signal(g_release_fence12.Get(), release_value);
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to signal safe reuse of the single-flight frame bridge.");
        return false;
    }

    bridge.release_value = release_value;
    slot.completed_value = release_value;
    bridge.state.store(BridgeSlotState::wait_release, std::memory_order_release);
    return true;
}

void update_worker_fps(Snapshot &s, const sl::DLSSGState &state)
{
    if (g_reset_fps_counters.exchange(false, std::memory_order_acq_rel))
    {
        g_dlssg_presents_since_poll = 0;
        g_last_poll_ms = 0;
        g_app_presents_since_poll.store(0, std::memory_order_release);
    }

    g_dlssg_presents_since_poll += state.numFramesActuallyPresented;
    const ULONGLONG now = GetTickCount64();
    if (g_last_poll_ms == 0)
    {
        g_last_poll_ms = now;
        return;
    }
    if (now - g_last_poll_ms < 500)
        return;

    const double seconds = static_cast<double>(now - g_last_poll_ms) / 1000.0;
    g_last_poll_ms = now;
    const uint64_t app_frames = g_app_presents_since_poll.exchange(0, std::memory_order_acq_rel);
    if (seconds > 0.0)
    {
        s.app_present_fps = static_cast<double>(app_frames) / seconds;
        s.output_fps = static_cast<double>(g_dlssg_presents_since_poll) / seconds;
    }
    g_dlssg_presents_since_poll = 0;
}

bool process_bridge_slot_on_worker(uint32_t slot_index, Snapshot &s)
{
    if (!g_native_queue12 || !g_input_fence12 || !g_release_fence12 || !g_proxy_swapchain ||
        !g_command_list || !g_get_new_frame_token || !g_set_tag_for_frame || !g_set_constants ||
        !g_set_options || !g_get_state)
    {
        g_bridge_slots[slot_index].state.store(BridgeSlotState::free, std::memory_order_release);
        return false;
    }

    BridgeSlot &bridge = g_bridge_slots[slot_index];
    CommandSlot &slot = g_command_slots[slot_index];
    const D3D11_TEXTURE2D_DESC cdesc = bridge.color_desc;
    const D3D11_TEXTURE2D_DESC mdesc = bridge.mv_desc;
    const D3D11_TEXTURE2D_DESC ddesc = bridge.depth_desc;
    const uint32_t frame_number = bridge.frame_number;

    // The game thread has already submitted the copies and returned. Wait for their shared fence
    // here on the FG worker CPU thread before Streamline can inspect/tag the resources. This is
    // deliberately conservative and cannot reduce the game's simulation/render FPS.
    if (g_input_fence12->GetCompletedValue() < bridge.input_ready_value)
    {
        if (g_input_fence_event == nullptr ||
            FAILED(g_input_fence12->SetEventOnCompletion(bridge.input_ready_value, g_input_fence_event)) ||
            WaitForSingleObject(g_input_fence_event, 2000) != WAIT_OBJECT_0)
        {
            bridge.state.store(BridgeSlotState::free, std::memory_order_release);
            set_hr(s, DXGI_ERROR_DEVICE_HUNG, "Timed out waiting for the D3D11 Feeder copies on the FG worker.");
            return false;
        }
    }

    HRESULT hr = S_OK;
    sl::FrameToken *token = nullptr;
    s.last_get_frame_token = (*g_get_new_frame_token)(token, &frame_number);
    s.frame_token_ready = s.last_get_frame_token == sl::Result::eOk && token != nullptr;
    if (!s.frame_token_ready)
    {
        bridge.state.store(BridgeSlotState::free, std::memory_order_release);
        set_note(s, "slGetNewFrameToken failed on the dedicated FG thread.");
        return false;
    }

    if (g_reflex_set_options)
    {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::eLowLatency;
        s.last_reflex_options = (*g_reflex_set_options)(reflex);
        s.reflex_enabled = s.last_reflex_options == sl::Result::eOk;
    }
    s.reflex_sleep_called = false;

    bool markers_ok = true;
    markers_ok &= emit_marker(sl::PCLMarker::eSimulationStart, *token, s);
    markers_ok &= emit_marker(sl::PCLMarker::eSimulationEnd, *token, s);
    markers_ok &= emit_marker(sl::PCLMarker::eRenderSubmitStart, *token, s);

    // A slot is only returned to FREE after its release fence has completed, so its allocator is
    // guaranteed idle here. There is intentionally no CPU wait in the normal frame path.
    hr = slot.allocator->Reset();
    if (SUCCEEDED(hr))
        hr = g_command_list->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(hr))
    {
        bridge.state.store(BridgeSlotState::free, std::memory_order_release);
        set_hr(s, hr, "Failed to reset the async D3D12 endpoint command list.");
        return false;
    }

    const UINT back_index = g_proxy_swapchain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> endpoint_backbuffer;
    hr = g_proxy_swapchain->GetBuffer(back_index, IID_PPV_ARGS(&endpoint_backbuffer));
    if (FAILED(hr) || !endpoint_backbuffer)
    {
        // Reset() put the shared command list into recording state; close it before abandoning
        // this slot so the next frame can legally Reset() the list again.
        g_command_list->Close();
        bridge.state.store(BridgeSlotState::free, std::memory_order_release);
        set_hr(s, FAILED(hr) ? hr : E_FAIL, "Streamline proxy swapchain GetBuffer failed on the FG thread.");
        return false;
    }

    D3D12_RESOURCE_BARRIER begin_barriers[4]{};
    begin_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barriers[0].Transition.pResource = bridge.color.d12.Get();
    begin_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    begin_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    begin_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    begin_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barriers[1].Transition.pResource = endpoint_backbuffer.Get();
    begin_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    begin_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    begin_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    begin_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barriers[2].Transition.pResource = bridge.mv.d12.Get();
    begin_barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    begin_barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    begin_barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    begin_barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin_barriers[3].Transition.pResource = bridge.depth.d12.Get();
    begin_barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    begin_barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    begin_barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g_command_list->ResourceBarrier(4, begin_barriers);
    g_command_list->CopyResource(endpoint_backbuffer.Get(), bridge.color.d12.Get());

    D3D12_RESOURCE_BARRIER end_barriers[2] = { begin_barriers[0], begin_barriers[1] };
    std::swap(end_barriers[0].Transition.StateBefore, end_barriers[0].Transition.StateAfter);
    std::swap(end_barriers[1].Transition.StateBefore, end_barriers[1].Transition.StateAfter);
    g_command_list->ResourceBarrier(2, end_barriers);

    hr = g_command_list->Close();
    if (FAILED(hr))
    {
        bridge.state.store(BridgeSlotState::free, std::memory_order_release);
        set_hr(s, hr, "Failed to close the async D3D12 endpoint command list.");
        return false;
    }

    ID3D12CommandList *lists[] = { g_command_list.Get() };
    g_native_queue12->ExecuteCommandLists(1, lists);
    markers_ok &= emit_marker(sl::PCLMarker::eRenderSubmitEnd, *token, s);

    sl::Resource mv_res(sl::ResourceType::eTex2d, bridge.mv.d12.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    mv_res.width = mdesc.Width;
    mv_res.height = mdesc.Height;
    mv_res.nativeFormat = static_cast<uint32_t>(bridge.mv.format);
    sl::Resource depth_res(sl::ResourceType::eTex2d, bridge.depth.d12.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    depth_res.width = ddesc.Width;
    depth_res.height = ddesc.Height;
    depth_res.nativeFormat = static_cast<uint32_t>(bridge.depth.format);

    const sl::Extent mv_extent{0, 0, mdesc.Width, mdesc.Height};
    const sl::Extent depth_extent{0, 0, ddesc.Width, ddesc.Height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth_res, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depth_extent),
        sl::ResourceTag(&mv_res, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &mv_extent),
    };
    s.last_set_tags = (*g_set_tag_for_frame)(*token, k_viewport, tags,
                                              static_cast<uint32_t>(sizeof(tags) / sizeof(tags[0])), nullptr);
    s.tags_submitted = s.last_set_tags == sl::Result::eOk;
    if (!s.tags_submitted)
    {
        retire_bridge_slot(slot_index, s);
        g_reset_next_capture.store(true, std::memory_order_release);
        set_note(s, "Streamline rejected the async DLSS-G resource tags.");
        return false;
    }

    const sl::Constants constants = make_test_constants(mdesc.Width, mdesc.Height, cdesc.Width, cdesc.Height, bridge.reset_history);
    s.last_set_constants = (*g_set_constants)(constants, *token, k_viewport);
    s.constants_submitted = s.last_set_constants == sl::Result::eOk;
    if (!s.constants_submitted)
    {
        retire_bridge_slot(slot_index, s);
        g_reset_next_capture.store(true, std::memory_order_release);
        set_note(s, "Streamline rejected the async DLSS-G common constants.");
        return false;
    }

    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = static_cast<uint32_t>(std::max(1, std::clamp(g_requested_multiplier.load(std::memory_order_relaxed), 2, 6) - 1));
    if (s.max_generated_frames > 0)
        options.numFramesToGenerate = std::min(options.numFramesToGenerate, s.max_generated_frames);
    // Everything below is intentionally left at the SDK defaults. numBackBuffers, dimensions and
    // native formats are OPTIONAL hints; guessing them is worse than omitting them when DLSS-G is
    // free to virtualize/replace parts of the presentation chain.
    options.onErrorCallback = &on_dlssg_api_error;

    // SetOptions and Present run on the SAME dedicated thread. Clear the asynchronous DXGI
    // error latch before each submission so the callback can attribute an error to this Present.
    g_last_dlssg_api_error.store(S_OK, std::memory_order_release);
    s.last_set_options = (*g_set_options)(k_viewport, options);
    if (s.last_set_options != sl::Result::eOk)
    {
        retire_bridge_slot(slot_index, s);
        g_reset_next_capture.store(true, std::memory_order_release);
        set_note(s, "slDLSSGSetOptions rejected the dedicated-thread endpoint frame.");
        return false;
    }

    markers_ok &= emit_marker(sl::PCLMarker::ePresentStart, *token, s);
    s.pcl_markers_submitted = markers_ok;
    const UINT present_flags = g_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    s.endpoint_present_attempted = true;
    s.last_present_hr = g_proxy_swapchain->Present(0, present_flags);
    // Do not treat a positive DXGI status (for example an occlusion status) as a rendered frame.
    s.endpoint_present_succeeded = s.last_present_hr == S_OK;
    markers_ok &= emit_marker(sl::PCLMarker::ePresentEnd, *token, s);
    s.pcl_markers_submitted = markers_ok;
    g_dlssg_was_enabled = s.endpoint_present_succeeded;

    const HRESULT callback_hr = static_cast<HRESULT>(g_last_dlssg_api_error.load(std::memory_order_acquire));
    const HRESULT removed_hr = g_device12 ? g_device12->GetDeviceRemovedReason() : S_OK;
    if (FAILED(callback_hr) || FAILED(removed_hr))
    {
        // Restore the shared guide state before abandoning the slot, then stop submitting new FG
        // work. This is an internal fail-safe, not another overlay diagnostic mode.
        retire_bridge_slot(slot_index, s);
        g_requested_enabled.store(false, std::memory_order_release);
        g_dlssg_was_enabled = false;
        set_hr(s, FAILED(removed_hr) ? removed_hr : callback_hr,
               FAILED(removed_hr) ? "D3D12 device was removed during DLSS-G Present."
                                  : "DLSS-G reported an asynchronous DXGI Present error.");
        return false;
    }

    // Query state on the presenting thread. If NVIDIA provides an explicit input-consumption
    // fence, chain it onto our native queue before releasing this ring slot back to D3D11.
    sl::DLSSGState present_state{};
    bool bad_dlssg_status = false;
    s.last_get_state = (*g_get_state)(k_viewport, present_state, nullptr);
    s.state_queried = true;
    if (s.last_get_state == sl::Result::eOk)
    {
        s.status = present_state.status;
        s.max_generated_frames = present_state.numFramesToGenerateMax;
        s.dynamic_mfg_supported = present_state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        s.frames_presented_last_poll = present_state.numFramesActuallyPresented;
        update_worker_fps(s, present_state);
        bad_dlssg_status = present_state.status != sl::DLSSGStatus::eOk;

        if (present_state.inputsProcessingCompletionFence != nullptr &&
            present_state.lastPresentInputsProcessingCompletionFenceValue != 0)
        {
            auto *sl_completion_fence = static_cast<ID3D12Fence *>(present_state.inputsProcessingCompletionFence);
            hr = g_native_queue12->Wait(sl_completion_fence,
                                        present_state.lastPresentInputsProcessingCompletionFenceValue);
            if (FAILED(hr))
            {
                set_hr(s, hr, "FG worker failed to wait for NVIDIA input-consumption completion.");
                return false;
            }
        }
    }

    if (!retire_bridge_slot(slot_index, s))
        return false;

    // NVIDIA exposes contract failures through DLSSGState. Never submit a second frame after the
    // plugin has already told us the first contract is invalid; stop safely instead of allowing
    // an asynchronous GPU fault to cascade into the game process.
    if (bad_dlssg_status)
    {
        g_requested_enabled.store(false, std::memory_order_release);
        g_dlssg_was_enabled = false;
        if (present_state.status & sl::DLSSGStatus::eFailCommonConstantsInvalid)
            set_note(s, "DLSS-G rejected the common frame constants; Frame Generation was stopped safely.");
        else if (present_state.status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)
            set_note(s, "DLSS-G did not accept the Reflex frame cadence; Frame Generation was stopped safely.");
        else if (present_state.status & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)
            set_note(s, "DLSS-G rejected the swapchain backbuffer-index contract; Frame Generation was stopped safely.");
        else if (present_state.status & sl::DLSSGStatus::eFailHDRFormatNotSupported)
            set_note(s, "DLSS-G rejected the endpoint color format; Frame Generation was stopped safely.");
        else if (present_state.status & sl::DLSSGStatus::eFailResolutionTooLow)
            set_note(s, "DLSS-G rejected the endpoint resolution; Frame Generation was stopped safely.");
        else
            set_note(s, "DLSS-G reported a non-OK frame contract; Frame Generation was stopped safely.");
        return false;
    }

    if (!s.endpoint_present_succeeded)
    {
        g_requested_enabled.store(false, std::memory_order_release);
        g_dlssg_was_enabled = false;
        if (FAILED(s.last_present_hr))
            set_hr(s, s.last_present_hr, "Streamline proxy Present failed on the dedicated FG thread.");
        else
        {
            s.endpoint_hr = s.last_present_hr;
            set_note(s, "Streamline Present returned a non-S_OK DXGI status; Frame Generation was stopped safely.");
        }
        return false;
    }

    set_note(s, "DLSS-G ACTIVE: single-flight safe contract, explicit guide states, non-blocking game capture.");
    return true;
}

void disable_dlssg_on_worker(Snapshot &s)
{
    drop_queued_bridge_slots();
    if (!g_dlssg_was_enabled || !g_set_options)
        return;

    sl::DLSSGOptions off{};
    off.mode = sl::DLSSGMode::eOff;
    s.last_set_options = (*g_set_options)(k_viewport, off);
    g_dlssg_was_enabled = false;
}

DWORD WINAPI fg_worker_thread_proc(LPVOID)
{
    g_fg_worker_running.store(true, std::memory_order_release);

    while (!g_fg_worker_stop.load(std::memory_order_acquire))
    {
        if (g_fg_worker_wake == nullptr)
            break;
        const DWORD wait = WaitForSingleObject(g_fg_worker_wake, INFINITE);
        if (wait != WAIT_OBJECT_0)
            break;
        if (g_fg_worker_stop.load(std::memory_order_acquire))
            break;

        Snapshot s = snapshot();
        if (!requested_enabled())
        {
            disable_dlssg_on_worker(s);
            std::lock_guard lock(g_state_mutex);
            g_state = s;
            continue;
        }

        for (;;)
        {
            const int slot_index = acquire_queued_bridge_slot();
            if (slot_index < 0)
                break;

            g_fg_worker_busy.store(true, std::memory_order_release);
            process_bridge_slot_on_worker(static_cast<uint32_t>(slot_index), s);
            g_fg_worker_busy.store(false, std::memory_order_release);
            reclaim_completed_bridge_slots();

            if (!requested_enabled() || g_fg_worker_stop.load(std::memory_order_acquire))
                break;
        }

        if (!requested_enabled())
            disable_dlssg_on_worker(s);

        std::lock_guard lock(g_state_mutex);
        g_state = s;
    }

    // Disable on the same thread that owned SetOptions/Present before exiting whenever possible.
    Snapshot final_state = snapshot();
    disable_dlssg_on_worker(final_state);
    {
        std::lock_guard lock(g_state_mutex);
        g_state = final_state;
    }

    g_fg_worker_busy.store(false, std::memory_order_release);
    g_fg_worker_running.store(false, std::memory_order_release);
    return 0;
}

bool ensure_fg_worker_started(Snapshot &s)
{
    if (g_fg_worker_handle != nullptr)
        return true;

    if (g_fg_worker_wake == nullptr)
        g_fg_worker_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_fg_worker_wake == nullptr)
    {
        set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Could not create the FG worker wake event.");
        return false;
    }

    g_fg_worker_stop.store(false, std::memory_order_release);
    g_fg_worker_handle = CreateThread(nullptr, 0, &fg_worker_thread_proc, nullptr, 0, &g_fg_worker_id);
    if (g_fg_worker_handle == nullptr)
    {
        set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Could not start the dedicated FG Present thread.");
        return false;
    }

    set_note(s, "Dedicated FG Present thread started; game Present will only enqueue completed NR frames.");
    return true;
}

void stop_fg_worker()
{
    g_requested_enabled.store(false, std::memory_order_release);
    g_fg_worker_stop.store(true, std::memory_order_release);
    if (g_fg_worker_wake)
        SetEvent(g_fg_worker_wake);

    bool worker_stopped = true;
    if (g_fg_worker_handle)
    {
        // This runs from ReShade's runtime-destroy event, not DllMain, so waiting here is safe.
        const DWORD wait = WaitForSingleObject(g_fg_worker_handle, 5000);
        worker_stopped = wait == WAIT_OBJECT_0;
        if (worker_stopped)
        {
            CloseHandle(g_fg_worker_handle);
            g_fg_worker_handle = nullptr;
            g_fg_worker_id = 0;
        }
        else
        {
            reshade::log::message(reshade::log::level::warning,
                                  "[ReShadeFrameGen] FG worker did not stop within 5 seconds; leaving its handles intact rather than unloading underneath it.");
        }
    }
    if (worker_stopped && g_fg_worker_wake)
    {
        CloseHandle(g_fg_worker_wake);
        g_fg_worker_wake = nullptr;
    }

    drop_queued_bridge_slots();
    if (worker_stopped)
    {
        g_fg_worker_running.store(false, std::memory_order_release);
        g_fg_worker_busy.store(false, std::memory_order_release);
    }
}

void refresh_state(Snapshot &s)
{
    const deps::Snapshot d = deps::probe();
    s.interposer_found = d.interposer.found;
    s.interposer_loaded = d.interposer.loaded || GetModuleHandleW(L"sl.interposer.dll") != nullptr;
    if (!d.interposer.path.empty())
        s.interposer_path = d.interposer.path;
    if (s.plugin_directory.empty())
        s.plugin_directory = deps::preferred_streamline_directory();

    HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    if (module)
        resolve_core_exports(module, s);
    if (s.streamline_initialized)
        query_requirements(s);
    if (s.endpoint_device_submitted)
        resolve_feature_functions(s);
}

void hide_endpoint()
{
    if (g_endpoint_hwnd)
        ShowWindow(g_endpoint_hwnd, SW_HIDE);
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
    if (!device || device->get_api() != reshade::api::device_api::d3d11)
        return;

    // Do NOT bind the private endpoint to the first D3D11 device in the process. Launchers, splash
    // renderers and video paths can create temporary devices. We only create the endpoint after
    // ReShade has selected the primary effect runtime / game surface.
    Snapshot s = snapshot();
    s.game_d3d11_seen = true;
    if (s.streamline_initialized && !s.endpoint_device_created)
        set_note(s, "D3D11 device detected. Waiting for the primary game surface before creating the same-adapter D3D12 endpoint.");

    std::lock_guard lock(g_state_mutex);
    g_state = s;
}

void on_init_swapchain(reshade::api::swapchain *, bool)
{
    // The game's D3D11 swapchain deliberately remains ReShade/native. DLSS-G owns the separate
    // private D3D12 proxy swapchain created lazily once the primary runtime dimensions are known.
}

void on_primary_runtime(reshade::api::effect_runtime *runtime)
{
    if (!runtime)
        return;
    g_game_hwnd = static_cast<HWND>(runtime->get_hwnd());

    reshade::api::device *device = runtime->get_device();
    if (!device || device->get_api() != reshade::api::device_api::d3d11)
        return;

    Snapshot s = snapshot();
    s.game_d3d11_seen = true;
    if (s.streamline_initialized && !s.endpoint_device_created)
        set_note(s, "Primary D3D11 surface selected. Waiting for DLSS5_Feed.fx to be enabled before binding the D3D12 endpoint to this device.");

    std::lock_guard lock(g_state_mutex);
    g_state = s;
}

void on_primary_runtime_destroyed(reshade::api::effect_runtime *runtime)
{
    if (runtime && static_cast<HWND>(runtime->get_hwnd()) != g_game_hwnd)
        return;

    // Stop/join outside DllMain while ReShade's runtime is still alive. This guarantees the
    // dedicated Present thread cannot outlive the add-on or touch an endpoint being destroyed.
    stop_fg_worker();
    g_game_hwnd = nullptr;
    hide_endpoint();
}

void set_game_api(reshade::api::device_api api)
{
    g_game_api.store(api, std::memory_order_relaxed);
}

void request_enabled(bool enabled)
{
    const bool previous = g_requested_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled && !previous)
    {
        g_reset_next_capture.store(true, std::memory_order_release);
        g_reset_fps_counters.store(true, std::memory_order_release);
    }
    if (!enabled)
        hide_endpoint();
    if (g_fg_worker_wake)
        SetEvent(g_fg_worker_wake);
}

void request_multiplier(int multiplier)
{
    g_requested_multiplier.store(std::clamp(multiplier, 2, 6), std::memory_order_relaxed);
}

void request_depth_inverted(bool inverted)
{
    g_depth_inverted.store(inverted, std::memory_order_relaxed);
    g_reset_next_capture.store(true, std::memory_order_release);
}

bool requested_enabled() { return g_requested_enabled.load(std::memory_order_relaxed); }
int requested_multiplier() { return g_requested_multiplier.load(std::memory_order_relaxed); }
bool requested_depth_inverted() { return g_depth_inverted.load(std::memory_order_relaxed); }

void present_tick(reshade::api::effect_runtime *runtime)
{
    g_app_presents_since_poll.fetch_add(1, std::memory_order_relaxed);
    pump_endpoint_messages();
    reclaim_completed_bridge_slots();

    if ((GetAsyncKeyState(VK_F6) & 1) != 0)
        request_enabled(!requested_enabled());

    Snapshot s = snapshot();
    const ULONGLONG now = GetTickCount64();
    if (g_force_probe.exchange(false, std::memory_order_acq_rel) || g_last_probe_ms == 0 || now - g_last_probe_ms >= 1000)
    {
        g_last_probe_ms = now;
        refresh_state(s);
    }

    if (runtime && runtime->get_device() && runtime->get_device()->get_api() == reshade::api::device_api::d3d11 &&
        s.streamline_initialized)
    {
        const bool endpoint_setup_incomplete =
            !s.endpoint_device_submitted || !s.feature_loaded || !s.feature_functions_ready ||
            !s.proxy_device_ready || !s.proxy_queue_ready || !s.native_queue_resolved || !s.proxy_factory_ready ||
            !g_command_list || !g_input_fence12 || !g_input_fence11 || !g_release_fence12 || !g_game_context11_4;

        if (endpoint_setup_incomplete)
        {
            const guides::Snapshot guide_state = guides::snapshot();
            if (guide_state.effect_enabled)
            {
                ID3D11Device *native = reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(runtime->get_device()->get_native()));
                create_private_d3d12_endpoint(native, s);
            }
        }
    }

    bool queued_frame = false;
    if (runtime && runtime->get_device() && runtime->get_device()->get_api() == reshade::api::device_api::d3d11 &&
        s.endpoint_device_submitted && s.feature_loaded && s.feature_functions_ready)
    {
        // Resolve/build resources on the game thread, but only while no worker-owned slot would be
        // destroyed. Once READY, the callback never performs a Streamline feature call or Present.
        guides::NativeTextures guide{};
        const guides::Snapshot guide_state = guides::snapshot();
        const bool guides_ok = guide_state.effect_enabled && guides::acquire_native_textures(runtime, guide);
        s.feeder_mv_acquired = guides_ok && guide.motion_vectors != nullptr;
        s.feeder_depth_acquired = guides_ok && guide.depth != nullptr;
        if (guide_state.effect_present && !guide_state.effect_enabled)
            set_note(s, "DLSS5_Feed.fx is installed but disabled. Enable its DLSS5_Feed technique so MV/depth update every frame.");

        const reshade::api::resource bb = runtime->get_current_back_buffer();
        auto *color = bb.handle ? reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(bb.handle)) : nullptr;
        if (guides_ok && color)
            ensure_frame_bridge(color, guide.motion_vectors, guide.depth, s);

        s.controller_ready = s.streamline_initialized && s.endpoint_device_submitted && s.endpoint_feature_supported &&
                             s.feature_loaded && s.proxy_device_ready && s.proxy_queue_ready && s.native_queue_resolved && s.proxy_factory_ready &&
                             s.proxy_swapchain_ready && s.feeder_bridge_ready && guide_state.effect_enabled &&
                             s.feature_functions_ready && s.reflex_functions_ready && s.pcl_functions_ready &&
                             g_command_list && g_input_fence12 && g_input_fence11 && g_release_fence12 && g_game_context11_4;

        if (s.controller_ready)
            ensure_fg_worker_started(s);

        if (requested_enabled() && s.controller_ready && g_fg_worker_handle != nullptr)
            queued_frame = capture_real_frame_nonblocking(runtime, s);

        // The endpoint must already be visible before its first real Present. Previously we kept it
        // hidden until Present "succeeded", which made the first CPU-side success an unreliable
        // signal of an actually displayed/generated frame. Positioning remains on the game thread.
        align_endpoint_window(requested_enabled() && s.controller_ready &&
                              (queued_frame || s.endpoint_present_succeeded));
    }
    else
    {
        hide_endpoint();
    }

    if (!requested_enabled())
        hide_endpoint();

    // Publish the game-thread state BEFORE waking the worker, so a fast FG Present cannot be
    // immediately overwritten by a stale pre-submit snapshot from this callback.
    {
        std::lock_guard lock(g_state_mutex);
        g_state = s;
    }

    if (queued_frame && g_fg_worker_wake)
        SetEvent(g_fg_worker_wake);
}

void force_reprobe()
{
    g_force_probe.store(true, std::memory_order_release);
}

void retry_bootstrap_now()
{
    perform_bootstrap(false);
    force_reprobe();
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
