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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "rocprofv3_avail.hpp"

#include "lib/common/logging.hpp"
#include "lib/output/agent_info.hpp"
#include "lib/output/counter_info.hpp"

#include <rocprofiler-sdk/agent.h>
#include <rocprofiler-sdk/counters.h>
#include <rocprofiler-sdk/rocprofiler.h>
#include <rocprofiler-sdk/cxx/serialization.hpp>

#include <algorithm>
#include <exception>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace tool = ::rocprofiler::tool;
using JSONOutputArchive = cereal::MinimalJSONOutputArchive;

namespace
{
struct availability_cache
{
    std::map<rocprofiler_agent_id_t, tool::agent_info>         agents{};
    std::map<rocprofiler_agent_id_t, tool::counter_info_vec_t> counters{};
    std::map<rocprofiler_agent_id_t, std::string>              agent_json{};
    int                                                        error_code = 0;
    std::string                                                error_message{};

    availability_cache()
    {
        auto logging_cfg = rocprofiler::common::logging_config{.install_failure_handler = true};
        rocprofiler::common::init_logging("ROCPROF", logging_cfg);
        try
        {
            initialize();
        } catch(const std::exception& error)
        {
            set_error(ROCPROFILER_STATUS_ERROR, "availability initialization", error.what());
        } catch(...)
        {
            set_error(ROCPROFILER_STATUS_ERROR,
                      "availability initialization",
                      "unknown exception");
        }
        if(error_code != 0)
        {
            agents.clear();
            counters.clear();
            agent_json.clear();
        }
    }

    void set_error(rocprofiler_status_t status,
                   std::string_view     operation,
                   std::string_view     detail = {})
    {
        if(error_code != 0) return;
        error_code = (status == ROCPROFILER_STATUS_SUCCESS) ? -1 : static_cast<int>(status);
        error_message.assign(operation);
        if(!detail.empty())
        {
            error_message.append(": ");
            error_message.append(detail);
        }
        else if(const auto* value = rocprofiler_get_status_string(status))
        {
            error_message.append(": ");
            error_message.append(value);
        }
    }

    void initialize()
    {
        auto agents_vec = tool::agent_info_vec_t{};
        auto status     = rocprofiler_query_available_agents(
            ROCPROFILER_AGENT_INFO_VERSION_0,
            [](rocprofiler_agent_version_t, const void** values, size_t count, void* data) {
                auto* output = static_cast<tool::agent_info_vec_t*>(data);
                output->reserve(count);
                for(size_t i = 0; i < count; ++i)
                {
                    if(!values[i]) return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;
                    output->emplace_back(*static_cast<const rocprofiler_agent_v0_t*>(values[i]));
                }
                return ROCPROFILER_STATUS_SUCCESS;
            },
            sizeof(rocprofiler_agent_v0_t),
            &agents_vec);
        if(status != ROCPROFILER_STATUS_SUCCESS)
        {
            set_error(status, "rocprofiler_query_available_agents");
            return;
        }

        std::sort(agents_vec.begin(), agents_vec.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.node_id < rhs.node_id;
        });

        int64_t gpu_index = 0;
        for(auto& agent : agents_vec)
        {
            if(agent.type == ROCPROFILER_AGENT_TYPE_GPU) agent.gpu_index = gpu_index++;
            agents.emplace(agent.id, agent);
        }

        for(const auto& [agent_id, agent] : agents)
        {
            if(agent.type != ROCPROFILER_AGENT_TYPE_GPU) continue;

            status = rocprofiler_iterate_agent_supported_counters(
                agent_id,
                [](rocprofiler_agent_id_t    id,
                   rocprofiler_counter_id_t* counter_ids,
                   size_t                    count,
                   void*                     data) {
                    auto& cache  = *static_cast<availability_cache*>(data);
                    auto& values = cache.counters[id];
                    values.reserve(count);

                    for(size_t i = 0; i < count; ++i)
                    {
                        auto info     = rocprofiler_counter_info_v1_t{};
                        auto dim_ids  = tool::counter_dimension_id_vec_t{};
                        auto dim_info = tool::counter_dimension_info_vec_t{};
                        auto query_status = rocprofiler_query_counter_info(
                            counter_ids[i], ROCPROFILER_COUNTER_INFO_VERSION_1, &info);
                        if(query_status != ROCPROFILER_STATUS_SUCCESS)
                        {
                            cache.set_error(query_status, "rocprofiler_query_counter_info");
                            return query_status;
                        }
                        dim_ids.reserve(info.dimensions_count);
                        dim_info.reserve(info.dimensions_count);
                        for(uint64_t j = 0; j < info.dimensions_count; ++j)
                        {
                            if(!info.dimensions || !info.dimensions[j])
                            {
                                cache.set_error(ROCPROFILER_STATUS_ERROR,
                                                "rocprofiler_query_counter_info",
                                                "missing counter dimension metadata");
                                return ROCPROFILER_STATUS_ERROR;
                            }
                            dim_ids.emplace_back(info.dimensions[j]->id);
                            dim_info.emplace_back(*info.dimensions[j]);
                        }

                        values.emplace_back(
                            id, info, std::move(dim_ids), std::move(dim_info));
                    }
                    return ROCPROFILER_STATUS_SUCCESS;
                },
                this);
            if(status != ROCPROFILER_STATUS_SUCCESS)
            {
                set_error(status, "rocprofiler_iterate_agent_supported_counters");
                return;
            }
        }
    }

    const tool::tool_counter_info* find_counter(rocprofiler_counter_id_t id) const
    {
        for(const auto& [_, values] : counters)
        {
            auto itr = std::find_if(values.begin(), values.end(), [id](const auto& value) {
                return value.id.handle == id.handle;
            });
            if(itr != values.end()) return &*itr;
        }
        return nullptr;
    }
};

