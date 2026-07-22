// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "counter_config_common.hpp"
#include "kernel_selector.hpp"
#include "lib/rocprofiler-sdk/hsa/windows_tool.hpp"

#include "lib/common/demangle.hpp"
#include "lib/common/windows_result.hpp"
#include "lib/output/counter_output_columns.hpp"
#include "lib/output/csv.hpp"
#include "lib/output/statistics.hpp"
#include "lib/output/stream_info.hpp"

#include <amd_comgr/amd_comgr.h>
#include <rocprofiler-sdk/cxx/serialization.hpp>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>

#include <cereal/archives/json.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

std::string
get_env(const char* name, std::string default_value = {})
{
    char*  value = nullptr;
    size_t size  = 0;
    if(_dupenv_s(&value, &size, name) != 0 || !value) return default_value;
    auto result = std::string{value};
    std::free(value);
    return result;
}

bool
get_env_bool(const char* name, bool default_value = false)
{
    auto value = get_env(name);
    if(value.empty()) return default_value;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "on" || value == "yes" || value == "true";
}

std::vector<std::string>
parse_counters()
{
    auto groups = rocprofiler::tool::common_config::parse_counter_groups(
        get_env("ROCPROF_COUNTERS"), get_env("ROCPROF_COUNTER_GROUPS"));
    if(groups.empty()) return {};
    return {groups.front().begin(), groups.front().end()};
}

struct output_config
{
    std::string output_path = get_env("ROCPROF_OUTPUT_PATH", fs::current_path().string());
    std::string output_file = get_env("ROCPROF_OUTPUT_FILE_NAME", "%pid%");
    std::string formats     = get_env("ROCPROF_OUTPUT_FORMAT", "csv");
    std::string include_expression =
        get_env("ROCPROF_KERNEL_FILTER_INCLUDE_REGEX", ".*");
    std::string exclude_expression = get_env("ROCPROF_KERNEL_FILTER_EXCLUDE_REGEX");
    bool demangle          = get_env_bool("ROCPROF_DEMANGLE_KERNELS", true);
    bool truncate          = get_env_bool("ROCPROF_TRUNCATE_KERNELS", false);
    bool kernel_trace      = get_env_bool("ROCPROF_KERNEL_TRACE", false);
    bool stats             = get_env_bool("ROCPROF_STATS", false);
    bool selected_regions  = get_env_bool("ROCPROF_SELECTED_REGIONS", false);
    bool selected_regions_ref_count =
        get_env_bool("ROCPROF_SELECTED_REGIONS_REF_COUNT", false);
    std::vector<std::string> counters = parse_counters();
    std::string iteration_expression = get_env("ROCPROF_KERNEL_FILTER_RANGE");

    bool has_format(std::string_view name) const
    {
        const auto ascii_space = [](char value) {
            return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
                   value == '\f' || value == '\v';
        };
        const auto ascii_lower = [](char value) {
            return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a')
                                                  : value;
        };

        size_t begin = 0;
        while(begin < formats.size())
        {
            const auto end = formats.find(',', begin);
            auto       token = std::string_view{formats}.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            while(!token.empty() && ascii_space(token.front())) token.remove_prefix(1);
            while(!token.empty() && ascii_space(token.back())) token.remove_suffix(1);
            if(token.size() == name.size() &&
               std::equal(token.begin(), token.end(), name.begin(),
                          [&](char lhs, char rhs) {
                              return ascii_lower(lhs) == ascii_lower(rhs);
                          }))
                return true;
            if(end == std::string::npos) break;
            begin = end + 1;
        }
        return false;
    }
};

struct counter_value
{
    rocprofiler_counter_id_t counter_id = {};
    double                   value      = 0.0;

    template <typename ArchiveT>
    void save(ArchiveT& archive) const
    {
        archive(cereal::make_nvp("counter_id", counter_id), cereal::make_nvp("value", value));
    }
};

struct stream_id
{
    uint64_t handle = 0;

    template <typename ArchiveT>
    void save(ArchiveT& archive) const
    {
        archive(cereal::make_nvp("handle", handle));
    }
};

struct counter_record
{
    rocprofiler_thread_id_t                        thread_id     = 0;
    rocprofiler_dispatch_counting_service_data_t   dispatch_data = {};
    std::vector<counter_value>                     records       = {};
    stream_id                                      stream        = {};

    template <typename ArchiveT>
    void save(ArchiveT& archive) const
    {
        archive(cereal::make_nvp("thread_id", thread_id),
                cereal::make_nvp("dispatch_data", dispatch_data),
                cereal::make_nvp("records", records),
                cereal::make_nvp("stream_id", stream));
    }
};

using kernel_dispatch_record =
    rocprofiler::tool::tool_buffer_tracing_kernel_dispatch_ext_record_t;

struct counter_metadata
{
    rocprofiler_agent_id_t   agent_id   = {};
    rocprofiler_counter_id_t id         = {};
    uint8_t                  is_constant = 0;
    uint8_t                  is_derived  = 0;
    std::string              name        = {};
    std::string              description = {};
    std::string              block       = {};
    std::string              expression  = {};

    template <typename ArchiveT>
    void save(ArchiveT& archive) const
    {
        archive(cereal::make_nvp("agent_id", agent_id),
                cereal::make_nvp("id", id),
                cereal::make_nvp("is_constant", is_constant),
                cereal::make_nvp("is_derived", is_derived),
                cereal::make_nvp("name", name),
                cereal::make_nvp("description", description),
                cereal::make_nvp("block", block),
                cereal::make_nvp("expression", expression));
    }
};

struct kernel_metadata
{
    uint64_t    size                = 0;
    uint64_t    kernel_id           = 0;
    uint64_t    code_object_id      = 0;
    std::string kernel_name         = {};
    uint64_t    kernel_object       = 0;
    uint64_t    kernarg_segment_size = 0;
    uint64_t    kernarg_segment_alignment = 0;
    uint32_t    group_segment_size  = 0;
    uint32_t    private_segment_size = 0;
    uint32_t    sgpr_count          = 0;
    uint32_t    arch_vgpr_count     = 0;
    uint32_t    accum_vgpr_count    = 0;
    int64_t     kernel_code_entry_byte_offset = 0;
    uint64_t    kernel_address      = 0;
    std::string formatted_kernel_name = {};
    std::string demangled_kernel_name = {};
    std::string truncated_kernel_name = {};

