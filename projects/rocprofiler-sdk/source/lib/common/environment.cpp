// MIT License
//
// Copyright (c) 2023-2025 Advanced Micro Devices, Inc.
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

#include "lib/common/environment.hpp"
#include "lib/common/demangle.hpp"
#include "lib/common/logging.hpp"

#include <fmt/format.h>

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <sys/auxv.h>
#    include <unistd.h>
#endif
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace rocprofiler
{
namespace common
{
namespace impl
{
#if defined(_WIN32)
namespace
{
std::optional<std::wstring>
to_wide(std::string_view value)
{
    if(value.empty()) return std::wstring{};
    const auto size = ::MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr,
                                             0);
    if(size <= 0) return std::nullopt;

    auto result = std::wstring(static_cast<size_t>(size), L'\0');
    if(::MultiByteToWideChar(CP_UTF8,
                             MB_ERR_INVALID_CHARS,
                             value.data(),
                             static_cast<int>(value.size()),
                             result.data(),
                             size) != size)
        return std::nullopt;
    return result;
}

std::optional<std::string>
to_utf8(std::wstring_view value)
{
    if(value.empty()) return std::string{};
    const auto size = ::WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if(size <= 0) return std::nullopt;

    auto result = std::string(static_cast<size_t>(size), '\0');
    if(::WideCharToMultiByte(CP_UTF8,
                             WC_ERR_INVALID_CHARS,
                             value.data(),
                             static_cast<int>(value.size()),
                             result.data(),
                             size,
                             nullptr,
                             nullptr) != size)
        return std::nullopt;
    return result;
}
}  // namespace
#endif

// Safely read environment variable directly from environ array.
// This avoids issues with bash's custom getenv() implementation which
// breaks when setenv() is called before bash initializes its internal tables.
//
// THREAD SAFETY: Reads are NOT safe against concurrent setenv()/putenv()/
// unsetenv() from any thread, because glibc may reallocate the environ
// array itself (not just mutate entries). Prefer reading at init time or
// caching in a function-local static.
std::optional<std::string>
get_env_direct(std::string_view name)
{
    if(name.empty()) return std::nullopt;

#if defined(_WIN32)
    const auto env_name = to_wide(name);
    if(!env_name) return std::nullopt;

    ::SetLastError(ERROR_SUCCESS);
    const auto size = ::GetEnvironmentVariableW(env_name->c_str(), nullptr, 0);
    if(size == 0)
    {
        if(::GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string{};
    }

    auto value = std::vector<wchar_t>(size);
    const auto written =
        ::GetEnvironmentVariableW(env_name->c_str(), value.data(), static_cast<DWORD>(value.size()));
    if(written == 0 || written >= value.size()) return std::nullopt;
    return to_utf8(std::wstring_view{value.data(), written});
#else
    if(!environ) return std::nullopt;

    for(char** env = environ; *env; ++env)
    {
        std::string_view entry{*env};
        if(entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 &&
           entry[name.size()] == '=')
        {
            // copy the value so callers do not retain pointers into environ.
            return std::string{entry.substr(name.size() + 1)};
        }
    }

    return std::nullopt;
#endif
}

int
set_env_direct(std::string_view name, std::string_view value, int override)
{
    if(name.empty()) return -1;

#if defined(_WIN32)
    if(override == 0 && get_env_direct(name).has_value()) return 0;
    const auto env_name  = to_wide(name);
    const auto env_value = to_wide(value);
    if(!env_name || !env_value) return -1;
    return (::SetEnvironmentVariableW(env_name->c_str(), env_value->c_str()) != 0) ? 0 : -1;
#else
    auto env_name  = std::string{name};
    auto env_value = std::string{value};
    return ::setenv(env_name.c_str(), env_value.c_str(), override);
#endif
}

int
unset_env_direct(std::string_view name)
{
    if(name.empty()) return -1;

#if defined(_WIN32)
    const auto env_name = to_wide(name);
    if(!env_name) return -1;
    return (::SetEnvironmentVariableW(env_name->c_str(), nullptr) != 0) ? 0 : -1;
#else
    auto env_name = std::string{name};
    return ::unsetenv(env_name.c_str());
#endif
}

std::string
get_env(std::string_view env_id, std::string_view _default)
{
    auto env_var = get_env_direct(env_id);
    if(env_var) return *env_var;
    return std::string{_default};
}

std::string
get_env(std::string_view env_id, const char* _default)
{
    return get_env(env_id, std::string_view{_default});
}

bool
get_env(std::string_view env_id, bool _default)
{
    if(env_id.empty()) return _default;
    auto env_var = get_env_direct(env_id);
    if(env_var)
    {
        if(env_var->empty())
        {
            ROCP_FATAL << fmt::format("No boolean value provided for {}", env_id);
        }

        if(env_var->find_first_not_of("0123456789") == std::string_view::npos)
        {
            return static_cast<bool>(std::stoi(*env_var));
        }

        // Convert to lowercase in-place (cast to unsigned char to avoid UB)
        for(size_t i = 0; i < env_var->length(); ++i)
            (*env_var)[i] =
                static_cast<char>(std::tolower(static_cast<unsigned char>((*env_var)[i])));

        for(const auto& itr : {"off", "false", "no", "n", "f", "0"})
            if(*env_var == itr) return false;

        return true;
    }
    return _default;
}

template <typename Tp>
Tp
get_env(std::string_view env_id,
        Tp               _default,
        std::enable_if_t<std::is_integral<Tp>::value || std::is_floating_point<Tp>::value, sfinae>)
{
    static_assert(!std::is_same<Tp, bool>::value, "unexpected! should be using bool overload");
    static_assert(
        sizeof(Tp) <= sizeof(uint64_t),
        "change use of stol/stoul if instantiating for type larger than a 64-bit integer");

    auto env_var = get_env_direct(env_id);
    if(env_var)
    {
        try
        {
            if constexpr(std::is_integral<Tp>::value)
            {
                // use stol/stoul
                if constexpr(std::is_signed<Tp>::value)
                    return static_cast<Tp>(std::stol(*env_var));
                else
                    return static_cast<Tp>(std::stoul(*env_var));
            }
            else if constexpr(std::is_floating_point<Tp>::value)
            {
                return static_cast<Tp>(std::stod(*env_var));
            }
        } catch(std::exception& _e)
        {
            ROCP_ERROR << "[rocprofiler][get_env] Exception thrown converting getenv(\"" << env_id
                       << "\") = " << *env_var << " to " << cxx_demangle(typeid(Tp).name())
                       << " :: " << _e.what() << ". Using default value of " << _default << "\n";
        }
        return _default;
    }
    return _default;
}

int
set_env(std::string_view env_id, bool value, int override)
{
    return set_env_direct(env_id, (value) ? "1" : "0", override);
}

template <typename Tp>
int
set_env(std::string_view env_id,
        Tp               value,  // NOLINT(performance-unnecessary-value-param)
        int              override)
{
    auto str_value = std::stringstream{};
    str_value << value;
    return set_env_direct(env_id, str_value.str(), override);
}

#define SPECIALIZE_GET_ENV(TYPE)                                                                   \
    template TYPE get_env<TYPE>(                                                                   \
        std::string_view,                                                                          \
        TYPE,                                                                                      \
        std::enable_if_t<std::is_integral<TYPE>::value || std::is_floating_point<TYPE>::value,     \
                         sfinae>);

#define SPECIALIZE_SET_ENV(TYPE) template int set_env<TYPE>(std::string_view, TYPE, int);

SPECIALIZE_GET_ENV(int8_t)
SPECIALIZE_GET_ENV(int16_t)
SPECIALIZE_GET_ENV(int32_t)
SPECIALIZE_GET_ENV(int64_t)
SPECIALIZE_GET_ENV(uint8_t)
SPECIALIZE_GET_ENV(uint16_t)
SPECIALIZE_GET_ENV(uint32_t)
SPECIALIZE_GET_ENV(uint64_t)
SPECIALIZE_GET_ENV(float)
SPECIALIZE_GET_ENV(double)

SPECIALIZE_SET_ENV(const char*)
SPECIALIZE_SET_ENV(std::string)
SPECIALIZE_SET_ENV(std::string_view)
SPECIALIZE_SET_ENV(float)
SPECIALIZE_SET_ENV(double)
SPECIALIZE_SET_ENV(int8_t)
SPECIALIZE_SET_ENV(int16_t)
SPECIALIZE_SET_ENV(int32_t)
SPECIALIZE_SET_ENV(int64_t)
SPECIALIZE_SET_ENV(uint8_t)
SPECIALIZE_SET_ENV(uint16_t)
SPECIALIZE_SET_ENV(uint32_t)
SPECIALIZE_SET_ENV(uint64_t)
}  // namespace impl

bool
is_at_secure()
{
    // AT_SECURE is set by the kernel when the program was executed in a way that
    // requires "secure execution" (setuid/setgid, file capabilities, etc.).
    // Cache the value since it cannot change during the lifetime of the process.
#if defined(_WIN32)
    return false;
#else
    static const bool _v = (::getauxval(AT_SECURE) != 0);
    return _v;
#endif
}

env_store::env_store(std::initializer_list<env_config>&& _container)
{
    for(const auto& itr : _container)
    {
        m_original.emplace_back(env_config{itr.env_name, get_env(itr.env_name, ""), 1});
        m_modified.emplace_back(env_config{itr.env_name, itr.env_value, itr.overwrite});
    }
}

env_store::~env_store() { pop(); }

bool
env_store::push()
{
    // not that push ignored bc already pushed
    if(m_pushed) return false;

    for(const auto& itr : m_modified)
        itr();

    m_pushed = true;
    return true;
}

bool
env_store::pop(bool unset_if_empty)
{
    if(!m_pushed) return false;

    for(const auto& itr : m_original)
    {
        auto _current = get_env(itr.env_name, "");
        if(!unset_if_empty && itr.env_value.empty())
            continue;
        else if(_current == itr.env_value)
            continue;
        else if(_current != itr.env_value)
        {
            ROCP_INFO << fmt::format("[rocprofiler][env][pop] {}=\"{}\" => {}=\"{}\"",
                                     itr.env_name,
                                     _current,
                                     itr.env_name,
                                     itr.env_value);
        }
        itr();
    }

    m_pushed = false;
    return true;
}
}  // namespace common
}  // namespace rocprofiler
