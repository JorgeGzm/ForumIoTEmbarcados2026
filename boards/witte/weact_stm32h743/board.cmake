# SPDX-License-Identifier: Apache-2.0

set(SUPPORTED_EMU_PLATFORMS renode)
set(RENODE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/support/weact_stm32h743.resc)
set(RENODE_UART sysbus.uart4)

board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")

include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/renode.board.cmake)
