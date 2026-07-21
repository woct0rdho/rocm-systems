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

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void
set_environment(const char* name, const std::string& value)
{
    if(_putenv_s(name, value.c_str()) != 0)
        throw std::runtime_error(std::string{"failed to set "} + name);
}

template <typename Tp>
Tp
get_export(HMODULE module, const char* name)
{
    auto* address = ::GetProcAddress(module, name);
    if(!address) throw std::runtime_error(std::string{"missing export: "} + name);
    return reinterpret_cast<Tp>(address);
}

HMODULE
load_library(const std::string& path)
{
    auto module = ::LoadLibraryA(path.c_str());
    if(!module)
        throw std::runtime_error("LoadLibrary failed for " + path + " with " +
                                 std::to_string(::GetLastError()));
    return module;
}

std::string
read_file(const std::string& path)
{
    auto stream = std::ifstream{path, std::ios::binary};
    if(!stream) throw std::runtime_error("failed to read trace log: " + path);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string
hex_encode(std::string_view value)
{
    constexpr char digits[] = "0123456789abcdef";
    auto           result   = std::string{};
    result.reserve(value.size() * 2);
    for(unsigned char byte : value)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}
}  // namespace

int
main(int argc, char** argv)
{
    if(argc != 7)
    {
        std::cerr << "usage: registration-test <sdk> <hip> <roctx> <register> <trace> <normal|late>\n";
        return 2;
    }

    const auto sdk_path      = std::string{argv[1]};
    const auto hip_path      = std::string{argv[2]};
    const auto roctx_path    = std::string{argv[3]};
    const auto register_path = std::string{argv[4]};
    const auto trace_path    = std::string{argv[5]};
    const auto mode          = std::string_view{argv[6]};
    if(mode != "normal" && mode != "late") return 3;

    std::remove(trace_path.c_str());
    set_environment("ROCP_TOOL_LIBRARIES", "");
    set_environment("ROCPROFILER_REGISTER_LIBRARY", "");
    set_environment("ROCPROFILER_REGISTER_FORCE_LOAD", "");
    set_environment("ROCPROFILER_REGISTER_ENABLED", "1");
    set_environment("ROCPROFILER_REGISTER_SECURE", "1");
    set_environment("ROCPROFILER_WINDOWS_TRACE_LOG", trace_path);

    try
    {
        const auto register_module = load_library(register_path);
        if(mode == "normal")
        {
            load_library(sdk_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", sdk_path);
        }

        const auto hip_module   = load_library(hip_path);
        const auto roctx_module = load_library(roctx_path);
        using hip_api_name_t     = const char* (*)(uint32_t);
        using roctx_mark_t       = void (*)(const char*);
        using roctx_push_t       = int (*)(const char*);
        using roctx_pop_t        = int (*)();
        const auto hip_api_name = get_export<hip_api_name_t>(hip_module, "hipApiName");
        const auto roctx_mark   = get_export<roctx_mark_t>(roctx_module, "roctxMarkA");
        const auto roctx_push   = get_export<roctx_push_t>(roctx_module, "roctxRangePushA");
        const auto roctx_pop    = get_export<roctx_pop_t>(roctx_module, "roctxRangePop");
        const auto* baseline    = hip_api_name(0);
        if(!baseline) return 10;
        const auto baseline_value = std::string{baseline};
        if(mode == "late") roctx_mark("before late registration");

        if(mode == "late")
        {
            load_library(sdk_path);
            set_environment("ROCPROFILER_REGISTER_LIBRARY", sdk_path);
            using invoke_t = int (*)();
            const auto invoke = get_export<invoke_t>(
                register_module, "rocprofiler_register_invoke_nonpropagated_registrations");
            if(invoke() != 0) return 11;
            const auto* registered = hip_api_name(0);
            if(!registered || baseline_value != registered) return 12;
        }

        roctx_mark("registration marker");
        if(roctx_push("registration range") != 0 || roctx_pop() != 0) return 17;
        auto long_message = std::string{"registration long marker: "};
        while(long_message.size() < 4096) long_message += "0123456789abcdef";
        roctx_mark(long_message.c_str());

        const auto trace = read_file(trace_path);
        if(trace.find("event=api_table status=accepted name=hip ") == std::string::npos)
            return 13;
        if(trace.find("event=hip_api phase=enter operation=hipApiName") == std::string::npos)
            return 14;
        if(trace.find("event=hip_api phase=exit operation=hipApiName") == std::string::npos)
            return 15;
        if(trace.find("event=api_table status=accepted name=roctx ") == std::string::npos)
            return 18;
        if(trace.find("event=roctx_marker operation=roctxMarkA kind=mark") == std::string::npos)
            return 19;
        if(trace.find("event=roctx_marker operation=roctxRangePushA kind=thread_range phase=exit") ==
           std::string::npos)
            return 20;
        if(trace.find("message_hex=" + hex_encode(long_message) + "\n") == std::string::npos)
            return 21;
        if(::GetModuleHandleW(L"hsa-runtime64.dll")) return 16;

        std::cout << "windows_sdk_registration=" << mode
                  << " passed hip_result=" << baseline_value
                  << " roctx_markers=yes hsa_loaded=no hsa_initialized=no gpu_work_executed=no\n";
        return 0;
    } catch(const std::exception& error)
    {
        std::cerr << "ERROR: " << error.what() << "\n";
        return 1;
    }
}
