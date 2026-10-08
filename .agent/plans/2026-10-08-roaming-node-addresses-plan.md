# Roaming Node Addresses Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `fastcache-compile-node` keeps working — as worker, scheduler and Raft voter — when its
host's IP changes (DHCP, Wi-Fi ↔ LAN, network moves), with no config edit and no restart, and a
stock socket-activated package install starts without `--advertise`.

**Architecture:** A new `IRouteProbe` (Platform) reports the local address the kernel routes from.
A new `EndpointResolver` (compile-node) turns the advertise policy + that address into the node's
`0xFC` and Raft endpoints and is the sole publisher of both `AnnouncedEndpoint`s; it re-probes when
POSIX/Windows network watchers report a change, and every 30 s. Existing machinery carries the
change: the worker re-registers, presence announces, the leader re-proposes the member record
(Raft host coupled to the announced `0xFC` host), and `RaftPeerTransport::Learn` redials.

**Tech Stack:** C++23, CMake presets (`clang-debug` = ASan+UBSan+clang-tidy), Catch2, core-cpp v0.6.0.

**Spec:** `.agent/plans/2026-10-08-roaming-node-addresses-design.md`

## Global Constraints

- Read `AGENT.md` and the `.agent/rules/` file for the area before touching it
  (Platform → `platform-service-and-config.md`; node → `distributed-compilation.md`;
  Cluster/Consensus → `consensus-and-cluster.md`; metrics → `metrics-and-observability.md`;
  tests → `testing.md`). Every rule there has been a bug.
- **No wire change:** `CompileCacheWire::CurrentVersion` stays 15; no request/reply arity changes.
- No new persisted fields: `ClusterMember`, Raft `Command`, roster, `FleetSummary` keep one
  `host:port` string each.
- Dependency injection for every OS touch (route probe, netlink, clock); data-driven tables over
  `if` ladders; `std::expected` for fallible API; Doxygen on every new public symbol; no `k`
  prefixes; no C-style loops; no `NOLINT`; `clang-format` every changed file.
- Tests: Catch2 `X_test.cpp` next to `X.cpp`; names without leading `-` or commas; bounded waits
  only (`tests/BoundedWait.hpp`); `Unwrap(optional)` after `REQUIRE(opt.has_value())`; threaded
  Platform tests tagged `[platform][host-events]`.
- Build/test: `cmake --build --preset clang-debug -j 28` then
  `ctest --preset clang-debug -j 28 --output-on-failure` (narrow with `-R` while iterating).
