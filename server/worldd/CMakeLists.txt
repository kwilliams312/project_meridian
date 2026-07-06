# SPDX-License-Identifier: Apache-2.0
#
# worldd — shard worker / map simulation daemon (server SAD §2.5).
# M0 stub: links the shared core static lib, handles --version/--help.

add_executable(worldd
  main.cpp
)

target_link_libraries(worldd
  PRIVATE
    meridian::core
)
