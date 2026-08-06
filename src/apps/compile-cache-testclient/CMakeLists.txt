# SPDX-License-Identifier: Apache-2.0
# Standalone compile-cache test client. Links the FastCache library so it
# reuses the exact PathCanon / CompileValue codec the server uses — the
# canonicalization parity contract is exercised, not reimplemented.

add_executable(compile-cache-testclient main.cpp)
target_link_libraries(compile-cache-testclient PRIVATE FastCache)
