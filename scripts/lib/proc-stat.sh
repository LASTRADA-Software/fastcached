# SPDX-License-Identifier: Apache-2.0
#
# The one reader of `/proc/<pid>/stat` in the scripts: the gate's lock-holder walk
# (`local-gate.sh`) and the reaper's ancestry and process table (`reap-my-gate.sh`) both
# ask it, so a fix to how the file is read reaches both. Sourced, never run.
#
# bash 3.2, because a hygiene script `ctest` runs is constrained to it.

# One process's parent pid from `/proc/<pid>/stat`, left in `proc_stat_reply` (REPLY-style),
# or status 1 with `proc_stat_reply` empty when the file is absent, unreadable, or does not
# parse -- never a guess. Linux writes the file, and so does the MSYS2 runtime Git Bash runs on.
#
# The command name in that file is parenthesised and may itself contain `) ` (a program
# named `x) y` is legal), so the fields are read after the LAST `) `: `<pid> (<comm>)
# <state> <ppid> ...`. FORK-FREE, and the answer comes back through a variable for that
# reason: a `read` rather than `cat`, and no `$(...)` at the caller, because the reaper's
# process table asks it once per process on the machine and a command substitution is a
# fork each time.
#
# @param 1 the pid
proc_stat_reply=""
proc_stat_ppid() {
    local stat=""
    proc_stat_reply=""
    [[ -r "/proc/$1/stat" ]] || return 1
    IFS= read -r stat 2>/dev/null < "/proc/$1/stat" || [[ -n "$stat" ]] || return 1
    [[ "$stat" == *") "* ]] || return 1
    stat="${stat##*) }"
    stat="${stat#* }"
    stat="${stat%% *}"
    [[ -n "$stat" && "$stat" != *[!0-9]* ]] || return 1
    proc_stat_reply="$stat"
}
