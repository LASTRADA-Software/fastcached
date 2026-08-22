// SPDX-License-Identifier: Apache-2.0
#include "WorkerServer.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <CompileJob.hpp>

using namespace FastCache;
using namespace FastCache::Node;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
}

/// A runner that writes a canned object.
class StubRunner final: public Cc::IProcessRunner
{
  public:
    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        for (std::size_t i = 0; i + 1 < argv.size(); ++i)
            if (argv[i] == "-o")
                std::ofstream { argv[i + 1], std::ios::binary } << "OBJECT";
        return Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }
};

/// A listener that hands out one prepared connection and then reports EOF, so a
/// single `Run()` serves exactly one request and returns.
class OneShotListener final: public IListener
{
  public:
    explicit OneShotListener(std::unique_ptr<ISocket> socket):
        _socket { std::move(socket) }
    {
    }

    AcceptAwaitable Accept() override
    {
        if (_socket != nullptr)
            return AcceptAwaitable { AcceptResult { std::move(_socket) } };
        // After the one connection, report EOF so the loop returns and the test
        // completes rather than parking on a listener nothing will ever feed.
        return AcceptAwaitable { AcceptResult { std::unexpect,
                                                NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} } } };
    }
    void Close() noexcept override {}

  private:
    std::unique_ptr<ISocket> _socket;
};

struct Fixture
{
    StubRunner runner;
    std::filesystem::path scratch;
    Cc::CompileJobRunner jobs;
    Cc::WorkerProtocol protocol;
    NullLogger logger;

    Fixture():
        scratch { std::filesystem::temp_directory_path() / std::format("fc-ws-{}", ++Counter()) },
        jobs { runner, (std::filesystem::create_directories(scratch), scratch), { { "gcc-13", "g++" } } },
        protocol { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec } }
    {
    }
    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    static int& Counter()
    {
        static int counter = 0;
        return counter;
    }
};

[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13")
{
    constexpr std::string_view Source = "int main(){return 0;}";
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Source.size()), Wire::AsBytes(Source));
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = enveloped,
                                                      .acceptedCodecs = { Wire::IdentityCodec } });
}

/// Drive one request through a server and return everything written back.
[[nodiscard]] std::vector<std::byte> ServeOne(Fixture& fix, std::vector<std::byte> const& request, std::size_t slots)
{
    auto pair = InMemorySocketPair::Create();
    REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
        auto const r = co_await s->Write(std::span<std::byte const> { bytes });
        co_return r.has_value();
    }(pair.client.get(), request)));
    pair.client->ShutdownWrite();

    OneShotListener listener { std::move(pair.server) };
    WorkerServer server { listener, fix.protocol, slots, fix.logger };
    SyncRun(server.Run());

    return SyncRun([](ISocket* s) -> Task<std::vector<std::byte>> {
        std::vector<std::byte> out;
        while (true)
        {
            std::vector<std::byte> chunk(4096);
            auto const r = co_await s->Read(std::span<std::byte> { chunk });
            if (!r.has_value() || *r == 0)
                break;
            out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
            if (*r < chunk.size())
                break;
        }
        co_return out;
    }(pair.client.get()));
}

[[nodiscard]] Wire::ErrorCode ErrorOf(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    if (!header.has_value())
        return Wire::ErrorCode::MalformedFrame;
    auto const body = std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize);
    auto const decoded = Wire::DecodeErrorPayload(body);
    return decoded.has_value() ? decoded->first : Wire::ErrorCode::MalformedFrame;
}

} // namespace

TEST_CASE("A worker server answers one compile and closes", "[worker-server]")
{
    Fixture fix;
    auto const reply = ServeOne(fix, CompileFrame(), /*slots=*/2);

    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Ok);
}

TEST_CASE("A worker at capacity refuses rather than queues", "[worker-server]")
{
    // Refused, never queued: queueing hides the overload from the scheduler that is
    // trying to route around it, and the client has a local compile waiting either
    // way. Zero slots is the degenerate case of a full worker.
    Fixture fix;
    auto const reply = ServeOne(fix, CompileFrame(), /*slots=*/0);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::NoCapacity);
}

TEST_CASE("A capacity refusal is answered before the payload is read", "[worker-server]")
{
    // The cap is checked before the request is buffered, so an over-capacity client
    // does not make this worker hold its multi-megabyte payload first. Observable
    // as an answer to a frame whose body was never sent.
    Fixture fix;
    auto truncated = CompileFrame();
    truncated.resize(Wire::RequestHeaderSize); // header only; the body never arrives
    CHECK(ErrorOf(ServeOne(fix, truncated, /*slots=*/0)) == Wire::ErrorCode::NoCapacity);
}

TEST_CASE("A foreign magic gets no reply at all", "[worker-server]")
{
    // There is no framing in which an answer would be meaningful to a peer that is
    // not speaking this protocol, so the connection is simply closed.
    Fixture fix;
    std::vector<std::byte> junk(Wire::RequestHeaderSize, std::byte { 'G' });
    CHECK(ServeOne(fix, junk, /*slots=*/2).empty());
}

TEST_CASE("A wrong-fingerprint job is refused by the server path too", "[worker-server]")
{
    Fixture fix;
    CHECK(ErrorOf(ServeOne(fix, CompileFrame("clang-19"), /*slots=*/2)) == Wire::ErrorCode::FingerprintMismatch);
}

TEST_CASE("In-flight returns to zero after a job", "[worker-server]")
{
    // The heartbeat reports this number, and a count that drifted upward would make
    // the scheduler believe a worker is busier than it is -- taking it out of
    // rotation permanently and silently.
    Fixture fix;
    auto pair = InMemorySocketPair::Create();
    auto const request = CompileFrame();
    REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
        auto const r = co_await s->Write(std::span<std::byte const> { bytes });
        co_return r.has_value();
    }(pair.client.get(), request)));
    pair.client->ShutdownWrite();

    OneShotListener listener { std::move(pair.server) };
    WorkerServer server { listener, fix.protocol, 2, fix.logger };
    SyncRun(server.Run());
    CHECK(server.InFlight() == 0);
}