    template <typename ArchiveT>
    void save(ArchiveT& archive) const
    {
        archive(cereal::make_nvp("size", size),
                cereal::make_nvp("kernel_id", kernel_id),
                cereal::make_nvp("code_object_id", code_object_id),
                cereal::make_nvp("kernel_name", kernel_name),
                cereal::make_nvp("kernel_object", kernel_object),
                cereal::make_nvp("kernarg_segment_size", kernarg_segment_size),
                cereal::make_nvp("kernarg_segment_alignment", kernarg_segment_alignment),
                cereal::make_nvp("group_segment_size", group_segment_size),
                cereal::make_nvp("private_segment_size", private_segment_size),
                cereal::make_nvp("sgpr_count", sgpr_count),
                cereal::make_nvp("arch_vgpr_count", arch_vgpr_count),
                cereal::make_nvp("accum_vgpr_count", accum_vgpr_count),
                cereal::make_nvp("kernel_code_entry_byte_offset", kernel_code_entry_byte_offset),
                cereal::make_nvp("kernel_address", stream_id{kernel_address}),
                cereal::make_nvp("formatted_kernel_name", formatted_kernel_name),
                cereal::make_nvp("demangled_kernel_name", demangled_kernel_name),
                cereal::make_nvp("truncated_kernel_name", truncated_kernel_name));
    }
};

struct tool_state
{
    output_config config = {};
    rocprofiler_context_id_t context = {};
    rocprofiler_context_id_t control_context = {};
    rocprofiler_timestamp_t init_time = 0;
    rocprofiler_timestamp_t fini_time = 0;
    std::mutex mutex = {};
    std::mutex control_mutex = {};
    std::mutex result_mutex = {};
    std::unordered_map<uint64_t, rocprofiler_counter_config_id_t> profiles = {};
    std::unordered_map<uint64_t, std::string> counter_names = {};
    std::vector<counter_metadata> counter_info = {};
    std::unordered_map<uint64_t, kernel_metadata> kernel_info = {};
    std::vector<counter_record> records = {};
    size_t selected_dispatches = 0;
    std::vector<rocprofiler_agent_t> agents = {};
    rocprofiler::tool::kernel_selector selector;
    bool selected_active = false;
    int64_t selected_ref_count = 0;
    std::atomic<bool> unknown_counter{false};
    std::atomic<bool> failed{false};
    std::string failure_status = "profiler_failure";
    std::string failure_detail = {};

    tool_state()
    : selector{config.include_expression,
               config.exclude_expression,
               config.iteration_expression}
    {}
};

tool_state*&
get_state()
{
    static auto* value = static_cast<tool_state*>(nullptr);
    return value;
}

bool
check(rocprofiler_status_t status, const char* operation, tool_state* state)
{
    if(status == ROCPROFILER_STATUS_SUCCESS) return true;
    const auto* status_message = rocprofiler_get_status_string(status);
    auto detail = std::string{operation} + " failed: " +
                  (status_message ? status_message : "unknown SDK error");
    std::fprintf(stderr, "%s\n", detail.c_str());
    if(state)
    {
        state->failed.store(true, std::memory_order_release);
        auto lock = std::lock_guard<std::mutex>{state->result_mutex};
        if(state->failure_detail.empty()) state->failure_detail = detail;
    }
    rocprofiler::windows::result::write("profiler_failure", detail);
    return false;
}

kernel_metadata
make_kernel_metadata(const output_config&,
                     rocprofiler_dispatch_counting_service_data_t,
                     const rocprofiler::hsa::windows::kernel_metadata&);

void
set_failure(tool_state& state, std::string status, std::string detail)
{
    state.failed.store(true, std::memory_order_release);
    auto lock = std::lock_guard<std::mutex>{state.result_mutex};
    if(state.failure_detail.empty())
    {
        state.failure_status = std::move(status);
        state.failure_detail = std::move(detail);
    }
}

rocprofiler_counter_config_id_t
get_profile(tool_state& state, rocprofiler_agent_id_t agent_id)
{
    auto lock = std::lock_guard<std::mutex>{state.mutex};
    if(auto itr = state.profiles.find(agent_id.handle); itr != state.profiles.end())
        return itr->second;

    auto supported = std::vector<rocprofiler_counter_id_t>{};
    if(!check(rocprofiler_iterate_agent_supported_counters(
                  agent_id,
                  [](rocprofiler_agent_id_t,
                     rocprofiler_counter_id_t* counters,
                     size_t count,
                     void* data) {
                      auto& output = *static_cast<std::vector<rocprofiler_counter_id_t>*>(data);
                      output.insert(output.end(), counters, counters + count);
                      return ROCPROFILER_STATUS_SUCCESS;
                  },
                  &supported),
              "rocprofiler_iterate_agent_supported_counters",
              &state))
        return {};

    auto available = std::unordered_map<std::string, rocprofiler_counter_id_t>{};
    for(const auto id : supported)
    {
        auto info   = rocprofiler_counter_info_v0_t{};
        auto status = rocprofiler_query_counter_info(
            id, ROCPROFILER_COUNTER_INFO_VERSION_0, &info);
        if(!check(status, "rocprofiler_query_counter_info", &state) || !info.name)
        {
            if(!info.name)
                check(ROCPROFILER_STATUS_ERROR_COUNTER_NOT_FOUND,
                      "rocprofiler_query_counter_info returned an unnamed counter",
                      &state);
            return {};
        }
        available.emplace(info.name, id);
    }

    auto selected = std::vector<rocprofiler_counter_id_t>{};
    for(const auto& name : state.config.counters)
    {
        auto itr = available.find(name);
        if(itr == available.end())
        {
            std::fprintf(stderr,
                         "Unable to find counter for agent %llu: %s\n",
                         static_cast<unsigned long long>(agent_id.handle),
                         name.c_str());
            state.unknown_counter.store(true, std::memory_order_release);
            continue;
        }
        selected.emplace_back(itr->second);

        auto info   = rocprofiler_counter_info_v0_t{};
        auto status = rocprofiler_query_counter_info(
            itr->second, ROCPROFILER_COUNTER_INFO_VERSION_0, &info);
        if(!check(status, "rocprofiler_query_counter_info", &state)) return {};
        state.counter_names.emplace(itr->second.handle, info.name ? info.name : name);
        state.counter_info.emplace_back(counter_metadata{agent_id,
                                                         itr->second,
                                                         info.is_constant,
                                                         info.is_derived,
                                                         info.name ? info.name : "",
                                                         info.description ? info.description : "",
                                                         info.block ? info.block : "",
                                                         info.expression ? info.expression : ""});
    }

    auto profile = rocprofiler_counter_config_id_t{};
    if(!selected.empty() &&
       !check(rocprofiler_create_counter_config(
                  agent_id, selected.data(), selected.size(), &profile),
              "rocprofiler_create_counter_config",
              &state))
        profile = {};
    state.profiles.emplace(agent_id.handle, profile);
    return profile;
}

