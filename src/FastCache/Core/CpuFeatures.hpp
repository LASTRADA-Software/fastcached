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
/// It answers what THIS PROCESS can execute, which is decided by the architecture
/// it was compiled for: an x86-64 process under emulation on arm64 runs x86-64
/// opcodes, and CPUID there reports what the emulator provides. That is
/// `ToolchainHost.hpp`'s `HostFacts::architecture` question, not
/// `NativeArchitecture()`'s, so the compiled-architecture guards in the
/// implementation are right and must not become a query of the machine.
///
/// Detection exists only where a CI leg compiles and runs it: x86-64 on every
/// leg, arm64 macOS on the macOS leg, and Linux aarch64 and Windows ARM64 on the
/// `-arm64` legs of the Linux and Windows jobs (#1432). Anywhere else nothing is
/// reported, so the portable scalar paths run -- correct, only slower -- because a
/// detection branch no leg compiles is one that rots with nothing to say so.
/// ARM64EC defines `_M_X64` as well and not `_M_ARM64`, so such a build would take
/// the x86 branches; no preset targets it.
///
/// In Core rather than Platform because its consumer, `Sha256`, is Core, and
/// Core does not reach up into Platform.
struct CpuFeatures
{
    bool x86Sha = false;   ///< The x86 SHA extensions (SHA-NI).
    bool x86Ssse3 = false; ///< SSSE3, which the SHA-NI rounds need to reorder bytes.
    bool x86Sse41 = false; ///< SSE4.1, which the SHA-NI rounds need for their blends.
    bool armSha2 = false;  ///< The ARMv8 SHA-256 instructions.
};

/// Ask this process's CPU which extensions it offers.
/// @return What the CPU reports; all false where this build does not detect.
[[nodiscard]] CpuFeatures DetectCpuFeatures() noexcept;

} // namespace FastCache
