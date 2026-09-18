// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ISecureRandom.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <system_error>

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <bcrypt.h>
#elif defined(__linux__) || defined(__APPLE__)
    // Linux declares `getrandom` here, and macOS declares `getentropy` here rather than in
    // <unistd.h>, where POSIX.1-2024 puts it.
    #include <sys/random.h>
#else
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
#if !defined(_WIN32)
    /// What an `errno` says, for `SecureRandomError::detail`.
    /// @param code The `errno` the primitive set.
    /// @return Its text and its number.
    [[nodiscard]] std::string ErrnoText(int code)
    {
        return std::format("{} (errno {})", std::generic_category().message(code), code);
    }
#endif

#if defined(_WIN32)
    /// The most one `BCryptGenRandom` call fills: its length is a `ULONG`.
    constexpr std::size_t PrimitiveLimit = std::numeric_limits<ULONG>::max();
#elif defined(__linux__)
    /// No limit worth chunking at: `getrandom` answers a SHORT read above 32 MiB, and the
    /// loop below takes short reads anyway.
    constexpr std::size_t PrimitiveLimit = std::numeric_limits<std::size_t>::max();
#else
    /// The most one `getentropy` call fills; asking for more is `EINVAL`, not a short read.
    constexpr std::size_t PrimitiveLimit = 256;
#endif

    /// Fill at most `PrimitiveLimit` bytes of @p out with one call of this platform's primitive.
    ///
    /// The table, and why each row is the primitive it is:
    ///
    ///   Linux      `getrandom(2)`, flags 0: the kernel CSPRNG with no file descriptor to run
    ///              out of and no `/dev` to be missing from a chroot. It BLOCKS until the pool
    ///              has been seeded once at boot and never after, which is the right answer for
    ///              a nonce -- an unseeded pool is exactly the weak draw this seam refuses.
    ///   macOS and  `getentropy(2)`, rather than `arc4random_buf`. Both read the kernel's
    ///   other      generator; the difference that decides it is that `arc4random_buf` returns
    ///   POSIX      `void` and CANNOT report a failure, while `getentropy` answers one -- and
    ///              this seam's contract is that a failure is a refusal, which a primitive that
    ///              cannot fail would make a promise nothing could ever exercise. Its 256-byte
    ///              ceiling is why the loop chunks. POSIX.1-2024 names `getentropy` in
    ///              `<unistd.h>`, which is what the last row relies on; only macOS declares it
    ///              in `<sys/random.h>` instead.
    ///   Windows    `BCryptGenRandom` with no algorithm handle and
    ///              `BCRYPT_USE_SYSTEM_PREFERRED_RNG`: the documented way to reach the system
    ///              generator without opening a provider, available since Windows 7.
    ///
    /// **`EINTR` is retried and nothing else is**: an interrupted call drew nothing and says so,
    /// while every other answer is a fact about this host that a retry would only repeat.
    /// @param out Where the bytes go; never empty, never longer than `PrimitiveLimit`.
    /// @return How many bytes were filled -- at least one -- or why none were.
    [[nodiscard]] std::expected<std::size_t, SecureRandomError> FillOnce(std::span<std::byte> out)
    {
#if defined(_WIN32)
        auto const status = ::BCryptGenRandom(nullptr,
                                              static_cast<PUCHAR>(static_cast<void*>(out.data())),
                                              static_cast<ULONG>(out.size()),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status))
            return std::unexpected { SecureRandomError {
                .primitive = "BCryptGenRandom",
                .detail = std::format("NTSTATUS {:#010x}", static_cast<std::uint32_t>(status)) } };
        return out.size();
#else
    #if defined(__linux__)
        constexpr std::string_view Primitive = "getrandom";
        auto const draw = [out] {
            return ::getrandom(out.data(), out.size(), 0);
        };
    #else
        constexpr std::string_view Primitive = "getentropy";
        auto const draw = [out] {
            return ::getentropy(out.data(), out.size()) == 0 ? static_cast<long>(out.size()) : -1L;
        };
    #endif
        auto drawn = draw();
        while (drawn < 0 && errno == EINTR)
            drawn = draw();

        if (drawn > 0)
            return static_cast<std::size_t>(drawn);

        // Zero bytes for a non-empty request is not an answer either primitive gives, and taking
        // it as one would spin the caller's loop forever; so it is a refusal too.
        return std::unexpected { SecureRandomError {
            .primitive = Primitive,
            .detail = drawn < 0 ? ErrnoText(errno) : std::string { "it filled nothing and reported no error" } } };
#endif
    }
} // namespace

std::expected<void, SecureRandomError> SystemSecureRandom::Fill(std::span<std::byte> out)
{
    while (!out.empty())
    {
        auto const filled = FillOnce(out.first(std::min(out.size(), PrimitiveLimit)));
        if (!filled.has_value())
            return std::unexpected { filled.error() };
        out = out.subspan(*filled);
    }
    return {};
}

} // namespace FastCache
