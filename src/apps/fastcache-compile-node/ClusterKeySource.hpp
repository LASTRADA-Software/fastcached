// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/SecureBytes.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <utility>

namespace FastCache::Node
{

/// @file ClusterKeySource.hpp
/// Where this node's `--cluster-key-file` is read from, at the moment it is used.
///
/// The cluster's pre-shared key still signs lease grants and proves a caller on the node port
/// (#1428), until #178 retires it. It no longer crosses an enrollment and no longer proves a
/// discovery beacon: both moved to each node's own identity key, which is why the reader lives
/// here and not beside the discovery tier any more.
///
/// One reader, and that is the load-bearing part rather than the tidying: `ReadClusterKey`
/// holds the minimum-length rule and the trailing-newline trim, so every tier on this node
/// authenticates against byte-for-byte the same key. A second reader would eventually differ
/// by a newline, which is an HMAC that verifies nowhere and no diagnostic anywhere.

/// Read the cluster's pre-shared key from a file.
///
/// **A file rather than a flag, and that is the whole reason this function
/// exists.** A command line is world-readable through `ps` on every POSIX system
/// and through the process list on Windows, and a service's arguments end up in a
/// unit file or a registry key that more accounts can read than can read a
/// mode-0600 file. A key that leaks admits a node to the fleet, and an admitted
/// node is assigned compile jobs and returns objects cached fleet-wide -- so it is
/// object injection into everybody's build, which is why the key never travels
/// anywhere it does not have to.
///
/// Trailing whitespace is stripped, because the overwhelmingly common way to
/// produce one of these is `... > key` or an editor that ends files with a newline
/// -- and a key that differs from its peers' by one byte fails to authenticate with
/// a message about a bad proof rather than about a newline.
/// @param path Where the key is.
/// @return The key bytes, or why the file cannot serve as one.
[[nodiscard]] std::expected<SecureByteBuffer, std::string> ReadClusterKey(std::filesystem::path const& path);

/// Where the cluster key is read from when a caller proves it holds it.
///
/// **Read where it is USED, never captured at construction**, which is this project's rule
/// for a credential and is the whole reason this is a seam rather than a `SecureByteBuffer`
/// member: holding a second copy of the cluster's secret in this process's memory for its
/// whole uptime buys nothing and costs exactly what the rule is about.
class IClusterKeySource
{
  public:
    IClusterKeySource() = default;
    IClusterKeySource(IClusterKeySource const&) = delete;
    IClusterKeySource(IClusterKeySource&&) = delete;
    IClusterKeySource& operator=(IClusterKeySource const&) = delete;
    IClusterKeySource& operator=(IClusterKeySource&&) = delete;
    virtual ~IClusterKeySource() = default;

    /// The key, as the file holds it now.
    ///
    /// An `expected` rather than an empty buffer for a failure, because *this node
    /// holds no key* and *the key file has gone* are different facts and only the
    /// second is worth an operator's attention -- and an empty key would authenticate
    /// every caller that also presents an empty one.
    /// @return The key, or why it could not be read.
    [[nodiscard]] virtual std::expected<SecureByteBuffer, std::string> ClusterKey() const = 0;
};

/// `IClusterKeySource` reading `--cluster-key-file` at each use.
class FileClusterKeySource final: public IClusterKeySource
{
  public:
    /// @param path Where the key lives; empty means this node holds none.
    explicit FileClusterKeySource(std::filesystem::path path) noexcept:
        _path { std::move(path) }
    {
    }

    /// @copydoc IClusterKeySource::ClusterKey
    ///
    /// Through `ReadClusterKey`, which is the one reader in this binary: it holds the
    /// minimum-length rule and the trailing-newline trim, so the key a proof is checked
    /// against is byte-for-byte the key every other tier on this node already uses. A
    /// second reader would eventually differ by a newline, which is an HMAC that verifies
    /// nowhere and no diagnostic anywhere.
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override;

  private:
    std::filesystem::path _path;
};

} // namespace FastCache::Node
