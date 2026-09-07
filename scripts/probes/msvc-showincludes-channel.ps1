# Which stream does `cl /showIncludes` write its notes to, with and without `/EP`?
#
# This is the measurement #825 asks for and nobody on this team could take: it needs a
# real MSVC toolchain and every maintainer here is on Linux. Committed so that whoever
# next has a Windows host can settle it with one command instead of re-deriving the
# question -- a citation people can re-run is one they stop re-litigating, which is the
# idiom `redis-eof-semantics.py` and `ninja-msvc-deps-prefix.sh` set.
#
# ## The contradiction it settles
#
# Three sites in this tree said `cl` writes `/showIncludes` on **stderr**; the analysis
# on #700 and the review of #821 said **stdout**. Neither was measured. All three sites
# now say UNVERIFIED and point at `.agent/rules/compile-cache.md`, which states the
# question once -- so the tree no longer contradicts itself, and this is what replaces
# "unverified" with an answer.
#
# The `clang-cl` half IS measured (LLVM D46394): `/c` puts notes on stdout, `/EP` moves
# them to stderr so they do not corrupt the preprocessed text. Case 3 re-measures it
# here as a CONTROL -- if it disagrees with D46394 the probe itself is wrong, and no
# verdict about `cl` from the same run should be believed.
#
# ## Why it matters even though nothing is broken today
#
# At the one site that reads it, both regions are tagged `ShowIncludes` either way, so
# the replay is correct under either reading. It stops being inert the moment something
# ACTS on the channel, and something nearly did: #821 built a dispatch refusal on a
# predicate fed from stderr alone. Under one reading that guard covers `cl`; under the
# other it cannot fire for `cl` at all.
#
# ## Running it
#
#     powershell -ExecutionPolicy Bypass -File scripts\probes\msvc-showincludes-channel.ps1
#
# From a Visual Studio developer prompt, or anywhere `cl` and `clang-cl` are on PATH.
# Streams are captured to SEPARATE FILES rather than to a pipeline, because PowerShell's
# merge operators would answer the very question being asked.
#
# Record the output on #825, including the toolchain version and whether a language pack
# is installed: the answer may differ for a localized `cl`, which is the case #700 is
# about, and a reading taken on an en-US install does not settle a localized one.

