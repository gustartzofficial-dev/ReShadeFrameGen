#pragma once

#include <windows.h>
#include <string>

namespace fg::deps
{
struct DllState
{
    bool found = false;
    bool loaded = false;
    std::wstring path;
};

struct Snapshot
{
    DllState interposer;
    DllState sl_dlssg;
    DllState sl_reflex;
    DllState sl_pcl;
    DllState sl_common;
    DllState ngx_dlssg;

    bool renodx_dlss_loaded = false;
    bool renodx_dlss5_loaded = false;
    bool feeder_loaded = false;
};

Snapshot probe();
DllState probe_dll(const wchar_t *name);
std::wstring executable_directory();
std::wstring preferred_streamline_directory();
HMODULE load_from_detected_path(const DllState &state, DWORD *last_error = nullptr);
std::string narrow(const std::wstring &value);
}