void
dispatch_callback(rocprofiler_dispatch_counting_service_data_t data,
                  rocprofiler_counter_config_id_t* config,
                  rocprofiler_user_data_t* user_data,
                  void* callback_data)
{
    auto& state = *static_cast<tool_state*>(callback_data);
    if(state.failed.load(std::memory_order_acquire))
    {
        *config = {};
        return;
    }

    const auto kernel_id = data.dispatch_info.kernel_id;
    const auto resource  = rocprofiler::hsa::windows::get_kernel_metadata(kernel_id);
    if(!resource || !resource->valid)
    {
        auto detail = std::string{"mandatory resource metadata is unavailable for kernel "} +
                      std::to_string(kernel_id);
        if(resource && !resource->error.empty()) detail += ": " + resource->error;
        set_failure(state, "kernel_metadata_missing", std::move(detail));
        *config = {};
        return;
    }
    const auto metadata = make_kernel_metadata(state.config, data, *resource);

    {
        auto lock = std::lock_guard<std::mutex>{state.mutex};
        state.kernel_info[kernel_id] = metadata;

        const auto selected =
            state.selector.select(data.dispatch_info.dispatch_id, metadata.formatted_kernel_name);
        state.selector.erase(data.dispatch_info.dispatch_id);
        if(!selected) return;
    }

    *config = get_profile(state, data.dispatch_info.agent_id);
    if(config->handle == 0) return;
    {
        auto lock = std::lock_guard<std::mutex>{state.mutex};
        ++state.selected_dispatches;
    }
    user_data->value = ::GetCurrentThreadId();
}

void
record_callback(rocprofiler_dispatch_counting_service_data_t data,
                rocprofiler_counter_record_t* records,
                size_t count,
                rocprofiler_user_data_t user_data,
                void* callback_data)
{
    auto& state  = *static_cast<tool_state*>(callback_data);
    auto  output = counter_record{};
    output.thread_id     = user_data.value;
    output.dispatch_data = data;
    output.records.reserve(count);
    for(size_t index = 0; index < count; ++index)
    {
        auto counter_id = rocprofiler_counter_id_t{};
        if(check(rocprofiler_query_record_counter_id(records[index].id, &counter_id),
                 "rocprofiler_query_record_counter_id",
                 &state))
            output.records.emplace_back(counter_value{counter_id, records[index].counter_value});
    }
    if(output.records.empty()) return;

    auto lock = std::lock_guard<std::mutex>{state.mutex};
    if(auto metadata = state.kernel_info.find(data.dispatch_info.kernel_id);
       metadata != state.kernel_info.end())
    {
        output.dispatch_data.dispatch_info.group_segment_size =
            metadata->second.group_segment_size;
        output.dispatch_data.dispatch_info.private_segment_size =
            metadata->second.private_segment_size;
    }
    state.records.emplace_back(std::move(output));
}

void
marker_control_callback(rocprofiler_callback_tracing_record_t record,
                        rocprofiler_user_data_t*,
                        void* callback_data)
{
    if(record.kind != ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API) return;

    auto& state = *static_cast<tool_state*>(callback_data);
    auto lock = std::lock_guard<std::mutex>{state.control_mutex};
    if(record.phase == ROCPROFILER_CALLBACK_PHASE_EXIT &&
       record.operation == ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerResume)
    {
        if(state.config.selected_regions_ref_count)
        {
            if(state.selected_ref_count++ == 0 && !state.selected_active)
            {
                state.selected_active = true;
                check(rocprofiler_start_context(state.context),
                      "selected-region context start",
                      &state);
            }
        }
        else if(!state.selected_active)
        {
            state.selected_active = true;
            check(rocprofiler_start_context(state.context),
                  "selected-region context start",
                  &state);
        }
    }
    else if(record.phase == ROCPROFILER_CALLBACK_PHASE_ENTER &&
            record.operation == ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerPause)
    {
        if(state.config.selected_regions_ref_count)
        {
            if(state.selected_ref_count > 0 && --state.selected_ref_count == 0 &&
               state.selected_active)
            {
                state.selected_active = false;
                check(rocprofiler_stop_context(state.context),
                      "selected-region context stop",
                      &state);
            }
        }
        else if(state.selected_active)
        {
            state.selected_active = false;
            check(rocprofiler_stop_context(state.context),
                  "selected-region context stop",
                  &state);
        }
    }
}

std::string
replace_all(std::string value, std::string_view key, std::string_view replacement)
{
    for(auto position = value.find(key); position != std::string::npos;
        position = value.find(key, position + replacement.size()))
        value.replace(position, key.size(), replacement);
    return value;
}

std::string
format_output_value(std::string value)
{
    return replace_all(
        std::move(value), "%pid%", std::to_string(::GetCurrentProcessId()));
}

fs::path
output_path(const tool_state& state, std::string_view suffix, std::string_view extension)
{
    auto directory = fs::path{format_output_value(state.config.output_path)};
    auto prefix    = format_output_value(state.config.output_file);
    return directory / (prefix + "_" + std::string{suffix} + std::string{extension});
}

