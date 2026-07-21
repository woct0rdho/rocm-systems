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

#include "lib/rocprofiler-sdk/windows_trace.hpp"
#include "lib/common/utility.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace rocprofiler
{
namespace windows
{
namespace trace
{
namespace
{
std::atomic<uint64_t> correlation_id{1};

std::wstring
get_trace_path()
{
    const auto required = ::GetEnvironmentVariableW(L"ROCPROFILER_WINDOWS_TRACE_LOG", nullptr, 0);
    if(required == 0) return {};

    auto value = std::wstring(required, L'\0');
    const auto written =
        ::GetEnvironmentVariableW(L"ROCPROFILER_WINDOWS_TRACE_LOG", value.data(), required);
    if(written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}
}  // namespace

uint64_t
next_correlation_id()
{
    return correlation_id.fetch_add(1, std::memory_order_relaxed);
}

uint64_t
timestamp_ns()
{
    return ::rocprofiler::common::timestamp_ns();
}

uint32_t
process_id()
{
    return static_cast<uint32_t>(::rocprofiler::common::get_pid());
}

uint32_t
thread_id()
{
    return static_cast<uint32_t>(::rocprofiler::common::get_tid());
}

std::string
encode_text(const char* value)
{
    if(!value) return {};

    constexpr char digits[] = "0123456789abcdef";
    auto           result   = std::string{};
    for(const auto* itr = reinterpret_cast<const unsigned char*>(value); *itr != 0; ++itr)
    {
        result.push_back(digits[*itr >> 4]);
        result.push_back(digits[*itr & 0x0F]);
    }
    return result;
}

void
append(const char* message)
{
    if(!message || *message == '\0') return;
    const auto path = get_trace_path();
    if(path.empty()) return;

    const auto file = ::CreateFileW(path.c_str(),
                                    FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if(file == INVALID_HANDLE_VALUE) return;

    const auto size = static_cast<DWORD>(std::char_traits<char>::length(message));
    auto       written = DWORD{0};
    if(::WriteFile(file, message, size, &written, nullptr) && written == size)
        ::FlushFileBuffers(file);
    ::CloseHandle(file);
}
}  // namespace trace
}  // namespace windows
}  // namespace rocprofiler
