// SPDX-License-Identifier: Apache-2.0
//
// Refuses to compile when the compiler was told to assume an instruction set above the
// architecture's baseline (#1447). Attached to every executable this build defines by
// `cmake/InstructionSetProbe.cmake`, so it is compiled by the same driver, with the same
// flags and IN THE SAME ENVIRONMENT as the executable's own units.
//
// ## Why a unit the build compiles, and not a check that reads the build
//
// `scripts/check-instruction-set-flags.cmake` (#1442) reads the compile database, which
// records only what CMake PUT on a command line. A wider instruction set reaches a unit by
// three other routes, and the database shows none of them:
//
//   - the build-time environment: `cl`'s `CL` and `_CL_`, clang's `CCC_OVERRIDE_OPTIONS`;
//   - driver configuration files clang reads beside its binary, or `--config`;
//   - defaults a compiler was built with -- a GCC configured `--with-arch=`.
//
// Every one of those still arrives in the compiler's PREDEFINED MACROS, so this asks the
// compiler rather than the database. And it asks while BUILDING rather than from a test,
// because a test runs in ctest's environment, which is not the build's: a `CL` set for
// the build and not for the suite would pass a probe driven from ctest. Here there is no
// second environment to get wrong. The two checks complement each other: #1442 names the
// FLAG, which this cannot see, and this sees what no flag spells.
//
// ## Why it matters although every runner passes
//
// A binary compiled for AVX2 or SHA runs everywhere CI runs it -- the runners HAVE the
// instructions -- and dies of SIGILL on the machine that lacks them. A per-function
// `__attribute__((target(...)))` behind a CPU check (`Core/CpuFeatures`, `Core/Sha256`) is
// the legitimate spelling, and it defines no translation-unit-wide macro, so it never
// reaches this file.
//
// ## The table
//
// One block per architecture, one line per macro above that architecture's baseline, each
// with its own `#error` so the diagnostic NAMES the macro. The preprocessor has no loop,
// so the table is spelled as its rows. An architecture with no block is REFUSED rather
// than passed: a build this file cannot judge is not a build it has cleared.
//
//   x86-64    the x86-64 baseline (SSE2). Everything x86-64-v2 adds and above is refused.
//   aarch64   Armv8.0-A, on Linux and Windows. A Raspberry Pi 4's Cortex-A72 has no LSE
//             atomics and no crypto extension, so `__ARM_FEATURE_ATOMICS` and the crypto
//             macros are refused, as is any `__ARM_ARCH` above 8, which is how clang and
//             cl both state Armv8.1 and later.
//   Apple     arm64 macOS runs only on Apple silicon, whose floor is the M1: its compilers
//             define the crypto, atomics and dot-product macros by DEFAULT, and every Mac
//             that can run the binary has them. Only what an M1 lacks is refused.

#if defined(__x86_64__) || defined(_M_X64)
    #if defined(__SSE3__)
        #error "instruction set above the x86-64 baseline: __SSE3__ is defined (#1447)"
    #endif
    #if defined(__SSSE3__)
        #error "instruction set above the x86-64 baseline: __SSSE3__ is defined (#1447)"
    #endif
    #if defined(__SSE4_1__)
        #error "instruction set above the x86-64 baseline: __SSE4_1__ is defined (#1447)"
    #endif
    #if defined(__SSE4_2__)
        #error "instruction set above the x86-64 baseline: __SSE4_2__ is defined (#1447)"
    #endif
    #if defined(__POPCNT__)
        #error "instruction set above the x86-64 baseline: __POPCNT__ is defined (#1447)"
    #endif
    #if defined(__AVX__)
        #error "instruction set above the x86-64 baseline: __AVX__ is defined (#1447)"
    #endif
    #if defined(__AVX2__)
        #error "instruction set above the x86-64 baseline: __AVX2__ is defined (#1447)"
    #endif
    #if defined(__AVX512F__)
        #error "instruction set above the x86-64 baseline: __AVX512F__ is defined (#1447)"
    #endif
    #if defined(__AVX10_VER__)
        #error "instruction set above the x86-64 baseline: __AVX10_VER__ is defined (#1447)"
    #endif
    #if defined(__FMA__)
        #error "instruction set above the x86-64 baseline: __FMA__ is defined (#1447)"
    #endif
    #if defined(__F16C__)
        #error "instruction set above the x86-64 baseline: __F16C__ is defined (#1447)"
    #endif
    #if defined(__BMI__)
        #error "instruction set above the x86-64 baseline: __BMI__ is defined (#1447)"
    #endif
    #if defined(__BMI2__)
        #error "instruction set above the x86-64 baseline: __BMI2__ is defined (#1447)"
    #endif
    #if defined(__LZCNT__)
        #error "instruction set above the x86-64 baseline: __LZCNT__ is defined (#1447)"
    #endif
    #if defined(__MOVBE__)
        #error "instruction set above the x86-64 baseline: __MOVBE__ is defined (#1447)"
    #endif
    #if defined(__AES__)
        #error "instruction set above the x86-64 baseline: __AES__ is defined (#1447)"
    #endif
    #if defined(__PCLMUL__)
        #error "instruction set above the x86-64 baseline: __PCLMUL__ is defined (#1447)"
    #endif
    #if defined(__SHA__)
        #error "instruction set above the x86-64 baseline: __SHA__ is defined (#1447)"
    #endif
    #if defined(__VAES__)
        #error "instruction set above the x86-64 baseline: __VAES__ is defined (#1447)"
    #endif
    #if defined(__VPCLMULQDQ__)
        #error "instruction set above the x86-64 baseline: __VPCLMULQDQ__ is defined (#1447)"
    #endif
    #if defined(__GFNI__)
        #error "instruction set above the x86-64 baseline: __GFNI__ is defined (#1447)"
    #endif
    #if defined(__ADX__)
        #error "instruction set above the x86-64 baseline: __ADX__ is defined (#1447)"
    #endif
#elif defined(__APPLE__) && defined(__aarch64__)
    #if defined(__ARM_FEATURE_SVE)
        #error "instruction set above the Apple M1: __ARM_FEATURE_SVE is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SVE2)
        #error "instruction set above the Apple M1: __ARM_FEATURE_SVE2 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SME)
        #error "instruction set above the Apple M1: __ARM_FEATURE_SME is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_BF16)
        #error "instruction set above the Apple M1: __ARM_FEATURE_BF16 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_MATMUL_INT8)
        #error "instruction set above the Apple M1: __ARM_FEATURE_MATMUL_INT8 is defined (#1447)"
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #if defined(__ARM_ARCH) && __ARM_ARCH > 8
        #error "instruction set above Armv8.0-A: __ARM_ARCH is above 8 (#1447)"
    #endif
    #if defined(__ARM_FEATURE_ATOMICS)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_ATOMICS is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_CRYPTO)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_CRYPTO is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_AES)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_AES is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SHA2)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SHA2 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SHA3)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SHA3 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SHA512)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SHA512 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_DOTPROD)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_DOTPROD is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SVE)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SVE is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SVE2)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SVE2 is defined (#1447)"
    #endif
    #if defined(__ARM_FEATURE_SME)
        #error "instruction set above Armv8.0-A: __ARM_FEATURE_SME is defined (#1447)"
    #endif
#else
    #error \
        "no instruction-set baseline row for this architecture: add one to src/tests/InstructionSetBaseline.cpp, with its reason (#1447)"
#endif
