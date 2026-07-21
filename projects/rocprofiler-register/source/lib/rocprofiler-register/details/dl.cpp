// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

#define GNU_SOURCE 1

#include "dl.hpp"
#include "filesystem.hpp"
#include "utility.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#    include <windows.h>
#    include <TlHelp32.h>
#else
#    include <elf.h>
#    include <fmt/format.h>
#    include <link.h>
#endif

namespace rocprofiler_register
{
namespace binary
{
namespace
{
const open_modes_vec_t default_link_open_modes = {open_mode_lazy | open_mode_no_load};

#if defined(_WIN32)
std::wstring
widen(std::string_view value)
{
    if(value.empty()) return {};
    auto size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if(size <= 0)
        size = MultiByteToWideChar(
            CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if(size <= 0) return {};
    auto output = std::wstring(static_cast<size_t>(size), L'\0');
    auto code_page = (MultiByteToWideChar(CP_UTF8,
                                          MB_ERR_INVALID_CHARS,
                                          value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr,
                                          0) > 0)
                         ? CP_UTF8
                         : CP_ACP;
    MultiByteToWideChar(code_page,
                        (code_page == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size);
    return output;
}

std::string
narrow(std::wstring_view value)
{
    if(value.empty()) return {};
    auto size = WideCharToMultiByte(CP_UTF8,
                                    0,
                                    value.data(),
                                    static_cast<int>(value.size()),
                                    nullptr,
                                    0,
                                    nullptr,
                                    nullptr);
    if(size <= 0) return {};
    auto output = std::string(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size,
                        nullptr,
                        nullptr);
    return output;
}

HANDLE
create_module_snapshot(process_id_t pid)
{
    for(int attempt = 0; attempt < 8; ++attempt)
    {
        auto snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if(snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            return snapshot;
    }
    return INVALID_HANDLE_VALUE;
}

bool
same_path_or_name(std::wstring_view requested, const MODULEENTRY32W& entry)
{
    auto requested_path = fs::path{requested};
    auto module_path    = fs::path{entry.szExePath};
    if(requested_path.has_parent_path())
    {
        auto error = std::error_code{};
        if(!requested_path.is_absolute()) requested_path = fs::absolute(requested_path, error);
        if(error) return false;
        auto requested_text = requested_path.lexically_normal().wstring();
        auto module_text    = module_path.lexically_normal().wstring();
        return _wcsicmp(requested_text.c_str(), module_text.c_str()) == 0;
    }
    return _wcsicmp(requested_path.filename().c_str(), module_path.filename().c_str()) == 0;
}

HMODULE
find_loaded_module(std::wstring_view name, process_id_t pid = GetCurrentProcessId())
{
    if(name.empty()) return GetModuleHandleW(nullptr);

    auto snapshot = create_module_snapshot(pid);
    if(snapshot == INVALID_HANDLE_VALUE) return nullptr;

    auto entry   = MODULEENTRY32W{};
    entry.dwSize = sizeof(entry);
    auto module  = static_cast<HMODULE>(nullptr);
    if(Module32FirstW(snapshot, &entry))
    {
        do
        {
            if(same_path_or_name(name, entry))
            {
                module = entry.hModule;
                break;
            }
        } while(Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return module;
}
#endif
}  // namespace

process_id_t
current_process_id()
{
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

std::vector<segment_address_ranges>
get_segment_addresses(process_id_t pid)
{
    auto data = std::vector<segment_address_ranges>{};
#if defined(_WIN32)
    auto snapshot = create_module_snapshot(pid);
    if(snapshot == INVALID_HANDLE_VALUE) return data;

    auto entry   = MODULEENTRY32W{};
    entry.dwSize = sizeof(entry);
    if(Module32FirstW(snapshot, &entry))
    {
        do
        {
            auto start = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            auto last  = start + static_cast<uintptr_t>(entry.modBaseSize);
            data.emplace_back(segment_address_ranges{
                narrow(entry.szExePath), std::vector<address_range>{{start, last}}});
        } while(Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    auto fname = fmt::format("/{}/{}/{}", "proc", pid, "maps");
    auto ifs   = std::ifstream{fname};
    if(!ifs)
    {
        fprintf(stderr, "Failure opening %s\n", fname.c_str());
    }
    else
    {
        auto get_entry = [&data](std::string_view name) -> segment_address_ranges& {
            for(auto& itr : data)
            {
                if(itr.filepath == name) return itr;
            }
            return data.emplace_back(
                segment_address_ranges{.filepath = std::string{name}});
        };

        while(ifs)
        {
            auto line = std::string{};
            if(std::getline(ifs, line) && !line.empty())
            {
                auto delim = utility::delimit(line, " \t\n\r");
                if(delim.size() > 5 && fs::exists(fs::path{delim.back()}))
                {
                    auto& entry = get_entry(delim.back());
                    auto  addr  = utility::delimit(delim.front(), "-");
                    auto  start = std::stoull(addr.front(), nullptr, 16);
                    auto  last  = std::stoull(addr.back(), nullptr, 16);
                    entry.ranges.emplace_back(address_range{start, last});
                }
            }
        }
    }
#endif
    return data;
}

library_handle_t
open_library(std::string_view name, int mode)
{
#if defined(_WIN32)
    auto wide_name = widen(name);
    if(auto* loaded = find_loaded_module(wide_name)) return loaded;
    if((mode & open_mode_no_load) == open_mode_no_load) return nullptr;
    return LoadLibraryW(wide_name.c_str());
#else
    return dlopen(name.empty() ? nullptr : std::string{name}.c_str(), mode);
#endif
}

void*
get_symbol(library_handle_t handle, std::string_view symbol)
{
    if(!handle || symbol.empty()) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), std::string{symbol}.c_str()));
#else
    return dlsym(handle, std::string{symbol}.c_str());
#endif
}

void*
get_default_symbol(std::string_view symbol)
{
    if(symbol.empty()) return nullptr;
#if defined(_WIN32)
    auto snapshot = create_module_snapshot(GetCurrentProcessId());
    if(snapshot == INVALID_HANDLE_VALUE) return nullptr;

    auto entry   = MODULEENTRY32W{};
    entry.dwSize = sizeof(entry);
    auto result  = static_cast<void*>(nullptr);
    if(Module32FirstW(snapshot, &entry))
    {
        do
        {
            result = reinterpret_cast<void*>(
                GetProcAddress(entry.hModule, std::string{symbol}.c_str()));
            if(result) break;
        } while(Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
#else
    return dlsym(RTLD_DEFAULT, std::string{symbol}.c_str());
#endif
}

void
close_library(library_handle_t handle)
{
    if(!handle) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::optional<std::string>
get_library_path(library_handle_t handle)
{
    if(!handle) return std::nullopt;
#if defined(_WIN32)
    auto buffer = std::vector<wchar_t>(MAX_PATH);
    while(true)
    {
        auto size = GetModuleFileNameW(
            static_cast<HMODULE>(handle), buffer.data(), static_cast<DWORD>(buffer.size()));
        if(size == 0) return std::nullopt;
        if(size < buffer.size())
            return narrow(std::wstring_view{buffer.data(), static_cast<size_t>(size)});
        buffer.resize(buffer.size() * 2);
    }
#else
    struct link_map* link_map = nullptr;
    if(dlinfo(handle, RTLD_DI_LINKMAP, &link_map) == 0 && link_map != nullptr &&
       !std::string_view{link_map->l_name}.empty())
        return fs::absolute(fs::path{link_map->l_name}).string();
    return std::nullopt;
#endif
}

std::optional<std::string>
get_linked_path(std::string_view name, open_modes_vec_t&& open_modes)
{
    if(name.empty()) return fs::current_path().string();
    if(open_modes.empty()) open_modes = default_link_open_modes;

    for(auto mode : open_modes)
    {
        if(auto* handle = open_library(name, mode))
        {
            auto path = get_library_path(handle);
#if !defined(_WIN32)
            if((mode & open_mode_no_load) != open_mode_no_load) close_library(handle);
#endif
            if(path) return path;
        }
    }
    return std::nullopt;
}
}  // namespace binary
}  // namespace rocprofiler_register
