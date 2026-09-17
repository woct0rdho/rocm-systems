/*
 * Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <hip/hip_runtime.h>

#ifndef HIP_LATE_DSO_ID
#define HIP_LATE_DSO_ID 1
#endif

#define HIP_LATE_DSO_CONCAT_IMPL(a, b) a##b
#define HIP_LATE_DSO_CONCAT(a, b) HIP_LATE_DSO_CONCAT_IMPL(a, b)
#define HIP_LATE_DSO_GLOBAL HIP_LATE_DSO_CONCAT(hip_late_dso_global_, HIP_LATE_DSO_ID)
#define HIP_LATE_DSO_MANAGED HIP_LATE_DSO_CONCAT(hip_late_dso_managed_, HIP_LATE_DSO_ID)
#define HIP_LATE_DSO_KERNEL HIP_LATE_DSO_CONCAT(hip_late_dso_kernel_, HIP_LATE_DSO_ID)

__device__ int HIP_LATE_DSO_GLOBAL = 10 + HIP_LATE_DSO_ID;
__managed__ int HIP_LATE_DSO_MANAGED = 20 + HIP_LATE_DSO_ID;

__global__ void HIP_LATE_DSO_KERNEL(int* output) {
  *output += 1 + HIP_LATE_DSO_GLOBAL + HIP_LATE_DSO_MANAGED;
}

extern "C" int hip_late_dso_expected() { return 31 + 2 * HIP_LATE_DSO_ID; }

extern "C" int hip_late_dso_launch(int* output) {
  hipLaunchKernelGGL(HIP_LATE_DSO_KERNEL, dim3(1), dim3(1), 0, nullptr, output);
  return static_cast<int>(hipGetLastError());
}
