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

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rocprofiler
{
namespace tool
{
namespace common_config
{
inline std::string
trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if(first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::set<std::string>
parse_counter_line(std::string line, std::string_view qualifier)
{
    if(auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
    line = trim(std::move(line));
    if(line.empty()) return {};

    const auto qualifier_position = line.find(qualifier);
    if(qualifier_position == std::string::npos) return {};
    line.erase(0, qualifier_position + qualifier.size());

    for(auto& character : line)
    {
        constexpr auto special = std::string_view{"!@#$%&(),*+-./;<>?@{}^`~|"};
        if(special.find(character) != std::string_view::npos) character = ' ';
    }

    auto output = std::set<std::string>{};
    auto input  = std::istringstream{line};
    for(auto counter = std::string{}; input >> counter;)
    {
        const auto valid = std::any_of(counter.begin(), counter.end(), [](unsigned char value) {
            return std::isalnum(value) != 0 || value == '_';
        });
        if(valid && counter != qualifier) output.emplace(std::move(counter));
    }
    return output;
}

inline std::vector<std::set<std::string>>
parse_counter_groups(std::string single, std::string groups)
{
    if(!single.empty()) return {parse_counter_line(std::move(single), "pmc:")};
    if(groups.empty()) return {};

    auto output = std::vector<std::set<std::string>>{};
    auto input  = std::istringstream{groups};
    for(auto line = std::string{}; std::getline(input, line);)
        output.emplace_back(parse_counter_line(std::move(line), "pmc:"));
    return output;
}

inline std::unordered_set<size_t>
parse_kernel_filter_range(std::string input)
{
    for(auto& character : input)
    {
        if(character == '[' || character == ']' || character == ',') character = ' ';
    }

    auto output = std::unordered_set<size_t>{};
    auto stream = std::istringstream{input};
    for(auto token = std::string{}; stream >> token;)
    {
        const auto separator = token.find('-');
        if(separator == std::string::npos)
        {
            if(token.empty() || token.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument{"expected an integer kernel iteration: " + token};
            output.emplace(std::stoull(token));
            continue;
        }

        if(separator == 0 || separator + 1 >= token.size() ||
           token.find('-', separator + 1) != std::string::npos)
            throw std::invalid_argument{"bad kernel iteration range: " + token};
        const auto first_text = token.substr(0, separator);
        const auto last_text  = token.substr(separator + 1);
        if(first_text.find_first_not_of("0123456789") != std::string::npos ||
           last_text.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument{"bad kernel iteration range: " + token};

        const auto first = std::stoull(first_text);
        const auto last  = std::stoull(last_text);
        if(first > last) throw std::invalid_argument{"descending kernel iteration range: " + token};
        for(auto value = first;; ++value)
        {
            output.emplace(value);
            if(value == last) break;
        }
    }
    return output;
}
}  // namespace common_config
}  // namespace tool
}  // namespace rocprofiler
