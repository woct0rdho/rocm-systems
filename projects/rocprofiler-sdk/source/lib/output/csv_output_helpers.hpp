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

#include "counter_output_columns.hpp"
#include "csv.hpp"
#include "statistics.hpp"

#include <rocprofiler-sdk/agent.h>

#include <sstream>
#include <string>
#include <string_view>

namespace rocprofiler
{
namespace tool
{
namespace csv
{
inline std::string_view
agent_type_name(rocprofiler_agent_type_t type)
{
    if(type == ROCPROFILER_AGENT_TYPE_CPU) return "CPU";
    if(type == ROCPROFILER_AGENT_TYPE_GPU) return "GPU";
    return "UNK";
}

inline std::string_view
nullable_string(const char* value)
{
    return (value) ? std::string_view{value} : std::string_view{};
}

template <typename AgentT>
std::string
format_agent_info_row(const AgentT& agent)
{
    auto output = std::ostringstream{};
    agent_info_csv_encoder::write_row(output,
                                      agent.node_id,
                                      agent.logical_node_id,
                                      agent_type_name(agent.type),
                                      agent.cpu_cores_count,
                                      agent.simd_count,
                                      agent.cpu_core_id_base,
                                      agent.simd_id_base,
                                      agent.max_waves_per_simd,
                                      agent.lds_size_in_kb,
                                      agent.gds_size_in_kb,
                                      agent.num_gws,
                                      agent.wave_front_size,
                                      agent.num_xcc,
                                      agent.cu_count,
                                      agent.array_count,
                                      agent.num_shader_banks,
                                      agent.simd_arrays_per_engine,
                                      agent.cu_per_simd_array,
                                      agent.simd_per_cu,
                                      agent.max_slots_scratch_cu,
                                      agent.gfx_target_version,
                                      agent.vendor_id,
                                      agent.device_id,
                                      agent.location_id,
                                      agent.domain,
                                      agent.drm_render_minor,
                                      agent.num_sdma_engines,
                                      agent.num_sdma_xgmi_engines,
                                      agent.num_sdma_queues_per_engine,
                                      agent.num_cp_queues,
                                      agent.max_engine_clk_ccompute,
                                      agent.max_engine_clk_fcompute,
                                      agent.sdma_fw_version.Value,
                                      agent.fw_version.Value,
                                      agent.capability.Value,
                                      agent.cu_per_engine,
                                      agent.max_waves_per_cu,
                                      agent.family_id,
                                      agent.workgroup_max_size,
                                      agent.grid_max_size,
                                      agent.local_mem_size,
                                      agent.hive_id,
                                      agent.gpu_id,
                                      agent.workgroup_max_dim.x,
                                      agent.workgroup_max_dim.y,
                                      agent.workgroup_max_dim.z,
                                      agent.grid_max_dim.x,
                                      agent.grid_max_dim.y,
                                      agent.grid_max_dim.z,
                                      nullable_string(agent.name),
                                      nullable_string(agent.vendor_name),
                                      nullable_string(agent.product_name),
                                      nullable_string(agent.model_name));
    return output.str();
}

inline stats_entry_t
make_statistics_entry(const stats_entry_vec_t& entries)
{
    auto output = stats_entry_t{};
    output.entries.reserve(entries.size());
    for(const auto& entry : entries)
    {
        output.entries.emplace_back(entry);
        output.total += entry.second;
    }
    return output.sort();
}

inline std::string
format_statistics_rows(const stats_entry_t& stats)
{
    auto output = std::ostringstream{};
    for(const auto& [name, value] : stats.entries)
        stats_csv_encoder::write_row<stats_formatter>(output,
                                                      name,
                                                      value.get_count(),
                                                      value.get_sum(),
                                                      value.get_mean(),
                                                      percentage{value.get_percent(stats.total)},
                                                      value.get_min(),
                                                      value.get_max(),
                                                      value.get_stddev());
    return output.str();
}
}  // namespace csv
}  // namespace tool
}  // namespace rocprofiler
