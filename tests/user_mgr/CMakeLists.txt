# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
# The library under test: the in-house gzm/ module of this repository.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../gzm)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_user_mgr)

target_sources(app PRIVATE src/main.c src/test_limit.c src/wdt_feed.c)
