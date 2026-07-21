// MIT License
//
// Copyright (c) 2023-2026 Advanced Micro Devices, Inc. All rights reserved.
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
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "lib/rocprofiler-sdk/platform/windows/agent.hpp"

#include "lib/common/logging.hpp"
#include "lib/common/string_entry.hpp"
#include "lib/common/utility.hpp"

#include <rocprofiler-sdk/agent.h>

#include <fmt/format.h>

// d3dkmthk.h depends on declarations omitted by WIN32_LEAN_AND_MEAN.
#ifdef WIN32_LEAN_AND_MEAN
#    undef WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <d3dkmthk.h>

#ifdef max
#    undef max
#endif
#ifdef min
#    undef min
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rocprofiler
{
namespace agent
{
// Keep the platform implementation independent of the internal agent.hpp header. That header
// pulls the HSA interception and AQL Profile graphs into this pre-runtime topology path.
void
update_agent_runtime_visibility(rocprofiler_agent_t& agent_info);
std::unordered_set<std::string>&
get_agent_available_properties();
}  // namespace agent

namespace platform
{
namespace windows
{
namespace
{
using ::rocprofiler::agent::update_agent_runtime_visibility;

constexpr uint16_t kAmdVendorId = 0x1002;
constexpr LONG     kSuccess     = 0;

uint64_t
get_agent_offset()
{
    static uint64_t _v = []() {
        auto gen = std::mt19937{std::random_device{}()};
        auto rng = std::uniform_int_distribution<uint64_t>{std::numeric_limits<uint8_t>::max(),
                                                           std::numeric_limits<uint16_t>::max()};
        return rng(gen);
    }();
    return _v;
}

std::string
wchar_to_utf8(const wchar_t* value, size_t capacity)
{
    if(value == nullptr || capacity == 0) return {};

    size_t length = 0;
    while(length < capacity && value[length] != L'\0')
        ++length;
    if(length == 0) return {};

    const int required = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             value,
                                             static_cast<int>(length),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if(required <= 0) return {};

    auto result = std::string(static_cast<size_t>(required), '\0');
    if(WideCharToMultiByte(CP_UTF8,
                           WC_ERR_INVALID_CHARS,
                           value,
                           static_cast<int>(length),
                           result.data(),
                           required,
                           nullptr,
                           nullptr) != required)
        return {};

    return result;
}

std::string
cpu_model_name()
{
    constexpr auto key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";

    DWORD type = 0;
    DWORD size = 0;
    if(RegGetValueW(HKEY_LOCAL_MACHINE,
                    key,
                    L"ProcessorNameString",
                    RRF_RT_REG_SZ,
                    &type,
                    nullptr,
                    &size) != kSuccess ||
       size < sizeof(wchar_t))
        return "unknown CPU";

    auto value = std::vector<wchar_t>((size / sizeof(wchar_t)) + 1, L'\0');
    if(RegGetValueW(HKEY_LOCAL_MACHINE,
                    key,
                    L"ProcessorNameString",
                    RRF_RT_REG_SZ,
                    &type,
                    value.data(),
                    &size) != kSuccess)
        return "unknown CPU";

    auto result = wchar_to_utf8(value.data(), value.size());
    while(!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    while(!result.empty() && result.back() == ' ')
        result.pop_back();
    return result.empty() ? "unknown CPU" : result;
}

uint32_t
cpu_max_clock_mhz()
{
    constexpr auto key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    DWORD          type = 0;
    DWORD          mhz  = 0;
    DWORD          size = sizeof(mhz);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,
                    key,
                    L"~MHz",
                    RRF_RT_REG_DWORD,
                    &type,
                    &mhz,
                    &size) != kSuccess)
        return 0;
    return mhz;
}

uint32_t
cpu_family_id()
{
#if defined(_MSC_VER)
    int regs[4] = {};
    __cpuid(regs, 1);
    const auto eax         = static_cast<uint32_t>(regs[0]);
    const auto base_family = (eax >> 8) & 0xF;
    const auto ext_family  = (eax >> 20) & 0xFF;
    return (base_family == 0xF) ? (base_family + ext_family) : base_family;
#else
    return 0;
#endif
}

struct cpu_set_record
{
    uint32_t id              = 0;
    uint16_t group           = 0;
    uint8_t  logical_index   = 0;
    uint8_t  numa_node_index = 0;
};

uint64_t
cpu_set_key(uint16_t group, uint32_t logical_index)
{
    return (static_cast<uint64_t>(group) << 32) | logical_index;
}

std::vector<cpu_set_record>
query_cpu_sets()
{
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, nullptr, 0);
    if(size == 0) return {};

    auto data = std::vector<uint8_t>(size);
    if(!GetSystemCpuSetInformation(
           reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()), size, &size, nullptr, 0))
        return {};

    auto result = std::vector<cpu_set_record>{};
    for(ULONG offset = 0; offset < size;)
    {
        const auto* info =
            reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(data.data() + offset);
        if(info->Size == 0) break;
        if(info->Type == CpuSetInformation)
        {
            result.emplace_back(cpu_set_record{info->CpuSet.Id,
                                               info->CpuSet.Group,
                                               info->CpuSet.LogicalProcessorIndex,
                                               info->CpuSet.NumaNodeIndex});
        }
        offset += info->Size;
    }
    return result;
}

struct numa_record
{
    uint32_t node_number = 0;
    uint32_t core_count  = 0;
    uint32_t cpu_id_base = 0;
    std::vector<rocprofiler_agent_cache_t> caches = {};
};

std::vector<uint8_t>
query_logical_processor_information(LOGICAL_PROCESSOR_RELATIONSHIP relationship)
{
    DWORD size = 0;
    if(GetLogicalProcessorInformationEx(relationship, nullptr, &size) ||
       GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};

    auto data = std::vector<uint8_t>(size);
    if(!GetLogicalProcessorInformationEx(
           relationship,
           reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()),
           &size))
        return {};
    data.resize(size);
    return data;
}

