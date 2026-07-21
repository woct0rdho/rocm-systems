// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "registration_test.hpp"

#include <rocprofiler-register/rocprofiler-register.h>

#include <windows.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
uint32_t
invalid_import()
{
    return 70000;
}

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
        std::cerr << "usage: registration-test <profiler> <producer> <register> <mode>\n";
        return 2;
    }

    auto profiler_path = std::string{argv[1]};
    auto producer_path = std::string{argv[2]};
    auto register_path = std::string{argv[3]};
    auto mode          = std::string_view{argv[4]};

    clear_environment("ROCP_TOOL_LIBRARIES");
    clear_environment("ROCPROFILER_REGISTER_LIBRARY");
    clear_environment("ROCPROFILER_REGISTER_FORCE_LOAD");
    set_environment("ROCPROFILER_REGISTER_ENABLED", "1");
    set_environment("ROCPROFILER_REGISTER_SECURE",
                    (mode == "secure" || mode == "invalid") ? "1" : "0");
    if(mode == "normal") set_environment("ROCPROFILER_REGISTER_FORCE_LOAD", "1");

    try
    {
        auto register_module = load_library(register_path);
        HMODULE profiler_module = nullptr;
        HMODULE producer_module = nullptr;

        if(mode == "secure" || mode == "invalid")
        {
            profiler_module = load_library(profiler_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", profiler_path);
        }

        producer_module = load_library(producer_path);
        auto probe = get_export<windows_registration_probe_t>(
            producer_module, "windows_registration_probe");
        auto value = get_export<windows_registration_value_t>(
            producer_module, "windows_registration_value");

        if(mode == "normal" || mode == "secure")
        {
            auto status = probe(0);
            if(status != 0 || value(7) != 17) return 10 + status;
        }
        else if(mode == "late")
        {
            auto status = probe(1);
            if(status != 0 || value(7) != 8) return 30 + status;

            profiler_module = load_library(profiler_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", profiler_path);
            auto invoke = get_export<decltype(&rocprofiler_register_invoke_nonpropagated_registrations)>(
                register_module, "rocprofiler_register_invoke_nonpropagated_registrations");
            if(invoke() != ROCP_REG_SUCCESS || value(7) != 17) return 40;
        }
        else if(mode == "invalid")
        {
            auto invalid_probe = get_export<windows_registration_probe_with_import_t>(
                producer_module, "windows_registration_probe_with_import");
            if(invalid_probe(&invalid_import) != ROCP_REG_INVALID_API_ADDRESS) return 50;
        }
        else
        {
            std::cerr << "unknown mode: " << mode << "\n";
            return 3;
        }

        std::cout << "windows_registration_mode=" << mode << " passed\n";
        if(producer_module) FreeLibrary(producer_module);
        if(profiler_module) FreeLibrary(profiler_module);
        FreeLibrary(register_module);
        return 0;
    } catch(const std::exception& error)
    {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
