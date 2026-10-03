# SPDX-License-Identifier: Apache-2.0
#
# The environment a PowerShell e2e fixture starts from -- the PowerShell half of
# `e2e_scrub_fastcache_environment` in `e2e-common.sh`.
#
# The launcher, fastcached and the node read their whole configuration from FASTCACHE_*
# variables, so one this process inherited decides what a fixture measures: an operator's
# FASTCACHE_SCHEDULER dispatches each "local" compile to their fleet, whose objects name a
# worker's scratch directory instead of the fixture's checkout, and FASTCACHE_VERIFY or
# FASTCACHE_NO_DIRECT changes the outcome a case asserts. A fixture sets what it means and
# inherits nothing.
#
# IMPORTING this module clears them, so a fixture gets that by construction rather than by
# remembering a loop: two fixtures carried their own copy, one of them wedged into the
# middle of an unrelated comment, and the rest carried none. `E2EPorts.psm1` imports it, so
# every fixture that draws a port is covered too. Import it BEFORE the fixture's own first
# `$env:FASTCACHE_*` assignment -- which is what "the fixture sets what it means" requires.

# A variable the HARNESS reads and no server or launcher does, which an operator sets on
# purpose and which a fixture must therefore see. None on this side today; the POSIX list
# names why each of its rows is there. A row here is a claim that nothing this project
# ships reads the name.
$script:InheritedKnobs = @()

<#
.SYNOPSIS
    Remove every FASTCACHE_* variable from this process's environment, except the
    harness knobs in `$InheritedKnobs`.
#>
function Clear-FastcacheEnvironment {
    Get-ChildItem Env: |
        Where-Object { $_.Name -like 'FASTCACHE_*' -and $script:InheritedKnobs -notcontains $_.Name } |
        ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }
}

Clear-FastcacheEnvironment

Export-ModuleMember -Function Clear-FastcacheEnvironment
