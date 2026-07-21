// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions:
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

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <hsa.h>
#include <hsa_ven_amd_aqlprofile.h>

#include "aqlprofile-sdk/aql_profile_v2.h"
#include "impl/wddm/profiling.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
class ScopedModule
{
public:
    explicit ScopedModule(HMODULE module)
    : module_(module)
    {}

    ~ScopedModule()
    {
        if(module_ != nullptr) FreeLibrary(module_);
    }

    ScopedModule(const ScopedModule&)            = delete;
    ScopedModule& operator=(const ScopedModule&) = delete;

    HMODULE get() const { return module_; }
    bool valid() const { return module_ != nullptr; }

private:
    HMODULE module_ = nullptr;
};

struct AqlProfileApi
{
    decltype(&aqlprofile_register_agent) register_agent     = nullptr;
    decltype(&aqlprofile_get_pmc_info) get_pmc_info         = nullptr;
    decltype(&aqlprofile_validate_pmc_event) validate_event = nullptr;
    decltype(&aqlprofile_pmc_create_packets) create_packets = nullptr;
    decltype(&aqlprofile_pmc_delete_packets) delete_packets = nullptr;
};

const AqlProfileApi* aql_api = nullptr;

constexpr uint32_t wddm_frame_bytes = 0x500;
// Two timestamp COPY_DATA packets, one barrier, one ACQUIRE_MEM, completion signaling,
// and the queue read-pointer update in VendorSpecificAqlToPm4().
constexpr uint32_t atomic_tail_with_profiled_completion_bytes = 160;
constexpr uint32_t cpu_tail_with_profiled_completion_bytes    = 112;
constexpr uint32_t pm4_size_mask                              = 0x000fffff;
constexpr uint32_t manifest_magic                             = 0x504d4357;
constexpr uint32_t manifest_version                           = 1;
constexpr uint32_t manifest_required_flags                    = 0x3;

struct Allocation
{
    void*                          pointer = nullptr;
    uint64_t                       size    = 0;
    aqlprofile_buffer_desc_flags_t flags   = {};
};

struct AllocationTracker
{
    std::vector<Allocation> active = {};
    uint64_t                total_allocated = 0;
    uint64_t                largest_allocation = 0;
};

hsa_status_t
allocate_buffer(void**                            pointer,
                uint64_t                          size,
                aqlprofile_buffer_desc_flags_t    flags,
                void*                             userdata)
{
    if(pointer == nullptr || userdata == nullptr || size == 0)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;

    auto* tracker = static_cast<AllocationTracker*>(userdata);
    auto* memory  = _aligned_malloc(static_cast<size_t>(size), 4096);
    if(memory == nullptr) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;

    std::memset(memory, 0, static_cast<size_t>(size));
    *pointer = memory;
    tracker->active.push_back({memory, size, flags});
    tracker->total_allocated += size;
    tracker->largest_allocation = std::max(tracker->largest_allocation, size);
    return HSA_STATUS_SUCCESS;
}

void
deallocate_buffer(void* pointer, void* userdata)
{
    if(pointer == nullptr) return;

    auto* tracker = static_cast<AllocationTracker*>(userdata);
    if(tracker != nullptr)
    {
        const auto itr = std::find_if(tracker->active.begin(),
                                      tracker->active.end(),
                                      [pointer](const auto& allocation) {
                                          return allocation.pointer == pointer;
                                      });
        if(itr != tracker->active.end()) tracker->active.erase(itr);
    }
    _aligned_free(pointer);
}

hsa_status_t
copy_buffer(void* destination, const void* source, size_t size, void*)
{
    if(size == 0) return HSA_STATUS_SUCCESS;
    if(destination == nullptr || source == nullptr) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::memcpy(destination, source, size);
    return HSA_STATUS_SUCCESS;
}

int
fail(const char* message, uint32_t detail = 0)
{
    std::fprintf(stderr, "FAIL: %s", message);
    if(detail != 0) std::fprintf(stderr, " detail=0x%x", detail);
    std::fputc('\n', stderr);
    return 1;
}

hsa_status_t
query_block(aqlprofile_agent_handle_t                    agent,
            const char*                                  name,
            hsa_ven_amd_aqlprofile_id_query_t*           query)
{
    *query = {name, 0, 0};
    const aqlprofile_pmc_profile_t profile = {agent, nullptr, 0};
    return aql_api->get_pmc_info(&profile, AQLPROFILE_INFO_BLOCK_ID, query);
}

struct PacketInfo
{
    const char*             name           = nullptr;
    uint32_t                expected_stage = 0;
    uint16_t                format         = 0;
    uint32_t                pm4_dwords     = 0;
    uint64_t                pm4_address    = 0;
    std::array<uint32_t, 8> manifest       = {};
};

