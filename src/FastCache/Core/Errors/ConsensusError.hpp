// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of consensus-layer errors.
///
/// Deliberately small, and grown one enumerator at a time as a phase of the
/// consensus library actually produces one — the taxonomy is not invented up
/// front. Today the library has exactly one way to refuse a caller: a cluster
/// configuration that cannot be run.
enum class ConsensusErrorCode : std::uint8_t
{
    InvalidConfiguration = 0, ///< The cluster configuration is not self-consistent.
};

/// Structured consensus error.
struct ConsensusError
{
    ConsensusErrorCode code = ConsensusErrorCode::InvalidConfiguration;

    /// What specifically was wrong, in terms an operator can act on.
    ///
    /// Carried rather than derived from `code`, because every current use is a
    /// configuration rejection and "invalid configuration" on its own tells
    /// somebody editing a file nothing about which field to look at.
    std::string context;

    /// Render for a log line or a startup refusal.
    /// @return The formatted error.
    [[nodiscard]] std::string ToString() const
    {
        return std::format("ConsensusError(code={} context={})", static_cast<unsigned>(code), context);
    }
};

/// Build an `InvalidConfiguration` error.
/// @param context What was wrong with it.
/// @return The error.
[[nodiscard]] inline ConsensusError InvalidConfiguration(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::InvalidConfiguration, .context = std::string { context } };
}

} // namespace FastCache
