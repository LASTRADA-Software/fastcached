// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Which root of the Windows registry a lookup starts from.
///
/// Two, because two is what locating a toolchain needs: a machine-wide install
/// writes under `HKEY_LOCAL_MACHINE` and a per-user one under
/// `HKEY_CURRENT_USER`, and LLVM's installer offers both. A caller that does not
/// know which it is looking at asks for each in turn rather than being given a
/// third enumerator meaning "either" -- "search both" is a policy, and policies
/// belong above this file.
enum class RegistryHive : std::uint8_t
{
    LocalMachine, ///< HKEY_LOCAL_MACHINE.
    CurrentUser,  ///< HKEY_CURRENT_USER.
};

/// Which of a 64-bit Windows' two registry views a lookup reads.
///
/// Not a detail that can be left to the default. A 64-bit process reads the
/// 64-bit view, and the two keys this exists for -- `Windows Kits\Installed
/// Roots` and `LLVM\LLVM` -- are written by 32-bit installers, so they land in
/// `WOW6432Node` and a native read finds nothing at all. The failure is silent:
/// no error, just a value that is not there, and a toolchain that is never
/// discovered on a machine that has it.
enum class RegistryView : std::uint8_t
{
    Native,       ///< The calling process's own view.
    ThirtyTwoBit, ///< The 32-bit view (`WOW6432Node`), whatever this process is.
};

/// Read a string value from the Windows registry.
///
/// The single place the registry is read, for the reason
/// `ReadEnvironmentVariable` is the single place the environment is read: the
/// two-call size dance, the `REG_EXPAND_SZ` expansion and the WOW64 view are
/// each a chance to get it wrong, and one copy is one chance rather than one per
/// caller. Anything with a decision to test reaches it through an injected seam
/// instead -- `IToolchainHost::RegistryString` is this function wrapped in one.
///
/// `REG_EXPAND_SZ` is expanded before it is returned, because every value this
/// serves names a directory and `%ProgramFiles%\LLVM` is not a path anyone can
/// open. `REG_SZ` is returned verbatim. Any other type is reported as absent
/// rather than reinterpreted: a `REG_DWORD` read as characters is garbage, and
/// garbage that looks like a path is worse than nothing.
///
/// Compiled on every platform and always empty off Windows, so no caller needs
/// a `#if` of its own -- the same arrangement `Platform/CpuAffinity` uses.
///
/// @param hive Which root to start from.
/// @param subKey Key path beneath it, backslash-separated, no leading separator.
/// @param valueName The value to read; empty names the key's default value.
/// @param view Which of a 64-bit host's two views to read.
/// @return The value, or `std::nullopt` when the key, the value, or the whole
///         registry (off Windows) is absent.
[[nodiscard]] std::optional<std::string> ReadRegistryString(RegistryHive hive,
                                                            std::string_view subKey,
                                                            std::string_view valueName,
                                                            RegistryView view);

/// List the value names directly under a registry key.
///
/// Needed because one key this locates a toolchain through is a *set*: the
/// Windows SDK records each installed kit as a value under `Installed Roots`,
/// so "which SDK versions are here" cannot be answered by reading a value whose
/// name is already known.
///
/// The key's default value has an empty name and is reported as such rather than
/// skipped, so a caller counting entries sees what is really there.
///
/// @param hive Which root to start from.
/// @param subKey Key path beneath it, backslash-separated, no leading separator.
/// @param view Which of a 64-bit host's two views to read.
/// @return The value names in the order the registry enumerated them; empty when
///         the key is absent, has no values, or the host has no registry.
[[nodiscard]] std::vector<std::string> ListRegistryValueNames(RegistryHive hive, std::string_view subKey, RegistryView view);

} // namespace FastCache
