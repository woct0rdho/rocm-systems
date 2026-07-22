// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include <hip/hip_runtime.h>
#include <rocprofiler-sdk/external_correlation.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/rocprofiler.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
rocprofiler_context_id_t      context_id      = {};
rocprofiler_buffer_id_t       buffer_id       = {};
rocprofiler_client_finalize_t client_finalize = nullptr;
rocprofiler_client_id_t*      client_id       = nullptr;
std::atomic<uint64_t>         record_count{0};
std::atomic<uint64_t>         value_record_count{0};
std::atomic<uint64_t>         sq_waves_value{0};
std::atomic<uint64_t>         timed_record_count{0};
std::atomic<bool>             invalid_timing{false};
std::mutex                    profile_mutex = {};
std::unordered_map<uint64_t, rocprofiler_counter_config_id_t> profile_cache = {};
std::vector<rocprofiler_counter_config_id_t> group_profiles = {};
std::array<std::atomic<uint64_t>, 2> group_record_counts = {};
std::array<std::atomic<uint64_t>, 2> group_value_counts = {};
std::atomic<size_t> active_group_pass{0};
std::atomic<bool>   group_validation_failed{false};
bool                group_setup_failed = false;

struct group_expectation
{
    std::unordered_set<uint64_t> counter_ids = {};
    std::unordered_set<uint64_t> instance_ids = {};
    std::unordered_map<uint64_t,
                       std::vector<std::pair<rocprofiler_counter_dimension_id_t, size_t>>>
        dimension_positions = {};
    size_t raw_counters     = 0;
    size_t derived_counters = 0;
};

std::array<group_expectation, 2> group_expectations = {};

struct dispatch_observation
{
    rocprofiler_dispatch_counting_service_data_t data      = {};
    rocprofiler_user_data_t                      user_data = {};
};

std::mutex                        observation_mutex   = {};
std::vector<dispatch_observation> enqueued_dispatches = {};
std::vector<dispatch_observation> completed_dispatches = {};

const std::string&
test_mode()
{
    static const auto value = []() {
        char*  mode = nullptr;
        size_t size = 0;
        if(_dupenv_s(&mode, &size, "ROCPROFILER_WINDOWS_DISPATCH_TEST_MODE") != 0)
            return std::string{"callback"};
        auto result = std::string{mode ? mode : "callback"};
        std::free(mode);
        return result;
    }();
    return value;
}

bool
use_buffered_service()
{
    return test_mode() == "buffered";
}

bool
use_concurrent_service()
{
    return test_mode() == "concurrent";
}

bool
use_grouped_service()
{
    return test_mode() == "grouped";
}

bool
check(rocprofiler_status_t status, const char* operation)
{
    if(status == ROCPROFILER_STATUS_SUCCESS) return true;
    std::fprintf(stderr,
                 "%s failed: %s\n",
                 operation,
                 rocprofiler_get_status_string(status));
    return false;
}

void
observe_timing(rocprofiler_timestamp_t start, rocprofiler_timestamp_t end)
{
    if(start == 0 || end <= start)
        invalid_timing.store(true);
    else
        timed_record_count.fetch_add(1);
}

int
external_correlation_callback(rocprofiler_thread_id_t,
                               rocprofiler_context_id_t,
                               rocprofiler_external_correlation_id_request_kind_t,
                               rocprofiler_tracing_operation_t,
                               uint64_t internal_corr_id,
                               rocprofiler_user_data_t* external_corr_id,
                               void*)
{
    external_corr_id->value = 0xe000000000000000ull | internal_corr_id;
    return 0;
}