PacketInfo
packet_info(const char* name,
            uint32_t expected_stage,
            const hsa_ext_amd_aql_pm4_packet_t& packet)
{
    struct PacketPrefix
    {
        uint16_t header;
        uint16_t format;
        uint32_t indirect_buffer[4];
        uint32_t remaining_dwords;
        uint32_t reserved[8];
    };
    static_assert(sizeof(PacketPrefix) == 56);

    const auto* prefix = reinterpret_cast<const PacketPrefix*>(&packet);
    PacketInfo result  = {
        name,
        expected_stage,
        prefix->format,
        prefix->indirect_buffer[3] & pm4_size_mask,
        (static_cast<uint64_t>(prefix->indirect_buffer[2]) << 32) |
            (static_cast<uint64_t>(prefix->indirect_buffer[1]) & ~uint64_t{3}),
    };
    std::copy(std::begin(prefix->reserved), std::end(prefix->reserved), result.manifest.begin());
    return result;
}

uint32_t
command_checksum(uint64_t address, uint32_t dwords)
{
    uint32_t checksum = 2166136261u;
    const auto* commands = reinterpret_cast<const uint32_t*>(address);
    for(uint32_t i = 0; i < dwords; ++i)
    {
        checksum ^= commands[i];
        checksum *= 16777619u;
    }
    return checksum;
}

bool
allocation_contains(const AllocationTracker& tracker, uint64_t address, uint64_t bytes)
{
    return std::any_of(tracker.active.begin(), tracker.active.end(), [&](const auto& allocation) {
        const auto begin = reinterpret_cast<uint64_t>(allocation.pointer);
        const auto end   = begin + allocation.size;
        return address >= begin && address <= end && bytes <= end - address;
    });
}

template <typename Tp>
bool
load_symbol(HMODULE module, const char* name, Tp* output)
{
    *output = reinterpret_cast<Tp>(GetProcAddress(module, name));
    return *output != nullptr;
}
}  // namespace

