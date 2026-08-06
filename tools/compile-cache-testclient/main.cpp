// SPDX-License-Identifier: Apache-2.0
//
// Standalone compile-cache test client — the reference localizer.
//
// Drives a real compiler with /showIncludes, frames the result as a
// CompileValue, and STOREs/FETCHes it against a running fastcached over the
// custom 0xFC protocol. On FETCH it localizes the canonical paths back to the
// consumer's layout (linking the same PathCanon the server canonicalizes with,
// so the parity contract is exercised, not reimplemented).
//
// This tool exists purely for cross-depth validation (see run-crossdepth.ps1).
// It contains NO project-specific source; it compiles whatever source file it
// is pointed at and prints only synthetic counts/paths under the caller's
// control.

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Endian.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{

using namespace FastCache;

constexpr std::byte CompileCacheMagic { 0xFC };
constexpr std::byte OpStore { 0x01 };
constexpr std::byte OpFetch { 0x02 };
constexpr std::byte StatusOk { 0x01 };

/// Print an error and exit non-zero.
[[noreturn]] void Die(std::string_view message)
{
    std::cerr << "testclient: " << message << '\n';
    std::exit(2);
}

/// Read an entire file into a byte vector.
[[nodiscard]] std::vector<std::byte> ReadFileBytes(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        Die("cannot open file: " + path.string());
    std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (char const c: raw)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

/// Write a byte vector to a file.
void WriteFileBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream out { path, std::ios::binary };
    if (!out)
        Die("cannot write file: " + path.string());
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Run a command, capturing its stdout+stderr text. Returns (exitCode, output).
/// Uses _popen so /showIncludes lines (emitted on stderr by cl, stdout by
/// clang-cl) are both captured via a `2>&1` redirection appended by the caller.
[[nodiscard]] std::pair<int, std::string> RunCapture(std::string const& command)
{
    std::string output;
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr)
        Die("failed to spawn compiler");
    std::array<char, 4096> buffer {};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        output += buffer.data();
#if defined(_WIN32)
    int const code = _pclose(pipe);
#else
    int const code = pclose(pipe);
#endif
    return { code, output };
}

// --- Minimal blocking TCP client -------------------------------------------

/// A connected TCP socket with blocking send/recv. Windows-only for now
/// (matches the compile-cache target platform).
class TcpClient
{
  public:
    TcpClient(std::string_view host, std::uint16_t port)
    {
#if defined(_WIN32)
        WSADATA wsa {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            Die("WSAStartup failed");
        _fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_fd == INVALID_SOCKET)
            Die("socket() failed");
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, std::string { host }.c_str(), &addr.sin_addr) != 1)
            Die("bad host address");
        if (connect(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            Die("connect() failed — is fastcached running?");
#else
        Die("testclient TCP is Windows-only");
#endif
    }

    ~TcpClient()
    {
#if defined(_WIN32)
        if (_fd != INVALID_SOCKET)
            closesocket(_fd);
        WSACleanup();
#endif
    }

    TcpClient(TcpClient const&) = delete;
    TcpClient& operator=(TcpClient const&) = delete;
    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

    /// Send all bytes.
    void SendAll(std::span<std::byte const> bytes)
    {
        std::size_t sent = 0;
        while (sent < bytes.size())
        {
            int const n =
                send(_fd, reinterpret_cast<char const*>(bytes.data()) + sent, static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0)
                Die("send() failed");
            sent += static_cast<std::size_t>(n);
        }
    }

    /// Receive exactly `count` bytes.
    [[nodiscard]] std::vector<std::byte> RecvExactly(std::size_t count)
    {
        std::vector<std::byte> out(count);
        std::size_t got = 0;
        while (got < count)
        {
            int const n = recv(_fd, reinterpret_cast<char*>(out.data()) + got, static_cast<int>(count - got), 0);
            if (n <= 0)
                Die("recv() failed (peer closed early)");
            got += static_cast<std::size_t>(n);
        }
        return out;
    }

  private:
#if defined(_WIN32)
    SOCKET _fd { INVALID_SOCKET };
#else
    int _fd { -1 };
#endif
};

/// Append a big-endian u32.
void AppendU32(std::vector<std::byte>& out, std::uint32_t n)
{
    std::array<std::byte, sizeof(std::uint32_t)> buf {};
    WriteBigEndian<std::uint32_t>(buf, n);
    out.insert(out.end(), buf.begin(), buf.end());
}

/// Append a length-prefixed field.
void AppendField(std::vector<std::byte>& out, std::string_view s)
{
    AppendU32(out, static_cast<std::uint32_t>(s.size()));
    auto const* p = reinterpret_cast<std::byte const*>(s.data());
    out.insert(out.end(), p, p + s.size());
}
void AppendField(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    AppendU32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A parsed command line for one sub-command.
struct Args
{
    std::string host { "127.0.0.1" };
    std::uint16_t port { 0 };
    std::string key;
    std::string cohort { "default" };
    std::string srcRoot;
    std::string buildTree;
    std::string compiler { "cl" }; // or clang-cl
    std::string source;
    std::string object; // output object path (store) / expected-write path (fetch)
};

/// Pull a required string option `--name value`.
[[nodiscard]] std::string_view Opt(std::span<char const* const> argv, std::string_view name)
{
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        if (name == argv[i])
            return argv[i + 1];
    return {};
}

// --- store: compile, frame, STORE -----------------------------------------

int DoStore(Args const& a)
{
    // Compile with /showIncludes, capturing header lines. cl emits them on
    // stderr, so redirect 2>&1. Produce the object at a temp path we then read.
    std::filesystem::path const objPath = a.object.empty() ? (std::filesystem::temp_directory_path() / "cc-testclient.obj")
                                                           : std::filesystem::path { a.object };

    std::string const cmd =
        a.compiler + " /nologo /c /showIncludes /Fo\"" + objPath.string() + "\" \"" + a.source + "\" 2>&1";
    auto const [code, output] = RunCapture(cmd);
    if (code != 0)
    {
        std::cerr << "compiler failed (exit " << code << "):\n" << output << '\n';
        return 3;
    }

    CompileValue value;
    value.objectBlob = ReadFileBytes(objPath);
    value.textRegions.push_back({ PathCanon::Grammar::ShowIncludes, output });

    auto const encoded = EncodeCompileValue(value);

    std::vector<std::byte> frame;
    frame.push_back(CompileCacheMagic);
    frame.push_back(OpStore);
    AppendField(frame, a.key);
    AppendField(frame, a.cohort);
    AppendField(frame, a.srcRoot);
    AppendField(frame, a.buildTree);
    AppendField(frame, std::span<std::byte const> { encoded.data(), encoded.size() });

    TcpClient client { a.host, a.port };
    client.SendAll(frame);
    auto const reply = client.RecvExactly(1);
    if (reply[0] != StatusOk)
        Die("STORE rejected by server");

    std::cout << "STORE ok key=" << a.key << " objectBytes=" << value.objectBlob.size() << '\n';
    return 0;
}

// --- fetch: FETCH, localize, verify ----------------------------------------

int DoFetch(Args const& a)
{
    std::vector<std::byte> frame;
    frame.push_back(CompileCacheMagic);
    frame.push_back(OpFetch);
    AppendField(frame, a.key);

    TcpClient client { a.host, a.port };
    client.SendAll(frame);

    auto const status = client.RecvExactly(1);
    if (status[0] != StatusOk)
    {
        std::cout << "FETCH miss key=" << a.key << '\n';
        return 4;
    }
    auto const lenBytes = client.RecvExactly(sizeof(std::uint32_t));
    auto const len = ReadBigEndian<std::uint32_t>(lenBytes);
    auto const payload = client.RecvExactly(len);

    auto decoded = DecodeCompileValue(payload);
    if (!decoded.has_value())
        Die("FETCH returned a malformed compile-value");

    // Localize every region to the CONSUMER layout (srcRoot/buildTree passed on
    // the fetch command line) and check that each showIncludes path resolves.
    PathCanon::Layout const consumer { .sourceRoot = a.srcRoot, .buildTree = a.buildTree };
    std::size_t checked = 0;
    std::size_t missing = 0;
    for (auto const& region: decoded->textRegions)
    {
        auto const localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, consumer);
        if (!localized.has_value())
            Die("localization failed");
        // Each "Note: including file: <path>" line's path must exist on disk.
        std::string_view text = *localized;
        constexpr std::string_view prefix = "Note: including file:";
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t const nl = text.find('\n', pos);
            std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.starts_with(prefix))
            {
                std::string_view p = line.substr(prefix.size());
                while (!p.empty() && p.front() == ' ')
                    p.remove_prefix(1);
                ++checked;
                if (!p.empty() && !std::filesystem::exists(std::filesystem::path { std::string { p } }))
                {
                    ++missing;
                    std::cerr << "  UNRESOLVED: " << p << '\n';
                }
            }
            if (nl == std::string_view::npos)
                break;
            pos = nl + 1;
        }
    }

    // Write the object out so the caller can byte-compare it if desired.
    if (!a.object.empty())
        WriteFileBytes(std::filesystem::path { a.object }, decoded->objectBlob);

    std::cout << "FETCH ok key=" << a.key << " objectBytes=" << decoded->objectBlob.size()
              << " includePathsChecked=" << checked << " unresolved=" << missing << '\n';
    return missing == 0 ? 0 : 5;
}

} // namespace

int main(int argc, char** argv)
{
    std::span<char const* const> args { argv, static_cast<std::size_t>(argc) };
    if (argc < 2)
    {
        std::cerr << "usage: compile-cache-testclient <store|fetch> --port N [--key K] [--cohort C]\n"
                     "         --srcroot P --buildtree Q [--compiler cl|clang-cl] [--source F] [--out OBJ]\n";
        return 2;
    }

    Args a;
    auto const s = [&](std::string_view name, std::string& dst) {
        auto const v = Opt(args, name);
        if (!v.empty())
            dst = std::string { v };
    };
    s("--host", a.host);
    s("--key", a.key);
    s("--cohort", a.cohort);
    s("--srcroot", a.srcRoot);
    s("--buildtree", a.buildTree);
    s("--compiler", a.compiler);
    s("--source", a.source);
    s("--out", a.object);
    if (auto const p = Opt(args, "--port"); !p.empty())
        a.port = static_cast<std::uint16_t>(std::stoi(std::string { p }));
    if (a.port == 0)
        Die("--port is required");

    std::string_view const cmd = argv[1];
    if (cmd == "store")
        return DoStore(a);
    if (cmd == "fetch")
        return DoFetch(a);
    Die("unknown sub-command (expected store|fetch)");
}