void
dispatch_callback(rocprofiler_dispatch_counting_service_data_t data,
                  rocprofiler_counter_config_id_t* config,
                  rocprofiler_user_data_t* user_data,
                  void*)
{
    if(use_grouped_service())
    {
        auto lock = std::lock_guard<std::mutex>{profile_mutex};
        if(group_profiles.empty() && !group_setup_failed)
        {
            auto supported = std::vector<rocprofiler_counter_id_t>{};
            if(!check(rocprofiler_iterate_agent_supported_counters(
                          data.dispatch_info.agent_id,
                          [](rocprofiler_agent_id_t,
                             rocprofiler_counter_id_t* counters,
                             size_t count,
                             void* user_data) {
                              auto& output =
                                  *static_cast<std::vector<rocprofiler_counter_id_t>*>(user_data);
                              output.insert(output.end(), counters, counters + count);
                              return ROCPROFILER_STATUS_SUCCESS;
                          },
                          &supported),
                      "rocprofiler_iterate_agent_supported_counters"))
            {
                group_setup_failed = true;
            }
            else
            {
                const std::vector<std::vector<const char*>> group_names = {
                    {"GRBM_COUNT",
                     "GRBM_GUI_ACTIVE",
                     "SQ_WAVES",
                     "TA_TA_BUSY",
                     "TCP_REQ",
                     "GL1C_BUSY",
                     "GL2C_HIT",
                     "GDSInsts"},
                    {"GDSInsts"}};
                for(const auto& names : group_names)
                {
                    auto selected = std::vector<rocprofiler_counter_id_t>{};
                    for(const auto counter : supported)
                    {
                        auto info = rocprofiler_counter_info_v0_t{};
                        if(rocprofiler_query_counter_info(
                               counter, ROCPROFILER_COUNTER_INFO_VERSION_0, &info) !=
                               ROCPROFILER_STATUS_SUCCESS)
                            continue;
                        for(const auto name : names)
                        {
                            if(info.name && std::strcmp(info.name, name) == 0)
                                selected.emplace_back(counter);
                        }
                    }
                    const auto group = group_profiles.size();
                    auto&      expectation = group_expectations[group];
                    for(const auto counter : selected)
                    {
                        auto info = rocprofiler_counter_info_v1_t{};
                        if(!check(rocprofiler_query_counter_info(
                                      counter, ROCPROFILER_COUNTER_INFO_VERSION_1, &info),
                                  "rocprofiler_query_counter_info v1"))
                        {
                            group_setup_failed = true;
                            break;
                        }
                        expectation.counter_ids.emplace(counter.handle);
                        expectation.raw_counters += info.is_derived == 0 ? 1 : 0;
                        expectation.derived_counters += info.is_derived != 0 ? 1 : 0;
                        for(uint64_t instance_index = 0;
                            instance_index < info.dimensions_instances_count;
                            ++instance_index)
                        {
                            const auto* instance = info.dimensions_instances[instance_index];
                            if(!instance ||
                               !expectation.instance_ids.emplace(instance->instance_id).second)
                            {
                                group_setup_failed = true;
                                break;
                            }
                            auto positions = std::vector<
                                std::pair<rocprofiler_counter_dimension_id_t, size_t>>{};
                            for(uint64_t dimension_index = 0;
                                dimension_index < instance->dimensions_count;
                                ++dimension_index)
                            {
                                const auto* instance_dimension =
                                    instance->dimensions[dimension_index];
                                const rocprofiler_counter_record_dimension_info_t* dimension =
                                    nullptr;
                                for(uint64_t info_index = 0; info_index < info.dimensions_count;
                                    ++info_index)
                                {
                                    const auto* candidate = info.dimensions[info_index];
                                    if(candidate && instance_dimension && candidate->name &&
                                       instance_dimension->dimension_name &&
                                       std::strcmp(candidate->name,
                                                   instance_dimension->dimension_name) == 0)
                                    {
                                        dimension = candidate;
                                        break;
                                    }
                                }
                                if(!dimension)
                                {
                                    group_setup_failed = true;
                                    break;
                                }
                                positions.emplace_back(dimension->id, instance_dimension->index);
                            }
                            expectation.dimension_positions.emplace(instance->instance_id,
                                                                    std::move(positions));
                            if(group_setup_failed) break;
                        }
                        if(group_setup_failed) break;
                    }
                    const std::array<size_t, 2> expected_instances = {91, 1};
                    const std::array<size_t, 2> expected_raw = {7, 0};
                    const std::array<size_t, 2> expected_derived = {1, 1};
                    auto profile = rocprofiler_counter_config_id_t{};
                    if(group_setup_failed || selected.size() != names.size() ||
                       expectation.instance_ids.size() != expected_instances[group] ||
                       expectation.raw_counters != expected_raw[group] ||
                       expectation.derived_counters != expected_derived[group] ||
                       !check(rocprofiler_create_counter_config(data.dispatch_info.agent_id,
                                                                selected.data(),
                                                                selected.size(),
                                                                &profile),
                              "rocprofiler_create_counter_config"))
                    {
                        group_setup_failed = true;
                        break;
                    }
                    group_profiles.emplace_back(profile);
                }
            }
        }
        if(group_setup_failed || group_profiles.size() != 2) return;
        const auto group = active_group_pass.load();
        if(group >= group_profiles.size())
        {
            group_validation_failed.store(true);
            return;
        }
        user_data->value = group + 1;
        *config          = group_profiles[group];
        return;
    }

    if(use_concurrent_service())
    {
        user_data->value = data.dispatch_info.dispatch_id;
        auto lock = std::lock_guard<std::mutex>{observation_mutex};
        enqueued_dispatches.emplace_back(dispatch_observation{data, *user_data});
        if(data.dispatch_info.grid_size.x == 1)
        {
            *config = {};
            return;
        }
    }

    auto lock = std::lock_guard<std::mutex>{profile_mutex};
    if(auto pos = profile_cache.find(data.dispatch_info.agent_id.handle);
       pos != profile_cache.end())
    {
        *config = pos->second;
        return;
    }

    auto supported = std::vector<rocprofiler_counter_id_t>{};
    if(!check(rocprofiler_iterate_agent_supported_counters(
                  data.dispatch_info.agent_id,
                  [](rocprofiler_agent_id_t,
                     rocprofiler_counter_id_t* counters,
                     size_t count,
                     void* user_data) {
                      auto& output = *static_cast<std::vector<rocprofiler_counter_id_t>*>(user_data);
                      output.insert(output.end(), counters, counters + count);
                      return ROCPROFILER_STATUS_SUCCESS;
                  },
                  &supported),
              "rocprofiler_iterate_agent_supported_counters"))
        return;

    auto selected = std::vector<rocprofiler_counter_id_t>{};
    for(const auto counter : supported)
    {
        auto info = rocprofiler_counter_info_v0_t{};
        if(rocprofiler_query_counter_info(
               counter, ROCPROFILER_COUNTER_INFO_VERSION_0, &info) ==
               ROCPROFILER_STATUS_SUCCESS &&
           info.name && std::strcmp(info.name, "SQ_WAVES") == 0)
            selected.emplace_back(counter);
    }

    auto profile = rocprofiler_counter_config_id_t{};
    if(selected.size() != 1 ||
       !check(rocprofiler_create_counter_config(data.dispatch_info.agent_id,
                                                selected.data(),
                                                selected.size(),
                                                &profile),
              "rocprofiler_create_counter_config"))
        return;

    profile_cache.emplace(data.dispatch_info.agent_id.handle, profile);
    *config = profile;
}

