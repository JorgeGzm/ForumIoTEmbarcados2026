# Copyright (c) 2026 GZM Embarcados

zephyr_library_sources_ifdef(CONFIG_GZM_USER_MGR
    src/user_mgr.c
)

if(CONFIG_GZM_USER_MGR AND CONFIG_GZM_USER_MGR_SHELL)
    zephyr_library_sources(src/user_mgr_shell.c)
endif()

if(CONFIG_GZM_USER_MGR)
    zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
