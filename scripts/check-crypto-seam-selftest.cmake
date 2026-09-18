# SPDX-License-Identifier: Apache-2.0
#
# `crypto-seam` must be SEEN to refuse an include past the seam, seen to stay QUIET over a healthy
# tree, and seen to refuse every way of not being able to conclude (#178).
#
# Each case stages a synthetic tree -- the two permitted Core units, a first-party caller of the
# Core header, a vendored Monocypher copy under a third-party root -- applies ONE change, and
# requires one verdict and the phrase that says which verdict fired. The accepting cases are what
# make the refusing ones evidence: a check that refused every tree would pass every refusal here.
#
#   clean, cleanGit                  a healthy tree, by directory walk and by a real git index
#   angled, relativeIntoVendor,      an include past the seam, in each spelling it can take --
#   upperCase, implementationFile,   angled, a relative path into vendor/, another case, the `.c`,
#   importDirective, violationGit    `#import`, and under git
#   commentedOut, bridgeHeader       a comment naming the header, and `MonocypherBytes.hpp`: quiet
#   vendoredDeclined, vendoredOwned  a vendored include is declined by the roots file, and refused
#                                    once the roots stop naming its directory
#   staleNoInclude, staleMissing     a permitted row that includes nothing, or names no file
#   empty                            no first-party C++ at all
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> [-DGIT_EXECUTABLE=<git>] \
#         -P scripts/check-crypto-seam-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-crypto-seam.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(ran 0)
set(failures "")

# A healthy tree: both permitted units include Monocypher, a caller includes the Core header only,
# and the vendored copy includes its own header under a third-party root.
#
# @param name The case name and directory.
# @param outVar Set to the tree's path.
function(fastcached_seam_tree name outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/monocypher\n")
    file(WRITE "${tree}/src/FastCache/Core/Ed25519.cpp"
        "#include <FastCache/Core/Ed25519.hpp>\n#include <FastCache/Core/MonocypherBytes.hpp>\n#include <monocypher-ed25519.h>\n")
    file(WRITE "${tree}/src/FastCache/Core/X25519.cpp" "#include <FastCache/Core/X25519.hpp>\n#include <monocypher.h>\n")
    file(WRITE "${tree}/src/FastCache/Node/Caller.cpp"
        "// Signs through the seam; never includes monocypher.h itself.\n#include <FastCache/Core/Ed25519.hpp>\n")
    # A header, as upstream's is: the scan reads C++ by extension, so a `.c` would never be read
    # and the vendoredOwned case below would pass without the roots file deciding anything.
    file(WRITE "${tree}/vendor/monocypher/src/optional/monocypher-ed25519.h" "#include \"monocypher.h\"\n")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Stages @p tree as a real git repository, so the enumeration CI uses is exercised rather than
# assumed. A synthetic tree is otherwise a directory walk in every case.
#
# @param tree The tree.
function(fastcached_stage_git tree)
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "crypto-seam-selftest: INCONCLUSIVE -- no git to stage ${tree} with")
    endif()
    foreach(step IN ITEMS "init;-q" "add;-A")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" ${step} RESULT_VARIABLE status
                        OUTPUT_QUIET ERROR_VARIABLE errors)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "crypto-seam-selftest: INCONCLUSIVE -- `git ${step}` in ${tree} failed: ${errors}")
        endif()
    endforeach()
endfunction()

# Runs the check over @p tree and judges it.
#
# @param name Case name.
# @param tree The tree.
# @param expect `accept` or `refuse`.
# @param mustSay A phrase the flattened output must contain: what says WHICH verdict fired.
function(fastcached_seam_judge name tree expect mustSay)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${check}"
        OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result MATCHES "^[0-9]+$")
        message(FATAL_ERROR "crypto-seam-selftest: INCONCLUSIVE -- the check could not be RUN (${result})")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}${errors}")
    string(REPLACE ";" "," flat "${flat}")
    set(objected FALSE)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(objected TRUE)
    endif()
    string(FIND "${flat}" "${mustSay}" said)
    if(expect STREQUAL "refuse" AND NOT objected)
        list(APPEND failures "${name}: accepted, and must refuse. Output: ${flat}")
    elseif(expect STREQUAL "accept" AND objected)
        list(APPEND failures "${name}: refused, and must accept. Output: ${flat}")
    elseif(said EQUAL -1)
        list(APPEND failures "${name}: ${expect}ed without saying `${mustSay}`. Output: ${flat}")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

