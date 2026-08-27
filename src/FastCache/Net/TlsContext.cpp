// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/TlsContext.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace FastCache
{

namespace
{

    /// Drain one entry from the OpenSSL error queue (if any) into a NetError.
    /// @param context Human-readable description of what was being attempted.
    [[nodiscard]] NetError SslError(std::string context)
    {
        unsigned long const code = ERR_get_error();
        if (code != 0)
        {
            std::array<char, 256> buffer {};
            ERR_error_string_n(code, buffer.data(), buffer.size());
            context += ": ";
            context += buffer.data();
        }
        return NetError { .code = NetErrorCode::SystemError,
                          .systemCode = static_cast<int>(code),
                          .context = std::move(context) };
    }

    /// `std::unique_ptr` deleters for the OpenSSL objects built below.
    ///
    /// Every one of these functions has several failure exits, and a hand-written
    /// `X509_free` on each of them is how one gets forgotten. RAII is the rule this
    /// codebase applies to every other resource handle; OpenSSL's C types are no
    /// exception just because they came from a C library.
    struct EvpPkeyDeleter
    {
        void operator()(EVP_PKEY* p) const noexcept
        {
            EVP_PKEY_free(p);
        }
    };
    struct EvpPkeyCtxDeleter
    {
        void operator()(EVP_PKEY_CTX* p) const noexcept
        {
            EVP_PKEY_CTX_free(p);
        }
    };
    struct X509Deleter
    {
        void operator()(X509* p) const noexcept
        {
            X509_free(p);
        }
    };
    struct SslCtxDeleter
    {
        void operator()(SSL_CTX* p) const noexcept
        {
            SSL_CTX_free(p);
        }
    };
    struct Asn1StringDeleter
    {
        void operator()(ASN1_OCTET_STRING* p) const noexcept
        {
            ASN1_OCTET_STRING_free(p);
        }
    };

    using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
    using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
    using X509Ptr = std::unique_ptr<X509, X509Deleter>;
    using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

    /// Generate a P-256 keypair.
    ///
    /// Elliptic curve rather than RSA, because this runs on the startup path: an
    /// RSA-2048 keygen is occasionally a second or more of a node not answering,
    /// while P-256 is sub-millisecond and is what a modern client prefers anyway.
    /// Built through the generic `EVP_PKEY_CTX` API rather than the 3.0-only
    /// convenience functions, so this compiles against OpenSSL 1.1.1 too -- the
    /// build names no minimum, so it must not silently acquire one.
    /// @return The key, or nullptr with the reason on the error queue.
    [[nodiscard]] PkeyPtr GenerateKey()
    {
        PkeyCtxPtr ctx { EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr) };
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1)
            return nullptr;
        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) != 1)
            return nullptr;
        // A named curve rather than explicit parameters: some clients refuse the
        // explicit encoding outright, and it is larger for no benefit.
        if (EVP_PKEY_CTX_set_ec_param_enc(ctx.get(), OPENSSL_EC_NAMED_CURVE) != 1)
            return nullptr;

        EVP_PKEY* raw = nullptr;
        if (EVP_PKEY_keygen(ctx.get(), &raw) != 1)
            return nullptr;
        return PkeyPtr { raw };
    }

    /// Render the subject-alternative-name extension value for `names`.
    ///
    /// Each name is classified by what it *is* rather than by how it is spelled:
    /// `a2i_IPADDRESS` is asked whether the text parses as an address literal, and
    /// only what it refuses becomes a DNS name. Guessing from the spelling -- a
    /// digit at the front, a colon somewhere -- gets `2001:db8::1` and a host
    /// called `10things` wrong in opposite directions, and the certificate then
    /// silently does not match the name an operator types.
    /// @param names What the certificate should be valid for.
    /// @return An OpenSSL SAN configuration string.
    [[nodiscard]] std::string SubjectAltNames(std::span<std::string const> names)
    {
        std::string value;
        for (auto const& name: names)
        {
            if (name.empty())
                continue;
            if (!value.empty())
                value += ',';
            std::unique_ptr<ASN1_OCTET_STRING, Asn1StringDeleter> const address { a2i_IPADDRESS(name.c_str()) };
            value += address ? "IP:" : "DNS:";
            value += name;
        }
        return value;
    }

} // namespace

std::expected<std::unique_ptr<TlsContext>, NetError> TlsContext::Create(std::string_view certPath, std::string_view keyPath)
{
    // Clear any stale entries left on this thread's OpenSSL error queue so a
    // failure below is reported with its own error, not an earlier unrelated one
    // (SslError drains the queue via ERR_get_error, which is FIFO).
    ERR_clear_error();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr)
        return std::unexpected(SslError("SSL_CTX_new failed"));

    // Refuse legacy protocol versions: TLS 1.2 is the floor.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    std::string const cert { certPath };
    std::string const key { keyPath };

    if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1)
    {
        auto error = SslError("loading certificate '" + cert + "'");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1)
    {
        auto error = SslError("loading private key '" + key + "'");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }
    if (SSL_CTX_check_private_key(ctx) != 1)
    {
        auto error = SslError("private key does not match certificate");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }

    return std::unique_ptr<TlsContext>(new TlsContext(ctx));
}

