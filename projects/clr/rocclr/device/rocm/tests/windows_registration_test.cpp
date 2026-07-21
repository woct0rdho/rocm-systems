// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <rocprofiler-register/rocprofiler-register.h>

#include <windows.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
constexpr auto profiled_api_name = "windows-registered-hip-api";

void
set_environment(const char* name, const std::string& value)
{
    if(_putenv_s(name, value.c_str()) != 0)
        throw std::runtime_error(std::string{"failed to set "} + name);
}

void
clear_environment(const char* name)
{
    if(_putenv_s(name, "") != 0)
        throw std::runtime_error(std::string{"failed to clear "} + name);
}

template <typename Tp>
Tp
get_export(HMODULE module, const char* name)
{
    auto* address = GetProcAddress(module, name);
    if(!address) throw std::runtime_error(std::string{"missing export: "} + name);
    return reinterpret_cast<Tp>(address);
}

HMODULE
load_library(const std::string& path)
{
    auto module = LoadLibraryA(path.c_str());
    if(!module)
        throw std::runtime_error("LoadLibrary failed for " + path + " with " +
                                 std::to_string(GetLastError()));
    return module;
}
}  // namespace

int
main(int argc, char** argv)
{
    if(argc != 5)
    {
        std::cerr << "usage: windows-registration-test <profiler> <hip> <register> <mode>\n";
        return 2;
    }

    auto profiler_path = std::string{argv[1]};
    auto hip_path      = std::string{argv[2]};
    auto register_path = std::string{argv[3]};
    auto mode          = std::string_view{argv[4]};
    if(mode != "normal" && mode != "late")
    {
        std::cerr << "unknown registration mode: " << mode << "\n";
        return 3;
    }

    clear_environment("ROCP_TOOL_LIBRARIES");
    clear_environment("ROCPROFILER_REGISTER_LIBRARY");
    clear_environment("ROCPROFILER_REGISTER_FORCE_LOAD");
    set_environment("ROCPROFILER_REGISTER_ENABLED", "1");
    set_environment("ROCPROFILER_REGISTER_SECURE", "1");

    HMODULE register_module = nullptr;
    HMODULE hip_module      = nullptr;
    HMODULE profiler_module = nullptr;
    try
    {
        register_module = load_library(register_path);
        if(mode == "normal")
        {
            profiler_module = load_library(profiler_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", profiler_path);
        }

        hip_module = load_library(hip_path);
        using hip_api_name_t = const char* (*)(uint32_t);
        auto hip_api_name = get_export<hip_api_name_t>(hip_module, "hipApiName");
        auto* initial     = hip_api_name(0);

        if(mode == "normal")
        {
            if(!initial || std::strcmp(initial, profiled_api_name) != 0) return 10;
        }
        else
        {
            if(initial && std::strcmp(initial, profiled_api_name) == 0) return 11;
            profiler_module = load_library(profiler_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", profiler_path);
            auto invoke = get_export<
                decltype(&rocprofiler_register_invoke_nonpropagated_registrations)>(
                register_module, "rocprofiler_register_invoke_nonpropagated_registrations");
            if(invoke() != ROCP_REG_SUCCESS) return 12;

            auto* profiled = hip_api_name(0);
            if(!profiled || std::strcmp(profiled, profiled_api_name) != 0) return 13;
        }

        std::cout << "windows_hip_registration=" << mode
                  << " passed hsa_loaded=no gpu_work_executed=no\n";
        FreeLibrary(hip_module);
        FreeLibrary(profiler_module);
        FreeLibrary(register_module);
        return 0;
    } catch(const std::exception& error)
    {
        std::cerr << "ERROR: " << error.what() << "\n";
        if(hip_module) FreeLibrary(hip_module);
        if(profiler_module) FreeLibrary(profiler_module);
        if(register_module) FreeLibrary(register_module);
        return 1;
    }
}