set(accepted "2 of 2 permitted unit(s) seen including Monocypher, none other does")

fastcached_seam_tree(clean tree)
fastcached_seam_judge(clean "${tree}" accept "${accepted}")

fastcached_seam_tree(cleanGit tree)
fastcached_stage_git("${tree}")
fastcached_seam_judge(cleanGit "${tree}" accept "via git ls-files")

# Each spelling of the crossing, appended to the caller.
foreach(spelling IN ITEMS
        "angled|#include <monocypher.h>"
        "relativeIntoVendor|#include \"../../../vendor/monocypher/src/optional/monocypher-ed25519.h\""
        "upperCase|#include <Monocypher.h>"
        "implementationFile|#include \"monocypher.c\""
        "importDirective|  #  import <monocypher-ed25519.h>")
    string(FIND "${spelling}" "|" bar)
    string(SUBSTRING "${spelling}" 0 ${bar} caseName)
    math(EXPR afterBar "${bar} + 1")
    string(SUBSTRING "${spelling}" ${afterBar} -1 directive)
    fastcached_seam_tree(${caseName} tree)
    file(APPEND "${tree}/src/FastCache/Node/Caller.cpp" "${directive}\n")
    fastcached_seam_judge(${caseName} "${tree}" refuse "src/FastCache/Node/Caller.cpp:3: includes a Monocypher header")
endforeach()

fastcached_seam_tree(violationGit tree)
file(APPEND "${tree}/src/FastCache/Node/Caller.cpp" "#include <monocypher.h>\n")
fastcached_stage_git("${tree}")
fastcached_seam_judge(violationGit "${tree}" refuse "via git ls-files")

fastcached_seam_tree(commentedOut tree)
file(APPEND "${tree}/src/FastCache/Node/Caller.cpp" "// #include <monocypher.h> is what this file must never do\n")
fastcached_seam_judge(commentedOut "${tree}" accept "${accepted}")

fastcached_seam_tree(bridgeHeader tree)
file(APPEND "${tree}/src/FastCache/Node/Caller.cpp" "#include <FastCache/Core/MonocypherBytes.hpp>\n")
fastcached_seam_judge(bridgeHeader "${tree}" accept "${accepted}")

fastcached_seam_tree(vendoredDeclined tree)
fastcached_seam_judge(vendoredDeclined "${tree}" accept "declined under third-party roots")

fastcached_seam_tree(vendoredOwned tree)
file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/elsewhere\n")
file(MAKE_DIRECTORY "${tree}/vendor/elsewhere")
fastcached_seam_judge(vendoredOwned "${tree}" refuse
    "vendor/monocypher/src/optional/monocypher-ed25519.h:1: includes a Monocypher header")

fastcached_seam_tree(staleNoInclude tree)
file(WRITE "${tree}/src/FastCache/Core/X25519.cpp" "#include <FastCache/Core/X25519.hpp>\n")
fastcached_seam_judge(staleNoInclude "${tree}" refuse "STALE permitted row -- src/FastCache/Core/X25519.cpp: was read")

fastcached_seam_tree(staleMissing tree)
file(REMOVE "${tree}/src/FastCache/Core/Ed25519.cpp")
fastcached_seam_judge(staleMissing "${tree}" refuse "STALE permitted row -- src/FastCache/Core/Ed25519.cpp: does not exist")

set(tree "${FASTCACHED_SCRATCH_DIR}/empty")
file(REMOVE_RECURSE "${tree}")
file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/monocypher\n")
file(WRITE "${tree}/vendor/monocypher/src/optional/monocypher-ed25519.h" "#include \"monocypher.h\"\n")
fastcached_seam_judge(empty "${tree}" refuse "matched no first-party C++ source")

list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    string(REPLACE ";" "\n  " printable "${failures}")
    message(FATAL_ERROR "crypto-seam-selftest: ${failureCount} of ${ran} case(s) did not judge as they must:\n  ${printable}\n")
endif()
message(STATUS "crypto-seam-selftest: ${ran} case(s) ran, every verdict as it must be")
