// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace FastCache::Detail
{

/// `FastCache::SecureDirectoryForService`, with a hook that runs between its two passes.
///
/// The public function forwards here with an empty hook. It lives in a detail header so the
/// seam does not shape the public signature: only a test has a reason to reach the window
/// between the read-only pre-pass and the apply, and it reaches it by naming this overload,
/// not through a test-only friend or a defaulted public parameter.
/// @param directory An existing directory.
/// @param account The service's account, or empty for a LocalSystem service.
/// @param credentialLeaves Leaf names of credential files in @p directory.
/// @param afterPreCheck Run once the pre-pass has passed and BEFORE the list is applied, so a
///        test can mutate the tree in that window and show the post-apply pass catches it.
///        Empty does nothing.
/// @return As `FastCache::SecureDirectoryForService`.
[[nodiscard]] std::expected<void, std::string> SecureDirectoryForService(
    std::filesystem::path const& directory,
    std::string const& account,
    std::span<std::filesystem::path const> credentialLeaves,
    std::function<void()> const& afterPreCheck);

#if defined(_WIN32)
/// The access list `FastCache::SecureDirectoryForService` gives a service's private directory,
/// as SDDL: SYSTEM and Administrators in full, the directory's owner held to reading the list,
/// and @p account's Modify entry, inherited by what is created inside.
///
/// Its own function so a test can apply the very list an install applies without the right to
/// set an owner, which `SecureDirectoryForService` needs and an unelevated process lacks -- and
/// then create in it as a principal holding nothing but @p account's entry.
/// @param account The service's account, or empty for a LocalSystem service, which the list
///        names already.
/// @return The list, or why @p account did not resolve.
[[nodiscard]] std::expected<std::wstring, std::string> ServiceDirectoryAccessList(std::string const& account);
#endif

} // namespace FastCache::Detail
