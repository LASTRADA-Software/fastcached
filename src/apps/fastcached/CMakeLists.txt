# SPDX-License-Identifier: Apache-2.0
# fastcached — the cache daemon. Links the whole FastCache library.

add_executable(fastcached main.cpp)
target_link_libraries(fastcached PRIVATE FastCache)

install(TARGETS fastcached
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
