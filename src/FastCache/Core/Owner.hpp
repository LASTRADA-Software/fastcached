// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Project-local stand-in for `gsl::owner<T>` — a tag annotation indicating
/// that a pointer carries ownership of the pointee. It has no runtime effect;
/// clang-tidy's `cppcoreguidelines-owning-memory` check looks for the name
/// to verify that owning resources (FILE*, malloc-allocated buffers, etc.)
/// flow through correctly between functions.
///
/// We define our own rather than pull in microsoft/GSL just for this alias.
namespace gsl
{

template <typename T>
using owner = T;

} // namespace gsl