void
record_callback(rocprofiler_dispatch_counting_service_data_t data,
                rocprofiler_counter_record_t* records,
                size_t count,
                rocprofiler_user_data_t user_data,
                void*)
{
    observe_timing(data.start_timestamp, data.end_timestamp);
    for(size_t index = 0; index < count; ++index)
        sq_waves_value.fetch_add(static_cast<uint64_t>(records[index].counter_value));
    if(use_grouped_service() && user_data.value >= 1 && user_data.value <= 2)
    {
        const auto group       = static_cast<size_t>(user_data.value - 1);
        const auto& expectation = group_expectations[group];
        auto observed_instances = std::unordered_set<uint64_t>{};
        auto valid = count == expectation.instance_ids.size();
        for(size_t index = 0; index < count; ++index)
        {
            rocprofiler_counter_id_t counter = {};
            valid &= rocprofiler_query_record_counter_id(records[index].id, &counter) ==
                     ROCPROFILER_STATUS_SUCCESS;
            valid &= expectation.counter_ids.count(counter.handle) == 1;
            valid &= expectation.instance_ids.count(records[index].id) == 1;
            valid &= observed_instances.emplace(records[index].id).second;
            valid &= records[index].dispatch_id == data.dispatch_info.dispatch_id;
            valid &= records[index].agent_id.handle == data.dispatch_info.agent_id.handle;
            valid &= records[index].user_data.value == user_data.value;
            if(const auto pos = expectation.dimension_positions.find(records[index].id);
               pos != expectation.dimension_positions.end())
            {
                for(const auto& [dimension, expected_position] : pos->second)
                {
                    auto position = size_t{};
                    valid &= rocprofiler_query_record_dimension_position(
                                 records[index].id, dimension, &position) ==
                             ROCPROFILER_STATUS_SUCCESS;
                    valid &= position == expected_position;
                }
            }
            else
            {
                valid = false;
            }
        }
        valid &= observed_instances == expectation.instance_ids;
        if(!valid) group_validation_failed.store(true);
        group_record_counts[group].fetch_add(1);
        group_value_counts[group].fetch_add(count);
    }
    if(use_concurrent_service())
    {
        auto lock = std::lock_guard<std::mutex>{observation_mutex};
        completed_dispatches.emplace_back(dispatch_observation{data, user_data});
    }
    value_record_count.fetch_add(count);
    record_count.fetch_add(1);
    std::printf("dispatch=%llu queue=%llu kernel=%llu correlation=%llu records=%zu "
                "SQ_WAVES=%.0f\n",
                static_cast<unsigned long long>(data.dispatch_info.dispatch_id),
                static_cast<unsigned long long>(data.dispatch_info.queue_id.handle),
                static_cast<unsigned long long>(data.dispatch_info.kernel_id),
                static_cast<unsigned long long>(data.correlation_id.internal),
                count,
                count > 0 ? records[0].counter_value : 0.0);
}