- Commit per task, conventional title, body ends with
  `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. **Offline at start / no route** — resolver must never publish loopback/empty; start falls back
   to today's FQDN chain. Test in Task 4.
2. **Pinned endpoints never move** — a literal or name `--advertise`/`--raft-self`, and a member
   whose recorded Raft host differs from its `0xFC` host, are never re-addressed. Tests in Tasks 3, 6.
3. **Reload while roaming** — a SIGHUP with an unchanged config must not revert the published
   endpoint to a stale start-time address. Test in Task 4.
4. **Socket activation bound to a specific IP** — advertises that IP (pinned), wildcard → auto.
   Test in Task 5.
5. **Rapid flapping** — repeated changes coalesce; WorkerTier withdraws every superseded
   registration exactly once. Test in Task 4.

---

### Task 1: `IRouteProbe` — which local address routes to a host

**Files:**
- Create: `src/FastCache/Platform/RouteProbe.hpp`, `src/FastCache/Platform/RouteProbe.cpp`,
  `src/FastCache/Platform/RouteProbe_test.cpp`
- Modify: `src/FastCache/CMakeLists.txt` (add `Platform/RouteProbe.cpp` beside `Platform/LocalAddresses.cpp`)
- Modify: `scripts/check-udp-opener.cmake:66-68` (second `FastCachedUdpOpenerUnits` row)

**Interfaces — Produces:**
```cpp
namespace FastCache {
/// Why a route probe produced no address.
enum class RouteProbeError : std::uint8_t { InvalidTarget, NoRoute, Unsupported, Last };

/// Asks the kernel which local address it would send from to reach a host. Sends nothing.
class IRouteProbe {
  public:
    // copy/move deleted, virtual dtor (match IHostAddressSource in LocalAddresses.hpp)
    /// @param target An IPv4 or IPv6 LITERAL (no name resolution, no port).
    /// @return The local address in `inet_ntop` spelling without `%scope`, or why none.
    [[nodiscard]] virtual std::expected<std::string, RouteProbeError> SourceFor(std::string_view target) const = 0;
};
/// The real kernel: UDP socket, connect() to target:9, getsockname(), close. Never sends.
[[nodiscard]] std::unique_ptr<IRouteProbe> MakeSystemRouteProbe();
/// The fixed targets that only consult the default route: TEST-NET-1 and the documentation prefix.
inline constexpr std::array<std::string_view, 2> DefaultRouteProbeTargets { "192.0.2.1", "2001:db8::1" };
}
```

- [ ] **Step 1: Write failing tests** in `RouteProbe_test.cpp` (tags `[platform][route]`):
  - `"The route probe reaches loopback from loopback"`: `MakeSystemRouteProbe()->SourceFor("127.0.0.1")` has value `"127.0.0.1"`.
  - `"The route probe refuses a target that is not an address literal"`: `SourceFor("example.com")` → `RouteProbeError::InvalidTarget`; same for `""` and `"10.0.0.1:80"`.
  - `"The route probe answers in the spelling local addresses use"`: for `"::1"`, if a value is returned it equals `"::1"` (IPv6 may be disabled on CI: accept `NoRoute`/`Unsupported` there, assert the error is one of those two, not `InvalidTarget`).
- [ ] **Step 2: Build + run** `ctest --preset clang-debug -R "route probe"` — FAIL (undefined).
- [ ] **Step 3: Implement** `RouteProbe.cpp`: parse literal with `inet_pton` (v4 then v6) → `InvalidTarget` on failure; `socket(family, SOCK_DGRAM, IPPROTO_UDP)` (Winsock on `_WIN32`, `closesocket`), RAII close; `connect()` to port 9; `ENETUNREACH`/`EHOSTUNREACH`/`EADDRNOTAVAIL`/`WSAENETUNREACH` → `NoRoute`, `EAFNOSUPPORT` → `Unsupported`; `getsockname` → format with `inet_ntop` exactly like `QueryLocalAddresses` (no scope). Keep the errno → error mapping a small table.
- [ ] **Step 4: Register the opener** — add row
  `"src/FastCache/Platform/RouteProbe.cpp|the route probe: connect() only, to read the kernel's source-address choice; it never sends or receives, so no firewall rule is involved"`
  and run `ctest --preset clang-debug -R udp-opener` — PASS (also run its selftest if one exists: `-R udp-opener`).
- [ ] **Step 5: Run tests** — PASS. clang-format changed files.
- [ ] **Step 6: Commit** `feat(platform): add a route probe for the local address that reaches a host`.

### Task 2: POSIX network-change watchers (Linux netlink, macOS route socket)

**Files:**
- Create: `src/FastCache/Platform/NetworkChangeMessages.hpp/.cpp` (pure classifiers),
  `src/FastCache/Platform/NetworkChangeMessages_test.cpp`
- Modify: `src/FastCache/Platform/NetworkChangeWatcher.cpp:69-75` (POSIX branch),
  `src/FastCache/Platform/NetworkChangeWatcher.hpp` (doc), `NetworkChangeWatcher_test.cpp:183` (expectation),
  `src/FastCache/CMakeLists.txt`
- Modify: `src/apps/fastcache-compile-node/main.cpp:2738-2747` (start POSIX watcher after the fork)

**Interfaces — Produces:**
```cpp
namespace FastCache {
/// Whether a buffer read from the OS change socket reports an interface or address change.
/// Linux: a sequence of `nlmsghdr` (native endian: u32 len, u16 type, u16 flags, u32 seq, u32 pid),
/// types RTM_NEWLINK=16, RTM_DELLINK=17, RTM_NEWADDR=20, RTM_DELADDR=21 count; NLMSG_DONE/ERROR/NOOP do not.
[[nodiscard]] bool NetlinkReportsChange(std::span<std::byte const> buffer) noexcept;
/// macOS/BSD: a sequence of `rt_msghdr` prefixes (u16 msglen, u8 version, u8 type),
/// types RTM_NEWADDR=0xc, RTM_DELADDR=0xd, RTM_IFINFO=0xe count (plus RTM_ADD/RTM_DELETE=1/2 for default-route changes).
[[nodiscard]] bool RouteSocketReportsChange(std::span<std::byte const> buffer) noexcept;
}
```
Both are table-driven (one `constexpr` array of counted message types each), walk messages by
their length field, stop at a truncated/zero-length header (return what was seen so far).

- [ ] **Step 1: Failing tests** (`[platform][host-events]`): build byte fixtures in the test
  (helper that appends a header with given type and padded length) —
  `"A netlink address message reports a change"`, `"A netlink done message alone reports no change"`,
  `"A truncated netlink buffer reports what it holds and nothing more"`,
  `"A route-socket address message reports a change"`, `"A route-socket miss message reports no change"` (RTM_MISS=7).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** classifiers. **Step 4: Run** — PASS.
- [ ] **Step 5: POSIX watcher.** In `NetworkChangeWatcher.cpp` replace the null branch:
  - Linux (`__linux__`): `socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)`, bind
    `sockaddr_nl{ .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE }`.
  - Apple: `socket(PF_ROUTE, SOCK_RAW | ..., AF_UNSPEC)` (set `FD_CLOEXEC` via `fcntl`).
  - Other POSIX: keep `return nullptr`.
  - `class PosixNetworkChangeWatcher final: public NetworkChangeWatcher` owns the fd, a self-pipe
    (or `eventfd` on Linux) for stop, a `NetworkChangeRelay _relay`, and a `std::jthread` that
    `poll()`s both fds, `read()`s into a 16 KiB buffer and calls `_relay.Notify()` when the
    classifier says so. Destructor: signal the pipe, join, close fds (member order: relay before thread, thread LAST as in `NetworkChangeRelay`).
  - Socket/bind failure → `std::unexpected(std::format("{} failed: {}", "netlink socket", std::system_category().message(errno)))`.
- [ ] **Step 6:** Update `NetworkChangeWatcher_test.cpp:183` expectation: non-null on
  `_WIN32 || __linux__ || __APPLE__`, null elsewhere. Rename the case to
  `"The network watcher runs where the platform offers one"` only if its text no longer fits.
- [ ] **Step 7: main.cpp.** A POSIX watcher owns a thread, which does not survive `fork()`. Start
  it after the daemon fork: keep the Windows start where it is (before `host->Run`), and on POSIX
  start it inside the body passed to `host->Run` (or immediately after the fork branch when the
  host runs the body in-process) — the watcher must outlive the body. Rewrite the comment at
  2738-2743 to say so.
- [ ] **Step 8:** Build, run `ctest -R "network|netlink|route-socket|host-events"`, then the
  TSan-scope hygiene check `ctest --preset clang-debug -R tsan-scope`. If it demands the new test
  file be listed (it lists `NetworkChangeWatcher_test.cpp` at `scripts/check-tsan-scope.cmake:128-129`),
  the classifier tests are not threaded — prefer tag `[platform][network]` there if the check
  only constrains threaded files; follow what the check says.
- [ ] **Step 9: Commit** `feat(platform): watch interface and address changes on Linux and macOS`.

### Task 3: Advertise policy — `auto`, pinned names and literals; `--raft-self` reloadable; `routeHost`

**Files:**
- Modify: `src/apps/fastcache-compile-node/NodeConfig.hpp/.cpp` (rows at 975-1010 and 1190-1203,
  `ParseAdvertise` 391-406, `AddressReloadableFlags` 1144, `AdvertisedEndpoint` 2809-2849,
  `RaftSelfEndpoint` 2759-2785, `AdvertisedNameWithheld` 2851-2857)
- Modify: `src/apps/fastcache-compile-node/NodeDefaults.hpp` (apply helper beside `ApplyHostNames`)
- Modify: `docs/tools/fastcache-compile-node.md:2931-2941` (reloadable table: add `raft_self`),
  `packaging/config/fastcache-compile-node.yaml:13-31` (count SEVEN → EIGHT; describe `auto`)
- Test: `src/apps/fastcache-compile-node/NodeConfig_test.cpp`

**Interfaces — Produces:**
```cpp
namespace FastCache::Node {
/// How an advertised address is chosen.
enum class AdvertiseMode : std::uint8_t { Auto, PinnedName, PinnedLiteral, Last };
/// One row per mode: what the mode promises.
struct AdvertiseModeRow {
    AdvertiseMode mode;
    bool followsRoute;          ///< Re-derived from the route probe on every change.
    std::string_view describe;  ///< One sentence for logs and docs.
};
[[nodiscard]] AdvertiseModeRow const& AdvertiseModeRowFor(AdvertiseMode mode) noexcept;
/// The mode a `--advertise` / `--raft-self` value selects: "" or "auto" or "auto:<port>" → Auto,
/// an IP literal host → PinnedLiteral, anything else → PinnedName.
[[nodiscard]] AdvertiseMode AdvertiseModeOf(std::string_view value) noexcept;
/// Set the probed route host (runtime fact, like hostNames). Empty clears it.
void ApplyRouteHost(NodeConfig& cfg, std::string host);
}
// NodeConfig gains:  std::optional<std::string> routeHost;  ///< Probed source address; nullopt = not probed / no route.
```
Semantics:
- `ParseAdvertise` accepts `auto` and `auto:<port>` (port 1-65535, else `ConfigError` naming the value).
- `AdvertisedEndpoint(cfg)`: explicit non-auto value → as today. Auto (empty/`auto`/`auto:N`):
  port = `N` or the bound Node port; host = bind host if not wildcard (PinnedLiteral by bind),
  else `cfg.routeHost` if set, else today's chain (FQDN → withheld/loopback logic).
- `RaftSelfEndpoint(cfg)`: `raftSelf` = `auto` or empty behaves like empty did, but prefers
  `cfg.routeHost` over the FQDN for a wildcard bind. `--raft-self` row: `.reloadable = Reloadable::Yes`,
  operand `=<host|auto>`, description mentions auto; add `"--raft-self"` to `AddressReloadableFlags`
  (array size 2). The static_asserts at `NodeConfig.cpp:2083-2137` must pass.
- Update both flag descriptions: default is the address this machine routes from, re-derived when
  the network changes; a literal IP pins it and turns roaming off.

- [ ] **Step 1: Failing tests** (`[node][config][advertise]`):
  - `"An unset advertise follows the probed route host on a wildcard bind"`: cfg with wildcard Node bind port 6674, `routeHost = "10.1.2.3"`, fqdn `"box.lan"` → `AdvertisedEndpoint == "10.1.2.3:6674"`.
  - `"Advertise auto with a port keeps the probed host and overrides the port"`: `"auto:7000"` → `"10.1.2.3:7000"`.
  - `"Without a probed route the advertised endpoint falls back to the fully qualified name"`: routeHost nullopt → `"box.lan:6674"`.
  - `"A literal advertise is pinned and ignores the probed route"`: `"192.168.1.9:6674"` → unchanged, `AdvertiseModeOf == PinnedLiteral`, `!AdvertiseModeRowFor(...).followsRoute`.
  - `"A named advertise is pinned"`: `"build.example:6674"` → PinnedName.
  - `"Raft self follows the probed route host unless pinned"`: wildcard raft bind 6680 → `"10.1.2.3:6680"`; `raftSelf = "peer.lan"` → `"peer.lan:6680"`.
  - `"Advertise auto refuses a port out of range"`: `"auto:0"`, `"auto:70000"` → parse error.
  - `"Raft self is reloadable"`: a reload changing `raft_self` is not refused (copy the shape of the test at `NodeConfig_test.cpp:4423`).
- [ ] **Step 2: Run** `ctest --preset clang-debug -R "advertise|Raft self"` — FAIL.
- [ ] **Step 3: Implement** as specified (mode table `constexpr std::array<AdvertiseModeRow, 3>` + `static_assert` row order).
- [ ] **Step 4: Docs/YAML** reloadable table + count; run `ctest --preset clang-debug -R "reloadable-docs|config-reference|cli-node-flags"` — PASS.
- [ ] **Step 5: Full** `ctest --preset clang-debug -R node -j 28` — PASS (existing advertise tests at
  `NodeConfig_test.cpp:805,1686,1744,5063` and `NodeAnnounce_test.cpp:215,265,293` may encode the
  FQDN default with `routeHost` unset — they must still pass unchanged because the fallback chain is
  preserved; if one fails, fix the code, not the test).
- [ ] **Step 6: Commit** `feat(node): advertise the routed address by default and make --raft-self reloadable`.

### Task 4: `EndpointResolver` — the sole publisher of the node's endpoints

**Files:**
- Create: `src/apps/fastcache-compile-node/EndpointResolver.hpp/.cpp`, `EndpointResolver_test.cpp`
  (register both in `src/apps/fastcache-compile-node/CMakeLists.txt`: source list and `fastcache-compile-node-tests` list near :181)
- Modify: `WorkerTier.cpp:446-464, 533-565` + `WorkerTier.hpp` (re-register on published change)
- Modify: `NodeAnnounce.hpp/.cpp` (`AdvertisedEndpointChange` retired or reduced — see below)
- Modify: `NodeReload.hpp:52`, `NodeReload.cpp:26-29` (reload applies the live route host)
- Modify: `main.cpp` (~:646-657 construct resolver + two `AnnouncedEndpoint`s; subscribe it to
  `hostEvents` FIRST; pass raft endpoint source on; ~:2538-2547 apply initial route host to `cfg`
  and `cliOnly` before `StartupPolicyRejection`)
- Modify: `src/FastCache/Metrics/IMetricsSink.hpp`, `MetricsCatalog.hpp`, `StatsReading.hpp:542`
  (`CounterSoleWriterTable`), `docs/tools/fastcache-compile-node.md` metrics table
- Test: `WorkerTier_test.cpp` (fixture `WorkerTierTestFixture.hpp`)

**Interfaces — Consumes:** `IRouteProbe`, `DefaultRouteProbeTargets` (Task 1); `ApplyRouteHost`,
`AdvertisedEndpoint`, `RaftSelfEndpoint`, `AdvertiseModeOf` (Task 3); `IHostEventSink` /
`HostEvent::NetworkChanged`; `AnnouncedEndpoint` (NodeAnnounce.hpp:78).
**Produces:**
```cpp
namespace FastCache::Node {
/// Literal IP hosts worth probing before the default route (e.g. applied schedulers given as IPs).
class IProbeTargets { public: /*...*/ [[nodiscard]] virtual std::vector<std::string> Targets() const = 0; };

class EndpointResolver final: public IHostEventSink {
  public:
    /// @param config   The configuration in force (LiveNodeConfig / reloader snapshot source).
    /// @param probe    Route probe. @param targets Preferred targets (may return none).
    /// @param node     Published 0xFC endpoint. @param raft Published Raft endpoint.
    EndpointResolver(INodeConfigSource const& config, IRouteProbe const& probe, IProbeTargets const& targets,
                     core::platform::IClock const& clock, AnnouncedEndpoint& node, AnnouncedEndpoint& raft,
                     IMetricsSink& metrics, ILogger& logger);
    /// Re-probe if stale (event seen, or older than RefreshInterval) and publish what moved.
    void Refresh();
    /// The last probed route host, empty when none was ever found.
    [[nodiscard]] std::string RouteHost() const;
    void OnHostEvent(HostEvent event) override; // NetworkChanged → stale; never probes on the hub's thread
    static constexpr std::chrono::seconds RefreshInterval { 30 };
};
}
```
(Use the existing config-source seam; it is named `INodeConfigSource` per
`platform-service-and-config.md:1290` — confirm the exact header/name with grep before use.)

Behaviour:
- Probe order: `targets.Targets()` then `DefaultRouteProbeTargets`; first success wins.
  Reject loopback, link-local (`169.254/16`, `fe80::/10`), unspecified, multicast results
  (one predicate table). No usable result → keep the last route host (never publish empty/loopback).
- Endpoints: copy `*config.Current()`, `ApplyRouteHost(copy, host)`, compute
  `AdvertisedEndpoint(copy)` / `RaftSelfEndpoint(copy)`; publish each when non-empty and different;
  Info log `"endpoint {} -> {}"` / `"raft endpoint {} -> {}"`; increment
  `Counter::NodeEndpointChanges` / `Counter::NodeRaftEndpointChanges`.
  Literal mode: warn ONCE `"--advertise {} is a literal address: this node will not follow network changes"`.
- A background `std::jthread` waits (condition variable, stop_token) for stale-or-interval and
  calls `Refresh()`; `WorkerTier` also calls `Refresh()` at the top of each heartbeat (cheap).
- **WorkerTier:** keep `_registeredAs` (the endpoint registrars were adopted for). Each beat:
  `_resolver.Refresh(); if (auto now = _announced.Current(); now != _registeredAs) { log Warn the
  existing EndpointChange announcement text; AdoptRegistrars(...); PublishRegistration(...); _registeredAs = now; }`.
  `AnnounceAs` no longer publishes (the resolver does). Replace the `AdvertisedEndpointChange`
  call site; delete `AdvertisedEndpointChange` if it has no other caller, moving its announcement
  sentence into a helper `DescribeEndpointMove(from, to)` with its existing test text updated.
  `WorkerTier` gets the resolver through its parts struct (`WorkerTierParts`, WorkerTier.hpp:135 area).
- **Reload:** `ReloadBasis` gains `std::function<std::string()> routeHost`; `ReloadCandidateReader`
  calls `ApplyRouteHost(candidate, basis.routeHost ? basis.routeHost() : "")` after `ApplyHostNames`.
- **Metrics:** two enumerators before `Last`, rows `fastcache_node_endpoint_changes_total` /
  `fastcache_node_raft_endpoint_changes_total` (help text: "Times this node's advertised ... moved
  because the address it routes from changed or a reload re-pinned it."), sole-writer rows on the
  node surface the compile node already uses for node-tier counters (find one with grep in
  `StatsReading.hpp`), and the two series in the docs metrics tables.

- [ ] **Step 1: Failing tests** `EndpointResolver_test.cpp` (`[node][endpoint]`), with a
  `FakeRouteProbe` (map target → expected) and `ManualClock` and a fixed config source:
  - `"The resolver publishes the routed address at its first refresh"`.
  - `"A network change makes the next refresh re-probe and publish the move"`: probe answers `10.0.0.5` then `192.168.7.2`; after `OnHostEvent(NetworkChanged)` + `Refresh()` node endpoint is `192.168.7.2:6674`, raft `192.168.7.2:6680`, counters each 1.
  - `"Without a network change a refresh inside the interval does not re-probe"` (probe call count).
  - `"A refresh after the interval re-probes without any event"`.
  - `"With no usable route the resolver keeps the last published endpoint"`: probe returns `NoRoute` / `"127.0.0.1"` / `"169.254.3.3"` → unchanged.
  - `"A pinned literal advertise never moves and is reported once"` (CapturingLogger, one Warn).
  - `"A preferred target is probed before the default route"`.
  - `"A reload that changes nothing keeps the roamed endpoint"` (Review Focus 3): config source returns a new but equal snapshot; endpoint stays the roamed one.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** resolver. **Step 4: Run** — PASS.
- [ ] **Step 5: WorkerTier failing test** in `WorkerTier_test.cpp` (`[node][worker][endpoint]`):
  `"A worker re-registers under a moved endpoint and withdraws the old one exactly once"` —
  publish A, beat, publish B, beat, publish C, beat: withdrawals = {A, B} each once, registrations
  end at C (Review Focus 5). Use the fixture's fake scheduler.
- [ ] **Step 6: Implement** WorkerTier change; run node tests — PASS.
- [ ] **Step 7: Wire main.cpp + reload + metrics + docs**; build all; run
  `ctest --preset clang-debug -j 28 -R "node|metrics|counter|reload"` — PASS.
- [ ] **Step 8: Commit** `feat(node): re-derive the advertised endpoints when the network changes`.

### Task 5: Socket activation derives the advertised address

**Files:**
- Modify: `src/apps/fastcache-compile-node/main.cpp:252-291, 1775-1782`
- Create or modify: a testable home for the decision, e.g. `src/apps/fastcache-compile-node/NodeActivation.hpp/.cpp`
  (+ `_test.cpp`, registered in the app CMakeLists) — `main.cpp` is in no test target.
- Modify: `packaging/linux/fastcache-compile-node.socket`, `.service` comments;
  `.agent/rules/distributed-compilation.md:266-276` (rule rewritten: the inherited socket's bound
  address is read with `getsockname`; wildcard → Auto; specific → pinned).

**Interfaces — Consumes:** `BoundEndpointOfDescriptor(int)` (`src/FastCache/Transport/NativeListen.hpp`,
returns `std::optional<BoundEndpoint>`), Task 3 policy.
**Produces:**
```cpp
namespace FastCache::Node {
/// Make the configuration describe the socket a supervisor handed over: the Node surface's bind
/// becomes the descriptor's bound host and port, so the advertised endpoint is derived as for any bind.
/// @return The refusal when the descriptor cannot be asked what it is bound to.
[[nodiscard]] std::expected<void, NodeRefusal> AdoptActivatedBind(NodeConfig& cfg, BoundEndpoint const& bound);
}
```
- [ ] **Step 1: Failing tests** (`[node][activation]`):
  `"An activated wildcard socket advertises the routed address on its port"` (bound `0.0.0.0:6676`, routeHost `10.0.0.9` → `AdvertisedEndpoint == "10.0.0.9:6676"`),
  `"An activated socket bound to one address advertises that address"` (`192.168.1.4:6676` → same, PinnedLiteral),
  `"An explicit advertise still wins under activation"`.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** — find how the Node surface bind is stored
  (`RowFor(NodeSurface::Node).Resolve(cfg)`; the field behind `--listen-node`) and set it. In
  `main.cpp`, remove the `cfg.advertise.empty()` refusal; after adopting the descriptor call
  `BoundEndpointOfDescriptor` and `AdoptActivatedBind` BEFORE the first `AdvertisedEndpoint(cfg)`
  (check ordering against `main.cpp:646`; move the derivation after activation if needed).
  A descriptor whose bound address cannot be read keeps the old refusal text.
- [ ] **Step 4: Run** — PASS; packaging comments; rule text. **Step 5: Commit**
  `fix(node): derive the advertised address under socket activation`.

### Task 6: Cluster — Raft endpoint follows the announced host; own move resets sessions; live beacon

**Files:**
- Modify: `src/FastCache/Cluster/AnnouncedEndpoints.hpp/.cpp` (+ `_test.cpp`)
- Modify: `src/FastCache/Consensus/RaftPeerTransport.hpp/.cpp` (public `ResetSessions()`)
- Modify: `src/apps/fastcache-compile-node/ConsensusTier.hpp/.cpp` (raft source; reconcile)
- Modify: `main.cpp:1239` + `DiscoveryTier.cpp:102-123` (live fleet summary)
- Test: `src/FastCache/Cluster/MembershipCluster_test.cpp`, `RaftPeerTransport_test.cpp`, `ConsensusTier_test.cpp`

**Interfaces — Consumes:** raft `AnnouncedEndpoint` (Task 4) as `Cc::IAdvertisedEndpointSource const&`.
**Produces:**
```cpp
// AnnouncedEndpoints.cpp — host coupling, one helper:
/// The Raft endpoint a member's record should hold once it announced @p announced:
/// the announced host on the recorded Raft port when the recorded Raft and 0xFC hosts are the
/// same host, else the recorded Raft endpoint unchanged.
[[nodiscard]] std::string CoupledRaftEndpoint(ClusterMember const& recorded, std::string_view announced);
// RaftPeerTransport:
/// Close every dialled and attached session so senders redial; the transport keeps running.
/// Called when this node's own address moved. No-op unless Running.
void ResetSessions();
```
- `AnnouncedEndpointDesires`: `.raftEndpoint = CoupledRaftEndpoint(*recorded, endpoint)`. Host
  parsing via `Core/HostPort.hpp` (`ParseDialEndpoint` / the existing host splitter); an
  unparsable or empty recorded endpoint is never coupled.
- ConsensusTier gets `Cc::IAdvertisedEndpointSource const& raftAdvertised` (thread through
  `Start`, the private ctor and `StartConsensusOrExplain` at `ConsensusTier.hpp:1080`, call site
  `main.cpp:1352`). In `Reconcile` beside the `schedulerEndpoint` line (cpp:1020-1021): when this
  node dials (not a learner) and `PeerDialableOrNone(raftAdvertised.Current())` is non-empty, set
  `self->raftEndpoint` to it. Keep `_lastOwnRaft`; when it changes (not the first pass), log Info
  `"raft: this node moved to {}; reconnecting to every peer"` and call `_transport->ResetSessions()`.
- `ResetSessions` reuses `CloseSockets(this, std::nullopt)` under the same lock/state checks
  `Learn` uses at RaftPeerTransport.cpp:1231-1287.
- Discovery: replace `FixedFleetSummary` at `main.cpp:1239` with a `Cluster::IFleetSummarySource`
  that returns `ConfiguredFleetSummary(cfg, raftAdvertised.Current())` (same guards as
  `AnsweredFleetSummary`: empty summary when `!RunsConsensus(cfg)`; empty raft endpoint for a dial-in mode).

- [ ] **Step 1: Failing tests:**
  - `AnnouncedEndpoints_test.cpp` (`[cluster][formation][endpoint]`):
    `"An announced move of a member whose Raft and scheduler hosts match moves its Raft endpoint too"`
    (recorded raft `10.0.0.5:6680`, scheduler `10.0.0.5:6674`, announced `192.168.7.2:6674` → raft `192.168.7.2:6680`);
    `"A member whose Raft host differs from its scheduler host keeps its Raft endpoint"` (raft `peer.lan:6680`);
    `"A learner's empty Raft endpoint stays empty"` (existing first test still passes);
    IPv6: recorded `[fd00::5]:6680`/`[fd00::5]:6674`, announced `[fd00::9]:6674` → `[fd00::9]:6680`.
  - `MembershipCluster_test.cpp`: `"A voter that announces a moved address has its Raft endpoint re-recorded"` — reuse the file's cluster helpers; feed `AnnouncedEndpointDesires` → `MembershipProposals` → propose; after commit the state records the coupled endpoint, seat and key unchanged.
  - `RaftPeerTransport_test.cpp` (`[consensus][raft][transport]`): `"Resetting sessions makes a dialled peer redial while the transport keeps running"` — use the existing in-memory listener/`CountingConnector` pattern from the readdress test at :1184; assert dial count increases by one after `ResetSessions()` and the next message, and the peer receives it.
  - `ConsensusTier_test.cpp`: if the tier can be built in a unit test there (check existing cases), `"A leader asserts its own moved Raft endpoint"`; otherwise cover via the desire computation by extracting `SelfDesire(...)` as a free function and testing that.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement.** **Step 4: Run**
  `ctest --preset clang-debug -j 28 -R "cluster|consensus|raft|discovery|formation"` — PASS.
- [ ] **Step 5: Commit** `feat(cluster): move a voter's Raft endpoint with its announced address`.

