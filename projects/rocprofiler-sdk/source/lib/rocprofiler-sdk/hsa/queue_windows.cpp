// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/hsa/queue.hpp"

#include "lib/common/logging.hpp"
#include "lib/common/scope_destructor.hpp"
#include "lib/common/static_object.hpp"
#include "lib/common/utility.hpp"
#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/context/correlation_id.hpp"
#include "lib/rocprofiler-sdk/hsa/hsa.hpp"
#include "lib/rocprofiler-sdk/hsa/queue_controller.hpp"
#include "lib/rocprofiler-sdk/hsa/signal_pool.hpp"
#include "lib/rocprofiler-sdk/hsa/windows_tool.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"
#include "lib/rocprofiler-sdk/tracing/tracing.hpp"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace rocprofiler
{
namespace hsa
{
namespace
{
constexpr auto null_signal = hsa_signal_t{.handle = 0};

struct kernel_metadata
{
    std::unordered_map<uint64_t, rocprofiler_kernel_id_t> object_ids   = {};
    std::unordered_map<uint64_t, std::string>             object_names = {};
    std::unordered_map<rocprofiler_kernel_id_t, std::string> id_names = {};
    uint64_t sequence = 0;
};

using kernel_metadata_t = common::Synchronized<kernel_metadata>;

kernel_metadata_t&
get_kernel_metadata()
{
    static auto*& value = common::static_object<kernel_metadata_t>::construct();
    return *value;
}

rocprofiler_kernel_id_t
get_kernel_id(uint64_t kernel_object)
{
    if(kernel_object == 0) return 0;

    return get_kernel_metadata().wlock([&](auto& values) {
        auto [itr, inserted] = values.object_ids.emplace(kernel_object, 0);
        if(inserted)
        {
            itr->second = ++values.sequence;
            if(auto name = values.object_names.find(kernel_object);
               name != values.object_names.end())
                values.id_names.emplace(itr->second, name->second);
        }
        return itr->second;
    });
}

bool
completion_handler(hsa_signal_value_t, void* data)
{
    using session_ptr_t = std::shared_ptr<queue_info_session_t>;
    auto session_ptr    = std::unique_ptr<session_ptr_t>{static_cast<session_ptr_t*>(data)};
    if(!session_ptr || !*session_ptr) return false;

    auto& session = **session_ptr;
    for(auto& packet : session.packet_data)
    {
        auto dispatch_time = kernel_dispatch::profiling_time{};
        auto* ext          = get_amd_ext_table();
        if(ext && ext->hsa_amd_profiling_get_dispatch_time_fn)
        {
            auto raw_time = hsa_amd_profiling_dispatch_time_t{};
            dispatch_time.status = ext->hsa_amd_profiling_get_dispatch_time_fn(
                session.queue.get_agent().get_hsa_agent(),
                packet.kernel_packet.kernel_dispatch.completion_signal,
                &raw_time);
            if(dispatch_time.status == HSA_STATUS_SUCCESS)
            {
                dispatch_time.start = raw_time.start;
                dispatch_time.end   = raw_time.end;
            }
        }

        session.queue.signal_callback([&](const auto& callbacks) {
            for(const auto& [_, callback] : callbacks)
            {
                if(callback.signal_completion)
                    callback.signal_completion(session.queue,
                                               packet.kernel_packet,
                                               *session_ptr,
                                               packet,
                                               packet.instrumentation_packets,
                                               dispatch_time);
            }
        });

        if(packet.is_serialized)
        {
            CHECK_NOTNULL(get_queue_controller())
                ->serializer(&session.queue)
                .wlock([&](auto& serializer) {
                    serializer.kernel_completion_signal(session.queue);
                });
        }

        if(auto* correlation_id = session.correlation_id)
        {
            correlation_id->sub_kern_count();
            correlation_id->sub_ref_count();
        }

        if(packet.interrupt_signal.handle != 0 && get_core_table()->hsa_signal_destroy_fn)
            get_core_table()->hsa_signal_destroy_fn(packet.interrupt_signal);
        if(packet.pooled_signal) Queue::release_signal(packet.pooled_signal);
    }

    session.queue.async_complete();
    return false;
}

uint16_t
packet_type(uint16_t header)
{
    return static_cast<uint16_t>((header >> HSA_PACKET_HEADER_TYPE) &
                                 ((1u << HSA_PACKET_HEADER_WIDTH_TYPE) - 1u));
}

void
write_interceptor(const void* packets,
                  uint64_t packet_count,
                  uint64_t,
                  void* data,
                  hsa_amd_queue_intercept_packet_writer writer)
{
    if(!writer || !data || packet_count == 0 || registration::get_fini_status() != 0)
    {
        if(writer) writer(packets, packet_count);
        return;
    }

    auto& queue = *static_cast<Queue*>(data);
    if(queue.get_notifiers() == 0)
    {
        writer(packets, packet_count);
        return;
    }

    auto tracing_data = tracing::tracing_data{};
    tracing::populate_contexts(ROCPROFILER_CALLBACK_TRACING_KERNEL_DISPATCH,
                               ROCPROFILER_BUFFER_TRACING_KERNEL_DISPATCH,
                               tracing_data);
    for(const auto* context : context::get_active_contexts([](const context::context* value) {
            return value->dispatch_counter_collection != nullptr;
        }))
        tracing_data.external_correlation_ids.emplace(context, tracing::empty_user_data);

    auto* correlation_id       = context::get_latest_correlation_id();
    auto* owned_correlation_id = static_cast<context::correlation_id*>(nullptr);
    if(!correlation_id)
    {
        correlation_id = context::correlation_tracing_service::construct(1);
        owned_correlation_id = correlation_id;
    }
    if(!correlation_id)
    {
        writer(packets, packet_count);
        return;
    }

    auto correlation_scope = common::scope_destructor{[owned_correlation_id]() {
        if(owned_correlation_id)
        {
            context::pop_latest_correlation_id(owned_correlation_id);
            owned_correlation_id->sub_ref_count();
        }
    }};

    using packet_vector_t = common::container::small_vector<rocprofiler_packet, 32>;
    auto transformed      = packet_vector_t{};
    auto session          = std::make_shared<queue_info_session_t>(queue_info_session_t{
        .queue          = queue,
        .tid            = correlation_id->thread_idx,
        .enqueue_ts     = common::timestamp_ns(),
        .correlation_id = correlation_id});
    static auto dispatch_sequence = std::atomic<uint64_t>{0};

    const auto* source = static_cast<const rocprofiler_packet*>(packets);
    for(uint64_t index = 0; index < packet_count; ++index)
    {
        if(packet_type(source[index].kernel_dispatch.header) != HSA_PACKET_TYPE_KERNEL_DISPATCH)
        {
            transformed.emplace_back(source[index]);
            continue;
        }

        correlation_id->add_ref_count();
        correlation_id->add_kern_count();

        auto packet               = packet_data_t{};
        packet.tracing_data       = tracing_data;
        packet.kernel_packet      = source[index];
        auto& kernel              = packet.kernel_packet.kernel_dispatch;
        const auto original_done  = kernel.completion_signal;
        packet.pooled_signal      = queue.create_signal(0, &kernel.completion_signal, true);
        packet.completion_signal  = kernel.completion_signal;
        tracing::populate_external_correlation_ids(
            packet.tracing_data.external_correlation_ids,
            correlation_id->thread_idx,
            ROCPROFILER_EXTERNAL_CORRELATION_REQUEST_KERNEL_DISPATCH,
            ROCPROFILER_KERNEL_DISPATCH_ENQUEUE,
            correlation_id->internal);
        auto dispatch_info = common::init_public_api_struct(rocprofiler_kernel_dispatch_info_t{});
        dispatch_info.agent_id             = queue.get_agent().get_rocp_agent()->id;
        dispatch_info.queue_id             = queue.get_id();
        dispatch_info.kernel_id            = get_kernel_id(kernel.kernel_object);
        dispatch_info.dispatch_id          = rocprofiler_dispatch_id_t{++dispatch_sequence};
        dispatch_info.private_segment_size = kernel.private_segment_size;
        dispatch_info.group_segment_size   = kernel.group_segment_size;
        dispatch_info.workgroup_size       =
            {kernel.workgroup_size_x, kernel.workgroup_size_y, kernel.workgroup_size_z};
        dispatch_info.grid_size = {kernel.grid_size_x, kernel.grid_size_y, kernel.grid_size_z};
        packet.callback_record = common::init_public_api_struct(
            rocprofiler_callback_tracing_kernel_dispatch_data_t{});
        packet.callback_record.dispatch_info = dispatch_info;

        queue.signal_callback([&](const auto& callbacks) {
            for(const auto& [client_id, callback] : callbacks)
            {
                if(!callback.write_interceptor) continue;
                auto [instrumentation, serialized] = callback.write_interceptor(
                    queue,
                    packet.kernel_packet,
                    dispatch_info.kernel_id,
                    dispatch_info.dispatch_id,
                    &packet.user_data,
                    packet.tracing_data.external_correlation_ids,
                    correlation_id);
                if(instrumentation)
                {
                    packet.is_serialized |= serialized;
                    packet.instrumentation_packets.emplace_back(
                        std::move(instrumentation), client_id);
                }
            }
        });

        auto inserted_before = false;
        if(packet.is_serialized)
        {
            inserted_before = true;
            CHECK_NOTNULL(get_queue_controller())
                ->serializer(&queue)
                .rlock([&](const auto& serializer) {
                    for(const auto& serializer_packet : serializer.kernel_dispatch(queue))
                        transformed.emplace_back(serializer_packet);
                });
        }

        for(const auto& instrumentation : packet.instrumentation_packets)
        {
            for(const auto& barrier : instrumentation.first->before_krn_barrier_pkt)
                transformed.emplace_back(barrier);
            for(const auto& before : instrumentation.first->before_krn_pkt)
            {
                transformed.emplace_back(before);
                inserted_before = true;
            }
        }

        if(inserted_before) kernel.header |= 1u << HSA_PACKET_HEADER_BARRIER;
        transformed.emplace_back(packet.kernel_packet);

        if(original_done.handle != 0)
        {
            auto barrier              = hsa_barrier_and_packet_t{};
            barrier.header            = HSA_PACKET_TYPE_BARRIER_AND << HSA_PACKET_HEADER_TYPE;
            barrier.header |= 1u << HSA_PACKET_HEADER_BARRIER;
            barrier.completion_signal = original_done;
            transformed.emplace_back(barrier);
        }

        auto inserted_after = false;
        for(const auto& instrumentation : packet.instrumentation_packets)
        {
            for(auto after : instrumentation.first->after_krn_pkt)
            {
                transformed.emplace_back(after);
                inserted_after = true;
            }
        }

        if(inserted_after)
        {
            queue.create_signal(0, &packet.interrupt_signal, false);
            packet.completion_signal = packet.interrupt_signal;
            transformed.back().kernel_dispatch.completion_signal = packet.interrupt_signal;

            auto completion_barrier              = hsa_barrier_and_packet_t{};
            completion_barrier.header = HSA_PACKET_TYPE_BARRIER_AND << HSA_PACKET_HEADER_TYPE;
            completion_barrier.header |= 1u << HSA_PACKET_HEADER_BARRIER;
            completion_barrier.dep_signal[0]     = packet.interrupt_signal;
            completion_barrier.completion_signal = packet.interrupt_signal;
            transformed.emplace_back(completion_barrier);
        }
        else
        {
            get_core_table()->hsa_signal_store_screlease_fn(packet.completion_signal, 0);
        }

        session->packet_data.emplace_back(std::move(packet));
    }

    for(auto& packet : session->packet_data)
    {
        auto packet_session = std::make_shared<queue_info_session_t>(queue_info_session_t{
            .queue          = queue,
            .tid            = session->tid,
            .enqueue_ts     = session->enqueue_ts,
            .correlation_id = session->correlation_id});
        packet_session->packet_data.emplace_back(std::move(packet));
        const auto& pending = packet_session->packet_data.back();
        queue.async_started();
        queue.signal_async_handler(
            pending.pooled_signal,
            pending.completion_signal,
            new std::shared_ptr<queue_info_session_t>{std::move(packet_session)});
    }

    writer(transformed.data(), transformed.size());
}
}  // namespace

namespace windows
{
void
register_kernel_name(uint64_t kernel_object, std::string_view name)
{
    if(kernel_object == 0 || name.empty()) return;
    get_kernel_metadata().wlock([&](auto& values) {
        values.object_names.insert_or_assign(kernel_object, std::string{name});
        if(auto id = values.object_ids.find(kernel_object); id != values.object_ids.end())
            values.id_names.insert_or_assign(id->second, std::string{name});
    });
}

std::string
get_kernel_name(rocprofiler_kernel_id_t kernel_id)
{
    return get_kernel_metadata().rlock([&](const auto& values) {
        if(auto itr = values.id_names.find(kernel_id); itr != values.id_names.end())
            return itr->second;
        return fmt::format("kernel_{}", kernel_id);
    });
}
}  // namespace windows

Queue::Queue(const AgentCache& agent, CoreApiTable table)
: _core_api(table)
, _agent(agent)
{
    _core_api.hsa_signal_create_fn(0, 0, nullptr, &_active_kernels);
}

Queue::Queue(const AgentCache& agent,
             uint32_t size,
             hsa_queue_type32_t type,
             callback_t callback,
             void* data,
             uint32_t private_segment_size,
             uint32_t group_segment_size,
             CoreApiTable core_api,
             AmdExtTable ext_api,
             hsa_queue_t** queue)
: _core_api(core_api)
, _ext_api(ext_api)
, _agent(agent)
{
    auto status = _ext_api.hsa_amd_queue_intercept_create_fn(_agent.get_hsa_agent(),
                                                              size,
                                                              type,
                                                              callback,
                                                              data,
                                                              private_segment_size,
                                                              group_segment_size,
                                                              &_intercept_queue);
    if(status != HSA_STATUS_SUCCESS) throw std::runtime_error("HSA intercept queue creation failed");

    if(_ext_api.hsa_amd_profiling_set_profiler_enabled_fn)
        _ext_api.hsa_amd_profiling_set_profiler_enabled_fn(_intercept_queue, true);

    status = _ext_api.hsa_amd_queue_intercept_register_fn(
        _intercept_queue, write_interceptor, this);
    if(status != HSA_STATUS_SUCCESS) throw std::runtime_error("HSA intercept registration failed");

    create_signal(0, &ready_signal, false);
    create_signal(0, &block_signal, false);
    create_signal(0, &_active_kernels, false);
    _core_api.hsa_signal_store_screlease_fn(ready_signal, 0);
    _core_api.hsa_signal_store_screlease_fn(_active_kernels, 0);
    *queue = _intercept_queue;
    signal_pool_init();
}

Queue::Queue(const AgentCache& agent,
             CoreApiTable core_api,
             AmdExtTable ext_api,
             hsa_queue_t* queue,
             set_write_interceptor_t set_write_interceptor)
: _core_api(core_api)
, _ext_api(ext_api)
, _agent(agent)
, _intercept_queue(queue)
{
    create_signal(0, &ready_signal, false);
    create_signal(0, &block_signal, false);
    create_signal(0, &_active_kernels, false);
    _core_api.hsa_signal_store_screlease_fn(ready_signal, 0);
    _core_api.hsa_signal_store_screlease_fn(_active_kernels, 0);
    signal_pool_init();
    set_write_interceptor(write_interceptor, this);
}

Queue::~Queue()
{
    sync();
    if(ready_signal.handle != 0 && _core_api.hsa_signal_destroy_fn)
        _core_api.hsa_signal_destroy_fn(ready_signal);
    if(block_signal.handle != 0 && _core_api.hsa_signal_destroy_fn)
        _core_api.hsa_signal_destroy_fn(block_signal);
    if(_active_kernels.handle != 0 && _core_api.hsa_signal_destroy_fn)
        _core_api.hsa_signal_destroy_fn(_active_kernels);
}

void
Queue::invoke_write_interceptor(const void* packets,
                                uint64_t packet_count,
                                hsa_amd_queue_intercept_packet_writer writer) const
{
    write_interceptor(packets, packet_count, 0, const_cast<Queue*>(this), writer);
}

void
Queue::signal_async_handler(pooled_signal_t* signal, hsa_signal_t raw_signal, void* data) const
{
    auto status = _ext_api.hsa_amd_signal_async_handler_fn(
        raw_signal, HSA_SIGNAL_CONDITION_EQ, -1, completion_handler, data);
    if(status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK)
        throw std::runtime_error("HSA async signal registration failed");
    common::consume_args(signal);
}

Queue::pooled_signal_t*
Queue::create_signal(uint32_t attribute, hsa_signal_t* signal, bool use_pool)
{
    if(auto* pool = get_signal_pool(); use_pool && pool && attribute == 0)
    {
        auto& pooled = pool->acquire(construct_hsa_signal, 0, 0, nullptr, attribute);
        *signal      = pooled.get().value;
        get_core_table()->hsa_signal_store_screlease_fn(*signal, 1);
        return &pooled;
    }

    auto status = get_amd_ext_table()->hsa_amd_signal_create_fn(1, 0, nullptr, attribute, signal);
    if(status != HSA_STATUS_SUCCESS && status != HSA_STATUS_INFO_BREAK)
        throw std::runtime_error("HSA signal creation failed");
    return nullptr;
}

void
Queue::release_signal(pooled_signal_t* signal)
{
    if(signal && signal->in_use()) signal->release();
}

void
Queue::destroy_signal(pooled_signal_t* signal)
{
    release_signal(signal);
    if(signal && get_core_table()->hsa_signal_destroy_fn)
    {
        get_core_table()->hsa_signal_destroy_fn(signal->get().value);
        signal->get().value = null_signal;
    }
}

void
Queue::sync() const
{
    if(_active_kernels.handle == 0 || !_core_api.hsa_signal_wait_relaxed_fn) return;
    constexpr auto timeout =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds{5});
    _core_api.hsa_signal_wait_relaxed_fn(_active_kernels,
                                         HSA_SIGNAL_CONDITION_EQ,
                                         0,
                                         timeout.count(),
                                         HSA_WAIT_STATE_BLOCKED);
}

void
Queue::register_callback(ClientID id, queue_callbacks_t callbacks)
{
    _callbacks.wlock([&](auto& values) {
        if(values.emplace(id, std::move(callbacks)).second) ++_notifiers;
    });
}

void
Queue::remove_callback(ClientID id)
{
    _callbacks.wlock([&](auto& values) {
        if(values.erase(id) == 1) --_notifiers;
    });
}

queue_state
Queue::get_state() const
{
    return _state;
}

void
Queue::set_state(queue_state state)
{
    _state = state;
}

void
queue_init()
{}

void
queue_fini()
{
    signal_pool_fini();
}
}  // namespace hsa
}  // namespace rocprofiler
