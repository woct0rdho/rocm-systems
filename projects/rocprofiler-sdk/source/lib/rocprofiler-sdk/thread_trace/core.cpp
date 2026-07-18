// MIT License
//
// Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
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

// Implements the core coordination logic for thread trace start/stop, buffer
// iteration, and integration with the public API surface.
#include "lib/rocprofiler-sdk/thread_trace/core.hpp"
#include "lib/rocprofiler-sdk/thread_trace/threading.hpp"

#include "lib/common/container/stable_vector.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/agent.hpp"
#include "lib/rocprofiler-sdk/buffer.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/counters/device_counting.hpp"
#include "lib/rocprofiler-sdk/hsa/queue.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/internal_threading.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"

#include <rocprofiler-sdk/fwd.h>
#include <rocprofiler-sdk/hsa.h>
#include <rocprofiler-sdk/intercept_table.h>

#include <hsa/hsa_api_trace.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK_HSA(fn, message)                                                                     \
    {                                                                                              \
        auto _status = (fn);                                                                       \
        if(_status != HSA_STATUS_SUCCESS)                                                          \
        {                                                                                          \
            ROCP_ERROR << "HSA Err: " << _status << '\n';                                          \
            throw std::runtime_error(message);                                                     \
        }                                                                                          \
    }

namespace rocprofiler
{
namespace thread_trace
{
constexpr uint64_t MIN_BUFFER_SIZE = 1 << 20;  // 1MB minimum to give the GPU room before copies

bool
needs_application_queue_sqtt_control(std::string_view agent_name)
{
    return agent_name.compare(0, 5, "gfx10") == 0 || agent_name.compare(0, 5, "gfx11") == 0;
}

struct cbdata_t
{
    rocprofiler_agent_id_t                          agent      = {.handle = 0};
    rocprofiler_thread_trace_shader_data_callback_t cb_fn      = nullptr;
    const rocprofiler_user_data_t*                  userdata   = nullptr;
    uint64_t                                        next_chunk = 0;
};

// Keeps track of a single client registering for serialized thread trace
// operations so we can gate new traces while one is active.
common::Synchronized<std::optional<int64_t>> client;

// True once the HSA runtime is registered. Gates start_context() so pre-init
// start requests are deferred and replayed by start_active_contexts().
std::atomic<bool>&
hsa_inited()
{
    static std::atomic<bool> inited{false};
    return inited;
}

// Set before final shutdown starts. Device contexts must not submit new SQTT
// start packets after stop-and-drain has begun.
std::atomic<bool>&
shutting_down()
{
    static std::atomic<bool> value{false};
    return value;
}

hsa_status_t
thread_trace_callback(uint32_t shader, void* buffer, uint64_t size, void* callback_data)
{
    auto& cb_data = *static_cast<cbdata_t*>(callback_data);

    auto shader_data             = rocprofiler_thread_trace_shader_data_t{};
    shader_data.size             = sizeof(shader_data);
    shader_data.data             = buffer;
    shader_data.data_size        = size;
    shader_data.shader_engine_id = shader;
    shader_data.chunk_index      = cb_data.next_chunk++;
    shader_data.read_offset      = 0;
    shader_data.agent            = cb_data.agent;
    shader_data.flags            = ROCPROFILER_THREAD_TRACE_SHADER_DATA_FLAGS_END;

    cb_data.cb_fn(shader_data, *cb_data.userdata);
    // The iterator guarantees the last chunk is tagged with END; here we just
    // ferry the data to the user callback.
    return HSA_STATUS_SUCCESS;
}

bool
thread_trace_parameter_pack::are_params_valid() const
{
    // Guard against the most common misconfigurations before touching HSA
    // state so we can fail early with a descriptive message.
    if(shader_cb_fn == nullptr)
    {
        ROCP_CI_LOG(WARNING) << "Callback cannot be null!";
        return false;
    }

    if(shader_engine_mask == 0) return false;

    if(buffer_size < MIN_BUFFER_SIZE)
    {
        ROCP_CI_LOG(WARNING) << "Invalid buffer size: " << buffer_size;
        return false;
    }

    if(target_cu > 0xF) return false;
    if(simd_select > 0xF) return false;  // Only 16 CUs and 4 SIMDs

    return true;
}

ThreadTracerAgent::ThreadTracerAgent(thread_trace_parameter_pack _params,
                                     rocprofiler_agent_id_t      cache)
: params(std::move(_params))
, agent_id(cache)
{
    // Allocate and configure all heavy-weight objects up front: subsequent
    // start calls reuse the queue and packet factory without additional setup.
    ROCP_TRACE << "Constructing ATT instance for agent " << agent_id.handle;
    auto* core = CHECK_NOTNULL(hsa::get_core_table());
    auto* ext  = CHECK_NOTNULL(hsa::get_amd_ext_table());

    const auto* agent =
        CHECK_NOTNULL(rocprofiler::agent::get_agent_cache(rocprofiler::agent::get_agent(agent_id)));
    application_queue_control = needs_application_queue_sqtt_control(agent->name());

    size_t staging_size = (params.num_buffers > 1) ? params.buffer_size : 0ul;
    size_t staging_n    = (params.num_buffers > 1) ? params.num_buffers : 0ul;
    queue               = make_att_queue(*agent, staging_size, staging_n);

    factory = std::make_unique<aql::ThreadTraceAQLPacketFactory>(*agent, this->params, *core, *ext);
    control_packet = factory->construct_control_packet();

#if ROCPROFILER_EXTERNAL_AQLPROFILE == 0
    auto queue_packet_status = aqlprofile_att_get_queue_control_packet(
        &queue_enable_packet, control_packet->GetHandle(), 1);
    queue_enable_packet_valid = queue_packet_status == HSA_STATUS_SUCCESS;
    ROCP_WARNING_IF(!queue_enable_packet_valid)
        << "Could not create queue-local ATT enable packet: " << queue_packet_status;
#endif

    codeobj_reg = std::make_unique<code_object::CodeobjCallbackRegistry>(
        [this](rocprofiler_agent_id_t _agent, uint64_t codeobj_id, uint64_t addr, uint64_t size) {
            if(_agent == this->agent_id) this->load_codeobj(codeobj_id, addr, size);
        },
        [this](uint64_t codeobj_id) { this->unload_codeobj(codeobj_id); });

    codeobj_reg->IterateLoaded();
}

ThreadTracerAgent::~ThreadTracerAgent()
{
    ROCP_TRACE << "Destroying ATT Queue...";
    if(active_traces.load() < 1) return;

    // Resource teardown should have stopped and drained the trace already. Keep
    // this as a last-resort safety path, but complete the single-buffer stop
    // signal and final iteration instead of dropping the pending ATT payload.
    ROCP_WARNING << "Thread tracer being destroyed with thread trace active";

    if(auto flag = worker_flag) flag->store(WORKER_FLAG_DESTRUCTOR);
    if(auto signal = stop_thread_trace()) signal_wait(*signal);
    iterate_data();

    ROCP_ERROR_IF(active_traces.load() > 0)
        << "Thread tracer destruction could not drain all active thread traces";
}

/**
 * Callback we get from HSA interceptor when a kernel packet is being enqueued.
 * We return an AQLPacket containing the start/stop/read packets for injection.
 */
std::unique_ptr<hsa::TraceControlAQLPacket>
ThreadTracerAgent::get_control(bool bStart)
{
    auto active_resources = std::make_unique<hsa::TraceControlAQLPacket>(*control_packet);
    // Clone the control packet so callers can safely mutate state without
    // racing with concurrent dispatches.
    active_resources->clear();

    if(bStart) active_traces.fetch_add(1);

    return active_resources;
}

std::unique_ptr<hsa::TraceControlAQLPacket>
ThreadTracerAgent::get_start_packet()
{
    auto lock = std::unique_lock{trace_resources_mut};
    return get_control(true);
}

std::unique_ptr<hsa::TraceControlAQLPacket>
ThreadTracerAgent::get_queue_start_packet()
{
    auto lock = std::unique_lock{trace_resources_mut};
    return get_control(false);
}

void
ThreadTracerAgent::iterate_data(aqlprofile_handle_t handle, rocprofiler_user_data_t data)
{
    if(active_traces.load() <= 0) return;

    cbdata_t cb_dt{};

    cb_dt.agent = agent_id;
    // Walk each buffer produced by the ATT runtime and forward it to the
    // registered shader callback.
    cb_dt.cb_fn    = params.shader_cb_fn;
    cb_dt.userdata = &data;

    auto status = aqlprofile_att_iterate_data(handle, thread_trace_callback, &cb_dt);
    if(status == HSA_STATUS_ERROR_OUT_OF_RESOURCES)
        ROCP_WARNING << "Thread trace buffer full!";
    else if(status != HSA_STATUS_SUCCESS)
        ROCP_CI_LOG(ERROR) << "Failed to iterate ATT data: " << status;

    active_traces.fetch_sub(1);
}

void
ThreadTracerAgent::iterate_data()
{
    // Already executed by producer thread, skip
    if(params.num_buffers > 1) return;

    auto lock = std::unique_lock{trace_resources_mut};
    iterate_data(control_packet->GetHandle(), params.callback_userdata);
}

std::unique_ptr<hsa::QueueThreadTraceEnableAQLPacket>
ThreadTracerAgent::get_queue_enable_packet() const
{
    if(!queue_enable_packet_valid) return nullptr;
    return std::make_unique<hsa::QueueThreadTraceEnableAQLPacket>(
        control_packet->GetAgent(), queue_enable_packet);
}

signal_ptr_t
ThreadTracerAgent::activate_application_queue(hsa_queue_t* application_queue) const
{
    if(application_queue == nullptr) return nullptr;

    auto packet = get_queue_enable_packet();
    if(packet == nullptr || packet->before_krn_pkt.empty()) return nullptr;

    auto signal = make_signal(&packet->before_krn_pkt.back());
    auto packet_ptrs = std::vector<const void*>{};
    packet_ptrs.reserve(packet->before_krn_pkt.size());
    for(auto& entry : packet->before_krn_pkt)
        packet_ptrs.emplace_back(&entry);
    counters::submitPackets(application_queue, packet_ptrs.data(), packet_ptrs.size());
    return signal;
}

void
ThreadTracerAgent::load_codeobj(code_object_id_t id, uint64_t addr, uint64_t size)
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);

