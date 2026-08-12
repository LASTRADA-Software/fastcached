// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>

#include <cstddef>
#include <cstdlib>

namespace FastCache
{

std::optional<std::string> ReadEnvironmentVariable(std::string_view name)
{
    // Both platform APIs take a NUL-terminated name, and string_view is not.
    std::string const key { name };

#if defined(_WIN32)
    // The secure CRT form, so the build stays warning-clean under /WX.
    // getenv_s reports the length INCLUDING the NUL: 0 means not present.
    std::size_t size = 0;
    if (::getenv_s(&size, nullptr, 0, key.c_str()) != 0 || size == 0)
        return std::nullopt;

    // The size check again, on the fetching call: getenv_s reports 0 for a
    // variable it did not find, and between the two calls it can genuinely go
    // missing. Without this, `size - 1` underflows and resize() is asked for
    // the whole address space.
    std::string value(size, '\0');
    if (::getenv_s(&size, value.data(), size, key.c_str()) != 0 || size == 0)
        return std::nullopt;
    value.resize(size - 1);
    return value;
#else
    char const* const value = std::getenv(key.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string { value };
#endif
}

} // namespace FastCache
