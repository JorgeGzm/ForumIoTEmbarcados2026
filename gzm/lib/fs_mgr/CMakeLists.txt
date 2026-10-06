# Copyright (c) 2026 GZM Embarcados

zephyr_library_sources_ifdef(CONFIG_GZM_FS_MGR src/fs_mgr.c)

if(CONFIG_GZM_FS_MGR)
    zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
