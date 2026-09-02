#include "dependency_probe.hpp"

#include <array>

namespace fg::deps
{
namespace
{
bool loaded(const wchar_t *name)
{
    return GetModuleHandleW(name) != nullptr;
}

bool is_file(const std::wstring &path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring join(std::wstring base, const wchar_t *name)
{
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
        base.push_back(L'\\');
    base += name;
    return base;
}
}

std::wstring executable_directory()
{
    std::array<wchar_t, 32768> path{};
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size())
        return {};

    std::wstring out(path.data(), len);
    const size_t slash = out.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : out.substr(0, slash);
}

DllState probe_dll(const wchar_t *name)
{
    DllState result{};
    result.loaded = loaded(name);

    const std::wstring root = executable_directory();
    if (root.empty())
        return result;

    // Primary layout: the DLLs sit directly beside the game's EXE.
    const std::wstring beside_exe = join(root, name);
    if (is_file(beside_exe))
    {
        result.found = true;
        result.path = beside_exe;
        return result;
    }

    // Optional fallback for SDK/package layouts. This is NOT the assumed install layout.
    const std::wstring subdir = join(join(root, L"streamline"), name);
    if (is_file(subdir))
    {
        result.found = true;
        result.path = subdir;
    }
    return result;
}

std::wstring preferred_streamline_directory()
{
    const DllState interposer = probe_dll(L"sl.interposer.dll");
    if (interposer.found)
    {
        const size_t slash = interposer.path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            return interposer.path.substr(0, slash);
    }
    return executable_directory();
}

HMODULE load_from_detected_path(const DllState &state, DWORD *last_error)
{
    if (last_error != nullptr)
        *last_error = ERROR_SUCCESS;

    // The caller should first check GetModuleHandleW by DLL basename. Do not use
    // GetModuleHandleW(nullptr) when a DLL is mapped but its on-disk path was not found: that
    // would return the game EXE and make a failed dependency probe look like a loaded module.
    if (!state.found || state.path.empty())
    {
        if (last_error != nullptr)
            *last_error = ERROR_FILE_NOT_FOUND;
        return nullptr;
    }

    HMODULE module = LoadLibraryExW(
        state.path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (module == nullptr)
    {
        const DWORD first_error = GetLastError();
        // Some older Windows configurations do not enable LOAD_LIBRARY_SEARCH_* flags.
        module = LoadLibraryW(state.path.c_str());
        if (module == nullptr && last_error != nullptr)
            *last_error = GetLastError() != ERROR_SUCCESS ? GetLastError() : first_error;
    }
    return module;
}

Snapshot probe()
{
    Snapshot s{};
    s.interposer = probe_dll(L"sl.interposer.dll");
    s.sl_dlssg = probe_dll(L"sl.dlss_g.dll");
    s.sl_reflex = probe_dll(L"sl.reflex.dll");
    s.sl_pcl = probe_dll(L"sl.pcl.dll");
    s.sl_common = probe_dll(L"sl.common.dll");
    s.ngx_dlssg = probe_dll(L"nvngx_dlssg.dll");

    s.renodx_dlss5_loaded = loaded(L"renodx-dlss5.addon64");
    s.feeder_loaded = loaded(L"dlss5-feed.addon64");
    return s;
}

std::string narrow(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}
}
