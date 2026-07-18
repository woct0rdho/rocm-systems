/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <hip/hip_runtime.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Plugin {
  using Launch = int (*)(int*);
  using Expected = int (*)();

  void* handle = nullptr;
  Launch launch = nullptr;
  Expected expected = nullptr;
};

bool checkHip(hipError_t status, const char* operation) {
  if (status == hipSuccess) {
    return true;
  }
  std::cerr << operation << " failed: " << hipGetErrorString(status) << '\n';
  return false;
}

bool initializeHip() {
  int* value = nullptr;
  if (!checkHip(hipMalloc(&value, sizeof(*value)), "hipMalloc")) {
    return false;
  }
  if (!checkHip(hipMemset(value, 0, sizeof(*value)), "hipMemset") ||
      !checkHip(hipDeviceSynchronize(), "hipDeviceSynchronize") ||
      !checkHip(hipFree(value), "hipFree")) {
    return false;
  }
  return true;
}

Plugin loadPlugin(const char* path) {
  Plugin plugin;
  plugin.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (plugin.handle == nullptr) {
    std::cerr << "dlopen(" << path << ") failed: " << dlerror() << '\n';
    return plugin;
  }

  plugin.launch = reinterpret_cast<Plugin::Launch>(dlsym(plugin.handle, "hip_late_dso_launch"));
  plugin.expected =
      reinterpret_cast<Plugin::Expected>(dlsym(plugin.handle, "hip_late_dso_expected"));
  if (plugin.launch == nullptr || plugin.expected == nullptr) {
    std::cerr << "dlsym failed: " << dlerror() << '\n';
    dlclose(plugin.handle);
    plugin = {};
  }
  return plugin;
}

bool usePlugin(const Plugin& plugin) {
  int* device_value = nullptr;
  int host_value = 0;
  if (!checkHip(hipMalloc(&device_value, sizeof(*device_value)), "hipMalloc(plugin output)")) {
    return false;
  }
  if (!checkHip(hipMemset(device_value, 0, sizeof(*device_value)), "hipMemset(plugin output)")) {
    checkHip(hipFree(device_value), "hipFree(plugin output after memset failure)");
    return false;
  }

  const auto launch_status = static_cast<hipError_t>(plugin.launch(device_value));
  bool success = checkHip(launch_status, "plugin launch");
  if (success) {
    success = checkHip(hipDeviceSynchronize(), "plugin synchronize");
  }
  if (success) {
    success =
        checkHip(hipMemcpy(&host_value, device_value, sizeof(host_value), hipMemcpyDeviceToHost),
                 "plugin result copy");
  }
  success = checkHip(hipFree(device_value), "hipFree(plugin output)") && success;
  if (!success) {
    return false;
  }

  if (host_value != plugin.expected()) {
    std::cerr << "unexpected plugin result: " << host_value << " expected " << plugin.expected()
              << '\n';
    return false;
  }
  return true;
}

bool closePlugin(Plugin& plugin) {
  if (plugin.handle == nullptr) {
    return true;
  }
  if (dlclose(plugin.handle) != 0) {
    std::cerr << "dlclose failed: " << dlerror() << '\n';
    return false;
  }
  plugin = {};
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr
        << "usage: " << argv[0]
        << " <before-init|unused-exit|unused-ab|unused-ba|used-exit|used-dlclose|"
           "repeat-dlclose|parallel-dlclose|exit-ab|exit-ba|close-ab|close-ba> [iterations]\n";
    return 2;
  }

  const std::string mode = argv[1];
  constexpr const char* kPluginA = "./libHipLateDsoA.so";
  constexpr const char* kPluginB = "./libHipLateDsoB.so";

  if (mode == "before-init") {
    Plugin plugin = loadPlugin(kPluginA);
    return plugin.handle != nullptr && initializeHip() ? 0 : 1;
  }

  if (!initializeHip()) {
    return 1;
  }

  if (mode == "unused-exit") {
    return loadPlugin(kPluginA).handle != nullptr ? 0 : 1;
  }

  if (mode == "unused-ab" || mode == "unused-ba") {
    const bool load_ab = mode == "unused-ab";
    Plugin first = loadPlugin(load_ab ? kPluginA : kPluginB);
    Plugin second = loadPlugin(load_ab ? kPluginB : kPluginA);
    return first.handle != nullptr && second.handle != nullptr ? 0 : 1;
  }

  if (mode == "used-exit" || mode == "used-dlclose") {
    Plugin plugin = loadPlugin(kPluginA);
    if (plugin.handle == nullptr || !usePlugin(plugin)) {
      return 1;
    }
    return mode == "used-dlclose" && !closePlugin(plugin) ? 1 : 0;
  }

  if (mode == "repeat-dlclose") {
    const int iterations = argc >= 3 ? std::atoi(argv[2]) : 100;
    for (int i = 0; i < iterations; ++i) {
      Plugin plugin = loadPlugin(kPluginA);
      if (plugin.handle == nullptr || !usePlugin(plugin) || !closePlugin(plugin)) {
        return 1;
      }
    }
    return 0;
  }

  if (mode == "parallel-dlclose") {
    const int iterations = argc >= 3 ? std::atoi(argv[2]) : 25;
    constexpr int kThreadCount = 4;
    std::atomic<bool> success{true};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int thread = 0; thread < kThreadCount; ++thread) {
      threads.emplace_back([&]() {
        for (int i = 0; i < iterations && success.load(); ++i) {
          Plugin plugin = loadPlugin(kPluginA);
          if (plugin.handle == nullptr || !usePlugin(plugin) || !closePlugin(plugin)) {
            success.store(false);
            return;
          }
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    return success.load() ? 0 : 1;
  }

  if (mode == "exit-ab" || mode == "exit-ba" || mode == "close-ab" || mode == "close-ba") {
    const bool load_ab = mode != "exit-ba";
    Plugin first = loadPlugin(load_ab ? kPluginA : kPluginB);
    Plugin second = loadPlugin(load_ab ? kPluginB : kPluginA);
    if (first.handle == nullptr || second.handle == nullptr || !usePlugin(first) ||
        !usePlugin(second)) {
      return 1;
    }

    if (mode == "close-ab") {
      return closePlugin(first) && closePlugin(second) ? 0 : 1;
    }
    if (mode == "close-ba") {
      return closePlugin(second) && closePlugin(first) ? 0 : 1;
    }
    return 0;
  }

  std::cerr << "unknown mode: " << mode << '\n';
  return 2;
}
