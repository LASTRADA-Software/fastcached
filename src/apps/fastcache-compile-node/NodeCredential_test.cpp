// SPDX-License-Identifier: Apache-2.0
#include "ClusterAdminCli.hpp"
#include "NodeAnnounce.hpp"
#include "NodeCredential.hpp"
#include "RemoteUpstream.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAsyncAddressResolver.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <WorkerProtocol.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/ScriptedSocket.hpp>

// **All three sites in one file, and that arrangement IS the test** (#404).
//
// `--requirepass` on this worker is presented and never required, and it was captured
// by value at three construction sites: the cache tier's upstream client, the cluster
// admin verbs, and the heartbeat round. A rotation that reached two of the three is a
// process running a configuration no file describes -- in the one area where the
// symptom is an authentication failure nobody can reproduce, on a machine nobody is
// watching.
//
// The property is therefore "ONE rotation, THREE sites", and a suite that spreads it
// across three files beside three implementations is a suite in which the third site
// is the one nobody adds. Each case below rotates the same shape of source and reads
// the bytes that went OUT, because a site holding a stale copy still returns a correct
// object, still logs nothing, and still moves no counter.
//
// What is deliberately NOT asserted anywhere here is `ICredentialSource::Current()`
// alone. A source that rotates while every site ignores it is precisely the bug, and a
// case asserting the source would pass under it.

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

namespace Wire = FastCache::CompileCacheWire;

constexpr std::string_view FirstSecret = "secret-before-rotation";
constexpr std::string_view SecondSecret = "secret-after-rotation";

