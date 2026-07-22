// MIT License
//
// Copyright (c) 2023-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#pragma once

#include "domain_type.hpp"
#include "statistics_data.hpp"

#include "lib/common/mpl.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rocprofiler
{
namespace tool
{
using domain_stats_t     = std::pair<domain_type, stats_entry_t>;
using domain_stats_vec_t = std::vector<domain_stats_t>;

struct stats_formatter
{
    template <typename Tp>
    std::ostream& operator()(std::ostream& ofs, const Tp& _val) const;
};

struct percentage
{
    float_type           value = {};
    friend std::ostream& operator<<(std::ostream& os, percentage val)
    {
        return (stats_formatter{}(os, val) << val.value);
    }
};
}  // namespace tool
}  // namespace rocprofiler

namespace std
{
template <typename Tp>
::rocprofiler::tool::statistics<Tp>
max(::rocprofiler::tool::statistics<Tp> lhs, const Tp& rhs)
{
    return lhs.get_max(rhs);
}

template <typename Tp>
::rocprofiler::tool::statistics<Tp>
min(::rocprofiler::tool::statistics<Tp> lhs, const Tp& rhs)
{
    return lhs.get_min(rhs);
}

inline std::string
to_string(::rocprofiler::tool::percentage val)
{
    auto _ss = std::stringstream{};
    _ss << val;
    return _ss.str();
}
}  // namespace std
