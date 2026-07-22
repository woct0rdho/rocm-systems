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

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
constexpr auto element_count  = size_t{1024} * 1024;
constexpr auto workgroup_size = uint32_t{256};
constexpr auto dispatch_count = uint32_t{6};

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

int
parse_exit_code(const char* value)
{
    errno     = 0;
    char* end = nullptr;
    auto code = std::strtol(value, &end, 10);
    if(errno != 0 || end == value || *end != '\0' || code < 1 || code > 255) return -1;
    return static_cast<int>(code);
}
}  // namespace

__global__ void
dispatch_vector(const float* input, float* output, size_t size, uint32_t rounds, float bias)
{
    const auto index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if(index >= size) return;

    auto value = input[index] + bias;
    for(uint32_t round = 0; round < rounds; ++round)
        value = value * 1.00000011920928955078125F + static_cast<float>(round & 3U) * 0.00001F;
    output[index] = value;
}

__global__ void
dispatch_lds_conflict(const float* input, float* output, size_t size, uint32_t rounds)
{
    __shared__ volatile float banked[2048];
    const auto local  = static_cast<uint32_t>(threadIdx.x);
    const auto global = static_cast<size_t>(blockIdx.x) * blockDim.x + local;
    const auto slot   = (local & 31U) * 2U;
    auto       value  = (global < size) ? input[global] : 0.0F;

    for(uint32_t round = 0; round < rounds; ++round)
    {
        banked[slot] = value + static_cast<float>(round);
        __syncthreads();
        value += banked[slot] * 0.000001F;
        __syncthreads();
    }
    if(global < size) output[global] = value;
}

__global__ void
dispatch_resource(const float* input, float* output, size_t size)
{
    __shared__ float shared_values[1024];
    const auto local  = static_cast<uint32_t>(threadIdx.x);
    const auto global = static_cast<size_t>(blockIdx.x) * blockDim.x + local;
    float      values[16] = {};
    const auto seed       = (global < size) ? input[global] : 0.0F;
    for(uint32_t index = 0; index < 16; ++index)
        values[index] = seed + static_cast<float>(index) * 0.125F;
    shared_values[local] = values[(local >> 4U) & 15U];
    __syncthreads();
    if(global < size)
        output[global] = values[local & 15U] + shared_values[(local + 17U) & 255U];
}