bool
write_exclusive(const fs::path& path, std::string_view content)
{
    std::error_code error = {};
    fs::create_directories(path.parent_path(), error);
    if(error)
    {
        std::fprintf(stderr,
                     "rocprofv3 could not create output directory: %ls (%s)\n",
                     path.parent_path().c_str(),
                     error.message().c_str());
        return false;
    }
    const auto file = ::CreateFileW(path.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if(file == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr,
                     "rocprofv3 output already exists or cannot be created: %ls (error %lu)\n",
                     path.c_str(),
                     static_cast<unsigned long>(::GetLastError()));
        return false;
    }

    size_t offset = 0;
    while(offset < content.size())
    {
        const auto remaining = std::min<size_t>(content.size() - offset, 0x7ffff000u);
        auto       written   = DWORD{0};
        if(!::WriteFile(file,
                        content.data() + offset,
                        static_cast<DWORD>(remaining),
                        &written,
                        nullptr) ||
           written == 0)
        {
            const auto write_error = ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileW(path.c_str());
            std::fprintf(stderr,
                         "rocprofv3 could not write output: %ls (error %lu)\n",
                         path.c_str(),
                         static_cast<unsigned long>(write_error));
            return false;
        }
        offset += written;
    }
    const auto flushed = (::FlushFileBuffers(file) != FALSE);
    const auto closed  = (::CloseHandle(file) != FALSE);
    if(!flushed || !closed)
    {
        const auto write_error = ::GetLastError();
        ::DeleteFileW(path.c_str());
        std::fprintf(stderr,
                     "rocprofv3 could not publish output: %ls (error %lu)\n",
                     path.c_str(),
                     static_cast<unsigned long>(write_error));
        return false;
    }
    return true;
}

std::string
normalize_kernel_name(std::string name)
{
    constexpr auto kernel_descriptor_suffix = std::string_view{".kd"};
    if(name.size() >= kernel_descriptor_suffix.size() &&
       name.compare(name.size() - kernel_descriptor_suffix.size(),
                    kernel_descriptor_suffix.size(),
                    kernel_descriptor_suffix) == 0)
        name.resize(name.size() - kernel_descriptor_suffix.size());
    return name;
}

std::string
demangle_kernel_name(std::string_view name)
{
    auto mangled = amd_comgr_data_t{};
    auto output  = amd_comgr_data_t{};
    if(amd_comgr_create_data(AMD_COMGR_DATA_KIND_BYTES, &mangled) !=
       AMD_COMGR_STATUS_SUCCESS)
        return std::string{name};

    auto result = std::string{name};
    if(amd_comgr_set_data(mangled, name.size(), name.data()) == AMD_COMGR_STATUS_SUCCESS &&
       amd_comgr_demangle_symbol_name(mangled, &output) == AMD_COMGR_STATUS_SUCCESS)
    {
        size_t size = 0;
        if(amd_comgr_get_data(output, &size, nullptr) == AMD_COMGR_STATUS_SUCCESS && size > 0)
        {
            auto demangled = std::string(size, '\0');
            if(amd_comgr_get_data(output, &size, demangled.data()) == AMD_COMGR_STATUS_SUCCESS)
            {
                demangled.resize(size);
                if(!demangled.empty() && demangled.back() == '\0') demangled.pop_back();
                result = std::move(demangled);
            }
        }
        amd_comgr_release_data(output);
    }
    amd_comgr_release_data(mangled);
    return result;
}

kernel_metadata
make_kernel_metadata(const output_config&                            config,
                     rocprofiler_dispatch_counting_service_data_t    data,
                     const rocprofiler::hsa::windows::kernel_metadata& resource)
{
    auto output                          = kernel_metadata{};
    output.size                          = sizeof(kernel_metadata);
    output.kernel_id                     = data.dispatch_info.kernel_id;
    output.kernel_name                   = normalize_kernel_name(resource.name);
    output.kernel_object                 = resource.kernel_object;
    output.kernarg_segment_size          = resource.kernarg_segment_size;
    output.kernarg_segment_alignment     = resource.kernarg_segment_alignment;
    output.group_segment_size            = resource.group_segment_size;
    output.private_segment_size          = resource.private_segment_size;
    output.sgpr_count                    = resource.sgpr_count;
    output.arch_vgpr_count               = resource.arch_vgpr_count;
    output.accum_vgpr_count              = resource.accum_vgpr_count;
    output.kernel_code_entry_byte_offset = resource.kernel_code_entry_offset;
    output.kernel_address                = resource.kernel_address;
    output.demangled_kernel_name         = demangle_kernel_name(output.kernel_name);
    output.truncated_kernel_name =
        rocprofiler::common::truncate_name(output.demangled_kernel_name);
    output.formatted_kernel_name =
        config.demangle ? output.demangled_kernel_name : output.kernel_name;
    if(config.truncate) output.formatted_kernel_name = output.truncated_kernel_name;
    return output;
}

uint64_t
magnitude(rocprofiler_dim3_t dimensions)
{
    return dimensions.x * dimensions.y * dimensions.z;
}

std::string
agent_label(const tool_state& state, rocprofiler_agent_id_t id)
{
    for(const auto& agent : state.agents)
        if(agent.id.handle == id.handle)
            return "Agent " + std::to_string(agent.logical_node_id);
    return "Agent " + std::to_string(id.handle);
}

std::vector<counter_record>
sorted_counter_records(const tool_state& state)
{
    auto records = state.records;
    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.dispatch_data.dispatch_info.dispatch_id <
               rhs.dispatch_data.dispatch_info.dispatch_id;
    });
    return records;
}

std::vector<kernel_dispatch_record>
make_kernel_dispatch_records(const std::vector<counter_record>& records)
{
    using base_record_t = rocprofiler_buffer_tracing_kernel_dispatch_record_t;

    auto output = std::vector<kernel_dispatch_record>{};
    output.reserve(records.size());
    for(const auto& record : records)
    {
        const auto& dispatch = record.dispatch_data;
        auto base            = base_record_t{};
        base.size            = sizeof(base_record_t);
        base.kind            = ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH;
        base.operation       = ROCPROFILER_KERNEL_DISPATCH_COMPLETE;
        base.correlation_id  = dispatch.correlation_id;
        base.thread_id       = record.thread_id;
        base.start_timestamp = dispatch.start_timestamp;
        base.end_timestamp   = dispatch.end_timestamp;
        base.dispatch_info   = dispatch.dispatch_info;

        auto stream = rocprofiler_stream_id_t{};
        stream.handle = record.stream.handle;
        output.emplace_back(base,
                            stream,
                            rocprofiler_graph_exec_id_t{},
                            rocprofiler_graph_node_id_t{});
    }
    return output;
}

