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

#include <rocprofiler-sdk/registration.h>

#include <atomic>
#include <iostream>

namespace
{
struct callback_state
{
    std::atomic<int> initialize_count{0};
    std::atomic<int> finalize_count{0};
    std::atomic<int> finalize_status{-2};
    const rocprofiler_client_id_t* client_id = nullptr;
};

callback_state state{};

void
tool_finalize(void* data)
{
    auto* value = static_cast<callback_state*>(data);
    value->finalize_count.fetch_add(1, std::memory_order_relaxed);
    auto status = int{-2};
    if(rocprofiler_is_finalized(&status) == ROCPROFILER_STATUS_SUCCESS)
        value->finalize_status.store(status, std::memory_order_release);
}

int
tool_initialize(rocprofiler_client_finalize_t finalize_func, void* data)
{
    auto* value = static_cast<callback_state*>(data);
    value->initialize_count.fetch_add(1, std::memory_order_relaxed);
    finalize_func(*value->client_id);
    return 0;
}

rocprofiler_tool_configure_result_t*
tool_configure(uint32_t,
               const char*,
               uint32_t,
               rocprofiler_client_id_t* client_id)
{
    client_id->name = "windows-registration-reentrancy";
    state.client_id = client_id;
    static auto result = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t),
                                                              &tool_initialize,
                                                              &tool_finalize,
                                                              &state};
    return &result;
}
}  // namespace

int
main()
{
    const auto result = rocprofiler_force_configure(&tool_configure);
    if(result != ROCPROFILER_STATUS_SUCCESS)
    {
        std::cerr << "force configure failed with status " << static_cast<int>(result) << '\n';
        return 1;
    }

    auto initialized = int{-2};
    auto finalized   = int{-2};
    if(rocprofiler_is_initialized(&initialized) != ROCPROFILER_STATUS_SUCCESS ||
       rocprofiler_is_finalized(&finalized) != ROCPROFILER_STATUS_SUCCESS)
    {
        std::cerr << "lifecycle query failed\n";
        return 2;
    }

    if(state.initialize_count.load(std::memory_order_acquire) != 1 ||
       state.finalize_count.load(std::memory_order_acquire) != 1 ||
       state.finalize_status.load(std::memory_order_acquire) != -1 || initialized != 1 ||
       finalized != 1)
    {
        std::cerr << "unexpected lifecycle state: initialize_count="
                  << state.initialize_count.load() << " finalize_count="
                  << state.finalize_count.load() << " callback_finalize_status="
                  << state.finalize_status.load() << " initialized=" << initialized
                  << " finalized=" << finalized << '\n';
        return 3;
    }

    std::cout << "windows_registration_reentrancy=passed initialize=1 finalize=1\n";
    return 0;
}
