////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// MIT License
//
////////////////////////////////////////////////////////////////////////////////

#include <cstdio>

#include "hsa-runtime/inc/amd_hsa_queue.h"

int main() {
  amd_queue_v2_t queue{};
  const bool initially_disabled = amd_hsa_queue_profiling_enabled(&queue) == 0;
  amd_hsa_queue_set_profiling(&queue, 1);
  const bool enabled = amd_hsa_queue_profiling_enabled(&queue) != 0;
  amd_hsa_queue_set_profiling(&queue, 0);
  const bool disabled = amd_hsa_queue_profiling_enabled(&queue) == 0;

  const bool passed = initially_disabled && enabled && disabled;
  std::printf("queue_profiling=%s initially_disabled=%u enabled=%u disabled=%u\n",
              passed ? "passed" : "failed",
              initially_disabled ? 1U : 0U,
              enabled ? 1U : 0U,
              disabled ? 1U : 0U);
  return passed ? 0 : 1;
}
