// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file ScopedUmask.hpp
/// The process umask for one scope, restored on every way out of it (POSIX only).
///
/// Shared because the umask is process-wide: a case that set it and did not restore it would
/// change the mode of every file every later case in the same binary creates.

#if !defined(_WIN32)
    #include <sys/stat.h>

namespace FastCache::Testing
{

/// The umask for a scope.
class ScopedUmask
{
  public:
    /// @param mask The umask for the scope.
    explicit ScopedUmask(::mode_t mask):
        _previous { ::umask(mask) }
    {
    }

    ScopedUmask(ScopedUmask const&) = delete;
    ScopedUmask(ScopedUmask&&) = delete;
    ScopedUmask& operator=(ScopedUmask const&) = delete;
    ScopedUmask& operator=(ScopedUmask&&) = delete;

    ~ScopedUmask()
    {
        ::umask(_previous);
    }

  private:
    ::mode_t _previous; ///< What the umask was before the scope.
};

} // namespace FastCache::Testing
#endif
