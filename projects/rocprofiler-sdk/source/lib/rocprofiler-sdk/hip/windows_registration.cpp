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

#include "lib/rocprofiler-sdk/hip/windows_registration.hpp"
#include "lib/rocprofiler-sdk/windows_trace.hpp"

#include <hip/hip_runtime_api.h>
#include <hip/hip_deprecated.h>
#include <hip/hip_gl_interop.h>
#include <hip/amd_detail/hip_api_trace.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace rocprofiler
{
namespace hip
{
namespace windows
{
namespace
{
template <typename Tp>
using atomic_function_t = std::atomic<Tp>;

atomic_function_t<t_hipApiName>           original_hip_api_name{nullptr};
atomic_function_t<t_hipDeviceSynchronize> original_hip_device_synchronize{nullptr};
atomic_function_t<t_hipFree>              original_hip_free{nullptr};
atomic_function_t<t_hipGraphAddKernelNode> original_hip_graph_add_kernel_node{nullptr};
atomic_function_t<t_hipGraphCreate>        original_hip_graph_create{nullptr};
atomic_function_t<t_hipGraphDestroy>       original_hip_graph_destroy{nullptr};
atomic_function_t<t_hipGraphExecDestroy>   original_hip_graph_exec_destroy{nullptr};
atomic_function_t<t_hipGraphInstantiate>   original_hip_graph_instantiate{nullptr};
atomic_function_t<t_hipGraphLaunch>        original_hip_graph_launch{nullptr};
atomic_function_t<t_hipLaunchKernel>       original_hip_launch_kernel{nullptr};
atomic_function_t<t_hipMalloc>             original_hip_malloc{nullptr};
atomic_function_t<t_hipMemcpy>             original_hip_memcpy{nullptr};
atomic_function_t<t_hipMemcpyAsync>        original_hip_memcpy_async{nullptr};
atomic_function_t<t_hipStreamSynchronize>  original_hip_stream_synchronize{nullptr};

std::mutex graph_mutex = {};
std::unordered_map<hipGraph_t, uint64_t>     graph_kernel_counts = {};
std::unordered_map<hipGraphExec_t, uint64_t> graph_exec_kernel_counts = {};

uint64_t
timestamp_ns()
{
    return ::rocprofiler::windows::trace::timestamp_ns();
}

void
append_trace(const char* message)
{
    ::rocprofiler::windows::trace::append(message);
}

struct api_scope
{
    const char* operation      = nullptr;
    uint64_t    correlation_id = 0;
    uint64_t    start_ns       = 0;
    uint32_t    process_id     = 0;
    uint32_t    thread_id      = 0;

    explicit api_scope(const char* value, const char* arguments = nullptr)
    : operation{value}
    , correlation_id{::rocprofiler::windows::trace::next_correlation_id()}
    , start_ns{timestamp_ns()}
    , process_id{::rocprofiler::windows::trace::process_id()}
    , thread_id{::rocprofiler::windows::trace::thread_id()}
    {
        char message[640] = {};
        std::snprintf(message,
                      sizeof(message),
                      "event=hip_api phase=enter operation=%s correlation_id=%llu "
                      "process_id=%u thread_id=%u timestamp_ns=%llu%s%s\n",
                      operation,
                      static_cast<unsigned long long>(correlation_id),
                      process_id,
                      thread_id,
                      static_cast<unsigned long long>(start_ns),
                      (arguments && *arguments) ? " " : "",
                      (arguments && *arguments) ? arguments : "");
        append_trace(message);
    }

    void complete(hipError_t status) const
    {
        char message[480] = {};
        std::snprintf(message,
                      sizeof(message),
                      "event=hip_api phase=exit operation=%s correlation_id=%llu "
                      "process_id=%u thread_id=%u timestamp_ns=%llu status=%d\n",
                      operation,
                      static_cast<unsigned long long>(correlation_id),
                      process_id,
                      thread_id,
                      static_cast<unsigned long long>(timestamp_ns()),
                      static_cast<int>(status));
        append_trace(message);
    }
};

template <typename Tp>
Tp
load_original(const atomic_function_t<Tp>& value)
{
    return value.load(std::memory_order_acquire);
}

const char*
hip_api_name_wrapper(uint32_t id)
{
    char arguments[64] = {};
    std::snprintf(arguments, sizeof(arguments), "operation_id=%u", id);
    const auto scope    = api_scope{"hipApiName", arguments};
    const auto original = load_original(original_hip_api_name);
    const auto* result  = (original) ? original(id) : nullptr;
    scope.complete(result ? hipSuccess : hipErrorUnknown);
    return result;
}

hipError_t
hip_device_synchronize_wrapper()
{
    const auto scope    = api_scope{"hipDeviceSynchronize"};
    const auto original = load_original(original_hip_device_synchronize);
    const auto status   = original ? original() : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_free_wrapper(void* ptr)
{
    char arguments[96] = {};
    std::snprintf(arguments, sizeof(arguments), "pointer=%p", ptr);
    const auto scope    = api_scope{"hipFree", arguments};
    const auto original = load_original(original_hip_free);
    const auto status   = original ? original(ptr) : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_add_kernel_node_wrapper(hipGraphNode_t*           node,
                                  hipGraph_t                graph,
                                  const hipGraphNode_t*     dependencies,
                                  size_t                    num_dependencies,
                                  const hipKernelNodeParams* params)
{
    char arguments[160] = {};
    std::snprintf(arguments,
                  sizeof(arguments),
                  "graph=%p dependencies=%llu",
                  static_cast<void*>(graph),
                  static_cast<unsigned long long>(num_dependencies));
    const auto scope    = api_scope{"hipGraphAddKernelNode", arguments};
    const auto original = load_original(original_hip_graph_add_kernel_node);
    const auto status = original
                            ? original(node, graph, dependencies, num_dependencies, params)
                            : hipErrorUnknown;
    if(status == hipSuccess)
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        ++graph_kernel_counts[graph];
    }
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_create_wrapper(hipGraph_t* graph, unsigned int flags)
{
    char arguments[96] = {};
    std::snprintf(arguments, sizeof(arguments), "flags=%u", flags);
    const auto scope    = api_scope{"hipGraphCreate", arguments};
    const auto original = load_original(original_hip_graph_create);
    const auto status   = original ? original(graph, flags) : hipErrorUnknown;
    if(status == hipSuccess && graph && *graph)
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        graph_kernel_counts[*graph] = 0;
    }
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_destroy_wrapper(hipGraph_t graph)
{
    char arguments[96] = {};
    std::snprintf(arguments, sizeof(arguments), "graph=%p", static_cast<void*>(graph));
    const auto scope    = api_scope{"hipGraphDestroy", arguments};
    const auto original = load_original(original_hip_graph_destroy);
    const auto status   = original ? original(graph) : hipErrorUnknown;
    if(status == hipSuccess)
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        graph_kernel_counts.erase(graph);
    }
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_exec_destroy_wrapper(hipGraphExec_t graph_exec)
{
    char arguments[112] = {};
    std::snprintf(
        arguments, sizeof(arguments), "graph_exec=%p", static_cast<void*>(graph_exec));
    const auto scope    = api_scope{"hipGraphExecDestroy", arguments};
    const auto original = load_original(original_hip_graph_exec_destroy);
    const auto status   = original ? original(graph_exec) : hipErrorUnknown;
    if(status == hipSuccess)
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        graph_exec_kernel_counts.erase(graph_exec);
    }
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_instantiate_wrapper(hipGraphExec_t* graph_exec,
                              hipGraph_t      graph,
                              hipGraphNode_t* error_node,
                              char*           log_buffer,
                              size_t          buffer_size)
{
    char arguments[128] = {};
    std::snprintf(arguments, sizeof(arguments), "graph=%p", static_cast<void*>(graph));
    const auto scope    = api_scope{"hipGraphInstantiate", arguments};
    const auto original = load_original(original_hip_graph_instantiate);
    const auto status = original
                            ? original(graph_exec, graph, error_node, log_buffer, buffer_size)
                            : hipErrorUnknown;
    if(status == hipSuccess && graph_exec && *graph_exec)
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        graph_exec_kernel_counts[*graph_exec] = graph_kernel_counts[graph];
    }
    scope.complete(status);
    return status;
}

hipError_t
hip_graph_launch_wrapper(hipGraphExec_t graph_exec, hipStream_t stream)
{
    char arguments[160] = {};
    std::snprintf(arguments,
                  sizeof(arguments),
                  "graph_exec=%p stream=%p",
                  static_cast<void*>(graph_exec),
                  static_cast<void*>(stream));
    const auto scope    = api_scope{"hipGraphLaunch", arguments};
    const auto original = load_original(original_hip_graph_launch);
    const auto status   = original ? original(graph_exec, stream) : hipErrorUnknown;

    auto kernel_count = uint64_t{0};
    {
        std::lock_guard<std::mutex> lock{graph_mutex};
        if(const auto itr = graph_exec_kernel_counts.find(graph_exec);
           itr != graph_exec_kernel_counts.end())
            kernel_count = itr->second;
    }
    char message[480] = {};
    std::snprintf(message,
                  sizeof(message),
                  "event=hip_graph phase=launch graph_exec_id=%p kernel_dispatch_count=%llu "
                  "correlation_id=%llu process_id=%u thread_id=%u timestamp_ns=%llu status=%d\n",
                  static_cast<void*>(graph_exec),
                  static_cast<unsigned long long>(kernel_count),
                  static_cast<unsigned long long>(scope.correlation_id),
                  scope.process_id,
                  scope.thread_id,
                  static_cast<unsigned long long>(timestamp_ns()),
                  static_cast<int>(status));
    append_trace(message);
    scope.complete(status);
    return status;
}

hipError_t
hip_launch_kernel_wrapper(const void* function_address,
                          dim3        num_blocks,
                          dim3        dim_blocks,
                          void**      args,
                          size_t      shared_memory_bytes,
                          hipStream_t stream)
{
    char arguments[320] = {};
    std::snprintf(arguments,
                  sizeof(arguments),
                  "function=%p grid=%u,%u,%u workgroup=%u,%u,%u shared_memory=%llu stream=%p",
                  function_address,
                  num_blocks.x,
                  num_blocks.y,
                  num_blocks.z,
                  dim_blocks.x,
                  dim_blocks.y,
                  dim_blocks.z,
                  static_cast<unsigned long long>(shared_memory_bytes),
                  static_cast<void*>(stream));
    const auto scope    = api_scope{"hipLaunchKernel", arguments};
    const auto original = load_original(original_hip_launch_kernel);
    const auto status = original ? original(function_address,
                                            num_blocks,
                                            dim_blocks,
                                            args,
                                            shared_memory_bytes,
                                            stream)
                                 : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_malloc_wrapper(void** ptr, size_t size)
{
    char arguments[96] = {};
    std::snprintf(arguments, sizeof(arguments), "bytes=%llu", static_cast<unsigned long long>(size));
    const auto scope    = api_scope{"hipMalloc", arguments};
    const auto original = load_original(original_hip_malloc);
    const auto status   = original ? original(ptr, size) : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_memcpy_wrapper(void* dst, const void* src, size_t size, hipMemcpyKind kind)
{
    char arguments[192] = {};
    std::snprintf(arguments,
                  sizeof(arguments),
                  "destination=%p source=%p bytes=%llu kind=%d",
                  dst,
                  src,
                  static_cast<unsigned long long>(size),
                  static_cast<int>(kind));
    const auto scope    = api_scope{"hipMemcpy", arguments};
    const auto original = load_original(original_hip_memcpy);
    const auto status   = original ? original(dst, src, size, kind) : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_memcpy_async_wrapper(void*         dst,
                         const void*   src,
                         size_t        size,
                         hipMemcpyKind kind,
                         hipStream_t   stream)
{
    char arguments[224] = {};
    std::snprintf(arguments,
                  sizeof(arguments),
                  "destination=%p source=%p bytes=%llu kind=%d stream=%p",
                  dst,
                  src,
                  static_cast<unsigned long long>(size),
                  static_cast<int>(kind),
                  static_cast<void*>(stream));
    const auto scope    = api_scope{"hipMemcpyAsync", arguments};
    const auto original = load_original(original_hip_memcpy_async);
    const auto status = original ? original(dst, src, size, kind, stream) : hipErrorUnknown;
    scope.complete(status);
    return status;
}

hipError_t
hip_stream_synchronize_wrapper(hipStream_t stream)
{
    char arguments[96] = {};
    std::snprintf(arguments, sizeof(arguments), "stream=%p", static_cast<void*>(stream));
    const auto scope    = api_scope{"hipStreamSynchronize", arguments};
    const auto original = load_original(original_hip_stream_synchronize);
    const auto status   = original ? original(stream) : hipErrorUnknown;
    scope.complete(status);
    return status;
}

template <typename Tp>
bool
table_has(const HipDispatchTable* table, Tp HipDispatchTable::* member)
{
    const auto* begin = reinterpret_cast<const std::byte*>(table);
    const auto* field = reinterpret_cast<const std::byte*>(&(table->*member));
    return table->size >= static_cast<size_t>((field - begin) + sizeof(table->*member));
}

#define ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(TABLE, FIELD, ORIGINAL, WRAPPER)                    \
    do                                                                                              \
    {                                                                                               \
        if(table_has((TABLE), &HipDispatchTable::FIELD) && (TABLE)->FIELD &&                        \
           (TABLE)->FIELD != &(WRAPPER))                                                             \
        {                                                                                           \
            (ORIGINAL).store((TABLE)->FIELD, std::memory_order_release);                            \
            (TABLE)->FIELD = &(WRAPPER);                                                            \
        }                                                                                           \
    } while(false)
}  // namespace

bool
set_api_table(void* value)
{
    auto* table = static_cast<HipDispatchTable*>(value);
    if(!table || table->size < sizeof(size_t) + sizeof(t_hipApiName) || !table->hipApiName_fn)
        return false;

    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipApiName_fn, original_hip_api_name, hip_api_name_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table,
                                            hipDeviceSynchronize_fn,
                                            original_hip_device_synchronize,
                                            hip_device_synchronize_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table, hipFree_fn, original_hip_free, hip_free_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table,
                                            hipGraphAddKernelNode_fn,
                                            original_hip_graph_add_kernel_node,
                                            hip_graph_add_kernel_node_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipGraphCreate_fn, original_hip_graph_create, hip_graph_create_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipGraphDestroy_fn, original_hip_graph_destroy, hip_graph_destroy_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table,
                                            hipGraphExecDestroy_fn,
                                            original_hip_graph_exec_destroy,
                                            hip_graph_exec_destroy_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table,
                                            hipGraphInstantiate_fn,
                                            original_hip_graph_instantiate,
                                            hip_graph_instantiate_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipGraphLaunch_fn, original_hip_graph_launch, hip_graph_launch_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipLaunchKernel_fn, original_hip_launch_kernel, hip_launch_kernel_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipMalloc_fn, original_hip_malloc, hip_malloc_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipMemcpy_fn, original_hip_memcpy, hip_memcpy_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(
        table, hipMemcpyAsync_fn, original_hip_memcpy_async, hip_memcpy_async_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_HIP_WRAPPER(table,
                                            hipStreamSynchronize_fn,
                                            original_hip_stream_synchronize,
                                            hip_stream_synchronize_wrapper);
    return table->hipApiName_fn == &hip_api_name_wrapper;
}
}  // namespace windows
}  // namespace hip
}  // namespace rocprofiler
