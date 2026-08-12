// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/Environment.hpp>

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

/// Shared helpers for tests that need a scripted process environment. Header-
/// only so each test translation unit gets its own `inline` copies without an
/// extra link target, the same arrangement as Cache/StorageTestUtils.hpp.
namespace FastCache::Testing
{

/// Set and then clear an environment variable for the duration of a test.
///
/// Windows' `_putenv_s` and POSIX' `setenv`/`unsetenv` differ enough that
/// tests would otherwise repeat the `#if` in every case — and this is the one
/// place allowed to write the environment directly, since faking the thing
/// under test would assert nothing.
///
/// **A variable cannot be set to the empty string on Windows.** `_putenv_s`
/// with `""` *removes* it, and so does `SetEnvironmentVariable`; there is no
/// call that produces a present-but-empty variable. Cases that turn on that
/// distinction are therefore POSIX-only, however much
/// ReadEnvironmentVariable's contract has to describe it.
class ScopedEnv
{
  public:
    /// @param name Variable to set for the lifetime of this object.
    /// @param value Value to set it to.
    ScopedEnv(std::string name, std::string const& value):
        _name { std::move(name) },
        _previous { ReadEnvironmentVariable(_name) }
    {
        Assign(value);
    }

    /// Puts back whatever was there, rather than merely removing the variable.
    /// The difference bites on exactly the variables worth scripting — `HOME`,
    /// `ProgramData`, `XDG_CONFIG_HOME` — where deleting one for the rest of the
    /// test binary strands every later case that reads it, as a failure whose
    /// appearance depends on Catch2's ordering.
    ~ScopedEnv()
    {
        if (_previous.has_value())
            Assign(*_previous);
        else
            Remove();
    }

    ScopedEnv(ScopedEnv const&) = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

  private:
    /// @param value Value to write.
    void Assign(std::string const& value) const
    {
#if defined(_WIN32)
        ::_putenv_s(_name.c_str(), value.c_str());
#else
        ::setenv(_name.c_str(), value.c_str(), 1);
#endif
    }

    void Remove() const
    {
#if defined(_WIN32)
        // An empty value is how the Windows CRT removes a variable.
        ::_putenv_s(_name.c_str(), "");
#else
        ::unsetenv(_name.c_str());
#endif
    }

    std::string _name;
    std::optional<std::string> _previous;
};

} // namespace FastCache::Testing
