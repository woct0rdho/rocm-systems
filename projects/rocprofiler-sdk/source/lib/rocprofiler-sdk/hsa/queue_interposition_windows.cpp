// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/hsa/queue_interposition.hpp"

#include "lib/common/static_object.hpp"
#include "lib/common/utility.hpp"

namespace rocprofiler
{
namespace hsa
{
namespace queue_interposition
{
queue_registry_t&
get_queue_registry()
{
    static auto*& value = common::static_object<queue_registry_t>::construct();
    return *value;
}

queue_state_ptr_t
lookup_queue_state(const hsa_queue_t*, bool)
{
    return {};
}

queue_state_ptr_t
lookup_queue_state_by_doorbell(hsa_signal_t, bool)
{
    return {};
}

uint64_t
add_write_index_impl(QueueState* state, uint64_t value, std::memory_order order)
{
    return state->virtual_wptr.fetch_add(value, order);
}

void
store_write_index_impl(QueueState* state, uint64_t value, std::memory_order order)
{
    state->virtual_wptr.store(value, order);
}

uint64_t
cas_write_index_impl(QueueState* state, uint64_t expected, uint64_t value, std::memory_order order)
{
    state->virtual_wptr.compare_exchange_strong(expected, value, order);
    return expected;
}

uint64_t
load_write_index_impl(const QueueState* state, std::memory_order order)
{
    return state->virtual_wptr.load(order);
}

size_t
get_async_signal_handler_thread_count()
{
    return 0;
}

void
process_doorbell_impl(const queue_state_ptr_t&, hsa_signal_value_t, const doorbell_fn_t&)
{}

std::shared_ptr<QueueState>
create_queue_state(const hsa_queue_t*, bool)
{
    return {};
}

void
destroy_queue_state(const hsa_queue_t*)
{}

bool
supports_queue_interposition()
{
    return false;
}

void
notify_queue_interposition_consumer_context_started(const context::context*)
{}

void
notify_queue_interposition_consumer_context_stopped(const context::context*)
{}

void
interposition_init(CoreApiTable*, bool)
{}

void
interposition_fini()
{}

void
interposition_sync()
{}
}  // namespace queue_interposition
}  // namespace hsa
}  // namespace rocprofiler