std::vector<numa_record>
query_numa_nodes(const std::vector<cpu_set_record>& cpu_sets)
{
    auto data = query_logical_processor_information(RelationNumaNodeEx);
    if(data.empty()) data = query_logical_processor_information(RelationNumaNode);

    auto cpu_set_ids = std::unordered_map<uint64_t, uint32_t>{};
    for(const auto& itr : cpu_sets)
        cpu_set_ids.emplace(cpu_set_key(itr.group, itr.logical_index), itr.id);

    auto result = std::vector<numa_record>{};
    for(size_t offset = 0; offset < data.size();)
    {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            data.data() + offset);
        if(info->Size == 0) break;
        if(info->Relationship == RelationNumaNode)
        {
            numa_record node{};
            node.node_number = info->NumaNode.NodeNumber;
            node.cpu_id_base = std::numeric_limits<uint32_t>::max();

            uint16_t group_count = info->NumaNode.GroupCount;
            if(group_count == 0) group_count = 1;
            for(uint16_t i = 0; i < group_count; ++i)
            {
                const auto& affinity = info->NumaNode.GroupMasks[i];
                const auto  mask     = static_cast<uint64_t>(affinity.Mask);
                node.core_count += std::popcount(mask);
                for(uint32_t bit = 0; bit < sizeof(affinity.Mask) * 8; ++bit)
                {
                    if((mask & (uint64_t{1} << bit)) == 0) continue;
                    if(auto pos = cpu_set_ids.find(cpu_set_key(affinity.Group, bit));
                       pos != cpu_set_ids.end())
                        node.cpu_id_base = std::min(node.cpu_id_base, pos->second);
                }
            }
            if(node.cpu_id_base == std::numeric_limits<uint32_t>::max()) node.cpu_id_base = 0;
            result.emplace_back(std::move(node));
        }
        offset += info->Size;
    }

    if(result.empty())
    {
        numa_record node{};
        node.core_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if(!cpu_sets.empty())
        {
            node.cpu_id_base = cpu_sets.front().id;
            for(const auto& itr : cpu_sets)
                node.cpu_id_base = std::min(node.cpu_id_base, itr.id);
        }
        result.emplace_back(std::move(node));
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.node_number < rhs.node_number;
    });
    return result;
}