constexpr std::chrono::milliseconds RefreshInterval { 30'000 };
constexpr std::chrono::milliseconds ConnectTimeout { 1'000 };
constexpr std::chrono::milliseconds IoTimeout { 5'000 };

/// A credential source an operator can be simulated rotating.
///
/// Stands in for `ConfiguredCredential` over a reloader whose file changed, without
/// a file: what every site sees is the interface, and what the cases are about is
/// whether a site re-asks. Its own fidelity is covered by the reloader case at the
/// bottom, which drives the production type over a real edit.
class RotatingCredential final: public ICredentialSource
{
  public:
    /// @param secret What to present until somebody rotates it.
    explicit RotatingCredential(std::string_view secret):
        _secret { secret }
    {
    }

    /// @copydoc ICredentialSource::Current
    [[nodiscard]] Cc::Credential Current() const override
    {
        return Cc::Credential { .username = {}, .secret = _secret };
    }

    /// Replace the secret, as an accepted reload does.
    /// @param secret The new secret.
    void Rotate(std::string_view secret)
    {
        _secret = secret;
    }

  private:
    std::string _secret;
};

/// The bytes a client sends when it presents @p secret before @p command.
/// @param secret The credential's secret.
/// @param command The framed command that follows the AUTH.
/// @return AUTH followed by the command, which is what the wire pipelines.
[[nodiscard]] std::vector<std::byte> AuthThen(std::string_view secret, std::vector<std::byte> const& command)
{
    auto out = Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = std::string { secret } });
    out.insert(out.end(), command.begin(), command.end());
    return out;
}

/// A reply stream that accepts the AUTH and then answers @p answer.
/// @param answer The command's reply.
/// @return The two framed replies, in order.
[[nodiscard]] std::vector<std::byte> AcceptedThen(std::vector<std::byte> const& answer)
{
    return Testing::Replies({ Wire::EncodeReply(Wire::Status::Ok, {}), answer });
}

/// A resolver that never answers, so `RemoteUpstream` dials its configured endpoint.
///
/// Deliberate rather than a shortcut: a real lookup makes these cases depend on the
/// host's resolver, which `RemoteUpstream_test` has to `SKIP` around. A failed lookup
/// leaves the endpoint verbatim, which is the path that reaches the connector.
class UnresolvingResolver final: public IAsyncAddressResolver
{
  public:
    [[nodiscard]] Task<ResolveResult> Resolve(std::string host, std::uint16_t port, IReactor* /*reactor*/) override
    {
        co_return std::unexpected(ResolveFailure(host, port, "scripted: this case dials the literal"));
    }
};

/// An `ISocket` that forwards to one somebody else owns.
///
/// `IConnector::Connect` hands back a `unique_ptr`, and `RemoteUpstream` destroys it
/// when the operation ends -- so a connector that merely remembered the raw pointer
/// would be reading freed memory by the time a case asked what went out. That is not
/// a hypothetical: written that way, this file's first case segfaulted and its
/// sibling read an empty buffer, which is the *quiet* half and the one that would
/// have been mistaken for the defect under test.
///
/// A forwarding shell rather than a fourth private copy of a recording socket: the
/// script and the trace stay `Testing::ScriptedSocket`'s, which is the shared fake
/// this repository keeps for the reason that private copies carry private bugs.
class BorrowedSocket final: public ISocket
{
  public:
    /// @param inner Where every call goes; must outlive this.
    explicit BorrowedSocket(Testing::ScriptedSocket& inner):
        _inner { inner }
    {
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        return _inner.Read(buffer);
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        return _inner.Write(buffer);
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        return _inner.WriteVectored(segments, std::move(keepAlive));
    }

    void Close() noexcept override
    {
        _inner.Close();
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _inner.IsClosed();
    }

    [[nodiscard]] std::string PeerAddress() const override
    {
        return _inner.PeerAddress();
    }

  private:
    Testing::ScriptedSocket& _inner;
};

/// A connector handing out one scripted socket per dial, and OUTLIVING them.
///
/// Every case here dials twice -- once before the rotation and once after -- and the
/// question is what the SECOND connection sent. A connector returning one shared
/// socket would let the first exchange's bytes answer for the second, so each dial
/// gets its own; the connector keeps them so they can still be read afterwards.
class ScriptingConnector final: public IConnector
{
  public:
    /// @param script What each socket replays.
    explicit ScriptingConnector(std::vector<std::byte> script):
        _script { std::move(script) }
    {
    }

    [[nodiscard]] Task<SocketResult> Connect(std::string /*host*/, std::uint16_t /*port*/, DialOptions /*options*/) override
    {
        _dialled.push_back(std::make_unique<Testing::ScriptedSocket>(_script));
        co_return SocketResult { std::make_unique<BorrowedSocket>(*_dialled.back()) };
    }

    /// What each dial sent, oldest first.
    /// @param nth Which dial, zero-based.
    /// @return The bytes that connection wrote.
    [[nodiscard]] std::vector<std::byte> const& SentOn(std::size_t nth) const
    {
        return _dialled.at(nth)->Sent();
    }

    /// How many connections were opened.
    /// @return The dial count.
    [[nodiscard]] std::size_t Dials() const noexcept
    {
        return _dialled.size();
    }

  private:
    /// The sockets handed out, oldest first. Owned here, so a case may read them
    /// after the operation that used them has ended.
    std::vector<std::unique_ptr<Testing::ScriptedSocket>> _dialled;
    std::vector<std::byte> _script;
};

/// A load sampler that reports nothing, so a heartbeat round reads no host at all.
class SilentLoadSampler final: public IHostLoadSampler
{
  public:
    [[nodiscard]] HostLoad Sample() override
    {
        return HostLoad {};
    }
};

/// Write @p body to a configuration file this case owns.
/// @param dir Scratch directory.
/// @param body YAML text.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteConfig(std::filesystem::path const& dir, std::string_view body)
{
    std::filesystem::create_directories(dir);
    auto const path = dir / "node.yaml";
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out << body;
    return path;
}

/// Read @p path into a fresh configuration, exactly as the worker's reloader does.
/// @param path The configuration file.
/// @return The candidate, or why it could not be read.
[[nodiscard]] std::expected<NodeConfig, ConfigError> Reparse(std::filesystem::path const& path)
{
    NodeConfig candidate;
    auto const loaded = ReadYamlSettings(path).and_then([&candidate, &path](std::vector<YamlSetting> const& settings) {
        return ApplyNodeConfiguration(settings, path, {}, candidate);
    });
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    return candidate;
}

} // namespace

TEST_CASE("Site 1: the shared cache is asked with the secret in force NOW", "[node][credential][rotation]")
{
    // The cache tier's upstream client held `Cc::Credential { .secret = cfg.token }`,
    // copied when the tier was built. Everything here is the ordinary success path --
    // the daemon accepts the AUTH, answers the FETCH -- because the failure this
    // rejects produces an ordinary success path too, against a daemon that has not
    // rotated yet.
    RotatingCredential credential { FirstSecret };
    UnresolvingResolver resolver;
    ScriptingConnector connector { AcceptedThen(Wire::EncodeReply(Wire::Status::Miss, {})) };
    ManualClock clock;

    RemoteUpstream upstream { "127.0.0.1:6674", credential, [](std::string_view) {}, connector, nullptr,
                              resolver,         clock,      ConnectTimeout,          IoTimeout, RefreshInterval };

    (void) SyncRun(upstream.Fetch("k"));
    REQUIRE(connector.Dials() == 1);
    CHECK(connector.SentOn(0) == AuthThen(FirstSecret, Wire::EncodeFetch("k")));

    credential.Rotate(SecondSecret);
    (void) SyncRun(upstream.Fetch("k"));

    REQUIRE(connector.Dials() == 2);
    // Both directions. "Carries the new secret" alone would pass for a client that
    // sent both, and "does not carry the old one" alone would pass for a client that
    // sent no credential at all -- which is a different bug with the same green.
    CHECK(connector.SentOn(1) == AuthThen(SecondSecret, Wire::EncodeFetch("k")));
    CHECK(connector.SentOn(1) != connector.SentOn(0));
}

