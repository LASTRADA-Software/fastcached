# SPDX-License-Identifier: Apache-2.0
#
# Refuse to believe a Windows Debug build is exercising MSVC's checked runtime
# until a program that MUST die has died.
#
# Registered as `ctest -R iterator-debug-canary`, and only when the build is MSVC
# and Debug -- see src/tests/CMakeLists.txt for why the guard does not weaken the
# check.
#
# ## Why this exists
#
# The point of a Windows Debug leg is `_ITERATOR_DEBUG_LEVEL=2`: it traps
# invalidated iterators, out-of-range indexing and mismatched container iterators
# at the point they happen, which is the class of defect a Release build tolerates
# in silence. But nothing in this project states that level. It follows from
# `_DEBUG`, which follows from the runtime library flavour, which follows from
# `CMAKE_BUILD_TYPE` and `CMAKE_MSVC_RUNTIME_LIBRARY`. Any of those moving removes
# the checks without touching a line of our code and without a warning -- and the
# leg would still compile, still run the whole suite, and still report green while
# checking nothing it was added to check.
#
# That is the same shape as a sanitizer that is on in the cache and absent from the
# build, and it gets the same answer: a canary that would have gone red.
#
# ## What counts as proof
#
# A non-zero exit is NOT enough on its own. The canary must die *of the thing being
# tested*, so its output has to carry the debug runtime's own diagnostic -- the same
# reason `scripts/tsan-gate.sh` greps for `data race` rather than trusting exit 66.
# A canary that crashed for an unrelated reason is a canary proving nothing.

[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Canary)

$ErrorActionPreference = "Stop"

function Fail([string]$message) {
    Write-Host "ITERATOR-DEBUG GATE FAILED: $message"
    exit 1
}

if (-not (Test-Path $Canary)) {
    # A missing binary and an unchecked runtime are different failures with
    # different fixes, and they must not share a message.
    Fail "the canary was not built: $Canary`n    Build it before running this gate; it is guarded to MSVC Debug builds."
}

# The canary sets its own report modes so the assertion goes to stderr rather than
# to a modal dialog -- without that, this line hangs a CI job until its timeout.
$stdout = & $Canary 2>&1 | Out-String
$code = $LASTEXITCODE

if ($code -eq 0) {
    Fail @"
the deliberate out-of-range vector subscript was NOT trapped.
    The canary read past the end of a vector and returned normally, so this build
    is NOT exercising MSVC's checked iterators and the Debug leg is checking
    nothing it was added to check. _ITERATOR_DEBUG_LEVEL follows from _DEBUG,
    which follows from the runtime library: check CMAKE_BUILD_TYPE and
    CMAKE_MSVC_RUNTIME_LIBRARY reached the compile line, not just the cache.
    Canary output:
$stdout
"@
}

if ($stdout -notmatch "subscript out of range") {
    Fail @"
the canary exited $code but did not report a vector subscript violation.
    Something else killed it, so this run is no evidence that the checked runtime
    is live. Read the output before changing anything:
$stdout
"@
}

Write-Host "iterator-debug-canary: out-of-range subscript trapped (exit $code) -- MSVC's checked iterators are live"
exit 0