void
query_cpu_caches(std::vector<numa_record>&                numa_nodes,
                 const std::vector<cpu_set_record>& cpu_sets)
{
    auto data = query_logical_processor_information(RelationCache);
    if(data.empty()) return;

    auto cpu_set_to_numa = std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>{};
    for(const auto& itr : cpu_sets)
        cpu_set_to_numa.emplace(cpu_set_key(itr.group, itr.logical_index),
                                std::make_pair(itr.numa_node_index, itr.id));

    for(size_t offset = 0; offset < data.size();)
    {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            data.data() + offset);
        if(info->Size == 0) break;
        if(info->Relationship == RelationCache && info->Cache.GroupMask.Mask != 0)
        {
            const auto& affinity = info->Cache.GroupMask;
            const auto  bit = static_cast<uint32_t>(std::countr_zero(
                static_cast<uint64_t>(affinity.Mask)));
            if(auto pos = cpu_set_to_numa.find(cpu_set_key(affinity.Group, bit));
               pos != cpu_set_to_numa.end())
            {
                auto node = std::find_if(numa_nodes.begin(), numa_nodes.end(), [&](const auto& itr) {
                    return itr.node_number == pos->second.first;
                });
                if(node != numa_nodes.end())
                {
                    rocprofiler_agent_cache_t cache{};
                    cache.processor_id_low    = pos->second.second;
                    cache.size                = info->Cache.CacheSize;
                    cache.level               = info->Cache.Level;
                    cache.cache_line_size     = info->Cache.LineSize;
                    cache.cache_lines_per_tag = 0;
                    cache.association         = info->Cache.Associativity;
                    cache.latency             = 0;
                    cache.type.Value          = 0;
                    cache.type.ui32.CPU        = 1;
                    if(info->Cache.Type == CacheData || info->Cache.Type == CacheUnified)
                        cache.type.ui32.Data = 1;
                    if(info->Cache.Type == CacheInstruction || info->Cache.Type == CacheUnified)
                        cache.type.ui32.Instruction = 1;
                    node->caches.emplace_back(cache);
                }
            }
        }
        offset += info->Size;
    }
}

struct known_gpu_properties
{
    uint16_t    device_id;
    const char* canonical_name;
    const char* known_hsa_compatibility_alias;
    uint32_t    gfx_target_version;
    uint32_t    cu_count;
    uint32_t    simd_per_cu;
    uint32_t    wave_front_size;
    uint32_t    max_waves_per_simd;
    uint32_t    array_count;
    uint32_t    simd_arrays_per_engine;
    uint32_t    lds_size_in_kb;
    uint32_t    num_gws;
    uint32_t    max_slots_scratch_cu;
    uint32_t    num_sdma_engines;
    uint32_t    num_sdma_queues_per_engine;
    uint32_t    num_cp_queues;
    uint32_t    max_engine_clock_mhz;
    uint32_t    memory_bus_width;
    uint32_t    memory_clock_mhz;
    uint32_t    l1_cache_kb;
    uint32_t    l2_cache_kb;
    uint32_t    l3_cache_kb;
    bool        integrated;
};

