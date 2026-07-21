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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <rocprofiler-sdk-roctx/roctx.h>

#if defined(_WIN32)
#    include <Windows.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr auto work_item_count = size_t{1024} * 1024;
constexpr auto workgroup_size  = uint32_t{256};
constexpr auto default_dispatch_count = uint32_t{8};

bool
check_hip(hipError_t status, const char* expression, int line)
{
    if(status == hipSuccess) return true;

    std::fprintf(stderr,
                 "HIP failure at line %d: %s returned %s\n",
                 line,
                 expression,
                 hipGetErrorString(status));
    return false;
}

#define CHECK_HIP(EXPR)                                                                            \
    do                                                                                             \
    {                                                                                              \
        if(!check_hip((EXPR), #EXPR, __LINE__)) return 1;                                         \
    } while(false)
}  // namespace

__global__ void
vector_add(const float* lhs, const float* rhs, float* result, size_t size)
{
    const auto index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if(index < size) result[index] = lhs[index] + rhs[index];
}

int
main(int argc, char** argv)
{
    auto dispatch_count = default_dispatch_count;
    auto dispatch_set = false;
    auto device_reset = false;
    auto graph_mode              = false;
    auto marker_mode             = false;
    auto selected_regions        = false;
    auto nested_selected_regions = false;
    auto create_file             = std::string{};
    for(int index = 1; index < argc; ++index)
    {
        if(std::strcmp(argv[index], "--device-reset") == 0)
        {
            if(device_reset)
            {
                std::fprintf(stderr, "--device-reset may be specified only once\n");
                return 2;
            }
            device_reset = true;
            continue;
        }
        if(std::strcmp(argv[index], "--graph") == 0)
        {
            if(graph_mode)
            {
                std::fprintf(stderr, "--graph may be specified only once\n");
                return 2;
            }
            graph_mode = true;
            continue;
        }
        if(std::strcmp(argv[index], "--markers") == 0)
        {
            if(marker_mode)
            {
                std::fprintf(stderr, "--markers may be specified only once\n");
                return 2;
            }
            marker_mode = true;
            continue;
        }
        if(std::strcmp(argv[index], "--selected-regions") == 0)
        {
            if(selected_regions)
            {
                std::fprintf(stderr, "--selected-regions may be specified only once\n");
                return 2;
            }
            selected_regions = true;
            continue;
        }
        if(std::strcmp(argv[index], "--nested-selected-regions") == 0)
        {
            if(nested_selected_regions)
            {
                std::fprintf(stderr, "--nested-selected-regions may be specified only once\n");
                return 2;
            }
            selected_regions        = true;
            nested_selected_regions = true;
            continue;
        }
        if(std::strcmp(argv[index], "--create-file") == 0 && index + 1 < argc)
        {
            if(!create_file.empty())
            {
                std::fprintf(stderr, "--create-file may be specified only once\n");
                return 2;
            }
            create_file = argv[++index];
            continue;
        }
        if(std::strcmp(argv[index], "--dispatches") != 0 || index + 1 >= argc)
        {
            std::fprintf(stderr,
                         "usage: %s [--dispatches 1..8] [--device-reset] [--graph] [--markers] "
                         "[--selected-regions] [--nested-selected-regions] "
                         "[--create-file PATH]\n",
                         argv[0]);
            return 2;
        }
        if(dispatch_set)
        {
            std::fprintf(stderr, "--dispatches may be specified only once\n");
            return 2;
        }
        dispatch_set = true;
        errno = 0;
        char* end = nullptr;
        const auto value = std::strtoul(argv[++index], &end, 10);
        if(errno != 0 || end == argv[index] || *end != '\0' || value < 1 ||
           value > default_dispatch_count)
        {
            std::fprintf(stderr, "--dispatches must be between 1 and 8\n");
            return 2;
        }
        dispatch_count = static_cast<uint32_t>(value);
    }
#if defined(_WIN32)
    wchar_t executable_path[32768] = {};
    wchar_t runtime_path[MAX_PATH] = {};
    char    hsa_tools_lib[32768]   = {};
    ::GetModuleFileNameW(
        nullptr, executable_path, sizeof(executable_path) / sizeof(executable_path[0]));
    std::wstring resource_path{executable_path};
    resource_path.resize(resource_path.find_last_of(L"\\/") + 1);
    resource_path += L"target-resource.txt";
    FILE* resource = nullptr;
    ::_wfopen_s(&resource, resource_path.c_str(), L"rb");
    char resource_contents[64] = {};
    if(resource == nullptr ||
       std::fgets(resource_contents, sizeof(resource_contents), resource) == nullptr ||
       std::strcmp(resource_contents, "rocprofv3-target-resource\n") != 0)
    {
        if(resource != nullptr) std::fclose(resource);
        std::fprintf(stderr, "application-local target resource is unavailable\n");
        return 3;
    }
    std::fclose(resource);
    const auto runtime_module = ::GetModuleHandleW(L"amdhip64_7.dll");
    if(runtime_module)
        ::GetModuleFileNameW(runtime_module, runtime_path, MAX_PATH);
    ::GetEnvironmentVariableA("HSA_TOOLS_LIB", hsa_tools_lib, sizeof(hsa_tools_lib));
    std::wprintf(L"executable=%ls runtime=%ls resource=loaded HSA_TOOLS_LIB=%hs\n",
                 executable_path,
                 runtime_path,
                 (hsa_tools_lib[0] != '\0') ? hsa_tools_lib : "<unset>");
#endif

    const auto hsa_status = hsa_init();
    if(hsa_status != HSA_STATUS_SUCCESS)
    {
        std::fprintf(stderr, "hsa_init returned 0x%x\n", static_cast<unsigned>(hsa_status));
        return 1;
    }

    auto properties = hipDeviceProp_t{};
    CHECK_HIP(hipGetDeviceProperties(&properties, 0));
    CHECK_HIP(hipSetDevice(0));

    hipStream_t work_stream = nullptr;
    if(device_reset)
    {
        if(properties.multiProcessorCount <= 0)
        {
            std::fprintf(stderr, "device reported no logical compute units\n");
            return 1;
        }
        auto cu_mask = std::vector<uint32_t>(
            (static_cast<uint32_t>(properties.multiProcessorCount) + 31U) / 32U, ~uint32_t{0});
        const auto remainder = static_cast<uint32_t>(properties.multiProcessorCount) % 32U;
        if(remainder != 0) cu_mask.back() = (uint32_t{1} << remainder) - 1U;
        CHECK_HIP(hipExtStreamCreateWithCUMask(
            &work_stream, static_cast<uint32_t>(cu_mask.size()), cu_mask.data()));
    }

    auto lhs    = std::vector<float>(work_item_count);
    auto rhs    = std::vector<float>(work_item_count);
    auto result = std::vector<float>(work_item_count, 0.0F);
    for(size_t i = 0; i < work_item_count; ++i)
    {
        lhs.at(i) = static_cast<float>(i % 1024);
        rhs.at(i) = static_cast<float>((i * 3) % 2048);
    }

    if(marker_mode)
    {
        roctxMarkA("hip workload begin");
        if(roctxRangePushA("hip workload") != 0) return 1;
    }

    float* device_lhs    = nullptr;
    float* device_rhs    = nullptr;
    float* device_result = nullptr;
    const auto byte_count = work_item_count * sizeof(float);
    CHECK_HIP(hipMalloc(&device_lhs, byte_count));
    CHECK_HIP(hipMalloc(&device_rhs, byte_count));
    CHECK_HIP(hipMalloc(&device_result, byte_count));
    if(work_stream != nullptr)
    {
        CHECK_HIP(hipMemcpyAsync(
            device_lhs, lhs.data(), byte_count, hipMemcpyHostToDevice, work_stream));
        CHECK_HIP(hipMemcpyAsync(
            device_rhs, rhs.data(), byte_count, hipMemcpyHostToDevice, work_stream));
    }
    else
    {
        CHECK_HIP(hipMemcpy(device_lhs, lhs.data(), byte_count, hipMemcpyHostToDevice));
        CHECK_HIP(hipMemcpy(device_rhs, rhs.data(), byte_count, hipMemcpyHostToDevice));
    }

    const auto grid_size = static_cast<uint32_t>(work_item_count / workgroup_size);
    hipGraph_t     graph       = nullptr;
    hipGraphExec_t graph_exec  = nullptr;
    auto           kernel_size = work_item_count;
    if(selected_regions && roctxProfilerResume(0) != 0) return 1;
    if(nested_selected_regions && roctxProfilerResume(0) != 0) return 1;
    const auto marker_range = marker_mode ? roctxRangeStartA("hip dispatches") : 0;
    if(marker_mode && marker_range == 0) return 1;
    if(graph_mode)
    {
        CHECK_HIP(hipGraphCreate(&graph, 0));
        auto kernel_args =
            std::vector<void*>{&device_lhs, &device_rhs, &device_result, &kernel_size};
        auto params             = hipKernelNodeParams{};
        params.func             = reinterpret_cast<void*>(vector_add);
        params.gridDim          = dim3(grid_size);
        params.blockDim         = dim3(workgroup_size);
        params.sharedMemBytes   = 0;
        params.kernelParams     = kernel_args.data();
        params.extra            = nullptr;
        hipGraphNode_t kernel_node = nullptr;
        CHECK_HIP(hipGraphAddKernelNode(&kernel_node, graph, nullptr, 0, &params));
        CHECK_HIP(hipGraphInstantiate(&graph_exec, graph, nullptr, nullptr, 0));
        for(uint32_t dispatch = 0; dispatch < dispatch_count; ++dispatch)
        {
            CHECK_HIP(hipGraphLaunch(graph_exec, work_stream));
            if(nested_selected_regions &&
               dispatch + 1 == ((dispatch_count > 1) ? dispatch_count / 2 : 1))
            {
                CHECK_HIP(hipDeviceSynchronize());
                if(roctxProfilerPause(0) != 0) return 1;
            }
        }
    }
    else
    {
        for(uint32_t dispatch = 0; dispatch < dispatch_count; ++dispatch)
        {
            hipLaunchKernelGGL(vector_add,
                               dim3(grid_size),
                               dim3(workgroup_size),
                               0,
                               work_stream,
                               device_lhs,
                               device_rhs,
                               device_result,
                               work_item_count);
            CHECK_HIP(hipGetLastError());
            if(nested_selected_regions &&
               dispatch + 1 == ((dispatch_count > 1) ? dispatch_count / 2 : 1))
            {
                CHECK_HIP(hipDeviceSynchronize());
                if(roctxProfilerPause(0) != 0) return 1;
            }
        }
    }

    if(work_stream != nullptr)
    {
        CHECK_HIP(hipMemcpyAsync(
            result.data(), device_result, byte_count, hipMemcpyDeviceToHost, work_stream));
        CHECK_HIP(hipStreamSynchronize(work_stream));
    }
    else
    {
        CHECK_HIP(hipDeviceSynchronize());
        CHECK_HIP(hipMemcpy(result.data(), device_result, byte_count, hipMemcpyDeviceToHost));
    }

    if(selected_regions && roctxProfilerPause(0) != 0) return 1;

    for(size_t i = 0; i < work_item_count; ++i)
    {
        const auto expected   = lhs.at(i) + rhs.at(i);
        const auto difference = result.at(i) - expected;
        if(difference < -0.001F || difference > 0.001F)
        {
            std::fprintf(stderr,
                         "validation failure at %zu: expected %.3f, observed %.3f\n",
                         i,
                         expected,
                         result.at(i));
            return 1;
        }
    }

    if(marker_mode)
    {
        roctxRangeStop(marker_range);
        if(roctxRangePop() != 0) return 1;
        roctxMarkA("hip workload end");
    }

    if(graph_exec != nullptr) CHECK_HIP(hipGraphExecDestroy(graph_exec));
    if(graph != nullptr) CHECK_HIP(hipGraphDestroy(graph));
    if(work_stream != nullptr) CHECK_HIP(hipStreamDestroy(work_stream));
    CHECK_HIP(hipFree(device_result));
    CHECK_HIP(hipFree(device_rhs));
    CHECK_HIP(hipFree(device_lhs));
    if(device_reset) CHECK_HIP(hipDeviceReset());

    if(!create_file.empty())
    {
        auto* output = static_cast<FILE*>(nullptr);
#if defined(_WIN32)
        ::fopen_s(&output, create_file.c_str(), "wb");
#else
        output = std::fopen(create_file.c_str(), "wb");
#endif
        if(!output)
        {
            std::fprintf(stderr, "could not create requested test file: %s\n", create_file.c_str());
            return 4;
        }
        const auto write_status = std::fputs("target-created\n", output);
        const auto close_status = std::fclose(output);
        if(write_status < 0 || close_status != 0)
        {
            std::fprintf(stderr, "could not create requested test file: %s\n", create_file.c_str());
            return 4;
        }
    }

    std::printf("device=%s architecture=%s dispatches=%u work_items=%zu workgroup_size=%u "
                "kernel=vector_add execution=%s markers=%s queue_policy=%s validation=passed\n",
                properties.name,
                properties.gcnArchName,
                dispatch_count,
                work_item_count,
                workgroup_size,
                graph_mode ? "graph" : "direct",
                marker_mode ? "enabled" : "disabled",
                device_reset ? "full-device-cu-mask" : "default");
    return 0;
}
