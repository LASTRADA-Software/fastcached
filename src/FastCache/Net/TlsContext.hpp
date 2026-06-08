// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>

#include <expected>
#include <memory>
#include <string_view>

// Forward declaration so this header pulls in no OpenSSL headers (and so the
// public surface is unchanged whether or not TLS is compiled in).
extern "C" struct ssl_ctx_st;

namespace FastCache
{

/// Owns a server-side OpenSSL `SSL_CTX` (certificate chain + private key),
/// shared read-only across every TLS connection for the server's lifetime.
/// RAII over the `SSL_CTX`. Construction validates that the key matches the
/// certificate and fails with a NetError rather than throwing.
class TlsContext
{
  public:
    /// Build a server context from PEM files.
    /// @param certPath PEM certificate (chain) file.
    /// @param keyPath PEM private key file.
    /// @return The context, or a NetError describing the load failure.
    [[nodiscard]] static std::expected<std::unique_ptr<TlsContext>, NetError> Create(std::string_view certPath,
                                                                                     std::string_view keyPath);

    TlsContext(TlsContext const&) = delete;
    TlsContext(TlsContext&&) = delete;
    TlsContext& operator=(TlsContext const&) = delete;
    TlsContext& operator=(TlsContext&&) = delete;
    ~TlsContext();

    /// @return The underlying SSL_CTX (never null for a live context).
    [[nodiscard]] ssl_ctx_st* Native() const noexcept
    {
        return _ctx;
    }

  private:
    explicit TlsContext(ssl_ctx_st* ctx) noexcept:
        _ctx { ctx }
    {
    }

    ssl_ctx_st* _ctx;
};

} // namespace FastCache
