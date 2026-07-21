// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/hsa/windows_api.hpp"

#include "lib/rocprofiler-sdk/hsa/hsa.hpp"

namespace rocprofiler
{
namespace hsa
{
namespace
{
CoreApiTable&
core_table()
{
    static auto value = CoreApiTable{};
    return value;
}

AmdExtTable&
ext_table()
{
    static auto value = AmdExtTable{};
    return value;
}

PcSamplingExtTable&
pc_sampling_table()
{
    static auto value = PcSamplingExtTable{};
    return value;
}
}  // namespace

namespace windows
{
void
install_internal_tables(const CoreApiTable& core, const AmdExtTable& ext)
{
    core_table() = core;
    ext_table()  = ext;
}
}  // namespace windows

hsa_core_table_t*
get_core_table()
{
    return &core_table();
}

hsa_amd_ext_table_t*
get_amd_ext_table()
{
    return &ext_table();
}

PcSamplingExtTable*
get_pc_sampling_ext_table()
{
    return &pc_sampling_table();
}

std::string_view
get_hsa_status_string(hsa_status_t status)
{
    const char* message = nullptr;
    if(core_table().hsa_status_string_fn &&
       core_table().hsa_status_string_fn(status, &message) == HSA_STATUS_SUCCESS && message)
        return message;
    return "unknown HSA status";
}
}  // namespace hsa
}  // namespace rocprofiler
