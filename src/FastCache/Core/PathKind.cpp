// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/PathKind.hpp>

#include <filesystem>
#include <system_error>

namespace FastCache
{

bool PathNamesAFile(std::filesystem::path const& path) noexcept
{
    // The non-throwing overloads throughout. One caller is the install-time
    // handover, where an unreadable parent must not turn a best-effort grant into
    // an exception thrown out of the service registration.
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return false; // an existing directory
    if (std::filesystem::exists(path, ec))
        return true; // an existing regular file

    // Nothing there yet, so the spelling is the only evidence.
    return path.has_extension();
}

} // namespace FastCache