std::expected<std::unique_ptr<TlsContext>, NetError> TlsContext::CreateSelfSigned(std::span<std::string const> subjectNames,
                                                                                  std::chrono::seconds validity)
{
    ERR_clear_error();

    // Refused rather than defaulted to something: a certificate valid for no name
    // matches nothing, so every client would reject it -- an encrypted surface
    // nobody can reach, which looks like a network fault rather than a
    // misconfiguration.
    // A comma ENDS one entry and starts another in the config grammar below, so a
    // name carrying one would silently add a SAN nobody asked for -- and a
    // certificate valid for the wrong name is exactly what this function's own
    // argument says must not happen. These names are operator-supplied (a hostname
    // and the `--admin-listen` bind host), so it is a typo away.
    //
    // A colon is deliberately NOT refused: `X509V3_parse_list` splits the type
    // from the value at the FIRST one, so the rest is the value -- which is what
    // makes `::1` a legal IPv6 SAN rather than a malformed entry.
    if (auto const injected = std::ranges::find_if(subjectNames, [](std::string const& name) { return name.contains(','); });
        injected != subjectNames.end())
        return std::unexpected(
            NetError { .code = NetErrorCode::SystemError,
                       .systemCode = 0,
                       .context = std::format("a subject name may not contain a comma: '{}'", *injected) });

    auto const names = SubjectAltNames(subjectNames);
    if (names.empty())
        // `SystemError` because this taxonomy has no enumerator for "the caller
        // asked for something incoherent", and inventing one for a single
        // internal precondition would widen a wire-facing vocabulary for it.
        return std::unexpected(NetError { .code = NetErrorCode::SystemError,
                                          .systemCode = 0,
                                          .context = "a self-signed certificate needs at least one subject name" });

    auto key = GenerateKey();
    if (!key)
        return std::unexpected(SslError("generating a P-256 key"));

    X509Ptr cert { X509_new() };
    if (!cert)
        return std::unexpected(SslError("X509_new failed"));

    // Version 3, which is what an X509v3 extension requires -- and the SAN below
    // is one. The field is zero-based, so 2 IS version 3.
    if (X509_set_version(cert.get(), 2) != 1)
        return std::unexpected(SslError("setting the certificate version"));

    // A fixed serial is fine for a certificate nothing else ever sees: serial
    // uniqueness matters to a CA issuing many, and this issues exactly one.
    if (ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1)
        return std::unexpected(SslError("setting the certificate serial"));

    if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr
        || X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(validity.count())) == nullptr)
        return std::unexpected(SslError("setting the certificate validity"));

    if (X509_set_pubkey(cert.get(), key.get()) != 1)
        return std::unexpected(SslError("setting the certificate public key"));

    // Subject and issuer are the same name, which is what "self-signed" means.
    X509_NAME* subject = X509_get_subject_name(cert.get());
    auto const* const commonName = reinterpret_cast<unsigned char const*>("fastcache-node");
    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, commonName, -1, -1, 0) != 1
        || X509_set_issuer_name(cert.get(), subject) != 1)
        return std::unexpected(SslError("setting the certificate subject"));

    X509V3_CTX extensionContext {};
    X509V3_set_ctx_nodb(&extensionContext);
    X509V3_set_ctx(&extensionContext, cert.get(), cert.get(), nullptr, nullptr, 0);

    // The extension a browser actually reads. Every modern client ignores the
    // common name above entirely, so without this the certificate matches nothing
    // whatever it is called.
    if (auto* const san = X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name, names.c_str()))
    {
        auto const added = X509_add_ext(cert.get(), san, -1);
        X509_EXTENSION_free(san);
        if (added != 1)
            return std::unexpected(SslError("adding the subjectAltName extension"));
    }
    else
        return std::unexpected(SslError("building the subjectAltName extension"));

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0)
        return std::unexpected(SslError("signing the certificate"));

    SslCtxPtr ctx { SSL_CTX_new(TLS_server_method()) };
    if (!ctx)
        return std::unexpected(SslError("SSL_CTX_new failed"));

    // The same floor `Create` sets: a generated certificate is no reason to accept
    // a protocol version a named one would not be served over.
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);

    if (SSL_CTX_use_certificate(ctx.get(), cert.get()) != 1)
        return std::unexpected(SslError("installing the generated certificate"));
    if (SSL_CTX_use_PrivateKey(ctx.get(), key.get()) != 1)
        return std::unexpected(SslError("installing the generated private key"));
    if (SSL_CTX_check_private_key(ctx.get()) != 1)
        return std::unexpected(SslError("the generated key does not match its certificate"));

    return std::unique_ptr<TlsContext>(new TlsContext(ctx.release()));
}

std::string TlsContext::CertificateFingerprint() const
{
    if (_ctx == nullptr)
        return {};
    X509 const* const cert = SSL_CTX_get0_certificate(_ctx);
    if (cert == nullptr)
        return {};

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), digest.data(), &length) != 1)
        return {};

    std::string out;
    out.reserve(std::size_t { length } * 2);
    for (unsigned int i = 0; i < length; ++i)
    {
        std::array<char, 3> byte {};
        (void) std::snprintf(byte.data(), byte.size(), "%02x", digest[i]);
        out += byte.data();
    }
    return out;
}

TlsContext::~TlsContext()
{
    if (_ctx != nullptr)
        SSL_CTX_free(_ctx);
}

} // namespace FastCache
