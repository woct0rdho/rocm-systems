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

#include "lib/rocprofiler-sdk/roctx/windows_registration.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/windows_trace.hpp"

#include <rocprofiler-sdk-roctx/api_trace.h>

#include <fmt/format.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rocprofiler
{
namespace roctx
{
namespace windows
{
namespace
{
template <typename Tp>
using atomic_function_t = std::atomic<Tp>;

atomic_function_t<roctxMarkA_fn_t>       original_mark{nullptr};
atomic_function_t<roctxRangePushA_fn_t>  original_range_push{nullptr};
atomic_function_t<roctxRangePop_fn_t>    original_range_pop{nullptr};
atomic_function_t<roctxRangeStartA_fn_t>   original_range_start{nullptr};
atomic_function_t<roctxRangeStop_fn_t>     original_range_stop{nullptr};
atomic_function_t<roctxProfilerPause_fn_t> original_profiler_pause{nullptr};
atomic_function_t<roctxProfilerResume_fn_t> original_profiler_resume{nullptr};

struct range_state
{
    uint64_t    correlation_id = 0;
    uint64_t    start_ns       = 0;
    uint32_t    process_id     = 0;
    uint32_t    thread_id      = 0;
    std::string message_hex    = {};
};

thread_local std::vector<range_state> nested_ranges = {};
std::mutex                            range_mutex    = {};
std::unordered_map<roctx_range_id_t, range_state> process_ranges = {};

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
    , start_ns{::rocprofiler::windows::trace::timestamp_ns()}
    , process_id{::rocprofiler::windows::trace::process_id()}
    , thread_id{::rocprofiler::windows::trace::thread_id()}
    {
        const auto message = fmt::format(
            "event=roctx_api phase=enter operation={} correlation_id={} "
            "process_id={} thread_id={} timestamp_ns={}{}{}\n",
            operation,
            correlation_id,
            process_id,
            thread_id,
            start_ns,
            (arguments && *arguments) ? " " : "",
            (arguments && *arguments) ? arguments : "");
        ::rocprofiler::windows::trace::append(message.c_str());
    }

    void complete(int64_t status = 0) const
    {
        const auto message = fmt::format(
            "event=roctx_api phase=exit operation={} correlation_id={} "
            "process_id={} thread_id={} timestamp_ns={} status={}\n",
            operation,
            correlation_id,
            process_id,
            thread_id,
            ::rocprofiler::windows::trace::timestamp_ns(),
            status);
        ::rocprofiler::windows::trace::append(message.c_str());
    }
};

template <typename Tp>
Tp
load_original(const atomic_function_t<Tp>& value)
{
    return value.load(std::memory_order_acquire);
}

void
append_marker(const char* operation,
              const char* kind,
              const char* phase,
              const range_state& state,
              roctx_range_id_t range_id,
              int64_t status,
              uint64_t timestamp)
{
    const auto message = fmt::format(
        "event=roctx_marker operation={} kind={} phase={} correlation_id={} "
        "range_id={} process_id={} thread_id={} timestamp_ns={} status={} "
        "message_hex={}\n",
        operation,
        kind,
        phase,
        state.correlation_id,
        range_id,
        state.process_id,
        state.thread_id,
        timestamp,
        status,
        state.message_hex);
    ::rocprofiler::windows::trace::append(message.c_str());
}

void
mark_wrapper(const char* message)
{
    const auto encoded   = ::rocprofiler::windows::trace::encode_text(message);
    const auto arguments = fmt::format("message_hex={}", encoded);
    const auto scope     = api_scope{"roctxMarkA", arguments.c_str()};
    const auto original = load_original(original_mark);
    if(original) original(message);
    const auto state = range_state{scope.correlation_id,
                                   scope.start_ns,
                                   scope.process_id,
                                   scope.thread_id,
                                   encoded};
    append_marker("roctxMarkA", "mark", "instant", state, 0, 0, scope.start_ns);
    scope.complete();
}

int
range_push_wrapper(const char* message)
{
    const auto encoded   = ::rocprofiler::windows::trace::encode_text(message);
    const auto arguments = fmt::format("message_hex={}", encoded);
    const auto scope     = api_scope{"roctxRangePushA", arguments.c_str()};
    const auto original = load_original(original_range_push);
    const auto status   = original ? original(message) : -1;
    if(status >= 0)
    {
        auto state = range_state{scope.correlation_id,
                                 scope.start_ns,
                                 scope.process_id,
                                 scope.thread_id,
                                 encoded};
        nested_ranges.emplace_back(state);
        append_marker("roctxRangePushA",
                      "thread_range",
                      "enter",
                      state,
                      scope.correlation_id,
                      0,
                      scope.start_ns);
    }
    scope.complete(status >= 0 ? 0 : status);
    return status;
}

int
range_pop_wrapper()
{
    const auto scope    = api_scope{"roctxRangePop"};
    const auto original = load_original(original_range_pop);
    const auto status   = original ? original() : -1;
    if(status >= 0 && !nested_ranges.empty())
    {
        const auto state = std::move(nested_ranges.back());
        nested_ranges.pop_back();
        append_marker("roctxRangePushA",
                      "thread_range",
                      "exit",
                      state,
                      state.correlation_id,
                      0,
                      ::rocprofiler::windows::trace::timestamp_ns());
    }
    scope.complete(status >= 0 ? 0 : status);
    return status;
}

roctx_range_id_t
range_start_wrapper(const char* message)
{
    const auto encoded   = ::rocprofiler::windows::trace::encode_text(message);
    const auto arguments = fmt::format("message_hex={}", encoded);
    const auto scope     = api_scope{"roctxRangeStartA", arguments.c_str()};
    const auto original = load_original(original_range_start);
    const auto range_id = original ? original(message) : 0;
    if(range_id != 0)
    {
        auto state = range_state{scope.correlation_id,
                                 scope.start_ns,
                                 scope.process_id,
                                 scope.thread_id,
                                 encoded};
        {
            std::lock_guard<std::mutex> lock{range_mutex};
            process_ranges[range_id] = state;
        }
        append_marker(
            "roctxRangeStartA", "process_range", "enter", state, range_id, 0, scope.start_ns);
    }
    scope.complete(range_id != 0 ? 0 : -1);
    return range_id;
}

struct control_callback
{
    const context::context* context   = nullptr;
    rocprofiler_user_data_t user_data = {};
};

std::vector<control_callback>
begin_control_callbacks(rocprofiler_tracing_operation_t operation)
{
    auto output = std::vector<control_callback>{};
    for(const auto* ctx : context::get_active_contexts())
    {
        if(!ctx || !ctx->is_tracing(ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API,
                                    operation) ||
           !ctx->callback_tracer)
            continue;
        const auto& callback = ctx->callback_tracer->callback_data.at(
            ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API);
        if(!callback.callback) continue;

        output.emplace_back(control_callback{ctx, {}});
        auto record = rocprofiler_callback_tracing_record_t{
            rocprofiler_context_id_t{ctx->context_idx},
            ::rocprofiler::windows::trace::thread_id(),
            {},
            ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API,
            operation,
            ROCPROFILER_CALLBACK_PHASE_ENTER,
            nullptr};
        callback.callback(record, &output.back().user_data, callback.data);
    }
    return output;
}

void
end_control_callbacks(std::vector<control_callback>& callbacks,
                      rocprofiler_tracing_operation_t operation)
{
    for(auto& entry : callbacks)
    {
        if(!entry.context || !entry.context->callback_tracer) continue;
        const auto& callback = entry.context->callback_tracer->callback_data.at(
            ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API);
        if(!callback.callback) continue;
        auto record = rocprofiler_callback_tracing_record_t{
            rocprofiler_context_id_t{entry.context->context_idx},
            ::rocprofiler::windows::trace::thread_id(),
            {},
            ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API,
            operation,
            ROCPROFILER_CALLBACK_PHASE_EXIT,
            nullptr};
        callback.callback(record, &entry.user_data, callback.data);
    }
}

int
profiler_pause_wrapper(roctx_thread_id_t tid)
{
    auto callbacks =
        begin_control_callbacks(ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerPause);
    const auto original = load_original(original_profiler_pause);
    const auto status   = original ? original(tid) : -1;
    end_control_callbacks(
        callbacks, ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerPause);
    return status;
}

int
profiler_resume_wrapper(roctx_thread_id_t tid)
{
    auto callbacks =
        begin_control_callbacks(ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerResume);
    const auto original = load_original(original_profiler_resume);
    const auto status   = original ? original(tid) : -1;
    end_control_callbacks(
        callbacks, ROCPROFILER_MARKER_CONTROL_API_ID_roctxProfilerResume);
    return status;
}

void
range_stop_wrapper(roctx_range_id_t range_id)
{
    const auto arguments = fmt::format("range_id={}", range_id);
    const auto scope     = api_scope{"roctxRangeStop", arguments.c_str()};
    const auto original = load_original(original_range_stop);
    if(original) original(range_id);

    auto state = range_state{};
    auto found = false;
    {
        std::lock_guard<std::mutex> lock{range_mutex};
        const auto itr = process_ranges.find(range_id);
        if(itr != process_ranges.end())
        {
            state = std::move(itr->second);
            process_ranges.erase(itr);
            found = true;
        }
    }
    if(found)
        append_marker("roctxRangeStartA",
                      "process_range",
                      "exit",
                      state,
                      range_id,
                      0,
                      ::rocprofiler::windows::trace::timestamp_ns());
    scope.complete(found ? 0 : -1);
}

template <typename TableT, typename MemberT>
bool
table_has(const TableT* table, MemberT TableT::* member)
{
    const auto* begin = reinterpret_cast<const std::byte*>(table);
    const auto* field = reinterpret_cast<const std::byte*>(&(table->*member));
    return table->size >= static_cast<uint64_t>((field - begin) + sizeof(table->*member));
}

#define ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(TABLE, FIELD, ORIGINAL, WRAPPER)                  \
    do                                                                                              \
    {                                                                                               \
        if(table_has((TABLE), &roctxCoreApiTable_t::FIELD) && (TABLE)->FIELD &&                     \
           (TABLE)->FIELD != &(WRAPPER))                                                             \
        {                                                                                           \
            (ORIGINAL).store((TABLE)->FIELD, std::memory_order_release);                            \
            (TABLE)->FIELD = &(WRAPPER);                                                            \
        }                                                                                           \
    } while(false)
}  // namespace

bool
set_api_tables(void** tables, uint64_t num_tables)
{
    if(!tables || num_tables != 3 || !tables[0] || !tables[1] || !tables[2]) return false;
    auto* core    = static_cast<roctxCoreApiTable_t*>(tables[0]);
    auto* control = static_cast<roctxControlApiTable_t*>(tables[1]);
    if(core->size < sizeof(uint64_t) + sizeof(roctxMarkA_fn_t) ||
       control->size < sizeof(uint64_t) + sizeof(roctxProfilerPause_fn_t))
        return false;

    ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(core, roctxMarkA_fn, original_mark, mark_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(
        core, roctxRangePushA_fn, original_range_push, range_push_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(
        core, roctxRangePop_fn, original_range_pop, range_pop_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(
        core, roctxRangeStartA_fn, original_range_start, range_start_wrapper);
    ROCPROFILER_WINDOWS_INSTALL_ROCTX_WRAPPER(
        core, roctxRangeStop_fn, original_range_stop, range_stop_wrapper);
    if(table_has(control, &roctxControlApiTable_t::roctxProfilerPause_fn) &&
       control->roctxProfilerPause_fn &&
       control->roctxProfilerPause_fn != &profiler_pause_wrapper)
    {
        original_profiler_pause.store(control->roctxProfilerPause_fn,
                                      std::memory_order_release);
        control->roctxProfilerPause_fn = &profiler_pause_wrapper;
    }
    if(table_has(control, &roctxControlApiTable_t::roctxProfilerResume_fn) &&
       control->roctxProfilerResume_fn &&
       control->roctxProfilerResume_fn != &profiler_resume_wrapper)
    {
        original_profiler_resume.store(control->roctxProfilerResume_fn,
                                       std::memory_order_release);
        control->roctxProfilerResume_fn = &profiler_resume_wrapper;
    }
    return core->roctxMarkA_fn == &mark_wrapper &&
           control->roctxProfilerPause_fn == &profiler_pause_wrapper &&
           control->roctxProfilerResume_fn == &profiler_resume_wrapper;
}
}  // namespace windows
}  // namespace roctx
}  // namespace rocprofiler
