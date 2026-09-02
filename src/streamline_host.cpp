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
uint64_t g_app_presents_since_poll = 0;
uint32_t g_frame_index = 0;
bool g_first_endpoint_frame = true;
bool g_dlssg_was_enabled = false;

std::wstring g_plugin_dir;
std::array<const wchar_t *, 1> g_plugin_paths{};
constexpr std::array<sl::Feature, 3> k_requested_features = {
    sl::kFeaturePCL,
    sl::kFeatureReflex,
    sl::kFeatureDLSS_G,
};

constexpr const char *k_project_id = "68c3c204-a7b9-43e0-a319-37b62eef12f7";
constexpr const char *k_engine_version = "ReShadeFrameGen-DLSSGHost-0.3-D3D12Endpoint";
constexpr sl::ViewportHandle k_viewport{0};

// The private endpoint is deliberately created with the real system DLLs rather than the game's
// proxy dxgi.dll. ReShade continues to own the game's D3D11 swapchain; Streamline owns this D3D12
// swapchain. This keeps the two presentation stacks from wrapping each other recursively.
HMODULE g_system_d3d12 = nullptr;
HMODULE g_system_dxgi = nullptr;
using D3D12CreateDeviceProc = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
using CreateDXGIFactory2Proc = HRESULT (WINAPI *)(UINT, REFIID, void **);
D3D12CreateDeviceProc g_d3d12_create_device = nullptr;
CreateDXGIFactory2Proc g_create_dxgi_factory2 = nullptr;

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

struct CommandSlot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    uint64_t completed_value = 0;
};
std::array<CommandSlot, 3> g_command_slots{};
ComPtr<ID3D12GraphicsCommandList> g_command_list;

ComPtr<ID3D12Fence> g_shared_fence12;
ComPtr<ID3D11Fence> g_shared_fence11;
HANDLE g_fence_event = nullptr;
uint64_t g_fence_value = 0;
uint64_t g_last_endpoint_done = 0;

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
        return d12 && d11 && format == desc.Format && width == desc.Width && height == desc.Height &&
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

SharedTexture g_shared_color;
SharedTexture g_shared_mv;
SharedTexture g_shared_depth;

HWND g_game_hwnd = nullptr;
HWND g_endpoint_hwnd = nullptr;
ATOM g_window_class = 0;
uint32_t g_endpoint_width = 0;
uint32_t g_endpoint_height = 0;
DXGI_FORMAT g_endpoint_format = DXGI_FORMAT_UNKNOWN;
bool g_allow_tearing = false;

