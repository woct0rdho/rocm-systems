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

#include "lib/common/dl.hpp"
#include "lib/common/environment.hpp"
#include "lib/common/logging.hpp"
#include "lib/common/windows_result.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/hip/windows_registration.hpp"
#include "lib/rocprofiler-sdk/hsa/windows_tool.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/internal_threading.hpp"
#include "lib/rocprofiler-sdk/roctx/windows_registration.hpp"
#include "lib/rocprofiler-sdk/windows_trace.hpp"

#include <rocprofiler-register/rocprofiler-register.h>
#include <rocprofiler-sdk/experimental/registration.h>
#include <rocprofiler-sdk/version.h>

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rocprofiler
{
namespace registration
{
namespace
{
constexpr auto init_uninitialized = 0;
constexpr auto init_in_progress   = -1;
constexpr auto init_failed        = -2;
constexpr auto init_complete      = 1;

using configure_func_t = rocprofiler_tool_configure_result_t* (*)(uint32_t,
                                                                  const char*,
                                                                  uint32_t,
                                                                  rocprofiler_client_id_t*);

struct client_record
{
    explicit client_record(uint32_t priority)
    : internal_id{sizeof(rocprofiler_client_id_t), nullptr, get_client_offset() + priority}
    {}

    rocprofiler_client_id_t             internal_id = {};
    rocprofiler_tool_configure_result_t result      = {};
    bool                                initialized = false;
    bool                                finalized   = false;
};

std::atomic<int> init_status{init_uninitialized};
std::atomic<int> fini_status{0};
std::mutex       lifecycle_mutex = {};
std::mutex       finalization_mutex = {};
std::condition_variable lifecycle_condition = {};
std::thread::id  initializing_thread = {};
std::vector<std::unique_ptr<client_record>> clients = {};
struct pending_api_table
{
    std::string        name{};
    uint64_t           lib_version  = 0;
    uint64_t           lib_instance = 0;
    std::vector<void*> tables{};
};
std::vector<pending_api_table> deferred_api_tables = {};
bool atexit_registered = false;

void append_trace(const char* message);
int apply_api_table(const char*, uint64_t, uint64_t, void**, uint64_t);
void client_finalize(rocprofiler_client_id_t);

void
append_lifecycle(const char* phase)
{
    char message[256] = {};
    std::snprintf(message,
                  sizeof(message),
                  "event=sdk_lifecycle phase=%s process_id=%u thread_id=%u timestamp_ns=%llu\n",
                  phase,
                  ::rocprofiler::windows::trace::process_id(),
                  ::rocprofiler::windows::trace::thread_id(),
                  static_cast<unsigned long long>(
                      ::rocprofiler::windows::trace::timestamp_ns()));
    ::rocprofiler::windows::trace::append(message);
}

void
append_trace(const char* message)
{
    ::rocprofiler::windows::trace::append(message);
}

void
record_failure(std::string_view status, std::string_view detail)
{
    if(!::rocprofiler::windows::result::write(status, detail))
        std::fprintf(stderr, "rocprofiler-sdk could not publish its private result status\n");
    auto message = std::string{"event=sdk_error status="};
    message.append(status);
    message.append(" detail=");
    message.append(detail);
    message.push_back('\n');
    append_trace(message.c_str());
}

client_record*
configure_client(configure_func_t configure, uint32_t priority, bool* failed = nullptr)
{
    if(!configure) return nullptr;

    auto value = std::make_unique<client_record>(priority);

    rocprofiler::context::push_client(value->internal_id.handle);
    rocprofiler_tool_configure_result_t* configured = nullptr;
    try
    {
        configured = configure(ROCPROFILER_VERSION,
                               ROCPROFILER_VERSION_STRING,
                               priority,
                               &value->internal_id);
    } catch(...)
    {
        if(failed) *failed = true;
        rocprofiler::context::pop_client(value->internal_id.handle);
        rocprofiler::context::deactivate_client_contexts(value->internal_id);
        rocprofiler::context::deregister_client_contexts(value->internal_id);
        record_failure("tool_configure_failed", "rocprofiler_configure raised an exception");
        return nullptr;
    }
    rocprofiler::context::pop_client(value->internal_id.handle);

    if(!configured)
    {
        rocprofiler::context::deactivate_client_contexts(value->internal_id);
        rocprofiler::context::deregister_client_contexts(value->internal_id);
        return nullptr;
    }

    value->result = *configured;
    auto* output      = value.get();
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        clients.emplace_back(std::move(value));
    }
    return output;
}

bool
configure_tool_libraries()
{
    auto libraries = ::rocprofiler::common::get_env("ROCP_TOOL_LIBRARIES", std::string{});
    if(libraries.empty()) return true;

    auto loaded_count          = size_t{0};
    auto configured_count      = size_t{0};
    auto configuration_failure = false;
    auto priority              = uint32_t{0};
    for(size_t begin = 0; begin <= libraries.size();)
    {
        const auto end = libraries.find(';', begin);
        const auto path = libraries.substr(begin,
                                           end == std::string::npos ? std::string::npos
                                                                    : end - begin);
        begin = (end == std::string::npos) ? libraries.size() + 1 : end + 1;
        if(path.empty()) continue;

        auto module = ::LoadLibraryA(path.c_str());
        if(!module)
        {
            char detail[512] = {};
            std::snprintf(detail,
                          sizeof(detail),
                          "LoadLibrary failed for %s with error %lu",
                          path.c_str(),
                          static_cast<unsigned long>(::GetLastError()));
            record_failure("tool_load_failed", detail);
            configuration_failure = true;
            ++priority;
            continue;
        }
        ++loaded_count;

        auto configure = reinterpret_cast<configure_func_t>(
            ::GetProcAddress(module, "rocprofiler_configure"));
        if(!configure)
        {
            record_failure("tool_configure_missing", path);
            configuration_failure = true;
            ::FreeLibrary(module);
            ++priority;
            continue;
        }

        if(configure_client(configure, priority, &configuration_failure))
        {
            ++configured_count;
        }
        else
        {
            ::FreeLibrary(module);
        }
        ++priority;
    }

    if(configured_count == 0)
    {
        record_failure("tool_configure_failed",
                       loaded_count == 0 ? "no tool library could be loaded"
                                         : "no tool accepted the configuration");
        return false;
    }
    return !configuration_failure;
}

bool
initialize_clients()
{
    auto snapshot = std::vector<client_record*>{};
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        snapshot.reserve(clients.size());
        for(auto& client : clients)
            snapshot.emplace_back(client.get());
    }

    auto success = true;
    for(auto* client : snapshot)
    {
        {
            auto lock          = std::lock_guard<std::mutex>{lifecycle_mutex};
            client->initialized = true;
        }

        auto status = 0;
        if(client->result.initialize)
        {
            rocprofiler::context::push_client(client->internal_id.handle);
            try
            {
                status = client->result.initialize(&client_finalize, client->result.tool_data);
            } catch(...)
            {
                status = -1;
            }
            rocprofiler::context::pop_client(client->internal_id.handle);
        }

        if(status != 0)
        {
            {
                auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
                if(!client->finalized) client->initialized = false;
            }
            record_failure("client_initialize_failed",
                           client->internal_id.name ? client->internal_id.name : "unnamed client");
            success = false;
        }
    }

    auto needs_atexit = false;
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        for(const auto& client : clients)
            needs_atexit = needs_atexit ||
                           (client->initialized && !client->finalized && client->result.finalize);
        if(needs_atexit && !atexit_registered)
        {
            std::atexit(&rocprofiler::registration::finalize);
            atexit_registered = true;
        }
    }
    return success;
}