void
buffer_callback(rocprofiler_context_id_t,
                rocprofiler_buffer_id_t,
                rocprofiler_record_header_t** headers,
                size_t count,
                void*,
                uint64_t)
{
    for(size_t index = 0; index < count; ++index)
    {
        const auto* header = headers[index];
        if(!header || header->category != ROCPROFILER_BUFFER_CATEGORY_COUNTERS) continue;

        if(header->kind == ROCPROFILER_COUNTER_RECORD_PROFILE_COUNTING_DISPATCH_HEADER)
        {
            const auto* record = static_cast<rocprofiler_dispatch_counting_service_record_t*>(
                header->payload);
            if(!record) continue;
            observe_timing(record->start_timestamp, record->end_timestamp);
            record_count.fetch_add(1);
            std::printf("buffered_dispatch=%llu records=%llu\n",
                        static_cast<unsigned long long>(record->dispatch_info.dispatch_id),
                        static_cast<unsigned long long>(record->num_records));
        }
        else if(header->kind == ROCPROFILER_COUNTER_RECORD_VALUE)
        {
            const auto* record = static_cast<rocprofiler_counter_record_t*>(header->payload);
            if(!record) continue;
            sq_waves_value.fetch_add(static_cast<uint64_t>(record->counter_value));
            value_record_count.fetch_add(1);
        }
    }
}

int
tool_initialize(rocprofiler_client_finalize_t finalize, void*)
{
    client_finalize = finalize;
    if(!check(rocprofiler_create_context(&context_id), "rocprofiler_create_context")) return -1;
    const auto external_kind = ROCPROFILER_EXTERNAL_CORRELATION_REQUEST_KERNEL_DISPATCH;
    if(!check(rocprofiler_configure_external_correlation_id_request_service(
                  context_id, &external_kind, 1, external_correlation_callback, nullptr),
              "rocprofiler_configure_external_correlation_id_request_service"))
        return -1;
    if(use_buffered_service())
    {
        if(!check(rocprofiler_create_buffer(context_id,
                                            1024 * 1024,
                                            512 * 1024,
                                            ROCPROFILER_BUFFER_POLICY_LOSSLESS,
                                            buffer_callback,
                                            nullptr,
                                            &buffer_id),
                  "rocprofiler_create_buffer") ||
           !check(rocprofiler_configure_buffer_dispatch_counting_service(
                      context_id, buffer_id, dispatch_callback, nullptr),
                  "rocprofiler_configure_buffer_dispatch_counting_service"))
            return -1;
    }
    else if(!check(rocprofiler_configure_callback_dispatch_counting_service(context_id,
                                                                            dispatch_callback,
                                                                            nullptr,
                                                                            record_callback,
                                                                            nullptr),
                   "rocprofiler_configure_callback_dispatch_counting_service"))
    {
        return -1;
    }
    return check(rocprofiler_start_context(context_id), "rocprofiler_start_context") ? 0 : -1;
}

void
tool_finalize(void*)
{
    if(context_id.handle != 0) rocprofiler_stop_context(context_id);
}

