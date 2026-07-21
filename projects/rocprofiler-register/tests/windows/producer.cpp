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

#include <cstring>

namespace
{
constexpr uint32_t registration_version =
    ROCPROFILER_REGISTER_COMPUTE_VERSION_3(7, 0, 0);

int
original_operation(int value)
{
    return value + 1;
}

windows_registration_api_table api_table{sizeof(windows_registration_api_table),
                                         &original_operation};

struct registration_info
{
    bool found = false;
};

int
registration_callback(rocprofiler_register_registration_info_t* info, void* data)
{
    auto* state = static_cast<registration_info*>(data);
    if(info && info->common_name && std::strcmp(info->common_name, "hip") == 0 &&
       info->lib_version == registration_version && info->api_table_length == 1)
        state->found = true;
    return 0;
}

int
register_table(rocprofiler_register_import_func_t import_func, int expected_status)
{
    auto* table = static_cast<void*>(&api_table);
    auto  id    = rocprofiler_register_library_indentifier_t{};
    auto status = rocprofiler_register_library_api_table(
        "hip", import_func, registration_version, &table, 1, &id);
    if(status != expected_status) return 10 + status;
    if(id.handle != 7) return 30;

    auto info = registration_info{};
    if(rocprofiler_register_iterate_registration_info(&registration_callback, &info) !=
       ROCP_REG_SUCCESS)
        return 31;
    if(!info.found) return 32;
    return 0;
}
}  // namespace

ROCPROFILER_REGISTER_DEFINE_IMPORT(hip, registration_version)

extern "C" __declspec(dllexport) int
windows_registration_probe(int mode)
{
    api_table.operation = &original_operation;
    auto expected = (mode == 0) ? ROCP_REG_SUCCESS : ROCP_REG_NO_TOOLS;
    auto status = register_table(&ROCPROFILER_REGISTER_IMPORT_FUNC(hip), expected);
    if(status != 0) return status;
    auto value = api_table.operation(7);
    if(mode == 0 && value != 17) return 40;
    if(mode != 0 && value != 8) return 41;
    return 0;
}

extern "C" __declspec(dllexport) int
windows_registration_probe_with_import(uint32_t (*import_func)(void))
{
    api_table.operation = &original_operation;
    auto* table = static_cast<void*>(&api_table);
    auto  id    = rocprofiler_register_library_indentifier_t{};
    return rocprofiler_register_library_api_table(
        "hip", import_func, registration_version, &table, 1, &id);
}

extern "C" __declspec(dllexport) int
windows_registration_value(int value)
{
    return api_table.operation(value);
}