bool
drain_deferred_api_tables()
{
    auto success = true;
    while(true)
    {
        auto pending = std::vector<pending_api_table>{};
        {
            auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
            if(deferred_api_tables.empty()) break;
            pending.swap(deferred_api_tables);
        }
        for(auto& entry : pending)
        {
            if(apply_api_table(entry.name.c_str(),
                               entry.lib_version,
                               entry.lib_instance,
                               entry.tables.data(),
                               entry.tables.size()) != 0)
                success = false;
        }
    }
    return success;
}

bool
defer_reentrant_api_table(const char* name,
                          uint64_t    lib_version,
                          uint64_t    lib_instance,
                          void**      tables,
                          uint64_t    num_tables)
{
    auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
    if(init_status.load(std::memory_order_acquire) != init_in_progress ||
       initializing_thread != std::this_thread::get_id())
        return false;
    auto copied_tables = std::vector<void*>(tables, tables + num_tables);
    deferred_api_tables.emplace_back(pending_api_table{
        name, lib_version, lib_instance, std::move(copied_tables)});
    return true;
}

void
complete_initialization(bool success)
{
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        initializing_thread = {};
        init_status.store(success ? init_complete : init_failed, std::memory_order_release);
    }
    lifecycle_condition.notify_all();
    if(!success)
        record_failure("sdk_initialization_failed",
                       "one or more configured profiler clients failed to initialize");
}

