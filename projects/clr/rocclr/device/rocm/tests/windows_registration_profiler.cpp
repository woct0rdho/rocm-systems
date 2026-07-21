// Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
using hip_api_name_t = const char* (*)(uint32_t);

// HIP's stable dispatch ABI starts with the table size and hipApiName slot.
struct hip_dispatch_table_prefix
{
    size_t         size;
    hip_api_name_t hip_api_name;
};

const char*
profiled_hip_api_name(uint32_t)
{
    return "windows-registered-hip-api";
}
}  // namespace

extern "C" {
struct rocprofiler_client_id_t
{
    const char* name;
    uint32_t    handle;
};

struct rocprofiler_tool_configure_result_t
{
    size_t size;
    int (*initialize)(void (*)(rocprofiler_client_id_t), void*);
    void (*finalize)(void*);
    void* tool_data;
};

__declspec(dllexport) rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t, const char*, uint32_t, rocprofiler_client_id_t*)
{
    return nullptr;
}

__declspec(dllexport) int
rocprofiler_set_api_table(const char* name,
                          uint64_t,
                          uint64_t instance,
                          void**   tables,
                          uint64_t num_tables)
{
    if(!name || std::strcmp(name, "hip") != 0) return 1;
    if(instance != 0 || !tables || num_tables != 1) return 2;

    auto* table = static_cast<hip_dispatch_table_prefix*>(tables[0]);
    if(!table || table->size < sizeof(hip_dispatch_table_prefix)) return 3;
    table->hip_api_name = &profiled_hip_api_name;
    return 0;
}
}
