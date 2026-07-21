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

#if !defined(_WIN32)
#    define _GNU_SOURCE 1
#endif

#include "lib/common/dl.hpp"
#include "lib/common/environment.hpp"
#include "lib/common/filesystem.hpp"
#include "lib/common/logging.hpp"
#include "lib/common/utility.hpp"

#include <rocprofiler-sdk/cxx/details/tokenize.hpp>

#include <fmt/format.h>

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <dlfcn.h>
#    include <elf.h>
#    include <link.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace rocprofiler
{
namespace common
{
namespace dl
{
namespace
{
namespace fs = ::rocprofiler::common::filesystem;

const auto default_link_open_modes = open_modes_vec_t{(RTLD_LAZY | RTLD_NOLOAD)};

#if defined(_WIN32)
std::string
path_to_utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<std::string>
get_module_path(HMODULE module, bool canonicalize = false)
{
    if(module == nullptr) return std::nullopt;

    auto path = std::vector<wchar_t>(MAX_PATH);
    while(true)
    {
        const auto size = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if(size == 0) return std::nullopt;
        if(size < path.size() - 1)
        {
            auto fs_path = fs::path{std::wstring_view{path.data(), size}};
            auto ec      = std::error_code{};
            auto result  = (canonicalize) ? fs::canonical(fs_path, ec) : fs::absolute(fs_path, ec);
            if(ec) return std::nullopt;
            return path_to_utf8(result);
        }
        path.resize(path.size() * 2);
    }
}
#endif
}  // namespace

std::optional<std::string>
get_linked_path(std::string_view _name, open_modes_vec_t&& _open_modes)
{
#if defined(_WIN32)
    (void) _open_modes;
    if(_name.empty()) return path_to_utf8(fs::current_path());

    const auto name   = std::string{_name};
    const auto module = ::GetModuleHandleA(name.c_str());
    return get_module_path(module);
#else
    auto dl_iterate_callback = [](struct dl_phdr_info* info, size_t, void* data) -> int {
        auto* _data     = static_cast<std::vector<std::string>*>(data);
        auto  _existing = std::unordered_set<std::string>{};
        for(int i = 0; i < info->dlpi_phnum; ++i)
        {
            if(info->dlpi_phdr[i].p_type == PT_LOAD && info->dlpi_name != nullptr &&
               !std::string_view{info->dlpi_name}.empty())
            {
                if(_existing.emplace(info->dlpi_name).second)
                {
                    ROCP_TRACE << "dl_iterate_phdr found loaded library: " << info->dlpi_name;
                    _data->emplace_back(info->dlpi_name);
                }
            }
        }
        return 0;
    };

    auto loaded_libs = std::vector<std::string>{};
    dl_iterate_phdr(dl_iterate_callback, &loaded_libs);
    for(const auto& itr : loaded_libs)
    {
        ROCP_TRACE << fmt::format("Checking loaded library '{}' for match to '{}'", itr, _name);
        if(fs::path{itr}.filename().string().find(_name) == 0)
        {
            ROCP_INFO << fmt::format(
                "+ Matched '{}' to '{}' from libraries from dl_iterate_phdr", itr, _name);
            return fs::absolute(fs::path{itr}).string();
        }
    }

    // using dlopen, even via RTLD_NOLOAD, can deadlock the application if called from within
    // another dlopen or if the dlopen triggers this library constructor from within this library
    // constructor. This is kept here for debugging purposes only.
    if(common::get_env("ROCPROFILER_LINKED_PATH_USE_DLOPEN", false))
    {
        ROCP_INFO << fmt::format("Attempting to resolve linked path for '{}' via dlopen ... "
                                 "(ROCPROFILER_LINKED_PATH_USE_DLOPEN=1)",
                                 _name);

        if(_name.empty()) return fs::current_path().string();

        if(_open_modes.empty()) _open_modes = default_link_open_modes;

        void* _handle = nullptr;
        bool  _noload = false;
        for(auto _mode : _open_modes)
        {
            _handle = dlopen(_name.data(), _mode);
            _noload = (_mode & RTLD_NOLOAD) == RTLD_NOLOAD;
            if(_handle) break;
        }

        if(_handle)
        {
            struct link_map* _link_map = nullptr;
            dlinfo(_handle, RTLD_DI_LINKMAP, &_link_map);
            if(_link_map != nullptr && !std::string_view{_link_map->l_name}.empty())
            {
                return fs::absolute(fs::path{_link_map->l_name}).string();
            }
            if(_noload == false) dlclose(_handle);
        }
    }

    return std::nullopt;
#endif
}

std::optional<std::string>
get_symbol_path(const std::vector<std::string>& _lib_names,
                std::string_view                _sym_name,
                const void*                     _addr,
                bool                            _canonicalize)
{
#if defined(_WIN32)
    if(_addr != nullptr)
    {
        auto module = HMODULE{};
        if(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(_addr),
                               &module) != 0)
            return get_module_path(module, _canonicalize);
    }

    const auto symbol_name = std::string{_sym_name};
    for(const auto& lib_name : _lib_names)
    {
        const auto module = ::GetModuleHandleA(lib_name.c_str());
        if(module != nullptr && ::GetProcAddress(module, symbol_name.c_str()) != nullptr)
            return get_module_path(module, _canonicalize);
    }

    return std::nullopt;
#else
    if(auto dl_info = Dl_info{}; _addr && dladdr(_addr, &dl_info) != 0 && dl_info.dli_fname)
    {
        ROCP_CI_LOG_IF(WARNING,
                       dl_info.dli_sname && std::string_view{dl_info.dli_sname} != _sym_name)
            << fmt::format("rocprofiler-sdk located '{}' symbol via dladdr in '{}' but the symbol "
                           "name resolves to '{}' instead",
                           _sym_name,
                           dl_info.dli_fname,
                           dl_info.dli_sname);

        auto _ec   = std::error_code{};
        auto _path = (_canonicalize) ? fs::canonical(fs::path{dl_info.dli_fname}, _ec)
                                     : fs::absolute(fs::path{dl_info.dli_fname}, _ec);
        ROCP_CI_LOG_IF(WARNING, _ec) << fmt::format("Error resolving {} path for '{}': {} :: {}",
                                                    (_canonicalize) ? "canonical" : "absolute",
                                                    dl_info.dli_fname,
                                                    _ec.value(),
                                                    _ec.message());
        if(!_ec) return _path.string();
    }

    // using dlopen, even via RTLD_NOLOAD, can deadlock the application if called from within
    // another dlopen or if the dlopen triggers this library constructor from within this library
    // constructor. This is kept here for debugging purposes only.
    if(common::get_env("ROCPROFILER_SYMBOL_PATH_USE_DLOPEN", false))
    {
        ROCP_INFO << fmt::format("Attempting to resolve symbol path for '{}' via dlopen ... "
                                 "(ROCPROFILER_SYMBOL_PATH_USE_DLOPEN=1)",
                                 _sym_name);

        auto  _lib_name = std::string{};
        void* _handle   = nullptr;

        for(const auto& itr : _lib_names)
        {
            if(!itr.empty())
            {
                ROCP_INFO << fmt::format("Attempting dlopen('{}', RTLD_NOLOAD | RTLD_LAZY) ...",
                                         itr);
                void* _handle_v = dlopen(itr.c_str(), RTLD_NOLOAD | RTLD_LAZY);
                if(_handle_v)
                {
                    _lib_name = itr;
                    _handle   = _handle_v;
                    break;
                }
            }
        }

        if(!_handle) _handle = RTLD_DEFAULT;

        const void* _fn = dlsym(_handle, _sym_name.data());
        ROCP_INFO_IF(!_fn) << fmt::format("rocprofiler-sdk could not locate '{}' symbol in {}",
                                          _sym_name,
                                          (_handle) ? _lib_name : "RTLD_DEFAULT");
    }

    return std::nullopt;
#endif
}
}  // namespace dl
}  // namespace common
}  // namespace rocprofiler
