// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeReload.hpp"

#include <CacheProtocol.hpp>

namespace FastCache::Node
{

/// What this worker PRESENTS to its peers, asked afresh at every exchange.
///
/// ## Outbound, and that is why the daemon's answer does not transfer
///
/// `fastcached` REQUIRES a credential of its clients and answers that question with
/// a shared auth source behind its protocol handlers. This worker does the opposite:
/// `--requirepass` here is presented to the shared cache, to the scheduler and to a
/// cluster verb, and is never checked against anybody. The two are different shapes
/// of the same word, and reaching for the daemon's type would put an inbound policy
/// object on an outbound path
/// ([#404](https://github.com/LASTRADA-Software/fastcached/issues/404)).
///
/// ## Why a seam rather than three values
///
/// The secret is a shared one across a fleet, so rotating it is an operator task
/// rather than an emergency, and it used to mean restarting every worker. Making it
/// reloadable is only half the job: three call sites took a **copy** at construction,
/// and a rotation reaching two of them is a process running a configuration no file
/// describes -- in the one area where the symptom is an authentication failure
/// nobody can reproduce, on a machine nobody is watching.
///
/// So there is one place a `Cc::Credential` is derived from a `NodeConfig`, and every
/// site that presents one holds a reference to this interface instead of a value.
/// That is the type system doing the work: a reference of this type cannot go stale,
/// because it holds no secret to be stale about. A site that wanted a fixed one would
/// have to construct a `Cc::Credential` of its own, which is what the seam case in
/// `NodeCredential_test.cpp` refuses -- `ctest -R node-credential-tests` or, directly,
/// the `[node][credential][seam]` tag. The guard is a scan rather than the type, because
/// nothing forces a site to reach for the seam at all, and forgetting to must not read
/// the same as deciding not to.
///
/// **Named by its TAG and not by a phrase, because the phrase sent a reader looking for
/// a check that does not exist.** This said `node-credential-seam`, which matches no
/// ctest registration and no script in the tree -- so somebody grepping for it finds two
/// comments, nine unrelated `*-seam` checks, and concludes the hazard is unguarded. It is
/// guarded; a scan implemented as a Catch2 case walking the sources is simply invisible
/// to a search shaped for `scripts/check-*`.
///
/// Implementations must be safe to call from several threads at once: the heartbeat
/// thread, the node's reactor and the process main thread each reach one.
class ICredentialSource
{
  public:
    virtual ~ICredentialSource() = default;

    ICredentialSource() = default;
    ICredentialSource(ICredentialSource const&) = default;
    ICredentialSource& operator=(ICredentialSource const&) = default;
    ICredentialSource(ICredentialSource&&) = default;
    ICredentialSource& operator=(ICredentialSource&&) = default;

    /// The credential to present right now.
    ///
    /// **Returned by value, and it OWNS.** A `Cc::Credential` decides whether a peer
    /// authenticates, so a view into a snapshot this call does not keep alive is the
    /// shape of borrow this repository has already paid for three times -- and the
    /// worst of the three was the one whose borrowed field decided a trust question
    /// rather than an arithmetic one. The copy is two small strings against a network
    /// round trip.
    /// @return The credential; an empty `secret` means none is configured.
    [[nodiscard]] virtual Cc::Credential Current() const = 0;
};

/// The credential this node's configuration names, now.
///
/// The one place a `Cc::Credential` is derived from a `NodeConfig`, which is what
/// makes "three sites" a property of the type system rather than a census somebody
/// has to keep re-taking.
///
/// **The reloader is nullable, and the two arms are two real deployments rather than
/// a convenience.** A worker started with a configuration file reads the live
/// snapshot, so a rotation reaches every site at once. A worker started with none has
/// no second moment at which anything could be re-read -- and neither do the cluster
/// admin verbs, which run before any reloader exists and exit without serving. Those
/// still take this seam rather than a bare `Cc::Credential`, because "the site that
/// did not need it" is exactly how three sites came to be two-thirds correct.
///
/// Reads the snapshot on every call rather than subscribing to it. That is the
/// cheaper half of a rule this binary already states: `ConfigReloaderOf::Subscribe`
/// has no unsubscribe, so a subscriber capturing a worker-scoped object outlives what
/// it captured. Nothing is captured here -- the snapshot is fetched, read and dropped
/// -- so this may live wherever it is convenient, as long as the reloader outlives it.
///
/// A reload that is DECLINED changes nothing, because a declined reload publishes no
/// snapshot.
class ConfiguredCredential final: public ICredentialSource
{
  public:
    /// @param startup What the process was started with. Presented for as long as
    ///        @p reloader is null, and dead weight when it is not -- the reloader's
    ///        first snapshot IS this configuration.
    /// @param reloader Where the live configuration lives, or **null** when this
    ///        process has no configuration file and therefore no second moment.
    ///        Borrowed; must outlive this object.
    ConfiguredCredential(NodeConfig const& startup, NodeReloader const* reloader):
        // `std::string` at the boundary (#1125). NOT because `CacheProtocol.hpp` cannot
        // name a `Core/` type -- it already includes `Net/ISocket.hpp`, and
        // `Core/SecureBytes.cpp` is already a `_fc_cc_core` row -- but because retyping
        // `Cc::Credential::secret` would RELOCATE this copy rather than remove it, to
        // `Wire::AuthRequest` and into the RESP `AUTH` vector `SocketExchange` encodes
        // -- both heap residue, so #1125's own subject (#1578). The secret is protected
        // everywhere this process HOLDS it; `fastcache-cli/main.cpp` carries the full
        // argument at the sibling boundary.
        _startup { .username = {}, .secret = std::string { startup.requirePass.View() } },
        _reloader { reloader }
    {
    }

    /// @copydoc ICredentialSource::Current
    [[nodiscard]] Cc::Credential Current() const override
    {
        if (_reloader == nullptr)
            return _startup;

        // One snapshot, read once. `Current()` on the reloader takes its swap lock and
        // hands back an immutable `shared_ptr`, so the secret cannot change underneath
        // this expression however the reload races it.
        auto const live = _reloader->Current();
        return Cc::Credential { .username = {}, .secret = std::string { live->requirePass.View() } };
    }

  private:
    Cc::Credential _startup;
    NodeReloader const* _reloader;
};

} // namespace FastCache::Node