size_t
unique_dispatch_count(const tool_state& state)
{
    auto dispatches = std::unordered_set<rocprofiler_dispatch_id_t>{};
    dispatches.reserve(state.records.size());
    for(const auto& record : state.records)
        dispatches.emplace(record.dispatch_data.dispatch_info.dispatch_id);
    return dispatches.size();
}

uint64_t
find_invalid_kernel_timing_dispatch(const tool_state& state)
{
    for(const auto& record : state.records)
    {
        const auto& dispatch = record.dispatch_data;
        if(dispatch.start_timestamp == 0 ||
           dispatch.end_timestamp <= dispatch.start_timestamp)
            return dispatch.dispatch_info.dispatch_id;
    }
    return 0;
}

uint64_t
find_missing_kernel_metadata_dispatch(const tool_state& state)
{
    for(const auto& record : state.records)
    {
        const auto& info = record.dispatch_data.dispatch_info;
        if(state.kernel_info.count(info.kernel_id) == 0) return info.dispatch_id;
    }
    return 0;
}

rocprofiler::tool::stats_entry_t
generate_kernel_statistics(const tool_state&                    state,
                           const std::vector<counter_record>& records)
{
    auto kernel_stats = rocprofiler::tool::stats_map_t{};
    for(const auto& record : records)
    {
        const auto& dispatch = record.dispatch_data;
        const auto  metadata = state.kernel_info.find(dispatch.dispatch_info.kernel_id);
        if(metadata == state.kernel_info.end()) continue;
        kernel_stats[metadata->second.formatted_kernel_name] +=
            dispatch.end_timestamp - dispatch.start_timestamp;
    }

    auto output = rocprofiler::tool::stats_entry_t{};
    for(const auto& [name, value] : kernel_stats)
    {
        output.entries.emplace_back(name, value);
        output.total += value;
    }
    return output.sort();
}

std::string
generate_kernel_stats_csv(const rocprofiler::tool::stats_entry_t& stats)
{
    constexpr auto columns = std::array<std::string_view, 8>{"Name",
                                                              "Calls",
                                                              "TotalDurationNs",
                                                              "AverageNs",
                                                              "Percentage",
                                                              "MinNs",
                                                              "MaxNs",
                                                              "StdDev"};
    auto output = std::ostringstream{};
    for(size_t index = 0; index < std::size(columns); ++index)
    {
        if(index > 0) output << ',';
        output << rocprofiler::tool::csv::quote(columns[index]);
    }
    output << '\n';

    for(const auto& [name, value] : stats.entries)
        rocprofiler::tool::csv::stats_csv_encoder::write_row<
            rocprofiler::tool::stats_formatter>(output,
                                                name,
                                                value.get_count(),
                                                value.get_sum(),
                                                value.get_mean(),
                                                rocprofiler::tool::percentage{
                                                    value.get_percent(stats.total)},
                                                value.get_min(),
                                                value.get_max(),
                                                value.get_stddev());
    return output.str();
}

std::string
generate_agent_csv(const tool_state& state)
{
    auto agents = state.agents;
    std::sort(agents.begin(), agents.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.node_id < rhs.node_id;
    });

    auto output = std::ostringstream{};
    for(size_t index = 0; index < rocprofiler::tool::csv::agent_info_columns.size(); ++index)
    {
        if(index > 0) output << ',';
        output << rocprofiler::tool::csv::quote(
            rocprofiler::tool::csv::agent_info_columns[index]);
    }
    output << '\n';

    for(const auto& agent : agents)
    {
        auto type = std::string_view{"UNK"};
        if(agent.type == ROCPROFILER_AGENT_TYPE_CPU)
            type = "CPU";
        else if(agent.type == ROCPROFILER_AGENT_TYPE_GPU)
            type = "GPU";
        output << agent.node_id << ',' << agent.logical_node_id << ','
               << rocprofiler::tool::csv::quote(type) << ',' << agent.cpu_cores_count << ','
               << agent.simd_count << ','
               << agent.cpu_core_id_base << ',' << agent.simd_id_base << ','
               << agent.max_waves_per_simd << ',' << agent.lds_size_in_kb << ','
               << agent.gds_size_in_kb << ',' << agent.num_gws << ','
               << agent.wave_front_size << ',' << agent.num_xcc << ',' << agent.cu_count << ','
               << agent.array_count << ',' << agent.num_shader_banks << ','
               << agent.simd_arrays_per_engine << ',' << agent.cu_per_simd_array << ','
               << agent.simd_per_cu << ',' << agent.max_slots_scratch_cu << ','
               << agent.gfx_target_version << ',' << agent.vendor_id << ',' << agent.device_id
               << ',' << agent.location_id << ',' << agent.domain << ','
               << agent.drm_render_minor << ',' << agent.num_sdma_engines << ','
               << agent.num_sdma_xgmi_engines << ',' << agent.num_sdma_queues_per_engine << ','
               << agent.num_cp_queues << ',' << agent.max_engine_clk_ccompute << ','
               << agent.max_engine_clk_fcompute << ',' << agent.sdma_fw_version.Value << ','
               << agent.fw_version.Value << ',' << agent.capability.Value << ','
               << agent.cu_per_engine << ',' << agent.max_waves_per_cu << ','
               << agent.family_id << ',' << agent.workgroup_max_size << ','
               << agent.grid_max_size << ',' << agent.local_mem_size << ',' << agent.hive_id << ','
               << agent.gpu_id << ',' << agent.workgroup_max_dim.x << ','
               << agent.workgroup_max_dim.y << ',' << agent.workgroup_max_dim.z << ','
               << agent.grid_max_dim.x << ',' << agent.grid_max_dim.y << ','
               << agent.grid_max_dim.z << ','
               << rocprofiler::tool::csv::quote(agent.name ? agent.name : "") << ','
               << rocprofiler::tool::csv::quote(agent.vendor_name ? agent.vendor_name : "")
               << ','
               << rocprofiler::tool::csv::quote(agent.product_name ? agent.product_name : "")
               << ','
               << rocprofiler::tool::csv::quote(agent.model_name ? agent.model_name : "") << '\n';
    }
    return output.str();
}