    control_packet->add_codeobj(id, addr, size);
    // Keep shader metadata in sync while traces are live so symbol resolution
    // remains accurate in the emitted stream.

    if(!queue || active_traces.load() < 1) return;

    auto packet = factory->construct_load_marker_packet(id, addr, size);
    auto sig    = att_queue_submit(*queue, &packet->packet, true);
    if(sig) signal_wait(*sig);
}

void
ThreadTracerAgent::unload_codeobj(code_object_id_t id)
{
    std::unique_lock<std::mutex> lk(trace_resources_mut);

    if(!control_packet->remove_codeobj(id)) return;
    // Tear down metadata when code objects disappear to avoid dangling
    // references in the trace stream.
    if(!queue || active_traces.load() < 1) return;

    auto packet = factory->construct_unload_marker_packet(id);
    auto sig    = att_queue_submit(*queue, &packet->packet, true);
    if(sig) signal_wait(*sig);
}

void
ThreadTracerAgent::prepare_queue_thread_trace(std::shared_ptr<std::atomic<int>> _flag)
{
    auto lock   = std::unique_lock{trace_resources_mut};
    worker_flag = std::move(_flag);
    active_traces.fetch_add(1);
}

std::shared_ptr<hsa_signal_t>
ThreadTracerAgent::start_thread_trace(std::shared_ptr<std::atomic<int>> _flag)
{
    ROCP_TRACE << "Starting thread trace for agent " << agent_id.handle;
    auto lock   = std::unique_lock{trace_resources_mut};
    worker_flag = std::move(_flag);

    auto control_packet_copy = get_control(true);
    control_packet_copy->clear();
    control_packet_copy->populate_before();
    control_packet_copy->populate_after();

    // Warmup the async copy so we dont wait too long for the flip.
    if(params.num_buffers > 1)
    {
        auto& buffer = queue->cpu_buffers;
        copy_data_sync(buffer.at(0),
                       buffer.at(1),
                       queue->near_cpu,
                       queue->hsa_agent,
                       MIN_BUFFER_SIZE,
                       nullptr);
    }

    auto    buffer_packet    = std::unique_ptr<rocprofiler::hsa::SQTTBufferingPackets>{};
    int64_t shader_engine_id = 0;
    if(params.num_buffers > 1)
    {
        // Build the status/swap packets before SQTT starts so the aqlprofile
        // manager is fully populated while the hardware is still idle.
        for(uint64_t i = 0; (params.shader_engine_mask >> i) != 0; i++)
            if((params.shader_engine_mask >> i) % 2 == 1) shader_engine_id = i;

        buffer_packet = std::make_unique<rocprofiler::hsa::SQTTBufferingPackets>(
            control_packet_copy->GetHandle(), shader_engine_id);
    }

    auto shared_signal = std::shared_ptr<hsa_signal_t>{};

    if(params.num_buffers > 1)
    {
        auto unique_signal = make_signal();
        signal_reset(*unique_signal);
        shared_signal = std::shared_ptr<hsa_signal_t>(std::move(unique_signal));

        auto worker_data         = std::make_shared<triple_buffer_shared_data_t>();
        worker_data->queue       = queue.get();  // non-owning; ThreadTracerAgent owns queue
        worker_data->num_buffers = params.num_buffers;

        // Wire each slot to its CPU staging buffer. Slots default to FREE.
        for(size_t i = 0; i < worker_data->num_buffers; i++)
            worker_data->buffers[i].memory = worker_data->queue->cpu_buffers.at(i);

        auto start_packets = control_packet_copy->before_krn_pkt;
        ROCP_FATAL_IF(start_packets.empty()) << "ATT start packet list is empty";
        auto producer_data             = triple_buffer_producer_data_t{};
        producer_data.producer_running = worker_flag;
        producer_data.start_pkt_signal = shared_signal;
        producer_data.control_packet   = std::move(control_packet_copy);
        producer_data.copy_data_fn     = copy_data_sync;
        producer_data.shared           = worker_data;
        producer_data.buffer_packet    = std::move(buffer_packet);
        producer_data.shader_engine_id = shader_engine_id;
        const auto* rocp_agent = CHECK_NOTNULL(agent::get_agent(agent_id));
        producer_data.gfx11_workarounds = ((rocp_agent->gfx_target_version / 10000) % 100) == 11;

        // Other call sites (kfd, internal_threading) wrap each std::thread
        // creation in its own pre/post pair, so match that convention.
        internal_threading::notify_pre_internal_thread_create(ROCPROFILER_LIBRARY);
        producer = std::thread{producer_loop, std::move(producer_data)};
        internal_threading::notify_post_internal_thread_create(ROCPROFILER_LIBRARY);

        // One consumer thread per slot. Each thread owns a single slot and
        // waits on its per-slot cv; callbacks run in parallel across slots.
        consumers.reserve(params.num_buffers);
        for(size_t i = 0; i < params.num_buffers; i++)
        {
            auto consumer_data        = triple_buffer_consumer_data_t{};
            consumer_data.callback_fn = params.shader_cb_fn;
            consumer_data.userdata    = params.callback_userdata;
            consumer_data.shared      = worker_data;
            consumer_data.slot_index  = i;

            internal_threading::notify_pre_internal_thread_create(ROCPROFILER_LIBRARY);
            consumers.emplace_back(consumer_loop, std::move(consumer_data));
            internal_threading::notify_post_internal_thread_create(ROCPROFILER_LIBRARY);
        }

        // Arm the producer before enabling SQTT so the first hardware buffer cannot
        // fill before status polling begins.
        while(!worker_data->producer_waiting.load(std::memory_order_acquire))
            std::this_thread::yield();

        att_queue_submit_signal_last(*queue, start_packets, *shared_signal);

        while(!worker_data->producer_ready.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    else
    {
        // Single-buffer tracing retains the existing fan-out behavior: submit without
        // waiting and let the caller wait on all agent start signals in parallel.
        auto unique_signal = att_queue_submit_signal_last(*queue, control_packet_copy->before_krn_pkt);
        shared_signal = std::shared_ptr<hsa_signal_t>(std::move(unique_signal));
    }
    return shared_signal;
}

signal_ptr_t
ThreadTracerAgent::stop_thread_trace(hsa_queue_t* application_queue)
{
    ROCP_TRACE << "Stopping thread trace on application queue for agent " << agent_id.handle;
    auto lock = std::unique_lock{trace_resources_mut};

    if(active_traces.load() == 0 || application_queue == nullptr || params.num_buffers > 1)
        return nullptr;

    auto control_packet_copy = get_control(false);
    control_packet_copy->clear();
    control_packet_copy->populate_after();
    if(control_packet_copy->after_krn_pkt.empty()) return nullptr;

    auto signal = make_signal(&control_packet_copy->after_krn_pkt.back());
    auto packet_ptrs = std::vector<const void*>{};
    packet_ptrs.reserve(control_packet_copy->after_krn_pkt.size());
    for(auto& packet : control_packet_copy->after_krn_pkt)
        packet_ptrs.emplace_back(&packet);
    counters::submitPackets(application_queue, packet_ptrs.data(), packet_ptrs.size());
    return signal;
}

signal_ptr_t
ThreadTracerAgent::stop_thread_trace()
{
    ROCP_TRACE << "Stopping Thread trace for agent " << agent_id.handle;
    auto lock = std::unique_lock{trace_resources_mut};

    if(active_traces.load() == 0) return nullptr;

    if(params.num_buffers > 1)
    {
        int expected = WORKER_FLAG_RUNNING;
        worker_flag->compare_exchange_strong(expected, WORKER_FLAG_STOP);

        if(producer.joinable()) producer.join();
        for(auto& t : consumers)
            if(t.joinable()) t.join();
        consumers.clear();
        active_traces.fetch_sub(1);
        worker_flag = nullptr;
        return nullptr;
    }
    else
    {
        auto control_packet_copy = get_control(false);
        control_packet_copy->clear();
        // Join helpers and emit the final set of packets so the GPU drains.
        control_packet_copy->populate_after();
        // Submit without waiting; DeviceThreadTracer::stop_context fans out
        // submissions across agents and waits on every signal in parallel
        // before calling iterate_data.
        return att_queue_submit_signal_last(*queue, control_packet_copy->after_krn_pkt);
    }
}

// Single buffering: inject the stop packets directly and return.
void
DispatchThreadTracer::resource_init()
{
    auto rocp_agents = rocprofiler::agent::get_agents();

    auto lk = std::unique_lock{agents_map_mut};

    for(const auto* rocp_agent : rocp_agents)
    {
        auto it = params.find(rocp_agent->id);
        if(it == params.end()) continue;

        auto cache = rocprofiler::agent::get_hsa_agent(rocp_agent);
        if(!cache.has_value())
        {
            ROCP_CI_LOG_IF(TRACE, rocp_agent->runtime_visibility.hsa != 0)
                << fmt::format("Could not find HSA Agent for agent-{} (handle={}, name={})",
                               rocp_agent->node_id,
                               rocp_agent->id.handle,
                               rocp_agent->name);
            continue;
        }
        agents[*cache] = std::make_unique<ThreadTracerAgent>(it->second, rocp_agent->id);
    }
}

void
DispatchThreadTracer::resource_deinit()
{
    ROCP_TRACE << "Clearing agents";
    auto lk = std::unique_lock{agents_map_mut};
    agents.clear();
}

/**
 * Callback we get from HSA interceptor when a kernel packet is being enqueued.
 * We return an AQLPacket containing the start/stop/read packets for injection.
 */
hsa::write_packet_t
DispatchThreadTracer::pre_kernel_call(const hsa::Queue&              queue,
                                      rocprofiler_kernel_id_t        kernel_id,
                                      rocprofiler_dispatch_id_t      dispatch_id,
                                      rocprofiler_user_data_t*       user_data,
                                      const context::correlation_id* corr_id)
{
    rocprofiler_async_correlation_id_t rocprof_corr_id =
        rocprofiler_async_correlation_id_t{.internal = 0, .external = context::null_user_data};

    if(corr_id)
    {
        rocprof_corr_id.internal = corr_id->internal;
    }
    // TODO: Get external

    std::shared_lock<std::shared_mutex> lk(agents_map_mut);

    auto it = agents.find(queue.get_agent().get_hsa_agent());

    if(it == agents.end()) return {nullptr, false};

    auto&       agent      = *CHECK_NOTNULL(it->second);
    const auto& parameters = agent.params;

    auto control_flags = parameters.dispatch_cb_fn(queue.get_agent().get_rocp_agent()->id,
                                                   queue.get_id(),
                                                   rocprof_corr_id,
                                                   kernel_id,
                                                   dispatch_id,
                                                   parameters.callback_userdata.ptr,
                                                   user_data);

    if(control_flags == ROCPROFILER_THREAD_TRACE_CONTROL_NONE)
        return {nullptr, parameters.bSerialize};

    auto packet = agent.get_start_packet();
    post_move_data.fetch_add(1);
    packet->populate_before();
    packet->populate_after();
    return {std::move(packet), true};
}

void
DispatchThreadTracer::post_kernel_call(DispatchThreadTracer::inst_pkt_t& aql,
                                       const hsa::queue_info_session_t& /*session*/,
                                       const hsa::packet_data_t& packet_data)
{
    if(post_move_data.load() < 1) return;

    for(auto& aql_pkt : aql)
    {
        auto* pkt = dynamic_cast<hsa::TraceControlAQLPacket*>(aql_pkt.first.get());
        if(!pkt) continue;

        std::shared_lock<std::shared_mutex> lk(agents_map_mut);
        post_move_data.fetch_sub(1);

        if(pkt->after_krn_pkt.empty()) continue;

        auto it = agents.find(pkt->GetAgent());
        if(it != agents.end() && it->second != nullptr)
            it->second->iterate_data(pkt->GetHandle(), packet_data.user_data);
    }
}

void
DispatchThreadTracer::start_context()
{
    using corr_id_map_t = hsa::queue_info_session_t::external_corr_id_map_t;

    // Only installs queue-controller callbacks (cached and applied to queues as
    // they are created), so this is safe to call before hsa_init.
    CHECK_NOTNULL(hsa::get_queue_controller())->enable_serialization();

    // Only one thread should be attempting to enable/disable this context
    client.wlock([&](auto& client_id) {
        if(client_id) return;

        auto&& _callbacks = hsa::queue_callbacks_t{
            .batch_packets = []() { return false; },
            .write_interceptor =
                [=](const hsa::Queue& q,
                    const hsa::rocprofiler_packet& /* kern_pkt */,
                    rocprofiler_kernel_id_t   kernel_id,
                    rocprofiler_dispatch_id_t dispatch_id,
                    rocprofiler_user_data_t*  user_data,
                    const corr_id_map_t& /* extern_corr_ids */,
                    const context::correlation_id* corr_id) {
                    return this->pre_kernel_call(q, kernel_id, dispatch_id, user_data, corr_id);
                },
            .signal_completion =
                [=](const hsa::Queue& /* q */,
                    hsa::rocprofiler_packet /* kern_pkt */,
                    std::shared_ptr<hsa::queue_info_session_t>& session,
                    hsa::packet_data_t&                         packet_data,
                    inst_pkt_t&                                 aql,
                    kernel_dispatch::profiling_time) {
                    this->post_kernel_call(aql, *session, packet_data);
                }};

        client_id = CHECK_NOTNULL(hsa::get_queue_controller())
                        ->add_callback(std::nullopt, std::move(_callbacks));
    });
}

void
DispatchThreadTracer::stop_context()  // NOLINT(readability-convert-member-functions-to-static)
{
    auto* controller = hsa::get_queue_controller();
    if(!controller) return;

    client.wlock([&](auto& client_id) {
        if(!client_id) return;

        // Remove our callbacks from HSA's queue controller
        controller->remove_callback(*client_id);
        client_id = std::nullopt;
    });

    controller->disable_serialization();
}

bool
DeviceThreadTracer::requires_queue_intercept()
{
    auto lock = std::unique_lock{agent_mut};
    for(const auto& [agent_id, pack] : params)
    {
        if(pack.perfcounter_ctrl != 0 && !pack.perfcounters.empty()) return true;

        if(const auto* rocp_agent = rocprofiler::agent::get_agent(agent_id);
           pack.num_buffers <= 1 && rocp_agent != nullptr &&
           needs_application_queue_sqtt_control(rocp_agent->name))
            return true;
    }
    return false;
}

DeviceThreadTracer::DeviceThreadTracer()
{
    worker_flag = std::make_shared<std::atomic<int>>(WORKER_FLAG_STOP);
}

hsa::write_packet_t
DeviceThreadTracer::pre_kernel_call(const hsa::Queue& queue)
{
    if(!queue_activation_enabled.load(std::memory_order_acquire)) return {nullptr, false};

    auto lock = std::unique_lock{agent_mut};
    if(!queue_activation_enabled.load(std::memory_order_relaxed)) return {nullptr, false};

    auto* rocp_agent = queue.get_agent().get_rocp_agent();
    if(rocp_agent == nullptr) return {nullptr, false};

    auto itr = agents.find(rocp_agent->id);
    if(itr == agents.end() || itr->second == nullptr || !itr->second->uses_single_buffer() ||
       !itr->second->requires_application_queue_control())
        return {nullptr, false};

    application_queue_ids[rocp_agent->id] = queue.intercept_queue()->id;

    if(application_queue_start_pending.erase(rocp_agent->id) > 0)
    {
        ROCP_TRACE << fmt::format(
            "Starting device thread trace on application queue for agent {}",
            rocp_agent->id.handle);
        auto packet = itr->second->get_queue_start_packet();
        packet->populate_before();
        return {std::move(packet), true};
    }

    ROCP_TRACE << fmt::format("Injecting queue-local thread-trace enable packet for agent {}",
                              rocp_agent->id.handle);
    return {itr->second->get_queue_enable_packet(), false};
}

void
DeviceThreadTracer::register_queue_callback()
{
    auto* controller = hsa::get_queue_controller();
    if(controller == nullptr) return;

    auto lock = std::unique_lock{agent_mut};
    if(queue_callback_id) return;

    using corr_id_map_t = hsa::queue_info_session_t::external_corr_id_map_t;
    auto callbacks      = hsa::queue_callbacks_t{
        .batch_packets = []() { return false; },
        .write_interceptor =
            [this](const hsa::Queue& q,
                   const hsa::rocprofiler_packet& /* kern_pkt */,
                   rocprofiler_kernel_id_t /* kernel_id */,
                   rocprofiler_dispatch_id_t /* dispatch_id */,
                   rocprofiler_user_data_t* /* user_data */,
                   const corr_id_map_t& /* extern_corr_ids */,
                   const context::correlation_id* /* corr_id */) {
                return this->pre_kernel_call(q);
            },
        .signal_completion = [](auto&&...) {}};

    queue_callback_id = controller->add_callback(std::nullopt, std::move(callbacks));
    ROCP_TRACE << fmt::format("Registered device thread-trace queue callback {}",
                              *queue_callback_id);
}

void
DeviceThreadTracer::remove_queue_callback()
{
    auto* controller = hsa::get_queue_controller();
    if(controller == nullptr) return;

    auto callback_id = std::optional<int64_t>{};
    {
        auto lock = std::unique_lock{agent_mut};
        callback_id.swap(queue_callback_id);
    }
    if(callback_id) controller->remove_callback(*callback_id);
}

void
DeviceThreadTracer::resource_init()
{
    auto rocp_agents = rocprofiler::agent::get_agents();

    std::unique_lock<std::mutex> lk(agent_mut);

    for(const auto* rocp_agent : rocp_agents)
    {
        auto it = params.find(CHECK_NOTNULL(rocp_agent)->id);
        if(it == params.end()) continue;

        if(!rocprofiler::agent::get_hsa_agent(rocp_agent).has_value())
        {
            ROCP_TRACE << "Could not find HSA Agent for " << rocp_agent->id.handle
                       << ". This agent maybe isolated by ROCR_VISIBLE_DEVICES env variable";
            continue;
        }

        agents[it->first] = std::make_unique<ThreadTracerAgent>(it->second, rocp_agent->id);
    }

    lk.unlock();
    register_queue_callback();
}

void
DeviceThreadTracer::resource_deinit()
{
    queue_activation_enabled.store(false, std::memory_order_release);

    // Never use ThreadTracerAgent destruction as the normal hardware-stop path.
    // stop_context() waits for stop packets and performs final data iteration.
    stop_context();
    remove_queue_callback();

    ROCP_TRACE << "Clearing agents";
    std::unique_lock<std::mutex> lk(agent_mut);
    application_queue_start_pending.clear();
    application_queue_ids.clear();
    agents.clear();
}

void
DeviceThreadTracer::start_context()
{
    if(shutting_down().load(std::memory_order_acquire))
    {
        ROCP_INFO << "Ignoring device thread trace start during shutdown";
        return;
    }

    // Per-agent resources don't exist until HSA is registered; the request is
    // cached in the active-context array and replayed by start_active_contexts().
    if(!hsa_inited().load())
    {
        ROCP_INFO << "Device thread trace start requested before hsa_init; deferring";
        return;
    }

    ROCP_INFO << "Start device thread trace context";
    CHECK_NOTNULL(worker_flag);
    register_queue_callback();
    std::unique_lock<std::mutex> lk(agent_mut);

    if(agents.empty())
    {
        ROCP_WARNING << "Thread trace context not present for agent!";
        return;
    }
    int expected = WORKER_FLAG_STOP;
    if(!worker_flag->compare_exchange_strong(expected, WORKER_FLAG_RUNNING))
    {
        ROCP_ERROR << "Unable to start thread trace worker thread";
        return;
    }
    queue_activation_enabled.store(true, std::memory_order_release);
    auto activation_wait_list = std::vector<signal_ptr_t>{};
    auto internal_tracers     = std::vector<ThreadTracerAgent*>{};
    auto* controller          = hsa::get_queue_controller();

    for(auto& [agent_id, tracer] : agents)
    {
        if(tracer->uses_single_buffer() && tracer->requires_application_queue_control())
        {
            tracer->prepare_queue_thread_trace(worker_flag);
            application_queue_start_pending.emplace(agent_id);
            continue;
        }

        internal_tracers.emplace_back(tracer.get());
        if(!tracer->requires_application_queue_control() || controller == nullptr) continue;

        const auto current_agent_id = agent_id;
        auto*      current_tracer   = tracer.get();
        controller->iterate_queues([&activation_wait_list,
                                    current_agent_id,
                                    current_tracer](const hsa::Queue* application_queue) {
            if(application_queue == nullptr) return;
            const auto* queue_agent = application_queue->get_agent().get_rocp_agent();
            if(queue_agent == nullptr || queue_agent->id != current_agent_id) return;

            auto signal = current_tracer->activate_application_queue(
                const_cast<hsa_queue_t*>(application_queue->intercept_queue()));
            if(signal) activation_wait_list.emplace_back(std::move(signal));
        });
    }

    // Queue context state must complete before global SQTT state is programmed
    // on the internal control queue.
    for(auto& sig : activation_wait_list)
        signal_wait(*CHECK_NOTNULL(sig));

    auto start_wait_list = std::vector<std::shared_ptr<hsa_signal_t>>{};
    for(auto* tracer : internal_tracers)
        start_wait_list.emplace_back(tracer->start_thread_trace(worker_flag));

    for(auto& sig : start_wait_list)
        signal_wait(*CHECK_NOTNULL(sig));
}

void
DeviceThreadTracer::stop_context()
{
    queue_activation_enabled.store(false, std::memory_order_release);
    auto lock = std::unique_lock{agent_mut};
    application_queue_start_pending.clear();

    if(agents.empty()) return;

    ROCP_INFO << "Stopping device thread trace context";

    int expected = WORKER_FLAG_RUNNING;
    if(auto flag = worker_flag) flag->compare_exchange_strong(expected, WORKER_FLAG_STOP);

    auto wait_list = std::vector<signal_ptr_t>{};
    auto* controller = hsa::get_queue_controller();

    for(auto& [agent_id, tracer] : agents)
    {
        auto* application_queue = static_cast<hsa_queue_t*>(nullptr);
        if(controller != nullptr)
        {
            if(auto itr = application_queue_ids.find(agent_id);
               itr != application_queue_ids.end())
            {
                if(const auto* tracked_queue = controller->get_queue(itr->second))
                    application_queue =
                        const_cast<hsa_queue_t*>(tracked_queue->intercept_queue());
            }
        }

        if(tracer->uses_single_buffer() && tracer->requires_application_queue_control() &&
           application_queue != nullptr)
            wait_list.emplace_back(tracer->stop_thread_trace(application_queue));
        else
            wait_list.emplace_back(tracer->stop_thread_trace());
    }

    // Wait on every agent's after-packets explicitly so iterate_data only runs
    // once the GPU has drained the trace; mirrors start_context's parallel wait.
    for(auto& sig : wait_list)
        if(sig) signal_wait(*sig);

    for(auto& [_, tracer] : agents)
        tracer->iterate_data();
}

void
initialize(HsaApiTable* table)
{
    ROCP_FATAL_IF(!table->core_ || !table->amd_ext_);

    for(auto& ctx : context::get_registered_contexts())
    {
        if(ctx->device_thread_trace) ctx->device_thread_trace->resource_init();
        if(ctx->dispatch_thread_trace) ctx->dispatch_thread_trace->resource_init();
    }
}

void
start_active_contexts()
{
    // HSA resources now exist; allow start_context() to program the hardware.
    shutting_down().store(false, std::memory_order_release);
    hsa_inited().store(true, std::memory_order_release);

    // Replay device contexts started before hsa_init() (their start_context()
    // returned early). Must run after the queue infrastructure is initialized
    // (see registration.cpp); starting the SQTT hardware earlier hangs the GPU.
    for(auto& ctx : context::get_active_contexts())
    {
        if(ctx->device_thread_trace) ctx->device_thread_trace->start_context();
    }
}

void
flush_and_stop()
{
    ROCP_TRACE << "flush_and_stop called";
    shutting_down().store(true, std::memory_order_release);

    for(auto& ctx : context::get_registered_contexts())
    {
        if(ctx->device_thread_trace)
        {
            if(CHECK_NOTNULL(ctx->device_thread_trace->worker_flag)->load() != WORKER_FLAG_ERROR)
                ctx->device_thread_trace->worker_flag->store(WORKER_FLAG_DESTRUCTOR);
            ctx->device_thread_trace->stop_context();
        }
        if(ctx->dispatch_thread_trace) ctx->dispatch_thread_trace->stop_context();
    }
}

void
finalize()
{
    ROCP_TRACE << "Finalize called";
    shutting_down().store(true, std::memory_order_release);
    hsa_inited().store(false, std::memory_order_release);
    for(auto& ctx : context::get_registered_contexts())
    {
        if(ctx->device_thread_trace) ctx->device_thread_trace->resource_deinit();
        if(ctx->dispatch_thread_trace) ctx->dispatch_thread_trace->resource_deinit();
    }

    code_object::finalize();
}
}  // namespace thread_trace
}  // namespace rocprofiler
