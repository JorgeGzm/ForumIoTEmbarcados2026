# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# In-house libraries (gzm/): a Zephyr module that lives in this repository.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../gzm)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(conferencia2026_demo LANGUAGES C)

target_include_directories(app PRIVATE inc)

target_sources(app PRIVATE
  src/main.c
  src/setup.c
  src/users_app.c
  src/ui_app.c
  src/images/gzm_logo_160.c
)

target_sources_ifdef(CONFIG_MCUBOOT_IMG_MANAGER app PRIVATE
  src/boot/boot.c
  src/boot/boot_shell.c
)
