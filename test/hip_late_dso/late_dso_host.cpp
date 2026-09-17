#include <dlfcn.h>
#include <hip/hip_runtime.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

#define HIP_CHECK(expr)                                                                    \
  do {                                                                                     \
    hipError_t status = (expr);                                                             \
    if (status != hipSuccess) {                                                             \
      std::cerr << #expr << " failed: " << hipGetErrorString(status) << " (" << status    \
                << ")\n";                                                                \
      return 2;                                                                            \
    }                                                                                      \
  } while (false)

__global__ void baseline_kernel(int* value) {
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    *value += 7;
  }
}

using LaunchFn = hipError_t (*)(int*);

struct Plugin {
  void* handle = nullptr;
  LaunchFn launch = nullptr;
};

Plugin load_plugin(const char* path) {
  Plugin plugin;
  plugin.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (plugin.handle == nullptr) {
    std::cerr << "dlopen failed: " << dlerror() << "\n";
    return plugin;
  }

  dlerror();
  plugin.launch = reinterpret_cast<LaunchFn>(dlsym(plugin.handle, "hip_late_dso_launch"));
  if (const char* error = dlerror(); error != nullptr) {
    std::cerr << "dlsym failed: " << error << "\n";
    dlclose(plugin.handle);
    plugin = {};
  }
  return plugin;
}

int launch_baseline(int* device_value) {
  baseline_kernel<<<1, 1>>>(device_value);
  HIP_CHECK(hipGetLastError());
  HIP_CHECK(hipDeviceSynchronize());
  return 0;
}

int run_late_case(const char* plugin_path, bool use_plugin, bool unload_plugin, int iterations,
                  bool load_before_init, bool synchronize_plugin = true) {
  int* device_value = nullptr;
  Plugin early_plugin;

  if (load_before_init) {
    early_plugin = load_plugin(plugin_path);
    if (early_plugin.handle == nullptr) {
      return 2;
    }
  }

  HIP_CHECK(hipMalloc(&device_value, sizeof(*device_value)));
  HIP_CHECK(hipMemset(device_value, 0, sizeof(*device_value)));
  if (launch_baseline(device_value) != 0) {
    return 2;
  }

  for (int iteration = 0; iteration < iterations; ++iteration) {
    Plugin plugin = load_before_init ? early_plugin : load_plugin(plugin_path);
    if (plugin.handle == nullptr) {
      return 2;
    }

    if (use_plugin) {
      HIP_CHECK(plugin.launch(device_value));
      if (synchronize_plugin) {
        HIP_CHECK(hipDeviceSynchronize());
      }
    }

    if (unload_plugin) {
      if (dlclose(plugin.handle) != 0) {
        std::cerr << "dlclose failed: " << dlerror() << "\n";
        return 2;
      }
      if (launch_baseline(device_value) != 0) {
        return 2;
      }
    }

    if (load_before_init) {
      break;
    }
  }

  int host_value = 0;
  HIP_CHECK(hipMemcpy(&host_value, device_value, sizeof(host_value), hipMemcpyDeviceToHost));
  const int expected = 7 * (1 + (unload_plugin ? iterations : 0)) +
                       (use_plugin ? (load_before_init ? 1 : iterations) : 0);
  if (host_value != expected) {
    std::cerr << "unexpected device value: " << host_value << ", expected " << expected << "\n";
    return 3;
  }

  // Keep the allocation and late-loaded DSO alive in process-exit modes. This exercises the same
  // native finalizer ordering as the Python/PyTorch reproducer. Explicit-unload modes clean up.
  if (unload_plugin) {
    HIP_CHECK(hipFree(device_value));
  }

  std::cout << "case completed before process finalization: value=" << host_value << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0]
              << " <plugin.so>"
                 " <before-init|unused-exit|used-exit|used-dlclose|used-pending-dlclose|"
                 "repeat-dlclose>"
                 " [iterations]\n";
    return 2;
  }

  const char* plugin_path = argv[1];
  const std::string mode = argv[2];
  const int iterations = argc >= 4 ? std::atoi(argv[3]) : 1;

  if (mode == "before-init") {
    return run_late_case(plugin_path, false, false, 1, true);
  }
  if (mode == "unused-exit") {
    return run_late_case(plugin_path, false, false, 1, false);
  }
  if (mode == "used-exit") {
    return run_late_case(plugin_path, true, false, 1, false);
  }
  if (mode == "used-dlclose") {
    return run_late_case(plugin_path, true, true, 1, false);
  }
  if (mode == "used-pending-dlclose") {
    return run_late_case(plugin_path, true, true, 1, false, false);
  }
  if (mode == "repeat-dlclose") {
    if (iterations <= 0) {
      std::cerr << "iterations must be positive\n";
      return 2;
    }
    return run_late_case(plugin_path, true, true, iterations, false);
  }

  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
