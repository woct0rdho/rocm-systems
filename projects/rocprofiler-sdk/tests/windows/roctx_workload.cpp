// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <rocprofiler-sdk-roctx/roctx.h>

#include <Windows.h>

#include <cstdio>

int
main()
{
    roctx_thread_id_t thread_id = 0;
    if(roctxGetThreadId(&thread_id) != 0 ||
       thread_id != static_cast<roctx_thread_id_t>(::GetCurrentThreadId()))
        return 1;
    if(roctxNameOsThread("windows-roctx-workload") != 0) return 1;

    roctxMarkA("standalone mark");
    if(roctxRangePushA("outer range") != 0) return 1;
    if(roctxRangePushA("inner range") != 1) return 1;
    if(roctxRangePop() != 1) return 1;
    if(roctxRangePop() != 0) return 1;
    if(roctxRangePop() >= 0) return 1;

    const auto range_id = roctxRangeStartA("process range");
    if(range_id == 0) return 1;
    roctxRangeStop(range_id);

    std::printf(
        "roctx_workload=passed thread_id=%llu mark=1 thread_ranges=2 process_ranges=1 "
        "hsa_initialized=no gpu_work_executed=no\n",
        static_cast<unsigned long long>(thread_id));
    return 0;
}
