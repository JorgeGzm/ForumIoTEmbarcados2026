# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_user_mgr_mock)

# The real user_mgr.c; everything it calls on the filesystem side
# (fs_mgr_* and fs_*) is an FFF fake in src/main.c. fs_mgr.c is not built.
set(GZM ${CMAKE_CURRENT_LIST_DIR}/../../gzm)
target_sources(app PRIVATE src/main.c ${GZM}/lib/user_mgr/src/user_mgr.c)
target_include_directories(app PRIVATE ${GZM}/lib/user_mgr/include ${GZM}/lib/fs_mgr/include)
