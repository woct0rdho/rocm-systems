// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc.
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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <hsa.h>
#include <hsa_ext_amd.h>
#include <hsa_ven_amd_aqlprofile.h>

#include "aqlprofile-sdk/aql_profile_v2.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
constexpr std::array<const char*, 9> required_exports = {
    "hsa_ven_amd_aqlprofile_start",
    "hsa_ven_amd_aqlprofile_stop",
    "hsa_ven_amd_aqlprofile_read",
    "hsa_ven_amd_aqlprofile_validate_event",
    "hsa_ven_amd_aqlprofile_get_info",
    "hsa_ven_amd_aqlprofile_iterate_data",
    "aqlprofile_get_version",
    "aqlprofile_register_agent",
    "aqlprofile_validate_pmc_event",
};

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
    HMODULE module_;
};

struct HsaApi
{
    decltype(&hsa_init) init                                         = nullptr;
    decltype(&hsa_shut_down) shut_down                              = nullptr;
    decltype(&hsa_iterate_agents) iterate_agents                    = nullptr;
    decltype(&hsa_agent_get_info) agent_get_info                    = nullptr;
    decltype(&hsa_system_get_major_extension_table) extension_table = nullptr;
};

const HsaApi* hsa_api = nullptr;

template <typename Tp>
bool
load_symbol(HMODULE module, const char* name, Tp* output)
{
    *output = reinterpret_cast<Tp>(GetProcAddress(module, name));
    return *output != nullptr;
}

struct AgentProbeData
{
    bool found_gpu_node_1       = false;
    bool pm4_ib_capability_seen = false;
};

hsa_status_t
agent_callback(hsa_agent_t agent, void* data)
{
    hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
    auto status             = hsa_api->agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
    if(status != HSA_STATUS_SUCCESS) return status;
    if(type != HSA_DEVICE_TYPE_GPU) return HSA_STATUS_SUCCESS;

    char     name[64] = {};
    uint32_t node_id  = 0;
    status            = hsa_api->agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    if(status != HSA_STATUS_SUCCESS) return status;
    status = hsa_api->agent_get_info(
        agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_DRIVER_NODE_ID), &node_id);
    if(status != HSA_STATUS_SUCCESS) return status;

    std::array<uint8_t, 8> aql_extensions = {};
    status = hsa_api->agent_get_info(
        agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_AQL_EXTENSIONS),
        aql_extensions.data());
    if(status != HSA_STATUS_SUCCESS) return status;
    constexpr uint8_t pm4_ib_property = 1u << 1;
    const bool pm4_ib_capability = (aql_extensions[0] & pm4_ib_property) != 0;

    std::cout << "GPU name=" << name << " node_id=" << node_id
              << " wddm_aql_profile_capability="
              << (pm4_ib_capability ? "available" : "unavailable") << '\n';
    if(node_id == 1 &&
       (std::string_view{name} == "gfx1150" || std::string_view{name} == "gfx1151"))
    {
        auto* probe_data                   = static_cast<AgentProbeData*>(data);
        probe_data->found_gpu_node_1        = true;
        probe_data->pm4_ib_capability_seen = pm4_ib_capability;
        if(std::string_view{name} == "gfx1150")
            std::cout << "NOTE: Windows HSA reports gfx1150 for the gfx1151 device\n";
    }
    return HSA_STATUS_SUCCESS;
}

int
fail(const char* message, uint32_t detail = 0)
{
    std::cerr << message;
    if(detail != 0) std::cerr << " (detail=" << detail << ")";
    std::cerr << '\n';
    return 1;
}
}  // namespace

int
wmain(int argc, wchar_t** argv)
{
    if(argc != 3)
        return fail(
            "usage: aqlprofile-windows-loader-probe.exe <aqlprofile-dll> <amdhip-runtime-dll>");

    const auto runtime_directory = std::filesystem::path{argv[2]}.parent_path().wstring();
    if(runtime_directory.empty() || !SetDllDirectoryW(runtime_directory.c_str()))
        return fail("Configuring the requested HIP/HSA runtime directory failed", GetLastError());

    ScopedModule runtime(LoadLibraryExW(
        argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
    if(!runtime.valid()) return fail("Loading the requested HIP/HSA runtime failed", GetLastError());

    HsaApi api = {};
    if(!load_symbol(runtime.get(), "hsa_init", &api.init) ||
       !load_symbol(runtime.get(), "hsa_shut_down", &api.shut_down) ||
       !load_symbol(runtime.get(), "hsa_iterate_agents", &api.iterate_agents) ||
       !load_symbol(runtime.get(), "hsa_agent_get_info", &api.agent_get_info) ||
       !load_symbol(runtime.get(),
                    "hsa_system_get_major_extension_table",
                    &api.extension_table))
        return fail("The requested HIP/HSA runtime is missing a required HSA export", GetLastError());
    hsa_api = &api;

    {
        ScopedModule module(LoadLibraryExW(
            argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
        if(!module.valid()) return fail("Loading the AQL Profile DLL failed", GetLastError());

        for(const auto* symbol : required_exports)
        {
            if(GetProcAddress(module.get(), symbol) == nullptr)
                return fail(symbol, GetLastError());
        }

        using get_version_fn_t = hsa_status_t (*)(aqlprofile_version_t*);
        auto* get_version = reinterpret_cast<get_version_fn_t>(
            GetProcAddress(module.get(), "aqlprofile_get_version"));
        aqlprofile_version_t direct_version = {};
        auto status = get_version(&direct_version);
        if(status != HSA_STATUS_SUCCESS) return fail("aqlprofile_get_version failed", status);
        std::cout << "Direct AQL Profile version=" << direct_version.major << '.'
                  << direct_version.minor << '.' << direct_version.patch << '\n';
    }

    auto status = api.init();
    if(status != HSA_STATUS_SUCCESS) return fail("hsa_init failed", status);

    hsa_ven_amd_aqlprofile_pfn_t extension_table = {};
    status = api.extension_table(HSA_EXTENSION_AMD_AQLPROFILE,
                                 hsa_ven_amd_aqlprofile_VERSION_MAJOR,
                                 sizeof(extension_table),
                                 &extension_table);
    if(status != HSA_STATUS_SUCCESS)
    {
        api.shut_down();
        return fail("HSA AQL Profile extension query failed", status);
    }
    if(extension_table.hsa_ven_amd_aqlprofile_version_major == nullptr ||
       extension_table.hsa_ven_amd_aqlprofile_version_minor == nullptr ||
       extension_table.hsa_ven_amd_aqlprofile_validate_event == nullptr)
    {
        api.shut_down();
        return fail("HSA AQL Profile extension table is incomplete");
    }

    std::cout << "HSA AQL Profile extension version="
              << extension_table.hsa_ven_amd_aqlprofile_version_major() << '.'
              << extension_table.hsa_ven_amd_aqlprofile_version_minor() << '\n';

    AgentProbeData probe_data = {};
    status                    = api.iterate_agents(agent_callback, &probe_data);
    if(status != HSA_STATUS_SUCCESS)
    {
        api.shut_down();
        return fail("hsa_iterate_agents failed", status);
    }
    if(!probe_data.found_gpu_node_1)
    {
        api.shut_down();
        return fail("gfx115x GPU node 1 was not found");
    }

    status = api.shut_down();
    if(status != HSA_STATUS_SUCCESS) return fail("hsa_shut_down failed", status);

    std::cout << "PASS: direct DLL loading and HSA extension loading succeeded\n";
    return 0;
}