// Native D3DKMT exposes adapter identity, memory, PCI location, clocks, and display names, but it
// does not expose ROCr's private CU/SIMD/GFX-IP topology. Keep the initial Windows profile honest
// and constrained: only devices whose invariant topology has been validated on the paired Linux
// host are admitted. Extend this table only with equivalent hardware validation or replace it with
// a public Windows topology query when one becomes available.
constexpr std::array<known_gpu_properties, 1> kKnownGpus = {{
    {0x1586,
     "gfx1151",
     "gfx1150",
     110501,
     40,
     2,
     32,
     16,
     4,
     2,
     64,
     64,
     32,
     1,
     6,
     8,
     2900,
     256,
     1000,
     32,
     2048,
     32768,
     true},
}};

const known_gpu_properties*
find_known_gpu(uint16_t device_id)
{
    auto itr = std::find_if(kKnownGpus.begin(), kKnownGpus.end(), [&](const auto& entry) {
        return entry.device_id == device_id;
    });
    return (itr == kKnownGpus.end()) ? nullptr : &(*itr);
}

template <typename Tp>
bool
query_adapter(D3DKMT_HANDLE handle, KMTQUERYADAPTERINFOTYPE type, Tp& value)
{
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter                   = handle;
    query.Type                       = type;
    query.pPrivateDriverData         = &value;
    query.PrivateDriverDataSize      = sizeof(value);
    return D3DKMTQueryAdapterInfo(&query) == STATUS_SUCCESS;
}

struct adapter_snapshot
{
    D3DKMT_HANDLE             handle = 0;
    LUID                      luid{};
    D3DKMT_QUERY_DEVICE_IDS   device_ids{};
    D3DKMT_ADAPTERADDRESS     address{};
    D3DKMT_ADAPTERREGISTRYINFO registry_info{};
    D3DKMT_SEGMENTSIZEINFO    segment_size{};
    D3DKMT_ADAPTERTYPE        adapter_type{};
    D3DKMT_ADAPTER_PERFDATA   adapter_perf{};
    D3DKMT_GPUVERSION         gpu_version{};
    uint32_t                  max_engine_clock_mhz = 0;
};

uint32_t
query_max_engine_clock_mhz(D3DKMT_HANDLE handle, LUID luid)
{
    D3DKMT_QUERYSTATISTICS stats{};
    stats.Type        = D3DKMT_QUERYSTATISTICS_ADAPTER;
    stats.AdapterLuid = luid;
    if(D3DKMTQueryStatistics(&stats) != STATUS_SUCCESS) return 0;

    uint64_t max_frequency = 0;
    for(uint32_t node = 0; node < stats.QueryResult.AdapterInformation.NodeCount; ++node)
    {
        D3DKMT_NODE_PERFDATA perf{};
        perf.NodeOrdinal          = node;
        perf.PhysicalAdapterIndex = 0;
        if(query_adapter(handle, KMTQAITYPE_NODEPERFDATA, perf))
            max_frequency = std::max(
                max_frequency, std::max<uint64_t>(perf.MaxFrequency, perf.MaxFrequencyOC));
    }
    return static_cast<uint32_t>(std::min<uint64_t>(
        max_frequency / 1000000, std::numeric_limits<uint32_t>::max()));
}

