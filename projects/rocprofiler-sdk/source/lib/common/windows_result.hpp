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

#pragma once

#if !defined(_WIN32)
#    error windows_result.hpp is only available on Windows
#endif

#include <Windows.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace rocprofiler
{
namespace windows
{
namespace result
{
inline bool
is_nonfailure_status(std::string_view status)
{
    return status == "configured" || status == "initializing" ||
           status == "success_records" || status == "success_no_dispatch" ||
           status == "success_unknown_counter";
}

inline HANDLE
open_file(const std::wstring& output_path,
          DWORD               access,
          DWORD               sharing,
          DWORD               disposition)
{
    constexpr auto attempts = 100;
    for(auto attempt = 0; attempt < attempts; ++attempt)
    {
        const auto file = ::CreateFileW(output_path.c_str(),
                                        access,
                                        sharing,
                                        nullptr,
                                        disposition,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if(file != INVALID_HANDLE_VALUE) return file;

        const auto error = ::GetLastError();
        if(error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) return file;
        if(attempt + 1 < attempts) ::Sleep(1);
    }
    return INVALID_HANDLE_VALUE;
}

inline int
existing_status_action(const std::wstring& output_path, std::string_view new_status)
{
    if(!is_nonfailure_status(new_status)) return 0;

    const auto file = open_file(output_path,
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                OPEN_EXISTING);
    if(file == INVALID_HANDLE_VALUE)
        return (::GetLastError() == ERROR_FILE_NOT_FOUND) ? 0 : -1;

    auto content = std::string(4096, '\0');
    auto count   = DWORD{0};
    const auto read = ::ReadFile(
        file, content.data(), static_cast<DWORD>(content.size()), &count, nullptr);
    const auto closed = ::CloseHandle(file);
    if(!read || !closed) return -1;
    content.resize(count);

    constexpr auto prefix = std::string_view{"status="};
    auto position = content.find(prefix);
    if(position == std::string::npos || (position > 0 && content[position - 1] != '\n'))
        return 1;
    position += prefix.size();
    const auto end = content.find_first_of("\r\n", position);
    const auto status = std::string_view{content}.substr(position, end - position);
    return is_nonfailure_status(status) ? 0 : 1;
}

inline std::wstring
path()
{
    const auto required =
        ::GetEnvironmentVariableW(L"ROCPROFILER_WINDOWS_RESULT_FILE", nullptr, 0);
    if(required == 0) return {};

    auto value   = std::wstring(required, L'\0');
    auto written = ::GetEnvironmentVariableW(
        L"ROCPROFILER_WINDOWS_RESULT_FILE", value.data(), required);
    if(written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

inline bool
write(std::string_view status, std::string_view detail = {})
{
    const auto output_path = path();
    if(output_path.empty()) return true;

    const auto existing_action = existing_status_action(output_path, status);
    if(existing_action > 0) return true;
    if(existing_action < 0) return false;

    auto content = std::string{"version=1\nstatus="};
    content.append(status);
    content.append("\ndetail=");
    for(const auto character : detail)
        content.push_back((character == '\r' || character == '\n') ? ' ' : character);
    content.push_back('\n');

    const auto file = open_file(output_path,
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_DELETE,
                                CREATE_ALWAYS);
    if(file == INVALID_HANDLE_VALUE) return false;

    auto success = true;
    for(size_t offset = 0; offset < content.size();)
    {
        const auto remaining =
            std::min<size_t>(content.size() - offset, 0x7ffff000u);
        auto written = DWORD{0};
        if(!::WriteFile(file,
                        content.data() + offset,
                        static_cast<DWORD>(remaining),
                        &written,
                        nullptr) ||
           written == 0)
        {
            success = false;
            break;
        }
        offset += written;
    }
    if(success) success = (::FlushFileBuffers(file) != FALSE);
    if(::CloseHandle(file) == FALSE) success = false;
    return success;
}
}  // namespace result
}  // namespace windows
}  // namespace rocprofiler
