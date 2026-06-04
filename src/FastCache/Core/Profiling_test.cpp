// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Profiling.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

// These tests pin the *compile-time and evaluation contract* of the Profiling
// wrapper, not Tracy's runtime behaviour (Tracy exposes no in-process query
// API for collected zones). They are built in whichever mode the active CMake
// preset selected: clang-tracy defines FC_TRACY_ENABLED, every other preset
// leaves it undefined, so CI exercises both branches of the wrapper.
//
// Note on Tracy's macro contract: FC_ZONE_SCOPED* declares a single
// fixed-name RAII guard, so at most one may appear per scope and its name must
// be a compile-time literal. FC_ZONE_NAME/FC_ZONE_TEXT annotate that current
// guard and DO take runtime (pointer, size) arguments — that is the macro used
// below to probe argument evaluation.

TEST_CASE("Profiling scope macros are well-formed statements", "[profiling]")
{
    // Each scoped zone lives in its own block: Tracy's guard has a fixed
    // variable name, so two in one scope would collide. Using each macro
    // proves it expands to a valid statement in the active build mode.
    {
        FC_ZONE_SCOPED;
    }
    {
        FC_ZONE_SCOPED_N("named-zone");
    }
    FC_FRAME_MARK;
    FC_FRAME_MARK_NAMED("request");
    FC_THREAD_NAME("test-thread");
    FC_PLOT("test.value", static_cast<std::int64_t>(1));
    SUCCEED("Profiling macros compiled and executed");
}

TEST_CASE("Disabled annotation macros discard their arguments unevaluated", "[profiling]")
{
    // FC_ZONE_TEXT takes a runtime (pointer, size) pair, so it is the macro
    // whose argument evaluation is observable. The wrapper's documented
    // contract: when the profiler is off the macros expand to (void) 0 and the
    // argument expressions are NOT evaluated, so callers must never rely on a
    // side-effect inside a macro argument. When the profiler is on, Tracy
    // evaluates the arguments and forwards them to the enclosing zone.
    int calls = 0;
    [[maybe_unused]] auto const probe = [&calls]() -> char const* {
        ++calls;
        return "dynamic-text";
    };

    {
        FC_ZONE_SCOPED_N("arg-eval");
        FC_ZONE_TEXT(probe(), std::size_t { 12 });
    }

#if defined(FC_TRACY_ENABLED)
    CHECK(calls == 1);
#else
    CHECK(calls == 0);
#endif
}

TEST_CASE("Profiling build mode is reported", "[profiling]")
{
#if defined(FC_TRACY_ENABLED)
    SUCCEED("Built with Tracy profiler ENABLED");
#else
    SUCCEED("Built with Tracy profiler DISABLED (zero-cost no-op macros)");
#endif
}