void set_note(Snapshot &s, const char *note)
{
    s.bootstrap_note = note != nullptr ? note : "";
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
    s.reflex_functions_ready = g_reflex_set_options != nullptr && g_reflex_sleep != nullptr;

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
        set_note(s, "sl.interposer.dll loaded, but v0.3 could not resolve the manual-hooking/frame-tagging exports it needs.");
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

        // Critical v0.3 change: D3D11 games feed a PRIVATE SAME-ADAPTER D3D12 endpoint. The game's
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
        set_note(s, "Streamline initialized as D3D12. Waiting for the D3D11 game device so v0.3 can create the same-adapter D3D12 endpoint.");
    }
    else if (!early_device_creation)
    {
        set_note(s, "DLL state refreshed. A cold restart is required to retry slInit; v0.3 never calls slInit a second time late.");
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

bool wait_for_fence_value(uint64_t value)
{
    if (value == 0 || !g_shared_fence12)
        return true;
    if (g_shared_fence12->GetCompletedValue() >= value)
        return true;
    if (g_fence_event == nullptr)
        return false;
    if (FAILED(g_shared_fence12->SetEventOnCompletion(value, g_fence_event)))
        return false;
    return WaitForSingleObject(g_fence_event, 2000) == WAIT_OBJECT_0;
}

void destroy_swapchain()
{
    if (g_endpoint_hwnd)
        ShowWindow(g_endpoint_hwnd, SW_HIDE);
    g_native_swapchain.Reset();
    g_proxy_swapchain.Reset();
    g_endpoint_width = g_endpoint_height = 0;
    g_endpoint_format = DXGI_FORMAT_UNKNOWN;
}

void destroy_bridge_resources()
{
    if (g_last_endpoint_done)
        wait_for_fence_value(g_last_endpoint_done);
    g_shared_color.reset();
    g_shared_mv.reset();
    g_shared_depth.reset();
    g_first_endpoint_frame = true;
}

bool create_shared_texture(const D3D11_TEXTURE2D_DESC &src, SharedTexture &out, Snapshot &s)
{
    if (!g_device12 || !g_game_device11_1 || src.Width == 0 || src.Height == 0 || src.SampleDesc.Count != 1)
        return false;

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
    desc.Format = src.Format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    ComPtr<ID3D12Resource> d12;
    HRESULT hr = g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&d12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D12 failed to create one of the shared Feeder bridge textures.");
        return false;
    }

    HANDLE handle = nullptr;
    hr = g_device12->CreateSharedHandle(d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr) || handle == nullptr)
    {
        set_hr(s, hr, "D3D12 failed to create a shared handle for a Feeder bridge texture.");
        return false;
    }

    ComPtr<ID3D11Texture2D> d11;
    hr = g_game_device11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&d11));
    CloseHandle(handle);
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D11 failed to open a D3D12 shared texture on the same adapter.");
        return false;
    }

    out.reset();
    out.d12 = d12;
    out.d11 = d11;
    out.format = src.Format;
    out.width = src.Width;
    out.height = src.Height;
    out.mip_levels = static_cast<uint16_t>(std::max<UINT>(1, src.MipLevels));
    out.array_size = static_cast<uint16_t>(std::max<UINT>(1, src.ArraySize));
    return true;
}

bool create_shared_fence(Snapshot &s)
{
    if (g_shared_fence12 && g_shared_fence11 && g_game_context11_4)
        return true;
    if (!g_device12 || !g_game_device11_5 || !g_game_context11_4)
    {
        set_note(s, "D3D11/D3D12 shared-fence interop is unavailable (ID3D11Device5 / ID3D11DeviceContext4 required).");
        return false;
    }

    HRESULT hr = g_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_shared_fence12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to create the shared D3D11/D3D12 synchronization fence.");
        return false;
    }

    HANDLE handle = nullptr;
    hr = g_device12->CreateSharedHandle(g_shared_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr) || handle == nullptr)
    {
        set_hr(s, hr, "Failed to export the D3D12 synchronization fence.");
        return false;
    }

    hr = g_game_device11_5->OpenSharedFence(handle, IID_PPV_ARGS(&g_shared_fence11));
    CloseHandle(handle);
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D11 failed to open the D3D12 shared fence.");
        return false;
    }

    if (g_fence_event == nullptr)
        g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_fence_event == nullptr)
    {
        set_hr(s, HRESULT_FROM_WIN32(GetLastError()), "Failed to create the endpoint fence event.");
        return false;
    }
    return true;
}

