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

#pragma once

#include "counter_config_common.hpp"
#include "lib/common/regex.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rocprofiler
{
namespace tool
{
class kernel_selector
{
public:
    kernel_selector(std::string include_expression,
                    std::string exclude_expression,
                    std::string iteration_expression)
    : kernel_selector{std::move(include_expression),
                      std::move(exclude_expression),
                      common_config::parse_kernel_filter_range(
                          std::move(iteration_expression))}
    {}

    kernel_selector(std::string                include_expression,
                    std::string                exclude_expression,
                    std::unordered_set<size_t> iteration_range)
    : include_expression_{std::move(include_expression)}
    , exclude_expression_{std::move(exclude_expression)}
    , iteration_range_{std::move(iteration_range)}
    {
        // Parse both expressions during tool configuration rather than allowing a malformed
        // pattern to escape through the queue callback.
        rocprofiler::common::regex::regex_search({}, include_expression_);
        if(!exclude_expression_.empty())
            rocprofiler::common::regex::regex_search({}, exclude_expression_);
    }

    // The caller serializes access at the enqueue-time dispatch callback. A cached decision
    // ensures trace, counters, JSON, and later statistics cannot advance iteration state
    // independently for the same SDK dispatch.
    bool select(uint64_t dispatch_id, std::string_view formatted_kernel_name)
    {
        if(auto itr = decisions_.find(dispatch_id); itr != decisions_.end())
            return itr->second;

        auto selected = rocprofiler::common::regex::regex_search(
                            formatted_kernel_name, include_expression_) &&
                        (exclude_expression_.empty() ||
                         !rocprofiler::common::regex::regex_search(
                             formatted_kernel_name, exclude_expression_));
        if(selected)
        {
            const auto iteration = ++iterations_[std::string{formatted_kernel_name}];
            selected = iteration_range_.empty() || iteration_range_.count(iteration) > 0;
        }
        decisions_.emplace(dispatch_id, selected);
        return selected;
    }

    std::optional<bool> find(uint64_t dispatch_id) const
    {
        if(auto itr = decisions_.find(dispatch_id); itr != decisions_.end())
            return itr->second;
        return std::nullopt;
    }

    std::optional<bool> take(uint64_t dispatch_id)
    {
        auto decision = find(dispatch_id);
        decisions_.erase(dispatch_id);
        return decision;
    }

    void erase(uint64_t dispatch_id) { decisions_.erase(dispatch_id); }

private:
    std::string include_expression_ = {};
    std::string exclude_expression_ = {};
    std::unordered_set<size_t> iteration_range_ = {};
    std::unordered_map<std::string, size_t> iterations_ = {};
    std::unordered_map<uint64_t, bool> decisions_ = {};
};
}  // namespace tool
}  // namespace rocprofiler
