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

#include "lib/rocprofiler-sdk/hsa/windows_tool.hpp"

#include "lib/rocprofiler-sdk/agent.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/hsa/windows_api.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

namespace
{
constexpr auto hsa_tool_log_environment = L"ROCPROFILER_WINDOWS_HSA_TOOL_LOG";
constexpr auto trace_log_environment    = L"ROCPROFILER_WINDOWS_TRACE_LOG";

std::wstring
get_environment_variable(const wchar_t* name)
{
    const auto required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if(required == 0) return {};

    auto value = std::wstring(required, L'\0');
    const auto written = ::GetEnvironmentVariableW(name, value.data(), required);
    if(written == 0 || written >= required) return {};

    value.resize(written);
    return value;
}

void
append_tool_log(const char* message)
{
    auto path = get_environment_variable(hsa_tool_log_environment);
    if(path.empty()) path = get_environment_variable(trace_log_environment);
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
    ::WriteFile(file, message, size, &written, nullptr);
    ::CloseHandle(file);
}

template <typename TableT, typename MemberT>
bool
has_table_member(const TableT* table, size_t offset, MemberT TableT::*)
{
    return table && table->version.minor_id >= offset + sizeof(MemberT);
}

using queue_create_fn_t       = decltype(CoreApiTable::hsa_queue_create_fn);
using queue_destroy_fn_t      = decltype(CoreApiTable::hsa_queue_destroy_fn);
using intercept_create_fn_t   = decltype(AmdExtTable::hsa_amd_queue_intercept_create_fn);
using intercept_register_fn_t = decltype(AmdExtTable::hsa_amd_queue_intercept_register_fn);

queue_create_fn_t       original_queue_create     = nullptr;
queue_destroy_fn_t      original_queue_destroy    = nullptr;
intercept_create_fn_t   intercept_queue_create    = nullptr;
intercept_register_fn_t intercept_queue_register  = nullptr;
decltype(CoreApiTable::hsa_init_fn) original_hsa_init = nullptr;
decltype(CoreApiTable::hsa_shut_down_fn) original_hsa_shut_down = nullptr;
decltype(CoreApiTable::hsa_executable_iterate_agent_symbols_fn)
    original_iterate_agent_symbols = nullptr;
decltype(CoreApiTable::hsa_executable_symbol_get_info_fn) original_symbol_get_info = nullptr;
HsaApiTable* registered_api_table = nullptr;
std::once_flag queue_controller_once = {};

void
initialize_queue_controller()
{
    std::call_once(queue_controller_once, []() {
        if(!registered_api_table || !registered_api_table->core_ ||
           !registered_api_table->amd_ext_)
            return;
        rocprofiler::agent::construct_agent_cache(registered_api_table);
        rocprofiler::hsa::queue_controller_init(registered_api_table);
    });
}

hsa_status_t
initialize_hsa()
{
    if(!original_hsa_init) return HSA_STATUS_ERROR;
    const auto status = original_hsa_init();
    if(status == HSA_STATUS_SUCCESS) initialize_queue_controller();
    return status;
}

hsa_status_t
shut_down_hsa()
{
    // Finalize SDK clients before tearing down the HSA queue controller. Dispatch-counting
    // clients stop their contexts and drain callback workers during finalization; doing this
    // after queue-controller teardown can strand the callback drain at process exit.
    rocprofiler::registration::finalize();
    return original_hsa_shut_down ? original_hsa_shut_down() : HSA_STATUS_ERROR;
}

struct symbol_callback_data
{
    hsa_status_t (*callback)(hsa_executable_t,
                             hsa_agent_t,
                             hsa_executable_symbol_t,
                             void*) = nullptr;
    void* data = nullptr;
};

void
observe_kernel_symbol(hsa_executable_symbol_t symbol, uint64_t known_kernel_object = 0)
{
    if(!original_symbol_get_info) return;

    auto kind = hsa_symbol_kind_t{};
    if(original_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_TYPE, &kind) !=
           HSA_STATUS_SUCCESS ||
       kind != HSA_SYMBOL_KIND_KERNEL)
        return;

    auto kernel_object = known_kernel_object;
    if(kernel_object == 0 &&
       original_symbol_get_info(symbol,
                                HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,
                                &kernel_object) != HSA_STATUS_SUCCESS)
        return;