### Task 7: Raft acceptor keeps one one-way session per proven dialler

**Files:**
- Modify: `src/FastCache/Consensus/RaftPeerServer.hpp/.cpp` (~251-281 accept path)
- Test: `src/FastCache/Consensus/RaftPeerLink_test.cpp` (or `RaftPeerServer_test.cpp` if present)

**Behaviour:** after a one-way handshake proves `dialler`, the server records
`dialler → socket` (map guarded like `_links`); if an older one-way socket for that dialler exists
it is `close()`d (its `Serve` ends at EOF; it is counted/logged as superseded at Debug). The entry
is erased by the connection's own RAII guard only if it still names that connection's socket.
Two-way (learner) sessions are untouched (they already attach as links).

- [ ] **Step 1: Failing test** (`[consensus][raft][peer-server]`):
  `"A second one-way session from the same member supersedes the first"` — dial twice as the same
  proven id with the in-memory `ListenerConnector`; assert the first socket reaches EOF within a
  bounded wait and the server's open-connection count is 1. And
  `"One-way sessions from different members coexist"`.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement.** **Step 4: Run** `-R "raft"` — PASS.
- [ ] **Step 5:** Add the rule to `.agent/rules/consensus-and-cluster.md` under "The Raft peer wire"
  (what: newest one-way session per dialler wins; breaks: each move parks a stale session against
  `maxConnections`; how known: the test above). **Commit**
  `fix(consensus): supersede a member's stale one-way session when it redials`.

