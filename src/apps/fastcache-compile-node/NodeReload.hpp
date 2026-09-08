// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeMembership.hpp"

#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Core/Logger.hpp>

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
void ApplyReloadRequest(NodeReloader* reloader, NodeMembership& membership, ILogger& logger);

} // namespace FastCache::Node