rocprofiler_tool_configure_result_t*
tool_configure(uint32_t, const char*, uint32_t, rocprofiler_client_id_t* id)
{
    id->name  = "windows-dispatch-counting-service-test";
    client_id = id;
    static auto result = rocprofiler_tool_configure_result_t{
        .size = sizeof(rocprofiler_tool_configure_result_t),
        .initialize = tool_initialize,
        .finalize = tool_finalize,
        .tool_data = nullptr};
    return &result;
}

rocprofiler_status_t early_configuration_status = ROCPROFILER_STATUS_ERROR;

struct early_tool_configuration
{
    early_tool_configuration()
    : status{rocprofiler_force_configure(tool_configure)}
    {
        early_configuration_status = status;
    }
    rocprofiler_status_t status;
};
#if defined(__clang__)
__attribute__((init_priority(101)))
#endif
early_tool_configuration early_configuration = {};

__global__ void
vector_add(const float* lhs, const float* rhs, float* output, size_t count)
{
    const auto index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if(index < count) output[index] = lhs[index] + rhs[index];
}

__global__ void
delay_dispatch(uint64_t cycles)
{
    const auto start = clock64();
    while(clock64() - start < cycles)
    {}
}
}  // namespace

int
main()
{
    if(!check(early_configuration_status, "rocprofiler_force_configure")) return 1;

    constexpr auto count = size_t{1024} * 1024;
    float *lhs = nullptr, *rhs = nullptr, *output = nullptr;
    if(hipMalloc(&lhs, count * sizeof(float)) != hipSuccess ||
       hipMalloc(&rhs, count * sizeof(float)) != hipSuccess ||
       hipMalloc(&output, count * sizeof(float)) != hipSuccess)
        return 2;

    hipStream_t    stream_a            = nullptr;
    hipStream_t    stream_b            = nullptr;
    hipGraph_t     graph               = nullptr;
    hipGraphExec_t graph_exec          = nullptr;
    auto           reversed_completion = true;
    if(use_concurrent_service())
    {
        if(hipStreamCreateWithFlags(&stream_a, hipStreamNonBlocking) != hipSuccess ||
           hipStreamCreateWithFlags(&stream_b, hipStreamNonBlocking) != hipSuccess)
            return 2;

        delay_dispatch<<<1, 1, 0, stream_a>>>(1000000000);
        vector_add<<<count / 256, 256, 0, stream_b>>>(lhs, rhs, output, count);
        if(hipStreamSynchronize(stream_b) != hipSuccess) return 3;
        reversed_completion = hipStreamQuery(stream_a) == hipErrorNotReady;
        vector_add<<<count / 256, 256, 0, stream_a>>>(lhs, rhs, output, count);
        if(hipStreamSynchronize(stream_a) != hipSuccess) return 3;

        for(int index = 0; index < 100 && record_count.load() < 2; ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});

        if(hipStreamBeginCapture(stream_b, hipStreamCaptureModeGlobal) != hipSuccess) return 3;
        vector_add<<<count / 256, 256, 0, stream_b>>>(lhs, rhs, output, count);
        if(hipStreamEndCapture(stream_b, &graph) != hipSuccess ||
           hipGraphInstantiate(&graph_exec, graph, nullptr, nullptr, 0) != hipSuccess)
            return 3;
        if(hipGraphLaunch(graph_exec, stream_b) != hipSuccess ||
           hipGraphLaunch(graph_exec, stream_b) != hipSuccess ||
           hipStreamSynchronize(stream_b) != hipSuccess)
            return 3;

        for(int index = 0; index < 3; ++index)
        {
            hipStream_t temporary = nullptr;
            if(hipStreamCreateWithFlags(&temporary, hipStreamNonBlocking) != hipSuccess)
                return 3;
            vector_add<<<count / 256, 256, 0, temporary>>>(lhs, rhs, output, count);
            if(hipStreamSynchronize(temporary) != hipSuccess ||
               hipStreamDestroy(temporary) != hipSuccess)
                return 3;
        }

        if(hipGraphExecDestroy(graph_exec) != hipSuccess || hipGraphDestroy(graph) != hipSuccess ||
           hipStreamDestroy(stream_a) != hipSuccess || hipStreamDestroy(stream_b) != hipSuccess)
            return 3;
    }
    else if(use_grouped_service())
    {
        if(hipStreamCreateWithFlags(&stream_a, hipStreamNonBlocking) != hipSuccess ||
           hipStreamBeginCapture(stream_a, hipStreamCaptureModeGlobal) != hipSuccess)
            return 3;
        vector_add<<<count / 256, 256, 0, stream_a>>>(lhs, rhs, output, count);
        if(hipStreamEndCapture(stream_a, &graph) != hipSuccess ||
           hipGraphInstantiate(&graph_exec, graph, nullptr, nullptr, 0) != hipSuccess)
            return 3;
        for(size_t pass = 0; pass < group_expectations.size(); ++pass)
        {
            active_group_pass.store(pass);
            for(int replay = 0; replay < 4; ++replay)
            {
                if(hipGraphLaunch(graph_exec, stream_a) != hipSuccess) return 3;
            }
            if(hipStreamSynchronize(stream_a) != hipSuccess) return 3;
            for(int index = 0; index < 100 && group_record_counts[pass].load() < 4; ++index)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            if(group_record_counts[pass].load() != 4) return 3;
        }
        if(hipGraphExecDestroy(graph_exec) != hipSuccess || hipGraphDestroy(graph) != hipSuccess ||
           hipStreamDestroy(stream_a) != hipSuccess)
            return 3;
    }
    else
    {
        for(int index = 0; index < 4; ++index)
            vector_add<<<count / 256, 256>>>(lhs, rhs, output, count);
        if(hipDeviceSynchronize() != hipSuccess) return 3;
    }

    const auto expected_records = use_concurrent_service()
                                      ? uint64_t{7}
                                      : (use_grouped_service() ? uint64_t{8} : uint64_t{4});
    for(int index = 0; index < 100 && record_count.load() < expected_records; ++index)
    {
        if(buffer_id.handle != 0 &&
           !check(rocprofiler_flush_buffer(buffer_id), "rocprofiler_flush_buffer"))
            return 3;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    if(buffer_id.handle != 0 &&
       !check(rocprofiler_flush_buffer(buffer_id), "rocprofiler_flush_buffer"))
        return 3;
    if(!check(rocprofiler_stop_context(context_id), "rocprofiler_stop_context")) return 3;
    context_id = {};
    if(buffer_id.handle != 0)
    {
        if(!check(rocprofiler_destroy_buffer(buffer_id), "rocprofiler_destroy_buffer")) return 3;
        buffer_id = {};
    }
    {
        auto lock = std::lock_guard<std::mutex>{profile_mutex};
        for(const auto& [_, profile] : profile_cache)
        {
            if(!check(rocprofiler_destroy_counter_config(profile),
                      "rocprofiler_destroy_counter_config"))
                return 3;
        }
        for(const auto profile : group_profiles)
        {
            if(!check(rocprofiler_destroy_counter_config(profile),
                      "rocprofiler_destroy_counter_config"))
                return 3;
        }
        profile_cache.clear();
    }
    const auto output_free = hipFree(output);
    const auto rhs_free    = hipFree(rhs);
    const auto lhs_free    = hipFree(lhs);
    if(output_free != hipSuccess || rhs_free != hipSuccess || lhs_free != hipSuccess) return 3;
    if(client_finalize && client_id) client_finalize(*client_id);

    auto identity_passed    = true;
    auto unique_queue_count = size_t{0};
    const auto groups_passed =
        !use_grouped_service() ||
        (!group_setup_failed && !group_validation_failed.load() && group_profiles.size() == 2 &&
         group_record_counts[0].load() == 4 && group_record_counts[1].load() == 4 &&
         group_value_counts[0].load() == 364 && group_value_counts[1].load() == 4 &&
         group_expectations[0].raw_counters == 7 &&
         group_expectations[0].derived_counters == 1 &&
         group_expectations[1].raw_counters == 0 &&
         group_expectations[1].derived_counters == 1);
    if(use_concurrent_service())
    {
        auto lock = std::lock_guard<std::mutex>{observation_mutex};
        identity_passed = enqueued_dispatches.size() == 8 && completed_dispatches.size() == 7;
        if(identity_passed)
        {
            auto enqueue_by_dispatch = std::unordered_map<uint64_t, dispatch_observation>{};
            auto dispatch_ids        = std::unordered_set<uint64_t>{};
            auto completed_ids       = std::unordered_set<uint64_t>{};
            auto queue_ids           = std::unordered_set<uint64_t>{};
            for(const auto& dispatch : enqueued_dispatches)
            {
                const auto dispatch_id = dispatch.data.dispatch_info.dispatch_id;
                dispatch_ids.emplace(dispatch_id);
                queue_ids.emplace(dispatch.data.dispatch_info.queue_id.handle);
                enqueue_by_dispatch.emplace(dispatch_id, dispatch);
                identity_passed &= dispatch_id != 0;
                identity_passed &= dispatch.data.start_timestamp == 0;
                identity_passed &= dispatch.data.end_timestamp == 0;
                identity_passed &= dispatch.data.dispatch_info.kernel_id != 0;
                identity_passed &= dispatch.data.correlation_id.internal != 0;
                identity_passed &= dispatch.data.correlation_id.external.value ==
                                   (0xe000000000000000ull | dispatch.data.correlation_id.internal);
                identity_passed &= dispatch.user_data.value == dispatch_id;
            }
            unique_queue_count = queue_ids.size();
            identity_passed &= dispatch_ids.size() == enqueued_dispatches.size();
            identity_passed &= unique_queue_count >= 2;
            identity_passed &= enqueued_dispatches[0].data.dispatch_info.queue_id.handle ==
                               enqueued_dispatches[2].data.dispatch_info.queue_id.handle;
            identity_passed &= enqueued_dispatches[0].data.dispatch_info.queue_id.handle !=
                               enqueued_dispatches[1].data.dispatch_info.queue_id.handle;

            for(const auto& dispatch : completed_dispatches)
            {
                const auto dispatch_id = dispatch.data.dispatch_info.dispatch_id;
                completed_ids.emplace(dispatch_id);
                const auto pos         = enqueue_by_dispatch.find(dispatch_id);
                identity_passed &= pos != enqueue_by_dispatch.end();
                if(pos == enqueue_by_dispatch.end()) continue;
                identity_passed &= dispatch.user_data.value == dispatch_id;
                identity_passed &= dispatch.data.start_timestamp > 0;
                identity_passed &= dispatch.data.end_timestamp > dispatch.data.start_timestamp;
                identity_passed &= dispatch.data.dispatch_info.queue_id.handle ==
                                   pos->second.data.dispatch_info.queue_id.handle;
                identity_passed &= dispatch.data.dispatch_info.kernel_id ==
                                   pos->second.data.dispatch_info.kernel_id;
                identity_passed &= dispatch.data.correlation_id.internal ==
                                   pos->second.data.correlation_id.internal;
                identity_passed &= dispatch.data.correlation_id.external.value ==
                                   pos->second.data.correlation_id.external.value;
            }

            identity_passed &= completed_ids.size() == completed_dispatches.size();
        }
        else
        {
            reversed_completion = false;
        }
    }

    const auto records       = record_count.load();
    const auto value_records = value_record_count.load();
    const auto value         = sq_waves_value.load();
    const auto expected_value_records = use_grouped_service() ? uint64_t{368}
                                                               : expected_records * 20;
    const auto timing_records = timed_record_count.load();
    const auto passed = records == expected_records && value_records == expected_value_records &&
                        value > 0 && timing_records == expected_records &&
                        !invalid_timing.load() && identity_passed && reversed_completion &&
                        groups_passed;
    std::printf("windows_dispatch_counting_service=%s records=%llu SQ_WAVES_total=%llu "
                "value_records=%llu timed_records=%llu mode=%s identities=%s reversed=%s "
                "queues=%zu groups=%zu "
                "group_records=%llu,%llu group_values=%llu,%llu group_counters=%zu/%zu,%zu/%zu "
                "replays=%u\n",
                passed ? "passed" : "failed",
                static_cast<unsigned long long>(records),
                static_cast<unsigned long long>(value),
                static_cast<unsigned long long>(value_records),
                static_cast<unsigned long long>(timing_records),
                test_mode().c_str(),
                identity_passed ? "passed" : "failed",
                reversed_completion ? "passed" : "failed",
                unique_queue_count,
                group_profiles.size(),
                static_cast<unsigned long long>(group_record_counts[0].load()),
                static_cast<unsigned long long>(group_record_counts[1].load()),
                static_cast<unsigned long long>(group_value_counts[0].load()),
                static_cast<unsigned long long>(group_value_counts[1].load()),
                group_expectations[0].raw_counters,
                group_expectations[0].derived_counters,
                group_expectations[1].raw_counters,
                group_expectations[1].derived_counters,
                use_grouped_service() ? 2u : 0u);
    return passed ? 0 : 4;
}
