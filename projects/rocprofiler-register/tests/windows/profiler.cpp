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
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "registration_test.hpp"

#include <cstdint>
#include <cstring>

namespace
{
int
profiled_operation(int value)
{
    return value + 10;
}
}  // namespace

extern "C" {
struct rocprofiler_client_id_t
{
    const char* name;
    uint32_t    handle;
};

struct rocprofiler_tool_configure_result_t
{
    size_t size;
    int (*initialize)(void (*)(rocprofiler_client_id_t), void*);
    void (*finalize)(void*);
    void* tool_data;
};

__declspec(dllexport) rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t, const char*, uint32_t, rocprofiler_client_id_t*)
{
    return nullptr;
}

__declspec(dllexport) int
rocprofiler_set_api_table(const char* name,
                          uint64_t    version,
                          uint64_t    instance,
                          void**      tables,
                          uint64_t    num_tables)
{
    if(!name || std::strcmp(name, "hip") != 0) return 1;
    if(version != 70000 || instance != 0 || !tables || num_tables != 1) return 2;
    auto* table = static_cast<windows_registration_api_table*>(tables[0]);
    if(!table || table->size != sizeof(windows_registration_api_table)) return 3;
    table->operation = &profiled_operation;
    return 0;
}
}