bool create_command_objects(Snapshot &s)
{
    if (g_command_list && g_command_slots[0].allocator)
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
    if (g_device12)
    {
        s.endpoint_device_created = true;
        return true;
    }
    if (game_device == nullptr || !s.streamline_initialized || !s.core_exports_ready)
        return false;
    if (!load_system_graphics_exports(s))
        return false;

    g_game_device11 = game_device;
    game_device->QueryInterface(IID_PPV_ARGS(&g_game_device11_1));
    game_device->QueryInterface(IID_PPV_ARGS(&g_game_device11_5));
    game_device->GetImmediateContext(&g_game_context11);
    if (g_game_context11)
        g_game_context11.As(&g_game_context11_4);

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

    hr = g_d3d12_create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D12CreateDevice failed on the D3D11 game's adapter. DLSS-G endpoint cannot start.");
        return false;
    }
    s.endpoint_device_created = true;

    s.last_set_device = (*g_set_d3d_device)(g_device12.Get());
    s.endpoint_device_submitted = s.last_set_device == sl::Result::eOk;
    if (!s.endpoint_device_submitted)
    {
        set_note(s, "Private D3D12 device exists, but slSetD3DDevice rejected it.");
        return false;
    }

    query_requirements(s);

    if (g_game_adapter && g_is_feature_supported)
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

    // Requested features are normally loaded automatically after slSetD3DDevice. If this exact
    // plugin build still reports not-loaded, explicitly ask SL to load it while no rendering is in flight.
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

    resolve_feature_functions(s);
    if (!s.feature_loaded)
    {
        set_note(s, "The D3D12 endpoint device is active, but Streamline still reports DLSS-G feature missing. See requirements/support and SL log.");
        return false;
    }

    ID3D12Device *proxy_device = g_device12.Get();
    s.last_upgrade_device = (*g_upgrade_interface)(reinterpret_cast<void **>(&proxy_device));
    if (s.last_upgrade_device != sl::Result::eOk || proxy_device == nullptr)
    {
        set_note(s, "slUpgradeInterface failed for the private D3D12 device.");
        return false;
    }
    g_proxy_device12 = proxy_device;
    s.proxy_device_ready = true;

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qdesc.NodeMask = 0;
    hr = g_proxy_device12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g_proxy_queue12));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Streamline proxy device failed to create the presenting command queue.");
        return false;
    }
    s.proxy_queue_ready = true;

    void *native_queue = nullptr;
    s.last_get_native_queue = (*g_get_native_interface)(g_proxy_queue12.Get(), &native_queue);
    if (s.last_get_native_queue != sl::Result::eOk || native_queue == nullptr)
    {
        set_note(s, "Streamline created the proxy queue, but slGetNativeInterface could not recover its native queue.");
        return false;
    }
    g_native_queue12.Attach(static_cast<ID3D12CommandQueue *>(native_queue));
    s.native_queue_resolved = true;

    hr = g_create_dxgi_factory2(0, IID_PPV_ARGS(&g_native_factory));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Could not create the native DXGI factory for the DLSS-G endpoint.");
        return false;
    }

    IDXGIFactory4 *proxy_factory = g_native_factory.Get();
    s.last_upgrade_factory = (*g_upgrade_interface)(reinterpret_cast<void **>(&proxy_factory));
    if (s.last_upgrade_factory != sl::Result::eOk || proxy_factory == nullptr)
    {
        set_note(s, "slUpgradeInterface failed for the endpoint DXGI factory.");
        return false;
    }
    g_proxy_factory = proxy_factory;
    s.proxy_factory_ready = true;

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(g_native_factory.As(&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            g_allow_tearing = allow == TRUE;
    }

    if (!create_command_objects(s) || !create_shared_fence(s))
        return false;

    set_note(s, "Private same-adapter D3D12 device is live and DLSS-G is loaded. Waiting for Feeder guides + endpoint swapchain on the primary game surface.");
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
    s.endpoint_width = width;
    s.endpoint_height = height;
    s.endpoint_format = static_cast<uint32_t>(format);
    s.proxy_swapchain_ready = true;
    g_first_endpoint_frame = true;

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
        set_note(s, "v0.3 first test requires single-sample color/MV/depth textures.");
        return false;
    }

    const bool rebuild = !g_shared_color.matches(cdesc) || !g_shared_mv.matches(mdesc) || !g_shared_depth.matches(ddesc);
    if (rebuild)
    {
        destroy_bridge_resources();
        if (!create_shared_texture(cdesc, g_shared_color, s) ||
            !create_shared_texture(mdesc, g_shared_mv, s) ||
            !create_shared_texture(ddesc, g_shared_depth, s))
            return false;
    }

    if (!create_proxy_swapchain(cdesc.Width, cdesc.Height, cdesc.Format, s))
        return false;

    s.feeder_bridge_ready = true;
    return true;
}

void set_identity(sl::float4x4 &m)
{
    m.row[0] = sl::float4(1.f, 0.f, 0.f, 0.f);
    m.row[1] = sl::float4(0.f, 1.f, 0.f, 0.f);
    m.row[2] = sl::float4(0.f, 0.f, 1.f, 0.f);
    m.row[3] = sl::float4(0.f, 0.f, 0.f, 1.f);
}