int
main(int argc, char** argv)
{
    auto no_dispatch        = false;
    auto reverse_completion = false;
    auto requested_exit     = 0;
    for(int index = 1; index < argc; ++index)
    {
        if(std::strcmp(argv[index], "--no-dispatch") == 0)
        {
            no_dispatch = true;
            continue;
        }
        if(std::strcmp(argv[index], "--reverse-completion") == 0)
        {
            reverse_completion = true;
            continue;
        }
        if(std::strcmp(argv[index], "--exit-code") == 0 && index + 1 < argc)
        {
            requested_exit = parse_exit_code(argv[++index]);
            if(requested_exit > 0) continue;
        }
        std::fprintf(stderr,
                     "usage: %s [--no-dispatch] [--reverse-completion] [--exit-code 1..255]\n",
                     argv[0]);
        return 2;
    }

    auto properties = hipDeviceProp_t{};
    CHECK_HIP(hipGetDeviceProperties(&properties, 0));
    CHECK_HIP(hipSetDevice(0));

    if(no_dispatch)
    {
        std::printf("dispatch_analysis=passed architecture=%s dispatches=0 enqueue=none\n",
                    properties.gcnArchName);
        return requested_exit;
    }

    auto input = std::vector<float>(element_count);
    auto output = std::vector<float>(element_count, 0.0F);
    for(size_t index = 0; index < input.size(); ++index)
        input[index] = static_cast<float>((index % 1024U) + 1U) * 0.03125F;

    float* device_input  = nullptr;
    float* device_vector = nullptr;
    float* device_lds    = nullptr;
    float* device_resource = nullptr;
    const auto byte_count = input.size() * sizeof(float);
    CHECK_HIP(hipMalloc(&device_input, byte_count));
    CHECK_HIP(hipMalloc(&device_vector, byte_count));
    CHECK_HIP(hipMalloc(&device_lds, byte_count));
    CHECK_HIP(hipMalloc(&device_resource, byte_count));
    CHECK_HIP(hipMemcpy(device_input, input.data(), byte_count, hipMemcpyHostToDevice));

    hipStream_t streams[2] = {nullptr, nullptr};
    CHECK_HIP(hipStreamCreateWithFlags(&streams[0], hipStreamNonBlocking));
    CHECK_HIP(hipStreamCreateWithFlags(&streams[1], hipStreamNonBlocking));

    const auto grid = dim3(static_cast<uint32_t>(element_count / workgroup_size));
    const auto slow_rounds = reverse_completion ? uint32_t{128} : uint32_t{32};
    const auto lds_rounds  = reverse_completion ? uint32_t{8} : uint32_t{24};

    hipLaunchKernelGGL(dispatch_vector,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[0],
                       device_input,
                       device_vector,
                       element_count,
                       slow_rounds,
                       1.0F);
    CHECK_HIP(hipGetLastError());
    hipLaunchKernelGGL(dispatch_lds_conflict,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[1],
                       device_input,
                       device_lds,
                       element_count,
                       lds_rounds);
    CHECK_HIP(hipGetLastError());
    hipLaunchKernelGGL(dispatch_vector,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[0],
                       device_input,
                       device_vector,
                       element_count,
                       slow_rounds,
                       2.0F);
    CHECK_HIP(hipGetLastError());
    hipLaunchKernelGGL(dispatch_lds_conflict,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[1],
                       device_input,
                       device_lds,
                       element_count,
                       lds_rounds);
    CHECK_HIP(hipGetLastError());
    hipLaunchKernelGGL(dispatch_vector,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[0],
                       device_input,
                       device_vector,
                       element_count,
                       slow_rounds,
                       3.0F);
    CHECK_HIP(hipGetLastError());
    hipLaunchKernelGGL(dispatch_resource,
                       grid,
                       dim3(workgroup_size),
                       0,
                       streams[1],
                       device_input,
                       device_resource,
                       element_count);
    CHECK_HIP(hipGetLastError());

    if(reverse_completion)
    {
        CHECK_HIP(hipStreamSynchronize(streams[1]));
        CHECK_HIP(hipStreamSynchronize(streams[0]));
    }
    else
    {
        CHECK_HIP(hipStreamSynchronize(streams[0]));
        CHECK_HIP(hipStreamSynchronize(streams[1]));
    }
    CHECK_HIP(hipMemcpy(output.data(), device_vector, byte_count, hipMemcpyDeviceToHost));

    if(output.front() == 0.0F || output.back() == 0.0F)
    {
        std::fprintf(stderr, "dispatch analysis output validation failed\n");
        return 1;
    }

    CHECK_HIP(hipStreamDestroy(streams[1]));
    CHECK_HIP(hipStreamDestroy(streams[0]));
    CHECK_HIP(hipFree(device_resource));
    CHECK_HIP(hipFree(device_lds));
    CHECK_HIP(hipFree(device_vector));
    CHECK_HIP(hipFree(device_input));

    std::printf(
        "dispatch_analysis=passed architecture=%s dispatches=%u streams=2 "
        "enqueue=1:dispatch_vector@0,2:dispatch_lds_conflict@1,3:dispatch_vector@0,"
        "4:dispatch_lds_conflict@1,5:dispatch_vector@0,6:dispatch_resource@1 "
        "completion_policy=%s\n",
        properties.gcnArchName,
        dispatch_count,
        reverse_completion ? "short-stream-first" : "stream-order");
    return requested_exit;
}