    uint32_t name_length = 0;
    if(original_symbol_get_info(symbol,
                                HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH,
                                &name_length) != HSA_STATUS_SUCCESS ||
       name_length == 0 || name_length == std::numeric_limits<uint32_t>::max())
        return;

    auto name = std::string(name_length + 1, '\0');
    if(original_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME, name.data()) !=
       HSA_STATUS_SUCCESS)
        return;
    name.resize(name.find_first_of('\0'));
    rocprofiler::hsa::windows::register_kernel_name(kernel_object, name);
}

hsa_status_t
symbol_get_info(hsa_executable_symbol_t      symbol,
                hsa_executable_symbol_info_t attribute,
                void*                        value)
{
    if(!original_symbol_get_info) return HSA_STATUS_ERROR;
    const auto status = original_symbol_get_info(symbol, attribute, value);
    if(status == HSA_STATUS_SUCCESS && value &&
       attribute == HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT)
        observe_kernel_symbol(symbol, *static_cast<const uint64_t*>(value));
    return status;
}

hsa_status_t
observe_symbol(hsa_executable_t        executable,
               hsa_agent_t             agent,
               hsa_executable_symbol_t symbol,
               void*                   data)
{
    observe_kernel_symbol(symbol);

    auto* callback_data = static_cast<symbol_callback_data*>(data);
    return (callback_data && callback_data->callback)
               ? callback_data->callback(executable, agent, symbol, callback_data->data)
               : HSA_STATUS_SUCCESS;
}

hsa_status_t
iterate_agent_symbols(
    hsa_executable_t executable,
    hsa_agent_t agent,
    hsa_status_t (*callback)(hsa_executable_t, hsa_agent_t, hsa_executable_symbol_t, void*),
    void* data)
{
    if(!original_iterate_agent_symbols) return HSA_STATUS_ERROR;
    auto callback_data = symbol_callback_data{callback, data};
    return original_iterate_agent_symbols(executable, agent, observe_symbol, &callback_data);
}

void
forward_packets(const void*                           packets,
                uint64_t                              packet_count,
                uint64_t                              user_packet_index,
                void*                                 data,
                hsa_amd_queue_intercept_packet_writer writer)
{
    const auto* queue       = static_cast<const hsa_queue_t*>(data);
    const auto  queue_id    = (queue) ? queue->id : 0;
    const auto* packet_data = static_cast<const std::byte*>(packets);
    for(uint64_t index = 0; index < packet_count; ++index)
    {
        const auto* packet = packet_data + (index * sizeof(hsa_kernel_dispatch_packet_t));
        const auto  header = *reinterpret_cast<const uint16_t*>(packet);
        const auto  type = static_cast<uint8_t>((header >> HSA_PACKET_HEADER_TYPE) & 0xFF);

        char message[512] = {};
        if(type == HSA_PACKET_TYPE_KERNEL_DISPATCH)
        {
            const auto* dispatch = reinterpret_cast<const hsa_kernel_dispatch_packet_t*>(packet);
            std::snprintf(message,
                          sizeof(message),
                          "event=kernel_packet queue_id=%llu dispatch_id=%llu "
                          "grid=%u,%u,%u workgroup=%u,%u,%u kernel_object=%llu "
                          "completion_signal=%llu\n",
                          static_cast<unsigned long long>(queue_id),
                          static_cast<unsigned long long>(user_packet_index + index),
                          dispatch->grid_size_x,
                          dispatch->grid_size_y,
                          dispatch->grid_size_z,
                          dispatch->workgroup_size_x,
                          dispatch->workgroup_size_y,
                          dispatch->workgroup_size_z,
                          static_cast<unsigned long long>(dispatch->kernel_object),
                          static_cast<unsigned long long>(dispatch->completion_signal.handle));
        }
        else
        {
            std::snprintf(message,
                          sizeof(message),
                          "event=packet queue_id=%llu packet_id=%llu packet_type=%u\n",
                          static_cast<unsigned long long>(queue_id),
                          static_cast<unsigned long long>(user_packet_index + index),
                          static_cast<unsigned>(type));
        }
        append_tool_log(message);
    }

    if(writer) writer(packets, packet_count);
}

