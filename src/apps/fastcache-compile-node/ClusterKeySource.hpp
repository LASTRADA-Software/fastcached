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
/// Extracted from `EnrollmentResponder.hpp` when the second reader arrived (#1428): the
/// cluster-key PROOF verifies against the same bytes an approved joiner is handed, and a
/// surface that only needs the key had no business including the enrollment window, the
/// scheduler service and the membership oracle to reach it.
///
/// One reader, and that is the load-bearing part rather than the tidying: `ReadClusterKey`
/// holds the minimum-length rule and the trailing-newline trim, so every tier on this node
/// authenticates against byte-for-byte the same key. A second reader would eventually differ
/// by a newline, which is an HMAC that verifies nowhere and no diagnostic anywhere.

/// Where the cluster key is read from when a joiner is approved.
///
/// **Read where it is PRESENTED, never captured at construction**, which is this
/// project's rule for an outbound credential and is the whole reason this is a seam
/// rather than a `SecureByteBuffer` member. The key is handed over a handful of times
/// in a fleet's life, so holding a second copy of the cluster's secret in this
/// process's memory for its whole uptime buys nothing and costs exactly what the rule
/// is about.
class IClusterKeySource
{
  public:
    IClusterKeySource() = default;
    IClusterKeySource(IClusterKeySource const&) = delete;
    IClusterKeySource(IClusterKeySource&&) = delete;
    IClusterKeySource& operator=(IClusterKeySource const&) = delete;
    IClusterKeySource& operator=(IClusterKeySource&&) = delete;
    virtual ~IClusterKeySource() = default;

    /// The key to hand an approved joiner.
    ///
    /// An `expected` rather than an empty buffer for a failure, because *this node
    /// holds no key* and *the key file has gone* are different facts and only the
    /// second is worth an operator's attention -- and a joiner handed zero bytes under
    /// a successful outcome would write an empty key file and fail much later, on a
    /// machine nobody is watching any more.
    /// @return The key, or why it could not be read.
    [[nodiscard]] virtual std::expected<SecureByteBuffer, std::string> ClusterKey() const = 0;
};

/// `IClusterKeySource` reading `--cluster-key-file` at each hand-over.
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
    /// minimum-length rule and the trailing-newline trim, so a key handed to a joiner
    /// is byte-for-byte the key every other tier on this node already uses. A second
    /// reader would eventually differ by a newline, which is an HMAC that verifies
    /// nowhere and no diagnostic anywhere.
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override;

  private:
    std::filesystem::path _path;
};

} // namespace FastCache::Node
