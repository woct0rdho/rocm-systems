// Copyright (c) 2023-2026 Advanced Micro Devices, Inc.
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

#include <rocprofiler-register/rocprofiler-register.h>

#include "details/checked_lock.hpp"
#include "details/dl.hpp"
#include "details/environment.hpp"
#include "details/filesystem.hpp"
#include "details/logging.hpp"

#include <fmt/format.h>
#include <glog/logging.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using namespace rocprofiler_register;

using rocprofiler_set_api_table_t = int (*)(const char*, uint64_t, uint64_t, void**, uint64_t);

constexpr auto rocprofiler_lib_name                = "rocprofiler-sdk.dll";
constexpr auto rocprofiler_lib_register_entrypoint = "rocprofiler_set_api_table";
constexpr auto rocprofiler_register_lib_name       = "rocprofiler-register.dll";

enum rocp_reg_supported_library
{
    ROCP_REG_HSA = 0,
    ROCP_REG_HIP,
    ROCP_REG_ROCTX,
    ROCP_REG_HIP_COMPILER,
    ROCP_REG_RCCL,
    ROCP_REG_ROCDECODE,
    ROCP_REG_ROCJPEG,
    ROCP_REG_ROCATTACH,
    ROCP_REG_HIPFILE,
    ROCP_REG_LAST,
};

struct rocp_import
{
    rocp_reg_supported_library library_idx = ROCP_REG_LAST;
    std::string_view           common_name = {};
    std::string_view           library_name_regex = {};
};

constexpr auto import_info = std::array<rocp_import, ROCP_REG_LAST>{
    rocp_import{ROCP_REG_HSA, "hsa", R"(^(amdhip64(?:_[0-9]+)?|hsa-runtime64)\.dll$)"},
    rocp_import{ROCP_REG_HIP, "hip", R"(^amdhip64(?:_[0-9]+)?\.dll$)"},
    rocp_import{ROCP_REG_ROCTX, "roctx", R"(^(rocprofiler-sdk-roctx|roctx64)\.dll$)"},
    rocp_import{ROCP_REG_HIP_COMPILER,
                "hip_compiler",
                R"(^amdhip64(?:_[0-9]+)?\.dll$)"},
    rocp_import{ROCP_REG_RCCL, "rccl", R"(^rccl\.dll$)"},
    rocp_import{ROCP_REG_ROCDECODE, "rocdecode", R"(^rocdecode\.dll$)"},
    rocp_import{ROCP_REG_ROCJPEG, "rocjpeg", R"(^rocjpeg\.dll$)"},
    rocp_import{ROCP_REG_ROCATTACH, "rocattach", R"(^rocprofiler-sdk-attach\.dll$)"},
    rocp_import{ROCP_REG_HIPFILE, "hipFile", R"(^hipfile\.dll$)"},
};

const rocp_import*
find_import(std::string_view common_name)
{
    for(const auto& itr : import_info)
        if(itr.common_name == common_name) return &itr;
    return nullptr;
}

const char*
error_string(rocprofiler_register_error_code_t code)
{
    switch(code)
    {
        case ROCP_REG_SUCCESS: return "Success";
        case ROCP_REG_NO_TOOLS: return "rocprofiler-register found no tools";
        case ROCP_REG_DEADLOCK: return "rocprofiler-register deadlocked";
        case ROCP_REG_BAD_API_TABLE_LENGTH:
            return "Library passed an invalid number of API tables";
        case ROCP_REG_UNSUPPORTED_API: return "Library's API is not supported";
        case ROCP_REG_INVALID_API_ADDRESS:
            return "Invalid API address (secure mode enabled)";
        case ROCP_REG_ROCPROFILER_ERROR:
            return "Unspecified rocprofiler-register error";
        case ROCP_REG_EXCESS_API_INSTANCES:
            return "Too many instances of the same library API were registered";
        case ROCP_REG_INVALID_ARGUMENT:
            return "rocprofiler-register API function was provided an invalid argument";
        case ROCP_REG_ATTACHMENT_NOT_AVAILABLE:
            return "rocprofiler-register attach was invoked, but the attachment library was "
                   "never loaded.";
        case ROCP_REG_ERROR_CODE_END: break;
    }
    return "rocprofiler_register_unknown_error";
}