$ErrorActionPreference = 'Stop'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("showincludes-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null

try {
    # A header the note must name, so a matched line is provably about THIS include and
    # not some toolchain header that happens to appear.
    $marker = 'fastcache_probe_header.h'
    Set-Content -Path (Join-Path $work $marker) -Value '/* probe */' -Encoding Ascii
    Set-Content -Path (Join-Path $work 'tu.cpp') -Value @"
#include "$marker"
int main() { return 0; }
"@ -Encoding Ascii

    function Measure-Channel {
        param([string]$Driver, [string]$Label, [string[]]$ExtraArgs)

        # The label is prose ("cl /EP"); a filename cannot carry its slash or space on
        # Windows, and a failed redirect would make every reading NEITHER -- which this
        # probe reports as "not a channel answer" rather than silently as stderr.
        $slug = $Label -replace '[^A-Za-z0-9]', '_'
        $out = Join-Path $work "$slug.out"
        $err = Join-Path $work "$slug.err"
        $argv = @('/nologo', '/showIncludes') + $ExtraArgs + @('tu.cpp')

        $p = Start-Process -FilePath $Driver -ArgumentList $argv -WorkingDirectory $work `
                           -RedirectStandardOutput $out -RedirectStandardError $err `
                           -NoNewWindow -PassThru -Wait -ErrorAction SilentlyContinue
        if ($null -eq $p) {
            Write-Host ("  {0,-22} DRIVER NOT FOUND ({1}) -- no reading, not a result" -f $Label, $Driver)
            return
        }

        # The note is identified by naming the probe header, never by the English
        # prefix: a localized toolchain writes a different sentence, and matching the
        # sentence would make this probe answer only for en-US -- which is the exact
        # class of mistake #700 records.
        $onOut = @(Select-String -Path $out -SimpleMatch $marker -ErrorAction SilentlyContinue).Count
        $onErr = @(Select-String -Path $err -SimpleMatch $marker -ErrorAction SilentlyContinue).Count

        $verdict =
            if ($onOut -gt 0 -and $onErr -gt 0) { 'BOTH -- unexpected, report it' }
            elseif ($onOut -gt 0)               { 'stdout' }
            elseif ($onErr -gt 0)               { 'stderr' }
            else { 'NEITHER -- the note was not produced; this is not a channel answer' }

        Write-Host ("  {0,-22} exit={1,-4} stdout={2,-3} stderr={3,-3} -> {4}" -f `
                    $Label, $p.ExitCode, $onOut, $onErr, $verdict)

        # The PREFIX, which is #878's question and comes free from the same run.
        #
        # Printed with delimiters and as a hex dump, because the two hazards #878 names
        # are both invisible in plain output:
        #
        #   1. `cl` INDENTS nested notes, so "the line minus the known path" captures
        #      the indent into the prefix. Only the top-level note is read here, and the
        #      bytes show whether trailing blanks are part of what was captured.
        #   2. A localized DIAGNOSTIC can also end in a known path, so a heuristic can
        #      learn a prefix that is not the note prefix at all. Every matching line is
        #      shown rather than the first, so a second shape is visible rather than
        #      silently adopted.
        #
        # This does NOT implement discovery. It is the reading a discovery would have to
        # be built on, taken on a machine that has what this one does not.
        foreach ($f in @($out, $err)) {
            foreach ($hit in @(Select-String -Path $f -SimpleMatch $marker -ErrorAction SilentlyContinue)) {
                $line = $hit.Line
                # Everything before the note's PATH -- and the path is found EXACTLY,
                # not guessed at. This probe created the header, so it knows the full
                # path the note should name; cutting at the last separator instead would
                # leave the directory inside the "prefix", which is the shape that makes
                # a derived prefix silently wrong and is hazard 1 in #878.
                #
                # Which anchor matched is REPORTED, because "the note named the full
                # path" and "it named something else ending in the file name" are
                # different observations and a discovery built on the second would be
                # learning the wrong string.
                $full = Join-Path $work $marker
                $cut  = $line.IndexOf($full)
                $how  = 'full path'
                if ($cut -lt 0) { $cut = $line.LastIndexOf($marker); $how = 'FILE NAME ONLY -- the note did not name the path this probe expected' }
                $prefix = $line.Substring(0, $cut)
                $hex = ($prefix.ToCharArray() | ForEach-Object { '{0:x2}' -f [int]$_ }) -join ' '
                Write-Host ("      note on {0}" -f (Split-Path $f -Leaf))
                Write-Host ("        full line   : [{0}]" -f $line)
                Write-Host ("        before path : [{0}]   (anchored on the {1})" -f $prefix, $how)
                Write-Host ("        those bytes : {0}" -f $hex)
            }
        }
    }

    Write-Host "toolchain:"
    foreach ($d in @('cl', 'clang-cl')) {
        $found = Get-Command $d -ErrorAction SilentlyContinue
        if ($found) { Write-Host ("  {0}: {1}" -f $d, $found.Source) }
        else        { Write-Host ("  {0}: NOT ON PATH" -f $d) }
    }
    Write-Host ""
    Write-Host "readings (a note is identified by naming $marker, never by its English prefix):"

    # The two `cl` cases are the open question.
    Measure-Channel -Driver 'cl'       -Label 'cl /c'          -ExtraArgs @('/c')
    Measure-Channel -Driver 'cl'       -Label 'cl /EP'         -ExtraArgs @('/EP')
    # CONTROLS: measured already (LLVM D46394). If these disagree, distrust the run.
    Measure-Channel -Driver 'clang-cl' -Label 'clang-cl /c'    -ExtraArgs @('/c')
    Measure-Channel -Driver 'clang-cl' -Label 'clang-cl /EP'   -ExtraArgs @('/EP')

    # ------------------------------------------------------------------
    # And the SECOND question this run can answer for free, because it needs the
    # same `cl`: can this machine produce a LOCALIZED one at all?
    #
    # [#878](https://github.com/LASTRADA-Software/fastcached/issues/878) asks for
    # "a means of exercising the localized case that is not a stub asserting its
    # own premise", and says outright that *"this needs a machine we do not have"*
    # is a legitimate finding. It is only legitimate if somebody MEASURED it.
    #
    # `VSLANG` selects `cl`'s diagnostic language -- the launcher already sets it on
    # its probe spawns for exactly that reason (#692, #200) -- but it can only
    # select a language whose resources are INSTALLED. So: ask for German, and see
    # whether the note comes back different. Same, and the runner has en-US only;
    # different, and the localized case is exercisable here after all.
    #
    # Compared BYTE FOR BYTE against the English run rather than pattern-matched for
    # German: the question is "did the language change", and a matcher for German
    # words would answer "no" for every language that is not German, which is the
    # narrower question wearing the wider one's clothes.
    Write-Host ""
    Write-Host "#878: can this machine produce a localized cl?"
    $langDir = Join-Path $work "lang"
    New-Item -ItemType Directory -Path $langDir -Force | Out-Null
    Set-Content -Path (Join-Path $langDir $marker) -Value '/* probe */' -Encoding Ascii
    Set-Content -Path (Join-Path $langDir 'tu.cpp') -Value @"
#include "$marker"
int main() { return 0; }
"@ -Encoding Ascii

    function Note-For {
        param([string]$Lang)
        $out = Join-Path $langDir ("lang-" + $Lang + ".out")
        $err = Join-Path $langDir ("lang-" + $Lang + ".err")
        $prev = $env:VSLANG
        if ($Lang -eq 'default') { Remove-Item Env:VSLANG -ErrorAction SilentlyContinue }
        else { $env:VSLANG = $Lang }
        try {
            Start-Process -FilePath 'cl' -ArgumentList @('/nologo', '/showIncludes', '/c', 'tu.cpp') `
                          -WorkingDirectory $langDir -RedirectStandardOutput $out -RedirectStandardError $err `
                          -NoNewWindow -Wait -ErrorAction SilentlyContinue | Out-Null
        } finally {
            if ($null -eq $prev) { Remove-Item Env:VSLANG -ErrorAction SilentlyContinue } else { $env:VSLANG = $prev }
        }
        foreach ($f in @($out, $err)) {
            $hit = @(Select-String -Path $f -SimpleMatch $marker -ErrorAction SilentlyContinue)[0]
            if ($null -ne $hit) { return $hit.Line }
        }
        return $null
    }

    # 1031 is de-DE. Any installed non-English pack would do; one is enough to
    # answer the question, and a machine with de-DE and not en-US is not a case
    # this needs to distinguish.
    $english = Note-For 'default'
    $german  = Note-For '1031'
    if ($null -eq $english) {
        Write-Host "  INCONCLUSIVE: no note in the default language, so there is nothing to compare against"
    } elseif ($null -eq $german) {
        Write-Host "  INCONCLUSIVE: VSLANG=1031 produced no note at all; that is not 'the same language'"
    } elseif ($english -eq $german) {
        Write-Host "  SAME under VSLANG=1031 -- this machine has en-US resources only."
        Write-Host "  So #878's localized case is NOT exercisable here, measured rather than assumed."
        Write-Host "  default: [$english]"
    } else {
        Write-Host "  DIFFERENT under VSLANG=1031 -- a language pack IS present and the localized"
        Write-Host "  case is exercisable on this runner. #878's clause 1 has an answer that is not"
        Write-Host "  'a machine we do not have'."
        Write-Host "  default : [$english]"
        Write-Host "  VSLANG=1031: [$german]"
    }

    Write-Host ""
    Write-Host "controls: clang-cl /c is expected stdout and clang-cl /EP stderr (LLVM D46394)."
    Write-Host 'If either disagrees, this probe is wrong and its cl readings prove nothing.'
    Write-Host 'Report the cl rows on #825 with the toolchain version and whether a language'
    Write-Host 'pack is installed: a reading on en-US does not settle a localized cl.' 
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
