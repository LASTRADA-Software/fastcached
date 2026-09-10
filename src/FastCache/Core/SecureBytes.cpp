// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/SecureBytes.hpp>

#include <cstring>

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__GLIBC__)
    #include <strings.h>
#endif

namespace FastCache
{

namespace
{
#if !defined(_WIN32) && !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25)))
    /// The fallback's whole mechanism: a `volatile` pointer must be re-read at every call,
    /// so the compiler cannot see that it is calling `memset` and cannot reason about the
    /// store being dead. Used by libsodium and OpenSSL for the same reason.
    void* (*const volatile MemsetPointer)(void*, int, std::size_t) = &std::memset;
#endif
} // namespace

void SecureZero(void* data, std::size_t bytes) noexcept
{
    // A zero-length wipe is a no-op and is allowed to carry a null pointer -- an empty
    // credential buffer is ordinary, and `std::vector` may hold no allocation at all.
    if (bytes == 0)
        return;

    // The table, and what each row's guarantee actually rests on:
    //
    //   Windows        `SecureZeroMemory` -- documented by Microsoft as not subject to
    //                  dead-store elimination. The only row whose guarantee is a vendor
    //                  contract rather than an inference.
    //   glibc >= 2.25  `explicit_bzero` -- specified for this exact purpose. The version
    //                  gate is real: it did not exist before 2.25.
    //   everything     The volatile-pointer fallback above.
    //   else
    //
    // macOS and the BSDs deliberately take the fallback rather than their own
    // `explicit_bzero`. They do declare one, but its availability varies with the
    // deployment target CI builds against, and this lane cannot test those hosts -- a
    // wrong guess there is a build break on a platform nobody here would see first. The
    // fallback is sound everywhere, so the cost of being conservative is nothing but a
    // weaker paper guarantee.
#if defined(_WIN32)
    SecureZeroMemory(data, bytes);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(data, bytes);
#else
    MemsetPointer(data, 0, bytes);
    // A second barrier costs nothing and closes the one remaining reading, in which a
    // compiler proves the call has no observable effect on memory it is tracking.
    #if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(data) : "memory");
    #endif
#endif
}

} // namespace FastCache
