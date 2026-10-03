// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Platform/Firewall.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// How the firewall spells a surface's protocol.
/// @param protocol A `SurfaceRow::protocol`.
/// @return The firewall's protocol, or nullopt for one the mapping lacks -- which no row may
///         have, since its surface would face the network with no rule.
[[nodiscard]] std::optional<FirewallProtocol> FirewallProtocolOf(SurfaceProtocol protocol) noexcept;

/// Which local ports the firewall admits for an endpoint of this port kind.
/// @param kind A `SurfaceEndpoint::portKind`.
/// @return `Fixed` for a port the configuration names, `Any` for one the kernel chooses.
[[nodiscard]] FirewallPortKind FirewallPortKindOf(SurfacePortKind kind) noexcept;

/// The firewall rules this node's registration needs: one per endpoint a `NodeSurfaceTable()`
/// row resolves to under @p cfg, skipping every loopback one.
///
/// Walks the surface table rather than naming ports, so a surface added there -- or a default
/// that turns one on -- is opened by its row alone. An endpoint whose port the kernel chooses
/// (discovery's reply socket, unless `--discovery-reply-port` pins it) gets a rule admitting
/// every local port of its protocol, still scoped to the program and the service, and named
/// `... udp/any` so the list says which rule that is.
/// @param cfg The configuration the service will run with (the merged one).
/// @param program The executable the rules admit.
/// @return The rules, in table order; empty when nothing faces the network.
[[nodiscard]] std::vector<FirewallRule> NodeFirewallRules(NodeConfig const& cfg, std::filesystem::path const& program);

/// Where `--install-service` reads the formation the service will start by.
///
/// A seam because the ORDER is the rule. The install secures the state directory
/// (`HandOverOwnedPaths`), and a record that directory held before -- an upgrade over a 0.3.0
/// directory other accounts could write in -- can be read only after that. So the reading is
/// asked AFTER the registration, and a test can make it answer differently on either side of it.
class IKeptFormationReader
{
  public:
    IKeptFormationReader() = default;
    IKeptFormationReader(IKeptFormationReader const&) = delete;
    IKeptFormationReader(IKeptFormationReader&&) = delete;
    IKeptFormationReader& operator=(IKeptFormationReader const&) = delete;
    IKeptFormationReader& operator=(IKeptFormationReader&&) = delete;
    virtual ~IKeptFormationReader() = default;

    /// @return What the state directory holds now, or why it cannot be read.
    [[nodiscard]] virtual std::expected<KeptFormation, std::string> Read() const = 0;
};

/// The production reader: the record and the remembered endpoints in the directory the start
/// resolves (`ChosenStateDirectory`), read through `ReadKeptFormation` as the start reads them.
///
/// **Without the start's owner judgement (`JudgeStateDirectory`), and that is the point rather
/// than a shortcut.** Who may own an entry is judged from the account ASKING: the start runs as
/// the service, and a file the service wrote is its own, while the install runs as an
/// administrator, to whom the same file belongs to "another account". Judged from the
/// installer's seat every upgrade over a directory the service had used would read as planted.
/// The install asks only after its handover made the directory the service's -- on Windows
/// `SecureDirectoryForService` refuses any entry owned by anybody but SYSTEM, Administrators or
/// the service -- and the record decides nothing here but which rules to open; the start judges
/// the directory again, as the service, before it acts on anything in it.
class StateDirectoryFormationReader final: public IKeptFormationReader
{
  public:
    /// @param cfg The configuration whose state directory is read; must outlive the reader.
    explicit StateDirectoryFormationReader(NodeConfig const& cfg);

    /// @copydoc IKeptFormationReader::Read
    [[nodiscard]] std::expected<KeptFormation, std::string> Read() const override;

  private:
    NodeConfig const& _cfg;
};

/// The configuration the SERVICE starts with: @p cfg shaped by the record @p formation reads now.
///
/// The formation decides whether consensus runs and whether discovery beacons beside it, so what
/// the service opens -- and what the startup rules judge -- is @p cfg shaped by the record, an
/// absent one by the solitary record the first start mints (`ProspectiveRecord`), as the start
/// does. Never @p cfg as the install found it: when the record was held at parse time `main`
/// skips the formation, and an unshaped configuration runs no consensus -- it opens the node port
/// alone, and the rules that judge an admitting node stay silent.
/// @param cfg The merged configuration the install was given.
/// @param formation Reads what the state directory holds.
/// @return The shaped configuration, or why the record cannot be read or applied.
[[nodiscard]] std::expected<NodeConfig, std::string> ShapedForService(NodeConfig const& cfg,
                                                                      IKeptFormationReader const& formation);

/// Register the service, THEN read and apply its formation, THEN judge it, THEN open its firewall.
///
/// **The order is the decision.** The registration secures the state directory, so a record that
/// directory held -- a 0.3.0 directory other accounts could write in, or any directory the
/// service wrote, judged from the installer's account -- is readable only afterwards. Then the
/// configuration the service STARTS with (`ShapedForService`) is judged by the install's rules
/// (`NodeInstallRejection`), so the install and the start cannot disagree; then the firewall
/// rules are derived from that same configuration (`NodeFirewallRules`).
///
/// **The record is read without the start's owner judgement (`JudgeStateDirectory`), and that is
/// deliberate.** Who may own an entry is judged from the account ASKING: the start runs as the
/// service, to which the files it wrote are its own, while the install runs as an administrator,
/// to which the same files belong to "another account" -- judged from that seat, every upgrade
/// over a used directory would read as planted and be refused. The install asks only after its
/// handover has secured the directory, which on Windows refuses any entry owned by anybody but
/// SYSTEM, Administrators or the service; the record merely selects which rules to open and which
/// startup rules apply; and the start judges the directory again, as the service, before it acts
/// on anything in it.
///
/// A registration that failed reads and opens nothing. A record that still cannot be read, or a
/// shaped configuration the install's rules refuse, REFUSES the install by name while the operator
/// is watching: no rule is opened, the exit code is a failure, and the message says the start will
/// refuse for the same reason. The registration itself is left as it is, because one this install
/// only RE-APPLIED is an upgrade's, which a refusal must not delete; the service it names refuses
/// to start. (`main` judges a configuration it could shape BEFORE registering, so a fresh install
/// is refused with nothing registered; only a held record defers the judgement to here.)
/// @param registerService Makes the registration (`InstallService`).
/// @param cfg The merged configuration the install was given.
/// @param program The executable the rules admit.
/// @param serviceName The service registered.
/// @param formation Reads what the state directory holds, asked after @p registerService.
/// @param firewall The machine's firewall, or null where none is managed.
/// @return The registration's result with the firewall folded in, or the refusal.
[[nodiscard]] ServiceControlResult InstallWithServiceFirewall(std::function<ServiceControlResult()> const& registerService,
                                                              NodeConfig const& cfg,
                                                              std::filesystem::path const& program,
                                                              std::string_view serviceName,
                                                              IKeptFormationReader const& formation,
                                                              IFirewall* firewall);

} // namespace FastCache::Node
