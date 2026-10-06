# SPDX-License-Identifier: Apache-2.0
# Out-of-tree board: hand BOARD_ROOT to sysbuild (and, through it, to
# every image) before the board is resolved.
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(BOARD_ROOT "${DEMO_ROOT}" CACHE INTERNAL "")

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sysbuild LANGUAGES)
