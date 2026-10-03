// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/StateFiles.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/// @file RosterStore.hpp
/// Where a machine that runs no consensus keeps the roster it adopted (#178).
///
/// Without it a restart would fall back to the trust anchors typed on `--voter-key`, which may
/// name a voter the cluster has since revoked -- the one thing a certified roster exists to
/// stop that voter doing. So a roster, once adopted, is kept, and a kept roster that cannot be
/// read refuses the start rather than being quietly replaced by the anchors.
namespace FastCache::Distributed
{

/// Where an adopted roster is kept.
class IRosterStore
{
  public:
    IRosterStore() = default;
    IRosterStore(IRosterStore const&) = delete;
    IRosterStore(IRosterStore&&) = delete;
    IRosterStore& operator=(IRosterStore const&) = delete;
    IRosterStore& operator=(IRosterStore&&) = delete;
    virtual ~IRosterStore() = default;

    /// Replace what is kept with @p roster.
    /// @param roster The roster just adopted.
    /// @return Nothing, or why it could not be kept.
    [[nodiscard]] virtual std::expected<void, std::string> Save(Cluster::PersistedRoster const& roster) = 0;
};

/// The file a state directory keeps its roster in.
inline constexpr std::string_view RosterFileName = StateFileName(StateFile::Roster);

/// The roster kept in one file, replaced indivisibly on every save.
class FileRosterStore final: public IRosterStore
{
  public:
    /// @param path The file, normally `<state directory>/roster`.
    explicit FileRosterStore(std::filesystem::path path);

    [[nodiscard]] std::expected<void, std::string> Save(Cluster::PersistedRoster const& roster) override;

  private:
    std::filesystem::path _path;
};

/// Which half of reading a kept roster failed.
///
/// The two halves are different kinds of answer: a READ that failed says nothing about the roster
/// and may succeed at the next start (a permission restored, a mount back), while a roster that
/// was read and cannot be used is a verdict on its bytes that the next start reaches again.
///
/// Private: never transmitted or persisted.
enum class RosterLoadStep : std::uint8_t
{
    Read,   ///< The file is there and could not be read.
    Decode, ///< The file was read, and is not a roster this build can use.
};

/// Why a kept roster cannot be used, and which half said so.
struct RosterLoadError
{
    RosterLoadStep step { RosterLoadStep::Read }; ///< Which half failed.
    std::string reason;                           ///< What an operator is told, naming the file.
};

/// Read the roster kept at @p path.
///
/// Three answers, because two of them must not be confused: NO FILE is a machine that has not
/// adopted a roster yet and starts from its anchors; a file that cannot be read, is not a
/// roster, or is another build's layout is a roster this machine DID adopt and cannot now use,
/// and that is a refusal -- replacing it with the anchors would hand a say back to whichever of
/// them the cluster revoked since.
/// @param path The file.
/// @return The kept roster, nothing when there is no file, or why the file cannot be used and
///         which half of reading it said so.
[[nodiscard]] std::expected<std::optional<Cluster::PersistedRoster>, RosterLoadError> LoadPersistedRoster(
    std::filesystem::path const& path);

} // namespace FastCache::Distributed
