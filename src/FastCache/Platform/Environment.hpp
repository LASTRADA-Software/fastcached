// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// Read a variable from the process environment.
///
/// The single place the FastCache library reads the environment. Every caller
/// previously spelled its own `#if defined(_WIN32)` / `getenv_s` /
/// `std::getenv` dance, which was three chances to get the Windows length
/// convention wrong (getenv_s reports the length *including* the NUL, so 0
/// means absent and 1 means present but empty).
///
/// `src/apps/fastcache-cc` still has copies of that dance, deliberately: the
/// launcher does not link this library (see its CMakeLists) so that it stays
/// free of vcpkg dependencies and can link the CRT statically.
///
/// Anything with a decision to test reaches the environment through an injected
/// seam instead — `SystemConfigPathProbe::GetEnv` is this function wrapped in
/// one. Calling it directly is for leaf probes that have no logic worth faking:
/// `NoColorRequested` in Terminal.cpp and `MetricsPortFromEnv` in main.cpp are
/// the whole set, and both are a read plus a comparison.
///
/// Deliberately NOT named `GetEnvironmentVariable`: `<windows.h>` defines that
/// as a macro expanding to `GetEnvironmentVariableA`/`W`, so any translation
/// unit including both headers would fail to compile.
///
/// @param name Variable name.
/// @return The value; an empty string when the variable is set but empty; and
///         `std::nullopt` when it is not set at all. The distinction matters:
///         an unset variable means "this location does not apply", while a set
///         but empty one is a deliberate value.
[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(std::string_view name);

} // namespace FastCache