void
invoke_client_finalizer(client_record& client)
{
    if(!client.result.finalize) return;
    try
    {
        client.result.finalize(client.result.tool_data);
    } catch(...)
    {
        record_failure("client_finalize_failed",
                       client.internal_id.name ? client.internal_id.name : "unnamed client");
    }
}

void
finalize_one(rocprofiler_client_id_t id)
{
    auto* client = static_cast<client_record*>(nullptr);
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        for(auto& candidate : clients)
        {
            if(candidate->internal_id.handle != id.handle || !candidate->initialized ||
               candidate->finalized)
                continue;
            candidate->finalized = true;
            client               = candidate.get();
            break;
        }
    }
    if(!client) return;

    auto finalization_lock = std::lock_guard<std::mutex>{finalization_mutex};
    ::rocprofiler::context::stop_client_contexts(client->internal_id);
    ::rocprofiler::hsa::queue_controller_sync();
    invoke_client_finalizer(*client);
    ::rocprofiler::context::deactivate_client_contexts(client->internal_id);
}

void
client_finalize(rocprofiler_client_id_t id)
{
    auto client_count = size_t{0};
    {
        auto lock   = std::lock_guard<std::mutex>{lifecycle_mutex};
        client_count = clients.size();
    }
    if(client_count == 1)
        finalize();
    else
        finalize_one(id);
}

void
set_register_library_to_self()
{
    const auto path = ::rocprofiler::common::dl::get_symbol_path(
        {},
        "rocprofiler_set_api_table",
        reinterpret_cast<const void*>(&rocprofiler_set_api_table));
    if(path) ::rocprofiler::common::set_env("ROCPROFILER_REGISTER_LIBRARY", *path, 1);
}

int
apply_api_table(const char* name,
                uint64_t    lib_version,
                uint64_t    lib_instance,
                void**      tables,
                uint64_t    num_tables)
{
    auto accepted = false;
    if(std::string_view{name} == "hip")
    {
        accepted = (num_tables == 1) && rocprofiler::hip::windows::set_api_table(tables[0]);
    }
    else if(std::string_view{name} == "hsa")
    {
        auto* table = static_cast<HsaApiTable*>(tables[0]);
        accepted = (num_tables == 1) && table &&
                   rocprofiler::hsa::windows::set_api_table(
                       table, table->version.major_id, 0);
    }
    else if(std::string_view{name} == "roctx")
    {
        accepted = rocprofiler::roctx::windows::set_api_tables(tables, num_tables);
    }
    else if(std::string_view{name} == "hip_compiler" ||
            std::string_view{name} == "hip_tools")
    {
        accepted = (num_tables == 1);
    }

    char message[320] = {};
    std::snprintf(message,
                  sizeof(message),
                  "event=api_table status=%s name=%s lib_version=%llu lib_instance=%llu "
                  "num_tables=%llu\n",
                  accepted ? "accepted" : "rejected",
                  name,
                  static_cast<unsigned long long>(lib_version),
                  static_cast<unsigned long long>(lib_instance),
                  static_cast<unsigned long long>(num_tables));
    append_trace(message);
    return accepted ? 0 : ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
}