uint64_t
find_invalid_sq_waves_dispatch(const tool_state& state)
{
    for(const auto& record : state.records)
    {
        auto total = 0.0;
        auto found = false;
        for(const auto& value : record.records)
        {
            const auto name = state.counter_names.find(value.counter_id.handle);
            if(name != state.counter_names.end() && name->second == "SQ_WAVES")
            {
                total += value.value;
                found = true;
            }
        }
        if(found && total <= 0.0)
            return record.dispatch_data.dispatch_info.dispatch_id;
    }
    return 0;
}

uint32_t
lds_block_size(const kernel_metadata& metadata)
{
    constexpr auto block_size = uint64_t{512};
    return static_cast<uint32_t>((metadata.group_segment_size + block_size - 1) &
                                 ~(block_size - 1));
}

std::string
generate_counter_csv(tool_state& state)
{
    const auto records = sorted_counter_records(state);
    auto       output  = std::ostringstream{};
    for(size_t index = 0;
        index < rocprofiler::tool::csv::counter_collection_columns.size();
        ++index)
    {
        if(index > 0) output << ',';
        output << rocprofiler::tool::csv::quote(
            rocprofiler::tool::csv::counter_collection_columns[index]);
    }
    output << '\n' << std::fixed << std::setprecision(6);

    for(const auto& record : records)
    {
        auto values = std::map<uint64_t, double>{};
        for(const auto& value : record.records)
            values[value.counter_id.handle] += value.value;

        const auto& dispatch = record.dispatch_data;
        const auto& info     = dispatch.dispatch_info;
        auto kernel_name = rocprofiler::hsa::windows::get_kernel_name(info.kernel_id);
        auto metadata    = kernel_metadata{};
        if(auto itr = state.kernel_info.find(info.kernel_id); itr != state.kernel_info.end())
        {
            metadata    = itr->second;
            kernel_name = metadata.formatted_kernel_name;
        }
        for(const auto& [counter_id, value] : values)
        {
            auto counter_name = std::string{"counter_"} + std::to_string(counter_id);
            if(auto itr = state.counter_names.find(counter_id); itr != state.counter_names.end())
                counter_name = itr->second;
            output << dispatch.correlation_id.internal << ',' << info.dispatch_id << ','
                   << rocprofiler::tool::csv::quote(agent_label(state, info.agent_id)) << ','
                   << info.queue_id.handle << ',' << ::GetCurrentProcessId() << ','
                   << record.thread_id << ',' << magnitude(info.grid_size) << ','
                   << info.kernel_id << ',' << rocprofiler::tool::csv::quote(kernel_name) << ','
                   << magnitude(info.workgroup_size) << ',' << lds_block_size(metadata) << ','
                   << metadata.private_segment_size << ',' << metadata.arch_vgpr_count << ','
                   << metadata.accum_vgpr_count << ',' << metadata.sgpr_count << ','
                   << rocprofiler::tool::csv::quote(counter_name) << ',' << value << ','
                   << dispatch.start_timestamp << ',' << dispatch.end_timestamp << '\n';
        }
    }
    return output.str();
}

std::string
generate_kernel_csv(tool_state& state)
{
    constexpr auto columns = std::array<std::string_view, 22>{"Kind",
                                                               "Agent_Id",
                                                               "Queue_Id",
                                                               "Stream_Id",
                                                               "Thread_Id",
                                                               "Dispatch_Id",
                                                               "Kernel_Id",
                                                               "Kernel_Name",
                                                               "Correlation_Id",
                                                               "Start_Timestamp",
                                                               "End_Timestamp",
                                                               "LDS_Block_Size",
                                                               "Scratch_Size",
                                                               "VGPR_Count",
                                                               "Accum_VGPR_Count",
                                                               "SGPR_Count",
                                                               "Workgroup_Size_X",
                                                               "Workgroup_Size_Y",
                                                               "Workgroup_Size_Z",
                                                               "Grid_Size_X",
                                                               "Grid_Size_Y",
                                                               "Grid_Size_Z"};
    const auto records = sorted_counter_records(state);
    auto       output  = std::ostringstream{};
    for(size_t index = 0; index < std::size(columns); ++index)
    {
        if(index > 0) output << ',';
        output << rocprofiler::tool::csv::quote(columns[index]);
    }
    output << '\n';

    for(const auto& record : records)
    {
        const auto& dispatch = record.dispatch_data;
        const auto& info     = dispatch.dispatch_info;
        auto kernel_name = rocprofiler::hsa::windows::get_kernel_name(info.kernel_id);
        auto metadata    = kernel_metadata{};
        if(auto itr = state.kernel_info.find(info.kernel_id); itr != state.kernel_info.end())
        {
            metadata    = itr->second;
            kernel_name = metadata.formatted_kernel_name;
        }

        output << "KERNEL_DISPATCH," << rocprofiler::tool::csv::quote(
                      agent_label(state, info.agent_id))
               << ',' << info.queue_id.handle << ',' << record.stream.handle << ','
               << record.thread_id << ',' << info.dispatch_id << ',' << info.kernel_id << ','
               << rocprofiler::tool::csv::quote(kernel_name) << ','
               << dispatch.correlation_id.internal << ',' << dispatch.start_timestamp << ','
               << dispatch.end_timestamp << ',' << lds_block_size(metadata) << ','
               << metadata.private_segment_size << ',' << metadata.arch_vgpr_count << ','
               << metadata.accum_vgpr_count << ',' << metadata.sgpr_count << ','
               << info.workgroup_size.x << ',' << info.workgroup_size.y << ','
               << info.workgroup_size.z << ',' << info.grid_size.x << ',' << info.grid_size.y
               << ',' << info.grid_size.z << '\n';
    }
    return output.str();
}

