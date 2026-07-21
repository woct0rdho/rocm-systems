// MIT License
//
// Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All rights reserved.
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

#include "dimensions.hpp"

#include "lib/common/static_object.hpp"
#include "lib/common/synchronized.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/aql/helpers.hpp"
#include "lib/rocprofiler-sdk/counters/evaluate_ast.hpp"

#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/rocprofiler.h>
#include <rocprofiler-sdk/cxx/hash.hpp>
#include <rocprofiler-sdk/cxx/operators.hpp>

#include <fmt/core.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rocprofiler
{
namespace counters
{
std::vector<MetricDimension>
getBlockDimensions(rocprofiler_agent_id_t agent_id, const Metric& metric)
{
    if(!metric.constant().empty())
    {
        // Special non-hardware counters without dimension data
        return std::vector<MetricDimension>{{dimension_map().at(ROCPROFILER_DIMENSION_INSTANCE),
                                             1,
                                             ROCPROFILER_DIMENSION_INSTANCE}};
    }

    std::unordered_map<rocprofiler_profile_counter_instance_types, uint64_t> count;

    std::vector<MetricDimension> ret;

    // Dimension metadata describes the catalog topology; it is not a request to collect the
    // metric. Construct descriptors directly so availability queries do not validate every
    // event as a side effect. Actual counter configurations still use CounterPacketConstruct
    // and validate descriptors before packet construction.
    const auto query_info = aql::get_query_info(agent_id, metric);
    auto       event_id   = uint64_t{0};
    if(!metric.event().empty()) event_id = std::stoul(metric.event(), nullptr);

    for(uint32_t block_index = 0; block_index < query_info.instance_count; ++block_index)
    {
        const auto event = aqlprofile_pmc_event_t{
            .block_index = block_index,
            .event_id    = static_cast<uint32_t>(event_id & 0xFFFFFFFF),
            .flags       = aqlprofile_pmc_event_flags_t{metric.flags()},
            .block_name =
                static_cast<hsa_ven_amd_aqlprofile_block_name_t>(query_info.id)};
        auto dims   = std::map<int, uint64_t>{};
        auto status = aql::get_dim_info(agent_id, event, 0, dims);
        CHECK_EQ(status, ROCPROFILER_STATUS_SUCCESS) << rocprofiler_get_status_string(status);

        for(const auto& [id, extent] : dims)
        {
            if(const auto* inst_type =
                   rocprofiler::common::get_val(aqlprofile_id_to_rocprof_instance(), id))
            {
                count.emplace(*inst_type, 0).first->second = extent;
            }
            else
            {
                ROCP_WARNING << "Unknown AQL Profiler Dimension " << id << " " << extent;
            }
        }
    }

    // Keep the public metadata order stable across AQL Profile implementations. The Linux
    // rocprofv3 contract orders block instances before the surrounding GPU topology.
    constexpr auto dimension_order = std::array{
        ROCPROFILER_DIMENSION_INSTANCE,
        ROCPROFILER_DIMENSION_WGP,
        ROCPROFILER_DIMENSION_SHADER_ARRAY,
        ROCPROFILER_DIMENSION_SHADER_ENGINE,
        ROCPROFILER_DIMENSION_AID,
        ROCPROFILER_DIMENSION_XCC,
        ROCPROFILER_DIMENSION_AGENT};

    ret.reserve(count.size());
    for(const auto dim : dimension_order)
    {
        if(const auto* size = common::get_val(count, dim))
            ret.emplace_back(dimension_map().at(dim), *size, dim);
    }

    return ret;
}

namespace
{
metric_dims
generate_dimensions(rocprofiler_agent_id_t agent_id)
{
    std::unordered_map<uint64_t, std::vector<MetricDimension>> dims;

    // Get the agent to determine which architecture's metrics to load
    const auto* agent = rocprofiler::agent::get_agent(agent_id);
    if(!agent) return {.id_to_dim = dims};

    const auto  asts = counters::get_ast_map();
    const auto* arch_asts =
        rocprofiler::common::get_val(asts->arch_to_counter_asts, std::string(agent->name));
    if(!arch_asts) return {.id_to_dim = dims};

    for(const auto& [metric, ast] : *arch_asts)
    {
        auto ast_copy = ast;
        try
        {
            // Generate dimensions for this specific agent
            dims.emplace(ast.out_id().handle, ast_copy.set_dimensions(agent_id));
        } catch(std::runtime_error& e)
        {
            ROCP_FATAL << metric << " has improper dimensions"
                       << " " << e.what();
        }
    }
    return {.id_to_dim = dims};
}
}  // namespace

std::shared_ptr<const metric_dims>
get_dimension_cache(rocprofiler_agent_id_t agent_id, bool reload)
{
    using DimSync = common::Synchronized<
        std::unordered_map<rocprofiler_agent_id_t, std::shared_ptr<const metric_dims>>>;
    static DimSync*& dim_data = common::static_object<DimSync>::construct();

    if(!dim_data) return nullptr;

    // Check if we need to generate (first time or reload)
    auto needs_generation = dim_data->rlock([agent_id, reload](const auto& data) {
        return reload || !rocprofiler::common::get_val(data, agent_id);
    });

    if(needs_generation)
    {
        return dim_data->wlock([agent_id](auto& data) -> std::shared_ptr<const metric_dims> {
            auto new_dims  = std::make_shared<const metric_dims>(generate_dimensions(agent_id));
            data[agent_id] = new_dims;
            return new_dims;
        });
    }

    return dim_data->rlock([agent_id](const auto& data) -> std::shared_ptr<const metric_dims> {
        if(const auto* ptr = rocprofiler::common::get_val(data, agent_id))
        {
            return *ptr;
        }
        return nullptr;
    });
}

}  // namespace counters
}  // namespace rocprofiler