int
registration_info_callback(rocprofiler_register_registration_info_t* info, void* data)
{
    auto* pair = static_cast<
        std::pair<rocprofiler_runtime_registration_info_cb_t, void*>*>(data);
    if(!info || !pair || !pair->first) return 1;

    auto converted = rocprofiler_runtime_registration_info_t{
        sizeof(rocprofiler_runtime_registration_info_t),
        info->common_name,
        info->lib_version,
        info->api_table_length};
    return pair->first(&converted, pair->second);
}

struct lifecycle_guard
{
    ~lifecycle_guard()
    {
        if(init_status.load(std::memory_order_acquire) != init_uninitialized &&
           fini_status.load(std::memory_order_acquire) == 0)
            finalize();
    }
};

lifecycle_guard process_lifecycle = {};
}  // namespace

void
init_logging()
{
    common::init_logging("ROCPROFILER");
}

void
initialize()
{
    {
        auto lock = std::unique_lock<std::mutex>{lifecycle_mutex};
        const auto status = init_status.load(std::memory_order_acquire);
        if(status == init_complete || status == init_failed) return;
        if(status == init_in_progress)
        {
            if(initializing_thread == std::this_thread::get_id()) return;
            lifecycle_condition.wait(lock, []() {
                return init_status.load(std::memory_order_acquire) != init_in_progress;
            });
            return;
        }
        init_status.store(init_in_progress, std::memory_order_release);
        initializing_thread = std::this_thread::get_id();
    }

    append_lifecycle("initialize");
    auto success = configure_tool_libraries();
    success      = initialize_clients() && success;
    success      = drain_deferred_api_tables() && success;
    complete_initialization(success);
}

void
finalize()
{
    int expected = 0;
    if(!fini_status.compare_exchange_strong(expected, -1)) return;

    {
        auto lock = std::unique_lock<std::mutex>{lifecycle_mutex};
        if(init_status.load(std::memory_order_acquire) == init_in_progress &&
           initializing_thread != std::this_thread::get_id())
        {
            lifecycle_condition.wait(lock, []() {
                return init_status.load(std::memory_order_acquire) != init_in_progress;
            });
        }
    }

    auto pending = std::vector<client_record*>{};
    {
        auto lock = std::lock_guard<std::mutex>{lifecycle_mutex};
        for(auto& client : clients)
        {
            if(client->initialized && !client->finalized)
            {
                client->finalized = true;
                pending.emplace_back(client.get());
            }
        }
    }

    auto finalization_lock = std::lock_guard<std::mutex>{finalization_mutex};
    for(auto* client : pending)
        ::rocprofiler::context::stop_client_contexts(client->internal_id);
    ::rocprofiler::hsa::queue_controller_sync();
    for(auto* client : pending)
    {
        invoke_client_finalizer(*client);
        ::rocprofiler::context::deactivate_client_contexts(client->internal_id);
    }
    if(init_status.load(std::memory_order_acquire) == init_failed)
        record_failure("sdk_initialization_failed",
                       "one or more configured profiler clients failed to initialize");

    ::rocprofiler::hsa::queue_controller_fini();
    ::rocprofiler::context::correlation_id_finalize();
    ::rocprofiler::internal_threading::finalize();
    fini_status.store(1, std::memory_order_release);
    append_lifecycle("finalize");
}

uint32_t
get_client_offset()
{
    return 1;
}

int
get_init_status()
{
    return init_status.load(std::memory_order_acquire);
}

int
get_fini_status()
{
    return fini_status.load(std::memory_order_acquire);
}

