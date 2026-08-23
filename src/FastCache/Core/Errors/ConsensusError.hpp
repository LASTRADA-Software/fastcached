// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of consensus-layer errors.
///
/// Deliberately small, and grown one enumerator at a time as a phase of the
/// consensus library actually produces one — the taxonomy is not invented up
/// front.
enum class ConsensusErrorCode : std::uint8_t
{
    InvalidConfiguration = 0, ///< The cluster configuration is not self-consistent.
    NotLeader,                ///< Only a leader may accept a proposal; see `knownLeader`.
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

    /// For `NotLeader`: who to ask instead, when this node knows.
    ///
    /// Carried in the error rather than left for the caller to go and look up,
    /// because the two answers are different and the difference is actionable:
    /// "somebody else leads, ask them" is a redirect, while "nobody leads right
    /// now" means an election is in progress and the caller should fall back
    /// rather than chase it. A bare refusal cannot express the second, and this
    /// system's whole distribution story rests on being able to give up
    /// immediately and compile locally.
    std::optional<std::string> knownLeader;

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
    return ConsensusError { .code = ConsensusErrorCode::InvalidConfiguration,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `NotLeader` error.
/// @param knownLeader Who leads instead, when this node knows.
/// @return The error.
[[nodiscard]] inline ConsensusError NotLeader(std::optional<std::string> knownLeader)
{
    return ConsensusError { .code = ConsensusErrorCode::NotLeader,
                            .context = knownLeader.has_value() ? std::format("not the leader; {} is", *knownLeader)
                                                               : std::string { "not the leader, and none is known" },
                            .knownLeader = std::move(knownLeader) };
}

} // namespace FastCache
