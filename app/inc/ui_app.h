/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_UI_APP_H_
#define APP_UI_APP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * Build the splash and main screens. The firmware major version picks the
 * colors: odd = white splash bar and black main screen, even = yellow.
 */
int ui_app_init(uint8_t fw_major);

#ifdef __cplusplus
}
#endif

#endif /* APP_UI_APP_H_ */