### Task 8: Rules, docs, tripwires, release note

**Files:**
- `.agent/rules/distributed-compilation.md`: :1686-1691 (watchers now on Linux/macOS too),
  :1718-1724 (stale: `--advertise` and now `--raft-self` ARE reloadable — fix), :521-536 (default
  advertise is the routed address; hints still apply to a name), socket-activation rule (Task 5).
- `.agent/rules/consensus-and-cluster.md` :1839-1849 (announced move also moves a coupled Raft endpoint).
- `AGENT.md` rulebook tripwires for any new rule (keep within `scripts/agent-md-budget.txt`;
  `ctest -R "rulebook-tripwires|agent-md-budget"`).
- `docs/tools/fastcache-compile-node.md` (flags, "sleep, wake and network change" ~:2881-2905),
  `docs/operations/cluster-communication.md` (~470-510 re-addressing), `docs/operations/windows-office-fleet.md`
  (~450-470: names are opt-in now; #1605 is mitigated by auto), `docs/getting-started/distributed-compilation.md` if it shows `--advertise`.
- Release note: find the project's changelog/metainfo convention (`git log -p -1 -- '*metainfo*' 'CHANGELOG*' docs/about`); add an entry incl. the upgrade hint "remove a literal `--advertise` work-around under socket activation, or the node stays pinned".

- [ ] **Step 1:** Edit docs. **Step 2:** `ctest --preset clang-debug -R "doc|rulebook|agent-md|table|reloadable|mkdocs|hygiene"` — PASS (mkdocs check may be skipped locally if mkdocs is not installed; note it).
- [ ] **Step 3: Commit** `docs: describe roaming node addresses`.

### Task 9: Whole-branch verification, PR, CI

- [ ] Full build + `ctest --preset clang-debug -j 28`; fix every failure at the source.
- [ ] `clang-format` check on changed files; clang-tidy runs in the clang-debug build (warnings = errors? fix all).
- [ ] TSan gate for concurrency (resolver thread, watcher thread): build `clang-tsan` targets per AGENT.md and `bash scripts/tsan-gate.sh out/build/clang-tsan` if time allows; otherwise rely on CI.
- [ ] Push branch, open PR with `type/feature` label (`gh pr create`, then `gh pr edit --add-label type/feature`), watch CI (`gh pr checks --watch`), fix failures (`/fix-ci` workflow), until all green.
