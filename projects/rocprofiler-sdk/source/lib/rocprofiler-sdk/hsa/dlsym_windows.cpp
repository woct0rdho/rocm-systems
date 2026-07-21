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

#include "lib/rocprofiler-sdk/hsa/hsa.hpp"

#include "lib/common/logging.hpp"

#include <Windows.h>

#include <type_traits>

namespace rocprofiler
{
namespace hsa
{
namespace
{
HMODULE
get_hsa_module()
{
    for(const auto* name : {L"amdhip64_7.dll", L"amdhip64.dll"})
    {
        if(auto module = ::GetModuleHandleW(name)) return module;
        if(auto module = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            return module;
    }

    return nullptr;
}

template <typename FuncT>
void
resolve(HMODULE module, FuncT& function, const char* name)
{
    function = reinterpret_cast<FuncT>(::GetProcAddress(module, name));
    ROCP_INFO_IF(!function) << "GetProcAddress did not find HSA symbol " << name;
}

void
resolve_core_table(HMODULE module, hsa_core_table_t* table)
{
    table->version.major_id = HSA_CORE_API_TABLE_MAJOR_VERSION;
    table->version.minor_id = sizeof(*table);
    table->version.step_id  = HSA_CORE_API_TABLE_STEP_VERSION;

#define ROCPROFILER_RESOLVE_CORE(NAME) resolve(module, table->NAME##_fn, #NAME)
    ROCPROFILER_RESOLVE_CORE(hsa_init);
    ROCPROFILER_RESOLVE_CORE(hsa_shut_down);
    ROCPROFILER_RESOLVE_CORE(hsa_iterate_agents);
    ROCPROFILER_RESOLVE_CORE(hsa_agent_get_info);
    ROCPROFILER_RESOLVE_CORE(hsa_system_get_major_extension_table);
    ROCPROFILER_RESOLVE_CORE(hsa_queue_create);
    ROCPROFILER_RESOLVE_CORE(hsa_queue_load_read_index_relaxed);
    ROCPROFILER_RESOLVE_CORE(hsa_queue_load_write_index_relaxed);
    ROCPROFILER_RESOLVE_CORE(hsa_queue_store_write_index_relaxed);
    ROCPROFILER_RESOLVE_CORE(hsa_signal_create);
    ROCPROFILER_RESOLVE_CORE(hsa_signal_destroy);
    ROCPROFILER_RESOLVE_CORE(hsa_signal_store_relaxed);
    ROCPROFILER_RESOLVE_CORE(hsa_signal_wait_scacquire);
    ROCPROFILER_RESOLVE_CORE(hsa_memory_free);
    ROCPROFILER_RESOLVE_CORE(hsa_code_object_reader_create_from_file);
    ROCPROFILER_RESOLVE_CORE(hsa_code_object_reader_destroy);
    ROCPROFILER_RESOLVE_CORE(hsa_executable_create_alt);
    ROCPROFILER_RESOLVE_CORE(hsa_executable_load_agent_code_object);
    ROCPROFILER_RESOLVE_CORE(hsa_executable_freeze);
    ROCPROFILER_RESOLVE_CORE(hsa_executable_get_symbol);
#undef ROCPROFILER_RESOLVE_CORE
}

void
resolve_amd_ext_table(HMODULE module, hsa_amd_ext_table_t* table)
{
    table->version.major_id = HSA_AMD_EXT_API_TABLE_MAJOR_VERSION;
    table->version.minor_id = sizeof(*table);
    table->version.step_id  = HSA_AMD_EXT_API_TABLE_STEP_VERSION;

#define ROCPROFILER_RESOLVE_AMD_EXT(NAME) resolve(module, table->NAME##_fn, #NAME)
    ROCPROFILER_RESOLVE_AMD_EXT(hsa_amd_agent_iterate_memory_pools);
    ROCPROFILER_RESOLVE_AMD_EXT(hsa_amd_memory_pool_get_info);
    ROCPROFILER_RESOLVE_AMD_EXT(hsa_amd_memory_pool_allocate);
    ROCPROFILER_RESOLVE_AMD_EXT(hsa_amd_agents_allow_access);
    ROCPROFILER_RESOLVE_AMD_EXT(hsa_amd_memory_async_copy);
#undef ROCPROFILER_RESOLVE_AMD_EXT
}
}  // namespace

template <typename TableT>
void
dlsym_table(TableT* table)
{
    if(!table) return;

    auto module = get_hsa_module();
    if(!module)
    {
        ROCP_WARNING << "Could not locate the loaded Windows HSA runtime module";
        return;
    }

    if constexpr(std::is_same_v<TableT, hsa_core_table_t>)
        resolve_core_table(module, table);
    else if constexpr(std::is_same_v<TableT, hsa_amd_ext_table_t>)
        resolve_amd_ext_table(module, table);
}

template void
dlsym_table<hsa_core_table_t>(hsa_core_table_t* table);

template void
dlsym_table<hsa_amd_ext_table_t>(hsa_amd_ext_table_t* table);
}  // namespace hsa
}  // namespace rocprofiler