std::string
get_this_library_directory()
{
    auto path = binary::get_linked_path(
        rocprofiler_register_lib_name,
        {binary::open_mode_no_load | binary::open_mode_lazy});
    if(!path) return {};
    return fs::path{*path}.parent_path().string();
}

binary::library_handle_t
get_library_handle(std::string_view library)
{
    if(library.empty()) return nullptr;

    auto requested  = fs::path{library};
    auto candidates = std::vector<fs::path>{};
    if(!requested.is_absolute())
    {
        auto directory = get_this_library_directory();
        if(!directory.empty()) candidates.emplace_back(fs::path{directory} / requested);
    }
    candidates.emplace_back(requested);
    if(!requested.is_absolute() && requested.has_parent_path())
        candidates.emplace_back(requested.filename());

    for(const auto& candidate : candidates)
    {
        if(auto* handle = binary::open_library(
               candidate.string(), binary::open_mode_no_load | binary::open_mode_lazy))
        {
            LOG(INFO) << "found loaded " << candidate.string() << " (handle=" << handle << ")";
            return handle;
        }
    }

    for(const auto& candidate : candidates)
    {
        if(auto* handle = binary::open_library(
               candidate.string(), binary::open_mode_global | binary::open_mode_lazy))
        {
            LOG(INFO) << "loaded " << candidate.string() << " (handle=" << handle << ")";
            return handle;
        }
    }

    LOG(WARNING) << "failed to load " << library;
    return nullptr;
}

struct rocp_scan_data
{
    rocprofiler_set_api_table_t set_api_table_fn = nullptr;
};

rocp_scan_data
scan_library(std::string_view library)
{
    auto* handle = get_library_handle(library);
    if(!handle) return {};

    auto* set_api = reinterpret_cast<rocprofiler_set_api_table_t>(
        binary::get_symbol(handle, rocprofiler_lib_register_entrypoint));
    if(!set_api)
    {
        LOG(WARNING) << library << " did not export "
                     << rocprofiler_lib_register_entrypoint;
        return {};
    }

    return rocp_scan_data{set_api};
}

rocp_scan_data
scan_for_tools()
{
    auto tool_libraries = common::get_env("ROCP_TOOL_LIBRARIES", std::string{});
    auto register_library =
        common::get_env("ROCPROFILER_REGISTER_LIBRARY", std::string{});
    auto force_tool = common::get_env("ROCPROFILER_REGISTER_FORCE_LOAD",
                                      !register_library.empty() || !tool_libraries.empty());

    // An explicit registration-library path is authoritative. Do not silently bind to
    // an unrelated loaded module which happens to export the same entry point.
    if(!register_library.empty()) return scan_library(register_library);

    auto configure = binary::get_default_symbol("rocprofiler_configure");
    auto set_table = binary::get_default_symbol(rocprofiler_lib_register_entrypoint);
    if(set_table)
    {
        return rocp_scan_data{reinterpret_cast<rocprofiler_set_api_table_t>(set_table)};
    }

    if(!configure && !force_tool) return {};
    return scan_library(rocprofiler_lib_name);
}

struct registered_library_api_table
{
    bool                               propagated     = false;
    bool                               propagating    = false;
    std::string                        common_name    = {};
    rocprofiler_register_import_func_t import_func    = nullptr;
    uint32_t                           lib_version    = 0;
    std::vector<void*>                 api_tables     = {};
    uint64_t                           instance_value = 0;
};

constexpr auto instance_bits = sizeof(uint64_t) * 8;
constexpr auto offset_factor = instance_bits / ((ROCP_REG_LAST > 8) ? ROCP_REG_LAST : 8);
constexpr auto max_instances = offset_factor * ROCP_REG_LAST;
static_assert(max_instances <= instance_bits,
              "ROCP_REG_LAST has exceeded the registration-handle capacity");

auto instance_counters = std::array<std::atomic_uint64_t, ROCP_REG_LAST>{};
auto registered =
    std::array<std::optional<registered_library_api_table>, max_instances>{};
