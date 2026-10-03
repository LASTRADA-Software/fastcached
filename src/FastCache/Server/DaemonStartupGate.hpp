// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/ConfigMerge.hpp>

#include <optional>
#include <string>

namespace FastCache
{

/// What `fastcached` refuses a command line with before the daemon host is entered, or nothing.
///
/// **The rules a SERVING daemon is held to, asked only of the outcomes that serve**
/// (`JudgedByServingRules`): a start, and an install, which registers the command line a start
/// replays forever and so is refused while somebody is watching. An uninstall refused over the
/// typo it was reached to undo, or a `--healthcheck` refused because this build has no TLS, would
/// be the refusal working against the operator. The rules, in the order they are asked:
///
///   1. the bind-flag shape (`ValidateBindFlagShape`): the legacy single-bind flags beside
///      `--listen` would silently vanish;
///   2. the keyspace-event grammar (`ParseKeyspaceEvents`), which the body parses again;
///   3. every other rule decided by the configuration and the build alone
///      (`DaemonStartupRejection`).
///
/// A function of its own, and in the library rather than in `main`, because `main` is in no test
/// target: the gate was a bare `if` there that nothing could hold to its table.
/// @param outcome What the command line asked for.
/// @param assembled The effective configuration, as `AssembleEffectiveConfig` built it.
/// @return The refusal's words, or nothing when this outcome may proceed.
[[nodiscard]] std::optional<std::string> ServingRulesRejection(CliOutcome outcome, EffectiveConfig const& assembled);

} // namespace FastCache
