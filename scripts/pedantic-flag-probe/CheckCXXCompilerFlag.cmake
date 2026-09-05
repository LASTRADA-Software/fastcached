# SPDX-License-Identifier: Apache-2.0
#
# A stand-in for CMake's own `CheckCXXCompilerFlag`, reached only by
# `scripts/check-pedantic-suppressions.cmake` through `CMAKE_MODULE_PATH`.
#
# `cmake/portable/PedanticCompiler.cmake` opens with `include(CheckCXXCompilerFlag)`
# and every flag it adds goes through `check_cxx_compiler_flag`, which runs
# `try_compile` -- unavailable in `cmake -P` script mode, and a real compiler spawn
# per flag even where it is available. The check wants to know WHICH CONDITION each
# flag is gated on, not whether this host's compiler accepts it, so it answers yes
# to everything and lets the subject file's own `if()`s do the deciding.
#
# Answering yes to everything is deliberately the STRONGER reading, not a shortcut.
# A probe that failed would drop a flag for a reason that has nothing to do with the
# rule under test, and a dropped flag cannot violate it -- so a real compiler would
# make this check weaker on exactly the hosts where a flag is unavailable, and the
# weakening would be invisible. Every flag present means every flag judged.
#
# A stub MODULE rather than a `macro()` defined in the check, because the subject's
# own `include(CheckCXXCompilerFlag)` runs after anything the check could define and
# would overwrite it. `CMAKE_MODULE_PATH` is searched before CMake's own Modules
# directory, so this is the copy that `include()` finds.
#
# A macro and not a function: the real one is a macro, `try_add_compile_options`
# reads the result variable in its own scope, and a function would set it in a scope
# that ends before the `if()` that reads it -- which reads as every probe FAILING and
# would leave the check judging an empty flag list.
# @param _flag The compiler flag being probed; unused, and named for the signature.
# @param _var Name of the variable the result is reported through.
macro(check_cxx_compiler_flag _flag _var)
    set(${_var} TRUE)
endmacro()
