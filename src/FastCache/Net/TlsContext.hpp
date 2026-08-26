// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/NetError.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
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

    /// How long a generated self-signed certificate is valid for.
    ///
    /// A year, and long rather than short deliberately: the certificate is
    /// regenerated on every restart anyway, so the expiry is a backstop for a
    /// process that runs untouched for a very long time rather than a rotation
    /// policy. Short-lived certificates buy nothing here and cost an outage the
    /// day a node stays up past them.
    static constexpr std::chrono::seconds DefaultSelfSignedValidity { 365 * 24 * 60 * 60 };

    /// Build a server context over a certificate generated here and now.
    ///
    /// For an internal deployment where obtaining a certificate is the only thing
    /// standing between an operator and an encrypted admin surface. **It buys
    /// confidentiality, not identity**: nothing signs this certificate, so a client
    /// that has not been told its fingerprint out of band cannot tell the node from
    /// anything else that answers on that address. That is why the credential is
    /// still required on a non-loopback bind, and why the fingerprint is reported.
    ///
    /// Held in memory only. Writing it out would mean choosing a path, owning a
    /// private key on disk and getting its permissions right on three platforms --
    /// all to save regenerating a keypair that costs milliseconds. The cost is that
    /// the certificate changes on every restart, so a browser exception pinned to
    /// it has to be granted again; an operator who wants a stable identity names a
    /// real certificate instead.
    ///
    /// @param subjectNames Names the certificate is valid for. Each is emitted as
    ///        an IP SAN when it parses as an address literal and a DNS SAN
    ///        otherwise, because a browser ignores the common name entirely and a
    ///        name mismatch is far harder to click past than an unknown issuer.
    ///        Must not be empty.
    /// @param validity How long it is valid for.
    /// @return The context, or a NetError describing what failed.
    [[nodiscard]] static std::expected<std::unique_ptr<TlsContext>, NetError> CreateSelfSigned(
        std::span<std::string const> subjectNames, std::chrono::seconds validity = DefaultSelfSignedValidity);

    /// The SHA-256 fingerprint of the certificate being served, as lower-case hex.
    ///
    /// The only thing that authenticates a self-signed certificate, so it is worth
    /// being able to print: an operator compares it against what their browser
    /// shows and knows they reached the node rather than something in between.
    /// @return The fingerprint, or empty when the context carries no certificate.
    [[nodiscard]] std::string CertificateFingerprint() const;

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