auto registration_mutex = common::checked_mutex{};

std::optional<registered_library_api_table>*
add_registration(std::string_view                    common_name,
                 rocprofiler_register_import_func_t import_func,
                 uint32_t                           lib_version,
                 void**                             api_tables,
                 uint64_t                           api_table_length,
                 uint64_t                           instance_value)
{
    auto tables = std::vector<void*>{};
    tables.reserve(api_table_length);
    for(uint64_t i = 0; i < api_table_length; ++i)
        tables.emplace_back(api_tables[i]);

    auto entry = registered_library_api_table{false,
                                              false,
                                              std::string{common_name},
                                              import_func,
                                              lib_version,
                                              std::move(tables),
                                              instance_value};

    for(auto& itr : registered)
    {
        if(!itr)
        {
            itr = std::move(entry);
            return &itr;
        }
    }
    return nullptr;
}

bool
address_in_ranges(uintptr_t address, const std::vector<binary::address_range>& ranges)
{
    for(const auto& range : ranges)
        if(address >= range.start && address < range.last) return true;
    return false;
}

bool
secure_import_address_valid(const rocp_import& import,
                            rocprofiler_register_import_func_t import_func)
{
    if(!import_func) return true;
    auto address = reinterpret_cast<uintptr_t>(import_func);
    auto expression = std::regex{std::string{import.library_name_regex},
                                 std::regex_constants::icase};
    for(const auto& module : binary::get_segment_addresses())
    {
        if(address_in_ranges(address, module.ranges) &&
           std::regex_search(fs::path{module.filepath}.filename().string(), expression))
            return true;
    }
    return false;
}

rocprofiler_register_error_code_t
invoke_registrations(bool invoke_all)
{
    auto pending = std::vector<size_t>{};
    {
        auto lock = common::checked_lock{registration_mutex};
        if(lock.recursive) return ROCP_REG_DEADLOCK;
        for(size_t index = 0; index < registered.size(); ++index)
        {
            const auto& itr = registered[index];
            if(itr && !itr->propagating && (!itr->propagated || invoke_all))
                pending.emplace_back(index);
        }
    }

    // Loading the SDK may load and configure ROCP_TOOL_LIBRARIES. Keep loader and
    // client code outside the registration lock so a configure callback can query
    // or replay the registration snapshot without recursive-lock failure.
    auto scan = scan_for_tools();
    if(!scan.set_api_table_fn) return ROCP_REG_NO_TOOLS;
    for(const auto index : pending)
    {
        auto snapshot = registered_library_api_table{};
        {
            auto lock = common::checked_lock{registration_mutex};
            if(lock.recursive) return ROCP_REG_DEADLOCK;
            if(!registered[index] || registered[index]->propagating ||
               (!invoke_all && registered[index]->propagated))
                continue;
            registered[index]->propagating = true;
            snapshot.common_name           = registered[index]->common_name;
            snapshot.lib_version           = registered[index]->lib_version;
            snapshot.api_tables            = registered[index]->api_tables;
            snapshot.instance_value        = registered[index]->instance_value;
        }

        auto status = scan.set_api_table_fn(snapshot.common_name.c_str(),
                                            snapshot.lib_version,
                                            snapshot.instance_value,
                                            snapshot.api_tables.data(),
                                            snapshot.api_tables.size());
        {
            auto lock = common::checked_lock{registration_mutex};
            if(lock.recursive) return ROCP_REG_DEADLOCK;
            if(registered[index])
            {
                registered[index]->propagating = false;
                if(status == 0) registered[index]->propagated = true;
            }
        }
        if(status != 0) return ROCP_REG_ROCPROFILER_ERROR;
    }
    return ROCP_REG_SUCCESS;
}
}  // namespace

