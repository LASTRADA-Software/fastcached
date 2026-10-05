// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/NodeKey.hpp>

/// @file NodeKeyFakes.hpp
/// The one scripted `INodeKeyFileGuard`.
///
/// Shared rather than written per file: every case resolving a key goes through it, and a copy
/// that answered `Protect` differently would make those cases pass whatever the resolver did
/// with the answer.
namespace FastCache::Testing
{

/// What a `ScriptedNodeKeyGuard` answers. Every field is REQUIRED: a case that does not care
/// still states the files it resolves are its owner's alone, so no case is silently judged by a
/// default somebody later changes.
struct NodeKeyGuardScript
{
    SecretExposure found;        ///< What `ExposureOf` answers for a key file that is there.
    SecretExposure afterProtect; ///< What `Protect` answers for a file just created.
    FileOwnerStanding owner;     ///< Whose `OwnerOf` says a key file that is there is.
    DirectoryWriters writers;    ///< What `WritersOf` answers for the state directory.
};

/// An `INodeKeyFileGuard` that answers what it was built with, and records what it was asked.
class ScriptedNodeKeyGuard final: public Node::INodeKeyFileGuard
{
  public:
    /// @param script What every question answers.
    explicit ScriptedNodeKeyGuard(NodeKeyGuardScript script):
        _script { script }
    {
    }

    /// A guard over files and a directory that are their owner's alone, before and after.
    /// @return The guard.
    [[nodiscard]] static ScriptedNodeKeyGuard OwnerOnly()
    {
        return ScriptedNodeKeyGuard { NodeKeyGuardScript { .found = SecretExposure::None,
                                                           .afterProtect = SecretExposure::None,
                                                           .owner = FileOwnerStanding::ThisProcess,
                                                           .writers = DirectoryWriters::OwnerOnly } };
    }

    /// @copydoc Node::INodeKeyFileGuard::ExposureOf
    [[nodiscard]] SecretExposure ExposureOf(std::filesystem::path const& file) override
    {
        _asked.push_back(file);
        return _script.found;
    }

    /// Answer `Another` for every entry named @p name, wherever it is, and the script's owner for
    /// the rest -- how a case plants one file another account wrote beside files that are this
    /// node's own.
    /// @param name The entry's file name.
    void OwnedByAnother(std::filesystem::path name)
    {
        _foreign.push_back(std::move(name));
    }

    /// Answer `IsLink` true for every entry named @p name, wherever it is.
    /// @param name The entry's file name.
    void LinkedEntry(std::filesystem::path name)
    {
        _links.push_back(std::move(name));
    }

    /// Answer `OthersMayWrite` true for every entry named @p name, wherever it is, and false for
    /// the rest.
    /// @param name The entry's file name.
    void WritableByOthers(std::filesystem::path name)
    {
        _writable.push_back(std::move(name));
    }

    /// @copydoc Node::INodeKeyFileGuard::IsLink
    [[nodiscard]] bool IsLink(std::filesystem::path const& entry) override
    {
        return std::ranges::contains(_links, entry.filename());
    }

    /// Answer `OthersMayWrite` with @p error for every entry named @p name, wherever it is: the
    /// platform would not say, which production meets as a failed `lstat` or an unreadable list.
    /// @param name The entry's file name.
    /// @param error What the query failed with.
    void WritersUnanswered(std::filesystem::path name, std::error_code error)
    {
        _unanswered.emplace_back(std::move(name), error);
    }

    /// @copydoc Node::INodeKeyFileGuard::OthersMayWrite
    [[nodiscard]] std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& entry) override
    {
        for (auto const& [name, error]: _unanswered)
            if (name == entry.filename())
                return std::unexpected { error };
        return std::ranges::contains(_writable, entry.filename());
    }

    /// @copydoc Node::INodeKeyFileGuard::OwnerOf
    [[nodiscard]] FileOwner OwnerOf(std::filesystem::path const& file) override
    {
        _owners.push_back(file);
        auto const foreign = std::ranges::contains(_foreign, file.filename());
        return FileOwner { .standing = foreign ? FileOwnerStanding::Another : _script.owner, .name = "scripted-owner" };
    }

    /// @copydoc Node::INodeKeyFileGuard::WritersOf
    [[nodiscard]] DirectoryWriters WritersOf(std::filesystem::path const& directory) override
    {
        _directories.push_back(directory);
        return _script.writers;
    }

    /// @copydoc Node::INodeKeyFileGuard::Protect
    ///
    /// Records the file's SIZE at the moment it was protected, which is how a case asserts that
    /// the secret was not in it yet.
    [[nodiscard]] SecretExposure Protect(std::filesystem::path const& file) override
    {
        auto failure = std::error_code {};
        auto const size = std::filesystem::file_size(file, failure);
        _protected.push_back({ .file = file, .sizeWhenProtected = failure ? UnknownSize : size });
        return _script.afterProtect;
    }

    /// Answer every later `SyncDirectory` with @p failure, as a volume or a failing disk would.
    /// @param failure Which call refused, and what it said.
    void DirectorySyncFails(Consensus::DirectorySyncFailure failure)
    {
        _syncFailure = failure;
    }

    /// @copydoc Node::INodeKeyFileGuard::SyncDirectory
    [[nodiscard]] std::expected<void, Consensus::DirectorySyncFailure> SyncDirectory(
        std::filesystem::path const& directory) override
    {
        _synced.push_back(directory);
        if (_syncFailure.has_value())
            return std::unexpected { *_syncFailure };
        return {};
    }

    /// @return Every directory `SyncDirectory` was asked to sync, in order.
    [[nodiscard]] std::vector<std::filesystem::path> const& Synced() const noexcept
    {
        return _synced;
    }

    /// A size `file_size` could not read.
    static constexpr std::uintmax_t UnknownSize = static_cast<std::uintmax_t>(-1);

    /// One call to `Protect`.
    struct ProtectCall
    {
        std::filesystem::path file;       ///< The file it was asked to protect.
        std::uintmax_t sizeWhenProtected; ///< Its size then, `UnknownSize` when it could not be read.
    };

    /// @return Every file `ExposureOf` was asked about, in order.
    [[nodiscard]] std::vector<std::filesystem::path> const& Asked() const noexcept
    {
        return _asked;
    }

    /// @return Every entry `OwnerOf` was asked about, in order.
    [[nodiscard]] std::vector<std::filesystem::path> const& Owners() const noexcept
    {
        return _owners;
    }

    /// @return Every directory `WritersOf` was asked about, in order.
    [[nodiscard]] std::vector<std::filesystem::path> const& Directories() const noexcept
    {
        return _directories;
    }

    /// @return Every call to `Protect`, in order.
    [[nodiscard]] std::vector<ProtectCall> const& Protected() const noexcept
    {
        return _protected;
    }

  private:
    NodeKeyGuardScript _script;
    std::vector<std::filesystem::path> _asked;
    std::vector<std::filesystem::path> _foreign;
    std::vector<std::filesystem::path> _links;
    std::vector<std::filesystem::path> _writable;
    std::vector<std::pair<std::filesystem::path, std::error_code>> _unanswered;
    std::vector<std::filesystem::path> _owners;
    std::vector<std::filesystem::path> _directories;
    std::vector<ProtectCall> _protected;
    std::vector<std::filesystem::path> _synced;
    std::optional<Consensus::DirectorySyncFailure> _syncFailure;
};

} // namespace FastCache::Testing
