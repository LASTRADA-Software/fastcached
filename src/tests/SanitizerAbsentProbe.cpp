// SPDX-License-Identifier: Apache-2.0
//
// Deliberately trivial. `vendor-sanitized` compiles this with `-fno-sanitize=all` in a sanitizer
// configuration and requires its object to carry NO sanitizer init reference: the control that
// proves the check can say "absent". See scripts/check-sanitized-objects.cmake.

namespace FastCache::Testing
{

/// A function, so the control object holds code the way an instrumented one would. Never called.
/// @return Zero.
int SanitizerAbsentProbe() noexcept;

int SanitizerAbsentProbe() noexcept
{
    return 0;
}

} // namespace FastCache::Testing