hsa_status_t
create_intercept_queue(
    hsa_agent_t        agent,
    uint32_t           size,
    hsa_queue_type32_t type,
    void (*callback)(hsa_status_t status, hsa_queue_t* source, void* data),
    void*         data,
    uint32_t      private_segment_size,
    uint32_t      group_segment_size,
    hsa_queue_t** queue)
{
    // HSA publishes the tool table from inside the first hsa_init call on
    // Windows. Consequently replacing hsa_init in that table is too late for
    // the first initialization. Queue creation is the first point at which
    // agent enumeration is guaranteed to be usable, so complete the standard
    // queue-controller setup lazily here.
    initialize_queue_controller();
    auto current_create = (registered_api_table && registered_api_table->core_)
                              ? registered_api_table->core_->hsa_queue_create_fn
                              : nullptr;
    if(current_create && current_create != create_intercept_queue)
        return current_create(agent,
                              size,
                              type,
                              callback,
                              data,
                              private_segment_size,
                              group_segment_size,
                              queue);

    // Keep the low-level packet observer available when no SDK context requests
    // queue interception. The queue controller replaces the table entry above
    // when dispatch counting is configured, so this path cannot double-wrap a
    // profiled queue.
    if(!intercept_queue_create || !intercept_queue_register)
    {
        return original_queue_create
                   ? original_queue_create(agent,
                                           size,
                                           type,
                                           callback,
                                           data,
                                           private_segment_size,
                                           group_segment_size,
                                           queue)
                   : HSA_STATUS_ERROR;
    }

    const auto create_status = intercept_queue_create(agent,
                                                      size,
                                                      type,
                                                      callback,
                                                      data,
                                                      private_segment_size,
                                                      group_segment_size,
                                                      queue);
    auto register_status = HSA_STATUS_ERROR;
    if(create_status == HSA_STATUS_SUCCESS && queue && *queue)
    {
        register_status = intercept_queue_register(*queue, forward_packets, *queue);
        if(register_status != HSA_STATUS_SUCCESS && original_queue_destroy)
        {
            original_queue_destroy(*queue);
            *queue = nullptr;
        }
    }

    const auto queue_id =
        (create_status == HSA_STATUS_SUCCESS && register_status == HSA_STATUS_SUCCESS && queue &&
         *queue)
            ? (*queue)->id
            : 0;
    char message[384] = {};
    std::snprintf(message,
                  sizeof(message),
                  "event=queue_create status=%s create_status=%u register_status=%u "
                  "agent_handle=%llu queue_id=%llu size=%u type=%u\n",
                  (create_status == HSA_STATUS_SUCCESS && register_status == HSA_STATUS_SUCCESS)
                      ? "intercepted"
                      : "failed",
                  static_cast<unsigned>(create_status),
                  static_cast<unsigned>(register_status),
                  static_cast<unsigned long long>(agent.handle),
                  static_cast<unsigned long long>(queue_id),
                  size,
                  static_cast<unsigned>(type));
    append_tool_log(message);

    return (create_status != HSA_STATUS_SUCCESS) ? create_status : register_status;
}
}  // namespace