std::vector<adapter_snapshot>
query_amd_adapters()
{
    D3DKMT_ENUMADAPTERS3 query{};
    query.Filter.IncludeComputeOnly = true;
    if(D3DKMTEnumAdapters3(&query) != STATUS_SUCCESS || query.NumAdapters == 0) return {};

    auto adapters = std::vector<D3DKMT_ADAPTERINFO>(query.NumAdapters);
    query.pAdapters = adapters.data();
    if(D3DKMTEnumAdapters3(&query) != STATUS_SUCCESS) return {};

    auto result = std::vector<adapter_snapshot>{};
    for(uint32_t i = 0; i < query.NumAdapters; ++i)
    {
        adapter_snapshot current{};
        current.handle = adapters[i].hAdapter;
        current.luid   = adapters[i].AdapterLuid;

        if(!query_adapter(
               current.handle, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, current.device_ids) ||
           current.device_ids.DeviceIds.VendorID != kAmdVendorId)
            continue;

        if(find_known_gpu(static_cast<uint16_t>(current.device_ids.DeviceIds.DeviceID)) == nullptr)
        {
            ROCP_WARNING << fmt::format(
                "windows::enumerate: skipping AMD adapter {} with unsupported device ID 0x{:04x}",
                i,
                current.device_ids.DeviceIds.DeviceID);
            continue;
        }

        if(!query_adapter(current.handle, KMTQAITYPE_ADAPTERADDRESS, current.address) ||
           !query_adapter(current.handle, KMTQAITYPE_ADAPTERREGISTRYINFO, current.registry_info) ||
           !query_adapter(current.handle, KMTQAITYPE_GETSEGMENTSIZE, current.segment_size))
        {
            ROCP_WARNING << fmt::format(
                "windows::enumerate: skipping AMD adapter {} because a required D3DKMT query failed",
                i);
            continue;
        }

        query_adapter(current.handle, KMTQAITYPE_ADAPTERTYPE, current.adapter_type);
        current.adapter_perf.PhysicalAdapterIndex = 0;
        query_adapter(current.handle, KMTQAITYPE_ADAPTERPERFDATA, current.adapter_perf);
        current.gpu_version.PhysicalAdapterIndex = 0;
        query_adapter(current.handle, KMTQUITYPE_GPUVERSION, current.gpu_version);
        current.max_engine_clock_mhz = query_max_engine_clock_mhz(current.handle, current.luid);
        result.emplace_back(current);
    }

    for(const auto& adapter : adapters)
    {
        if(adapter.hAdapter == 0) continue;
        D3DKMT_CLOSEADAPTER close{};
        close.hAdapter = adapter.hAdapter;
        D3DKMTCloseAdapter(&close);
    }

    return result;
}

template <typename Tp>
Tp*
copy_array(const std::vector<Tp>& values)
{
    if(values.empty()) return nullptr;
    auto* result = new Tp[values.size()];
    std::copy(values.begin(), values.end(), result);
    return result;
}

unique_agent_t
make_agent(rocprofiler_agent_t                        info,
           const std::vector<rocprofiler_agent_mem_bank_t>& mem_banks,
           const std::vector<rocprofiler_agent_cache_t>&    caches,
           const std::vector<rocprofiler_agent_io_link_t>&  io_links)
{
    info.mem_banks_count = static_cast<uint32_t>(mem_banks.size());
    info.caches_count    = static_cast<uint32_t>(caches.size());
    info.io_links_count  = static_cast<uint32_t>(io_links.size());
    info.mem_banks       = copy_array(mem_banks);
    info.caches          = copy_array(caches);
    info.io_links        = copy_array(io_links);

    update_agent_runtime_visibility(info);

    return unique_agent_t{new rocprofiler_agent_t{info}, [](rocprofiler_agent_t* ptr) {
                              if(ptr)
                              {
                                  delete[] ptr->mem_banks;
                                  delete[] ptr->caches;
                                  delete[] ptr->io_links;
                              }
                              delete ptr;
                          }};
}

std::vector<rocprofiler_agent_cache_t>
make_gpu_caches(const known_gpu_properties& gpu)
{
    auto result = std::vector<rocprofiler_agent_cache_t>{};
    for(const auto& [level, size, line_size] :
        std::array<std::array<uint32_t, 3>, 3>{{
            {1, gpu.l1_cache_kb, 128},
            {2, gpu.l2_cache_kb, 128},
            {3, gpu.l3_cache_kb, 64},
        }})
    {
        rocprofiler_agent_cache_t cache{};
        cache.level           = level;
        cache.size            = size;
        cache.cache_line_size = line_size;
        cache.type.Value      = 0;
        cache.type.ui32.Data  = 1;
        cache.type.ui32.HSACU = 1;
        result.emplace_back(cache);
    }
    return result;
}

}  // namespace