extern "C" {
ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_library_api_table(
    const char*                                 common_name,
    rocprofiler_register_import_func_t          import_func,
    uint32_t                                    lib_version,
    void**                                      api_tables,
    uint64_t                                    api_table_length,
    rocprofiler_register_library_indentifier_t* register_id)
{
    if(!common_name || !api_tables || !register_id) return ROCP_REG_INVALID_ARGUMENT;
    if(api_table_length < 1) return ROCP_REG_BAD_API_TABLE_LENGTH;

    logging::initialize();
    if(!common::get_env("ROCPROFILER_REGISTER_ENABLED", true)) return ROCP_REG_NO_TOOLS;

    auto*    import         = find_import(common_name);
    uint64_t instance       = 0;
    size_t   registered_idx = 0;
    if(!import) return ROCP_REG_UNSUPPORTED_API;
    if(common::get_env("ROCPROFILER_REGISTER_SECURE", false) &&
       !secure_import_address_valid(*import, import_func))
        return ROCP_REG_INVALID_API_ADDRESS;

    {
        auto lock = common::checked_lock{registration_mutex};
        if(lock.recursive) return ROCP_REG_DEADLOCK;
        if(instance_counters.at(import->library_idx) >= offset_factor)
            return ROCP_REG_EXCESS_API_INSTANCES;

        instance = instance_counters.at(import->library_idx)++;
        register_id->handle = (offset_factor * import->library_idx) + instance;
        auto* entry = add_registration(
            common_name, import_func, lib_version, api_tables, api_table_length, instance);
        if(!entry) return ROCP_REG_EXCESS_API_INSTANCES;
        (*entry)->propagating = true;
        registered_idx        = static_cast<size_t>(entry - registered.data());
    }

    // scan_for_tools can load rocprofiler-sdk.dll, and the SDK can in turn load
    // the configured tool DLL. Neither operation may run under registration_mutex.
    auto scan = scan_for_tools();
    if(!scan.set_api_table_fn)
    {
        auto lock = common::checked_lock{registration_mutex};
        if(lock.recursive) return ROCP_REG_DEADLOCK;
        if(registered_idx < registered.size() && registered[registered_idx])
            registered[registered_idx]->propagating = false;
        return ROCP_REG_NO_TOOLS;
    }
    auto status = scan.set_api_table_fn(
        common_name, lib_version, instance, api_tables, api_table_length);

    auto lock = common::checked_lock{registration_mutex};
    if(lock.recursive) return ROCP_REG_DEADLOCK;
    if(registered_idx < registered.size() && registered[registered_idx])
    {
        registered[registered_idx]->propagating = false;
        if(status == 0) registered[registered_idx]->propagated = true;
    }
    return (status == 0) ? ROCP_REG_SUCCESS : ROCP_REG_ROCPROFILER_ERROR;
}

ROCPROFILER_REGISTER_PUBLIC_API const char*
rocprofiler_register_error_string(rocprofiler_register_error_code_t code)
{
    return error_string(code);
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_iterate_registration_info(
    rocprofiler_register_registration_info_cb_t callback,
    void*                                       data)
{
    if(!callback) return ROCP_REG_INVALID_ARGUMENT;

    auto snapshot = std::vector<rocprofiler_register_registration_info_t>{};
    {
        auto lock = common::checked_lock{registration_mutex};
        if(lock.recursive) return ROCP_REG_DEADLOCK;
        for(const auto& itr : registered)
        {
            if(itr)
                snapshot.emplace_back(rocprofiler_register_registration_info_t{
                    sizeof(rocprofiler_register_registration_info_t),
                    itr->common_name.c_str(),
                    itr->lib_version,
                    itr->api_tables.size()});
        }
    }

    for(auto& info : snapshot)
        if(callback(&info, data) != 0) break;
    return ROCP_REG_SUCCESS;
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_invoke_nonpropagated_registrations(void)
{
    return invoke_registrations(false);
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_invoke_all_registrations(void)
{
    return invoke_registrations(true);
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_invoke_prestore_loads(void)
{
    return ROCP_REG_SUCCESS;
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_attach(const char*, const char*)
{
    return ROCP_REG_ATTACHMENT_NOT_AVAILABLE;
}

ROCPROFILER_REGISTER_PUBLIC_API rocprofiler_register_error_code_t
rocprofiler_register_detach(void)
{
    return ROCP_REG_ATTACHMENT_NOT_AVAILABLE;
}
}
