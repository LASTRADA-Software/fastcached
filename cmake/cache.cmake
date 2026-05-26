find_program(SCCACHE sccache)

if(SCCACHE AND USE_SCCACHE)
  message(STATUS "Enabling sccache")
  set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
  set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
  # sccache does not support /Zi (shared PDB). Force MSVC to embed debug info
  # in .obj files (/Z7) via the modern CMake knob (CMP0141), and also fix up
  # any legacy /Zi already present in FLAGS_DEBUG / FLAGS_RELWITHDEBINFO.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    foreach(_var
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_C_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS_RELWITHDEBINFO)
      string(REGEX REPLACE "([-/])Zi" "\\1Z7" ${_var} "${${_var}}")
    endforeach()
  endif()
else()
  message(STATUS "sccache not found or disabled by USE_SCCACHE option, caching disabled")
endif()
