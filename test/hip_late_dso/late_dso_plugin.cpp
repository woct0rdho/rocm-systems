#include <hip/hip_runtime.h>

namespace {

__global__ void late_dso_kernel(int* value) {
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    atomicAdd(value, 1);
  }
}

}  // namespace

extern "C" hipError_t hip_late_dso_launch(int* value) {
  late_dso_kernel<<<1, 1>>>(value);
  return hipGetLastError();
}

extern "C" const char* hip_late_dso_name() { return "hip_late_dso_plugin"; }
