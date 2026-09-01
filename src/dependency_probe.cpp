#include "dependency_probe.hpp"

namespace fg::deps
{
namespace
{
bool loaded(const wchar_t *name)
{
    return GetModuleHandleW(name) != nullptr;
}

bool beside_exe(const wchar_t *name)
{
    std::wstring path = executable_directory();
    if (path.empty())
        return false;
    if (!path.empty() && path.back() != L'\\')
        path.push_back(L'\\');
    path += name;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
}

std::wstring executable_directory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};
    std::wstring out(path, len);
    const size_t slash = out.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : out.substr(0, slash);
}

Snapshot probe()
{
    Snapshot s{};
    s.renodx_dlss_loaded  = loaded(L"renodx-dlss.addon64");
    s.renodx_dlss5_loaded = loaded(L"renodx-dlss5.addon64");
    s.feeder_loaded       = loaded(L"dlss5-feed.addon64");
    s.streamline_loaded   = loaded(L"sl.interposer.dll");
    s.sl_dlssg_loaded     = loaded(L"sl.dlss_g.dll");
    s.sl_reflex_loaded    = loaded(L"sl.reflex.dll");
    s.ngx_dlssg_loaded    = loaded(L"nvngx_dlssg.dll");

    s.renodx_dlss_file  = beside_exe(L"renodx-dlss.addon64");
    s.renodx_dlss5_file = beside_exe(L"renodx-dlss5.addon64");
    s.streamline_file   = beside_exe(L"sl.interposer.dll");
    return s;
}
}
