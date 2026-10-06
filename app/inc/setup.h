/* Copyright (c) 2026 GZM Embarcados
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SETUP_H_
#define APP_SETUP_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bring up USB, the user store and the UI; a failing step is
 *  logged and boot continues. */
void setup_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETUP_H_ */