sl::Constants make_test_constants(uint32_t mv_width, uint32_t mv_height, uint32_t color_width, uint32_t color_height)
{
    sl::Constants c{};
    set_identity(c.cameraViewToClip);
    set_identity(c.clipToCameraView);
    set_identity(c.clipToPrevClip);
    set_identity(c.prevClipToClip);
    c.jitterOffset = sl::float2(0.f, 0.f);
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
    c.reset = g_first_endpoint_frame ? sl::Boolean::eTrue : sl::Boolean::eFalse;
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

bool submit_real_frame(reshade::api::effect_runtime *runtime, Snapshot &s)
{
    if (!runtime || !g_game_device11 || !g_game_context11_4 || !g_native_queue12 ||
        !g_shared_fence12 || !g_shared_fence11 || !g_proxy_swapchain || !g_command_list)
        return false;

    guides::NativeTextures guide{};
    if (!guides::acquire_native_textures(runtime, guide))
    {
        s.feeder_mv_acquired = guide.motion_vectors != nullptr;
        s.feeder_depth_acquired = guide.depth != nullptr;
        set_note(s, "DLSS5_Feed.fx is present, but its MV/depth D3D11 texture bindings are not available this frame.");
        return false;
    }
    s.feeder_mv_acquired = true;
    s.feeder_depth_acquired = true;

    const reshade::api::resource backbuffer = runtime->get_current_back_buffer();
    if (backbuffer.handle == 0)
        return false;
    ID3D11Texture2D *game_color = reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(backbuffer.handle));

    if (!ensure_frame_bridge(game_color, guide.motion_vectors, guide.depth, s))
        return false;

    D3D11_TEXTURE2D_DESC cdesc{}, mdesc{}, ddesc{};
    game_color->GetDesc(&cdesc);
    guide.motion_vectors->GetDesc(&mdesc);
    guide.depth->GetDesc(&ddesc);

    // Reuse shared textures only after DLSS-G and our presenting queue are done consuming the
    // previous frame. This is a GPU-side wait, not a software frame pacer.
    if (g_last_endpoint_done != 0)
    {
        const HRESULT whr = g_game_context11_4->Wait(g_shared_fence11.Get(), g_last_endpoint_done);
        if (FAILED(whr))
        {
            set_hr(s, whr, "D3D11 failed to wait for the previous endpoint frame.");
            return false;
        }
    }

    g_game_context11->CopyResource(g_shared_color.d11.Get(), game_color);
    g_game_context11->CopyResource(g_shared_mv.d11.Get(), guide.motion_vectors);
    g_game_context11->CopyResource(g_shared_depth.d11.Get(), guide.depth);

    const uint64_t input_ready = ++g_fence_value;
    HRESULT hr = g_game_context11_4->Signal(g_shared_fence11.Get(), input_ready);
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D11 failed to signal the Feeder input fence.");
        return false;
    }
    g_game_context11->Flush();
    hr = g_native_queue12->Wait(g_shared_fence12.Get(), input_ready);
    if (FAILED(hr))
    {
        set_hr(s, hr, "D3D12 endpoint queue failed to wait for D3D11 Feeder inputs.");
        return false;
    }

    // Establish the Streamline frame identity and latency cadence before submitting the endpoint
    // render/copy work. These markers now bracket our real D3D12 submission rather than being
    // emitted as a cosmetic burst immediately before Present.
    sl::FrameToken *token = nullptr;
    const uint32_t frame_number = ++g_frame_index;
    s.last_get_frame_token = (*g_get_new_frame_token)(token, &frame_number);
    s.frame_token_ready = s.last_get_frame_token == sl::Result::eOk && token != nullptr;
    if (!s.frame_token_ready)
    {
        set_note(s, "slGetNewFrameToken failed on the real endpoint frame.");
        return false;
    }

    if (g_reflex_set_options)
    {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::eLowLatency;
        s.last_reflex_options = (*g_reflex_set_options)(reflex);
        s.reflex_enabled = s.last_reflex_options == sl::Result::eOk;
    }
    if (g_reflex_sleep)
    {
        s.last_reflex_sleep = (*g_reflex_sleep)(*token);
        s.reflex_sleep_called = s.last_reflex_sleep == sl::Result::eOk;
    }

    bool markers_ok = true;
    markers_ok &= emit_marker(sl::PCLMarker::eSimulationStart, *token, s);
    markers_ok &= emit_marker(sl::PCLMarker::eSimulationEnd, *token, s);
    markers_ok &= emit_marker(sl::PCLMarker::eRenderSubmitStart, *token, s);

    const uint32_t slot_index = g_frame_index % static_cast<uint32_t>(g_command_slots.size());
    CommandSlot &slot = g_command_slots[slot_index];
    if (slot.completed_value && !wait_for_fence_value(slot.completed_value))
    {
        set_hr(s, DXGI_ERROR_DEVICE_HUNG, "Timed out waiting to recycle a D3D12 endpoint command allocator.");
        return false;
    }

    hr = slot.allocator->Reset();
    if (SUCCEEDED(hr))
        hr = g_command_list->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to reset the D3D12 endpoint command list.");
        return false;
    }

    // Calling GetCurrentBackBufferIndex on the PROXY every frame is part of the DLSS-G contract.
    const UINT back_index = g_proxy_swapchain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> endpoint_backbuffer;
    hr = g_proxy_swapchain->GetBuffer(back_index, IID_PPV_ARGS(&endpoint_backbuffer));
    if (FAILED(hr))
    {
        set_hr(s, hr, "Streamline proxy swapchain GetBuffer failed.");
        return false;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_shared_color.d12.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = endpoint_backbuffer.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_command_list->ResourceBarrier(2, barriers);
    g_command_list->CopyResource(endpoint_backbuffer.Get(), g_shared_color.d12.Get());
    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    g_command_list->ResourceBarrier(2, barriers);

    hr = g_command_list->Close();
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to close the D3D12 endpoint command list.");
        return false;
    }
    ID3D12CommandList *lists[] = { g_command_list.Get() };
    g_native_queue12->ExecuteCommandLists(1, lists);
    markers_ok &= emit_marker(sl::PCLMarker::eRenderSubmitEnd, *token, s);

    sl::Resource mv_res(sl::ResourceType::eTex2d, g_shared_mv.d12.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
    mv_res.width = mdesc.Width; mv_res.height = mdesc.Height; mv_res.nativeFormat = static_cast<uint32_t>(mdesc.Format);
    sl::Resource depth_res(sl::ResourceType::eTex2d, g_shared_depth.d12.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
    depth_res.width = ddesc.Width; depth_res.height = ddesc.Height; depth_res.nativeFormat = static_cast<uint32_t>(ddesc.Format);
    sl::Resource color_res(sl::ResourceType::eTex2d, g_shared_color.d12.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
    color_res.width = cdesc.Width; color_res.height = cdesc.Height; color_res.nativeFormat = static_cast<uint32_t>(cdesc.Format);

    const sl::Extent mv_extent{0, 0, mdesc.Width, mdesc.Height};
    const sl::Extent depth_extent{0, 0, ddesc.Width, ddesc.Height};
    const sl::Extent color_extent{0, 0, cdesc.Width, cdesc.Height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth_res, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depth_extent),
        sl::ResourceTag(&mv_res, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &mv_extent),
        // For the first real test this is final color (HUD included). It is deliberately tagged as
        // HUD-less so FG can run; UI recomposition becomes a quality upgrade after function is proven.
        sl::ResourceTag(&color_res, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &color_extent),
    };
    s.last_set_tags = (*g_set_tag_for_frame)(*token, k_viewport, tags, static_cast<uint32_t>(sizeof(tags) / sizeof(tags[0])), nullptr);
    s.tags_submitted = s.last_set_tags == sl::Result::eOk;

    const sl::Constants constants = make_test_constants(mdesc.Width, mdesc.Height, cdesc.Width, cdesc.Height);
    s.last_set_constants = (*g_set_constants)(constants, *token, k_viewport);
    s.constants_submitted = s.last_set_constants == sl::Result::eOk;

    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = static_cast<uint32_t>(std::max(1, std::clamp(g_requested_multiplier.load(std::memory_order_relaxed), 2, 6) - 1));
    options.numBackBuffers = 3;
    options.mvecDepthWidth = mdesc.Width;
    options.mvecDepthHeight = mdesc.Height;
    options.colorWidth = cdesc.Width;
    options.colorHeight = cdesc.Height;
    options.colorBufferFormat = static_cast<uint32_t>(cdesc.Format);
    options.mvecBufferFormat = static_cast<uint32_t>(mdesc.Format);
    options.depthBufferFormat = static_cast<uint32_t>(ddesc.Format);
    options.hudLessBufferFormat = static_cast<uint32_t>(cdesc.Format);
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;

    if (s.max_generated_frames > 0)
        options.numFramesToGenerate = std::min(options.numFramesToGenerate, s.max_generated_frames);
    s.last_set_options = (*g_set_options)(k_viewport, options);

    markers_ok &= emit_marker(sl::PCLMarker::ePresentStart, *token, s);
    s.pcl_markers_submitted = markers_ok;

    align_endpoint_window(true);
    const UINT present_flags = g_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    s.endpoint_present_attempted = true;
    s.last_present_hr = g_proxy_swapchain->Present(0, present_flags);
    s.endpoint_present_succeeded = SUCCEEDED(s.last_present_hr);

    markers_ok &= emit_marker(sl::PCLMarker::ePresentEnd, *token, s);
    s.pcl_markers_submitted = markers_ok;
    g_dlssg_was_enabled = s.last_set_options == sl::Result::eOk;

    const uint64_t done = ++g_fence_value;
    hr = g_native_queue12->Signal(g_shared_fence12.Get(), done);
    if (FAILED(hr))
    {
        set_hr(s, hr, "Failed to signal completion of the DLSS-G endpoint frame.");
        return false;
    }
    g_last_endpoint_done = done;
    slot.completed_value = done;
    g_first_endpoint_frame = false;

    if (s.endpoint_present_succeeded)
        set_note(s, "REAL TEST ACTIVE: D3D11 frame + Feeder MV/depth -> shared D3D12 -> Streamline proxy Present -> NVIDIA DLSS-G x2+.");
    return s.endpoint_present_succeeded;
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

void poll_state(Snapshot &s)
{
    if (!g_get_state || !s.feature_functions_ready)
        return;

    const ULONGLONG now = GetTickCount64();
    if (g_last_poll_ms != 0 && now - g_last_poll_ms < 500)
        return;

    const double seconds = g_last_poll_ms == 0 ? 0.0 : static_cast<double>(now - g_last_poll_ms) / 1000.0;
    g_last_poll_ms = now;

    sl::DLSSGState state{};
    s.last_get_state = (*g_get_state)(k_viewport, state, nullptr);
    s.state_queried = true;
    if (s.last_get_state == sl::Result::eOk)
    {
        s.status = state.status;
        s.max_generated_frames = state.numFramesToGenerateMax;
        s.frames_presented_last_poll = state.numFramesActuallyPresented;
        s.dynamic_mfg_supported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        if (seconds > 0.0)
        {
            s.output_fps = static_cast<double>(state.numFramesActuallyPresented) / seconds;
            s.app_present_fps = static_cast<double>(g_app_presents_since_poll) / seconds;
        }
    }
    g_app_presents_since_poll = 0;
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
    g_game_hwnd = nullptr;
    hide_endpoint();
}

void set_game_api(reshade::api::device_api api)
{
    g_game_api.store(api, std::memory_order_relaxed);
}

void request_enabled(bool enabled)
{
    g_requested_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
        hide_endpoint();
}

void request_multiplier(int multiplier)
{
    g_requested_multiplier.store(std::clamp(multiplier, 2, 6), std::memory_order_relaxed);
}

void request_depth_inverted(bool inverted)
{
    g_depth_inverted.store(inverted, std::memory_order_relaxed);
    g_first_endpoint_frame = true;
}

bool requested_enabled() { return g_requested_enabled.load(std::memory_order_relaxed); }
int requested_multiplier() { return g_requested_multiplier.load(std::memory_order_relaxed); }
bool requested_depth_inverted() { return g_depth_inverted.load(std::memory_order_relaxed); }

void present_tick(reshade::api::effect_runtime *runtime)
{
    ++g_app_presents_since_poll;
    pump_endpoint_messages();

    // F6 is intentionally available even while the endpoint window covers the game's ReShade UI.
    if ((GetAsyncKeyState(VK_F6) & 1) != 0)
        request_enabled(!requested_enabled());

    Snapshot s = snapshot();
    const ULONGLONG now = GetTickCount64();
    if (g_force_probe.exchange(false, std::memory_order_acq_rel) || g_last_probe_ms == 0 || now - g_last_probe_ms >= 1000)
    {
        g_last_probe_ms = now;
        refresh_state(s);
    }

    // Bind the private D3D12 endpoint only after the primary runtime is known AND the Feeder guide
    // technique is actively updating. This avoids accidentally binding to splash/video D3D11 devices.
    if (runtime && runtime->get_device() && runtime->get_device()->get_api() == reshade::api::device_api::d3d11 &&
        s.streamline_initialized && !s.endpoint_device_created)
    {
        const guides::Snapshot guide_state = guides::snapshot();
        if (guide_state.effect_enabled)
        {
            ID3D11Device *native = reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(runtime->get_device()->get_native()));
            create_private_d3d12_endpoint(native, s);
        }
    }

    if (runtime && runtime->get_device() && runtime->get_device()->get_api() == reshade::api::device_api::d3d11 &&
        s.endpoint_device_submitted && s.feature_loaded && s.feature_functions_ready)
    {
        // Build the bridge/swapchain even while disabled so the UI can reach READY before the user
        // presses F6. The expensive per-frame copy/tag/Present happens only when enabled.
        guides::NativeTextures guide{};
        const guides::Snapshot guide_state = guides::snapshot();
        const bool guides_ok = guide_state.effect_enabled && guides::acquire_native_textures(runtime, guide);
        s.feeder_mv_acquired = guides_ok && guide.motion_vectors != nullptr;
        s.feeder_depth_acquired = guides_ok && guide.depth != nullptr;
        if (guide_state.effect_present && !guide_state.effect_enabled)
            set_note(s, "DLSS5_Feed.fx is installed but disabled. Enable its DLSS5_Feed technique so MV/depth update every frame.");

        const reshade::api::resource bb = runtime->get_current_back_buffer();
        ID3D11Texture2D *color = bb.handle ? reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(bb.handle)) : nullptr;
        if (guides_ok && color)
            ensure_frame_bridge(color, guide.motion_vectors, guide.depth, s);

        s.controller_ready = s.streamline_initialized && s.endpoint_device_submitted && s.endpoint_feature_supported &&
                             s.feature_loaded && s.proxy_device_ready && s.proxy_queue_ready && s.native_queue_resolved && s.proxy_factory_ready &&
                             s.proxy_swapchain_ready && s.native_swapchain_resolved && s.feeder_bridge_ready &&
                             guide_state.effect_enabled && s.feature_functions_ready &&
                             s.reflex_functions_ready && s.pcl_functions_ready;

        if (requested_enabled() && s.controller_ready)
        {
            submit_real_frame(runtime, s);
        }
        else
        {
            // F6 is an actual escape hatch: tell DLSS-G to turn off before hiding the endpoint.
            // Defer this to the present thread instead of calling feature APIs from the UI callback.
            if (!requested_enabled() && g_dlssg_was_enabled && g_set_options)
            {
                sl::DLSSGOptions off{};
                off.mode = sl::DLSSGMode::eOff;
                s.last_set_options = (*g_set_options)(k_viewport, off);
                g_dlssg_was_enabled = false;
            }
            hide_endpoint();
        }
    }
    else
    {
        hide_endpoint();
    }

    poll_state(s);

    std::lock_guard lock(g_state_mutex);
    g_state = s;
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