TEST_CASE("Site 1, the other verb: a STORE presents the rotated secret too", "[node][credential][rotation]")
{
    // `Fetch` and `Store` are two call sites in one class, and a fix applied to the
    // one a test happened to drive is exactly the shape of this ticket one level
    // down.
    RotatingCredential credential { FirstSecret };
    UnresolvingResolver resolver;
    ScriptingConnector connector { AcceptedThen(Wire::EncodeReply(Wire::Status::Ok, {})) };
    ManualClock clock;

    RemoteUpstream upstream { "127.0.0.1:6674", credential, [](std::string_view) {}, connector, nullptr,
                              resolver,         clock,      ConnectTimeout,          IoTimeout, RefreshInterval };

    auto const value = std::vector<std::byte> { std::byte { 0x01 } };
    (void) SyncRun(upstream.Store("k", value));
    credential.Rotate(SecondSecret);
    (void) SyncRun(upstream.Store("k", value));

    REQUIRE(connector.Dials() == 2);
    auto const& second = connector.SentOn(1);
    auto const authOnly = Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = std::string { SecondSecret } });
    REQUIRE(second.size() >= authOnly.size());
    CHECK(std::vector<std::byte> { second.begin(), second.begin() + static_cast<std::ptrdiff_t>(authOnly.size()) }
          == authOnly);
}

TEST_CASE("Site 2: a cluster verb presents the secret in force NOW", "[node][credential][rotation]")
{
    // This verb cannot observe a rotation in production -- it runs once and the
    // process exits -- and it takes the seam anyway. The case therefore asserts the
    // property that outlives that fact: the site reads the source at the exchange,
    // so it cannot become the stale one when somebody calls it from a running worker.
    RotatingCredential credential { FirstSecret };
    auto notice = Cc::CredentialNotice::Silent();
    ClusterRequest const request { .action = ClusterAction::Status, .key = {}, .value = {} };

    Testing::ScriptedSocket first { AcceptedThen(Wire::EncodeReply(Wire::Status::Ok, {})) };
    (void) PutClusterRequest(first, notice, request, credential, "scheduler.example:6676");
    CHECK(first.Sent() == AuthThen(FirstSecret, EncodeClusterRequest(request)));

    credential.Rotate(SecondSecret);
    Testing::ScriptedSocket second { AcceptedThen(Wire::EncodeReply(Wire::Status::Ok, {})) };
    (void) PutClusterRequest(second, notice, request, credential, "scheduler.example:6676");

    CHECK(second.Sent() == AuthThen(SecondSecret, EncodeClusterRequest(request)));
    CHECK(second.Sent() != first.Sent());
}

