// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/StateFiles.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

/// @file FleetEndpoints.hpp
/// Every voter's endpoint this node last knew about, kept in its state directory so a node that
/// falls off the office LAN can walk back to the fleet it joined without waiting on a DNS record
/// or an operator retyping `--fleet-seed`.
///
/// A hint file, in the sense `FleetHistory`'s file is one: no state of it may keep a node from
/// starting, and a file a LATER build wrote is kept and never overwritten -- a node rolled back
/// to an older build must not destroy endpoints a newer one recorded and could still read.
namespace FastCache::Cluster
{

/// One voter's endpoints, as they were last known.
struct FleetEndpoint
{
    Consensus::NodeId id;     ///< The voter's stable identity.
    std::string raftEndpoint; ///< Where its consensus port answers.
    std::string nodeEndpoint; ///< Where a client dials it -- what a seed walk uses.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FleetEndpoint const&, FleetEndpoint const&) = default;
};

/// A fleet's voters, as this node last knew them.
struct FleetEndpoints
{
    std::string clusterId;             ///< The fleet's id.
    std::vector<FleetEndpoint> voters; ///< Its voters, in record order.

    /// Field-wise equality, so a round trip is asserted whole.
    [[nodiscard]] friend bool operator==(FleetEndpoints const&, FleetEndpoints const&) = default;
};

/// The layout `FleetEndpointsFile` writes. A file above it was written by a later build and is
/// never overwritten; a file below it is a layout this build never wrote and is unreadable.
inline constexpr std::uint8_t FleetEndpointsFormat = 1;

/// The file a state directory keeps remembered fleet endpoints in.
inline constexpr std::string_view FleetEndpointsFileName = StateFileName(StateFile::FleetEndpoints);

/// What reading the file found.
///
/// **Private.** A missing file, a damaged file and a file a later build wrote are three
/// different outcomes -- and none of them may keep a node from starting, so this is reported
/// rather than folded into a bare failure.
enum class FleetEndpointsLoad : std::uint8_t
{
    Absent,     ///< No file was ever written here.
    Loaded,     ///< Read back exactly what `Save` wrote.
    LaterBuild, ///< Intact, but in a layout newer than this build writes; never overwritten.
    Unreadable, ///< Present but damaged, or in a layout older than this build understands.
    Last,
};

/// What `FleetEndpointsFile::Load` returns.
struct LoadedFleetEndpoints
{
    FleetEndpointsLoad outcome { FleetEndpointsLoad::Absent }; ///< What reading the file found.
    FleetEndpoints endpoints;                                  ///< Populated only when `Loaded`.
};

/// The remembered-endpoints file kept in a node's state directory.
class FleetEndpointsFile
{
  public:
    /// @param directory The state directory; created on the first save when it does not exist.
    explicit FleetEndpointsFile(std::filesystem::path directory);

    /// Read the file back.
    /// @return What was found; see `FleetEndpointsLoad`.
    [[nodiscard]] LoadedFleetEndpoints Load();

    /// Replace the file with @p endpoints.
    ///
    /// Refused, leaving the file untouched, once `Load` found a file a later build wrote -- see
    /// `ReadOnly`. A damaged file is not such a refusal: the next save replaces it, since damage
    /// is not a claim about which build wrote it.
    /// @param endpoints What to remember.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] std::expected<void, ConsensusError> Save(FleetEndpoints const& endpoints);

    /// Whether this file refuses to write, because `Load` found one a later build wrote.
    /// @return True when `Save` will decline.
    [[nodiscard]] bool ReadOnly() const noexcept;

  private:
    std::filesystem::path _directory;
    mutable std::mutex _mutex;
    bool _readOnly { false };
};

/// Every remembered endpoint a seed walk may dial: each voter's node endpoint, in record order.
/// @param endpoints What `FleetEndpointsFile::Load` returned.
/// @return The node endpoints, in the order their voters were recorded.
[[nodiscard]] std::vector<std::string> RememberedSeeds(FleetEndpoints const& endpoints);

} // namespace FastCache::Cluster
