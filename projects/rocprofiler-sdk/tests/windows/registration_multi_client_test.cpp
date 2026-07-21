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

#include <rocprofiler-sdk/experimental/registration.h>
#include <rocprofiler-sdk/registration.h>

#include <Windows.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

extern "C" int
rocprofiler_set_api_table(const char*, uint64_t, uint64_t, void**, uint64_t);

int
main(int argc, char** argv)
{
    if(argc != 4 && argc != 5)
    {
        std::fprintf(
            stderr, "usage: %s FIRST_TOOL SECOND_TOOL LOG [throw-first]\n", argv[0]);
        return 2;
    }

    const auto requested_failure = argc == 5 && std::string{argv[4]} == "throw-first";
    if(argc == 5 && !requested_failure)
    {
        std::fprintf(stderr, "unknown multi-client test mode: %s\n", argv[4]);
        return 2;
    }

    const auto result_path = std::string{argv[3]} + ".result";
    ::DeleteFileA(argv[3]);
    ::DeleteFileA(result_path.c_str());
    const auto libraries = std::string{argv[1]} + ";" + argv[2];
    if(!::SetEnvironmentVariableA("ROCP_TOOL_LIBRARIES", libraries.c_str()) ||
       !::SetEnvironmentVariableA("ROCPROFILER_MULTI_CLIENT_LOG", argv[3]) ||
       !::SetEnvironmentVariableA("ROCPROFILER_MULTI_CLIENT_THROW",
                                  requested_failure ? "first" : nullptr) ||
       !::SetEnvironmentVariableA("ROCPROFILER_WINDOWS_RESULT_FILE",
                                  requested_failure ? result_path.c_str() : nullptr))
    {
        std::fprintf(stderr, "could not configure the multi-client test environment\n");
        return 2;
    }

    auto table = uint64_t{0};
    auto ptr   = static_cast<void*>(&table);
    const auto status = rocprofiler_set_api_table("hip_tools", 1, 0, &ptr, 1);
    if(status != 0)
    {
        std::fprintf(stderr, "multi-client SDK initialization failed: %d\n", status);
        return 3;
    }

    auto input = std::ifstream{argv[3], std::ios::binary};
    const auto contents = std::string{std::istreambuf_iterator<char>{input},
                                      std::istreambuf_iterator<char>{}};
    const auto expected = std::array<std::string, 6>{"first configure 0\n",
                                                     "second configure 1\n",
                                                     "first initialize 1\n",
                                                     "first finalize 1\n",
                                                     "second initialize 2\n",
                                                     "second finalize 2\n"};
    auto expected_contents = std::string{};
    for(const auto& line : expected)
        expected_contents += line;
    if(contents != expected_contents)
    {
        std::fprintf(stderr,
                     "unexpected multi-client lifecycle log:\nexpected:\n%sobserved:\n%s",
                     expected_contents.c_str(),
                     contents.c_str());
        return 4;
    }

    if(requested_failure)
    {
        auto result_input = std::ifstream{result_path, std::ios::binary};
        const auto result = std::string{std::istreambuf_iterator<char>{result_input},
                                        std::istreambuf_iterator<char>{}};
        if(result.find("status=client_finalize_failed\n") == std::string::npos ||
           result.find("detail=first\n") == std::string::npos)
        {
            std::fprintf(stderr,
                         "finalization failure was not propagated:\n%s",
                         result.c_str());
            return 5;
        }
        std::printf("windows_registration_finalization_failure=passed client=first sticky=yes\n");
    }
    else
    {
        std::printf("windows_registration_multi_client=passed configured=2 initialized=2 "
                    "finalized=2\n");
    }
    return 0;
}
