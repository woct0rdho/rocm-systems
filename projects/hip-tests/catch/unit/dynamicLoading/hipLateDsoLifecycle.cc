/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <hip_test_common.hh>
#include <hip_test_process.hh>

#include <array>

HIP_TEST_CASE(Unit_hipLateDsoLifecycle_ProcessFinalization) {
  constexpr std::array<const char*, 12> kModes = {"before-init",
                                                  "unused-exit",
                                                  "unused-ab",
                                                  "unused-ba",
                                                  "used-exit",
                                                  "used-dlclose",
                                                  "exit-ab",
                                                  "exit-ba",
                                                  "close-ab",
                                                  "close-ba",
                                                  "repeat-dlclose 100",
                                                  "parallel-dlclose 25"};

  constexpr std::array<const char*, 2> kDeferredLoadingValues = {"0", "1"};

  for (const char* deferred_loading : kDeferredLoadingValues) {
    for (const char* mode : kModes) {
      DYNAMIC_SECTION("HIP_ENABLE_DEFERRED_LOADING=" << deferred_loading << " mode=" << mode) {
        hip::SpawnProc process("hipLateDsoLifecycle_exe", true);
        process.setEnv("HIP_ENABLE_DEFERRED_LOADING", deferred_loading);
        const int status = process.run(mode);
        INFO("child output:\n" << process.getOutput());
        REQUIRE(status == 0);
      }
    }
  }
}