TEST_CASE("Site 3: a registration presents the secret in force NOW", "[node][credential][rotation]")
{
    // The site that could not be shown at all before #404, because it lived in
    // `main.cpp` -- the one translation unit no test reaches. `HeartbeatRound` held a
    // `Cc::Credential const&` bound to a local `WorkerBody` built once, so this was
    // the site a rotation was GUARANTEED to miss while the other two moved.
    //
    // Asserted on the REGISTER frame's leading AUTH rather than on the whole exchange:
    // the registration's own payload carries this machine's capacity and a version
    // string, which are not what this case is about and would make it fail on an
    // unrelated wire change.
    NodeConfig cfg;
    cfg.scheduler = "scheduler.example:6676";

    RotatingCredential credential { FirstSecret };
    AtomicMetricsSink metrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    FleetSampler sampler { std::nullopt,
                           metrics,
                           [] {
                               return MetricsSnapshot { .storage = std::nullopt,
                                                        .storageTiers = {},
                                                        .host = HostCapacity { .configuredSlots = 1, .busySlots = 0 },
                                                        .upstreamConfigured = std::nullopt,
                                                        .uptime = {} };
                           },
                           // The process singleton, NOT a `SystemWallClock {}` temporary and not a
                           // local either. `FleetSampler` stores no clock; it hands the reference to
                           // `_fleet`, `_node` and `_received`, and `FleetHistory` keeps the ADDRESS
                           // (`IWallClock const* _wall`) and dereferences it from a thread the
                           // constructor itself starts -- so a temporary is read after it has died.
                           // At `-O0` the dead frame slot still holds a usable vptr and this passes;
                           // at `-O2` and above the locals declared after it reuse the slot and it is
                           // a SIGSEGV. Static storage leaves no lifetime to reason about, which is
                           // the argument `main.cpp` already makes beside its own use of it. Nothing
                           // here asserts on wall-clock VALUES, so the determinism a `ManualWallClock`
                           // would buy is not being given up. Compile-time guard: #1032.
                           DefaultSystemWallClock(),
                           HistoryPaths {},
                           logger };
    CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 1 }, logger };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::atomic<bool> fleetMismatch { false };

    Cc::CredentialNotice notice = Cc::CredentialNotice::Silent();
    std::vector<Cc::WorkerRegistrar> registrars;
    registrars.emplace_back(notice, "gcc-14", "10.0.0.2:6677", 1U, Wire::CodecList {}, Wire::CapacityFields {});

    HeartbeatRound const round { .cfg = cfg,
                                 .registrars = registrars,
                                 .capacity = capacity,
                                 .loadSampler = loadSampler,
                                 .cacheTier = nullptr,
                                 .metrics = metrics,
                                 .sampler = sampler,
                                 .credential = credential,
                                 .lease = lease,
                                 .fleetMismatch = fleetMismatch,
                                 .logger = logger };

    auto const authFor = [](std::string_view secret) {
        return Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = std::string { secret } });
    };
    auto const leadingAuth = [](Testing::ScriptedSocket const& socket, std::size_t length) {
        auto const& sent = socket.Sent();
        if (sent.size() < length)
            return std::vector<std::byte> {};
        return std::vector<std::byte> { sent.begin(), sent.begin() + static_cast<std::ptrdiff_t>(length) };
    };

    Testing::ScriptedSocket first { AcceptedThen(Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not today")) };
    (void) AnnounceOnce(round, first, cfg.scheduler);
    CHECK(leadingAuth(first, authFor(FirstSecret).size()) == authFor(FirstSecret));

    credential.Rotate(SecondSecret);
    Testing::ScriptedSocket second { AcceptedThen(Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not today")) };
    (void) AnnounceOnce(round, second, cfg.scheduler);

    CHECK(leadingAuth(second, authFor(SecondSecret).size()) == authFor(SecondSecret));
    // The round is `const` and was built once, before the rotation. That is the whole
    // point: a `Cc::Credential` member here could not have moved, and this assertion
    // is what says the member is a seam rather than a value.
    CHECK(leadingAuth(second, authFor(FirstSecret).size()) != authFor(FirstSecret));
}

TEST_CASE("The production source answers from the LIVE snapshot, not the startup one", "[node][credential][rotation]")
{
    // What the three cases above stand in for, driven through the real type over a
    // real edit: `--requirepass` is `Reloadable::Yes`, so the reload is ACCEPTED, and
    // `ConfiguredCredential` reads the snapshot it published rather than the
    // configuration this process started with.
    //
    // Both halves are load-bearing. A row that is not reloadable makes the reload
    // refuse and this case fail at the `REQUIRE`; a source that reads `cfg` makes it
    // fail at the `CHECK` -- and those are two different repairs in two different
    // files, which is why they are separate assertions rather than one.
    Testing::ScratchDirectory const scratch { "node-credential-rotation" };
    auto const path =
        WriteConfig(scratch.Path(), std::format("scheduler: scheduler.example:6676\nrequirepass: {}\n", SecondSecret));

    NodeConfig initial;
    initial.scheduler = "scheduler.example:6676";
    initial.token = std::string { FirstSecret };

    NodeReloader reloader { initial, path, &Reparse, &ValidateNodeReloadable };
    ConfiguredCredential const credential { initial, &reloader };

    CHECK(credential.Current().secret == FirstSecret);

    auto const outcome = reloader.Reload();
    INFO("reload said: " << (outcome.has_value() ? std::string { "accepted" } : outcome.error().ToString()));
    REQUIRE(outcome.has_value());

    CHECK(credential.Current().secret == SecondSecret);
}

TEST_CASE("A worker with no configuration file presents what it was started with", "[node][credential]")
{
    // The null-reloader arm is a deployment, not a fallback: a worker configured
    // entirely from argv has no second moment for anything to arrive at. It must keep
    // presenting its credential rather than presenting none, which is what a source
    // that treated "no reloader" as "no configuration" would do.
    NodeConfig cfg;
    cfg.token = std::string { FirstSecret };

    ConfiguredCredential const credential { cfg, nullptr };
    CHECK(credential.Current().secret == FirstSecret);
    CHECK(credential.Current().Configured());
}

TEST_CASE("Only the seam derives a credential from the configuration", "[node][credential][seam]")
{
    // **The guard is a scan, because nothing forces a site to reach for the seam.**
    //
    // The type system stops a site that HOLDS an `ICredentialSource const&` from going
    // stale -- there is no field to be stale in. It says nothing about a fourth site
    // that never asks for one and builds its own `Cc::Credential` from `cfg.token`
    // instead, which is exactly what the three sites this ticket is about did. That is
    // the rulebook's split: a guard folded INTO the operation is self-enforcing, a
    // guard nothing compels needs a scan.
    //
    // Test sources are excluded and they must be: this very file constructs
    // credentials, which is what makes it able to say what a rotation looks like.
    std::filesystem::path const nodeDir =
        std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node";
    REQUIRE(std::filesystem::is_directory(nodeDir));

    // A COMMENT is not a call site, and several files here discuss `Cc::Credential`
    // by name in prose that reaches the same shape. Full-line comments are dropped
    // before the search, which is the remedy this repository has already had to write
    // down twice.
    auto const withoutComments = [](std::string const& text) {
        std::string out;
        out.reserve(text.size());
        std::size_t line = 0;
        while (line < text.size())
        {
            auto const end = std::min(text.find('\n', line), text.size());
            auto const first = text.find_first_not_of(" \t", line);
            auto const isComment = first != std::string::npos && first < end && text.compare(first, 2, "//") == 0;
            if (!isComment)
                out.append(text, line, end - line);
            out.push_back('\n');
            line = end + 1;
        }
        return out;
    };

    std::vector<std::string> offenders;
    std::size_t scanned = 0;
    bool seamConstructsOne = false;

    for (auto const& entry: std::filesystem::directory_iterator { nodeDir })
    {
        auto const name = entry.path().filename().string();
        auto const extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".hpp")
            continue;
        if (name.ends_with("_test.cpp"))
            continue;

        // Via the stream buffer rather than `istreambuf_iterator`, which is the
        // remedy `DefaultConfigPath_test.cpp` and `fastcache-cc/FileBytes.hpp` both
        // carry: GCC at `-O3` inlines the iterator's `sbumpc()` far enough to see a
        // path where the buffer pointer could be null and refuses it under
        // `-Werror=null-dereference`, inside `<streambuf>` itself. Inserting a
        // `streambuf*` handles null by setting failbit, so there is nothing left to
        // complain about -- and it is the better implementation anyway: one read
        // rather than a per-character loop that regrows geometrically.
        //
        // Invisible to every gate this file was written under. It is a GCC-only,
        // RELEASE-only diagnostic, so an MSVC suite and a clang-tidy sweep are both
        // silent about it -- which is why `gcc-release` is a leg rather than a
        // weaker version of the others.
        std::ifstream in { entry.path(), std::ios::binary };
        std::ostringstream contents;
        contents << in.rdbuf();
        auto const text = std::move(contents).str();
        REQUIRE_FALSE(text.empty());
        ++scanned;

        auto const code = withoutComments(text);
        auto const constructs = code.contains("Cc::Credential {") || code.contains("Cc::Credential{");
        if (name == "NodeCredential.hpp")
        {
            seamConstructsOne = constructs;
            continue;
        }
        if (constructs)
            offenders.push_back(name);
    }

    // The positive control, in both halves. A scan over the wrong directory finds no
    // offenders and reads exactly like a clean one -- and a scan whose PATTERN has
    // stopped matching does too. The second half is the one that decays: a rename of
    // the construction's spelling would silently disarm this.
    CHECK(scanned > 20);
    INFO("the seam itself must construct one, or the pattern below means nothing");
    CHECK(seamConstructsOne);

    INFO("deriving a credential outside NodeCredential.hpp: " << [&] {
        std::string joined;
        for (auto const& name: offenders)
            joined += (joined.empty() ? "" : ", ") + name;
        return joined;
    }());
    CHECK(offenders.empty());
}
