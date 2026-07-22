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

#include <rocprofiler-sdk/cxx/serialization.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rocprofiler
{
namespace tool
{
/// \struct statistics
/// \tparam Tp data type for statistical accumulation
/// \tparam Fp floating point data type to use for division
/// \brief A dependency-neutral generic class for statistical accumulation.
template <typename Tp, typename Fp = double>
struct statistics
{
public:
    using value_type = Tp;
    using float_type = Fp;
    using this_type  = statistics<Tp, Fp>;
    static_assert(std::is_arithmetic<Tp>::value, "only supports arithmetic types");

    statistics()                      = default;
    ~statistics()                     = default;
    statistics(const statistics&)     = default;
    statistics(statistics&&) noexcept = default;
    statistics& operator=(const statistics&) = default;
    statistics& operator=(statistics&&) noexcept = default;

    explicit statistics(value_type val)
    : m_cnt(1)
    , m_sum(val)
    , m_sqr(val * val)
    , m_min(val)
    , m_max(val)
    {}

    statistics& operator=(value_type val)
    {
        m_cnt = 1;
        m_sum = val;
        m_min = val;
        m_max = val;
        m_sqr = (val * val);
        return *this;
    }

    int64_t    get_count() const { return m_cnt; }
    value_type get_min() const { return m_min; }
    value_type get_max() const { return m_max; }
    value_type get_sum() const { return m_sum; }
    value_type get_sqr() const { return m_sqr; }
    float_type get_mean() const { return static_cast<float_type>(m_sum) / m_cnt; }
    float_type get_variance() const
    {
        if(m_cnt < 2) return (m_sum - m_sum);

        auto _sum_of_squared_samples = m_sqr;
        auto _sum_squared_mean       = (m_sum * m_sum) / static_cast<float_type>(m_cnt);
        return (_sum_of_squared_samples - _sum_squared_mean) /
               static_cast<float_type>(m_cnt - 1);
    }

    float_type get_stddev() const { return ::std::sqrt(::std::abs(get_variance())); }
    float_type get_percent(float_type _total) const
    {
        constexpr float_type one_hundred = 100.0;
        return (static_cast<float_type>(get_sum()) / _total) * one_hundred;
    }
    float_type get_percent(const this_type& _rhs) const
    {
        return get_percent(static_cast<float_type>(_rhs.get_sum()));
    }

    void reset()
    {
        m_cnt = 0;
        m_sum = value_type{};
        m_sqr = value_type{};
        m_min = value_type{};
        m_max = value_type{};
    }

    statistics& operator+=(value_type val)
    {
        if(m_cnt == 0)
        {
            m_sum = val;
            m_sqr = (val * val);
            m_min = val;
            m_max = val;
        }
        else
        {
            m_sum += val;
            m_sqr += (val * val);
            m_min = ::std::min(m_min, val);
            m_max = ::std::max(m_max, val);
        }
        ++m_cnt;
        return *this;
    }

    statistics& operator-=(value_type val)
    {
        if(m_cnt > 1) --m_cnt;
        m_sum -= val;
        m_sqr -= (val * val);
        m_min -= val;
        m_max -= val;
        return *this;
    }

    statistics& operator*=(value_type val)
    {
        m_sum *= val;
        m_sqr *= (val * val);
        m_min *= val;
        m_max *= val;
        return *this;
    }

    statistics& operator/=(value_type val)
    {
        m_sum /= val;
        m_sqr /= (val * val);
        m_min /= val;
        m_max /= val;
        return *this;
    }

    statistics& operator+=(const statistics& rhs)
    {
        if(m_cnt == 0)
        {
            m_sum = rhs.m_sum;
            m_sqr = rhs.m_sqr;
            m_min = rhs.m_min;
            m_max = rhs.m_max;
        }
        else
        {
            m_sum += rhs.m_sum;
            m_sqr += rhs.m_sqr;
            m_min = ::std::min(m_min, rhs.m_min);
            m_max = ::std::max(m_max, rhs.m_max);
        }
        m_cnt += rhs.m_cnt;
        return *this;
    }

    statistics& operator-=(const statistics& rhs)
    {
        if(m_cnt > 0)
        {
            m_sum -= rhs.m_sum;
            m_sqr -= rhs.m_sqr;
            m_min = ::std::min(m_min, rhs.m_min);
            m_max = ::std::max(m_max, rhs.m_max);
        }
        return *this;
    }

private:
    int64_t    m_cnt = 0;
    value_type m_sum = value_type{};
    value_type m_sqr = value_type{};
    value_type m_min = value_type{};
    value_type m_max = value_type{};

public:
    friend statistics operator+(const statistics& lhs, const statistics& rhs)
    {
        return statistics(lhs) += rhs;
    }

    friend statistics operator-(const statistics& lhs, const statistics& rhs)
    {
        return statistics(lhs) -= rhs;
    }

    template <typename ArchiveT>
    void serialize(ArchiveT& ar, const unsigned int) const
    {
        const auto mean     = get_mean();
        const auto stddev   = get_stddev();
        const auto variance = get_variance();
        ar(cereal::make_nvp("count", m_cnt));
        ar(cereal::make_nvp("sum", m_sum));
        ar(cereal::make_nvp("sqr", m_sqr));
        ar(cereal::make_nvp("min", m_min));
        ar(cereal::make_nvp("max", m_max));
        ar(cereal::make_nvp("mean", mean));
        ar(cereal::make_nvp("stddev", stddev));
        ar(cereal::make_nvp("variance", variance));
    }
};

using float_type        = double;
using stats_data_t      = statistics<uint64_t, float_type>;
using stats_map_t       = std::map<std::string_view, stats_data_t>;
using stats_pair_t      = std::pair<std::string_view, stats_data_t>;
using stats_entry_vec_t = std::vector<stats_pair_t>;

namespace detail
{
template <typename ArchiveT, typename = void>
struct has_structured_archive_nodes : std::false_type
{};

template <typename ArchiveT>
struct has_structured_archive_nodes<
    ArchiveT,
    std::void_t<decltype(std::declval<ArchiveT&>().setNextName("operations")),
                decltype(std::declval<ArchiveT&>().startNode()),
                decltype(std::declval<ArchiveT&>().makeArray()),
                decltype(std::declval<ArchiveT&>().finishNode())>> : std::true_type
{};
}  // namespace detail

inline bool
default_stats_sorter(const stats_pair_t& lhs, const stats_pair_t& rhs)
{
    if(lhs.second.get_sum() != rhs.second.get_sum())
        return (lhs.second.get_sum() > rhs.second.get_sum());
    return (lhs.first < rhs.first);
}

struct stats_entry_t
{
    using sort_predicate_t = bool (*)(const stats_pair_t&, const stats_pair_t&);

    stats_entry_t()                         = default;
    ~stats_entry_t()                        = default;
    stats_entry_t(const stats_entry_t&)     = default;
    stats_entry_t(stats_entry_t&&) noexcept = default;
    stats_entry_t& operator=(const stats_entry_t&) = default;
    stats_entry_t& operator=(stats_entry_t&&) noexcept = default;

    template <typename FuncT = sort_predicate_t>
    stats_entry_t& sort(FuncT&& _predicate = default_stats_sorter)
    {
        std::sort(entries.begin(), entries.end(), std::forward<FuncT>(_predicate));
        return *this;
    }

    explicit operator bool() const { return (total.get_count() > 0 && !entries.empty()); }

    stats_data_t      total   = {};
    stats_entry_vec_t entries = {};

    template <typename ArchiveT>
    void serialize(ArchiveT& ar, const unsigned int) const
    {
        total.serialize(ar, 0);
        if constexpr(detail::has_structured_archive_nodes<ArchiveT>::value)
        {
            ar.setNextName("operations");
            ar.startNode();
            ar.makeArray();
            auto first = true;
            for(const auto& [name, value] : entries)
            {
                ar.startNode();
                ar(cereal::make_nvp("key", std::string{name}));
                ar.setNextName("value");
                ar.startNode();
                if(first)
                {
                    ar(cereal::make_nvp("cereal_class_version", uint32_t{0}));
                    first = false;
                }
                value.serialize(ar, 0);
                ar.finishNode();
                ar.finishNode();
            }
            ar.finishNode();
        }
        else
        {
            auto entries_map = std::map<std::string, stats_data_t>{};
            for(const auto& [name, value] : entries)
                entries_map.emplace(std::string{name}, value);
            ar(cereal::make_nvp("operations", entries_map));
        }
    }
};
}  // namespace tool
}  // namespace rocprofiler
