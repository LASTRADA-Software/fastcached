// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace FastCache
{

/// The instruction-set extensions this process's CPU offers, as far as code in
/// this tree needs to know.
///
/// **This is the one ambient read of the CPU, and it is a stated exception to
/// injecting the environment.** A CPU's features do not change for the life of
/// the process, so an answer taken once cannot go stale. The exception is only
/// the READ: every DECISION is a pure function over this struct
/// (`SelectSha256Engine`), so a test hands it any combination and exercises
/// each branch on one machine, whatever that machine's CPU is.
///
/// Detection exists only where a CI leg compiles and runs it: x86-64 on every
/// leg, and arm64 macOS on the macOS leg. Everywhere else, Linux aarch64 and
/// Windows ARM64 included, nothing is reported, so the portable scalar paths
/// run. That is correct, only slower. A detection branch no leg compiles is
/// one that rots with nothing to say so (#ISSUE).
///
/// Dependency-free, like `Sha256`, because the launcher compiles both in
/// rather than linking the library.
struct CpuFeatures
{
    bool x86Sha = false;   ///< CPUID.(EAX=7,ECX=0):EBX bit 29, the SHA extensions.
    bool x86Ssse3 = false; ///< CPUID.1:ECX bit 9, which the SHA-NI rounds need for byte order.
    bool x86Sse41 = false; ///< CPUID.1:ECX bit 19, which the SHA-NI rounds need for their blends.
    bool armSha2 = false;  ///< ARMv8 SHA-256 instructions (macOS: `hw.optional.arm.FEAT_SHA256`).
};

/// Ask this process's CPU which extensions it offers.
/// @return What the CPU reports; all false where this build does not detect.
[[nodiscard]] CpuFeatures DetectCpuFeatures() noexcept;

} // namespace FastCache
