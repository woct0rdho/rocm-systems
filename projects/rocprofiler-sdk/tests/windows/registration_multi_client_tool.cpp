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
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "lib/common/windows_result.hpp"

#include <rocprofiler-sdk/registration.h>

#include <Windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#if !defined(ROCPROFILER_TEST_TOOL_TAG)
#    error ROCPROFILER_TEST_TOOL_TAG must identify the test client
#endif

namespace
{
uint32_t client_handle = 0;

void
append_event(const char* event, uint32_t value)
{
    char path[32768] = {};
    if(::GetEnvironmentVariableA("ROCPROFILER_MULTI_CLIENT_LOG", path, sizeof(path)) == 0)
        return;
    auto file = ::CreateFileA(path,
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if(file == INVALID_HANDLE_VALUE) return;

    char line[128] = {};
    const auto size = std::snprintf(
        line, sizeof(line), "%s %s %u\n", ROCPROFILER_TEST_TOOL_TAG, event, value);
    auto written = DWORD{0};
    if(size > 0)
        ::WriteFile(file, line, static_cast<DWORD>(size), &written, nullptr);
    ::CloseHandle(file);
}

int
tool_initialize(rocprofiler_client_finalize_t finalize, void*)
{
    append_event("initialize", client_handle);
    auto id = rocprofiler_client_id_t{
        sizeof(rocprofiler_client_id_t), ROCPROFILER_TEST_TOOL_TAG, client_handle};
    finalize(id);
    finalize(id);
    return 0;
}

void
tool_finalize(void*)
{
    append_event("finalize", client_handle);
    char requested[64] = {};
    if(::GetEnvironmentVariableA(
           "ROCPROFILER_MULTI_CLIENT_THROW", requested, sizeof(requested)) > 0 &&
       std::string{requested} == ROCPROFILER_TEST_TOOL_TAG)
        throw std::runtime_error{"requested finalizer failure"};
    if(!rocprofiler::windows::result::write("success_no_dispatch"))
        throw std::runtime_error{"could not publish requested finalizer success"};
}
}  // namespace

extern "C" rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t,
                      const char*,
                      uint32_t                 priority,
                      rocprofiler_client_id_t* client_id)
{
    if(!client_id) return nullptr;
    client_id->name = ROCPROFILER_TEST_TOOL_TAG;
    client_handle   = client_id->handle;
    append_event("configure", priority);
    static auto result = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t), tool_initialize, tool_finalize, nullptr};
    return &result;
}