int
wmain(int argc, wchar_t** argv)
{
    if(argc != 3)
        return fail("usage: aqlprofile-windows-pmc-packet-probe.exe <aqlprofile-dll> "
                    "<amdhip-runtime-dll>");

    const auto runtime_directory = std::filesystem::path{argv[2]}.parent_path().wstring();
    if(runtime_directory.empty() || !SetDllDirectoryW(runtime_directory.c_str()))
        return fail("configuring the requested HIP/HSA runtime directory", GetLastError());

    ScopedModule runtime(LoadLibraryExW(
        argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if(!runtime.valid())
        return fail("loading the requested HIP/HSA runtime", GetLastError());

    ScopedModule module(LoadLibraryExW(
        argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if(!module.valid())
        return fail("loading the requested AQL Profile DLL", GetLastError());

    AqlProfileApi api = {};
    if(!load_symbol(module.get(), "aqlprofile_register_agent", &api.register_agent) ||
       !load_symbol(module.get(), "aqlprofile_get_pmc_info", &api.get_pmc_info) ||
       !load_symbol(module.get(), "aqlprofile_validate_pmc_event", &api.validate_event) ||
       !load_symbol(module.get(), "aqlprofile_pmc_create_packets", &api.create_packets) ||
       !load_symbol(module.get(), "aqlprofile_pmc_delete_packets", &api.delete_packets))
        return fail("loading required AQL Profile packet-construction exports");
    aql_api = &api;

    const aqlprofile_agent_info_t agent_info = {
        "gfx1151",
        1,
        2,
        40,
        2,
    };

    aqlprofile_agent_handle_t agent = {};
    auto status = api.register_agent(&agent, &agent_info);
    if(status != HSA_STATUS_SUCCESS) return fail("aqlprofile_register_agent", status);

    hsa_ven_amd_aqlprofile_id_query_t grbm = {};
    hsa_ven_amd_aqlprofile_id_query_t gcea = {};
    hsa_ven_amd_aqlprofile_id_query_t sq   = {};
    status = query_block(agent, "GRBM", &grbm);
    if(status != HSA_STATUS_SUCCESS) return fail("GRBM block query", status);
    status = query_block(agent, "GCEA", &gcea);
    if(status != HSA_STATUS_SUCCESS) return fail("GCEA block query", status);
    status = query_block(agent, "SQ", &sq);
    if(status != HSA_STATUS_SUCCESS) return fail("SQ block query", status);
    if(grbm.instance_count == 0 || gcea.instance_count == 0 || sq.instance_count == 0)
        return fail("counter block query returned zero instances");

    const std::array<aqlprofile_pmc_event_t, 3> catalog_boundary_events = {
        aqlprofile_pmc_event_t{0,
                               40,
                               {},
                               static_cast<hsa_ven_amd_aqlprofile_block_name_t>(grbm.id)},
        aqlprofile_pmc_event_t{gcea.instance_count - 1,
                               77,
                               {},
                               static_cast<hsa_ven_amd_aqlprofile_block_name_t>(gcea.id)},
        aqlprofile_pmc_event_t{gcea.instance_count - 1,
                               78,
                               {},
                               static_cast<hsa_ven_amd_aqlprofile_block_name_t>(gcea.id)}};
    for(const auto& event : catalog_boundary_events)
    {
        bool valid = false;
        status     = api.validate_event(agent, &event, &valid);
        if(status != HSA_STATUS_SUCCESS || !valid)
            return fail("gfx1151 catalog boundary event validation", status);
    }

    std::vector<aqlprofile_pmc_event_t> events = {};
    for(uint32_t instance = 0; instance < grbm.instance_count; ++instance)
    {
        const auto block = static_cast<hsa_ven_amd_aqlprofile_block_name_t>(grbm.id);
        events.push_back({instance, 0, {}, block});
        events.push_back({instance, 2, {}, block});
    }
    for(uint32_t instance = 0; instance < sq.instance_count; ++instance)
    {
        events.push_back({instance, 4, {}, static_cast<hsa_ven_amd_aqlprofile_block_name_t>(sq.id)});
    }

    for(const auto& event : events)
    {
        bool valid = false;
        status = api.validate_event(agent, &event, &valid);
        if(status != HSA_STATUS_SUCCESS || !valid)
            return fail("aqlprofile_validate_pmc_event", status);
    }

    auto invalid_event = events.front();
    invalid_event.event_id = UINT32_MAX;
    bool valid = true;
    status = api.validate_event(agent, &invalid_event, &valid);
    if(status == HSA_STATUS_SUCCESS || valid)
        return fail("out-of-range event selector was accepted", status);

    auto invalid_instance = events.front();
    invalid_instance.block_index = UINT32_MAX;
    valid = true;
    status = api.validate_event(agent, &invalid_instance, &valid);
    if(status == HSA_STATUS_SUCCESS || valid)
        return fail("out-of-range instance was accepted", status);

    const aqlprofile_pmc_profile_t profile = {
        agent,
        events.data(),
        static_cast<uint32_t>(events.size()),
    };
    AllocationTracker tracker = {};
    aqlprofile_handle_t handle = {};
    aqlprofile_pmc_aql_packets_t packets = {};
    status = api.create_packets(&handle,
                                &packets,
                                profile,
                                allocate_buffer,
                                deallocate_buffer,
                                copy_buffer,
                                &tracker);
    if(status != HSA_STATUS_SUCCESS) return fail("aqlprofile_pmc_create_packets", status);

    const std::array packet_infos = {
        packet_info("start", 1, packets.start_packet),
        packet_info("read", 2, packets.read_packet),
        packet_info("stop", 3, packets.stop_packet),
    };

    const std::array packet_addresses = {
        &packets.start_packet,
        &packets.read_packet,
        &packets.stop_packet,
    };
    const auto capability = wsl::thunk::profiling::DetectCapability(11, 5, 1, wddm_frame_bytes, true);
    wsl::thunk::profiling::SubmissionState submission_state = {};
    auto resolve = [&tracker](uint64_t address,
                              uint64_t bytes,
                              wsl::thunk::profiling::ResolvedMemory* resolved) {
        for(const auto& allocation : tracker.active)
        {
            const auto begin = reinterpret_cast<uint64_t>(allocation.pointer);
            const auto end   = begin + allocation.size;
            if(address >= begin && address <= end && bytes <= end - address)
            {
                resolved->cpu_address = static_cast<const uint8_t*>(allocation.pointer) +
                                        static_cast<size_t>(address - begin);
                resolved->owner = static_cast<uintptr_t>(begin);
                return true;
            }
        }
        std::fprintf(stderr,
                     "adapter unresolved memory address=0x%llx bytes=%llu allocations=",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(bytes));
        for(const auto& allocation : tracker.active)
            std::fprintf(stderr,
                         "[0x%llx,+%llu]",
                         static_cast<unsigned long long>(
                             reinterpret_cast<uint64_t>(allocation.pointer)),
                         static_cast<unsigned long long>(allocation.size));
        std::fputc('\n', stderr);
        return false;
    };
    auto validate_signal = [](uint64_t) { return false; };

    bool packets_valid            = true;
    uint64_t manifest_profile_key = 0;
    for(size_t packet_index = 0; packet_index < packet_infos.size(); ++packet_index)
    {
        const auto& info = packet_infos[packet_index];
        const auto pm4_bytes = static_cast<uint64_t>(info.pm4_dwords) * sizeof(uint32_t);
        const auto atomic_translated_bytes =
            pm4_bytes + atomic_tail_with_profiled_completion_bytes;
        const auto cpu_translated_bytes = pm4_bytes + cpu_tail_with_profiled_completion_bytes;
        const bool address_valid = info.pm4_address != 0 &&
                                   allocation_contains(tracker, info.pm4_address, pm4_bytes);
        const bool atomic_fits = atomic_translated_bytes <= wddm_frame_bytes;
        const bool cpu_fits    = cpu_translated_bytes <= wddm_frame_bytes;
        const uint64_t profile_key =
            static_cast<uint64_t>(info.manifest[5]) |
            (static_cast<uint64_t>(info.manifest[6]) << 32);
        if(manifest_profile_key == 0) manifest_profile_key = profile_key;
        const bool manifest_valid =
            info.manifest[0] == manifest_magic && info.manifest[1] == manifest_version &&
            info.manifest[2] == info.expected_stage && info.manifest[3] == info.pm4_dwords &&
            info.manifest[4] == command_checksum(info.pm4_address, info.pm4_dwords) &&
            profile_key != 0 && profile_key == manifest_profile_key &&
            (info.manifest[7] & 0xff) == manifest_required_flags &&
            (info.manifest[7] >> 8) == events.size();

        wsl::thunk::profiling::AqlProfilePacket adapter_packet = {};
        static_assert(sizeof(adapter_packet) == sizeof(*packet_addresses[packet_index]));
        std::memcpy(&adapter_packet, packet_addresses[packet_index], sizeof(adapter_packet));
        const auto adapter_result = wsl::thunk::profiling::ValidatePacket(
            capability, adapter_packet, resolve, validate_signal);
        auto sequence_status = adapter_result.status;
        if(adapter_result.status == wsl::thunk::profiling::ValidationStatus::kSuccess)
            sequence_status = submission_state.Advance(adapter_result.stage,
                                                       adapter_result.profile_key);
        const bool adapter_valid =
            sequence_status == wsl::thunk::profiling::ValidationStatus::kSuccess;

        std::printf("packet=%s format=%u stage=%u pm4_dwords=%u pm4_bytes=%llu "
                    "address_in_allocation=%s manifest_valid=%s adapter_valid=%s/%u "
                    "manifest_magic=0x%08x version=%u length=%u checksum=0x%08x/0x%08x "
                    "profile_key=0x%llx events_flags=0x%08x "
                    "wddm_atomic_profiled_completion=%llu/%u fits=%s "
                    "wddm_cpu_profiled_completion=%llu/%u fits=%s\n",
                    info.name,
                    static_cast<unsigned>(info.format),
                    info.manifest[2],
                    info.pm4_dwords,
                    static_cast<unsigned long long>(pm4_bytes),
                    address_valid ? "yes" : "no",
                    manifest_valid ? "yes" : "no",
                    adapter_valid ? "yes" : "no",
                    static_cast<unsigned>(sequence_status),
                    info.manifest[0],
                    info.manifest[1],
                    info.manifest[3],
                    info.manifest[4],
                    command_checksum(info.pm4_address, info.pm4_dwords),
                    static_cast<unsigned long long>(profile_key),
                    info.manifest[7],
                    static_cast<unsigned long long>(atomic_translated_bytes),
                    wddm_frame_bytes,
                    atomic_fits ? "yes" : "no",
                    static_cast<unsigned long long>(cpu_translated_bytes),
                    wddm_frame_bytes,
                    cpu_fits ? "yes" : "no");

        packets_valid = packets_valid && info.format == 1 && info.pm4_dwords > 0 &&
                        address_valid && manifest_valid && adapter_valid && atomic_fits && cpu_fits;
    }

    std::printf("agent=gfx1151 xcc=1 shader_engines=2 compute_units=40 arrays_per_se=2 "
                "grbm_instances=%u sq_instances=%u events=%zu allocations=%zu "
                "total_allocated=%llu largest_allocation=%llu\n",
                grbm.instance_count,
                sq.instance_count,
                events.size(),
                tracker.active.size(),
                static_cast<unsigned long long>(tracker.total_allocated),
                static_cast<unsigned long long>(tracker.largest_allocation));

    packets_valid = packets_valid && submission_state.active_profile() == 0;

    api.delete_packets(handle);
    if(!tracker.active.empty()) return fail("packet deletion leaked callback allocations");
    if(!packets_valid) return fail("generated packet failed offline WDDM validation");

    std::puts("PASS: gfx1151 PMC packets constructed without HSA initialization");
    return 0;
}
