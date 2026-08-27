// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Logger.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>

namespace FastCache::Node
{

/// One toolchain this worker will serve, before its identity is computed.
struct ToolchainEntry
{
    std::string fingerprint; ///< Empty when the node must compute it.
    std::string compiler;    ///< Path to the compiler.
};

/// Split a `--toolchain` value into its fingerprint and compiler.
///
/// Two accepted shapes, and the bare one is what operators should use:
///
///   `<compiler>`               -- the node computes the fingerprint itself
///   `<fingerprint>=<compiler>` -- an explicit override
///
/// The bare form exists because the fingerprint stopped being something a person
/// can derive. It used to be the compiler's `--version` line, which an operator
/// could read off a terminal; it is now a digest over the whole include tree, and
/// requiring that to be pasted into a config would make every toolchain update a
/// manual two-step that silently un-registers a worker when somebody forgets.
///
/// The override is kept because it is the only way to run a worker whose compiler
/// this process cannot execute -- a cross-compiler, or a wrapper that must not be
/// spawned at configuration time -- and because pinning a fingerprint by hand is
/// how an operator forces a fleet to agree while a machine is being repaired.
///
/// Split on the FIRST `=`, since a fingerprint is hex and contains none. A
/// compiler path containing `=` is therefore only reachable through the override
/// form, which is the documented escape hatch rather than a silent mis-parse --
/// and is why a DISCOVERED path never comes through here.
///
/// @param spec The flag's value.
/// @return The entry, or nullopt when it is empty or malformed.
[[nodiscard]] std::optional<ToolchainEntry> SplitToolchain(std::string_view spec);

/// The layouts discovery searches, for a refusal that can be acted on.
///
/// Off the shared table rather than a list written by hand, so a row added there
/// necessarily appears in the diagnosis -- a hand-written list is maintained by the
/// same person who forgot to add the row.
///
/// @return The layout names, comma-separated.
[[nodiscard]] std::string SearchedLayouts();

/// Every toolchain this worker will serve, from the operator or from the machine.
///
/// Lives here rather than in `main.cpp` because every rule below is a decision with
/// a failure mode, and `main.cpp` is in no test target -- which is exactly how the
/// two defects this function was written with (a discovered path re-parsed through
/// the operator's `=` grammar, and an empty result reported as a healthy worker)
/// got as far as review.
///
/// **The operator's list wins whole.** Naming any `--toolchain` pins the worker to
/// exactly that set; naming none, with discovery on, means "serve what this machine
/// has". The two are never merged, because a merged set would quietly re-add a
/// compiler an operator had deliberately narrowed away.
///
/// **A discovered compiler that cannot be spawned is dropped**, with a line naming
/// it and the layout that found it. That is the `SpawnFailed` refusal a client
/// otherwise meets at job time, moved to startup where an operator can see it. An
/// operator-NAMED toolchain is not probed: the `<fingerprint>=<compiler>` override
/// exists precisely for a compiler this process cannot execute.
///
/// **A worker with nothing to serve is refused here**, not reported as an empty set
/// for the caller to judge. Left to run it is the worst shape this system has:
/// nothing registers, the heartbeat calls "0 of 0 toolchain(s)" a success, and the
/// ready line says the node is up -- a healthy unit, a green fleet, and every build
/// compiling locally with no error at either end. Refusing is also what makes the
/// message testable, and there are three of them because there are three ways to
/// arrive: the machine was searched and holds nothing, every named compiler was
/// rejected, or nothing was named and nothing was to be searched. The first names
/// where it looked; the others must NOT, because reciting places nobody looked in
/// reads as "your compiler is not installed".
///
/// @param cfg What the operator asked for.
/// @param discovery Where the machine's own compilers come from; null when
///        `--no-toolchain-discovery` was given.
/// @param runner Process-spawning seam, for the compiler probes.
/// @param host The machine's filesystem, registry and environment.
/// @param logger Startup log.
/// @return Fingerprint to compiler path -- never empty -- or nullopt when a
///         `--toolchain` value is malformed or there is nothing to serve.
[[nodiscard]] std::optional<std::map<std::string, std::string>> ResolveToolchains(NodeConfig const& cfg,
                                                                                  Cc::IToolchainDiscovery* discovery,
                                                                                  Cc::IProcessRunner& runner,
                                                                                  Cc::IToolchainHost& host,
                                                                                  ILogger& logger);

} // namespace FastCache::Node
