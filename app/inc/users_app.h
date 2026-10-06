/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_USERS_APP_H_
#define APP_USERS_APP_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Mount LittleFS and load the user store (gzm user_mgr). */
int users_app_init(void);

bool users_app_ready(void);

/** @brief Registered users, the default admin included. */
size_t users_app_count(void);

size_t users_app_blocked(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_USERS_APP_H_ */