availability_cache&
get_cache()
{
    static auto value = availability_cache{};
    return value;
}
}  // namespace

ROCPROFILER_EXTERN_C_INIT

int
availability_status(const char** message)
{
    auto& cache = get_cache();
    if(message) *message = cache.error_message.c_str();
    return cache.error_code;
}

size_t
get_number_of_agents()
{
    return get_cache().agents.size();
}

void
agent_handles(uint64_t* handles, size_t count)
{
    if(!handles || count != get_cache().agents.size()) return;
    size_t index = 0;
    for(const auto& [id, _] : get_cache().agents)
        handles[index++] = id.handle;
}

uint64_t
get_agent_id(uint64_t agent_handle, int id_type)
{
    auto itr = get_cache().agents.find(rocprofiler_agent_id_t{agent_handle});
    if(itr == get_cache().agents.end()) return 0;
    if(id_type == 0) return itr->second.node_id;
    if(id_type == 1) return itr->second.logical_node_id;
    return itr->second.id.handle;
}

size_t
get_number_of_agent_counters(uint64_t agent_handle)
{
    auto itr = get_cache().counters.find(rocprofiler_agent_id_t{agent_handle});
    return (itr != get_cache().counters.end()) ? itr->second.size() : 0;
}

void
agent_counter_handles(uint64_t* handles, uint64_t agent_handle, size_t count)
{
    auto itr = get_cache().counters.find(rocprofiler_agent_id_t{agent_handle});
    if(!handles || itr == get_cache().counters.end() || count != itr->second.size()) return;
    std::transform(itr->second.begin(), itr->second.end(), handles, [](const auto& value) {
        return value.id.handle;
    });
}

void
counter_info(uint64_t     counter_handle,
             const char** counter_name,
             const char** counter_description,
             uint8_t*     is_derived,
             uint8_t*     is_hw_constant,
             uint8_t*     is_spm)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    if(!info) return;
    if(counter_name) *counter_name = info->name;
    if(counter_description) *counter_description = info->description;
    if(is_derived) *is_derived = info->is_derived;
    if(is_hw_constant) *is_hw_constant = info->is_constant;
    if(is_spm) *is_spm = info->spm_support;
}

void
counter_block(uint64_t counter_handle, const char** block)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    if(info && block) *block = info->block;
}

void
counter_expression(uint64_t counter_handle, const char** expression)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    if(info && expression) *expression = info->expression;
}

size_t
get_number_of_dimensions(uint64_t counter_handle)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    return (info) ? info->dimensions.size() : 0;
}

void
counter_dimension_ids(uint64_t counter_handle, uint64_t* ids, size_t count)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    if(!info || !ids || count != info->dimension_ids.size()) return;
    std::transform(info->dimension_ids.begin(), info->dimension_ids.end(), ids, [](auto id) {
        return static_cast<uint64_t>(id);
    });
}

void
counter_dimension(uint64_t     counter_handle,
                  uint64_t     dimension_handle,
                  const char** dimension_name,
                  uint64_t*    dimension_instance)
{
    const auto* info = get_cache().find_counter(rocprofiler_counter_id_t{counter_handle});
    if(!info) return;
    for(const auto& dimension : info->dimensions)
    {
        if(dimension.id != dimension_handle) continue;
        if(dimension_name) *dimension_name = dimension.name;
        if(dimension_instance) *dimension_instance = dimension.instance_size;
        return;
    }
}

size_t
get_number_of_pc_sample_configs(uint64_t)
{
    return 0;
}

size_t
get_number_of_spm_configs(uint64_t)
{
    return 0;
}

void
spm_sample_interval_config(uint64_t, uint64_t, uint64_t*, uint64_t*, uint64_t*)
{}

void
pc_sample_config(uint64_t, uint64_t, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*)
{}

bool
is_counter_set(const uint64_t* counter_handles, uint64_t agent_handle, size_t count)
{
    if(!counter_handles && count > 0) return false;

    auto config_id = rocprofiler_counter_config_id_t{.handle = 0};
    for(size_t i = 0; i < count; ++i)
    {
        auto counter_id = rocprofiler_counter_id_t{counter_handles[i]};
        if(rocprofiler_create_counter_config(
               rocprofiler_agent_id_t{agent_handle}, &counter_id, 1, &config_id) !=
           ROCPROFILER_STATUS_SUCCESS)
        {
            if(config_id.handle != 0) rocprofiler_destroy_counter_config(config_id);
            return false;
        }
    }
    if(config_id.handle != 0) rocprofiler_destroy_counter_config(config_id);
    return true;
}

void
agent_info(uint64_t agent_handle, const char** value)
{
    auto& cache = get_cache();
    auto  itr   = cache.agents.find(rocprofiler_agent_id_t{agent_handle});
    if(itr == cache.agents.end() || !value) return;

    auto json_itr = cache.agent_json.find(itr->first);
    if(json_itr == cache.agent_json.end())
    {
        auto stream = std::stringstream{};
        {
            constexpr auto json_prec = 16;
            constexpr auto json_indent = JSONOutputArchive::Options::IndentChar::space;
            auto options = JSONOutputArchive::Options{json_prec, json_indent, 0};
            auto archive = JSONOutputArchive{stream, options};
            cereal::save(archive, itr->second);
        }
        json_itr = cache.agent_json.emplace(itr->first, stream.str()).first;
    }
    *value = json_itr->second.c_str();
}

ROCPROFILER_EXTERN_C_FINI
