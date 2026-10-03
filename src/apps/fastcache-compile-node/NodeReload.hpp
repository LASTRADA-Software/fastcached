// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeIdentity.hpp"
#include "NodeMembership.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Core/Logger.hpp>

#include <optional>
#include <span>

namespace FastCache::Node
{

/// The node's reloader: `NodeConfig`, read through the option table, guarded by its
/// `reloadable` column.
///
/// A name in a header rather than an alias inside `main.cpp`, because the live
/// snapshot is what several node-level policies read: `ReloadedCredential` presents
/// whatever `--requirepass` currently says, and anything else that has to answer
/// *now* rather than *at startup* reaches the same object. An alias private to the
/// one translation unit no test can reach would force each of them to spell
/// `ConfigReloaderOf<NodeConfig>` again, which is a second name for one concept.
using NodeReloader = ConfigReloaderOf<NodeConfig>;

/// What the start resolved ONCE, which every reload candidate is shaped by again rather than
/// resolving.
///
/// A reload runs on a signal: it must not mint, re-derive a directory or re-ask DNS, since any of
/// those could land somewhere the running node is not. So the start's answers travel here.
struct ReloadBasis
{
    std::optional<NodeStateDirectoryChoice> stateDirectory; ///< Where the start put this node's state.
    NodeHostNames hostNames;                                ///< This machine's names, as the start asked them.
    Cluster::FormationRecord formation;                     ///< The record the start kept; a reload never mints one.
    Cluster::FleetEndpoints remembered;                     ///< The fleet endpoints the start last knew.
    NodeIdentity identity;                                  ///< The id and key the start resolved.
};

/// How the node's reloader reads its file: a FRESH configuration, the file applied through the
/// appliers argv reaches, then the command line -- the startup order -- and then shaped by
/// @p basis as the start shaped its own.
///
/// **A named function rather than a lambda in `main`, because each shaping step is load-bearing
/// and none fails the build when dropped.** A candidate shaped by no formation record runs no
/// consensus the running node does, so every scheduler's reload was declined, with a message
/// about a flag nobody changed; a candidate without the identity holds an empty id, an
/// unreloadable field that has CHANGED. `main.cpp` is in no test target, so this is where a test
/// can see them.
/// @param args The command line after the program name; must outlive the reader.
/// @param basis What the start resolved.
/// @return The reader to hand the reloader.
[[nodiscard]] NodeReloader::Reparse ReloadCandidateReader(std::span<char const* const> args, ReloadBasis basis);

/// Act on one SIGHUP, and say what happened either way.
///
/// **Both outcomes are logged, and that is the whole point of the ticket.** A reload
/// that silently ignored a changed field would leave the operator believing an edit
/// took effect -- they edited a file, saw no error, and got nothing. So a refusal
/// names every setting that may not change at runtime, and a success names what is now
/// in force.
///
/// **Out of `main.cpp` since #405**, and the membership parameter is why. A member
/// REMOVED from the file and still admitted is the fails-open direction this path
/// exists for, so "a revoked host is refused after the reload" has to be a case
/// somebody can run -- and while this lived in the one translation unit no test
/// reaches, it could not be. The decision (`AdmissionAnnouncement`) and the effect
/// (`NodeMembership::Adopt`) were already testable in isolation; what was not was that
/// a SIGHUP reaches them.
///
/// @param reloader The pipeline, or null when this worker has no configuration file.
/// @param membership Who this node admits, updated from the accepted snapshot.
/// @param conditions Where `unqualified-host-name` is answered again, since `--advertise` reloads.
/// @param logger Where the outcome is reported.
///
/// Returns nothing: the heartbeat thread notices a reload by comparing snapshots, so
/// there is no signal to hand it. A `ConfigReloaderOf::Subscribe` callback would be the
/// house idiom for one, and is declined here for a lifetime reason rather than a
/// stylistic one -- the reloader is declared in `main` and outlives `WorkerBody`, and
/// `Subscribe` has no unsubscribe, so a subscriber capturing this frame's locals would
/// outlive them.
///
/// **That decline is about THIS frame, and `main` does subscribe.**
/// `WatchSecretExposure` is attached beside the reloader's own declaration, capturing
/// only what `main` owns and what outlives the reloader -- which is the arrangement
/// the paragraph above describes as safe rather than an exception to it
/// ([#868](https://github.com/LASTRADA-Software/fastcached/issues/868)). Read as "the
/// worker cannot subscribe at all" the rule is one frame too wide, and it would leave
/// the secret-file check startup-only on the binary that holds five such files.
void ApplyReloadRequest(NodeReloader* reloader, NodeMembership& membership, NodeConditions& conditions, ILogger& logger);

} // namespace FastCache::Node
