#define NOMINMAX
#include <hsa/amd_hsa_queue.h>
#include <hsa/hsa.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <limits>

#include <windows.h>

namespace {
std::atomic<int> callback_status{-1};

const char* status_name(hsa_status_t status) {
  const char* name = "unknown";
  hsa_status_string(status, &name);
  return name;
}

void queue_error_callback(hsa_status_t status, hsa_queue_t*, void*) {
  callback_status.store(static_cast<int>(status), std::memory_order_release);
}

hsa_agent_t gpu_agent{};
bool found_gpu = false;

hsa_status_t find_gpu(hsa_agent_t agent, void*) {
  hsa_device_type_t type = HSA_DEVICE_TYPE_CPU;
  hsa_status_t status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type);
  if (status == HSA_STATUS_SUCCESS && type == HSA_DEVICE_TYPE_GPU && !found_gpu) {
    gpu_agent = agent;
    found_gpu = true;
  }
  return HSA_STATUS_SUCCESS;
}

void print_loaded_dll() {
  HMODULE module = GetModuleHandleA("amdhip64_7.dll");
  char path[MAX_PATH] = {};
  if (module != nullptr && GetModuleFileNameA(module, path, sizeof(path)) != 0) {
    std::printf("amdhip64=%s\n", path);
  }
}
}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  static_assert(offsetof(amd_queue_v2_t, hsa_queue) == 0);
  static_assert(offsetof(amd_queue_v2_t, queue_inactive_signal) == 0xc0);

  print_loaded_dll();
  hsa_status_t status = hsa_init();
  if (status != HSA_STATUS_SUCCESS) {
    std::fprintf(stderr, "hsa_init: %d (%s)\n", static_cast<int>(status), status_name(status));
    return 1;
  }

  status = hsa_iterate_agents(find_gpu, nullptr);
  if (status != HSA_STATUS_SUCCESS || !found_gpu) {
    std::fprintf(stderr, "find_gpu: %d (%s) found=%d\n", static_cast<int>(status),
                 status_name(status), found_gpu ? 1 : 0);
    hsa_shut_down();
    return 2;
  }

  hsa_queue_t* queue = nullptr;
  status = hsa_queue_create(gpu_agent, 64, HSA_QUEUE_TYPE_MULTI, queue_error_callback, nullptr,
                            std::numeric_limits<uint32_t>::max(),
                            std::numeric_limits<uint32_t>::max(), &queue);
  if (status != HSA_STATUS_SUCCESS) {
    std::fprintf(stderr, "hsa_queue_create: %d (%s)\n", static_cast<int>(status),
                 status_name(status));
    hsa_shut_down();
    return 3;
  }

  auto* amd_queue = reinterpret_cast<amd_queue_v2_t*>(queue);
  auto* packet = reinterpret_cast<hsa_kernel_dispatch_packet_t*>(queue->base_address);
  packet->header = HSA_PACKET_TYPE_INVALID;
  amd_queue->read_dispatch_id = 0;
  amd_queue->write_dispatch_id = 1;
  std::atomic_thread_fence(std::memory_order_release);

  std::printf("queue=%p ring=%p inactive_signal=0x%llx\n", static_cast<void*>(queue),
              queue->base_address,
              static_cast<unsigned long long>(amd_queue->queue_inactive_signal.handle));
  std::printf("injecting error=0x401 with unpublished packet at dispatch_id=0\n");
  hsa_signal_store_relaxed(amd_queue->queue_inactive_signal, 0x401);

  const hsa_signal_value_t done = hsa_signal_wait_scacquire(
      amd_queue->queue_inactive_signal, HSA_SIGNAL_CONDITION_EQ,
      static_cast<hsa_signal_value_t>(-1), 5'000'000'000ull, HSA_WAIT_STATE_ACTIVE);
  const int callback = callback_status.load(std::memory_order_acquire);
  std::printf("queue_signal=%lld callback_status=%d (%s)\n", static_cast<long long>(done), callback,
              callback >= 0 ? status_name(static_cast<hsa_status_t>(callback)) : "not-called");

  hsa_shut_down();
  if (done != static_cast<hsa_signal_value_t>(-1)) {
    return 4;
  }
  if (callback != static_cast<int>(HSA_STATUS_ERROR)) {
    return 5;
  }
  std::printf("rocr_scratch_recovery=passed\n");
  return 0;
}