template <typename ArchiveT>
void
serialize_statistics(ArchiveT& archive, const rocprofiler::tool::stats_data_t& stats)
{
    const auto count    = stats.get_count();
    const auto sum      = stats.get_sum();
    const auto sqr      = stats.get_sqr();
    const auto min      = stats.get_min();
    const auto max      = stats.get_max();
    const auto mean     = stats.get_mean();
    const auto stddev   = stats.get_stddev();
    const auto variance = stats.get_variance();
    archive(cereal::make_nvp("count", count),
            cereal::make_nvp("sum", sum),
            cereal::make_nvp("sqr", sqr),
            cereal::make_nvp("min", min),
            cereal::make_nvp("max", max),
            cereal::make_nvp("mean", mean),
            cereal::make_nvp("stddev", stddev),
            cereal::make_nvp("variance", variance));
}

template <typename ArchiveT>
void
serialize_statistics_entry(ArchiveT& archive,
                           const rocprofiler::tool::stats_entry_t& stats)
{
    const auto class_version = uint32_t{0};
    archive(cereal::make_nvp("cereal_class_version", class_version));
    serialize_statistics(archive, stats.total);

    auto operations = std::vector<const rocprofiler::tool::stats_pair_t*>{};
    operations.reserve(stats.entries.size());
    for(const auto& entry : stats.entries)
        operations.emplace_back(&entry);
    std::sort(operations.begin(), operations.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->first < rhs->first;
    });

    archive.setNextName("operations");
    archive.startNode();
    archive.makeArray();
    auto first = true;
    for(const auto* entry : operations)
    {
        archive.startNode();
        const auto key = std::string{entry->first};
        archive(cereal::make_nvp("key", key));
        archive.setNextName("value");
        archive.startNode();
        if(first)
        {
            archive(cereal::make_nvp("cereal_class_version", class_version));
            first = false;
        }
        serialize_statistics(archive, entry->second);
        archive.finishNode();
        archive.finishNode();
    }
    archive.finishNode();
}

std::string
generate_json(tool_state& state, const rocprofiler::tool::stats_entry_t& kernel_stats)
{
    const auto records        = sorted_counter_records(state);
    const auto kernel_records = state.config.kernel_trace
                                    ? make_kernel_dispatch_records(records)
                                    : std::vector<kernel_dispatch_record>{};
    auto stream = std::ostringstream{};
    {
        constexpr auto precision = 16;
        auto archive = cereal::PrettyJSONOutputArchive{
            stream,
            cereal::PrettyJSONOutputArchive::Options{precision,
                                                     cereal::PrettyJSONOutputArchive::Options::IndentChar::space,
                                                     0}};
        archive.setNextName("rocprofiler-sdk-tool");
        archive.startNode();
        archive.makeArray();
        archive.startNode();

        archive.setNextName("metadata");
        archive.startNode();
        archive(cereal::make_nvp("pid", static_cast<uint64_t>(::GetCurrentProcessId())),
                cereal::make_nvp("init_time", state.init_time),
                cereal::make_nvp("fini_time", state.fini_time));
        archive.finishNode();
        archive(cereal::make_nvp("agents", state.agents),
                cereal::make_nvp("counters", state.counter_info));

        archive.setNextName("strings");
        archive.startNode();
        archive.finishNode();
        archive(cereal::make_nvp("code_objects", std::vector<uint64_t>{}));

        auto kernels = std::vector<kernel_metadata>{};
        kernels.reserve(state.kernel_info.size());
        for(const auto& [_, value] : state.kernel_info)
            kernels.emplace_back(value);
        std::sort(kernels.begin(), kernels.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.kernel_id < rhs.kernel_id;
        });
        archive(cereal::make_nvp("kernel_symbols", kernels),
                cereal::make_nvp("host_functions", std::vector<uint64_t>{}));

        archive.setNextName("summary");
        archive.startNode();
        archive.makeArray();
        if(state.config.stats && kernel_stats)
        {
            archive.startNode();
            archive(cereal::make_nvp("domain", std::string{"KERNEL_DISPATCH"}));
            archive.setNextName("stats");
            archive.startNode();
            serialize_statistics_entry(archive, kernel_stats);
            archive.finishNode();
            archive.finishNode();
        }
        archive.finishNode();
        archive.setNextName("callback_records");
        archive.startNode();
        archive(cereal::make_nvp("counter_collection", records),
                cereal::make_nvp("spm_counter_collection", std::vector<uint64_t>{}));
        archive.finishNode();
        archive.setNextName("buffer_records");
        archive.startNode();
        archive(cereal::make_nvp("kernel_dispatch", kernel_records));
        archive.finishNode();

        archive.finishNode();
        archive.finishNode();
    }
    return stream.str();
}

int
tool_initialize(rocprofiler_client_finalize_t, void* data)
{
    auto& state = *static_cast<tool_state*>(data);
    rocprofiler::windows::result::write("initializing");
    if(!check(rocprofiler_get_timestamp(&state.init_time),
              "rocprofiler_get_timestamp",
              &state))
        return -1;
    if(!check(rocprofiler_query_available_agents(
        ROCPROFILER_AGENT_INFO_VERSION_0,
        [](rocprofiler_agent_version_t, const void** agents, size_t count, void* output) {
            auto& result = *static_cast<std::vector<rocprofiler_agent_t>*>(output);
            result.reserve(count);
            for(size_t index = 0; index < count; ++index)
                if(agents[index])
                    result.emplace_back(*static_cast<const rocprofiler_agent_t*>(agents[index]));
            return ROCPROFILER_STATUS_SUCCESS;
        },
        sizeof(rocprofiler_agent_t),
        &state.agents),
              "rocprofiler_query_available_agents",
              &state))
        return -1;

    if(!check(rocprofiler_create_context(&state.context),
              "rocprofiler_create_context",
              &state) ||
       !check(rocprofiler_configure_callback_dispatch_counting_service(state.context,
                                                                       dispatch_callback,
                                                                       &state,
                                                                       record_callback,
                                                                       &state),
              "rocprofiler_configure_callback_dispatch_counting_service",
              &state))
        return -1;

    if(state.config.selected_regions || state.config.selected_regions_ref_count)
    {
        if(!check(rocprofiler_create_context(&state.control_context),
                  "selected-region control context creation",
                  &state) ||
           !check(rocprofiler_configure_callback_tracing_service(
                      state.control_context,
                      ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API,
                      nullptr,
                      0,
                      marker_control_callback,
                      &state),
                  "selected-region marker control configuration",
                  &state) ||
           !check(rocprofiler_start_context(state.control_context),
                  "selected-region control context start",
                  &state))
            return -1;
        return 0;
    }
    return check(rocprofiler_start_context(state.context),
                 "rocprofiler_start_context",
                 &state)
               ? 0
               : -1;
}

