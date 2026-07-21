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
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <rocprofiler-sdk/registration.h>

#if !ROCPROFILER_SDK_WINDOWS_MINIMAL_API
#    error Native Windows clients must expose the Windows-minimal feature boundary
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

extern "C" int
rocprofiler_set_api_table(const char*, uint64_t, uint64_t, void**, uint64_t);

namespace
{
std::mutex              state_mutex{};
std::condition_variable state_condition{};
bool                    initialize_entered = false;
bool                    allow_initialize   = false;
std::atomic<int>        initialize_count{0};
std::atomic<int>        reentrant_status{-1};

int
tool_initialize(rocprofiler_client_finalize_t, void*)
{
    initialize_count.fetch_add(1, std::memory_order_relaxed);
    auto table = uint64_t{0};
    auto ptr   = static_cast<void*>(&table);
    reentrant_status.store(rocprofiler_set_api_table("hip_compiler", 1, 0, &ptr, 1),
                           std::memory_order_release);
    auto lock         = std::unique_lock<std::mutex>{state_mutex};
    initialize_entered = true;
    state_condition.notify_all();
    state_condition.wait(lock, []() { return allow_initialize; });
    return 0;
}

rocprofiler_tool_configure_result_t*
tool_configure(uint32_t, const char*, uint32_t, rocprofiler_client_id_t* client_id)
{
    client_id->name = "windows-concurrent-registration";
    static auto result = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t), &tool_initialize, nullptr, nullptr};
    return &result;
}
}  // namespace

int
main()
{
    auto force_status = std::atomic<int>{-1};
    auto table_status = std::atomic<int>{-1};
    auto table_done   = std::atomic<bool>{false};

    auto force_thread = std::thread{[&]() {
        force_status.store(static_cast<int>(rocprofiler_force_configure(&tool_configure)),
                           std::memory_order_release);
    }};

    {
        auto lock = std::unique_lock<std::mutex>{state_mutex};
        if(!state_condition.wait_for(
               lock, std::chrono::seconds{5}, []() { return initialize_entered; }))
        {
            std::cerr << "tool initializer did not start\n";
            allow_initialize = true;
            lock.unlock();
            state_condition.notify_all();
            force_thread.join();
            return 1;
        }
    }

    auto table_thread = std::thread{[&]() {
        auto table = uint64_t{0};
        auto ptr   = static_cast<void*>(&table);
        table_status.store(rocprofiler_set_api_table("hip_tools", 1, 0, &ptr, 1),
                           std::memory_order_release);
        table_done.store(true, std::memory_order_release);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto returned_early = table_done.load(std::memory_order_acquire);
    {
        auto lock        = std::lock_guard<std::mutex>{state_mutex};
        allow_initialize = true;
    }
    state_condition.notify_all();
    force_thread.join();
    table_thread.join();
    if(returned_early)
    {
        std::cerr << "API-table registration observed partial initialization\n";
        return 2;
    }

    if(force_status.load(std::memory_order_acquire) != ROCPROFILER_STATUS_SUCCESS ||
       table_status.load(std::memory_order_acquire) != 0 ||
       reentrant_status.load(std::memory_order_acquire) != 0 ||
       initialize_count.load(std::memory_order_acquire) != 1)
    {
        std::cerr << "unexpected concurrent registration result: force=" << force_status.load()
                  << " table=" << table_status.load()
                  << " reentrant=" << reentrant_status.load()
                  << " initialize=" << initialize_count.load() << '\n';
        return 3;
    }

    std::cout << "windows_registration_concurrent=passed initialize=1 waited=yes\n";
    return 0;
}
