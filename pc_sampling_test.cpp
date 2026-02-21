// Simple HIP workload for PC sampling sanity check.

#include <hip/hip_runtime.h>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{

int read_env_int(const char* name, int default_val, int min_val, int max_val)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return default_val;

    char* end = nullptr;
    errno = 0;
    long val = std::strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0') return default_val;
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return static_cast<int>(val);
}

size_t read_env_size(const char* name, size_t default_val, size_t min_val, size_t max_val)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return default_val;

    char* end = nullptr;
    errno = 0;
    unsigned long long val = std::strtoull(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0') return default_val;

    auto sval = static_cast<size_t>(val);
    if (sval < min_val) return min_val;
    if (sval > max_val) return max_val;
    return sval;
}

}  // namespace

__global__ void saxpy(const float* a, const float* b, float* c, size_t n, int inner_iters)
{
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
    {
        float x = a[i];
        float y = b[i];
        // Do a few ops to increase instruction count.
        for (int k = 0; k < inner_iters; ++k) x = x * 1.0001f + y * 0.9999f;
        c[i] = x;
    }
}

int main()
{
    // Defaults target a multi-second run to increase the chance of hitting live waves.
    const size_t n = read_env_size("PCS_N", (1u << 20), 1024, (1u << 26));
    const int launches = read_env_int("PCS_KERNEL_LAUNCHES", 2000, 1, std::numeric_limits<int>::max());
    const int inner_iters = read_env_int("PCS_INNER_ITERS", 128, 1, std::numeric_limits<int>::max());
    const size_t bytes = n * sizeof(float);
    const dim3 threads(256);
    const dim3 blocks((n + threads.x - 1) / threads.x);

    std::vector<float> h_a(n, 1.0f), h_b(n, 2.0f), h_c(n, 0.0f);
    float *d_a = nullptr, *d_b = nullptr, *d_c = nullptr;

    std::cout << "PCS workload config: N=" << n << " launches=" << launches << " inner_iters=" << inner_iters << "\n";

    auto err = hipMalloc(&d_a, bytes);
    err = hipMalloc(&d_b, bytes);
    err = hipMalloc(&d_c, bytes);

    err = hipMemcpy(d_a, h_a.data(), bytes, hipMemcpyHostToDevice);
    err = hipMemcpy(d_b, h_b.data(), bytes, hipMemcpyHostToDevice);

    hipEvent_t start_ev = nullptr, stop_ev = nullptr;
    err = hipEventCreate(&start_ev);
    err = hipEventCreate(&stop_ev);
    err = hipEventRecord(start_ev, nullptr);

    // Launch enough work to keep waves active for multiple host-trap periods.
    for (int it = 0; it < launches; ++it)
        hipLaunchKernelGGL(saxpy, blocks, threads, 0, 0, d_a, d_b, d_c, n, inner_iters);

    err = hipDeviceSynchronize();
    err = hipEventRecord(stop_ev, nullptr);
    err = hipEventSynchronize(stop_ev);

    float ms = 0.0f;
    err = hipEventElapsedTime(&ms, start_ev, stop_ev);

    err = hipMemcpy(h_c.data(), d_c, bytes, hipMemcpyDeviceToHost);

    std::cout << "done: " << h_c[0] << " elapsed_ms=" << ms << "\n";

    err = hipEventDestroy(start_ev);
    err = hipEventDestroy(stop_ev);
    err = hipFree(d_a);
    err = hipFree(d_b);
    err = hipFree(d_c);
    return 0;
}