void
tool_finalize(void* data)
{
    auto& state = *static_cast<tool_state*>(data);
    check(rocprofiler_get_timestamp(&state.fini_time), "rocprofiler_get_timestamp", &state);

    auto published = std::vector<fs::path>{};
    auto status    = std::string{"success_no_dispatch"};
    auto detail    = std::string{};
    {
        auto lock = std::lock_guard<std::mutex>{state.mutex};
        // Counter configurations are process-lifetime SDK resources. On Windows the SDK
        // controller's static teardown can precede this CRT client finalizer, so explicitly
        // destroying profiles here can call through an owner whose teardown has started.
        // Do not duplicate that destruction; process teardown reclaims any remaining resources.
        if(state.failed.load(std::memory_order_acquire))
        {
            auto failure_lock = std::lock_guard<std::mutex>{state.result_mutex};
            status = state.failure_status;
            detail = state.failure_detail.empty() ? "profiler operation failed"
                                                  : state.failure_detail;
        }
        else if(state.unknown_counter.load(std::memory_order_acquire))
        {
            status = "success_unknown_counter";
        }
        else if(state.records.size() != state.selected_dispatches)
        {
            status = "counter_record_mismatch";
            detail = "selected dispatches=" + std::to_string(state.selected_dispatches) +
                     ", completed counter records=" + std::to_string(state.records.size());
        }
        else if((state.config.kernel_trace || state.config.stats) &&
                unique_dispatch_count(state) != state.records.size())
        {
            status = "kernel_record_mismatch";
            detail = "completed counter records=" + std::to_string(state.records.size()) +
                     ", unique kernel dispatches=" +
                     std::to_string(unique_dispatch_count(state));
        }
        else if(state.config.stats &&
                find_invalid_kernel_timing_dispatch(state) != 0)
        {
            const auto dispatch_id = find_invalid_kernel_timing_dispatch(state);
            status = "kernel_timing_invalid";
            detail = "kernel statistics require a positive ordered interval for dispatch " +
                     std::to_string(dispatch_id);
        }
        else if(state.config.stats &&
                find_missing_kernel_metadata_dispatch(state) != 0)
        {
            const auto dispatch_id = find_missing_kernel_metadata_dispatch(state);
            status = "kernel_metadata_missing";
            detail = "kernel statistics require formatted metadata for dispatch " +
                     std::to_string(dispatch_id);
        }
        else if(const auto dispatch_id = find_invalid_sq_waves_dispatch(state);
                dispatch_id != 0)
        {
            status = "counter_sample_invalid";
            detail = "SQ_WAVES was not positive for dispatch " +
                     std::to_string(dispatch_id) +
                     "; another WDDM performance-counter session may have reset the "
                     "GPU-wide counters";
        }
        else if(!state.records.empty())
        {
            const auto records = sorted_counter_records(state);
            const auto kernel_stats = state.config.stats
                                          ? generate_kernel_statistics(state, records)
                                          : rocprofiler::tool::stats_entry_t{};
            auto publish = [&](const fs::path& path, std::string content) {
                if(!write_exclusive(path, content)) return false;
                published.emplace_back(path);
                return true;
            };

            auto success            = true;
            auto publication_detail = std::string{};
            try
            {
                if(state.config.has_format("csv"))
                {
                    success = publish(output_path(state, "agent_info", ".csv"),
                                      generate_agent_csv(state)) &&
                              success;
                    success = publish(output_path(state, "counter_collection", ".csv"),
                                      generate_counter_csv(state)) &&
                              success;
                    if(state.config.kernel_trace)
                        success = publish(output_path(state, "kernel_trace", ".csv"),
                                          generate_kernel_csv(state)) &&
                                  success;
                    if(state.config.stats)
                        success = publish(output_path(state, "kernel_stats", ".csv"),
                                          generate_kernel_stats_csv(kernel_stats)) &&
                                  success;
                }
                if(state.config.has_format("json"))
                    success = publish(output_path(state, "results", ".json"),
                                      generate_json(state, kernel_stats)) &&
                              success;
            } catch(const std::exception& error)
            {
                success            = false;
                publication_detail = error.what();
            } catch(...)
            {
                success            = false;
                publication_detail = "unknown output-generation exception";
            }

            if(success)
            {
                status = "success_records";
                detail = "selected dispatches=" +
                         std::to_string(state.selected_dispatches) +
                         ", completed counter records=" +
                         std::to_string(state.records.size()) +
                         ", emitted kernel records=" +
                         std::to_string(state.config.kernel_trace ? state.records.size() : 0) +
                         ", emitted kernel statistics=" +
                         std::to_string(state.config.stats ? kernel_stats.entries.size() : 0);
            }
            else
            {
                for(const auto& path : published)
                    ::DeleteFileW(path.c_str());
                status = "output_publication_failed";
                detail = publication_detail.empty()
                             ? "one or more reserved profiler outputs could not be published"
                             : "output generation failed: " + publication_detail;
                state.failed.store(true, std::memory_order_release);
            }
        }
    }

    if(!rocprofiler::windows::result::write(status, detail))
        std::fprintf(stderr, "rocprofv3 could not publish its private result status\n");
    if(get_state() == &state) get_state() = nullptr;
    delete &state;
}
}  // namespace

extern "C" rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t,
                      const char*,
                      uint32_t,
                      rocprofiler_client_id_t* client_id)
{
    if(!client_id || !get_env_bool("ROCPROF_COUNTER_COLLECTION", false)) return nullptr;

    static auto name = std::string{"rocprofiler-sdk-tool"};
    client_id->name  = name.c_str();
    try
    {
        get_state() = new tool_state{};
    } catch(const std::exception& error)
    {
        std::fprintf(stderr, "Windows rocprofv3 tool configuration failed: %s\n", error.what());
        rocprofiler::windows::result::write("tool_configure_failed", error.what());
        return nullptr;
    }

    rocprofiler::windows::result::write("configured");
    static auto result = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t), tool_initialize, tool_finalize, nullptr};
    result.tool_data = get_state();
    return &result;
}