void
set_init_status(int value)
{
    init_status.store(value, std::memory_order_release);
}

void
set_fini_status(int value)
{
    fini_status.store(value, std::memory_order_release);
}

bool
is_attached()
{
    return false;
}

bool
supports_attachment()
{
    return false;
}

rocprofiler_status_t
attach()
{
    return ROCPROFILER_STATUS_ERROR_NOT_AVAILABLE;
}

rocprofiler_status_t
detach()
{
    return ROCPROFILER_STATUS_ERROR_NOT_AVAILABLE;
}
}  // namespace registration
}  // namespace rocprofiler

extern "C" {
rocprofiler_status_t
rocprofiler_is_initialized(int* status)
{
    if(!status) return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
    *status = rocprofiler::registration::get_init_status();
    return ROCPROFILER_STATUS_SUCCESS;
}

rocprofiler_status_t
rocprofiler_is_finalized(int* status)
{
    if(!status) return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
    *status = rocprofiler::registration::get_fini_status();
    return ROCPROFILER_STATUS_SUCCESS;
}

rocprofiler_status_t
rocprofiler_force_configure(rocprofiler_configure_func_t configure_func)
{
    {
        auto lock = std::lock_guard<std::mutex>{rocprofiler::registration::lifecycle_mutex};
        if(rocprofiler::registration::get_init_status() !=
           rocprofiler::registration::init_uninitialized)
            return ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED;
        rocprofiler::registration::set_init_status(
            rocprofiler::registration::init_in_progress);
        rocprofiler::registration::initializing_thread = std::this_thread::get_id();
    }

    rocprofiler::registration::set_register_library_to_self();
    rocprofiler::common::set_env("ROCPROFILER_REGISTER_FORCE_LOAD", "1", 1);

    auto success = true;
    if(configure_func)
    {
        success = rocprofiler::registration::configure_client(configure_func, 0) != nullptr;
        success = rocprofiler::registration::initialize_clients() && success;
    }
    success = rocprofiler::registration::drain_deferred_api_tables() && success;
    rocprofiler::registration::complete_initialization(success);
    if(!success) return ROCPROFILER_STATUS_ERROR;

    const auto status = rocprofiler_register_invoke_all_registrations();
    return (status == ROCP_REG_SUCCESS || status == ROCP_REG_NO_TOOLS)
               ? ROCPROFILER_STATUS_SUCCESS
               : ROCPROFILER_STATUS_ERROR;
}

rocprofiler_status_t
rocprofiler_iterate_runtime_registration_info(rocprofiler_runtime_registration_info_cb_t callback,
                                              void*                                      data)
{
    if(!callback) return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
    auto callback_data = std::pair<rocprofiler_runtime_registration_info_cb_t, void*>{callback,
                                                                                      data};
    const auto status = rocprofiler_register_iterate_registration_info(
        &rocprofiler::registration::registration_info_callback, &callback_data);
    return (status == ROCP_REG_SUCCESS) ? ROCPROFILER_STATUS_SUCCESS : ROCPROFILER_STATUS_ERROR;
}

int
rocprofiler_set_api_table(const char* name,
                          uint64_t    lib_version,
                          uint64_t    lib_instance,
                          void**      tables,
                          uint64_t    num_tables)
{
    if(!name || !tables || num_tables == 0 || !tables[0])
        return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
    if(rocprofiler::registration::get_fini_status() != 0) return 0;
    if(rocprofiler::registration::defer_reentrant_api_table(
           name, lib_version, lib_instance, tables, num_tables))
        return 0;

    rocprofiler::registration::initialize();
    if(rocprofiler::registration::get_fini_status() != 0) return 0;
    if(rocprofiler::registration::get_init_status() !=
       rocprofiler::registration::init_complete)
        return ROCPROFILER_STATUS_ERROR;
    return rocprofiler::registration::apply_api_table(
        name, lib_version, lib_instance, tables, num_tables);
}
}
