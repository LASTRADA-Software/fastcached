// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ITcpClient.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// How a cache round trip ended.
///
/// `Miss` and `Rejected` are deliberately separate. They were the same byte on
/// the pre-version wire, which meant a launcher talking to a daemon that could
/// not serve it saw an endlessly cold cache and reported nothing — the build
/// merely got slower, forever, with no clue why. Telling them apart is the whole
/// reason the wire carries a status space and an error code.
enum class CacheOutcomeKind : std::uint8_t
{
    Hit,       ///< The value was served. `value` holds it.
    Miss,      ///< The daemon has no such entry. Not an error.
    Rejected,  ///< The daemon refused the command. `code` and `message` say why.
    Transport, ///< The exchange never completed (socket error, short read).
};

/// The result of one FETCH or STORE.
struct CacheOutcome
{
    CacheOutcomeKind kind { CacheOutcomeKind::Transport };
    std::vector<std::byte> value;                                                     ///< Payload on a hit.
    CompileCacheWire::ErrorCode code { CompileCacheWire::ErrorCode::MalformedFrame }; ///< Valid when `kind == Rejected`.
    std::string message; ///< The daemon's own words, when it refused.

    /// True when a credential was presented and the daemon did not understand the
    /// AUTH verb at all — i.e. it predates authentication on this protocol.
    ///
    /// The exchange still succeeds (such a daemon steps over the unknown verb and
    /// serves the command behind it), so this is not an error and must not be
    /// treated as one. It is reported because the operator asked for something
    /// that did not happen: they set a token, and this traffic went unauthenticated.
    /// Discovering that from a security review rather than from the tool is the
    /// silent no-op this codebase keeps a list about.
    bool credentialIgnored { false };

    /// @return True when the daemon served a value.
    [[nodiscard]] bool IsHit() const noexcept
    {
        return kind == CacheOutcomeKind::Hit;
    }
};

/// Describe an outcome for a diagnostic line — the daemon's own code and words
/// when it refused, so a launcher can say *why* a build is not caching rather
/// than only that it is not.
/// @param outcome The outcome to describe.
/// @return A short human-readable phrase; empty for a hit.
[[nodiscard]] std::string DescribeOutcome(CacheOutcome const& outcome);

/// The credential this launcher presents, if any.
///
/// An empty `secret` means "no credential configured", and every exchange below
/// then sends exactly the bytes it always did. That is what keeps a launcher that
/// has never heard of authentication byte-compatible on the wire with one that
/// has: the AUTH frame exists only when there is something to put in it.
struct Credential
{
    std::string username; ///< Empty selects the default user (the `requirepass` form).
    std::string secret;   ///< Empty means no credential is configured.

    /// @return True when a credential should be presented.
    [[nodiscard]] bool Configured() const noexcept
    {
        return !secret.empty();
    }
};

/// FETCH one key over an already-connected client.
///
/// When `credential` is configured, an AUTH frame is **pipelined** ahead of the
/// FETCH — both are written before either reply is read — so authenticating costs
/// bytes but no extra round trip. This matters because the launcher opens a fresh
/// connection per operation, so a wait-for-AUTH-then-send spelling would double
/// the round trips of every translation unit in a build. See the note in
/// `CompileCacheWire.hpp`.
///
/// A rejected credential surfaces as the *fetch's* outcome (`Rejected` /
/// `Unauthenticated`), because that is the answer the caller acts on; the AUTH
/// reply is consumed first so the two never desynchronise.
///
/// @param client Connected transport; not owned.
/// @param key The key to look up.
/// @param credential Credential to present; default-constructed sends none.
/// @return The outcome; `value` holds the stored bytes on a hit.
[[nodiscard]] CacheOutcome CacheFetch(ITcpClient& client, std::string_view key, Credential const& credential = {});

/// STORE one entry over an already-connected client.
///
/// Pipelines AUTH the same way `CacheFetch` does, for the same reason.
/// @param client Connected transport; not owned.
/// @param request The fields to send.
/// @param credential Credential to present; default-constructed sends none.
/// @return The outcome; `kind == Hit` means the daemon acknowledged the write.
[[nodiscard]] CacheOutcome CacheStore(ITcpClient& client,
                                      CompileCacheWire::StoreRequest const& request,
                                      Credential const& credential = {});

/// Default ceiling on a value the launcher will offer to the daemon.
///
/// 256 MiB, matching the daemon's own `--storage-max-value` default, so that out
/// of the box the launcher does not spend a build's time pushing something the
/// other side is certain to refuse. The two are separate settings on separate
/// processes that never negotiate -- the wire has no handshake by design -- so
/// this agreement is a chosen default, not a derived one, and either side can be
/// retuned without the other noticing.
// `ULL`, not `UL`: `unsigned long` is 32 bits on Win64 (LLP64), so a future
// edit raising this past 4 GiB would silently wrap there and nowhere else.
inline constexpr std::size_t DefaultMaxStoreBytes = 256ULL * 1024ULL * 1024ULL;

/// Whether a value of `valueBytes` is worth offering to the daemon at all.
///
/// A single object file can be enormous -- a C++23 translation unit built with
/// `-g` reached 356 MB in the report that prompted this -- and one entry that
/// size would dominate a cache sized for thousands of ordinary objects. Past the
/// limit the launcher stores nothing: the compile has already succeeded, so the
/// build is unaffected and only this one result stays uncached.
///
/// This is a *client* policy, deliberately checked before connecting rather than
/// left for the daemon to refuse. Sending it anyway costs the transfer on every
/// rebuild of that translation unit, and pays it to be told no.
/// @param valueBytes Size of the encoded value.
/// @param limitBytes The ceiling; **0 means no limit**, not "store nothing".
/// @return True when the value may be sent.
[[nodiscard]] constexpr bool IsStorableSize(std::size_t valueBytes, std::size_t limitBytes) noexcept
{
    return limitBytes == 0 || valueBytes <= limitBytes;
}

} // namespace FastCache::Cc
