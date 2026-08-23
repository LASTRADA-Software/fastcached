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
    StorageFailure,           ///< Durable state could not be written or read back.

    // Peer-wire decode failures. Three rather than one because a reader's
    // correct response differs: an unknown message type or an unsupported
    // version is a peer running another build, which is stepped over and logged
    // once, while a malformed frame means this reader and that sender disagree
    // about the bytes and the connection is no longer trustworthy.
    MalformedFrame,     ///< The payload does not match the shape its type declares.
    UnknownMessageType, ///< A type byte this build does not know; skip the frame.
    UnsupportedVersion, ///< A frame version outside the range this build decodes.
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

/// Build a `StorageFailure` error.
/// @param context What failed, and where.
/// @return The error.
[[nodiscard]] inline ConsensusError StorageFailure(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::StorageFailure,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `MalformedFrame` error.
/// @param context Which message, and what about it did not parse.
/// @return The error.
[[nodiscard]] inline ConsensusError MalformedWireFrame(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::MalformedFrame,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build an `UnknownMessageType` error.
///
/// Separate from `MalformedFrame` because it is the *expected* condition in a
/// fleet that is mid-upgrade: the frame is well-formed and simply says something
/// this build has no opinion about, so the reader steps over it and carries on.
/// Reporting it as malformed would make a rolling upgrade look like corruption.
/// @param context Which type code, in terms a log line can carry.
/// @return The error.
[[nodiscard]] inline ConsensusError UnknownWireMessage(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::UnknownMessageType,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build an `UnsupportedVersion` error.
/// @param context The offending version and the range that would have worked.
/// @return The error.
[[nodiscard]] inline ConsensusError UnsupportedWireVersion(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::UnsupportedVersion,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

} // namespace FastCache