bool
is_available()
{
    return true;
}

std::vector<unique_agent_t>
enumerate()
{
    // Linux learns these constant-counter names while decoding topology properties. Native
    // Windows materializes the same public agent fields directly, so register the equivalent
    // property names before the metrics YAML is parsed.
    auto& properties = rocprofiler::agent::get_agent_available_properties();
    properties.insert({"cpu_cores_count",
                       "simd_count",
                       "mem_banks_count",
                       "caches_count",
                       "io_links_count",
                       "cpu_core_id_base",
                       "simd_id_base",
                       "max_waves_per_simd",
                       "lds_size_in_kb",
                       "gds_size_in_kb",
                       "num_gws",
                       "wave_front_size",
                       "array_count",
                       "simd_arrays_per_engine",
                       "cu_per_simd_array",
                       "simd_per_cu",
                       "max_slots_scratch_cu",
                       "gfx_target_version",
                       "vendor_id",
                       "device_id",
                       "location_id",
                       "domain",
                       "drm_render_minor",
                       "hive_id",
                       "num_sdma_engines",
                       "num_sdma_xgmi_engines",
                       "num_sdma_queues_per_engine",
                       "num_cp_queues",
                       "max_engine_clk_ccompute",
                       "association",
                       "cache_line_size",
                       "cache_lines_per_tag",
                       "capability",
                       "flags",
                       "fw_version",
                       "heap_type",
                       "latency",
                       "level",
                       "local_mem_size",
                       "max_bandwidth",
                       "max_engine_clk_fcompute",
                       "max_latency",
                       "mem_clk_max",
                       "min_bandwidth",
                       "min_latency",
                       "node_from",
                       "node_to",
                       "num_xcc",
                       "processor_id_low",
                       "recommended_transfer_size",
                       "sdma_fw_version",
                       "size",
                       "size_in_bytes",
                       "type",
                       "unique_id",
                       "version_major",
                       "version_minor",
                       "weight",
                       "width"});

    auto cpu_sets   = query_cpu_sets();
    auto numa_nodes = query_numa_nodes(cpu_sets);
    query_cpu_caches(numa_nodes, cpu_sets);
    auto adapters = query_amd_adapters();

    auto result      = std::vector<unique_agent_t>{};
    auto logical_id  = uint64_t{0};
    auto cpu_type_id = int32_t{0};
    auto gpu_type_id = int32_t{0};
    const auto offset = get_agent_offset();

    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    GlobalMemoryStatusEx(&memory_status);
    const uint64_t memory_per_numa =
        numa_nodes.empty() ? memory_status.ullTotalPhys
                           : memory_status.ullTotalPhys / numa_nodes.size();

    const auto cpu_name   = cpu_model_name();
    const auto cpu_family = cpu_family_id();
    const auto cpu_clock  = cpu_max_clock_mhz();

    for(size_t i = 0; i < numa_nodes.size(); ++i)
    {
        const auto& node = numa_nodes[i];
        auto info                 = common::init_public_api_struct(rocprofiler_agent_t{});
        info.type                 = ROCPROFILER_AGENT_TYPE_CPU;
        info.logical_node_id      = static_cast<int32_t>(logical_id);
        info.logical_node_type_id = cpu_type_id++;
        info.node_id              = static_cast<uint32_t>(i);
        info.id.handle            = logical_id++ + offset;
        info.cpu_cores_count      = node.core_count;
        info.cpu_core_id_base     = node.cpu_id_base;
        info.cu_count             = node.core_count;
        info.family_id            = cpu_family;
        info.max_engine_clk_ccompute = cpu_clock;
        info.name                 = common::get_string_entry(cpu_name)->c_str();
        info.product_name         = info.name;
        info.model_name           = info.name;
        info.vendor_name          = common::get_string_entry("CPU")->c_str();
        std::memset(&info.uuid.bytes, 0, sizeof(info.uuid.bytes));

        rocprofiler_agent_mem_bank_t memory{};
        memory.heap_type     = HSA_HEAPTYPE_SYSTEM;
        memory.size_in_bytes = memory_per_numa;
        memory.width         = 64;

        result.emplace_back(make_agent(info, {memory}, node.caches, {}));
    }

    for(const auto& adapter : adapters)
    {
        const auto device_id = static_cast<uint16_t>(adapter.device_ids.DeviceIds.DeviceID);
        const auto* gpu      = find_known_gpu(device_id);
        if(gpu == nullptr) continue;

        const uint32_t node_id = static_cast<uint32_t>(numa_nodes.size() + gpu_type_id);
        auto info                 = common::init_public_api_struct(rocprofiler_agent_t{});
        info.type                 = ROCPROFILER_AGENT_TYPE_GPU;
        info.logical_node_id      = static_cast<int32_t>(logical_id);
        info.logical_node_type_id = gpu_type_id++;
        info.node_id              = node_id;
        info.id.handle            = logical_id++ + offset;
        info.vendor_id            = kAmdVendorId;
        info.device_id            = device_id;
        info.location_id = ((adapter.address.BusNumber & 0xFF) << 8) |
                           ((adapter.address.DeviceNumber & 0x1F) << 3) |
                           (adapter.address.FunctionNumber & 0x7);
        info.domain                  = 0;
        info.gpu_id                  = device_id;
        info.simd_count              = gpu->cu_count * gpu->simd_per_cu;
        info.simd_id_base            = 0;
        info.cu_count                = gpu->cu_count;
        info.simd_per_cu             = gpu->simd_per_cu;
        info.wave_front_size         = gpu->wave_front_size;
        info.max_waves_per_simd      = gpu->max_waves_per_simd;
        info.max_waves_per_cu        = gpu->max_waves_per_simd * gpu->simd_per_cu;
        info.array_count             = gpu->array_count;
        info.simd_arrays_per_engine  = gpu->simd_arrays_per_engine;
        info.num_shader_banks        = gpu->array_count / gpu->simd_arrays_per_engine;
        info.cu_per_simd_array       = gpu->cu_count / gpu->array_count;
        info.cu_per_engine           = gpu->cu_count / info.num_shader_banks;
        info.lds_size_in_kb          = gpu->lds_size_in_kb;
        info.gds_size_in_kb          = 0;
        info.num_gws                 = gpu->num_gws;
        info.num_xcc                 = 1;
        info.max_slots_scratch_cu    = gpu->max_slots_scratch_cu;
        info.gfx_target_version      = gpu->gfx_target_version;
        info.num_sdma_engines        = gpu->num_sdma_engines;
        info.num_sdma_xgmi_engines   = 0;
        info.num_sdma_queues_per_engine = gpu->num_sdma_queues_per_engine;
        info.num_cp_queues              = gpu->num_cp_queues;
        info.max_engine_clk_fcompute =
            adapter.max_engine_clock_mhz > 0 ? adapter.max_engine_clock_mhz
                                             : gpu->max_engine_clock_mhz;
        info.local_mem_size = gpu->integrated ? 0 : adapter.segment_size.DedicatedVideoMemorySize;
        info.workgroup_max_size = 1024;
        info.workgroup_max_dim  = {1024, 1024, 1024};
        info.grid_max_size      = std::numeric_limits<int32_t>::max();
        info.grid_max_dim       = {std::numeric_limits<int32_t>::max(),
                                   std::numeric_limits<uint16_t>::max(),
                                   std::numeric_limits<uint16_t>::max()};
        info.capability.Value                 = 0;
        info.capability.ui32.SVMAPISupported  = 1;
        info.capability.ui32.DoorbellType     = 2;
        info.fw_version.Value                 = 0;
        info.sdma_fw_version.Value            = 0;
        std::memset(&info.uuid.bytes, 0, sizeof(info.uuid.bytes));

        auto product_name = wchar_to_utf8(adapter.registry_info.AdapterString,
                                          std::size(adapter.registry_info.AdapterString));
        if(product_name.empty()) product_name = "unknown AMD GPU";
        auto reported_arch = wchar_to_utf8(adapter.gpu_version.GpuArchitecture,
                                           std::size(adapter.gpu_version.GpuArchitecture));
        if(reported_arch.empty()) reported_arch = gpu->canonical_name;

        info.name         = common::get_string_entry(gpu->canonical_name)->c_str();
        info.product_name = common::get_string_entry(product_name)->c_str();
        info.vendor_name  = common::get_string_entry("AMD")->c_str();
        info.model_name   = common::get_string_entry(reported_arch)->c_str();

        if(reported_arch != gpu->canonical_name)
        {
            ROCP_INFO << fmt::format(
                "windows::enumerate: canonicalizing device 0x{:04x} D3DKMT architecture '{}' "
                "to '{}'",
                device_id,
                reported_arch,
                gpu->canonical_name);
        }
        if(gpu->known_hsa_compatibility_alias != nullptr &&
           std::string_view{gpu->known_hsa_compatibility_alias} != gpu->canonical_name)
        {
            ROCP_INFO << fmt::format(
                "windows::enumerate: device 0x{:04x} has known installed-HSA compatibility "
                "alias '{}'; SDK counter identity remains '{}'",
                device_id,
                gpu->known_hsa_compatibility_alias,
                gpu->canonical_name);
        }

        uint64_t gpu_memory_size = adapter.segment_size.DedicatedVideoMemorySize;
        if(gpu->integrated)
        {
            gpu_memory_size += adapter.segment_size.DedicatedSystemMemorySize;
            gpu_memory_size += adapter.segment_size.SharedSystemMemorySize;
        }

        rocprofiler_agent_mem_bank_t memory{};
        memory.heap_type     = HSA_HEAPTYPE_FRAME_BUFFER_PRIVATE;
        memory.size_in_bytes = gpu_memory_size;
        memory.width         = gpu->memory_bus_width;
        memory.mem_clk_max = adapter.adapter_perf.MaxMemoryFrequency > 0
                                 ? static_cast<uint32_t>(adapter.adapter_perf.MaxMemoryFrequency /
                                                         1000000)
                                 : gpu->memory_clock_mhz;

        auto io_links = std::vector<rocprofiler_agent_io_link_t>{};
        if(!numa_nodes.empty())
        {
            rocprofiler_agent_io_link_t link{};
            link.type      = gpu->integrated ? HSA_IOLINK_TYPE_XGMI : HSA_IOLINKTYPE_PCIEXPRESS;
            link.node_from = node_id;
            link.node_to   = 0;
            link.weight    = gpu->integrated ? 10 : 20;
            link.flags.LinkProperty        = 0;
            link.flags.ui32.Override       = 1;
            link.flags.ui32.NonCoherent    = gpu->integrated ? 0 : 1;
            link.flags.ui32.NoAtomics32bit = 0;
            link.flags.ui32.NoAtomics64bit = 0;
            io_links.emplace_back(link);
        }

        ROCP_INFO << fmt::format(
            "windows::enumerate: node={} device=0x{:04x} target={} CUs={} SIMDs={} wave={} "
            "BDF={:02x}:{:02x}.{} memory={} product='{}'",
            node_id,
            device_id,
            gpu->canonical_name,
            info.cu_count,
            info.simd_count,
            info.wave_front_size,
            adapter.address.BusNumber,
            adapter.address.DeviceNumber,
            adapter.address.FunctionNumber,
            gpu_memory_size,
            product_name);

        result.emplace_back(make_agent(info, {memory}, make_gpu_caches(*gpu), io_links));
    }

    return result;
}

}  // namespace windows
}  // namespace platform
}  // namespace rocprofiler
