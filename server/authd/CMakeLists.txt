# SPDX-License-Identifier: Apache-2.0
#
# authd — login / realm-list / session-grant daemon (server SAD §2.1).
# M0 stub: links the shared core static lib, handles --version/--help.

add_executable(authd
  main.cpp
)

target_link_libraries(authd
  PRIVATE
    meridian::core
)
