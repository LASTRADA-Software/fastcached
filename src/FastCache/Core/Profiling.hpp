// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Profiling.hpp
/// Thin Tracy profiler wrapper. Every macro compiles to nothing unless the
/// build was configured with -DTRACY_ENABLE=ON, which both links the Tracy
/// client and defines FC_TRACY_ENABLED on the FastCache target. Keying off our
/// own FC_TRACY_ENABLED (rather than Tracy's internal TRACY_ENABLE) decouples
/// this header from Tracy's macro surface and guarantees no Tracy header is
/// pulled in when the profiler is disabled.
///
/// Coroutine constraint: an FC_ZONE_SCOPED* object is a stack-RAII guard tied
/// to the current thread's Tracy zone stack and MUST NOT straddle a co_await
/// (under the reactor model the await resumes on a later frame and the zone's
/// destructor would corrupt the stack). Place zones only in synchronous leaf
/// functions or in `{ }` blocks containing no co_await. FC_FRAME_MARK is a
/// stackless timeline event and is safe anywhere, including inside coroutines.
///
/// Caller contract: arguments passed to these macros must be free of
/// side-effects the program relies on — when the profiler is disabled the
/// macros expand to `(void) 0` and the arguments are discarded unevaluated.

#if defined(FC_TRACY_ENABLED)

    #include <tracy/Tracy.hpp>

    /// Scoped zone covering the enclosing C++ scope, auto-named by source location.
    #define FC_ZONE_SCOPED ZoneScoped
    /// Scoped zone with an explicit compile-time literal name.
    #define FC_ZONE_SCOPED_N(name) ZoneScopedN(name)
    /// Attach a dynamic name to the current zone. @param txt char pointer. @param size length.
    #define FC_ZONE_NAME(txt, size) ZoneName(txt, size)
    /// Attach a dynamic text annotation to the current zone. @param txt char pointer. @param size length.
    #define FC_ZONE_TEXT(txt, size) ZoneText(txt, size)
    /// Attach a numeric value to the current zone. @param value integral value.
    #define FC_ZONE_VALUE(value) ZoneValue(value)
    /// Mark the end of one logical frame/request on the default frame timeline.
    #define FC_FRAME_MARK FrameMark
    /// Named frame variant (separate timeline per name). @param name string literal.
    #define FC_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
    /// Name the calling OS thread in the Tracy timeline. @param name char pointer.
    #define FC_THREAD_NAME(name) tracy::SetThreadName(name)
    /// Plot a named scalar value over time. @param name string literal. @param value numeric value.
    #define FC_PLOT(name, value) TracyPlot(name, value)

#else

    #define FC_ZONE_SCOPED            (void) 0
    #define FC_ZONE_SCOPED_N(name)    (void) 0
    #define FC_ZONE_NAME(txt, size)   (void) 0
    #define FC_ZONE_TEXT(txt, size)   (void) 0
    #define FC_ZONE_VALUE(value)      (void) 0
    #define FC_FRAME_MARK             (void) 0
    #define FC_FRAME_MARK_NAMED(name) (void) 0
    #define FC_THREAD_NAME(name)      (void) 0
    #define FC_PLOT(name, value)      (void) 0

#endif