namespace rocprofiler
{
namespace hsa
{
namespace windows
{
bool
set_api_table(::HsaApiTable* api_table, uint64_t runtime_version, uint64_t failed_tool_count)
{
    const auto has_core =
        has_table_member(api_table, offsetof(HsaApiTable, core_), &HsaApiTable::core_);
    const auto has_amd_ext =
        has_table_member(api_table, offsetof(HsaApiTable, amd_ext_), &HsaApiTable::amd_ext_);
    const auto has_queue_create =
        has_core && api_table->core_ &&
        has_table_member(api_table->core_,
                         offsetof(CoreApiTable, hsa_queue_create_fn),
                         &CoreApiTable::hsa_queue_create_fn);
    const auto has_queue_destroy =
        has_core && api_table->core_ &&
        has_table_member(api_table->core_,
                         offsetof(CoreApiTable, hsa_queue_destroy_fn),
                         &CoreApiTable::hsa_queue_destroy_fn);
    const auto has_iterate_agent_symbols =
        has_core && api_table->core_ &&
        has_table_member(api_table->core_,
                         offsetof(CoreApiTable, hsa_executable_iterate_agent_symbols_fn),
                         &CoreApiTable::hsa_executable_iterate_agent_symbols_fn);
    const auto has_symbol_get_info =
        has_core && api_table->core_ &&
        has_table_member(api_table->core_,
                         offsetof(CoreApiTable, hsa_executable_symbol_get_info_fn),
                         &CoreApiTable::hsa_executable_symbol_get_info_fn);
    const auto has_intercept_create =
        has_amd_ext && api_table->amd_ext_ &&
        has_table_member(api_table->amd_ext_,
                         offsetof(AmdExtTable, hsa_amd_queue_intercept_create_fn),
                         &AmdExtTable::hsa_amd_queue_intercept_create_fn);
    const auto has_intercept_register =
        has_amd_ext && api_table->amd_ext_ &&
        has_table_member(api_table->amd_ext_,
                         offsetof(AmdExtTable, hsa_amd_queue_intercept_register_fn),
                         &AmdExtTable::hsa_amd_queue_intercept_register_fn);
    const auto usable =
        api_table && api_table->version.major_id == runtime_version && has_queue_create &&
        api_table->core_->hsa_queue_create_fn && has_queue_destroy &&
        api_table->core_->hsa_queue_destroy_fn && has_intercept_create &&
        api_table->amd_ext_->hsa_amd_queue_intercept_create_fn && has_intercept_register &&
        api_table->amd_ext_->hsa_amd_queue_intercept_register_fn;

    const auto root_major = (api_table) ? api_table->version.major_id : 0;
    const auto root_minor = (api_table) ? api_table->version.minor_id : 0;
    const auto root_step  = (api_table) ? api_table->version.step_id : 0;
    const auto core_major = (has_core && api_table->core_) ? api_table->core_->version.major_id : 0;
    const auto core_minor = (has_core && api_table->core_) ? api_table->core_->version.minor_id : 0;
    const auto amd_major =
        (has_amd_ext && api_table->amd_ext_) ? api_table->amd_ext_->version.major_id : 0;
    const auto amd_minor =
        (has_amd_ext && api_table->amd_ext_) ? api_table->amd_ext_->version.minor_id : 0;

    char message[512] = {};
    std::snprintf(message,
                  sizeof(message),
                  "event=onload status=%s runtime_version=%llu failed_tool_count=%llu "
                  "root_version=%u.%u.%u core_version=%u.%u amd_ext_version=%u.%u\n",
                  (usable) ? "accepted" : "rejected",
                  static_cast<unsigned long long>(runtime_version),
                  static_cast<unsigned long long>(failed_tool_count),
                  root_major,
                  root_minor,
                  root_step,
                  core_major,
                  core_minor,
                  amd_major,
                  amd_minor);
    append_tool_log(message);

    if(usable && api_table->core_->hsa_init_fn != initialize_hsa)
    {
        registered_api_table     = api_table;
        original_hsa_init        = api_table->core_->hsa_init_fn;
        original_hsa_shut_down   = api_table->core_->hsa_shut_down_fn;
        original_queue_create    = api_table->core_->hsa_queue_create_fn;
        original_queue_destroy   = api_table->core_->hsa_queue_destroy_fn;
        if(has_iterate_agent_symbols &&
           api_table->core_->hsa_executable_iterate_agent_symbols_fn != iterate_agent_symbols)
            original_iterate_agent_symbols =
                api_table->core_->hsa_executable_iterate_agent_symbols_fn;
        if(has_symbol_get_info &&
           api_table->core_->hsa_executable_symbol_get_info_fn != symbol_get_info)
            original_symbol_get_info = api_table->core_->hsa_executable_symbol_get_info_fn;
        intercept_queue_create   = api_table->amd_ext_->hsa_amd_queue_intercept_create_fn;
        intercept_queue_register = api_table->amd_ext_->hsa_amd_queue_intercept_register_fn;
        rocprofiler::hsa::windows::install_internal_tables(*api_table->core_,
                                                            *api_table->amd_ext_);
        api_table->core_->hsa_init_fn         = initialize_hsa;
        api_table->core_->hsa_shut_down_fn    = shut_down_hsa;
        api_table->core_->hsa_queue_create_fn = create_intercept_queue;
        if(original_iterate_agent_symbols)
            api_table->core_->hsa_executable_iterate_agent_symbols_fn = iterate_agent_symbols;
        if(original_symbol_get_info)
            api_table->core_->hsa_executable_symbol_get_info_fn = symbol_get_info;
    }

    return usable;
}
}  // namespace windows
}  // namespace hsa
}  // namespace rocprofiler

extern "C" __declspec(dllexport) bool
OnLoad(::HsaApiTable* api_table,
       uint64_t       runtime_version,
       uint64_t       failed_tool_count,
       const char* const*)
{
    return rocprofiler::hsa::windows::set_api_table(
        api_table, runtime_version, failed_tool_count);
}
