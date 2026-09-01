#pragma once

#include <windows.h>
#include <string>

namespace fg::deps
{
struct Snapshot
{
    bool renodx_dlss_loaded = false;
    bool renodx_dlss5_loaded = false;
    bool feeder_loaded = false;
    bool streamline_loaded = false;
    bool sl_dlssg_loaded = false;
    bool sl_reflex_loaded = false;
    bool ngx_dlssg_loaded = false;

    bool renodx_dlss_file = false;
    bool renodx_dlss5_file = false;
    bool streamline_file = false;
};

Snapshot probe();
std::wstring executable_directory();
}
