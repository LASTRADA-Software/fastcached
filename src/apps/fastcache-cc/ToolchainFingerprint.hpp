// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// One header in the toolchain's own include tree, as the fingerprint sees it.
struct ToolchainFile
{
    /// Path **relative to the include root it was found under**, `/`-separated.
    ///
    /// Relative, not absolute, and that is the whole design. Two machines running
    /// the same toolchain at different install prefixes — `/usr/lib/gcc/...` and
    /// `/opt/toolchains/gcc-13/...`, or a vendored SDK checked out at two depths —
    /// must produce the *same* fingerprint, or distribution is disabled between
    /// exactly the machines it exists to connect. An absolute path would make the
    /// prefix part of the identity and split the fleet by install location.
    std::string relativePath;

    /// Digest of the file's contents.
    ///
    /// Contents, never mtime or size. Install times differ on every machine, so an
    /// mtime-derived fingerprint would make every pair of machines mismatch — and
    /// it would do so *silently*, presenting as "distribution never finds a
    /// worker" rather than as an error. Size alone is far too weak: two headers
    /// differing by one character are the case that matters.
    std::string contentHash;
};

/// Schema tag for the fingerprint digest.
///
/// Versioned like the cache keys are, and for the same reason: if the *inputs*
/// ever change shape — a different set of roots, a different hash — old and new
/// launchers must not agree by accident on a value that means different things.
/// A fingerprint mismatch merely disables distribution, so a bump here is cheap;
/// a false *match* would dispatch to the wrong toolchain, which is the failure
/// this whole mechanism exists to prevent.
inline constexpr std::string_view FingerprintSchema = "toolchain-v1";

/// Compute a toolchain fingerprint from a compiler identity and its include tree.
///
/// **Pure**: it touches no filesystem and reads no clock. Gathering the file list
/// is the caller's job (see `ProbeToolchainFiles`), which is what lets every rule
/// below be a unit test rather than a fixture with a real compiler installed.
///
/// The banner is folded in as well as the files, because the two answer different
/// questions and neither subsumes the other. A banner alone is what the cache key
/// already uses, and it is too weak here: two machines can print an identical
/// `--version` while resolving different libstdc++ headers, and for *distribution*
/// that produces a silently wrong object rather than the stale path a replay guard
/// can probe. The headers alone are too weak in the other direction: two compilers
/// can share a header tree and generate different code from it.
///
/// The file list is sorted before digesting, so the caller's traversal order — a
/// directory iteration order, which is a property of the filesystem rather than of
/// the toolchain — cannot change the answer.
///
/// @param compilerBanner The compiler's own version line, verbatim.
/// @param files The toolchain's include tree; order does not matter.
/// @return An opaque fingerprint, stable across machines with the same toolchain.
[[nodiscard]] std::string ComputeToolchainFingerprint(std::string_view compilerBanner, std::vector<ToolchainFile> files);

} // namespace FastCache::Cc
